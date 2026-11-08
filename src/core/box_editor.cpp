// Script & boxes (see box_editor.h). Everything is read on the game thread in a training fight, into a view the UI
// draws. The fighters' boxes and scripts are the game's own loaded data (COL_<CHR> and BBS_<CHR>: the entity's box
// pointer and script pointers point into them); edits are written straight into them.
#include "box_editor.h"

#include "arcsys.h"
#include "bbscript.h"
#include "combo_log.h"
#include "pak_mount.h"

#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <tuple>

namespace bbscript { extern short* instruction_sizes; }

namespace box_editor {
namespace {
  std::filesystem::path g_dir;  // ue4ss/StriveLabs_scripts
  std::mutex g_m;               // guards g_view
  View g_view;
  std::atomic<bool> g_open{false};
  std::atomic<unsigned long long> g_ticked{0};  // when a training fight last ran it (it only counts as open then)
  std::atomic<int> g_export_state{-1}, g_export_boxes{-1};  // a side to export, from the UI

  // ---- the instruction database (BBScriptDX's GGSTRonRemake.ron and idNameWithDescription.json) ----
  enum class Arg { Number, String16, String32, Value, Enum };
  struct ArgDef { Arg kind = Arg::Number; std::string enum_name; };
  struct Instr { int size = 0; std::string name; int block = 0; std::vector<ArgDef> args; std::string desc; };  // block: 1 begins, -1 ends
  struct Db {
    bool ok = false;
    std::map<int, Instr> instr;
    std::map<int, std::string> vars;                          // named variables, for Mem(...)
    std::map<std::string, std::map<int, std::string>> enums;  // named values
  } g_db;
  std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == ',')) b--;
    return s.substr(a, b - a);
  }
  std::string quoted(const std::string& s) {  // the text inside the first "..."
    const size_t a = s.find('"'), b = a == std::string::npos ? a : s.find('"', a + 1);
    return a == std::string::npos || b == std::string::npos ? std::string() : s.substr(a + 1, b - a - 1);
  }
  // The .ron is pretty-printed one item per line, so it's read line by line by where we are in it
  bool readRon(const std::filesystem::path& file) {
    std::ifstream f(file);
    if (!f) return false;
    std::string line, map_name;
    enum { None, Vars, Maps, Map, Instrs, Args } where = None;
    int id = -1;
    Instr cur;
    auto flush = [&] { if (id >= 0) g_db.instr[id] = cur; id = -1; cur = Instr{}; };
    while (std::getline(f, line)) {
      const std::string t = trim(line);
      if (t.empty()) continue;
      if (t.rfind("named_variables:", 0) == 0) { where = Vars; continue; }
      if (t.rfind("named_value_maps:", 0) == 0) { where = Maps; continue; }
      if (t.rfind("instructions:", 0) == 0) { where = Instrs; continue; }
      switch (where) {
        case Vars:
          if (t == "}") { where = None; break; }
          if (const size_t c = t.find(':'); c != std::string::npos) g_db.vars[std::atoi(t.c_str())] = quoted(t.substr(c));
          break;
        case Maps:
          if (t == "}") { where = None; break; }
          if (t.back() == '{') { map_name = quoted(t); where = Map; }
          break;
        case Map:
          if (t == "}") { where = Maps; break; }
          if (const size_t c = t.find(':'); c != std::string::npos) g_db.enums[map_name][std::atoi(t.c_str())] = quoted(t.substr(c));
          break;
        case Instrs:
          if (t.size() > 1 && t.back() == '(' && t.find(':') != std::string::npos && std::isdigit(static_cast<unsigned char>(t[0]))) { flush(); id = std::atoi(t.c_str()); }
          else if (t.rfind("size:", 0) == 0) cur.size = std::atoi(t.c_str() + 5);
          else if (t.rfind("name:", 0) == 0) cur.name = quoted(t);
          else if (t.rfind("codeBlock:", 0) == 0) cur.block = t.find("End") != std::string::npos ? -1 : 1;
          else if (t.rfind("args:", 0) == 0) { if (t.find(']') == std::string::npos) where = Args; }
          break;
        case Args:
          if (t[0] == ']') { where = Instrs; break; }
          {
            ArgDef a;
            if (t.rfind("Number", 0) == 0) a.kind = Arg::Number;
            else if (t.rfind("String16", 0) == 0) a.kind = Arg::String16;
            else if (t.rfind("String32", 0) == 0) a.kind = Arg::String32;
            else if (t.rfind("AccessedValue", 0) == 0) a.kind = Arg::Value;
            else if (t.rfind("Enum", 0) == 0) { a.kind = Arg::Enum; a.enum_name = quoted(t); }
            cur.args.push_back(a);
            if (t.find(']') != std::string::npos) where = Instrs;  // (the list closed on its last argument's line)
          }
          break;
        default: break;
      }
    }
    flush();
    return !g_db.instr.empty();
  }
  std::string unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
      if (s[i] != '\\' || i + 1 >= s.size()) { out += s[i]; continue; }
      const char c = s[++i];
      if (c == 'n') out += '\n';
      else if (c == 't') out += ' ';
      else if (c == 'u' && i + 4 < s.size()) { const int u = std::strtol(s.substr(i + 1, 4).c_str(), nullptr, 16); i += 4; out += u < 128 ? static_cast<char>(u) : '?'; }
      else out += c;
    }
    return out;
  }
  void readDescs(const std::filesystem::path& file) {  // {"712": {"name": ..., "desc": "..."}}, one key per line
    std::ifstream f(file);
    std::string line;
    int id = -1;
    while (std::getline(f, line)) {
      const std::string t = trim(line);
      if (t.size() > 3 && t[0] == '"' && t.back() == '{' && std::isdigit(static_cast<unsigned char>(t[1]))) id = std::atoi(t.c_str() + 1);
      else if (id >= 0 && t.rfind("\"desc\":", 0) == 0) {
        const size_t a = t.find('"', 7), b = t.rfind('"');
        if (a != std::string::npos && b > a) { auto it = g_db.instr.find(id); if (it != g_db.instr.end()) it->second.desc = unescape(t.substr(a + 1, b - a - 1)); }
      }
    }
  }

  // ---- reading the game ----
  bool readable(const void* p, size_t n) {  // (across as many memory regions as the range covers)
    if (!p) return false;
    const uint8_t* at = static_cast<const uint8_t*>(p);
    const uint8_t* const end = at + n;
    do {
      MEMORY_BASIC_INFORMATION mbi{};
      if (!VirtualQuery(at, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
      if ((mbi.Protect & 0xFF) == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD)) return false;
      at = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
    } while (at < end);
    return true;
  }
  int32_t i32(const uint8_t* p) { int32_t v; std::memcpy(&v, p, 4); return v; }
  std::string cstr(const uint8_t* p, size_t n) { size_t k = 0; while (k < n && p[k]) k++; return std::string(reinterpret_cast<const char*>(p), k); }
  bool isBegin(int op, const Instr* d) {
    if (d) return d->block == 1;
    for (const auto& pr : bbscript::BeginEndPairs) if (op == pr[0]) return true;
    return false;
  }
  bool isEnd(int op, const Instr* d) {
    if (d) return d->block == -1;
    for (const auto& pr : bbscript::BeginEndPairs) if (op == pr[1]) return true;
    return false;
  }
  std::string esc(const std::string& s) { std::string o; for (char c : s) { if (c == '\'') o += '\\'; o += c; } return o; }  // (as BBScriptDX escapes them)
  

  // Names and tooltips for the numbers (StriveLabs_scripts/db/labels.json): "<instruction or routine>": [names...]
  // and "<...> tips": [tooltips...], one per argument in order
  std::map<std::string, std::vector<std::string>> g_labels;
  void readLabels(const std::filesystem::path& file) {
    g_labels.clear();
    std::ifstream f(file, std::ios::binary);
    if (!f) return;
    const std::string t((std::istreambuf_iterator<char>(f)), {});
    size_t i = 0;
    auto str = [&](std::string& out) {  // a JSON string at i (after its quote)
      out.clear();
      for (; i < t.size() && t[i] != '"'; i++) {
        if (t[i] == '\\' && i + 1 < t.size()) { i++; out += t[i] == 'n' ? '\n' : t[i]; }
        else out += t[i];
      }
      i++;
    };
    while (i < t.size()) {
      if (t[i] != '"') { i++; continue; }
      i++;
      std::string key;
      str(key);
      while (i < t.size() && (t[i] == ' ' || t[i] == ':' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) i++;
      if (i < t.size() && t[i] == '"') { std::string skip; i++; str(skip); continue; }  // ("_about")
      if (i >= t.size() || t[i] != '[') continue;
      std::vector<std::string> list;
      for (i++; i < t.size() && t[i] != ']'; i++)
        if (t[i] == '"') { i++; std::string s; str(s); list.push_back(s); i--; }
      g_labels[key] = std::move(list);
    }
  }
  std::string labelOf(const std::string& key, size_t k, bool tip) {
    auto it = g_labels.find(tip ? key + " tips" : key);
    return it != g_labels.end() && k < it->second.size() ? it->second[k] : std::string();
  }

  std::string argsOf(const uint8_t* p, int size, const Instr* d) {
    std::string out;
    int at = 4;
    auto add = [&](const std::string& s) { if (!out.empty()) out += ", "; out += s; };
    if (d)
      for (const ArgDef& a : d->args) {
        const int need = a.kind == Arg::String32 ? 32 : a.kind == Arg::String16 ? 16 : a.kind == Arg::Value ? 8 : 4;
        if (at + need > size) break;
        switch (a.kind) {
          case Arg::Number: add(std::to_string(i32(p + at))); break;
          case Arg::String16: add("s16'" + esc(cstr(p + at, 16)) + "'"); break;
          case Arg::String32: add("s32'" + esc(cstr(p + at, 32)) + "'"); break;
          case Arg::Value: {
            const int tag = i32(p + at), v = i32(p + at + 4);
            if (tag == 0) add("Val(" + std::to_string(v) + ")");
            else if (tag == 2) { auto it = g_db.vars.find(v); add("Mem(" + (it != g_db.vars.end() ? it->second : std::to_string(v)) + ")"); }
            else add("BadTag(" + std::to_string(tag) + ", " + std::to_string(v) + ")");
            break;
          }
          case Arg::Enum: {
            const int v = i32(p + at);
            auto m = g_db.enums.find(a.enum_name);
            auto it = m == g_db.enums.end() ? decltype(m->second.end()){} : m->second.find(v);
            bool once = m != g_db.enums.end() && it != m->second.end();
            if (once) { int n = 0; for (const auto& kv : m->second) if (kv.second == it->second) n++; once = n == 1; }  // (a name given to two values: the number)
            add(once ? "(" + it->second + ")" : std::to_string(v));
            break;
          }
        }
        at += need;
      }
    if (at < size) {  // What the database doesn't name (or all of it without one), as BBScriptDX's unknown bytes
      std::string h = "0x";
      char b[3];
      for (int k = at; k < size; k++) { snprintf(b, sizeof(b), "%02X", p[k]); h += b; }
      add(h);
    }
    return out;
  }
  std::string chrOf(const std::string& sprite) {  // "sol100_02" -> "SOL"
    std::string c;
    for (char ch : sprite) { if (!std::isalpha(static_cast<unsigned char>(ch)) || c.size() >= 3) break; c += static_cast<char>(std::toupper(static_cast<unsigned char>(ch))); }
    return c;
  }
  struct Cache { const char* first = nullptr; const void* entity = nullptr; std::string chr; std::vector<InstrView> script; std::string state, note; };
  Cache g_cache[2];
  void readScriptAt(const uint8_t* p, Cache& c);
  void readScript(asw_entity* e, Cache& c) {
    c.first = e->first_script_cmd;
    readScriptAt(reinterpret_cast<const uint8_t*>(c.first), c);
  }
  // Every move in the fighter's script file: its table (a count, then a 32-byte name and an offset each), the offsets
  // counted from the code after it
  struct States {
    const uint8_t* buf = nullptr;
    std::vector<std::string> names; std::vector<const uint8_t*> starts;
    std::vector<std::string> routines; std::vector<const uint8_t*> rstarts;  // beginSubroutine blocks, by walking the code
  };
  States g_states[2];
  void readStates(asw_entity* e, States& st) {
    st = States{};
    if (!e->bbs_file || !readable(e->bbs_file, 8)) return;
    const uint8_t* buf = *reinterpret_cast<const uint8_t* const*>(e->bbs_file);
    if (!readable(buf, 4)) return;
    const uint32_t count = *reinterpret_cast<const uint32_t*>(buf);
    if (count == 0 || count > 8000 || !readable(buf, 4 + static_cast<size_t>(count) * 36)) return;
    const uint8_t* code = buf + 4 + static_cast<size_t>(count) * 36;
    st.buf = buf;
    for (uint32_t k = 0; k < count; k++) {
      st.names.push_back(cstr(buf + 4 + k * 36, 32));
      st.starts.push_back(code + *reinterpret_cast<const uint32_t*>(buf + 4 + k * 36 + 32));
    }
    // the routines: the code walked line by line from its start (the game's own instruction sizes), every
    // beginSubroutine (8, or 15 with arguments) kept; the walk stops at the first line it can't size
    if (!bbscript::instruction_sizes) return;
    const uint8_t* p = code;
    const uint8_t* region_end = nullptr;  // (memory checked once per region, not per line)
    bool inside = false;                  // in a state or routine (at the top level only those begin)
    for (int n = 0; n < 2000000; n++) {
      if (!region_end || p + 4 > region_end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & 0xFF) == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD)) break;
        region_end = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
      }
      const int op = i32(p);
      if (op < 0 || op >= static_cast<int>(bbscript::opcode::MAX)) break;
      if (!inside && op != 0 && op != 8 && op != 15) break;  // (past the code: whatever follows isn't a state or routine)
      const int size = bbscript::instruction_sizes[op];
      if (size < 4 || size % 4 || p + size > region_end) break;
      if ((op == 8 || op == 15) && size >= 36) { st.routines.push_back(cstr(p + 4, 32)); st.rstarts.push_back(p); }
      if (op == 0 || op == 8 || op == 15) inside = true;
      if (op == 1 || op == 16) inside = false;
      p += size;
    }
  }
  struct Sel { std::string name; const uint8_t* start = nullptr; Cache c; };
  Sel g_sel_cache[2];
  std::mutex g_sel_m;
  std::string g_sel_want[2];  // the move the UI opened, per side (under g_sel_m)
  void readScriptAt(const uint8_t* p, Cache& c) {
    c.script.clear();
    c.state.clear();
    c.note.clear();
    if (!bbscript::scriptReaderAvailable() || !bbscript::instruction_sizes) { c.note = "The game's script reader wasn't found (a newer game version?)."; return; }
    if (!readable(p, 36)) { c.note = "No script."; return; }
    if (i32(p) == 0 || ((i32(p) == 8 || i32(p) == 15) && bbscript::instruction_sizes[i32(p)] >= 36)) c.state = cstr(p + 4, 32);  // beginState's (or a routine's) name
    int depth = 0;
    for (int step = 0, at = 0; step < 4000; step++) {
      const uint8_t* q = p + at;
      if (!readable(q, 4)) { c.note = "The script couldn't be read past byte " + std::to_string(at) + "."; break; }
      const int op = i32(q);
      if (op < 0 || op >= static_cast<int>(bbscript::opcode::MAX)) { c.note = "Unknown instruction " + std::to_string(op) + " at byte " + std::to_string(at) + "."; break; }
      const int size = bbscript::instruction_sizes[op];
      if (size < 4 || size % 4 || !readable(q, static_cast<size_t>(size))) { c.note = "Bad instruction size at byte " + std::to_string(at) + "."; break; }
      auto dit = g_db.instr.find(op);
      const Instr* d = dit == g_db.instr.end() ? nullptr : &dit->second;
      if (isEnd(op, d) && depth > 0) depth--;
      InstrView iv;
      iv.offset = at;
      iv.op = op;
      iv.depth = op == 1 ? 0 : depth;
      iv.name = d && !d->name.empty() ? d->name : "Unknown" + std::to_string(op);
      iv.args = argsOf(q, size, d);
      if (d) iv.desc = d->desc;
      if (op == 2 && size >= 40) { iv.sprite = cstr(q + 4, 32); iv.sprite_time = i32(q + 36); }  // sprite: name, frames
      if ((op == 17 || op == 75 || op == 11 || op == 12 || op == 13 || op == 14 || op == 24 || op == 25 || op == 31) && size >= 36)
        iv.call = cstr(q + 4, 32);  // callSubroutine(WithArgs): the routine; labels and gotos: the label
      if (d) {  // its numbers: where each is, and what it's called
        const std::string key = !iv.call.empty() && g_labels.count(iv.call) ? iv.call : iv.name;
        int fat = 4;
        for (size_t k = 0; k < d->args.size(); k++) {
          const ArgDef& a = d->args[k];
          const int need = a.kind == Arg::String32 ? 32 : a.kind == Arg::String16 ? 16 : a.kind == Arg::Value ? 8 : 4;
          if (fat + need > size) break;
          InstrView::ArgView av;
          if (a.kind == Arg::Number) { av.kind = 0; av.field = fat; av.value = i32(q + fat); av.editable = true; }
          else if (a.kind == Arg::Value) { av.kind = 1; av.field = fat + 4; av.value = i32(q + fat + 4); av.editable = i32(q + fat) == 0; }
          else if (a.kind == Arg::Enum) {
            av.kind = 2; av.field = fat; av.value = i32(q + fat); av.editable = true;
            auto m = g_db.enums.find(a.enum_name);
            if (m != g_db.enums.end()) if (auto it = m->second.find(av.value); it != m->second.end()) av.value_name = it->second;
          }
          if (a.kind == Arg::Number || a.kind == Arg::Value || a.kind == Arg::Enum) {
            av.label = labelOf(key, k, false);
            av.tip = labelOf(key, k, true);
            if (av.label.empty()) av.label = "value " + std::to_string(k + 1);
            if (iv.op == 2 && fat == 36) av.label = "frames";
            iv.argv.push_back(std::move(av));
          }
          fat += need;
        }
      }
      c.script.push_back(std::move(iv));
      if (isBegin(op, d)) depth++;
      if (op == 1 || op == 16) break;  // endState, endSubroutine
      at += size;
    }
  }
  void writeFile(const std::filesystem::path& f, const std::string& text, std::string& status) {
    std::error_code ec;
    std::filesystem::create_directories(f.parent_path(), ec);
    std::ofstream o(f, std::ios::binary | std::ios::trunc);
    o << text;
    o.flush();
    std::filesystem::path rel = f.lexically_relative(g_dir.parent_path());
    status = o.good() ? "Saved " + rel.string() : "Couldn't write " + rel.string();
    COMBO_LOG("script & boxes: " + status);
  }
  std::string num(float v) {  // whole numbers as such (BBScriptDX's COL JSON has them as integers)
    char b[32];
    if (v == static_cast<float>(static_cast<int>(v))) snprintf(b, sizeof(b), "%d", static_cast<int>(v));
    else snprintf(b, sizeof(b), "%g", v);
    return b;
  }
  std::string jsonStr(const std::string& s) {
    std::string o = "\"";
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
    return o + "\"";
  }

  // ---- box edits: the game thread writes them; the UI only asks ----
  struct Key {
    std::string chr, sprite;
    int index = 0;  // into the sprite's boxes
    bool operator<(const Key& o) const { return std::tie(chr, sprite, index) < std::tie(o.chr, o.sprite, o.index); }
  };
  struct Edit {
    BoxView orig, now;
    std::string label;        // "Hurt 2"
    std::vector<hitbox*> at;  // this fight's copies of it (one per loaded file: both sides', in a mirror)
  };
  // (kept when put back to the game's values too, so an undo still finds it). Sprites that share the same box records
  // share one Edit
  std::map<Key, std::shared_ptr<Edit>> g_edits;
  struct UndoStep { Key k; BoxView was; int group = 0; };  // (one Ctrl+Z takes back a whole group: a copy to many sprites)
  std::vector<UndoStep> g_undo;
  int g_group = 0;
  void remember(const Key& k, const BoxView& was, int group) {
    g_undo.push_back({k, was, group});
    if (g_undo.size() > 400) g_undo.erase(g_undo.begin(), g_undo.begin() + 100);
  }
  struct Req {
    enum T { Set, Reset, ResetSprite, ResetAll, Undo, Copy } t = Set;
    int side = -1, index = -1, fields = 0;
    std::string chr, sprite;
    BoxView b;
    bool undo_point = false, join = false;
    std::vector<std::string> to;  // Copy: the sprites
  };
  std::mutex g_req_m;  // guards g_reqs
  std::vector<Req> g_reqs;
  std::atomic<bool> g_keep{true};
  std::atomic<int> g_hl_side{-1}, g_hl_index{-1};
  std::atomic<const void*> g_ent[2] = {};   // the entity each side shows (the fighter, or one of its objects)
  std::atomic<const void*> g_show[2] = {};  // the object asked for, nullptr the fighter
  void ask(Req r) { std::lock_guard<std::mutex> lk(g_req_m); g_reqs.push_back(std::move(r)); }
  bool writable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!readable(p, n)) return false;
    for (const void* at : {p, static_cast<const void*>(static_cast<const uint8_t*>(p) + (n ? n - 1 : 0))}) {  // (its start and its end)
      if (!VirtualQuery(at, &mbi, sizeof(mbi))) return false;
      const DWORD pr = mbi.Protect & 0xFF;
      if (pr != PAGE_READWRITE && pr != PAGE_WRITECOPY && pr != PAGE_EXECUTE_READWRITE && pr != PAGE_EXECUTE_WRITECOPY) return false;
    }
    return true;
  }
  bool same(const hitbox& h, const BoxView& b) { return h.x == b.x && h.y == b.y && h.w == b.w && h.h == b.h; }
  bool sameBox(const BoxView& a, const BoxView& b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }
  void put(hitbox* p, const BoxView& b) { p->x = b.x; p->y = b.y; p->w = b.w; p->h = b.h; }
  // An edit moves to b, written wherever this fight's files still hold its last value (a copy that holds anything
  // else isn't it any more)
  void apply(Edit& ed, const BoxView& b) {
    for (hitbox* p : ed.at) if (writable(p, sizeof(hitbox)) && static_cast<int>(p->type) == ed.orig.kind && same(*p, ed.now)) put(p, b);
    ed.now.x = b.x; ed.now.y = b.y; ed.now.w = b.w; ed.now.h = b.h;
  }
  const char* shortName(int kind) { return kind == 0 ? "Hurt" : kind == 1 ? "Hit" : kind == 5 ? "Push" : kind == 18 ? "Combo hit" : kindName(kind); }
  int boxCount(const asw_entity* e) {  // the sprite's boxes, -1 unless every one of them can be read
    int total = 0;
    for (int k = 0; k < 19; k++) { const int n = e->box_counts[k]; if (n < 0 || n > 256) return -1; total += n; }
    return e->hitboxes && total <= 512 && readable(e->hitboxes, static_cast<size_t>(total) * sizeof(hitbox)) ? total : -1;
  }
  // ---- the fighter's whole COL file, for sprites not on screen: an FPAC (a table of 32-byte names, then the
  // entries), each entry a JONB (names, version, editor blocks, 45 box counts by kind, then the boxes in kind order;
  // checked against every entry of SOL's file) ----
  struct ColSprite { hitbox* boxes = nullptr; int counts[19] = {}; };
  struct Col { const uint8_t* base = nullptr; size_t size = 0; std::map<std::string, ColSprite> sprites; };
  std::vector<Col> g_cols;  // (dropped with a new fight: the files are loaded again)
  uint32_t u32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
  uint16_t u16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
  std::string g_col_why;  // why the last file read failed, for the log
  // One file, strictly: every entry's JONB must be exactly the size its table row says (true of every entry of
  // SOL's file). An entry that doesn't parse is left out, never guessed at.
  bool readCol(const uint8_t* base, Col& c) {
    if (!readable(base, 0x20)) { g_col_why = "header not readable"; return false; }
    const uint32_t ds = u32(base + 4), total = u32(base + 8), count = u32(base + 12), name_len = u32(base + 20);
    if ((name_len != 32 && name_len != 64) || count == 0 || count > 20000 || ds != 0x20 + count * (name_len + 16) || total < ds || total > (64u << 20)) {
      char b[160];
      snprintf(b, sizeof(b), "header says data at 0x%X, %u bytes, %u entries, names of %u", ds, total, count, name_len);
      g_col_why = b;
      return false;
    }
    if (!readable(base, total)) { g_col_why = "its " + std::to_string(total) + " bytes aren't all readable"; return false; }
    c.base = base;
    c.size = total;
    int bad = 0;
    for (uint32_t i = 0; i < count; i++) {
      const uint8_t* ent = base + 0x20 + static_cast<size_t>(i) * (name_len + 16);  // (a row: the name, then 16 bytes)
      const uint32_t off = u32(ent + name_len + 4), size = u32(ent + name_len + 8);
      if (static_cast<uint64_t>(ds) + off + size > total || size < 6 + 7 + 90) { bad++; continue; }
      const uint8_t* j = base + ds + off;
      if (std::memcmp(j, "JONB", 4) != 0) { bad++; continue; }
      size_t p = 6 + static_cast<size_t>(u16(j + 4)) * 32;  // (the names)
      if (p + 7 + 90 > size) { bad++; continue; }
      const uint32_t blocks = u32(j + p + 3);                 // (after the version and a byte)
      const uint8_t* layers = j + p + 7;
      p += 7 + 90 + static_cast<size_t>(blocks) * 0x50;
      ColSprite s;
      size_t n = 0;
      for (int k = 0; k < 45; k++) { const int m = u16(layers + k * 2); if (k < 19) s.counts[k] = m; n += static_cast<size_t>(m); }
      if (p + n * 20 != size) { bad++; continue; }
      s.boxes = reinterpret_cast<hitbox*>(const_cast<uint8_t*>(j + p));
      c.sprites[cstr(ent, static_cast<int>(name_len))] = s;
    }
    if (c.sprites.empty()) { g_col_why = "none of its " + std::to_string(count) + " entries could be read"; return false; }
    if (bad) COMBO_LOG("script & boxes: " + std::to_string(bad) + " of " + std::to_string(count) + " box file entries couldn't be read (left out)");
    return true;
  }
  std::string narrowName(const RC::StringType& w) { std::string o; for (auto ch : w) o += ch < 0x80 ? static_cast<char>(ch) : '?'; return o; }
  // The loaded COL files, as the game keeps them: each REDCollisionData object holds its file as a byte array at +0x78
  // (data pointer, then the count). Checked on COL_SOL: the array is the file's size, and Sol's box pointer sits at the
  // same offset as in the file. One walk of every object, only on a copy.
  bool holdsBoxes(const Col& c, const uint8_t* q) {  // one of its sprites is exactly the boxes on screen
    for (const auto& kv : c.sprites) if (reinterpret_cast<const uint8_t*>(kv.second.boxes) == q) return true;
    return false;
  }
  std::string hexOf(const uint8_t* p, int n) {
    std::string s;
    char h[8];
    if (!p || !readable(p, static_cast<size_t>(n))) return " (not readable)";
    for (int k = 0; k < n; k++) { snprintf(h, sizeof(h), "%s%02X", k % 16 ? "" : " ", p[k]); s += h; }
    return s;
  }
  bool colFromObjects(const uint8_t* q, Col& out, std::string& why) {
    std::vector<RC::Unreal::UObject*> objs;
    RC::Unreal::UObjectGlobals::FindAllOf(STR("REDCollisionData"), objs);
    std::string list;
    const uint8_t* holder = nullptr;
    for (RC::Unreal::UObject* o : objs) {
      const uint8_t* b = reinterpret_cast<const uint8_t*>(o);
      if (!o || !readable(b + 0x78, 12)) continue;
      const uint8_t* data;
      int32_t num;
      std::memcpy(&data, b + 0x78, 8);
      std::memcpy(&num, b + 0x80, 4);
      char at[64];
      snprintf(at, sizeof(at), "@%p(%d)", static_cast<const void*>(data), num);
      if (list.size() < 600) list += (list.empty() ? "" : ", ") + narrowName(o->GetName()) + at;
      if (!data || num <= 0x20 || q < data || q >= data + num) continue;
      holder = data;
      Col c;
      g_col_why.clear();
      const bool ok = readCol(data, c);
      if (ok && c.size <= static_cast<size_t>(num) && holdsBoxes(c, q)) { out = std::move(c); return true; }
      why = narrowName(o->GetName()) + " holds the boxes, but " +
            (!ok ? g_col_why
                 : c.size > static_cast<size_t>(num) ? "its header says " + std::to_string(c.size) + " bytes and it holds " + std::to_string(num)
                                                     : std::string("no sprite in it has these boxes"));
    }
    if (!holder) why = std::to_string(objs.size()) + " box file objects, none holding the boxes";
    why += " [" + list + "]";
    if (holder) why += "; its first bytes:" + hexOf(holder, 0x20 + 48 * 2);
    return false;
  }
  std::vector<const void*> g_col_failed;  // box pointers whose file wasn't found: logged once, not tried again this fight
  std::string g_col_tried[2];  // the move (and fighter) the strip last looked the box file up for, per side
  // The COL file a fighter's boxes are in. From the game's own objects first; else back from the box pointer to the
  // file's start, from its allocation's base (4 MB at most). Only on a copy, never per frame.
  const Col* colOf(const asw_entity* e) {
    const uint8_t* q = reinterpret_cast<const uint8_t*>(e->hitboxes);
    if (!q || !readable(q, 4)) return nullptr;
    for (const Col& c : g_cols) if (q >= c.base && q < c.base + c.size && readable(c.base, c.size) && std::memcmp(c.base, "FPAC", 4) == 0) return &c;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(q, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return nullptr;
    if (std::find(g_col_failed.begin(), g_col_failed.end(), q) != g_col_failed.end()) return nullptr;  // (failed already this fight: no second walk)
    std::string obj_why;
    {
      Col c;
      if (colFromObjects(q, c, obj_why)) { g_cols.push_back(std::move(c)); return &g_cols.back(); }
    }
    const uint8_t* lo = static_cast<const uint8_t*>(mbi.AllocationBase);
    if (q - lo > (4 << 20)) lo = q - (4 << 20);
    int headers = 0;
    std::string scan_why = "no file header (FPAC) from the allocation's base up to the boxes";
    const uint8_t* first = nullptr;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(q) & ~uintptr_t(3));
    while (p >= lo) {  // region by region (4 aligned bytes never cross a page, so one check covers the region)
      MEMORY_BASIC_INFORMATION r{};
      if (!VirtualQuery(p, &r, sizeof(r))) break;
      const uint8_t* rbase = static_cast<const uint8_t*>(r.BaseAddress);
      const bool can = r.State == MEM_COMMIT && (r.Protect & 0xFF) != PAGE_NOACCESS && !(r.Protect & PAGE_GUARD);
      const uint8_t* stop = rbase > lo ? rbase : lo;
      if (can)
        for (; p >= stop; p -= 4) {
          if (std::memcmp(p, "FPAC", 4) != 0) continue;
          headers++;
          Col c;
          g_col_why.clear();
          const bool ok = readCol(p, c);
          if (ok && q < c.base + c.size && holdsBoxes(c, q)) { g_cols.push_back(std::move(c)); return &g_cols.back(); }
          if (!first) {  // (the reason is the nearest header's, the one dumped below)
            first = p;
            scan_why = !ok ? "the nearest header: " + g_col_why : q >= c.base + c.size ? "the nearest header's file ends before the boxes" : "no sprite in the nearest header's file has these boxes";
          }
        }
      if (rbase <= lo) break;
      p = reinterpret_cast<const uint8_t*>((reinterpret_cast<uintptr_t>(rbase) - 4) & ~uintptr_t(3));
    }
    {  // Why, once, with what memory looks like there (and no second try this fight)
      if (g_col_failed.size() < 256) g_col_failed.push_back(q);
      char b[200];
      snprintf(b, sizeof(b), "script & boxes: box file not found for boxes at %p (region %p + 0x%zX, allocation %p, protect 0x%lX). ", static_cast<const void*>(q), mbi.BaseAddress,
               static_cast<size_t>(mbi.RegionSize), mbi.AllocationBase, mbi.Protect);
      std::string line = std::string(b) + "Objects: " + obj_why + ". Scan: " + std::to_string(headers) + " headers, " + scan_why;
      if (first) line += "; nearest header and its first two entries:" + hexOf(first, 0x20 + 48 * 2);
      COMBO_LOG(line);
    }
    return nullptr;
  }
  // A move's frames, from its script (see box_editor.h FramesView). col gives each sprite's boxes, if found. The rules
  // match Dustloop's frame data for all of Sol's normals (startup,
  // active, recovery): the instructions after a sprite line run on its first frame; hit(234) turns the attack on from
  // that sprite, recoveryState(235), attackOff(1915), attackEndDelay(2173) or a *AttackEnd* call off; a *Nandemo* call
  // is where the fighter can act again; gotoLabel is followed to its label, conditional gotos aren't taken.
  FramesView buildFrames(const std::vector<InstrView>& sc, const Col* col) {
    FramesView f;
    std::map<std::string, size_t> labels;  // top-level beginLabel lines, by name
    for (size_t i = 0; i < sc.size(); i++) if (sc[i].depth == 1 && sc[i].op == 11) labels.emplace(sc[i].call, i);
    std::vector<std::string> passed;       // labels already passed (a jump back to one is a loop)
    bool on = false;
    int act_from = -1;  // the cell the fighter can act again from
    size_t i = 0;
    for (int guard = 0; i < sc.size() && guard < 20000; guard++) {
      const InstrView& in = sc[i];
      if (in.depth != 1) { i++; continue; }  // (blocks: upon, if, runOnObject... run on events, not on the strip)
      if (in.op == 1) break;                 // endState
      if (in.op == 12) {                     // gotoLabel: followed
        if (std::find(passed.begin(), passed.end(), in.call) != passed.end()) { f.linear = false; if (f.note.empty()) f.note = "loops back to " + in.call; break; }
        auto t = labels.find(in.call);
        if (t == labels.end()) { f.linear = false; if (f.note.empty()) f.note = "goes to " + in.call + ", not found"; break; }
        passed.push_back(in.call);
        i = t->second + 1;
        continue;
      }
      switch (in.op) {
        case 11: passed.push_back(in.call); break;  // beginLabel: passed through
        case 13: case 14: case 24: case 25: case 31:  // a conditional goto: not taken
          f.linear = false;
          if (f.note.empty()) f.note = "branches at " + in.name + " " + in.call + " (shown not taken)";
          break;
        case 26:
          f.linear = false;
          if (f.note.empty()) f.note = "setSpriteTime changes a sprite's frames as it plays";
          break;
        case 2: {
          CellView c;
          c.sprite = in.sprite;
          c.line = static_cast<int>(i);
          c.first = f.total;
          c.dur = in.sprite_time;
          c.phase = on ? 1 : 0;  // (settled below)
          if (col) {
            auto it = col->sprites.find(c.sprite);
            if (it != col->sprites.end()) { c.hurts = it->second.counts[0]; c.hits = it->second.counts[1] + it->second.counts[18]; }
          }
          const bool held = c.dur <= 0 || c.dur >= 1000;  // held until something happens
          if (held) { c.held = true; c.dur = 1; }
          f.total += c.dur;
          f.cells.push_back(std::move(c));
          if (held) { f.linear = false; if (f.note.empty()) f.note = "the sprite " + in.sprite + " is held until an event"; i = sc.size(); continue; }
          break;
        }
        case 234:  // hit: on from the sprite this follows
          on = true;
          if (!f.cells.empty()) f.cells.back().phase = 1;
          break;
        case 235: case 1915: case 2173:  // recoveryState, attackOff, attackEndDelay: off from this sprite
          on = false;
          if (!f.cells.empty()) f.cells.back().phase = 0;
          break;
        default:
          if (in.op == 17 || in.op == 75) {
            if (in.call.find("AttackEnd") != std::string::npos) { on = false; if (!f.cells.empty()) f.cells.back().phase = 0; }
            if (in.call.find("Nandemo") != std::string::npos && act_from < 0) act_from = f.cells.empty() ? 0 : static_cast<int>(f.cells.size()) - 1;
          }
      }
      i++;
    }
    int last_hit = -1;  // the last sprite in an attack window with hit boxes (no-hitbox sprites after it are recovery)
    for (size_t k = 0; k < f.cells.size(); k++) if (f.cells[k].phase == 1 && f.cells[k].hits != 0) last_hit = static_cast<int>(k);
    bool seen = false;
    for (size_t k = 0; k < f.cells.size(); k++) {
      CellView& c = f.cells[k];
      if (c.phase == 1 && (c.hits != 0 || static_cast<int>(k) < last_hit)) { seen = true; c.phase = c.hits == 0 ? 2 : 1; }
      else c.phase = seen ? 3 : 0;
      if (act_from >= 0 && static_cast<int>(k) >= act_from && c.phase != 1 && c.phase != 2) c.phase = 4;
    }
    // its frame data: startup, active, recovery, the attack levels, on block / on hit. On block matches Dustloop on
    // Sol's ground normals: blockstun / hitstun from the level (Dustloop's table),
    // minus active - 1 + recovery; on hit isn't the table's when the move has an attack angle (it launches)
    {
      static const int kBlock[5] = {9, 11, 13, 16, 18}, kHit[5] = {12, 14, 16, 19, 21};
      int first_active = -1, last_active_end = -1, can_act = -1;
      for (size_t k = 0; k < f.cells.size(); k++) {
        const CellView& c = f.cells[k];
        if (c.phase == 1) { if (first_active < 0) first_active = c.first; last_active_end = c.first + c.dur; f.last_active = static_cast<int>(k); }
        if (c.phase == 4 && can_act < 0) can_act = c.first;
      }
      if (first_active >= 0) {
        f.startup = first_active + 1;
        f.active = last_active_end - first_active;
        f.recovery = (can_act >= 0 ? can_act : f.total) - last_active_end;
        for (size_t k = 0; k < f.cells.size(); k++) {
          const CellView& c = f.cells[k];
          if (c.first + c.dur <= first_active && c.phase == 0) f.last_startup = static_cast<int>(k);
          if (c.first >= last_active_end && c.phase == 3 && (can_act < 0 || c.first < can_act)) f.last_recovery = static_cast<int>(k);
        }
      }
      for (size_t k = 0; k < sc.size(); k++) {
        const InstrView& in = sc[k];
        if ((in.op == 17 || in.op == 75) && in.call.size() == 10 && in.call.rfind("cmn_AtkLv", 0) == 0 && in.call[9] >= '0' && in.call[9] <= '4') {
          std::string event;  // (the nearest upon block around it: IMMEDIATE is the move's own setup)
          for (int d = in.depth - 1, j = static_cast<int>(k) - 1; d >= 1 && j >= 0; j--) {
            if (sc[static_cast<size_t>(j)].depth != d) continue;
            if (sc[static_cast<size_t>(j)].op == 21) {
              std::string a = sc[static_cast<size_t>(j)].args;
              if (a.size() > 2 && a.front() == '(' && a.back() == ')') a = a.substr(1, a.size() - 2);
              event = a == "IMMEDIATE" ? std::string() : a;
              break;
            }
            d--;
          }
          f.levels.push_back({static_cast<int>(k), in.call[9] - '0', event});
        }
        if (in.name == "atkAngle") f.launches = true;
      }
      int hits = 0;  // (hit lines on the strip: a move of several hits isn't one number; the frame bar has it)
      for (const InstrView& in : sc) if (in.op == 234) hits++;  // (in blocks too: a hit that only happens sometimes still makes it several)
      f.multi = hits > 1;  // (a level for an event, like a clean hit, isn't another hit)
      if (f.recovery < 0) f.recovery = -1;
      if (f.active > 0 && f.recovery >= 0 && !f.levels.empty() && !f.multi && f.note.empty()) {  // (a strip that stops at a loop or jump, like falling until landing: its recovery isn't known)
        f.has_adv = true;
        const int lv = f.levels.front().level;
        f.on_block = kBlock[lv] - (f.active - 1 + f.recovery);
        f.on_hit = kHit[lv] - (f.active - 1 + f.recovery);
      }
    }
    return f;
  }
  std::string framesText(const FramesView& f, const std::string& state) {
    static const char* const ph[] = {"startup", "active", "attack on, no hitbox", "recovery", "can act"};
    std::string t = state + ": " + std::to_string(f.total) + " frames" + (f.linear ? "" : " (then " + f.note + ")") + "\n";
    for (const CellView& c : f.cells) {
      char b[200];
      snprintf(b, sizeof(b), "f%d-%d  %-14s %2d frames  %-20s hurt %d hit %d%s\n", c.first + 1, c.first + c.dur, c.sprite.c_str(), c.dur, ph[c.phase], c.hurts, c.hits, c.held ? "  (held)" : "");
      t += b;
    }
    return t;
  }
  // The edit for a box (made the first time), with this copy of it known
  Edit& editFor(const Key& k, hitbox* p, int kind, int nth) {
    auto it = g_edits.find(k);
    if (it == g_edits.end()) {
      std::shared_ptr<Edit> shared;
      for (const auto& kv : g_edits)  // another sprite with the same box records: it's that edit
        if (std::find(kv.second->at.begin(), kv.second->at.end(), p) != kv.second->at.end()) { shared = kv.second; break; }
      if (!shared) {
        shared = std::make_shared<Edit>();
        shared->orig = {kind, p->x, p->y, p->w, p->h};
        shared->now = shared->orig;
        shared->label = std::string(shortName(kind)) + " " + std::to_string(nth + 1);
      }
      it = g_edits.emplace(k, std::move(shared)).first;
    }
    Edit& ed = *it->second;
    if (std::find(ed.at.begin(), ed.at.end(), p) == ed.at.end()) {
      if (same(*p, ed.now)) ed.at.push_back(p);
      else if (same(*p, ed.orig) && writable(p, sizeof(hitbox))) { put(p, ed.now); ed.at.push_back(p); }  // (a new fight's copy: the edit goes back on, as sync does)
    }
    return ed;
  }
  // The sprite on screen: copies of its edited boxes not known yet are picked up, and a copy still holding the game's
  // values (a new fight's file, or the other side's in a mirror) gets the edit
  void sync(asw_entity* e, const std::string& chr, const std::string& sprite, int total) {
    for (auto it = g_edits.lower_bound(Key{chr, sprite, -1}); it != g_edits.end() && it->first.chr == chr && it->first.sprite == sprite; ++it) {
      Edit& ed = *it->second;
      if (it->first.index >= total) continue;
      hitbox* p = &e->hitboxes[it->first.index];
      if (static_cast<int>(p->type) != ed.orig.kind) continue;
      const bool known = std::find(ed.at.begin(), ed.at.end(), p) != ed.at.end();
      if (same(*p, ed.now)) { if (!known) ed.at.push_back(p); }
      else if (same(*p, ed.orig) && writable(p, sizeof(hitbox))) { put(p, ed.now); if (!known) ed.at.push_back(p); }  // (also a rematch loaded at the same address)
    }
  }

  // ---- script edits: the game thread writes them into the loaded script, as with boxes ----
  struct SKey {
    std::string chr, state;
    int offset = 0, field = 0;  // the line's offset in the state, the bytes' place in the line
    bool operator<(const SKey& o) const { return std::tie(chr, state, offset, field) < std::tie(o.chr, o.state, o.offset, o.field); }
  };
  struct SEdit {
    int op = 0, field = 0;
    std::vector<uint8_t> sig;  // (a sprite line: its sprite's name, checked before any write, so only that line is ever written)
    std::vector<uint8_t> orig, now;
    std::vector<uint8_t*> at;
  };
  bool sameLine(const uint8_t* p, const SEdit& ed) {  // the line at p still the one edited (p: the edited bytes)
    const uint8_t* line = p - ed.field;
    if (!readable(line, 4)) return false;
    const bool replaced = ed.field == 0 && ed.now.size() >= 4 && i32(line) == i32(ed.now.data());  // (a replaced line: ours, or the game's)
    if (i32(line) != ed.op && !replaced) return false;
    return ed.sig.empty() || (readable(line + 4, ed.sig.size()) && std::memcmp(line + 4, ed.sig.data(), ed.sig.size()) == 0);
  }
  std::map<SKey, SEdit> g_sedits;
  struct SReq { enum T { Frames, Level, Reset, Arg, Version, Replace } t = Frames; int side = -1; std::string state; int offset = 0, value = 0, field = 0; std::string folder; };
  std::mutex g_sreq_m;
  std::vector<SReq> g_sreqs;
  void sask(SReq r) { std::lock_guard<std::mutex> lk(g_sreq_m); g_sreqs.push_back(std::move(r)); }
  std::string sideChr(asw_entity* e) {
    if (!e) return {};
    const char* sn = e->get_sprite_name();
    return chrOf(std::string(sn, strnlen(sn, 32)));
  }
  const uint8_t* stateStart(const States& st, const std::string& name) {
    for (size_t k = 0; k < st.names.size(); k++) if (st.names[k] == name) return st.starts[k];
    for (size_t k = 0; k < st.routines.size(); k++) if (st.routines[k] == name) return st.rstarts[k];
    return nullptr;
  }
  // An edit onto every loaded copy of the character's script (both sides' in a mirror: one file, or two), only where
  // the line is still the one edited and holds the game's value or ours
  void applySEdit(const SKey& k, SEdit& ed, asw_entity* const ents[2]) {
    for (int i = 0; i < 2; i++) {
      if (!ents[i] || sideChr(ents[i]) != k.chr) continue;
      const uint8_t* start = stateStart(g_states[i], k.state);
      if (!start) continue;
      uint8_t* line = const_cast<uint8_t*>(start) + k.offset;
      uint8_t* p = line + k.field;
      if (!writable(line, 4) || !writable(p, ed.now.size()) || !sameLine(p, ed)) continue;
      if (std::memcmp(p, ed.orig.data(), ed.orig.size()) != 0 && std::memcmp(p, ed.now.data(), ed.now.size()) != 0) continue;
      std::memcpy(p, ed.now.data(), ed.now.size());
      if (std::find(ed.at.begin(), ed.at.end(), p) == ed.at.end()) ed.at.push_back(p);
    }
  }
  void putBackSEdit(SEdit& ed) {
    for (uint8_t* p : ed.at)
      if (writable(p, ed.now.size()) && sameLine(p, ed) && std::memcmp(p, ed.now.data(), ed.now.size()) == 0) std::memcpy(p, ed.orig.data(), ed.orig.size());
    ed.at.clear();
  }

  // ---- .bbs text back into script bytes: one instruction per line, "name: arg, arg", as argsOf writes them ----
  std::map<std::string, int> g_op_by_name;
  bool parseInt(const std::string& t, int& v) {
    if (t.empty()) return false;
    char* end = nullptr;
    const long long x = std::strtoll(t.c_str(), &end, 10);
    if (!end || *end != 0 || x < INT32_MIN || x > 0xFFFFFFFFll) return false;
    v = static_cast<int>(static_cast<uint32_t>(x));
    return true;
  }
  // a move's code the game can run: walked by the game's sizes exactly to its endState, every block (if, upon, label...)
  // closed (the game skips a block by looking for its end: an open one runs on past it)
  bool wellFormed(const std::vector<uint8_t>& c, std::string& err) {
    if (!bbscript::instruction_sizes) { err = "no instruction sizes"; return false; }
    int depth = 0;
    for (size_t p = 0; p + 4 <= c.size();) {
      const int op = i32(c.data() + p);
      if (op < 0 || op >= static_cast<int>(bbscript::opcode::MAX) || bbscript::instruction_sizes[op] < 4) { err = "an unknown instruction (" + std::to_string(op) + ")"; return false; }
      auto d = g_db.instr.find(op);
      const int blk = d != g_db.instr.end() ? d->second.block : 0;
      if (op != 0 && op != 1) depth = (std::max)(0, depth + blk);  // (a stray end: the game's own moves have a couple)
      p += static_cast<size_t>(bbscript::instruction_sizes[op]);
      if (op == 1) {
        if (p != c.size()) { err = "something after endState"; return false; }
        if (depth != 0) { err = std::to_string(depth) + " block(s) not closed (an if, upon or label without its end)"; return false; }
        return true;
      }
    }
    err = "no endState";
    return false;
  }
  bool assemble(const std::string& text, std::vector<uint8_t>& out, std::string& err) {
    if (!bbscript::instruction_sizes || g_db.instr.empty()) { err = "no instruction database"; return false; }
    if (g_op_by_name.empty()) for (const auto& [o, in] : g_db.instr) if (!in.name.empty()) g_op_by_name.emplace(in.name, o);
    std::istringstream is(text);
    std::string l;
    int ln = 0;
    while (std::getline(is, l)) {
      ln++;
      l = trim(l);
      if (l.empty() || l[0] == '#' || l.rfind("//", 0) == 0) continue;
      auto bad = [&](const std::string& why) { err = "line " + std::to_string(ln) + ": " + why; return false; };
      const size_t colon = l.find(':');
      if (colon == std::string::npos) return bad("no ':' after the instruction's name");
      const std::string name = trim(l.substr(0, colon));
      int op = -1;
      auto it = g_op_by_name.find(name);
      if (it != g_op_by_name.end()) op = it->second;
      else if (name.rfind("Unknown", 0) == 0 && !parseInt(name.substr(7), op)) op = -1;
      if (name.empty() || op < 0 || op >= static_cast<int>(bbscript::opcode::MAX)) return bad("no instruction called '" + name + "'");
      const int size = bbscript::instruction_sizes[op];
      if (size < 4) return bad(name + ": the game has no size for it");
      // the values, split at commas outside quotes and brackets
      std::vector<std::string> tok;
      {
        const std::string rest = l.substr(colon + 1);
        std::string cur;
        int paren = 0;
        bool quote = false;
        for (size_t i = 0; i < rest.size(); i++) {
          const char c = rest[i];
          if (quote) { cur += c; if (c == '\\' && i + 1 < rest.size()) cur += rest[++i]; else if (c == '\'') quote = false; continue; }
          if (c == '\'') quote = true;
          if (c == '(') paren++;
          if (c == ')') paren--;
          if (c == ',' && paren == 0) { tok.push_back(trim(cur)); cur.clear(); continue; }
          cur += c;
        }
        if (!trim(cur).empty() || !tok.empty()) tok.push_back(trim(cur));
      }
      std::vector<uint8_t> b(static_cast<size_t>(size), 0);
      std::memcpy(b.data(), &op, 4);
      int at = 4;
      size_t ti = 0;
      auto d = g_db.instr.find(op);
      if (d != g_db.instr.end())
        for (const ArgDef& a : d->second.args) {
          const int need = a.kind == Arg::String32 ? 32 : a.kind == Arg::String16 ? 16 : a.kind == Arg::Value ? 8 : 4;
          if (at + need > size) break;
          if (ti >= tok.size()) return bad(name + ": too few values");
          const std::string t = tok[ti++];
          int v = 0;
          switch (a.kind) {
            case Arg::Number:
              if (!parseInt(t, v)) return bad(name + ": '" + t + "' isn't a number");
              std::memcpy(b.data() + at, &v, 4);
              break;
            case Arg::String16:
            case Arg::String32: {
              const std::string pre = a.kind == Arg::String16 ? "s16'" : "s32'";
              if (t.rfind(pre, 0) != 0 || t.size() < 5 || t.back() != '\'') return bad(name + ": '" + t + "' should be " + pre + "...'");
              std::string s;
              for (size_t i = 4; i + 1 < t.size(); i++) { if (t[i] == '\\' && i + 2 < t.size()) i++; s += t[i]; }
              if (static_cast<int>(s.size()) >= need) return bad(name + ": '" + s + "' is too long");
              std::memcpy(b.data() + at, s.data(), s.size());
              break;
            }
            case Arg::Value: {
              int tag = 0;
              if (t.rfind("Val(", 0) == 0 && t.back() == ')') { if (!parseInt(t.substr(4, t.size() - 5), v)) return bad(name + ": bad " + t); }
              else if (t.rfind("Mem(", 0) == 0 && t.back() == ')') {
                tag = 2;
                const std::string vn = t.substr(4, t.size() - 5);
                if (!parseInt(vn, v)) {
                  bool found = false;
                  for (const auto& [id, n] : g_db.vars) if (n == vn) { v = id; found = true; break; }
                  if (!found) return bad(name + ": no variable called " + vn);
                }
              } else if (t.rfind("BadTag(", 0) == 0 && t.back() == ')') {
                const size_t c = t.find(',');
                if (c == std::string::npos || !parseInt(trim(t.substr(7, c - 7)), tag) || !parseInt(trim(t.substr(c + 1, t.size() - c - 2)), v)) return bad(name + ": bad " + t);
              } else return bad(name + ": '" + t + "' should be Val(n) or Mem(name)");
              std::memcpy(b.data() + at, &tag, 4);
              std::memcpy(b.data() + at + 4, &v, 4);
              break;
            }
            case Arg::Enum: {
              if (t.size() > 2 && t.front() == '(' && t.back() == ')') {
                const std::string en = t.substr(1, t.size() - 2);
                auto m = g_db.enums.find(a.enum_name);
                bool found = false;
                if (m != g_db.enums.end()) for (const auto& [id, n] : m->second) if (n == en) { v = id; found = true; break; }
                if (!found) return bad(name + ": no " + a.enum_name + " called " + en);
              } else if (!parseInt(t, v)) return bad(name + ": '" + t + "' isn't a number or (NAME)");
              std::memcpy(b.data() + at, &v, 4);
              break;
            }
          }
          at += need;
        }
      if (at < size) {  // (what the database doesn't name: 0x and the bytes, as argsOf writes them)
        if (ti >= tok.size()) return bad(name + ": its last " + std::to_string(size - at) + " bytes are missing (0x...)");
        const std::string t = tok[ti++];
        if (t.rfind("0x", 0) != 0 || static_cast<int>(t.size()) != 2 + (size - at) * 2) return bad(name + ": '" + t + "' should be 0x and " + std::to_string((size - at) * 2) + " hex digits");
        for (int k = 0; k < size - at; k++) {
          const std::string h = t.substr(2 + k * 2, 2);
          char* end = nullptr;
          const long x = std::strtol(h.c_str(), &end, 16);
          if (!end || *end) return bad(name + ": bad hex " + h);
          b[static_cast<size_t>(at + k)] = static_cast<uint8_t>(x);
        }
      }
      if (ti < tok.size()) return bad(name + ": more values than it takes");
      out.insert(out.end(), b.begin(), b.end());
    }
    if (out.size() < 40 || i32(out.data()) != 0) { err = "the file should start with beginState: s32'<move>'"; return false; }
    if (i32(out.data() + out.size() - 4) != 1 || bbscript::instruction_sizes[1] != 4) { err = "the file should end with endState:"; return false; }
    return wellFormed(out, err);
  }
  std::string moveFileState(const std::filesystem::path& f) {  // the move a .bbs file's first line names
    std::ifstream in(f);
    std::string l;
    while (std::getline(in, l)) {
      l = trim(l);
      if (l.empty() || l[0] == '#' || l.rfind("//", 0) == 0) continue;
      const size_t a = l.find("s32'");
      if (l.rfind("beginState", 0) != 0 || a == std::string::npos) return {};
      const size_t b = l.find('\'', a + 4);
      return b == std::string::npos ? std::string() : l.substr(a + 4, b - a - 4);
    }
    return {};
  }
  std::string g_smsg;  // (a script request's result, shown on the window's status line)
  // ---- other battle versions of a move ----
  struct VerFile { std::string folder; std::vector<uint8_t> payload; };  // a version's script file (from its states table on)
  std::map<std::string, std::vector<VerFile>> g_ver_files;  // by character, read once per session when a move's versions are asked for
  std::string verLabel(const std::string& f) { return f.size() == 3 ? f.substr(0, 1) + "." + f.substr(1) : f; }  // "306" -> "3.06"
  const std::vector<VerFile>& versionFiles(const std::string& chr) {
    auto it = g_ver_files.find(chr);
    if (it != g_ver_files.end()) return it->second;
    std::vector<VerFile> out;
    const ULONGLONG t0 = GetTickCount64();
    const std::wstring wchr(chr.begin(), chr.end());
    const std::wstring dir = L"../../../RED/Content/Chara/" + wchr + L"/Common/Data";
    std::vector<std::wstring> folders;
    pak_mount::listDirs(dir, folders);
    std::vector<std::string> names;
    for (const std::wstring& f : folders) {
      std::string n;
      for (wchar_t c : f.substr(f.find_last_of(L"/\\") + 1)) n += static_cast<char>(c);
      if (n.size() == 3 && std::all_of(n.begin(), n.end(), [](char c) { return c >= '0' && c <= '9'; })) names.push_back(n);
    }
    std::sort(names.begin(), names.end());
    for (const std::string& n : names) {
      std::vector<uint8_t> b;
      const std::wstring wn(n.begin(), n.end());
      if (!pak_mount::readFile(dir + L"/" + wn + L"/BBS_" + wchr + L"_" + wn + L".uexp", b) || b.size() < 0x40) continue;
      out.push_back({n, std::vector<uint8_t>(b.begin() + 0x34, b.end())});  // (the script starts 0x34 into the cooked file)
    }
    COMBO_LOG("script & boxes: " + chr + "'s " + std::to_string(out.size()) + " battle versions read (" + std::to_string(GetTickCount64() - t0) + " ms)");
    return g_ver_files[chr] = std::move(out);
  }
  // A state's lines in a script file (its table, then code): the bytes from beginState to endState, or empty
  std::vector<uint8_t> stateBytes(const std::vector<uint8_t>& f, const std::string& name) {
    if (f.size() < 4 || !bbscript::instruction_sizes) return {};
    const uint32_t count = *reinterpret_cast<const uint32_t*>(f.data());
    if (count == 0 || count > 8000 || 4 + static_cast<size_t>(count) * 36 > f.size()) return {};
    const size_t code = 4 + static_cast<size_t>(count) * 36;
    for (uint32_t k = 0; k < count; k++) {
      if (cstr(f.data() + 4 + k * 36, 32) != name) continue;
      size_t p = code + *reinterpret_cast<const uint32_t*>(f.data() + 4 + k * 36 + 32);
      const size_t from = p;
      for (int n = 0; n < 20000 && p + 4 <= f.size(); n++) {
        const int op = *reinterpret_cast<const int32_t*>(f.data() + p);
        if (op < 0 || op >= static_cast<int>(bbscript::opcode::MAX)) return {};
        const int size = bbscript::instruction_sizes[op];
        if (size < 4 || p + size > f.size()) return {};
        p += size;
        if (op == 1) return std::vector<uint8_t>(f.begin() + from, f.begin() + p);
      }
      return {};
    }
    return {};
  }
  // A move loaded from another version: its lines in memory of ours, the state table entry pointed there
  struct Swap {
    std::string folder;
    std::vector<uint8_t> code;
    struct At { uint32_t* entry; uint32_t orig, ours; uint64_t ptr = 0; };  // (ptr: a copy entry's +8 pointer, checked before putting back)
    std::vector<At> at;
  };
  std::map<std::pair<std::string, std::string>, Swap> g_swaps;  // (chr, state)
  std::map<const uint8_t*, std::vector<std::pair<uint8_t*, size_t>>> g_swap_mem;  // our blocks, by the script they're near (kept: a fighter may still run one)
  uint8_t* allocNear(const uint8_t* code, size_t size) {  // above the script's code and within 4 GB of it (the table's offsets are 32-bit)
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    const uintptr_t base = reinterpret_cast<uintptr_t>(code);
    for (uintptr_t a = (base + 0x1000000 + gran - 1) / gran * gran; a + size < base + 0x70000000ull; a += gran * 16) {  // (positive as an int32 too)
      MEMORY_BASIC_INFORMATION mbi{};
      if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi)) || mbi.State != MEM_FREE) continue;
      if (void* p = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) return static_cast<uint8_t*>(p);
    }
    return nullptr;
  }
  // The game's own copies of a script file's move list (it starts moves from these, not from the file's table: a
  // version swap writes them too). Found by reading its writable memory through ReadProcessMemory (a page freed
  // meanwhile is a failed read, not a crash).
  struct TableCopy { uintptr_t base; size_t size; uint64_t ptr; };  // a run of the game's copy of the list: 24-byte entries, `ptr` 8 bytes after each start
  std::map<const uint8_t*, std::vector<TableCopy>> g_table_copies;  // (by script file, once its search is in)
  std::map<const uint8_t*, std::future<std::vector<TableCopy>>> g_table_scans;  // (by script file, while searching)
  std::vector<std::future<std::vector<TableCopy>>> g_scan_orphans;  // (searches no longer wanted: let finish, never waited on)
  struct ScanJob {
    std::vector<std::pair<uint32_t, int>> offs;  // (start, state), sorted; starts under 64 left out
    uint32_t count = 0;
    uintptr_t table = 0, other = 0;               // (both sides' own files: never a copy)
    std::vector<std::pair<uintptr_t, size_t>> own;  // the mod's own memory holding tables (the versions read from the paks)
    uintptr_t exe_lo = 0, exe_hi = 0;
    std::string what;
  };
  uintptr_t exeEnd() {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(dos) + dos->e_lfanew);
    return reinterpret_cast<uintptr_t>(dos) + nt->OptionalHeader.SizeOfImage;
  }
  // (worker) every run of memory shaped like the game's copy of the move list
  std::vector<TableCopy> scanWorker(ScanJob job) {
    std::vector<TableCopy> found;
    const ULONGLONG t0 = GetTickCount64();
    std::string list;
    try {
      std::vector<uint8_t> buf(1 << 20);
      auto own = [&](uintptr_t a, size_t n) {
        auto hit = [&](uintptr_t d, size_t m) { return a < d + m && d < a + n; };
        if (hit(reinterpret_cast<uintptr_t>(buf.data()), buf.size()) || hit(reinterpret_cast<uintptr_t>(job.offs.data()), job.offs.capacity() * sizeof(job.offs[0]))) return true;
        return false;  // (the script files and the versions read can't have the entry shape: never skipped for them)
      };
      if (job.offs.empty()) return found;
      const uint32_t lo = job.offs.front().first, hi = job.offs.back().first;
      auto idx = [&](uint32_t v) { auto it = std::lower_bound(job.offs.begin(), job.offs.end(), std::make_pair(v, -1)); return it != job.offs.end() && it->first == v ? it->second : -1; };
      int unique_starts = 0;
      for (size_t k = 0; k < job.offs.size(); k++) if (k == 0 || job.offs[k].first != job.offs[k - 1].first) unique_starts++;
      const int need = unique_starts * 9 / 10;
      if (unique_starts < 32) return found;  // (too few starts to tell a list from chance)
      MEMORY_BASIC_INFORMATION mbi{};
      for (uintptr_t a = 0x10000; a < 0x7FFFFFFF0000ull && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi)); a = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || (mbi.Protect & (PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE)) || !(mbi.Protect & PAGE_READWRITE)) continue;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = base + mbi.RegionSize;
        for (uintptr_t at = base; at < end; at += buf.size()) {
          const size_t n = static_cast<size_t>((std::min)(static_cast<uintptr_t>(buf.size()), end - at));
          if (own(at, n)) continue;
          SIZE_T got = 0;
          if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(at), buf.data(), n, &got)) continue;
          // starts with a pointer into the exe 8 bytes after them (the entry shape), in the order they lie
          std::vector<std::pair<size_t, int>> hits;
          for (size_t k = 0; k + 16 <= got; k += 8) {
            const uint32_t v = *reinterpret_cast<const uint32_t*>(buf.data() + k);
            if (v < lo || v > hi) continue;
            const uint64_t q = *reinterpret_cast<const uint64_t*>(buf.data() + k + 8);
            if (q < job.exe_lo || q >= job.exe_hi) continue;
            const int st = idx(v);
            if (st >= 0) hits.push_back({k, st});
          }
          if (static_cast<int>(hits.size()) < need) continue;
          // runs of 24-byte steps sharing one pointer: each is a copy if it holds about every start
          std::sort(hits.begin(), hits.end());
          for (size_t h = 0; h < hits.size();) {
            const uint64_t ptr = *reinterpret_cast<const uint64_t*>(buf.data() + hits[h].first + 8);
            std::vector<char> seen(job.count, 0);
            int distinct = 0;
            size_t e = h, last = hits[h].first;
            for (size_t g = h; g < hits.size(); g++) {
              if ((hits[g].first - hits[h].first) % 24 != 0) continue;
              if (hits[g].first - last > 24 * 8) break;  // (a gap: the run is over)
              if (*reinterpret_cast<const uint64_t*>(buf.data() + hits[g].first + 8) != ptr) continue;
              if (!seen[hits[g].second]) { seen[hits[g].second] = 1; distinct++; }
              last = hits[g].first;
              e = g;
            }
            if (distinct >= need) {
              found.push_back({at + hits[h].first, last + 16 - hits[h].first, ptr});  // (to the last entry's pointer)
              char line[120];
              snprintf(line, sizeof line, " [%p, %llu bytes: %d starts]", reinterpret_cast<void*>(at + hits[h].first), static_cast<unsigned long long>(last + 16 - hits[h].first), distinct);
              list += line;
              h = e + 1;
            } else h++;
          }
        }
      }
    } catch (...) { list += " (search failed)"; }
    COMBO_LOG("script & boxes: " + job.what + ": the game's copies of the move list (" + std::to_string(GetTickCount64() - t0) + " ms, in the background):" + (list.empty() ? std::string(" none") : list));
    return found;
  }
  // How many of the table's starts (offset >= 64) a copy holds in its entry shape, each counted once
  int startsIn(const uint8_t* table, const uint8_t* p, size_t n, uint64_t ptr) {
    const uint32_t count = *reinterpret_cast<const uint32_t*>(table);
    std::vector<uint32_t> offs;
    for (uint32_t k = 0; k < count; k++) { const uint32_t o = *reinterpret_cast<const uint32_t*>(table + 4 + k * 36 + 32); if (o >= 64) offs.push_back(o); }
    std::sort(offs.begin(), offs.end());
    offs.erase(std::unique(offs.begin(), offs.end()), offs.end());
    std::vector<char> seen(offs.size());
    int no = 0;
    for (size_t q = 0; q + 16 <= n; q += 24) {
      const uint32_t v = *reinterpret_cast<const uint32_t*>(p + q);
      if (*reinterpret_cast<const uint64_t*>(p + q + 8) != ptr) continue;
      auto it = std::lower_bound(offs.begin(), offs.end(), v);
      if (it != offs.end() && *it == v && !seen[it - offs.begin()]) { seen[it - offs.begin()] = 1; no++; }
    }
    return static_cast<int>(offs.empty() ? 0 : no * static_cast<int64_t>(count) / static_cast<int64_t>(offs.size()));
  }
  void startScan(const States& st, const uint8_t* other, const std::string& chr) {  // (game thread: what the search needs, copied)
    if (g_table_copies.count(st.buf) || g_table_scans.count(st.buf)) return;
    ScanJob job;
    job.count = *reinterpret_cast<const uint32_t*>(st.buf);
    for (uint32_t k = 0; k < job.count; k++) {
      const uint32_t o = *reinterpret_cast<const uint32_t*>(st.buf + 4 + k * 36 + 32);
      if (o >= 64) job.offs.push_back({o, static_cast<int>(k)});
    }
    std::sort(job.offs.begin(), job.offs.end());
    job.table = reinterpret_cast<uintptr_t>(st.buf);
    job.other = reinterpret_cast<uintptr_t>(other);
    for (const auto& [c, files] : g_ver_files)
      for (const VerFile& f : files) job.own.push_back({reinterpret_cast<uintptr_t>(f.payload.data()), f.payload.size()});
    job.exe_lo = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    job.exe_hi = exeEnd();
    job.what = chr;
    try { g_table_scans[st.buf] = std::async(std::launch::async, scanWorker, std::move(job)); }
    catch (...) { COMBO_LOG("script & boxes: the move list search couldn't start"); }
  }
  // A swapped state's start, in every copy of the list now known: where a copy holds `orig` in its entry shape
  void patchCopies(const uint8_t* buf, Swap& sw, uint32_t orig, uint32_t ours, const std::string& state) {
    auto cp = g_table_copies.find(buf);
    if (cp == g_table_copies.end()) return;
    const uint32_t count = *reinterpret_cast<const uint32_t*>(buf);
    int sharing = 0;  // (another move starting at the same place: its copy entry looks the same, so none is written)
    std::map<const uint32_t*, uint32_t> was;  // (swapped table entries: their original starts)
    for (const auto& [key, other] : g_swaps)
      for (const auto& a : other.at) if (a.ptr == 0) was[a.entry] = a.orig;
    for (uint32_t k = 0; k < count; k++) {  // (each entry's original start: a swapped one holds ours, its record the game's)
      const uint32_t* e = reinterpret_cast<const uint32_t*>(buf + 4 + k * 36 + 32);
      auto w = was.find(e);
      if ((w != was.end() ? w->second : *e) == orig) sharing++;
    }
    if (sharing > 1) { COMBO_LOG("script & boxes: " + state + " starts where another move does: only its table entry is changed"); return; }
    for (const TableCopy& tc : cp->second) {
      std::vector<uint8_t> tmp(tc.size);
      SIZE_T got = 0;
      if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(tc.base), tmp.data(), tc.size, &got)) continue;
      if (startsIn(buf, tmp.data(), got, tc.ptr) < static_cast<int>(count) * 9 / 10) {  // (not the list any more: never written)
        COMBO_LOG("script & boxes: a copy of the move list isn't there any more: left alone");
        continue;
      }
      for (size_t q = 0; q + 16 <= got; q += 24) {
        if (*reinterpret_cast<const uint32_t*>(tmp.data() + q) != orig || *reinterpret_cast<const uint64_t*>(tmp.data() + q + 8) != tc.ptr) continue;
        uint32_t* c = reinterpret_cast<uint32_t*>(tc.base + q);
        bool known = false;
        for (const auto& a : sw.at) if (a.entry == c) known = true;
        if (known || !writable(c, 4) || *c != orig) continue;
        sw.at.push_back({c, orig, ours, tc.ptr});
        *c = ours;
        COMBO_LOG("script & boxes: " + state + ": the game's copy of the move list points at it too");
      }
    }
  }
  void applySwap(const std::string& chr, const std::string& state, Swap& sw, asw_entity* const ents[2]) {
    for (int i = 0; i < 2; i++) {
      if (!ents[i] || sideChr(ents[i]) != chr) continue;
      const States& st = g_states[i];
      if (!st.buf) continue;
      const uint32_t count = *reinterpret_cast<const uint32_t*>(st.buf);
      const uint8_t* code = st.buf + 4 + static_cast<size_t>(count) * 36;
      for (uint32_t k = 0; k < count && k < st.names.size(); k++) {
        if (st.names[k] != state) continue;
        uint32_t* entry = reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(st.buf) + 4 + k * 36 + 32);
        if (!writable(entry, 4)) break;
        bool known = false;
        for (const auto& a : sw.at) if (a.entry == entry) known = true;
        if (known) break;  // (both sides on one file: once)
        auto& mem = g_swap_mem[st.buf];
        {  // (a table still pointing into one of our blocks isn't the game's: never recorded as its original)
          const uint8_t* now_at = code + *entry;
          bool ours_now = false;
          for (auto& [p, n] : mem) if (now_at >= p && now_at < p + n) ours_now = true;
          if (ours_now) { COMBO_LOG("script & boxes: " + state + " still points at a loaded copy: not changed (reload the fight)"); break; }
        }
        uint8_t* block = nullptr;
        for (auto& [p, n] : mem) if (n >= sw.code.size() && std::memcmp(p, sw.code.data(), sw.code.size()) == 0) block = p;  // (this one's, from before)
        if (!block) {
          block = allocNear(code, sw.code.size() + 64);  // (padded: nothing reads past the end into another block)
          if (!block) { COMBO_LOG("script & boxes: no memory near the script for " + state); break; }
          std::memcpy(block, sw.code.data(), sw.code.size());
          mem.push_back({block, sw.code.size()});
        }
        const uint32_t orig = *entry, ours = static_cast<uint32_t>(block - code);
        sw.at.push_back({entry, orig, ours});
        *entry = ours;
        if (orig < 64) {  // (a move at the very start of the file: its start is too common a number to look for)
          COMBO_LOG("script & boxes: " + state + " starts at the very start of the file: only its table entry is changed");
          break;
        }
        if (g_table_copies.count(st.buf)) patchCopies(st.buf, sw, orig, ours, state);  // (else once the background search is in)
        else startScan(st, g_states[1 - i].buf != st.buf ? g_states[1 - i].buf : nullptr, chr);
        break;
      }
    }
  }
  // (game thread, each tick) a background search that's in: its copies kept, and every swap on that file written into them
  void pollScans(asw_entity* const ents[2]) {
    for (auto it = g_scan_orphans.begin(); it != g_scan_orphans.end();)
      it = it->wait_for(std::chrono::seconds(0)) == std::future_status::ready ? g_scan_orphans.erase(it) : std::next(it);
    for (auto it = g_table_scans.begin(); it != g_table_scans.end();) {
      if (it->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { ++it; continue; }
      const uint8_t* buf = it->first;
      std::vector<TableCopy> copies;
      try { copies = it->second.get(); } catch (...) {}
      it = g_table_scans.erase(it);
      bool loaded = false;  // (the fighters' scripts now: g_states may be cleared for a moment by a version or reset)
      for (int i = 0; i < 2; i++)
        if (ents[i] && ents[i]->bbs_file && readable(ents[i]->bbs_file, 8) && *reinterpret_cast<const uint8_t* const*>(ents[i]->bbs_file) == buf) loaded = true;
      if (!loaded) continue;  // (that file isn't loaded any more)
      if (copies.empty()) continue;  // (none found: the next swap searches again)
      g_table_copies[buf] = std::move(copies);
      const uint32_t count = *reinterpret_cast<const uint32_t*>(buf);
      const uint8_t* table_end = buf + 4 + static_cast<size_t>(count) * 36;
      for (auto& [key, sw] : g_swaps) {
        const std::vector<Swap::At> entries = sw.at;  // (the table entries on this file: their copies now)
        for (const auto& a : entries)
          if (reinterpret_cast<const uint8_t*>(a.entry) >= buf && reinterpret_cast<const uint8_t*>(a.entry) < table_end && a.orig >= 64)
            patchCopies(buf, sw, a.orig, a.ours, key.second);
      }
    }
  }
  void dropScans() {  // (a new fight's files, or out of training: searches still running aren't waited on)
    for (auto& [b, f] : g_table_scans) g_scan_orphans.push_back(std::move(f));
    g_table_scans.clear();
    g_table_copies.clear();
  }
  void putBackSwap(Swap& sw) {
    for (const auto& a : sw.at) {
      if (a.ptr && !(readable(reinterpret_cast<const uint8_t*>(a.entry) + 8, 8) && *reinterpret_cast<const uint64_t*>(reinterpret_cast<const uint8_t*>(a.entry) + 8) == a.ptr)) continue;  // (not the list any more)
      if (writable(a.entry, 4) && *a.entry == a.ours) *a.entry = a.orig;
    }
    sw.at.clear();
  }
  std::vector<SideView::VersionView> versionsOf(const std::string& chr, const std::string& state, const States& st) {
    std::vector<SideView::VersionView> out;
    const std::vector<VerFile>& files = versionFiles(chr);
    std::vector<uint8_t> today;
    if (st.buf && bbscript::instruction_sizes) {  // today's: the loaded script's own (the table's offset, or the one our swap replaced)
      const uint32_t count = *reinterpret_cast<const uint32_t*>(st.buf);
      const uint8_t* code = st.buf + 4 + static_cast<size_t>(count) * 36;
      for (uint32_t k = 0; k < count && k < st.names.size(); k++) {
        if (st.names[k] != state) continue;
        uint32_t off = *reinterpret_cast<const uint32_t*>(st.buf + 4 + k * 36 + 32);
        auto sw = g_swaps.find({chr, state});
        if (sw != g_swaps.end()) for (const auto& a : sw->second.at) if (a.entry == reinterpret_cast<const uint32_t*>(st.buf + 4 + k * 36 + 32)) off = a.orig;
        const uint8_t* p = code + off;
        for (int n = 0; n < 20000 && readable(p, 4); n++) {
          const int op = i32(p);
          if (op < 0 || op >= static_cast<int>(bbscript::opcode::MAX)) break;
          const int size = bbscript::instruction_sizes[op];
          if (size < 4 || !readable(p, static_cast<size_t>(size))) break;
          today.insert(today.end(), p, p + size);
          p += size;
          if (op == 1) break;
        }
      }
    }
    std::vector<uint8_t> prev;
    for (const VerFile& f : files) {
      std::vector<uint8_t> b = stateBytes(f.payload, state);
      if (b.empty()) { prev.clear(); continue; }
      if (!out.empty() && b == prev) { out.back().label = out.back().label.substr(0, out.back().label.find('-')) + "-" + verLabel(f.folder); out.back().now = b == today; continue; }
      out.push_back({verLabel(f.folder), f.folder, b == today});
      prev = std::move(b);
    }
    (void)st;
    return out;
  }

  // An instruction that may be replaced, or replace another: not a block line, not a sprite; as the new one,
  // not one taking a name either
  bool replaceable(int op, bool as_new) {
    if (op < 0 || op >= static_cast<int>(bbscript::opcode::MAX) || !bbscript::instruction_sizes || op == 2) return false;
    auto it = g_db.instr.find(op);
    if (it == g_db.instr.end() || it->second.block != 0) return false;
    const std::string& n = it->second.name;
    for (const char* p : {"if", "else", "end", "begin", "upon", "label", "goto", "jumpTo", "callSubroutine"}) if (n.rfind(p, 0) == 0) return false;
    if (as_new) for (const ArgDef& a : it->second.args) if (a.kind == Arg::String16 || a.kind == Arg::String32) return false;
    return true;
  }
  void handleS(const SReq& r, asw_entity* const ents[2]) {
    if (r.side < 0 || r.side > 1 || !ents[r.side]) return;
    if (!bbscript::scriptReaderAvailable() || !bbscript::instruction_sizes) return;  // (a game patch moved the script reader: nothing is edited)
    const std::string chr = sideChr(ents[r.side]);
    if (r.t == SReq::Version) {  // (the move's edits go with it: they were made on the other script)
      Swap sw;  // (made first: a file or version that doesn't load leaves the move as it was)
      sw.folder = r.folder;
      if (!r.folder.empty()) {
        std::string err;
        if (r.folder.rfind("file:", 0) == 0) {  // from a .bbs file
          const std::filesystem::path f = g_dir / chr / (r.folder.substr(5) + ".bbs");
          std::ifstream in(f, std::ios::binary);
          std::stringstream ss;
          ss << in.rdbuf();
          if (!in) err = "couldn't read " + r.folder.substr(5) + ".bbs";
          else if (!assemble(ss.str(), sw.code, err)) {}
          else if (cstr(sw.code.data() + 4, 32) != r.state) err = "the file is " + cstr(sw.code.data() + 4, 32) + ", not " + r.state;
        } else {
          for (const VerFile& f : versionFiles(chr)) if (f.folder == r.folder) sw.code = stateBytes(f.payload, r.state);
          if (sw.code.empty()) err = "no version " + r.folder;
          else if (!wellFormed(sw.code, err)) err = "version " + verLabel(r.folder) + " doesn't fit today's instructions: " + err;
        }
        if (!err.empty()) {
          g_smsg = (r.folder.rfind("file:", 0) == 0 ? r.folder.substr(5) + ".bbs: " : std::string()) + err;
          COMBO_LOG("script & boxes: " + chr + " " + r.state + ": " + g_smsg);
          return;
        }
      }
      for (auto it = g_sedits.begin(); it != g_sedits.end();) {
        if (it->first.chr == chr && it->first.state == r.state) { putBackSEdit(it->second); it = g_sedits.erase(it); }
        else ++it;
      }
      auto old = g_swaps.find({chr, r.state});
      if (old != g_swaps.end()) { putBackSwap(old->second); g_swaps.erase(old); }
      for (int i = 0; i < 2; i++)  // (read again if an earlier request in this batch cleared them)
        if (ents[i] && !g_states[i].buf) readStates(ents[i], g_states[i]);
      if (!r.folder.empty()) {
        auto& at = g_swaps[{chr, r.state}] = std::move(sw);
        applySwap(chr, r.state, at, ents);
        COMBO_LOG("script & boxes: " + chr + " " + r.state + ": loaded from " + (r.folder.rfind("file:", 0) == 0 ? r.folder.substr(5) + ".bbs" : "version " + verLabel(r.folder)) +
                  " (" + std::to_string(at.code.size()) + " bytes, " + std::to_string(at.at.size()) + " table entries pointed at it)");
        if (r.folder.rfind("file:", 0) == 0) g_smsg = "Loaded " + r.folder.substr(5) + ".bbs as " + r.state + (at.at.empty() ? " (couldn't apply it: see combo_log.txt)" : "");
        if (at.at.empty()) g_swaps.erase({chr, r.state});
      } else COMBO_LOG("script & boxes: " + chr + " " + r.state + ": back to today's version");
      g_sel_cache[r.side] = Sel{};
      g_states[0] = States{};  // (the moves' starts read again: the table points elsewhere now)
      g_states[1] = States{};
      return;
    }
    if (r.t == SReq::Reset) {
      for (auto it = g_swaps.begin(); it != g_swaps.end();) {  // (a loaded version goes too)
        if (it->first.first == chr && it->first.second == r.state) { putBackSwap(it->second); it = g_swaps.erase(it); g_states[0] = States{}; g_states[1] = States{}; }
        else ++it;
      }
      for (auto it = g_sedits.begin(); it != g_sedits.end();) {
        if (it->first.chr == chr && it->first.state == r.state) { putBackSEdit(it->second); it = g_sedits.erase(it); }
        else ++it;
      }
      g_sel_cache[r.side] = Sel{};
      COMBO_LOG("script & boxes: " + chr + " " + r.state + ": script edits reset");
      return;
    }
    const uint8_t* start = stateStart(g_states[r.side], r.state);
    if (!start) return;
    const uint8_t* line = start + r.offset;
    if (!readable(line, 4)) return;
    const int op = i32(line);
    if (op < 0 || op >= static_cast<int>(bbscript::opcode::MAX) || !bbscript::instruction_sizes) return;
    const int size = bbscript::instruction_sizes[op];
    if (size < 4 || !readable(line, static_cast<size_t>(size))) return;
    if (r.t != SReq::Replace && size < 8) return;
    if (r.t == SReq::Replace) {  // the line becomes another instruction of its size
      const SKey rk{chr, r.state, r.offset, 0};
      auto rep = g_sedits.find(rk);
      const int was = rep != g_sedits.end() ? rep->second.op : op;
      if (!replaceable(was, false) || (r.value != was && (!replaceable(r.value, true) || bbscript::instruction_sizes[r.value] != size))) return;
      for (auto it = g_sedits.lower_bound(SKey{chr, r.state, r.offset, 1});  // (the line's other edits go: they were the old instruction's numbers)
           it != g_sedits.end() && it->first.chr == chr && it->first.state == r.state && it->first.offset == r.offset;) { putBackSEdit(it->second); it = g_sedits.erase(it); }
      std::vector<uint8_t> orig;
      if (rep != g_sedits.end()) { putBackSEdit(rep->second); orig = rep->second.orig; g_sedits.erase(rep); }
      else orig.assign(line, line + size);
      if (r.value != was) {
        SEdit ed;
        ed.op = was;
        ed.field = 0;
        ed.orig = orig;
        ed.now.assign(static_cast<size_t>(size), 0);
        std::memcpy(ed.now.data(), &r.value, 4);
        auto it = g_sedits.emplace(rk, std::move(ed)).first;
        applySEdit(rk, it->second, ents);
      }
      g_sel_cache[r.side] = Sel{};
      auto nm = [](int o) { auto it = g_db.instr.find(o); return it != g_db.instr.end() ? it->second.name : std::to_string(o); };
      COMBO_LOG("script & boxes: " + chr + " " + r.state + " +" + std::to_string(r.offset) + ": " + nm(was) + (r.value != was ? " replaced by " + nm(r.value) : " put back"));
      return;
    }
    if (r.t != SReq::Arg && size < 36) return;  // (frames and levels: a sprite line or a call)
    SKey k{chr, r.state, r.offset, 0};
    std::vector<uint8_t> now;
    if (r.t == SReq::Arg) {  // any number of the line, only at a place the database says holds one (a value only if it's a literal)
      auto dit = g_db.instr.find(op);
      if (dit == g_db.instr.end()) return;
      bool ok = false;
      int fat = 4;
      for (const ArgDef& a : dit->second.args) {
        const int need = a.kind == Arg::String32 ? 32 : a.kind == Arg::String16 ? 16 : a.kind == Arg::Value ? 8 : 4;
        if (fat + need > size) break;
        if ((a.kind == Arg::Number || a.kind == Arg::Enum) && r.field == fat) ok = true;
        if (a.kind == Arg::Value && r.field == fat + 4 && i32(line + fat) == 0) ok = true;
        fat += need;
      }
      if (!ok) return;
      k.field = r.field;
      const int32_t v = op == 2 && r.field == 36 ? std::clamp(r.value, 1, 999) : r.value;  // (a sprite's frames: as the Frames fields)
      now.resize(4);
      std::memcpy(now.data(), &v, 4);
    } else if (r.t == SReq::Frames) {
      if (op != 2 || size < 40) return;  // (a sprite line: name 32 bytes, then its frames)
      k.field = 36;
      const int32_t f = std::clamp(r.value, 1, 999);
      now.resize(4);
      std::memcpy(now.data(), &f, 4);
    } else {
      if ((op != 17 && op != 75) || cstr(line + 4, 32).rfind("cmn_AtkLv", 0) != 0) return;  // (a cmn_AtkLvN call)
      k.field = 4;
      char name[32] = {};
      snprintf(name, sizeof(name), "cmn_AtkLv%d", std::clamp(r.value, 0, 4));
      now.assign(name, name + 32);
    }
    if (k.field > 0) {  // (a replaced line: its numbers go into the replacement)
      auto rep = g_sedits.find(SKey{chr, r.state, r.offset, 0});
      if (rep != g_sedits.end()) {
        SEdit& ed = rep->second;
        if (i32(line) != i32(ed.now.data()) || k.field + now.size() > ed.now.size()) return;
        putBackSEdit(ed);
        std::memcpy(ed.now.data() + k.field, now.data(), now.size());
        applySEdit(rep->first, ed, ents);
        g_sel_cache[r.side] = Sel{};
        COMBO_LOG("script & boxes: " + chr + " " + r.state + " +" + std::to_string(r.offset) + ": (replaced line) value at +" + std::to_string(r.field) + " = " + std::to_string(r.value));
        return;
      }
    }
    auto it = g_sedits.find(k);
    if (it == g_sedits.end()) {
      SEdit ed;
      ed.op = op;
      ed.field = k.field;
      if (op == 2) ed.sig.assign(line + 4, line + 36);  // (a sprite line: its sprite's name too)
      ed.orig.assign(line + k.field, line + k.field + now.size());
      it = g_sedits.emplace(k, std::move(ed)).first;
    } else putBackSEdit(it->second);  // (the last value off first, so the new one goes where it was)
    it->second.now = now;
    if (it->second.now == it->second.orig) g_sedits.erase(it);
    else applySEdit(k, it->second, ents);
    g_sel_cache[r.side] = Sel{};  // (read again: the panel shows it)
    COMBO_LOG("script & boxes: " + chr + " " + r.state + " +" + std::to_string(r.offset) +
              (r.t == SReq::Frames ? ": frames " : r.t == SReq::Level ? ": attack level " : ": value at +" + std::to_string(r.field) + " = ") + std::to_string(r.value));
  }

  void syncScripts(asw_entity* const ents[2]);
  void syncScripts(asw_entity* const ents[2]) {  // (the edits onto a new fight's copies)
    if (g_sedits.empty() && g_swaps.empty()) return;
    for (int i = 0; i < 2; i++) {
      if (!ents[i]) continue;
      const uint8_t* buf = ents[i]->bbs_file && readable(ents[i]->bbs_file, 8) ? *reinterpret_cast<const uint8_t* const*>(ents[i]->bbs_file) : nullptr;
      if (g_states[i].buf != buf || g_states[i].names.empty()) readStates(ents[i], g_states[i]);
    }
    if (!g_swaps.empty()) {  // versions first (the table), then the starts read again, then the edits on what they point at
      for (auto& [k, sw] : g_swaps) applySwap(k.first, k.second, sw, ents);
      for (int i = 0; i < 2; i++) if (ents[i]) readStates(ents[i], g_states[i]);
    }
    for (auto& [k, ed] : g_sedits) applySEdit(k, ed, ents);
  }
  // ---- saving and loading the edits ----
  std::mutex g_io_m;
  std::string g_io_save, g_io_load;  // (asked for by the UI, done on the game thread)
  ULONGLONG g_io_when = 0;           // (when: one left over from leaving training isn't done in the next)
  std::string hexOf(const std::vector<uint8_t>& b) {
    static const char* h = "0123456789abcdef";
    std::string s;
    for (uint8_t c : b) { s += h[c >> 4]; s += h[c & 15]; }
    return s;
  }
  std::vector<uint8_t> bytesOf(const std::string& s) {
    std::vector<uint8_t> b;
    auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
      const int a = v(s[i]), c = v(s[i + 1]);
      if (a < 0 || c < 0) return {};
      b.push_back(static_cast<uint8_t>(a * 16 + c));
    }
    return b;
  }
  std::string fnum(float f) { char b[32]; snprintf(b, sizeof b, "%.9g", f); return b; }
  std::filesystem::path editsDir() { return g_dir / "edits"; }
  std::string ioSave(const std::string& name) {
    std::error_code ec;
    std::filesystem::create_directories(editsDir(), ec);
    std::ofstream f(editsDir() / (name + ".txt"), std::ios::trunc);
    if (!f) return "Couldn't write " + name + ".txt";
    f << "# StriveLabs Script & boxes edits: box / script / version lines (written by the mod; a load checks each against the game)\n";
    int nb = 0, ns = 0, nv = 0;
    for (const auto& [k, ed] : g_edits) {
      if (sameBox(ed->now, ed->orig)) continue;
      f << "box\t" << k.chr << "\t" << k.sprite << "\t" << k.index << "\t" << ed->orig.kind << "\t" << fnum(ed->orig.x) << " " << fnum(ed->orig.y) << " "
        << fnum(ed->orig.w) << " " << fnum(ed->orig.h) << "\t" << fnum(ed->now.x) << " " << fnum(ed->now.y) << " " << fnum(ed->now.w) << " " << fnum(ed->now.h)
        << "\t" << ed->label << "\n";
      nb++;
    }
    for (const auto& [k, sw] : g_swaps) {
      if (sw.folder.rfind("file:", 0) == 0) f << "code\t" << k.first << "\t" << k.second << "\t" << sw.folder << "\t" << hexOf(sw.code) << "\n";  // (a file's move: its bytes)
      else f << "version\t" << k.first << "\t" << k.second << "\t" << sw.folder << "\n";
      nv++;
    }
    for (const auto& [k, ed] : g_sedits) {
      f << "script\t" << k.chr << "\t" << k.state << "\t" << k.offset << "\t" << k.field << "\t" << ed.op << "\t" << (ed.sig.empty() ? "-" : hexOf(ed.sig))
        << "\t" << hexOf(ed.orig) << "\t" << hexOf(ed.now) << "\n";
      ns++;
    }
    f.flush();
    if (!f.good()) return "Couldn't write " + name + ".txt";
    const std::string msg = "Saved " + name + ": " + std::to_string(nb) + " boxes, " + std::to_string(ns) + " script lines, " + std::to_string(nv) + " versions";
    COMBO_LOG("script & boxes: " + msg);
    return msg;
  }
  std::string ioLoad(const std::string& name, asw_entity* const ents[2]) {
    std::ifstream f(editsDir() / (name + ".txt"));
    if (!f) return "Couldn't read " + name + ".txt";
    int nb = 0, ns = 0, nv = 0, bad = 0;
    std::string l;
    while (std::getline(f, l)) {
      if (!l.empty() && l.back() == '\r') l.pop_back();
      if (l.empty() || l[0] == '#') continue;
      std::vector<std::string> c;
      for (size_t a = 0;;) { const size_t b = l.find('\t', a); c.push_back(l.substr(a, b == std::string::npos ? std::string::npos : b - a)); if (b == std::string::npos) break; a = b + 1; }
      if (c[0] == "box" && c.size() >= 7) {
        auto ed = std::make_shared<Edit>();
        std::istringstream o(c[5]), n(c[6]);
        ed->orig.kind = ed->now.kind = std::atoi(c[4].c_str());
        if (!(o >> ed->orig.x >> ed->orig.y >> ed->orig.w >> ed->orig.h) || !(n >> ed->now.x >> ed->now.y >> ed->now.w >> ed->now.h)) { bad++; continue; }
        ed->label = c.size() > 7 ? c[7] : std::string();
        const Key k{c[1], c[2], std::atoi(c[3].c_str())};
        if (k.index < 0) { bad++; continue; }
        auto old = g_edits.find(k);
        if (old != g_edits.end()) {  // (the box's current edit put back first, its undo steps gone with it)
          apply(*old->second, old->second->orig);
          g_undo.erase(std::remove_if(g_undo.begin(), g_undo.end(), [&](const UndoStep& u) { return !(u.k < k) && !(k < u.k); }), g_undo.end());
        }
        g_edits[k] = ed;  // (goes on as the sprite shows, where it still holds the game's value)
        nb++;
      } else if ((c[0] == "version" && c.size() >= 4) || (c[0] == "code" && c.size() >= 5)) {
        Swap sw;
        sw.folder = c[3];
        if (c[0] == "code") {  // (a move from a file, saved as its bytes)
          sw.code = bytesOf(c[4]);
          std::string why;
          if (sw.code.size() < 40 || i32(sw.code.data()) != 0 || cstr(sw.code.data() + 4, 32) != c[2] || !wellFormed(sw.code, why)) sw.code.clear();
        } else {
          for (const VerFile& vf : versionFiles(c[1])) if (vf.folder == c[3]) sw.code = stateBytes(vf.payload, c[2]);
          std::string why;
          if (!sw.code.empty() && !wellFormed(sw.code, why)) sw.code.clear();
        }
        if (sw.code.empty()) { bad++; continue; }
        for (auto it = g_sedits.begin(); it != g_sedits.end();) {  // (the move's edits go, as in the UI: the file's own come after)
          if (it->first.chr == c[1] && it->first.state == c[2]) { putBackSEdit(it->second); it = g_sedits.erase(it); }
          else ++it;
        }
        auto old = g_swaps.find({c[1], c[2]});
        if (old != g_swaps.end()) putBackSwap(old->second);
        g_swaps[{c[1], c[2]}] = std::move(sw);
        nv++;
      } else if (c[0] == "script" && c.size() >= 9) {
        SEdit ed;
        ed.op = std::atoi(c[5].c_str());
        ed.field = std::atoi(c[4].c_str());
        if (c[6] != "-") { ed.sig = bytesOf(c[6]); if (ed.sig.empty()) { bad++; continue; } }
        ed.orig = bytesOf(c[7]);
        ed.now = bytesOf(c[8]);
        if (ed.orig.empty() || ed.orig.size() != ed.now.size()) { bad++; continue; }
        const SKey k{c[1], c[2], std::atoi(c[3].c_str()), ed.field};
        if (k.offset < 0 || ed.field < 0) { bad++; continue; }
        if (ed.field == 0) {  // (a replaced line: only a replace the UI allows, both instructions the saved size)
          const int to = ed.now.size() >= 4 ? i32(ed.now.data()) : -1;
          if (!replaceable(ed.op, false) || (to != ed.op && !replaceable(to, true)) || bbscript::instruction_sizes[ed.op] != static_cast<int>(ed.orig.size()) ||
              bbscript::instruction_sizes[to] != static_cast<int>(ed.now.size())) { bad++; continue; }
        }
        if (ed.field == 0)  // (a replaced line: the line's other edits go, as when replacing in the UI)
          for (auto it = g_sedits.lower_bound(SKey{k.chr, k.state, k.offset, 1}); it != g_sedits.end() && it->first.chr == k.chr && it->first.state == k.state && it->first.offset == k.offset;) {
            putBackSEdit(it->second); it = g_sedits.erase(it);
          }
        auto old = g_sedits.find(k);
        if (old != g_sedits.end()) putBackSEdit(old->second);
        g_sedits[k] = std::move(ed);
        ns++;
      } else bad++;
    }
    g_states[0] = States{};  // (versions first, then the starts read again, then the lines: as after a new fight)
    g_states[1] = States{};
    for (int i = 0; i < 2; i++) if (ents[i]) readStates(ents[i], g_states[i]);
    syncScripts(ents);
    g_sel_cache[0] = Sel{};
    g_sel_cache[1] = Sel{};
    const std::string msg = "Loaded " + name + ": " + std::to_string(nb) + " boxes, " + std::to_string(ns) + " script lines, " + std::to_string(nv) + " versions" +
                            (bad ? " (" + std::to_string(bad) + " lines not understood)" : std::string()) + ". Each goes on only where the game's value still matches.";
    COMBO_LOG("script & boxes: " + msg);
    return msg;
  }

  bool sEdited(const std::string& chr, const std::string& state, int offset) {
    auto it = g_sedits.lower_bound(SKey{chr, state, offset, 0});
    return it != g_sedits.end() && it->first.chr == chr && it->first.state == state && it->first.offset == offset;
  }

  void handle(const Req& r, asw_entity* const ents[2], View& v) {
    switch (r.t) {
      case Req::Set:
      case Req::Reset: {
        if (r.side < 0 || r.side > 1 || !ents[r.side]) return;
        const SideView& s = v.side[r.side];
        asw_entity* e = ents[r.side];
        if (s.sprite != r.sprite || r.index < 0 || r.index >= static_cast<int>(s.boxes.size())) return;  // (the sprite changed since)
        hitbox* p = &e->hitboxes[r.index];
        const int kind = static_cast<int>(p->type);
        if (!editable(kind) || kind != s.boxes[static_cast<size_t>(r.index)].kind) return;
        const Key k{s.chr, s.sprite, r.index};
        if (r.t == Req::Reset && g_edits.find(k) == g_edits.end()) return;
        int nth = 0;
        for (int j = 0; j < r.index; j++) if (static_cast<int>(e->hitboxes[j].type) == kind) nth++;
        Edit& ed = editFor(k, p, kind, nth);
        const BoxView to = r.t == Req::Reset ? ed.orig : r.b;
        if (sameBox(to, ed.now)) return;
        if (r.undo_point || r.t == Req::Reset) remember(k, ed.now, ++g_group);  // (a drag is one step: one that outlives its sprite ends with it)
        apply(ed, to);
        return;
      }
      case Req::Copy: {
        if (r.side < 0 || r.side > 1 || !ents[r.side]) return;
        const SideView& s = v.side[r.side];
        asw_entity* e = ents[r.side];
        if (s.sprite != r.sprite) { v.status = "The sprite changed before the copy: pause first."; return; }
        if (r.index < 0 || r.index >= static_cast<int>(s.boxes.size())) return;
        const hitbox src = e->hitboxes[r.index];
        const int kind = static_cast<int>(src.type);
        if (!editable(kind)) return;
        int nth = 0;
        for (int j = 0; j < r.index; j++) if (static_cast<int>(e->hitboxes[j].type) == kind) nth++;
        const Col* col = colOf(e);
        if (!col) { v.status = "The fighter's box file wasn't found: nothing copied."; COMBO_LOG("script & boxes: " + v.status); return; }
        {  // The box numbers here are the file's: only if the sprite on screen is its file entry as it is
          const ColSprite* fe = nullptr;  // (found by where its boxes are, not by its name)
          for (const auto& kv : col->sprites) if (kv.second.boxes == e->hitboxes) { fe = &kv.second; break; }
          bool same_layout = fe != nullptr;
          for (int k = 0; same_layout && k < 19; k++) same_layout = fe->counts[k] == e->box_counts[k];
          if (!same_layout) {
            char d[160];
            snprintf(d, sizeof(d), " (%s: on screen %p hurt %d hit %d, in the file %s hurt %d hit %d)", s.sprite.c_str(), static_cast<const void*>(e->hitboxes), e->box_counts[0], e->box_counts[1],
                     fe ? "found" : "not found", fe ? fe->counts[0] : -1, fe ? fe->counts[1] : -1);
            v.status = "This sprite's boxes don't match its file right now: nothing copied.";
            COMBO_LOG("script & boxes: " + v.status + d);
            return;
          }
        }
        const Key src_key{s.chr, s.sprite, r.index};  // (joins only the step of the edit it spreads)
        const bool own = r.join && !g_undo.empty() && !(g_undo.back().k < src_key) && !(src_key < g_undo.back().k) && g_undo.back().group == g_group;
        const int group = own ? g_group : ++g_group;
        int done = 0, without = 0;
        for (const std::string& name : r.to) {
          if (name == s.sprite) continue;
          auto sp = col->sprites.find(name);
          if (sp == col->sprites.end() || sp->second.counts[kind] <= nth) { without++; continue; }
          int idx = nth;
          for (int k = 0; k < kind; k++) idx += sp->second.counts[k];
          hitbox* p = sp->second.boxes + idx;
          if (!writable(p, sizeof(hitbox)) || static_cast<int>(p->type) != kind) { without++; continue; }
          const Key k{chrOf(name), name, idx};
          Edit& ed = editFor(k, p, kind, nth);
          BoxView to = ed.now;  // (its values as they are, edits included)
          if (r.fields & 1) to.x = src.x;
          if (r.fields & 2) to.y = src.y;
          if (r.fields & 4) to.w = src.w;
          if (r.fields & 8) to.h = src.h;
          done++;
          if (sameBox(to, ed.now)) continue;
          remember(k, ed.now, group);
          apply(ed, to);
        }
        v.status = "Copied " + std::string(shortName(kind)) + " " + std::to_string(nth + 1) + " to " + std::to_string(done) + (done == 1 ? " sprite" : " sprites") +
                   (without ? " (" + std::to_string(without) + " don't have that box)" : "") + ".";
        if (!r.join) COMBO_LOG("script & boxes: " + v.status);
        return;
      }
      case Req::ResetSprite:
      case Req::ResetAll:
        for (auto it = g_edits.begin(); it != g_edits.end();) {
          if (r.t == Req::ResetSprite && (it->first.chr != r.chr || it->first.sprite != r.sprite)) { ++it; continue; }
          apply(*it->second, it->second->orig);
          const Key k = it->first;
          g_undo.erase(std::remove_if(g_undo.begin(), g_undo.end(), [&](const UndoStep& u) { return !(u.k < k) && !(k < u.k); }), g_undo.end());
          it = g_edits.erase(it);
        }
        return;
      case Req::Undo: {
        int group = -1;  // the last group there is still an edit for, all of it
        while (!g_undo.empty() && (group < 0 || g_undo.back().group == group)) {
          const UndoStep u = g_undo.back();
          g_undo.pop_back();
          auto it = g_edits.find(u.k);
          if (it == g_edits.end()) continue;
          apply(*it->second, u.was);
          group = u.group;
        }
        return;
      }
    }
  }
}  // namespace

const char* kindName(int kind) {
  static const char* const k[19] = {"Hurtbox", "Hitbox", "ExPoint", "ExRect", "ExVector", "Push", "TempCenter", "Neck", "Abdominal", "SPGuard",
                                    "RLeg", "LLeg", "Private0", "Private1", "Private2", "Private3", "ExtendJon", "Unknown", "ComboHitbox"};
  return kind >= 0 && kind < 19 ? k[kind] : "?";
}
bool editable(int kind) { return kind >= 0 && kind < 19; }  // every kind the box file has, not only hurt and hit

void init(const std::filesystem::path& ue4ss_dir) {
  g_dir = ue4ss_dir / "StriveLabs_scripts";
  const auto db = g_dir / "db";
  g_db = Db{};
  g_db.ok = readRon(db / "GGSTRonRemake.ron");
  readLabels(db / "labels.json");
  if (g_db.ok) readDescs(db / "idNameWithDescription.json");
  std::lock_guard<std::mutex> lk(g_m);
  g_view.db = g_db.ok;
  g_view.db_note = g_db.ok ? std::to_string(g_db.instr.size()) + " instructions (BBScriptDX's database)"
                           : "No instruction database (StriveLabs_scripts/db/GGSTRonRemake.ron): instructions show as numbers.";
  COMBO_LOG("script & boxes: " + g_view.db_note);
}

void toggle() {
  g_open = !g_open;
  if (!g_open) { std::lock_guard<std::mutex> lk(g_req_m); g_reqs.clear(); }  // (one asked for as it closed would go in when it opens)
  std::lock_guard<std::mutex> lk(g_m);
  g_view.open = g_open;
}
bool isOpen() { return g_open && GetTickCount64() - g_ticked < 1000; }
View view() { std::lock_guard<std::mutex> lk(g_m); return g_view; }
void exportState(int side) { g_export_state = side; }
void selectState(int side, const std::string& name) {
  if (side < 0 || side > 1) return;
  std::lock_guard<std::mutex> lk(g_sel_m);
  g_sel_want[side] = name;
}
void exportBoxes(int side) { g_export_boxes = side; }
void editBox(int side, const std::string& sprite, int index, const BoxView& b, bool undo_point) {
  Req r;
  r.t = Req::Set; r.side = side; r.sprite = sprite; r.index = index; r.b = b; r.undo_point = undo_point;
  ask(std::move(r));
}
void resetBox(int side, const std::string& sprite, int index) { Req r; r.t = Req::Reset; r.side = side; r.sprite = sprite; r.index = index; ask(std::move(r)); }
void copyBox(int side, const std::string& sprite, int index, int fields, const std::vector<std::string>& to, bool join) {
  Req r;
  r.t = Req::Copy; r.side = side; r.sprite = sprite; r.index = index; r.fields = fields; r.to = to; r.join = join;
  ask(std::move(r));
}
void resetSprite(const std::string& chr, const std::string& sprite) { Req r; r.t = Req::ResetSprite; r.chr = chr; r.sprite = sprite; ask(std::move(r)); }
void resetAll() { Req r; r.t = Req::ResetAll; ask(std::move(r)); }
void setFrames(int side, const std::string& state, int offset, int frames) { SReq r; r.t = SReq::Frames; r.side = side; r.state = state; r.offset = offset; r.value = frames; sask(std::move(r)); }
void setLevel(int side, const std::string& state, int offset, int level) { SReq r; r.t = SReq::Level; r.side = side; r.state = state; r.offset = offset; r.value = level; sask(std::move(r)); }
void setArg(int side, const std::string& state, int offset, int field, int value) { SReq r; r.t = SReq::Arg; r.side = side; r.state = state; r.offset = offset; r.field = field; r.value = value; sask(std::move(r)); }
void loadMoveFile(int side, const std::string& state, const std::string& file) { loadVersion(side, state, "file:" + file); }
std::vector<std::pair<std::string, std::string>> moveFiles(const std::string& chr) {
  std::vector<std::pair<std::string, std::string>> out;
  std::error_code ec;
  try {  // (a name the system code page can't hold is left out, never a throw out of the menu)
    for (const auto& e : std::filesystem::directory_iterator(g_dir / chr, ec)) {
      if (e.path().extension() != ".bbs") continue;
      try { out.push_back({e.path().stem().string(), moveFileState(e.path())}); } catch (...) {}
    }
  } catch (...) {}
  std::sort(out.begin(), out.end());
  return out;
}
void loadVersion(int side, const std::string& state, const std::string& folder) { SReq r; r.t = SReq::Version; r.side = side; r.state = state; r.folder = folder; sask(std::move(r)); }
void saveEdits(const std::string& name) { std::lock_guard<std::mutex> lk(g_io_m); g_io_save = name; g_io_when = GetTickCount64(); }
void loadEdits(const std::string& name) { std::lock_guard<std::mutex> lk(g_io_m); g_io_load = name; g_io_when = GetTickCount64(); }
std::vector<std::string> savedEdits() {
  std::vector<std::string> out;
  std::error_code ec;
  try {  // (as moveFiles: such a name is left out)
    for (const auto& e : std::filesystem::directory_iterator(editsDir(), ec)) {
      if (e.path().extension() != ".txt") continue;
      try { out.push_back(e.path().stem().string()); } catch (...) {}
    }
  } catch (...) {}
  std::sort(out.begin(), out.end());
  return out;
}
void replaceLine(int side, const std::string& state, int offset, int op) { SReq r; r.t = SReq::Replace; r.side = side; r.state = state; r.offset = offset; r.value = op; sask(std::move(r)); }
std::vector<std::pair<int, std::string>> replacements(int op) {
  std::vector<std::pair<int, std::string>> out;
  if (!replaceable(op, false)) return out;
  const int size = bbscript::instruction_sizes[op];
  for (const auto& [o, in] : g_db.instr)
    if (o != op && o < static_cast<int>(bbscript::opcode::MAX) && bbscript::instruction_sizes[o] == size && replaceable(o, true)) out.push_back({o, in.name});
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
  return out;
}
void resetMove(int side, const std::string& state) { SReq r; r.t = SReq::Reset; r.side = side; r.state = state; sask(std::move(r)); }
void undo() { Req r; r.t = Req::Undo; ask(std::move(r)); }
void setKeep(bool keep) { g_keep = keep; }
void setHighlight(int side, int index) { g_hl_side = side; g_hl_index = index; }
namespace {
  std::mutex g_map_m;  // guards g_map, g_map_t
  ScreenMap g_map[2];
  unsigned long long g_map_t[2] = {};
}
void setScreenMap(const void* entity, const ScreenMap& m) {
  for (int s = 0; s < 2; s++)
    if (entity && g_ent[s] == entity) { std::lock_guard<std::mutex> lk(g_map_m); g_map[s] = m; g_map_t[s] = GetTickCount64(); }
}
bool screenMap(int side, ScreenMap& m) {
  if (side < 0 || side > 1) return false;
  std::lock_guard<std::mutex> lk(g_map_m);
  if (GetTickCount64() - g_map_t[side] > 200) return false;
  m = g_map[side];
  return true;
}
bool highlighted(const void* entity, int& index) {
  const int s = g_hl_side, i = g_hl_index;
  if (!isOpen() || s < 0 || s > 1 || i < 0 || !entity || g_ent[s] != entity) return false;
  index = i;
  return true;
}

void outsideTraining() {
  { std::lock_guard<std::mutex> lk(g_io_m); g_io_save.clear(); g_io_load.clear(); }  // (not carried into the next training)
  // In case a COL file outlives training (into an online fight, a replay, versus): the game's boxes go back before the
  // fight runs. Kept edits are picked up again in training, on the copies they find there. The same for script edits.
  for (auto& kv : g_sedits) putBackSEdit(kv.second);
  for (auto& kv : g_swaps) putBackSwap(kv.second);
  if (!g_keep) { g_sedits.clear(); g_swaps.clear(); }
  dropScans();  // (the game may build its move lists again: found again in training)
  if (g_edits.empty()) return;
  for (auto& kv : g_edits) {
    Edit& ed = *kv.second;
    for (hitbox* p : ed.at) if (writable(p, sizeof(hitbox)) && static_cast<int>(p->type) == ed.orig.kind && same(*p, ed.now)) put(p, ed.orig);
    ed.at.clear();
  }
  g_cols.clear();
  if (!g_keep) { g_edits.clear(); g_undo.clear(); }
}

void showObject(int side, const void* id) { if (side >= 0 && side < 2) g_show[side] = id; }

namespace {
  // Edits go back on wherever their sprite shows: the fighters and every object
  void syncAll(asw_engine* engine) {
    if (g_edits.empty() || !engine) return;
    const int n = (std::min)(engine->entity_count, 107);
    for (int k = 0; k < n; k++) {
      asw_entity* e = engine->entities[k];
      if (!e) continue;
      const char* sn = e->get_sprite_name();
      const std::string sprite(sn, strnlen(sn, 32)), chr = chrOf(sprite);
      auto it = g_edits.lower_bound(Key{chr, sprite, -1});
      if (it == g_edits.end() || it->first.chr != chr || it->first.sprite != sprite) continue;  // (no edits for it: nothing to read)
      const int total = boxCount(e);
      if (total >= 0) sync(e, chr, sprite, total);
    }
  }
}

void tick() {
  {  // Ctrl+Alt+B opens and closes it
    static bool was = false;
    const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState('B') & 0x8000);
    if (down && !was) toggle();
    was = down;
  }
  g_ticked = GetTickCount64();
  asw_engine* engine = asw_engine::get();
  asw_entity* ents[2] = {};
  for (int i = 0; i < 2; i++) ents[i] = engine ? engine->players[i].entity : nullptr;
  {  // New script files mean a new fight, its COL files loaded again too: the old copies are gone
    // (the files' addresses tell; a copy is only ever written while it still holds the edit's last value)
    static const void* files[2] = {};
    bool fresh = false;
    for (int i = 0; i < 2; i++) {
      const asw_entity* e = ents[i];
      const void* f = e && e->bbs_file && readable(e->bbs_file, 8) ? *reinterpret_cast<const void* const*>(e->bbs_file) : nullptr;
      if (f != files[i]) { fresh = true; files[i] = f; }
    }
    if (fresh) {
      dropScans();  // (a new fight's files: their copies are found again)
      g_cols.clear();
      g_col_failed.clear();
      g_col_tried[0].clear();
      g_col_tried[1].clear();
      // Put back first, where the memory still holds our values (a file that outlived the fight must not keep them),
      // then the records go: picked up again on the new files
      for (auto& kv : g_sedits) putBackSEdit(kv.second);
      for (auto& kv : g_swaps) putBackSwap(kv.second);
      for (auto& kv : g_edits) {
        Edit& ed = *kv.second;
        for (hitbox* p : ed.at) if (writable(p, sizeof(hitbox)) && static_cast<int>(p->type) == ed.orig.kind && same(*p, ed.now)) put(p, ed.orig);
        ed.at.clear();
      }
      if (!g_keep) { g_edits.clear(); g_undo.clear(); g_sedits.clear(); g_swaps.clear(); }
    }
    static ULONGLONG synced = 0;  // (script edits onto the copies: after a new fight, and now and then)
    pollScans(ents);  // (a background search of the move list that's in: its copies written)
    if (fresh || GetTickCount64() - synced > 1000) { syncScripts(ents); synced = GetTickCount64(); }
  }
  {  // Script edits asked for by the UI
    std::vector<SReq> sreqs;
    { std::lock_guard<std::mutex> lk(g_sreq_m); sreqs.swap(g_sreqs); }
    for (const SReq& r : sreqs) handleS(r, ents);
  }
  syncAll(engine);  // (open or closed: the edits stay, and go back on the copies a new fight loads as their sprites show)
  if (!g_open) return;
  // What each side shows: the fighter, or the object asked for while it's still out
  asw_entity* shown[2] = {ents[0], ents[1]};
  std::vector<ObjView> objects[2];
  int object[2] = {-1, -1};
  if (engine) {
    const int n = (std::min)(engine->entity_count, 107);
    for (int k = 0; k < n; k++) {
      asw_entity* o = engine->entities[k];
      if (!o || o->is_player) continue;
      for (int i = 0; i < 2; i++) {
        if (!ents[i] || o->parent_ply != ents[i]) continue;
        const char* sn = o->get_sprite_name();
        ObjView ov;
        ov.id = o;
        ov.sprite = std::string(sn, strnlen(sn, 32));
        const uint8_t* q = reinterpret_cast<const uint8_t*>(o->first_script_cmd);
        if (q && readable(q, 36) && i32(q) == 0) ov.state = cstr(q + 4, 32);  // (its state: beginState, then the name)
        if (o == g_show[i]) { shown[i] = o; object[i] = static_cast<int>(objects[i].size()); }
        objects[i].push_back(std::move(ov));
      }
    }
  }
  for (int i = 0; i < 2; i++) {
    if (object[i] < 0) g_show[i] = nullptr;  // (gone: back to the fighter, and a later object in its slot isn't taken for it)
    g_ent[i] = shown[i];
  }
  View v;
  v.open = true;
  v.db = g_db.ok;
  {
    std::lock_guard<std::mutex> lk(g_m);
    v.db_note = g_view.db_note;
    v.status = g_view.status;
  }
  for (int i = 0; i < 2; i++) {
    SideView& s = v.side[i];
    asw_entity* e = shown[i];
    if (!e) continue;
    s.present = true;
    s.objects = std::move(objects[i]);
    s.object = object[i];
    const char* sn = e->get_sprite_name();
    s.sprite = std::string(sn, strnlen(sn, 32));
    s.chr = chrOf(s.sprite);
    s.sprite_frame = e->sprite_duration;
    s.sprite_frames = e->sprite_total_duration;
    s.sx = e->scale_x < 0 ? -1 : 1;
    s.sy = e->scale_y < 0 ? -1 : 1;
    {  // the sprite's boxes, kind by kind as the game lays them out (only if every one of them can be read)
      const int total = boxCount(e);
      for (int j = 0; j < total; j++) { const hitbox& b = e->hitboxes[j]; s.boxes.push_back({static_cast<int>(b.type), b.x, b.y, b.w, b.h}); }
    }
    s.push_on = e->is_pushbox_active();
    if (s.push_on) e->get_pushbox(&s.push[0], &s.push[1], &s.push[2], &s.push[3]);
    s.throw_range = e->throw_range;
    s.strike_invuln = e->is_strike_invuln();
    s.throw_invuln = e->is_throw_invuln();
    Cache& c = g_cache[i];
    if (c.first != e->first_script_cmd || c.entity != e || c.chr != s.chr) { readScript(e, c); c.entity = e; c.chr = s.chr; }  // a new state (or fighter)
    s.state = c.state;
    s.script = c.script;
    s.note = c.note;
    const ptrdiff_t next = e->next_script_cmd - e->first_script_cmd;
    for (size_t k = 0; k < s.script.size(); k++) if (s.script[k].offset == next) { s.current = static_cast<int>(k); break; }
    {  // The fighter's moves, and the one opened in the small panel
      States& st = g_states[i];
      const uint8_t* buf = e->bbs_file && readable(e->bbs_file, 8) ? *reinterpret_cast<const uint8_t* const*>(e->bbs_file) : nullptr;
      static const void* states_of[2] = {};
      if (st.buf != buf || st.names.empty() || states_of[i] != e) { readStates(e, st); states_of[i] = e; }  // (a new fighter too)
      s.states = st.names;
      s.routines = st.routines;
      std::string want;
      { std::lock_guard<std::mutex> lk(g_sel_m); want = g_sel_want[i]; }
      Sel& sel = g_sel_cache[i];
      const uint8_t* start = want.empty() ? nullptr : stateStart(st, want);
      if (!start) { sel = Sel{}; }
      else if (sel.name != want || sel.start != start) { sel.name = want; sel.start = start; readScriptAt(start, sel.c); }
      s.selected = sel.name;
      s.sel_script = sel.c.script;
      {
        static std::string vers_for[2];
        static std::vector<SideView::VersionView> vers[2];
        const std::string key = s.chr + "/" + sel.name;
        if (!sel.name.empty() && vers_for[i] != key) { vers_for[i] = key; vers[i] = versionsOf(s.chr, sel.name, st); }
        if (!sel.name.empty()) s.sel_versions = vers[i];
        auto sw = g_swaps.find({s.chr, sel.name});
        if (sw != g_swaps.end()) s.sel_loaded = verLabel(sw->second.folder);
      }
      for (InstrView& in : s.sel_script) {
        in.edited = sEdited(s.chr, s.selected, in.offset);
        auto rep = in.edited ? g_sedits.find(SKey{s.chr, s.selected, in.offset, 0}) : g_sedits.end();
        if (rep != g_sedits.end()) { auto d = g_db.instr.find(rep->second.op); in.replaced_from = d != g_db.instr.end() ? d->second.name : std::to_string(rep->second.op); in.replaced_op = rep->second.op; }
      }
      if (sel.start && reinterpret_cast<const char*>(sel.start) == e->first_script_cmd)  // it's the move playing
        for (size_t k = 0; k < s.sel_script.size(); k++) if (s.sel_script[k].offset == next) { s.sel_current = static_cast<int>(k); break; }
      if (!s.sel_script.empty() && !g_db.ok) s.sel_frames.note = "no instruction database";
      else if (!s.sel_script.empty()) {  // its frames, and the one playing
        // (the box file: from the cache; looked for once per move and fighter, and again after a new fight, so a failed
        // lookup isn't repeated on every sprite)
        const Col* col = nullptr;
        const uint8_t* q = reinterpret_cast<const uint8_t*>(e->hitboxes);
        for (const Col& c : g_cols) if (q >= c.base && q < c.base + c.size) col = &c;
        char who[40];
        snprintf(who, sizeof(who), "|%p", static_cast<const void*>(e));
        const std::string tried = s.selected + who;
        if (!col && q && g_col_tried[i] != tried) { g_col_tried[i] = tried; col = colOf(e); }
        s.sel_frames = buildFrames(s.sel_script, col);
        if (s.sel_current >= 0) {  // the sprite playing: the one with the latest line before the next to run
          const CellView* at = nullptr;
          for (const CellView& c : s.sel_frames.cells) if (c.line < s.sel_current && (!at || c.line > at->line)) at = &c;
          if (at) s.sel_frames.now = at->first + (std::min)((std::max)(static_cast<int>(e->sprite_duration), 0), at->dur - 1);
        }
      }
    }
  }
  {  // Box edits asked for by the UI, then the boxes as they are now
    std::vector<Req> reqs;
    { std::lock_guard<std::mutex> lk(g_req_m); reqs.swap(g_reqs); }
    for (const Req& r : reqs) handle(r, shown, v);
    for (int i = 0; i < 2; i++) {
      SideView& s = v.side[i];
      if (!s.present) continue;
      for (size_t j = 0; j < s.boxes.size(); j++) {
        BoxView& b = s.boxes[j];
        const hitbox& h = shown[i]->hitboxes[j];
        b.x = h.x; b.y = h.y; b.w = h.w; b.h = h.h;
        auto it = g_edits.find(Key{s.chr, s.sprite, static_cast<int>(j)});
        if (it == g_edits.end() || sameBox(it->second->now, it->second->orig)) continue;
        const BoxView& o = it->second->orig;
        b.edited = true;
        b.ox = o.x; b.oy = o.y; b.ow = o.w; b.oh = o.h;
      }
    }
    for (const auto& kv : g_edits) {
      if (sameBox(kv.second->now, kv.second->orig)) continue;
      if (v.edits.empty() || v.edits.back().chr != kv.first.chr || v.edits.back().sprite != kv.first.sprite) v.edits.push_back({kv.first.chr, kv.first.sprite, ""});
      v.edits.back().boxes += (v.edits.back().boxes.empty() ? "" : ", ") + kv.second->label;
    }
    v.keep = g_keep;
    v.can_undo = !g_undo.empty();
  }
  {  // Save / load, asked for by the UI
    std::string save, load;
    {
      std::lock_guard<std::mutex> lk(g_io_m);
      save.swap(g_io_save);
      load.swap(g_io_load);
      if (GetTickCount64() - g_io_when > 1500) { save.clear(); load.clear(); }
    }
    if (!save.empty()) v.status = ioSave(save);
    if (!load.empty()) v.status = ioLoad(load, ents);
    if (!g_smsg.empty()) { v.status = g_smsg; g_smsg.clear(); }  // (a script request's result)
  }
  // Exports, asked for by the UI
  for (int which = 0; which < 2; which++) {
    const int side = (which ? g_export_boxes : g_export_state).exchange(-1);
    if (side < 0 || side > 1 || !v.side[side].present) continue;
    const SideView& s = v.side[side];
    if (which == 0) {
      const std::vector<InstrView>& sc = s.selected.empty() ? s.script : s.sel_script;  // (the opened move's, else the running one's)
      const std::string& state_name = s.selected.empty() ? s.state : s.selected;
      if (sc.empty()) { v.status = "No script to export."; continue; }
      std::string text;
      int was_depth = 0;
      for (const InstrView& iv : sc) {
        text += std::string(static_cast<size_t>(iv.depth) * 2, ' ') + iv.name + ": " + iv.args + "\n";
        if (iv.depth == 0 && was_depth > 0) text += "\n";
        was_depth = iv.depth;
      }
      writeFile(g_dir / s.chr / ((state_name.empty() ? std::string("state") : state_name) + ".bbs"), text, v.status);
      if (!s.selected.empty() && !s.sel_frames.cells.empty()) {  // (and the opened move's frames, as the strip shows them)
        std::string st;
        writeFile(g_dir / s.chr / (state_name + "_frames.txt"), framesText(s.sel_frames, state_name), st);
        if (st.rfind("Saved", 0) != 0) v.status = st;  // (a failed frames file shows)
      }
    } else {
      std::string j = "{\n  \"entries\": [\n    {\n      \"name\": " + jsonStr(s.sprite) + ",\n      \"names\": [" + jsonStr(s.sprite) + "],\n      \"box_groups\": [";
      bool first_group = true;
      for (int k = 0; k < 19; k++) {
        std::string boxes;
        for (const BoxView& b : s.boxes)
          if (b.kind == k)
            boxes += std::string(boxes.empty() ? "" : ",") + "\n            { \"kind\": " + std::to_string(k) + ", \"x_offset\": " + num(b.x) + ", \"y_offset\": " + num(b.y) +
                     ", \"width\": " + num(b.w) + ", \"height\": " + num(b.h) + " }";
        if (boxes.empty()) continue;
        j += std::string(first_group ? "" : ",") + "\n        {\n          \"group_index\": " + std::to_string(k) + ",\n          \"boxes\": [" + boxes + "\n          ]\n        }";
        first_group = false;
      }
      j += "\n      ]\n    }\n  ]\n}\n";
      writeFile(g_dir / s.chr / ("COL_" + s.sprite + ".json"), j, v.status);
    }
  }
  std::lock_guard<std::mutex> lk(g_m);
  g_view = std::move(v);
}
}  // namespace box_editor

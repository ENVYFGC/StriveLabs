// Script & boxes (see box_editor.h). Everything is read on the game thread in a training fight, into a view the UI
// draws. The fighters' boxes and scripts are the game's own loaded data (COL_<CHR> and BBS_<CHR>: the entity's box
// pointer and script pointers point into them); edits are written straight into them.
#include "box_editor.h"

#include <polyhook2/Detour/x64Detour.hpp>
#include "arcsys.h"
#include "bbscript.h"
#include "combo_log.h"
#include "combo_trials.h"
#include "imgui_overlay.h"
#include "bake_core.h"
#include "pak_mount.h"
#include "perf_log.h"
#include "script_pak.h"
#include "sigscan.h"

#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include <Windows.h>
#include <psapi.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <tuple>
#include <unordered_map>

namespace bbscript { extern short* instruction_sizes; }
std::string battleVersionName(int index);

namespace box_editor {
namespace {
  std::filesystem::path g_dir;  // ue4ss/StriveLabs_scripts
  std::mutex g_m;               // guards g_view
  View g_view;
  std::atomic<bool> g_open{false};
  std::atomic<unsigned long long> g_ticked{0};  // when a training fight last ran it (it only counts as open then)
  std::atomic<int> g_export_state{-1}, g_export_boxes{-1};  // a side to export, from the UI
  std::atomic<int> g_export_pak{-1};
  std::mutex g_export_m;
  std::string g_export_name[2];

  // ---- the instruction database (BBScriptDX's GGSTRonRemake.ron and idNameWithDescription.json) ----
  using ops::Arg;
  using ops::ArgDef;
  using ops::Instr;
  using ops::Db;
  using ops::trim;
  using ops::quoted;
  Db g_db;
  bool readRon(const std::filesystem::path& file) { return ops::readRon(file, g_db); }
  ops::Ctx ctx() { return ops::Ctx{bbscript::instruction_sizes, static_cast<int>(bbscript::opcode::MAX), &g_db}; }
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
  constexpr size_t kMaxCode = 1u << 20;
  const char* const kTooLong = "This script is longer than 1 MB (the most the editor reads or writes for one move or routine): only its first 1 MB is shown, and it can't be changed here.";
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
  bool labelOp(int op) { return op == 11 || op == 12 || op == 13 || op == 14 || op == 24 || op == 25 || op == 31 || op == 35; }
  

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

  std::string argsOf(const uint8_t* p, int size, const Instr* d) { return ops::argsOf(g_db, p, size, d); }
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
    const uint8_t* code = nullptr;
  };
  States g_states[2];
  template <class F> void walkCode(const uint8_t* p, F&& f) {
    // the routines: the code walked line by line from its start (the game's own instruction sizes), every
    // beginSubroutine (8, or 15 with arguments) kept; the walk stops at the first line it can't size
    if (!p || !bbscript::instruction_sizes) return;
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
      const bool begins = op == 0 || op == 8 || op == 15 || op == 77 || op == 78;
      const bool ends = op == 1 || op == 16;
      if (!inside && !begins && !ends) break;
      const int size = bbscript::instruction_sizes[op];
      if (size < 4 || size % 4 || p + size > region_end) break;
      if (!f(p, op, size)) break;
      if (begins) inside = true;
      if (ends) inside = false;
      p += size;
    }
  }
  void readStatesAt(const uint8_t* buf, States& st) {
    st = States{};
    if (!readable(buf, 4)) return;
    const uint32_t count = *reinterpret_cast<const uint32_t*>(buf);
    if (count == 0 || count > 8000 || !readable(buf, 4 + static_cast<size_t>(count) * 36)) return;
    const uint8_t* code = buf + 4 + static_cast<size_t>(count) * 36;
    st.buf = buf;
    st.code = code;
    for (uint32_t k = 0; k < count; k++) {
      st.names.push_back(cstr(buf + 4 + k * 36, 32));
      st.starts.push_back(code + *reinterpret_cast<const uint32_t*>(buf + 4 + k * 36 + 32));
    }
    walkCode(code, [&](const uint8_t* p, int op, int size) {
      if ((op == 8 || op == 15 || op == 77 || op == 78) && size >= 36) { st.routines.push_back(cstr(p + 4, 32) + (op == 77 ? " (pre)" : op == 78 ? " (post)" : "")); st.rstarts.push_back(p); }
      return true;
    });
  }
  const uint8_t* fileOf(const asw_entity* e) {
    return e && e->bbs_file && readable(e->bbs_file, 8) ? *reinterpret_cast<const uint8_t* const*>(e->bbs_file) : nullptr;
  }
  void readStates(asw_entity* e, States& st) {
    st = States{};
    if (!e->bbs_file || !readable(e->bbs_file, 8)) return;
    readStatesAt(*reinterpret_cast<const uint8_t* const*>(e->bbs_file), st);
  }
  struct Sel { std::string name; const uint8_t* start = nullptr; Cache c; std::vector<uint8_t> code; const uint8_t* code_at = nullptr; unsigned long long gen = 0; ULONGLONG read = 0; };
  Sel g_sel_cache[2];
  unsigned long long g_code_gen = 0;
  std::mutex g_sel_m;
  std::string g_sel_want[2];  // the move the UI opened, per side (under g_sel_m)
  std::vector<std::string> g_inl_want[2];
  void readScriptAt(const uint8_t* p, Cache& c) {
    c.script.clear();
    c.state.clear();
    c.note.clear();
    if (!bbscript::scriptReaderAvailable() || !bbscript::instruction_sizes) { c.note = "The game's script reader wasn't found (a newer game version?)."; return; }
    if (!readable(p, 36)) { c.note = "No script."; return; }
    if (i32(p) == 0 || ((i32(p) == 8 || i32(p) == 15 || i32(p) == 77 || i32(p) == 78) && bbscript::instruction_sizes[i32(p)] >= 36)) c.state = cstr(p + 4, 32);  // beginState's (or a routine's) name
    int depth = 0;
    for (int at = 0;;) {
      if (static_cast<size_t>(at) >= kMaxCode) { c.note = kTooLong; break; }
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
          else if (a.kind == Arg::Value) {
            av.kind = 1; av.field = fat + 4; av.value = i32(q + fat + 4); av.editable = true; av.tag = i32(q + fat);
            if (av.tag == 2) if (auto it = g_db.vars.find(av.value); it != g_db.vars.end()) av.value_name = it->second;
          }
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
          } else {
            av.kind = 3; av.field = fat; av.size = need; av.text = cstr(q + fat, static_cast<size_t>(need));
            av.editable = true;
            av.label = op == 2 ? std::string("sprite") : op == 17 || op == 75 ? std::string("routine") : labelOf(d->name, k, false);
            av.tip = labelOf(d->name, k, true);
            if (av.label.empty()) av.label = "name";
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
  bool keepOld(const std::filesystem::path& f, const std::string& text, std::string& kept) {
    kept.clear();
    std::error_code ec;
    const bool there = std::filesystem::exists(f, ec);
    if (ec) return false;
    if (!there) return true;
    {
      std::ifstream in(f, std::ios::binary);
      const std::string was((std::istreambuf_iterator<char>(in)), {});
      if (in && was == text) return true;
    }
    for (int n = 1; n < 1000; n++) {
      std::filesystem::path o = f;
      o += n == 1 ? std::string(".old") : ".old" + std::to_string(n);
      const bool taken = std::filesystem::exists(o, ec);
      if (ec) return false;
      if (taken) continue;
      std::filesystem::copy_file(f, o, std::filesystem::copy_options::none, ec);
      if (ec) return false;
      kept = o.filename().string();
      return true;
    }
    return false;
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
    enum T { Set, Reset, ResetSprite, ResetAll, Undo, Copy, Redo, Goto } t = Set;
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
  // entries), each entry a JONB as the game's own reader takes it (names; a byte = box kinds + 3; three counts: 0x34
  // records, 0x50 editor blocks, 0x134 records; the box counts by kind; those records; then the boxes in kind order.
  // 19 kinds since 5.00, 18 before; checked against every entry of SOL's file and its 4.09 file) ----
  struct ColSprite { hitbox* boxes = nullptr; int counts[19] = {}; const uint8_t* rec = nullptr; uint32_t rec_size = 0, counts_at = 0; };
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
      if (static_cast<uint64_t>(ds) + off + size > total || size < 6 + 7 + 2) { bad++; continue; }
      const uint8_t* j = base + ds + off;
      if (std::memcmp(j, "JONB", 4) != 0) { bad++; continue; }
      size_t p = 6 + static_cast<size_t>(u16(j + 4)) * 32;  // (the names)
      if (p + 7 > size) { bad++; continue; }
      const int kinds = static_cast<int>(j[p]) - 3;
      if (kinds < 1 || kinds > 19 || p + 7 + static_cast<size_t>(kinds) * 2 > size) { bad++; continue; }
      const size_t recs = static_cast<size_t>(u16(j + p + 1)) * 0x34 + static_cast<size_t>(u16(j + p + 3)) * 0x50 + static_cast<size_t>(u16(j + p + 5)) * 0x134;
      const uint8_t* layers = j + p + 7;
      p += 7 + static_cast<size_t>(kinds) * 2 + recs;
      ColSprite s;
      size_t n = 0;
      for (int k = 0; k < kinds; k++) { const int m = u16(layers + k * 2); s.counts[k] = m; n += static_cast<size_t>(m); }
      if (p + n * 20 != size) { bad++; continue; }
      s.boxes = reinterpret_cast<hitbox*>(const_cast<uint8_t*>(j + p));
      s.rec = j;
      s.rec_size = size;
      s.counts_at = static_cast<uint32_t>(layers - j);
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
  const uint8_t* gameBoxes(const uint8_t* q);
  const Col* colOf(const asw_entity* e) {
    const uint8_t* q = gameBoxes(reinterpret_cast<const uint8_t*>(e->hitboxes));
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
    bool copy = false;
    bool operator<(const SKey& o) const { return std::tie(chr, state, offset, field, copy) < std::tie(o.chr, o.state, o.offset, o.field, o.copy); }
  };
  struct SEdit {
    int op = 0, field = 0;
    std::vector<uint8_t> sig;  // (a sprite line: its sprite's name, checked before any write, so only that line is ever written)
    std::vector<uint8_t> orig, now;
    std::vector<uint8_t*> at;
    std::vector<uint8_t> sig_alt;
  };
  bool sameLine(const uint8_t* p, const SEdit& ed) {  // the line at p still the one edited (p: the edited bytes)
    const uint8_t* line = p - ed.field;
    if (!readable(line, 4)) return false;
    const bool replaced = ed.field == 0 && ed.now.size() >= 4 && i32(line) == i32(ed.now.data());  // (a replaced line: ours, or the game's)
    if (i32(line) != ed.op && !replaced) return false;
    if (ed.sig.empty()) return true;
    if (!readable(line + 4, ed.sig.size())) return false;
    return std::memcmp(line + 4, ed.sig.data(), ed.sig.size()) == 0 ||
           (ed.sig_alt.size() == ed.sig.size() && std::memcmp(line + 4, ed.sig_alt.data(), ed.sig_alt.size()) == 0);
  }
  std::map<SKey, SEdit> g_sedits;
  void linkSigs() {
    for (auto& [k, ed] : g_sedits) {
      ed.sig_alt.clear();
      if (ed.sig.empty() || k.field == 4) continue;
      auto n = g_sedits.find(SKey{k.chr, k.state, k.offset, 4, k.copy});
      if (n != g_sedits.end() && n->second.now.size() == ed.sig.size()) ed.sig_alt = n->second.now;
    }
  }
  struct SReq { enum T { Frames, Level, Reset, Arg, Version, Replace, ResetLine, Name, Insert, Copy, Remove, Tag, Paste } t = Frames; int side = -1; std::string state; int offset = 0, value = 0, field = 0; std::string folder, text; };
  std::mutex g_sreq_m;
  std::vector<SReq> g_sreqs;
  void sask(SReq r) { std::lock_guard<std::mutex> lk(g_sreq_m); g_sreqs.push_back(std::move(r)); }
  std::string sideChr(asw_entity* e) {
    if (!e) return {};
    const char* sn = e->get_sprite_name();
    return chrOf(std::string(sn, strnlen(sn, 32)));
  }
  static_assert(sizeof(hitbox) == 20, "a box record is 20 bytes");
  constexpr int kOldBoxesVersion = 35;
  constexpr const wchar_t* kOldBoxesFolder = L"409";
  using ReadBoxes_t = char (*)(void*, const uint8_t*);
  ReadBoxes_t orig_ReadBoxes = nullptr;
  constexpr size_t kOldPad = 160;
  constexpr size_t kOldCopies = 8;
  struct OldCopy { std::vector<uint8_t> src, rec, clean; };
  struct OldRec { const OldCopy* copy = nullptr; size_t head = 0, boxes_at = 0; const uint8_t* game_boxes = nullptr; };
  std::unordered_map<const uint8_t*, OldRec> g_old_map;
  std::shared_mutex g_old_m;
  std::atomic<bool> g_old_want{false}, g_old_live{false};
  std::map<std::string, std::deque<OldCopy>> g_old_recs;
  std::map<std::string, std::map<std::string, std::vector<uint8_t>>> g_old_hurt;
  const uint8_t* g_old_base[2] = {};
  int g_old_tries[2] = {};
  ULONGLONG g_old_tried = 0;
  bool sameRecord(const uint8_t* rec, const OldRec& o) {
    const std::vector<uint8_t>& s = o.copy->src;
    return std::memcmp(rec, s.data(), o.head + 7) == 0 && std::memcmp(rec, s.data(), o.boxes_at) == 0 && std::memcmp(rec, s.data(), s.size()) == 0;
  }
  char hook_ReadBoxes(void* self, const uint8_t* rec) {
    if (rec && g_old_live.load(std::memory_order_relaxed) && combo_trials::trainingMode()) {
      std::shared_lock<std::shared_mutex> lk(g_old_m);
      const auto it = g_old_map.find(rec);
      if (it != g_old_map.end() && sameRecord(rec, it->second)) rec = it->second.copy->rec.data();
    }
    return orig_ReadBoxes(self, rec);
  }
  const uint8_t* gameBoxes(const uint8_t* q) {
    if (!q || g_old_map.empty()) return q;
    for (const auto& kv : g_old_map) {
      const uint8_t* a = kv.second.copy->rec.data();
      if (q >= a && q < a + kv.second.copy->rec.size()) return kv.second.game_boxes;
    }
    return q;
  }
  bool hitOrBlockSprite(const std::string& chr, const std::string& n) {
    if (chr.size() != 3 || n.size() < 7 || n[6] != '_') return false;
    for (int k = 0; k < 3; k++) if (n[k] != static_cast<char>(std::tolower(static_cast<unsigned char>(chr[k])))) return false;
    for (int k = 3; k < 6; k++) if (n[k] < '0' || n[k] > '9') return false;
    const int group = (n[3] - '0') * 100 + (n[4] - '0') * 10 + (n[5] - '0');
    return group >= 40 && group <= 99;
  }
  const std::map<std::string, std::vector<uint8_t>>& oldHurt(const std::string& chr) {
    const auto it = g_old_hurt.find(chr);
    if (it != g_old_hurt.end()) return it->second;
    std::map<std::string, std::vector<uint8_t>>& out = g_old_hurt[chr];
    const ULONGLONG t0 = GetTickCount64();
    const std::wstring w(chr.begin(), chr.end());
    std::vector<uint8_t> b;
    if (!pak_mount::readFile(L"../../../RED/Content/Chara/" + w + L"/Common/Data/" + kOldBoxesFolder + L"/COL_" + w + L"_" + kOldBoxesFolder + L".uexp", b)) {
      COMBO_LOG("old hurtboxes: " + chr + " has no 4.09 box file, its hurtboxes stay as they are");
      return out;
    }
    size_t at = b.size();
    for (size_t i = 0; i + 0x20 <= b.size() && i < 0x400; i++) if (std::memcmp(b.data() + i, "FPAC", 4) == 0) { at = i; break; }
    Col c;
    g_col_why.clear();
    if (at >= b.size() || u32(b.data() + at + 8) > b.size() - at || !readCol(b.data() + at, c)) {
      COMBO_LOG("old hurtboxes: " + chr + "'s 4.09 box file couldn't be read (" + g_col_why + ")");
      return out;
    }
    for (const auto& kv : c.sprites) {
      if (!hitOrBlockSprite(chr, kv.first)) continue;
      const uint8_t* from = reinterpret_cast<const uint8_t*>(kv.second.boxes);
      out[kv.first].assign(from, from + static_cast<size_t>(kv.second.counts[0]) * sizeof(hitbox));
    }
    COMBO_LOG("old hurtboxes: " + chr + "'s 4.09 box file read, " + std::to_string(out.size()) + " hit and block sprites (" + std::to_string(GetTickCount64() - t0) + " ms)");
    return out;
  }
  int addOldBoxes(const std::string& chr, const Col& col) {
    const std::map<std::string, std::vector<uint8_t>>& old = oldHurt(chr);
    int n = 0;
    for (const auto& kv : col.sprites) {
      const ColSprite& s = kv.second;
      const auto o = old.find(kv.first);
      if (o == old.end() || !s.rec || !s.boxes) continue;
      const uint8_t* cur = reinterpret_cast<const uint8_t*>(s.boxes);
      const size_t cur_bytes = static_cast<size_t>(s.counts[0]) * sizeof(hitbox), boxes_at = static_cast<size_t>(cur - s.rec);
      if (s.counts_at < 13 || boxes_at + cur_bytes > s.rec_size || o->second.size() / sizeof(hitbox) > 0xFFFF) continue;
      if (cur_bytes == o->second.size() && std::memcmp(cur, o->second.data(), cur_bytes) == 0) continue;
      std::deque<OldCopy>& copies = g_old_recs[kv.first];
      const OldCopy* copy = nullptr;
      for (const OldCopy& c : copies) if (c.src.size() == s.rec_size && std::memcmp(c.src.data(), s.rec, s.rec_size) == 0) { copy = &c; break; }
      if (!copy) {
        if (copies.size() >= kOldCopies) continue;
        copies.emplace_back();
        OldCopy& c = copies.back();
        c.src.assign(s.rec, s.rec + s.rec_size);
        c.rec.assign(s.rec, s.rec + boxes_at);
        c.rec.insert(c.rec.end(), o->second.begin(), o->second.end());
        c.rec.insert(c.rec.end(), cur + cur_bytes, s.rec + s.rec_size);
        const uint16_t count = static_cast<uint16_t>(o->second.size() / sizeof(hitbox));
        std::memcpy(c.rec.data() + s.counts_at, &count, 2);
        c.rec.insert(c.rec.end(), kOldPad, 0);
        c.clean = c.rec;
        copy = &c;
      }
      g_old_map[s.rec] = OldRec{copy, static_cast<size_t>(s.counts_at - 7), boxes_at, cur};
      n++;
    }
    return n;
  }
  void oldBoxesOff() {
    g_old_live = false;
    std::unique_lock<std::shared_mutex> lk(g_old_m);
    g_old_map.clear();
    for (auto& kv : g_old_recs)
      for (OldCopy& c : kv.second)
        if (c.rec.size() == c.clean.size() && std::memcmp(c.rec.data(), c.clean.data(), c.rec.size()) != 0) std::memcpy(c.rec.data(), c.clean.data(), c.rec.size());
    g_old_base[0] = g_old_base[1] = nullptr;
    g_old_tries[0] = g_old_tries[1] = 0;
  }
  void oldBoxesTick(asw_entity* const ents[2], bool fresh) {
    if (fresh) oldBoxesOff();
    const int bv = combo_trials::battleVersion();
    const bool want = orig_ReadBoxes && g_old_want.load(std::memory_order_relaxed) && !(bv > 0 && bv <= kOldBoxesVersion);
    if (!want) { g_old_live = false; return; }
    if ((!g_old_base[0] || !g_old_base[1]) && GetTickCount64() - g_old_tried >= 250) {
      g_old_tried = GetTickCount64();
      for (int i = 0; i < 2; i++) {
        if (g_old_base[i] || !ents[i]) continue;
        const std::string chr = sideChr(ents[i]);
        if (chr.size() != 3 || g_old_tries[i] >= 40) continue;
        const Col* col = colOf(ents[i]);
        if (!col) { g_old_tries[i]++; continue; }
        const ULONGLONG t0 = GetTickCount64();
        oldHurt(chr);
        int n = 0;
        {
          std::unique_lock<std::shared_mutex> lk(g_old_m);
          n = addOldBoxes(chr, *col);
        }
        g_old_base[i] = col->base;
        COMBO_LOG("old hurtboxes: " + chr + " (side " + std::to_string(i + 1) + "): " + std::to_string(n) + " hit and block sprites on 4.09 hurtboxes (" + std::to_string(GetTickCount64() - t0) + " ms)");
      }
    }
    g_old_live = !g_old_map.empty();
  }
  const uint8_t* stateStart(const States& st, const std::string& name) {
    for (size_t k = 0; k < st.names.size(); k++) if (st.names[k] == name) return st.starts[k];
    for (size_t k = 0; k < st.routines.size(); k++) if (st.routines[k] == name) return st.rstarts[k];
    return nullptr;
  }
  bool isObj(const std::string& n) { return n.rfind(kObjectPrefix, 0) == 0; }
  bool isShared(const std::string& n) { return n.rfind(kSharedPrefix, 0) == 0; }
  std::string sharedName(const std::string& n) { return isShared(n) ? n.substr(sizeof(kSharedPrefix) - 1) : n; }
  const uint8_t* sharedStart(const std::string& name);
  std::string objName(const std::string& n) { return isObj(n) ? n.substr(sizeof(kObjectPrefix) - 1) : n; }
  bool isCmnObj(const std::string& n) { return n.rfind(kCommonObjPrefix, 0) == 0; }
  struct URef { std::string chr, state; };
  URef unitOf(const std::string& chr, const std::string& key) {
    return isCmnObj(key) ? URef{"CMN", std::string(kObjectPrefix) + key.substr(sizeof(kCommonObjPrefix) - 1)} : URef{chr, key};
  }
  struct EfFiles { std::vector<States> files; std::vector<const uint8_t*> seen; std::vector<const void*> holders, table_holders; int tries = 0; ULONGLONG last = 0; std::string ver; bool ver_known = false, ver_tried = false; };
  bool inFightChr(const std::string& chr, asw_entity* const ents[2]);
  std::map<std::string, EfFiles> g_ef;
  bool g_ef_added = false;
  bool quickScript(const uint8_t* p) {
    if (!p || !readable(p, 4)) return false;
    const uint32_t count = u32(p);
    if (count == 0 || count > 8000 || !readable(p, 4 + static_cast<size_t>(count) * 36)) return false;
    if (p[4] < 0x21 || p[4] > 0x7E) return false;
    const uint8_t* s = p + 4 + static_cast<size_t>(count) * 36 + u32(p + 4 + 32);
    return readable(s, 36) && i32(s) == 0;
  }
  bool scriptFileAt(const uint8_t* buf) {
    if (!quickScript(buf)) return false;
    const uint32_t count = u32(buf);
    const uint8_t* code = buf + 4 + static_cast<size_t>(count) * 36;
    uint32_t named = 0;
    for (uint32_t k = 0; k < count; k++) {
      const uint8_t* nm = buf + 4 + static_cast<size_t>(k) * 36;
      if (!nm[0]) return false;
      for (int j = 0; j < 32 && nm[j]; j++) if (nm[j] < 0x20 || nm[j] > 0x7E) return false;
      const uint8_t* s = code + u32(nm + 32);
      if (!readable(s, 36) || i32(s) != 0) return false;
      if (cstr(s + 4, 32) == cstr(nm, 32)) named++;
    }
    return named * 2 >= count;
  }
  std::map<std::string, std::vector<uint8_t>> g_ef_table;
  std::string efName(const std::string& chr, const std::string& ver) { return "BBS_" + chr + "EF" + (ver.empty() ? std::string() : "_" + ver); }
  bool verOf(const std::string& name, const std::string& base, std::string& ver) {
    if (name == base) { ver.clear(); return true; }
    if (name.size() != base.size() + 4 || name.compare(0, base.size(), base) != 0 || name[base.size()] != '_') return false;
    for (size_t k = base.size() + 1; k < name.size(); k++) if (name[k] < '0' || name[k] > '9') return false;
    ver = name.substr(base.size() + 1);
    return true;
  }
  ptrdiff_t efVersion(const std::string& chr, asw_entity* const ents[2], const std::vector<RC::Unreal::UObject*>& objs, EfFiles& ef) {
    const std::string own = "BBS_" + chr;
    std::string v;
    ef.ver_tried = true;
    const uint8_t* files[2] = {};
    for (int i = 0; i < 2; i++) if (ents[i] && sideChr(ents[i]) == chr) files[i] = fileOf(ents[i]);
    if (!files[0] && !files[1]) return -1;
    for (RC::Unreal::UObject* o : objs) {
      if (!o || !verOf(narrowName(o->GetName()), own, v)) continue;
      const uint8_t* b = reinterpret_cast<const uint8_t*>(o);
      if (!readable(b + 0x28, 0xA0 - 0x28)) continue;
      for (size_t off = 0x28; off + 8 <= 0xA0; off += 8) {
        const uint8_t* p = *reinterpret_cast<const uint8_t* const*>(b + off);
        if (!p || (p != files[0] && p != files[1])) continue;
        ef.ver = v;
        ef.ver_known = true;
        return static_cast<ptrdiff_t>(off);
      }
    }
    return -1;
  }
  std::string scriptObjectNames(const std::vector<RC::Unreal::UObject*>& objs) {
    std::string list;
    for (RC::Unreal::UObject* o : objs) {
      if (!o || list.size() > 400) continue;
      list += (list.empty() ? "" : ", ") + narrowName(o->GetName());
    }
    return list;
  }
  const std::vector<uint8_t>& efTable(const std::string& chr, const std::string& ver) {
    const std::string name = efName(chr, ver);
    auto it = g_ef_table.find(name);
    if (it != g_ef_table.end()) return it->second;
    std::vector<uint8_t> b, t;
    const std::wstring w(chr.begin(), chr.end()), wn(name.begin(), name.end()), wv(ver.begin(), ver.end());
    if (pak_mount::readFile(L"../../../RED/Content/Chara/" + w + L"/Common/Data/" + (wv.empty() ? std::wstring() : wv + L"/") + wn + L".uexp", b) && b.size() > 0x38) {
      const uint32_t count = u32(b.data() + 0x34);
      const size_t n = 4 + static_cast<size_t>(count) * 36;
      if (count > 0 && count <= 8000 && 0x34 + n <= b.size()) t.assign(b.begin() + 0x34, b.begin() + 0x34 + static_cast<ptrdiff_t>(n));
    }
    COMBO_LOG("script & boxes: " + name + "'s move table " + (t.empty() ? std::string("couldn't be read from the game's files") : "read (" + std::to_string(t.size()) + " bytes)"));
    return g_ef_table[name] = std::move(t);
  }
  bool addEf(const std::string& chr, const uint8_t* buf, asw_entity* const ents[2], const std::string& how, bool named) {
    if (!buf || chr.empty()) return false;
    EfFiles& ef = g_ef[chr];
    if (std::find(ef.seen.begin(), ef.seen.end(), buf) != ef.seen.end()) return false;
    if (ef.seen.size() < 4096) ef.seen.push_back(buf);
    for (int i = 0; i < 2; i++) if (fileOf(ents[i]) == buf) return false;
    if (!scriptFileAt(buf)) return false;
    if (!ef.ver_known && !ef.ver_tried) {
      std::vector<RC::Unreal::UObject*> objs;
      RC::Unreal::UObjectGlobals::FindAllOf(STR("REDBBSData"), objs);
      efVersion(chr, ents, objs, ef);
    }
    const std::vector<uint8_t>& table = efTable(chr, ef.ver);
    if (table.empty() ? !named : !(readable(buf, table.size()) && std::memcmp(buf, table.data(), table.size()) == 0)) return false;
    States st;
    readStatesAt(buf, st);
    if (st.names.empty()) return false;
    char at[32];
    snprintf(at, sizeof(at), "%p", static_cast<const void*>(buf));
    COMBO_LOG("script & boxes: " + chr + "'s object script file at " + at + " (" + std::to_string(st.names.size()) + " states, " + std::to_string(st.routines.size()) + " routines), from " + how +
              (ef.ver.empty() ? std::string() : ", battle version file " + ef.ver));
    ef.files.push_back(std::move(st));
    g_ef_added = true;
    return true;
  }
  void findEf(const std::string& chr, asw_entity* const ents[2]) {
    EfFiles& ef = g_ef[chr];
    ef.tries++;
    ef.last = GetTickCount64();
    std::vector<RC::Unreal::UObject*> objs;
    RC::Unreal::UObjectGlobals::FindAllOf(STR("REDBBSData"), objs);
    const ptrdiff_t holder = efVersion(chr, ents, objs, ef);
    const std::string efn = efName(chr, ef.ver);
    int named = 0;
    const size_t before = ef.files.size();
    for (RC::Unreal::UObject* o : objs) {
      if (!o || narrowName(o->GetName()) != efn) continue;
      named++;
      const uint8_t* b = reinterpret_cast<const uint8_t*>(o);
      if (holder >= 0 && readable(b + holder, 8)) {
        char how[48];
        snprintf(how, sizeof(how), "its object (+0x%X, as the fighter's)", static_cast<unsigned>(holder));
        if (addEf(chr, *reinterpret_cast<const uint8_t* const*>(b + holder), ents, how, true)) continue;
      }
      for (size_t off = 0x28; off + 8 <= 0x400; off += 8) {
        if (!readable(b + off, 8)) break;
        const uint8_t* p = *reinterpret_cast<const uint8_t* const*>(b + off);
        char how[48];
        snprintf(how, sizeof(how), "its object (+0x%X)", static_cast<unsigned>(off));
        if (quickScript(p)) { addEf(chr, p, ents, how, false); continue; }
        if (p && readable(p, 8)) {
          const uint8_t* q = *reinterpret_cast<const uint8_t* const*>(p);
          if (quickScript(q)) addEf(chr, q, ents, std::string(how) + " through a pointer", false);
        }
      }
    }
    for (RC::Unreal::UObject* o : objs) {
      if (!o || ef.files.empty() || narrowName(o->GetName()) != efn) continue;
      const uint8_t* b = reinterpret_cast<const uint8_t*>(o);
      for (size_t off = 0x28; off + 8 <= 0x400 && readable(b + off, 8); off += 8) {
        const uint8_t* p = *reinterpret_cast<const uint8_t* const*>(b + off);
        for (const States& f : ef.files)
          if (p && p == f.buf && std::find(ef.table_holders.begin(), ef.table_holders.end(), b + off) == ef.table_holders.end() && ef.table_holders.size() < 16) ef.table_holders.push_back(b + off);
      }
    }
    if (ef.files.size() == before && (ef.tries <= 3 || ef.tries % 10 == 0)) {
      char h[48];
      if (holder >= 0) snprintf(h, sizeof(h), "at +0x%X of its object", static_cast<unsigned>(holder));
      else snprintf(h, sizeof(h), "not in its object");
      COMBO_LOG("script & boxes: " + chr + "'s object script file not found (try " + std::to_string(ef.tries) + ": " + std::to_string(objs.size()) + " script objects, " + std::to_string(named) + " called " + efn +
                ", the fighter's file " + h + ") [" + scriptObjectNames(objs) + "]");
    }
  }
  void wantEf(const std::string& chr, asw_entity* const ents[2]) {
    if (chr.empty() || chr == "CMN" || !inFightChr(chr, ents)) return;
    EfFiles& ef = g_ef[chr];
    if (!ef.files.empty() || (ef.tries && GetTickCount64() - ef.last < (ef.tries < 10 ? 1500u : 10000u))) return;
    findEf(chr, ents);
  }
  const uint8_t* efStart(const std::string& chr, const std::string& name) {
    auto it = g_ef.find(chr);
    if (it == g_ef.end()) return nullptr;
    for (const States& f : it->second.files) if (const uint8_t* p = stateStart(f, name)) return p;
    return nullptr;
  }
  void efStartsAgain() {
    for (auto& [c, ef] : g_ef)
      for (States& f : ef.files) {
        if (!f.buf || !f.code || f.starts.size() != f.names.size() || !readable(f.buf, 4 + f.names.size() * 36) || u32(f.buf) != f.names.size()) continue;
        for (size_t k = 0; k < f.names.size(); k++) f.starts[k] = f.code + u32(f.buf + 4 + k * 36 + 32);
      }
  }
  const uint8_t* moveStart(int side, const std::string& chr, const std::string& name);
  std::vector<const uint8_t*> copyStarts(const std::string& chr, const std::string& name);
  bool copyOff(const std::string& chr, const std::string& name);
  // An edit onto every loaded copy of the character's script (both sides' in a mirror: one file, or two), only where
  // the line is still the one edited and holds the game's value or ours
  bool inFight(const std::string& chr, asw_entity* const ents[2]) {
    if (chr == "CMN") return ents[0] || ents[1];
    for (int i = 0; i < 2; i++) if (ents[i] && sideChr(ents[i]) == chr) return true;
    return false;
  }
  bool inFightChr(const std::string& chr, asw_entity* const ents[2]) { return inFight(chr, ents); }
  bool swapHasCode(const std::string& chr, const std::string& state);
  bool swapCopyAt(const std::string& chr, const std::string& state, const States& f, const uint8_t* s);
  void applySEdit(const SKey& k, SEdit& ed, asw_entity* const ents[2]) {
    std::vector<uint8_t*> lines;
    const std::vector<const uint8_t*> copies = isShared(k.state) ? std::vector<const uint8_t*>() : copyStarts(k.chr, k.state);
    const bool swapped = swapHasCode(k.chr, k.state);
    const auto layout = [&](const States& f, const uint8_t* s) { return swapCopyAt(k.chr, k.state, f, s) == k.copy; };
    if (!copies.empty()) {
      if (inFight(k.chr, ents)) for (const uint8_t* s : copies) lines.push_back(const_cast<uint8_t*>(s) + k.offset);
    } else if (copyOff(k.chr, k.state) && (!swapped || k.copy)) {
    } else if (isObj(k.state)) {
      auto it = inFight(k.chr, ents) ? g_ef.find(k.chr) : g_ef.end();
      if (it != g_ef.end())
        for (const States& f : it->second.files)
          if (const uint8_t* s = stateStart(f, objName(k.state)); s && layout(f, s)) lines.push_back(const_cast<uint8_t*>(s) + k.offset);
    } else
      for (int i = 0; i < 2; i++) {
        if (!ents[i] || sideChr(ents[i]) != k.chr) continue;
        if (const uint8_t* s = stateStart(g_states[i], k.state); s && layout(g_states[i], s)) lines.push_back(const_cast<uint8_t*>(s) + k.offset);
      }
    for (uint8_t* line : lines) {
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
  bool routineFormed(const std::vector<uint8_t>& c, std::string& err) { return ops::formed(ctx(), c, err); }
  bool codeWalks(const std::vector<uint8_t>& c) { return ops::walks(ctx(), c); }
  bool assemble(const std::string& text, const std::string& want, bool routine, std::vector<uint8_t>& out, std::string& err, std::vector<int>* lines = nullptr) {
    return ops::assemble(ctx(), text, want, routine, out, err, lines);
  }
  std::string shapeNote(const std::vector<uint8_t>& before, const std::vector<uint8_t>& after, const std::vector<int>* lines = nullptr) {
    const std::string w = ops::warning(ctx(), before, after, lines);
    return w.empty() ? std::string() : " " + w;
  }
  std::vector<std::string> moveFileNames(const std::filesystem::path& f) {
    std::vector<std::string> out;
    std::ifstream in(f);
    std::string l;
    static const std::pair<const char*, const char*> kKeys[] = {{"beginState", ""}, {"beginSubroutine", ""}, {"preSubroutineBegin", " (pre)"}, {"postSubroutineBegin", " (post)"}};
    while (std::getline(in, l)) {
      l = trim(l);
      const size_t colon = l.find(':');
      if (colon == std::string::npos) continue;
      const std::string head = trim(l.substr(0, colon));
      const char* suffix = nullptr;
      for (const auto& [k, suf] : kKeys) if (head == k) suffix = suf;
      if (!suffix) continue;
      const size_t a = l.find("s32'", colon);
      const size_t b = a == std::string::npos ? a : l.find('\'', a + 4);
      if (b == std::string::npos || b == a + 4) continue;
      const std::string n = l.substr(a + 4, b - a - 4) + suffix;
      if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
    }
    return out;
  }
  std::string g_smsg;  // (a script request's result, shown on the window's status line)
  bool g_sclear = false;
  // ---- other battle versions of a move ----
  struct VerFile { std::string folder; std::vector<uint8_t> payload; };  // a version's script file (from its states table on)
  std::map<std::string, std::vector<VerFile>> g_ver_files;  // by character, read once per session when a move's versions are asked for
  std::string verLabel(const std::string& f) { return f.size() == 3 ? f.substr(0, 1) + "." + f.substr(1) : f; }  // "306" -> "3.06"
  const std::vector<VerFile>& verFiles(const std::string& chr, const std::string& base) {
    auto it = g_ver_files.find(base);
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
      if (!pak_mount::readFile(dir + L"/" + wn + L"/" + std::wstring(base.begin(), base.end()) + L"_" + wn + L".uexp", b) || b.size() < 0x40) continue;
      out.push_back({n, std::vector<uint8_t>(b.begin() + 0x34, b.end())});  // (the script starts 0x34 into the cooked file)
    }
    COMBO_LOG("script & boxes: " + base + ": " + std::to_string(out.size()) + " battle versions read (" + std::to_string(GetTickCount64() - t0) + " ms)");
    return g_ver_files[base] = std::move(out);
  }
  const std::vector<VerFile>& versionFiles(const std::string& chr) { return verFiles(chr, "BBS_" + chr); }
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
      while (p + 4 <= f.size() && p - from < kMaxCode) {
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
    std::string no;
    std::vector<char> added;
    int ver = -1;
    bool held = false;
    bool near_wait = false;
    std::vector<uint8_t> view;
  };
  std::map<std::pair<std::string, std::string>, Swap> g_swaps;  // (chr, state)
  bool swapHasCode(const std::string& chr, const std::string& state) {
    const auto it = g_swaps.find({chr, state});
    return it != g_swaps.end() && !it->second.code.empty();
  }
  bool swapCopyAt(const std::string& chr, const std::string& state, const States& f, const uint8_t* s) {
    const auto it = g_swaps.find({chr, state});
    if (it == g_swaps.end() || it->second.code.empty() || !f.buf || !f.code || !s) return false;
    for (const auto& a : it->second.at) {
      const uint8_t* e = reinterpret_cast<const uint8_t*>(a.entry);
      if (a.ptr == 0 && e > f.buf && e < f.code && f.code + a.ours == s) return true;
    }
    return false;
  }
  bool startOnCopy(int side, const std::string& chr, const std::string& state, const uint8_t* start) {
    if (const auto w = g_swaps.find({chr, state}); start && w != g_swaps.end() && !w->second.view.empty() && w->second.view.data() == start) return true;
    if (isObj(state)) {
      const auto it = g_ef.find(chr);
      if (it != g_ef.end())
        for (const States& f : it->second.files) if (swapCopyAt(chr, state, f, start)) return true;
      return false;
    }
    return side >= 0 && side < 2 && swapCopyAt(chr, state, g_states[side], start);
  }
  int copyVer(const std::string& chr);
  std::string verWords(int v);
  bool copyHeld(const std::string& chr, const std::string& name, Swap& sw);
  std::string g_swap_no;
  std::map<const uint8_t*, std::vector<std::pair<uint8_t*, size_t>>> g_swap_mem;  // our blocks, by the script they're near (kept: a fighter may still run one)
  struct NearArena { uint8_t* p; size_t size, used, committed; bool kept = false; };
  struct NearFound { uint8_t* p = nullptr; size_t size = 0; uintptr_t at = 0; bool failed = false; double ms = 0; };
  struct NearSearch { const uint8_t* code = nullptr; uintptr_t at = 0; size_t want = 0; bool going = false, failed = false, no_commit = false, said = false, none = false; double spent = 0, waited = 0; int steps = 0; std::future<NearFound> job; };
  std::vector<NearArena> g_near_arenas;
  std::map<const uint8_t*, NearSearch> g_near_search;
  std::vector<std::future<NearFound>> g_near_orphans;
  bool g_no_room = false;
  constexpr uintptr_t kNearGran = 0x10000;
  constexpr size_t kNearArena = 0x100000;
  constexpr uintptr_t kNearFrom = 0x1000000, kNearReach = 0x70000000, kNearFar = 0xF0000000;
  constexpr uintptr_t kKeptLowStep = 0x10000000, kKeptLow = 0x100000000, kKeptStep = 0x40000000, kKeptLast = 0x800000000;
  constexpr size_t kKeptLowSize = 0x1000000, kKeptSize = 0x4000000, kKeptLeast = 0x100000;
  const char* const kNoRoomNote = "No memory near the script in this fight: loaded moves are the game's own. Reload the fight.";
  const char* const kEfWait = "This fighter's object script hasn't been found in memory yet. It is looked for every few seconds, and putting one of its objects out (a projectile...) finds it at once; then this goes on by itself.";
  bool keepNearIn(uintptr_t from, uintptr_t to, size_t most) {
    uintptr_t best = 0, at = from;
    size_t best_size = 0;
    for (int n = 0; n < 8192 && at < to && best_size < most; n++) {
      MEMORY_BASIC_INFORMATION mbi{};
      if (!VirtualQuery(reinterpret_cast<void*>(at), &mbi, sizeof(mbi))) break;
      const uintptr_t re = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
      if (mbi.State == MEM_FREE) {
        const uintptr_t end = (std::min)(re, to);
        const size_t room = end > at ? (end - at) / kNearGran * kNearGran : 0;
        if (room > best_size) { best = at; best_size = (std::min)(room, most); }
      }
      at = re > at ? (re + kNearGran - 1) / kNearGran * kNearGran : at + kNearGran;
    }
    if (best_size < kKeptLeast) return false;
    void* p = VirtualAlloc(reinterpret_cast<void*>(best), best_size, MEM_RESERVE, PAGE_READWRITE);
    if (!p) return false;
    g_near_arenas.push_back({static_cast<uint8_t*>(p), best_size, 0, 0, true});
    return true;
  }
  void keepNearRoom() {
    int low = 0, high = 0;
    for (uintptr_t w = kKeptLowStep; w < kKeptLow; w += kKeptLowStep) low += keepNearIn(w, w + kKeptLowStep, kKeptLowSize);
    for (uintptr_t slot = kKeptLow; slot <= kKeptLast; slot += kKeptStep) high += keepNearIn(slot, slot + kKeptStep / 2, kKeptSize);
    size_t total = 0;
    for (const NearArena& a : g_near_arenas) total += a.size;
    uintptr_t bare = 0, bare_to = 0;
    for (uintptr_t x = 0; x < kKeptLast - 2 * kKeptStep; x += kNearFrom) {
      bool ok = false;
      for (const NearArena& a : g_near_arenas) {
        const uintptr_t p = reinterpret_cast<uintptr_t>(a.p);
        if (p >= x + kNearFrom && p + a.size <= x + kNearFar) { ok = true; break; }
      }
      if (!ok) { bare += kNearFrom; bare_to = x + kNearFrom; }
    }
    COMBO_LOG("script & boxes: room kept for loaded moves: " + std::to_string(low) + " blocks below 4 GB, " + std::to_string(high) + " above, " + std::to_string(total >> 20) +
              " MB of address space; scripts with none in reach: " +
              (bare ? std::to_string(bare >> 20) + " MB of the first 30 GB, the last ending at " + std::to_string(bare_to >> 20) + " MB" : std::string("none in the first 30 GB")));
  }
  double nearMs() {
    static const double per = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return 1000.0 / static_cast<double>(f.QuadPart); }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * per;
  }
  bool stepNear(NearSearch& s, double budget) {
    const double t0 = nearMs();
    const uintptr_t base = reinterpret_cast<uintptr_t>(s.code), end = base + kNearReach;
    if (!s.at) s.at = (base + kNearFrom + kNearGran - 1) / kNearGran * kNearGran;
    bool done = false;
    for (int n = 1; !done; n++) {
      MEMORY_BASIC_INFORMATION mbi{};
      if (s.at + s.want >= end || !VirtualQuery(reinterpret_cast<void*>(s.at), &mbi, sizeof(mbi))) { s.failed = true; done = true; break; }
      const uintptr_t re = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
      if (mbi.State == MEM_FREE && re >= s.at + s.want) {
        const size_t room = (std::min<uintptr_t>)(re, end) - s.at;
        size_t take = (std::max)(s.want, (std::min)(kNearArena, room / kNearGran * kNearGran));
        void* p = VirtualAlloc(reinterpret_cast<void*>(s.at), take, MEM_RESERVE, PAGE_READWRITE);
        if (!p && take != s.want) { take = s.want; p = VirtualAlloc(reinterpret_cast<void*>(s.at), take, MEM_RESERVE, PAGE_READWRITE); }
        if (p) {
          g_near_arenas.push_back({static_cast<uint8_t*>(p), take, 0, 0});
          s.at += take;
          done = true;
          break;
        }
        s.at += kNearGran;
      } else s.at = re > s.at ? (re + kNearGran - 1) / kNearGran * kNearGran : s.at + kNearGran;
      if ((n & 15) == 0 && nearMs() - t0 > budget) break;
    }
    s.spent += nearMs() - t0;
    s.steps++;
    if (!done) return false;
    s.going = false;
    char line[200];
    if (s.failed) snprintf(line, sizeof line, "script & boxes: no free memory within %llu MB above the script (%.1f ms over %d ticks)", static_cast<unsigned long long>(kNearReach >> 20), s.spent, s.steps);
    else snprintf(line, sizeof line, "script & boxes: room for loaded moves %llu MB above the script (%.1f ms over %d ticks)", static_cast<unsigned long long>((reinterpret_cast<uintptr_t>(g_near_arenas.back().p) - base) >> 20), s.spent, s.steps);
    COMBO_LOG(std::string(line));
    return true;
  }
  NearFound nearWorker(uintptr_t base, uintptr_t at, size_t want) {
    NearFound r;
    const double t0 = nearMs();
    const uintptr_t end = base + kNearReach;
    for (;;) {
      MEMORY_BASIC_INFORMATION mbi{};
      if (at + want >= end || !VirtualQuery(reinterpret_cast<void*>(at), &mbi, sizeof(mbi))) { r.failed = true; break; }
      const uintptr_t re = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
      if (mbi.State == MEM_FREE && re >= at + want) {
        const size_t room = (std::min<uintptr_t>)(re, end) - at;
        size_t take = (std::max)(want, (std::min)(kNearArena, room / kNearGran * kNearGran));
        void* p = VirtualAlloc(reinterpret_cast<void*>(at), take, MEM_RESERVE, PAGE_READWRITE);
        if (!p && take != want) { take = want; p = VirtualAlloc(reinterpret_cast<void*>(at), take, MEM_RESERVE, PAGE_READWRITE); }
        if (p) {
          r.p = static_cast<uint8_t*>(p);
          r.size = take;
          at += take;
          break;
        }
        at += kNearGran;
      } else at = re > at ? (re + kNearGran - 1) / kNearGran * kNearGran : at + kNearGran;
    }
    r.at = at;
    r.ms = nearMs() - t0;
    return r;
  }
  void startNear(NearSearch& s) {
    if (!s.at) s.at = (reinterpret_cast<uintptr_t>(s.code) + kNearFrom + kNearGran - 1) / kNearGran * kNearGran;
    try {
      s.job = std::async(std::launch::async, nearWorker, reinterpret_cast<uintptr_t>(s.code), s.at, s.want);
    } catch (...) {
      s.job = std::future<NearFound>();
      COMBO_LOG("script & boxes: the search for memory near the script couldn't start in the background: it runs on the game thread");
    }
  }
  void keepFound(const NearFound& f) {
    if (f.p) g_near_arenas.push_back({f.p, f.size, 0, 0});
  }
  uint8_t* takeNear(const uint8_t* buf, const uint8_t* code, size_t size, bool now) {
    size = (size + 15) & ~static_cast<size_t>(15);
    const size_t want = (size + kNearGran - 1) / kNearGran * kNearGran;
    const uint8_t* const lo = code + kNearFrom;
    for (int pass = 0; pass < 3; pass++) {
      NearSearch& s = g_near_search[buf];
      if (s.no_commit) break;
      const uint8_t* const hi = code + (s.failed ? kNearFar : kNearReach);
      for (NearArena& a : g_near_arenas) {
        if (a.p < lo || a.p + a.size > hi || a.size - a.used < size) continue;
        const size_t need = (std::min)(a.size, (a.used + size + kNearGran - 1) / kNearGran * kNearGran);
        if (need > a.committed) {
          if (!VirtualAlloc(a.p + a.committed, need - a.committed, MEM_COMMIT, PAGE_READWRITE)) {
            s.no_commit = true;
            g_no_room = true;
            COMBO_LOG("script & boxes: the system gave no memory for loaded moves: they stay the game's own until the fight is reloaded");
            return nullptr;
          }
          a.committed = need;
        }
        uint8_t* p = a.p + a.used;
        a.used += size;
        if ((a.kept || s.failed) && !s.said) {
          s.said = true;
          COMBO_LOG(std::string("script & boxes: loaded moves go in the room ") + (a.kept ? "kept at start, " : "found earlier, ") + std::to_string(static_cast<unsigned long long>(a.p - code) >> 20) + " MB above the script");
        }
        if (s.none) {
          s.none = false;
          g_no_room = false;
          for (const auto& [other, o] : g_near_search) if (o.none || o.no_commit) g_no_room = true;
        }
        return p;
      }
      if (s.failed && !s.none) {
        s.none = true;
        g_no_room = true;
        COMBO_LOG("script & boxes: no room within " + std::to_string(static_cast<unsigned long long>(kNearFar >> 20)) + " MB above the script either: loaded moves stay the game's own until the fight is reloaded");
      }
      if (pass == 2 || s.failed) break;
      const bool start = !s.going;
      if (start) {
        const uintptr_t at = s.at;
        if (s.job.valid()) g_near_orphans.push_back(std::move(s.job));
        s = NearSearch{};
        s.at = at;
        s.code = code;
        s.want = want;
        s.going = true;
        startNear(s);
      } else if (want > s.want && !s.job.valid()) s.want = want;
      if (s.job.valid()) break;
      if (!start && !now) break;
      const double budget = now ? 1500.0 - s.waited : 3.0;
      if (budget <= 0) break;
      const double before = s.spent;
      const bool ended = stepNear(s, budget);
      if (now) s.waited += s.spent - before;
      if (!ended) break;
    }
    return nullptr;
  }
  bool stepNearSearches() {
    for (auto it = g_near_orphans.begin(); it != g_near_orphans.end();) {
      if (it->wait_for(std::chrono::seconds(0)) != std::future_status::ready) { ++it; continue; }
      try { keepFound(it->get()); } catch (...) {}
      it = g_near_orphans.erase(it);
    }
    bool ended = false;
    double left = 3.0;
    for (auto& [buf, s] : g_near_search) {
      if (s.going && s.job.valid()) {
        if (s.job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
        NearFound f;
        try { f = s.job.get(); } catch (...) { f = NearFound{}; f.failed = true; }
        if (f.at) s.at = f.at;
        s.spent += f.ms;
        s.steps++;
        keepFound(f);
        if (!f.p) s.failed = true;
        s.going = false;
        const uintptr_t base = reinterpret_cast<uintptr_t>(s.code);
        char line[200];
        if (s.failed) snprintf(line, sizeof line, "script & boxes: no free memory within %llu MB above the script (%.1f ms in the background)", static_cast<unsigned long long>(kNearReach >> 20), s.spent);
        else snprintf(line, sizeof line, "script & boxes: room for loaded moves %llu MB above the script (%.1f ms in the background)", static_cast<unsigned long long>((reinterpret_cast<uintptr_t>(f.p) - base) >> 20), s.spent);
        COMBO_LOG(std::string(line));
        ended = true;
        continue;
      }
      if (!s.going || left <= 0) continue;
      const double before = s.spent;
      if (stepNear(s, left)) ended = true;
      left -= s.spent - before;
    }
    return ended;
  }
  bool nearGoing() {
    for (const auto& [buf, s] : g_near_search)
      if (s.going) return true;
    return false;
  }
  const uint8_t* waitingCopy(const std::string& chr, const std::string& state) {
    if (isShared(state) || !nearGoing()) return nullptr;
    const auto it = g_swaps.find({chr, state});
    if (it == g_swaps.end()) return nullptr;
    Swap& sw = it->second;
    if (!sw.near_wait || sw.held || !sw.at.empty() || sw.code.size() < 40) return nullptr;
    if (sw.view.size() != sw.code.size()) sw.view = sw.code;
    else std::memcpy(sw.view.data(), sw.code.data(), sw.code.size());
    for (const auto& [k, ed] : g_sedits) {
      if (k.chr != chr || k.state != state || !k.copy || k.offset < 0 || k.field != ed.field || ed.field < 0 || ed.orig.empty() || ed.orig.size() != ed.now.size()) continue;
      const size_t line = static_cast<size_t>(k.offset), at = line + static_cast<size_t>(k.field);
      if (line + 4 > sw.view.size() || at + ed.now.size() > sw.view.size() || line + 4 + ed.sig.size() > sw.view.size() || line + 4 + ed.sig_alt.size() > sw.view.size()) continue;
      uint8_t* p = sw.view.data() + at;
      if (!sameLine(p, ed)) continue;
      if (std::memcmp(p, ed.orig.data(), ed.orig.size()) != 0 && std::memcmp(p, ed.now.data(), ed.now.size()) != 0) continue;
      std::memcpy(p, ed.now.data(), ed.now.size());
    }
    return sw.view.data();
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
    unsigned long long taken = 0, left = 0;
    const double ws0 = perf::workingSetMb();
    try {
      std::vector<uint8_t> buf(1 << 20);
      std::vector<PSAPI_WORKING_SET_EX_INFORMATION> ws(buf.size() / 4096);
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
      const size_t step = buf.size() - (std::min)((static_cast<size_t>(job.count) * 24 + 16 + 4095) / 4096 * 4096, buf.size() / 2);
      MEMORY_BASIC_INFORMATION mbi{};
      for (uintptr_t a = 0x10000; a < 0x7FFFFFFF0000ull && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi)); a = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || (mbi.Protect & (PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE)) || !(mbi.Protect & PAGE_READWRITE)) continue;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = base + mbi.RegionSize;
        for (uintptr_t at = base; at < end; at = at + buf.size() >= end ? end : at + step) {
          const size_t n = static_cast<size_t>((std::min)(static_cast<uintptr_t>(buf.size()), end - at));
          if (own(at, n)) continue;
          const size_t pages = (n + 4095) / 4096;
          for (size_t i = 0; i < pages; i++) { ws[i].VirtualAddress = reinterpret_cast<void*>(at + i * 4096); ws[i].VirtualAttributes.Flags = 0; }
          if (!QueryWorkingSetEx(GetCurrentProcess(), ws.data(), static_cast<DWORD>(pages * sizeof(ws[0])))) { left += n; continue; }
          SIZE_T got = 0;
          for (size_t i = 0; i < pages;) {
            size_t j = i;
            const bool in_ram = ws[i].VirtualAttributes.Valid != 0;
            while (j < pages && (ws[j].VirtualAttributes.Valid != 0) == in_ram) j++;
            const size_t from = i * 4096, len = (std::min)(j * 4096, n) - from;
            SIZE_T g = 0;
            if (in_ram && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(at + from), buf.data() + from, len, &g) && g == len) { got = n; taken += len; }
            else { std::memset(buf.data() + from, 0, len); if (!in_ram) left += len; }
            i = j;
          }
          if (!got) continue;
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
              const uintptr_t from = at + hits[h].first;
              const size_t size = last + 16 - hits[h].first;
              bool again = false;
              for (TableCopy& f : found)
                if (f.ptr == ptr && from < f.base + f.size && f.base < from + size) {
                  const uintptr_t to = (std::max)(f.base + f.size, from + size);
                  f.base = (std::min)(f.base, from);
                  f.size = to - f.base;
                  again = true;
                }
              if (!again) {
                found.push_back({from, size, ptr});  // (to the last entry's pointer)
                char line[120];
                snprintf(line, sizeof line, " [%p, %llu bytes: %d starts]", reinterpret_cast<void*>(from), static_cast<unsigned long long>(size), distinct);
                list += line;
              }
              h = e + 1;
            } else h++;
          }
        }
      }
    } catch (...) { list += " (search failed)"; }
    COMBO_LOG("script & boxes: " + job.what + ": the game's copies of the move list (" + std::to_string(GetTickCount64() - t0) + " ms, in the background; " + std::to_string(taken >> 20) + " MB read, " +
              std::to_string(left >> 20) + " MB not in RAM left alone, working set " + std::to_string(static_cast<long long>(ws0)) + " -> " +
              std::to_string(static_cast<long long>(perf::workingSetMb())) + " MB):" + (list.empty() ? std::string(" none") : list));
    return found;
  }
  // How many of the table's starts (offset >= 64) a copy holds in its entry shape, each counted once
  int startsIn(const uint8_t* table, const uint8_t* p, size_t n, uint64_t ptr) {
    const uint32_t count = *reinterpret_cast<const uint32_t*>(table);
    std::map<const uint32_t*, uint32_t> was;
    for (const auto& [key, sw] : g_swaps)
      for (const auto& a : sw.at) if (a.ptr == 0) was[a.entry] = a.orig;
    std::vector<uint32_t> offs;
    std::map<uint32_t, uint32_t> ours;
    for (uint32_t k = 0; k < count; k++) {
      const uint32_t* e = reinterpret_cast<const uint32_t*>(table + 4 + k * 36 + 32);
      auto w = was.find(e);
      const uint32_t o = w != was.end() ? w->second : *e;
      if (o < 64) continue;
      offs.push_back(o);
      if (*e != o) ours[*e] = o;
    }
    std::sort(offs.begin(), offs.end());
    offs.erase(std::unique(offs.begin(), offs.end()), offs.end());
    std::vector<char> seen(offs.size());
    int no = 0;
    for (size_t q = 0; q + 16 <= n; q += 24) {
      uint32_t v = *reinterpret_cast<const uint32_t*>(p + q);
      if (*reinterpret_cast<const uint64_t*>(p + q + 8) != ptr) continue;
      auto mine = ours.find(v);
      if (mine != ours.end()) v = mine->second;
      auto it = std::lower_bound(offs.begin(), offs.end(), v);
      if (it != offs.end() && *it == v && !seen[it - offs.begin()]) { seen[it - offs.begin()] = 1; no++; }
    }
    return static_cast<int>(offs.empty() ? 0 : no * static_cast<int64_t>(count) / static_cast<int64_t>(offs.size()));
  }
  void startScan(const States& st, const uint8_t* other, const std::string& chr) {  // (game thread: what the search needs, copied)
    if (g_table_copies.count(st.buf) || g_table_scans.count(st.buf)) return;
    ScanJob job;
    job.count = *reinterpret_cast<const uint32_t*>(st.buf);
    std::map<const uint32_t*, uint32_t> was;
    for (const auto& [key, sw] : g_swaps)
      for (const auto& a : sw.at) if (a.ptr == 0) was[a.entry] = a.orig;
    for (uint32_t k = 0; k < job.count; k++) {
      const uint32_t* e = reinterpret_cast<const uint32_t*>(st.buf + 4 + k * 36 + 32);
      const auto w = was.find(e);
      const uint32_t o = w != was.end() ? w->second : *e;
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
  bool swapIn(const States& st, const std::string& name, const std::string& state, Swap& sw, bool now, const uint8_t* other, const std::string& what) {
    if (!st.buf) return false;
    const uint32_t count = *reinterpret_cast<const uint32_t*>(st.buf);
    const uint8_t* code = st.buf + 4 + static_cast<size_t>(count) * 36;
    for (uint32_t k = 0; k < count && k < st.names.size(); k++) {
      if (st.names[k] != name) continue;
      uint32_t* entry = reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(st.buf) + 4 + k * 36 + 32);
      if (!writable(entry, 4)) { sw.no = "its entry in the script's state table can't be written."; return true; }
      bool known = false;
      for (const auto& a : sw.at) if (a.entry == entry) known = true;
      if (known) return true;  // (both sides on one file: once)
      auto& mem = g_swap_mem[st.buf];
      {  // (a table still pointing into one of our blocks isn't the game's: never recorded as its original)
        const uint8_t* now_at = code + *entry;
        bool ours_now = false;
        for (auto& [p, n] : mem) if (now_at >= p && now_at < p + n) ours_now = true;
        if (ours_now) {
          COMBO_LOG("script & boxes: " + state + " still points at a loaded copy: not changed (reload the fight)");
          sw.no = "the script still points at a copy loaded earlier. Reload the fight.";
          return true;
        }
      }
      uint8_t* block = nullptr;
      for (auto& [p, n] : mem) if (n >= sw.code.size() && std::memcmp(p, sw.code.data(), sw.code.size()) == 0) block = p;  // (this one's, from before)
      if (!block) {
        block = takeNear(st.buf, code, sw.code.size() + 64, now);  // (padded: nothing reads past the end into another block)
        if (!block) {
          sw.no = g_no_room ? kNoRoomNote : "memory near the script is still being looked for.";
          if (const auto ns = g_near_search.find(st.buf); ns != g_near_search.end() && ns->second.going) sw.near_wait = true;
          return true;
        }
        std::memcpy(block, sw.code.data(), sw.code.size());
        mem.push_back({block, sw.code.size()});
      }
      const uint32_t orig = *entry, ours = static_cast<uint32_t>(block - code);
      sw.at.push_back({entry, orig, ours});
      *entry = ours;
      sw.no.clear();
      if (orig < 64) {  // (a move at the very start of the file: its start is too common a number to look for)
        COMBO_LOG("script & boxes: " + state + " starts at the very start of the file: only its table entry is changed");
        return true;
      }
      if (g_table_copies.count(st.buf)) patchCopies(st.buf, sw, orig, ours, state);  // (else once the background search is in)
      else startScan(st, other, what);
      return true;
    }
    return false;
  }
  void applySwap(const std::string& chr, const std::string& state, Swap& sw, asw_entity* const ents[2], bool now = false) {
    sw.near_wait = false;
    if (sw.at.empty() && inFight(chr, ents) && copyHeld(chr, state, sw)) return;
    if (isObj(state)) {
      if (!inFight(chr, ents)) return;
      auto it = g_ef.find(chr);
      if (it == g_ef.end() || it->second.files.empty()) { if (sw.at.empty()) sw.no = kEfWait; return; }
      const size_t before = sw.at.size();
      bool named = false;
      for (const States& f : it->second.files) named = swapIn(f, objName(state), state, sw, now, nullptr, chr + "'s object script") || named;
      if (!named && sw.at.empty()) sw.no = "no state called " + objName(state) + " in " + chr + "'s object script.";
      if (sw.at.size() != before) efStartsAgain();
      return;
    }
    for (int i = 0; i < 2; i++) {
      if (!ents[i] || sideChr(ents[i]) != chr) continue;
      swapIn(g_states[i], state, state, sw, now, g_states[1 - i].buf != g_states[i].buf ? g_states[1 - i].buf : nullptr, chr);
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
      for (const auto& [c, ef] : g_ef)
        for (const States& f : ef.files) if (f.buf == buf && inFight(c, ents)) loaded = true;
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
    bool back = false;
    for (const auto& a : sw.at) {
      if (a.ptr && !(readable(reinterpret_cast<const uint8_t*>(a.entry) + 8, 8) && *reinterpret_cast<const uint64_t*>(reinterpret_cast<const uint8_t*>(a.entry) + 8) == a.ptr)) continue;  // (not the list any more)
      if (writable(a.entry, 4) && *a.entry == a.ours) { *a.entry = a.orig; back = true; }
    }
    sw.at.clear();
    if (back && !g_ef.empty()) efStartsAgain();
  }

  bool replaceable(int op, bool as_new) {
    if (op < 0 || op >= static_cast<int>(bbscript::opcode::MAX) || !bbscript::instruction_sizes || op == 2) return false;
    auto it = g_db.instr.find(op);
    if (it == g_db.instr.end() || it->second.block != 0) return false;
    const std::string& n = it->second.name;
    for (const char* p : {"if", "else", "end", "begin", "upon", "label", "goto", "jumpTo", "callSubroutine"}) if (n.rfind(p, 0) == 0) return false;
    if (as_new) for (const ArgDef& a : it->second.args) if (a.kind == Arg::String16 || a.kind == Arg::String32) return false;
    return true;
  }
  struct Pending {
    SReq req;
    std::string head;
    bool line = false, on_copy = false, had = false, fresh = true;
    Swap was;
    std::vector<std::pair<SKey, SEdit>> dropped;
    std::vector<uint8_t> code;
    std::vector<SKey> after;
    int more = 0;
  };
  std::map<std::pair<std::string, std::string>, Pending> g_pending;
  struct PutReq { const SReq* req = nullptr; std::string head; bool line = false, on_copy = false; };
  bool putSwap(int side, const std::string& chr, const std::string& state, Swap sw, asw_entity* const ents[2], const std::string& from, const PutReq& pr = PutReq{}) {
    std::vector<std::pair<SKey, SEdit>> dropped;
    for (auto it = g_sedits.begin(); it != g_sedits.end();) {
      if (it->first.chr == chr && it->first.state == state) { putBackSEdit(it->second); dropped.push_back(*it); it = g_sedits.erase(it); }
      else ++it;
    }
    Swap was;
    bool had = false;
    auto old = g_swaps.find({chr, state});
    if (old != g_swaps.end()) { putBackSwap(old->second); was = std::move(old->second); had = true; g_swaps.erase(old); }
    const auto pend = g_pending.find({chr, state});
    bool pend_now = false;
    for (int i = 0; i < 2; i++)  // (read again if an earlier request in this batch cleared them)
      if (ents[i] && !g_states[i].buf) readStates(ents[i], g_states[i]);
    bool ok = true;
    if (!sw.folder.empty()) {
      auto& at = g_swaps[{chr, state}] = std::move(sw);
      applySwap(chr, state, at, ents, true);
      g_swap_no = at.no;
      COMBO_LOG("script & boxes: " + chr + " " + state + ": loaded from " + from + " (" + std::to_string(at.code.size()) + " bytes, " + std::to_string(at.at.size()) + " table entries pointed at it)");
      const bool no_ef = isObj(state) && at.at.empty() && (g_ef.find(chr) == g_ef.end() || g_ef[chr].files.empty());
      const bool waiting = at.at.empty() && (nearGoing() || no_ef);
      if (waiting) COMBO_LOG("script & boxes: " + chr + " " + state + (no_ef ? ": goes on once the object script is found (one of its objects out)" : ": goes on once memory near the script is found"));
      ok = !at.at.empty() || waiting;
      if (waiting && !no_ef && at.near_wait && (pr.req || pend != g_pending.end())) {
        pend_now = true;
        if (pend != g_pending.end()) {
          pend->second.code = at.code;
          pend->second.more++;
          pend->second.fresh = true;
        } else {
          Pending& p = g_pending[{chr, state}];
          p.req = *pr.req;
          p.head = pr.head;
          p.line = pr.line;
          p.on_copy = pr.on_copy;
          p.had = had;
          p.was = std::move(was);
          p.dropped = dropped;
          p.code = at.code;
        }
      }
      if (!ok) {
        g_swaps.erase({chr, state});
        if (had) {
          auto& back = g_swaps[{chr, state}] = std::move(was);
          applySwap(chr, state, back, ents, true);
          const bool kept = back.at.empty() && back.held;
          const bool again = !back.at.empty() || kept;
          if (!again) g_swaps.erase({chr, state});
          if (kept)
            for (auto& [dk, ded] : dropped) {
              ded.at.clear();
              auto in = g_sedits.emplace(dk, std::move(ded)).first;
              applySEdit(in->first, in->second, ents);
            }
          COMBO_LOG("script & boxes: " + chr + " " + state + (kept ? ": the copy loaded before is kept with its value edits (it isn't on in this fight)"
                                                                     : again ? ": the script loaded before is back in place" : ": the script loaded before couldn't be put back, the move is the game's own"));
        }
      }
    } else COMBO_LOG("script & boxes: " + chr + " " + state + ": back to today's version");
    if (!pend_now) g_pending.erase({chr, state});
    g_sel_cache[side] = Sel{};
    g_states[0] = States{};  // (the moves' starts read again: the table points elsewhere now)
    g_states[1] = States{};
    return ok;
  }
  std::vector<uint8_t> unitBytes(const uint8_t* p) {
    std::vector<uint8_t> out;
    if (!bbscript::instruction_sizes || !readable(p, 4)) return out;
    const int end_op = i32(p) == 0 ? 1 : 16;
    while (out.size() < kMaxCode && readable(p, 4)) {
      const int op = i32(p);
      if (op < 0 || op >= static_cast<int>(bbscript::opcode::MAX)) return {};
      const int size = bbscript::instruction_sizes[op];
      if (size < 4 || !readable(p, static_cast<size_t>(size))) return {};
      out.insert(out.end(), p, p + size);
      p += size;
      if (op == end_op) return out;
    }
    return {};
  }
  bool unitBytesLong(const uint8_t* p) {
    if (!bbscript::instruction_sizes) return false;
    size_t n = 0;
    while (n < kMaxCode && readable(p + n, 4)) {
      const int op = i32(p + n);
      if (op < 0 || op >= static_cast<int>(bbscript::opcode::MAX) || bbscript::instruction_sizes[op] < 4) return false;
      if (op == 1 || op == 16) return false;
      n += static_cast<size_t>(bbscript::instruction_sizes[op]);
    }
    return n >= kMaxCode;
  }
  bool lineStartAt(const uint8_t* start, int offset) {
    if (!start || offset < 0 || !bbscript::instruction_sizes) return false;
    const uint8_t* want = start + offset;
    const uint8_t* p = start;
    while (p <= want && static_cast<size_t>(p - start) < kMaxCode && readable(p, 4)) {
      if (p == want) return true;
      const int op = i32(p);
      if (op < 0 || op >= static_cast<int>(bbscript::opcode::MAX) || op == 1 || op == 16) return false;
      const int size = bbscript::instruction_sizes[op];
      if (size < 4) return false;
      p += size;
    }
    return false;
  }

  constexpr size_t kCommonScript = 0x8A8;
  using RKey = std::pair<std::string, std::string>;
  struct RPlace { const uint8_t* holder = nullptr; const uint8_t* file = nullptr; const uint8_t* table = nullptr; const uint8_t* arr = nullptr; const uint8_t* code = nullptr; int n = 0; };
  struct RSwap {
    Swap sw;
    std::vector<RPlace> places;
    int tries = 0;
    bool dead = false;
    std::string note;
  };
  std::map<RKey, RSwap> g_rswaps;
  std::map<std::string, RSwap> g_risc_rswaps;
  const uint8_t* g_risc_file = nullptr;
  std::map<std::string, std::vector<uint8_t>> g_common_ver;
  std::vector<uint8_t> routineBytes(const std::vector<uint8_t>& f, const std::string& name) {
    if (f.size() < 4 || !bbscript::instruction_sizes) return {};
    int want_op = 15;
    const std::string base = ops::baseName(name, want_op);
    const uint32_t count = u32(f.data());
    if (count == 0 || count > 8000 || 4 + static_cast<size_t>(count) * 36 > f.size()) return {};
    size_t p = 4 + static_cast<size_t>(count) * 36, from = 0;
    bool in = false;
    for (int n = 0; n < 4000000 && p + 4 <= f.size(); n++) {
      const int op = i32(f.data() + p);
      if (op < 0 || op >= static_cast<int>(bbscript::opcode::MAX)) return {};
      const int size = bbscript::instruction_sizes[op];
      if (size < 4 || size % 4 || p + static_cast<size_t>(size) > f.size()) return {};
      if (in && (op == 0 || op == 1 || ops::unitBegin(op))) return {};
      if (!in && op == want_op && size >= 36 && cstr(f.data() + p + 4, 32) == base) { in = true; from = p; }
      p += static_cast<size_t>(size);
      if (in && op == 16) return std::vector<uint8_t>(f.begin() + from, f.begin() + p);
    }
    return {};
  }
  const std::vector<uint8_t>& commonVersion(const std::string& folder) {
    const auto it = g_common_ver.find(folder);
    if (it != g_common_ver.end()) return it->second;
    std::vector<uint8_t> b;
    const std::wstring wn(folder.begin(), folder.end());
    if (!pak_mount::readFile(L"../../../RED/Content/Chara/CMN/Common/Data/" + wn + L"/BBS_CMNEF_" + wn + L".uexp", b) || b.size() < 0x40) b.clear();
    else b.erase(b.begin(), b.begin() + 0x34);
    return g_common_ver[folder] = std::move(b);
  }
  const std::vector<uint8_t>* versionPayload(const std::string& file, const std::string& name, const std::string& folder) {
    if (file == "CMN") { const std::vector<uint8_t>& f = commonVersion(folder); return f.empty() ? nullptr : &f; }
    for (const VerFile& v : verFiles(file, "BBS_" + file + (isObj(name) ? "EF" : ""))) if (v.folder == folder) return &v.payload;
    return nullptr;
  }
  std::string loadRoutine(const std::string& file, const std::string& name, const std::string& folder, std::vector<uint8_t>& code) {
    if (folder.size() != 3 || !std::all_of(folder.begin(), folder.end(), [](char c) { return c >= '0' && c <= '9'; })) return "no version " + folder;
    const std::string plain = objName(name);
    if (plain.empty() || plain.size() > 40) return "no routine of that name";
    const std::vector<uint8_t>* f = versionPayload(file, name, folder);
    if (!f) return "version " + verLabel(folder) + " of the script couldn't be read";
    code = routineBytes(*f, plain);
    if (code.empty()) return "version " + verLabel(folder) + " has no routine " + plain + " that fits today's instructions";
    return {};
  }
  struct CommonNow { const uint8_t* file = nullptr; const uint8_t* code = nullptr; const uint8_t* table = nullptr; const uint8_t* arr = nullptr; int n = 0; };
  bool holderNow(const uint8_t* holder, CommonNow& c, std::string& why) {
    if (!holder || !readable(holder, 0x38)) { why = "the script isn't there"; return false; }
    c.file = *reinterpret_cast<const uint8_t* const*>(holder);
    c.code = *reinterpret_cast<const uint8_t* const*>(holder + 8);
    c.table = *reinterpret_cast<const uint8_t* const*>(holder + 0x30);
    if (!quickScript(c.file)) { why = "the script isn't loaded"; return false; }
    if (c.code != c.file + 4 + static_cast<size_t>(u32(c.file)) * 36) { why = "the script isn't laid out as expected"; return false; }
    if (!c.table || !readable(c.table, 0x18)) { why = "the script's routine table isn't there"; return false; }
    c.arr = *reinterpret_cast<const uint8_t* const*>(c.table + 8);
    c.n = i32(c.table + 0x10);
    if (c.n <= 0 || c.n > 20000 || !c.arr || !readable(c.arr, static_cast<size_t>(c.n) * 24)) { why = "the script's routine table isn't laid out as expected"; return false; }
    return true;
  }
  bool commonNow(const asw_engine* engine, CommonNow& c, std::string& why) {
    if (!engine) { why = "no fight"; return false; }
    if (!holderNow(reinterpret_cast<const uint8_t*>(engine) + kCommonScript, c, why)) { why = "the common script: " + why; return false; }
    return true;
  }
  void wantCommonEf(const asw_engine* engine) {
    CommonNow c;
    std::string why;
    if (!commonNow(engine, c, why) || !c.file) return;
    EfFiles& ef = g_ef["CMN"];
    if (!ef.files.empty() && ef.files.front().buf == c.file) return;
    States st;
    readStatesAt(c.file, st);
    if (st.names.empty()) return;
    if (!ef.files.empty()) {
      for (auto& [k, sw] : g_swaps) if (k.first == "CMN") putBackSwap(sw);
      for (auto& [k, ed] : g_sedits) if (k.chr == "CMN") putBackSEdit(ed);
    }
    char at[32];
    snprintf(at, sizeof(at), "%p", static_cast<const void*>(c.file));
    COMBO_LOG("script & boxes: the common script at " + std::string(at) + " has " + std::to_string(st.names.size()) + " shared object states");
    ef.files.clear();
    ef.files.push_back(std::move(st));
    g_ef_added = true;
  }
  bool placeLive(const RPlace& p, const Swap::At& a) {
    if (!p.table || !p.arr || p.n <= 0 || !readable(p.table, 0x18)) return false;
    if (*reinterpret_cast<const uint8_t* const*>(p.table + 8) != p.arr || i32(p.table + 0x10) != p.n) return false;
    const uint8_t* e = reinterpret_cast<const uint8_t*>(a.entry);
    return e >= p.arr && e + 4 <= p.arr + static_cast<size_t>(p.n) * 24 && readable(e, 4);
  }
  bool rswapLive(const RSwap& r) {
    for (size_t i = 0; i < r.sw.at.size() && i < r.places.size(); i++)
      if (placeLive(r.places[i], r.sw.at[i]) && *r.sw.at[i].entry == r.sw.at[i].ours) return true;
    return false;
  }
  struct SharedCache { const uint8_t* file = nullptr; const uint8_t* table = nullptr; const uint8_t* arr = nullptr; int n = 0; std::map<std::string, const uint8_t*> starts; };
  SharedCache g_shared;
  const std::map<std::string, const uint8_t*>& sharedRoutines(const asw_engine* engine) {
    static const std::map<std::string, const uint8_t*> none;
    CommonNow c;
    std::string why;
    if (!commonNow(engine, c, why)) return none;
    if (g_shared.file != c.file || g_shared.table != c.table || g_shared.arr != c.arr || g_shared.n != c.n) {
      g_shared.file = c.file;
      g_shared.table = c.table;
      g_shared.arr = c.arr;
      g_shared.n = c.n;
      g_shared.starts.clear();
      walkCode(c.code, [&](const uint8_t* q, int op, int size) {
        if (op == 15 && size >= 36) g_shared.starts.emplace(cstr(q + 4, 32), q);
        return true;
      });
      COMBO_LOG("script & boxes: the common script has " + std::to_string(g_shared.starts.size()) + " shared routines");
    }
    return g_shared.starts;
  }
  std::vector<const uint8_t*> loadedStarts(const RSwap& r) {
    std::vector<const uint8_t*> out;
    for (size_t i = 0; i < r.sw.at.size() && i < r.places.size(); i++) {
      const Swap::At& a = r.sw.at[i];
      if (!placeLive(r.places[i], a) || *a.entry != a.ours || !r.places[i].code) continue;
      const uint8_t* p = r.places[i].code + a.ours;
      if (readable(p - 36, 40) && !ops::unitBegin(i32(p)) && ops::unitBegin(i32(p - 36))) p -= 36;
      if (readable(p, 40) && ops::unitBegin(i32(p)) && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    }
    return out;
  }
  const uint8_t* loadedRoutineStart(const RSwap& r) {
    const std::vector<const uint8_t*> s = loadedStarts(r);
    return s.empty() ? nullptr : s.front();
  }
  const uint8_t* sharedStart(const std::string& name) {
    const asw_engine* engine = asw_engine::get();
    const auto& all = sharedRoutines(engine);
    const auto it = all.find(name);
    if (it == all.end()) return nullptr;
    if (const auto r = g_rswaps.find({"CMN", name}); r != g_rswaps.end())
      if (const uint8_t* p = loadedRoutineStart(r->second)) return p;
    if (const auto r = g_risc_rswaps.find(name); r != g_risc_rswaps.end())
      if (const uint8_t* p = loadedRoutineStart(r->second)) return p;
    return it->second;
  }
  bool efFile(const std::string& chr, const uint8_t* file) {
    const auto it = g_ef.find(chr);
    if (it == g_ef.end()) return false;
    for (const States& f : it->second.files) if (f.buf == file) return true;
    return false;
  }
  std::vector<const uint8_t*> routineHolders(const RKey& k, asw_entity* const ents[2], const asw_engine* engine) {
    std::vector<const uint8_t*> out;
    auto add = [&](const void* h) {
      const uint8_t* p = static_cast<const uint8_t*>(h);
      if (p && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    };
    if (k.first == "CMN") {
      if (engine) add(reinterpret_cast<const uint8_t*>(engine) + kCommonScript);
      return out;
    }
    if (isObj(k.second)) {
      const auto it = g_ef.find(k.first);
      if (it == g_ef.end() || !inFight(k.first, ents)) return out;
      for (const void* h : it->second.table_holders) add(h);
      for (const void* h : it->second.holders) add(h);
      return out;
    }
    for (int i = 0; i < 2; i++) if (ents[i] && sideChr(ents[i]) == k.first) add(ents[i]->bbs_file);
    return out;
  }
  std::map<std::pair<const uint8_t*, uint32_t>, size_t> g_code_len;
  struct Stuck { RPlace place; Swap::At at; };
  std::vector<Stuck> g_stuck;
  bool repoint(RPlace& p, Swap::At& a) {
    CommonNow c;
    std::string why;
    if (!p.holder || !holderNow(p.holder, c, why) || c.file != p.file) return false;
    std::vector<int> hits;
    for (int j = 0; j < c.n && hits.size() < 2; j++)
      if (u32(c.arr + static_cast<size_t>(j) * 24 + 0x10) == a.ours) hits.push_back(j);
    if (hits.size() != 1) return false;
    p.table = c.table;
    p.arr = c.arr;
    p.n = c.n;
    p.code = c.code;
    a.entry = reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(c.arr) + static_cast<size_t>(hits[0]) * 24 + 0x10);
    return true;
  }
  void retryStuck() {
    for (auto it = g_stuck.begin(); it != g_stuck.end();) {
      const bool live = placeLive(it->place, it->at) && *it->at.entry == it->at.ours;
      if (live && !writable(it->at.entry, 4)) { ++it; continue; }
      if (live) *it->at.entry = it->at.orig;
      it = g_stuck.erase(it);
    }
  }
  std::vector<uint32_t> oursOn(const uint8_t* arr) {
    std::vector<uint32_t> out;
    auto add = [&](const RSwap& r) {
      for (size_t i = 0; i < r.sw.at.size() && i < r.places.size(); i++)
        if (r.places[i].arr == arr) out.push_back(r.sw.at[i].ours);
    };
    for (const auto& kv : g_rswaps) add(kv.second);
    for (const auto& kv : g_risc_rswaps) add(kv.second);
    return out;
  }
  std::string tableMismatch(const CommonNow& c) {
    const std::pair<const uint8_t*, uint32_t> key{c.file, u32(c.file)};
    auto len = g_code_len.find(key);
    if (len == g_code_len.end()) {
      size_t n = 0;
      walkCode(c.code, [&](const uint8_t* q, int, int size) { n = static_cast<size_t>(q - c.code) + static_cast<size_t>(size); return true; });
      len = g_code_len.emplace(key, n).first;
    }
    const std::vector<uint32_t> ours = oursOn(c.arr);
    for (int j = 0; j < c.n; j++) {
      const uint32_t v = u32(c.arr + static_cast<size_t>(j) * 24 + 0x10);
      if (std::find(ours.begin(), ours.end(), v) != ours.end()) continue;
      const bool at = v + 4 <= len->second && ops::unitBegin(i32(c.code + v));
      const bool after = v >= 36 && v - 36 + 4 <= len->second && ops::unitBegin(i32(c.code + v - 36));
      if (at || after) continue;
      return std::to_string(c.n) + " entries; entry " + std::to_string(j) + " holds " + std::to_string(v) + ", which isn't the start of a routine in its " + std::to_string(len->second) + " bytes of code";
    }
    return {};
  }
  bool applyRSwap(const RKey& k, RSwap& r, asw_entity* const ents[2], const asw_engine* engine, std::string& why) {
    Swap& sw = r.sw;
    int base_op = 15;
    const std::string base = ops::baseName(objName(k.second), base_op);
    const std::string who = k.first == "CMN" ? std::string("the common script") : k.first + (isObj(k.second) ? "'s object script" : "'s script");
    bool any = false, waiting = false;
    std::string bad;
    for (const uint8_t* h : routineHolders(k, ents, engine)) {
      CommonNow c;
      std::string no;
      if (!holderNow(h, c, no)) { if (bad.empty()) bad = who + ": " + no; continue; }
      if (isObj(k.second) && !efFile(k.first, c.file)) continue;
      bool have = false;
      for (size_t i = 0; i < sw.at.size() && i < r.places.size(); i++)
        if (r.places[i].arr == c.arr && placeLive(r.places[i], sw.at[i]) && *sw.at[i].entry == sw.at[i].ours) have = true;
      if (have) { any = true; continue; }
      const uint8_t* start = nullptr;
      walkCode(c.code, [&](const uint8_t* q, int op, int size) {
        if (op == base_op && size >= 36 && cstr(q + 4, 32) == base) { start = q; return false; }
        return true;
      });
      if (!start) { bad = who + " has no routine " + base; continue; }
      const uint64_t off0 = static_cast<uint64_t>(start - c.code);
      int at0 = -1, at1 = -1, n0 = 0, n1 = 0;
      for (int j = 0; j < c.n; j++) {
        const uint32_t v = u32(c.arr + static_cast<size_t>(j) * 24 + 0x10);
        if (v == off0) { at0 = j; n0++; }
        if (v == off0 + 36) { at1 = j; n1++; }
      }
      const int skip = (n0 == 1 && n1 == 0) ? 0 : (n1 == 1 && n0 == 0) ? 36 : -1;
      if (skip < 0) { bad = who + "'s routine table has " + std::to_string(n0 + n1) + " entries for " + base + ", not one, so the game's entry for it isn't known"; continue; }
      if (std::string tb = k.first == "CMN" ? std::string() : tableMismatch(c); !tb.empty()) {
        bad = who + "'s routine table doesn't match its script (" + tb + "), so nothing was changed";
        COMBO_LOG("script & boxes: routine " + base + ": " + bad);
        continue;
      }
      const int index = skip ? at1 : at0;
      uint32_t* entry = reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(c.arr) + static_cast<size_t>(index) * 24 + 0x10);
      if (!writable(entry, 4)) { bad = who + "'s routine table can't be written"; continue; }
      bool known = false;
      for (const auto& a : sw.at) if (a.entry == entry) known = true;
      if (known) { any = true; continue; }
      auto& mem = g_swap_mem[c.file];
      uint8_t* block = nullptr;
      for (auto& [bp, bn] : mem) if (bn >= sw.code.size() && std::memcmp(bp, sw.code.data(), sw.code.size()) == 0) block = bp;
      if (!block) {
        block = takeNear(c.file, c.code, sw.code.size() + 64, false);
        if (!block) { waiting = true; continue; }
        std::memcpy(block, sw.code.data(), sw.code.size());
        mem.push_back({block, sw.code.size()});
      }
      const uint64_t ours = static_cast<uint64_t>(block - c.code) + static_cast<uint64_t>(skip);
      if (block < c.code || ours > 0xFFFFFFFFull) { bad = "the loaded copy is out of the table's reach"; continue; }
      Swap::At a;
      a.entry = entry;
      a.orig = *entry;
      a.ours = static_cast<uint32_t>(ours);
      a.ptr = 0;
      sw.at.push_back(a);
      r.places.push_back({h, c.file, c.table, c.arr, c.code, c.n});
      *entry = a.ours;
      any = true;
      COMBO_LOG("script & boxes: routine " + base + " of " + who + ": " + (r.note.empty() ? "loaded from version " + verLabel(sw.folder) : r.note) + " (" + std::to_string(sw.code.size()) +
                " bytes, routine table entry " + std::to_string(index) + " of " + std::to_string(c.n) + ", start " + (skip ? "after" : "at") + " its first line)");
    }
    if (any) return true;
    why = waiting && g_no_room ? std::string(kNoRoomNote) : waiting ? std::string() : bad;
    return false;
  }
  void putBackRoutine(RSwap& r) {
    for (size_t i = 0; i < r.sw.at.size() && i < r.places.size(); i++) {
      Swap::At& a = r.sw.at[i];
      if (!placeLive(r.places[i], a)) repoint(r.places[i], a);
      if (!placeLive(r.places[i], a) || *a.entry != a.ours) continue;
      if (writable(a.entry, 4)) { *a.entry = a.orig; continue; }
      g_stuck.push_back({r.places[i], a});
      COMBO_LOG("script & boxes: a routine table entry can't be written right now: its put-back is kept and tried again");
    }
    r.sw.at.clear();
    r.places.clear();
    r.tries = 0;
    r.dead = false;
  }
  void putBackRiscRoutines() {
    bool any = false;
    for (auto& kv : g_risc_rswaps) {
      if (!kv.second.sw.at.empty()) any = true;
      putBackRoutine(kv.second);
    }
    g_risc_rswaps.clear();
    g_risc_file = nullptr;
    if (any) COMBO_LOG("battle rules: RISC gain values in the shared routines back to today's");
  }
  void putBackRoutines() {
    retryStuck();
    putBackRiscRoutines();
    for (auto& kv : g_rswaps) putBackRoutine(kv.second);
  }
  void syncRoutines(const asw_engine* engine, asw_entity* const ents[2]) {
    retryStuck();
    if (g_rswaps.empty()) return;
    const bool fight = ents[0] && ents[1];
    for (auto& [name, r] : g_risc_rswaps)
      if (!r.sw.at.empty() && g_rswaps.count({"CMN", name})) {
        putBackRoutine(r);
        COMBO_LOG("battle rules: shared routine " + name + " is set by the edits file: the 4.06 RISC values come off it");
      }
    for (auto& [k, r] : g_rswaps) {
      for (size_t i = 0; i < r.sw.at.size();) {
        if (i < r.places.size() && !placeLive(r.places[i], r.sw.at[i]) && repoint(r.places[i], r.sw.at[i]))
          COMBO_LOG("script & boxes: routine " + objName(k.second) + " (" + k.first + "): its routine table moved, the copy is followed there");
        if (i < r.places.size() && placeLive(r.places[i], r.sw.at[i]) && *r.sw.at[i].entry == r.sw.at[i].ours) { i++; continue; }
        r.sw.at.erase(r.sw.at.begin() + static_cast<ptrdiff_t>(i));
        if (i < r.places.size()) r.places.erase(r.places.begin() + static_cast<ptrdiff_t>(i));
        COMBO_LOG("script & boxes: routine " + objName(k.second) + " (" + k.first + "): its script was loaded again, putting the copy on again");
      }
      if (r.dead || r.sw.code.empty() || !engine || !fight) continue;
      if (routineHolders(k, ents, engine).empty()) continue;
      if (r.sw.at.empty() && copyHeld(k.first, k.second, r.sw)) continue;
      std::string why;
      if (applyRSwap(k, r, ents, engine, why)) { r.tries = 0; continue; }
      if (why.empty() && ++r.tries < 120) continue;
      r.dead = true;
      r.sw.no = why.empty() ? std::string("no memory near the script was found") : why;
      COMBO_LOG("script & boxes: routine " + objName(k.second) + " (" + k.first + "): " + r.sw.no + ": it stays the game's own until the fight is reloaded");
    }
  }
  std::string unitState(const RKey& k) { return k.first == "CMN" ? kSharedPrefix + k.second : k.second; }
  int putRSwap(const RKey& k, std::vector<uint8_t> code, const std::string& folder, const std::string& note, asw_entity* const ents[2], std::string& why, std::vector<char> added = {}) {
    const std::string chr = k.first, state = unitState(k);
    std::vector<std::pair<SKey, SEdit>> dropped;
    for (auto it = g_sedits.begin(); it != g_sedits.end();) {
      if (it->first.chr == chr && it->first.state == state) { putBackSEdit(it->second); dropped.push_back(*it); it = g_sedits.erase(it); }
      else ++it;
    }
    const asw_engine* engine = asw_engine::get();
    RSwap was;
    bool had = false;
    if (auto old = g_rswaps.find(k); old != g_rswaps.end()) { putBackRoutine(old->second); was = old->second; had = true; }
    RSwap n;
    n.sw.code = std::move(code);
    n.sw.folder = folder;
    n.sw.added = std::move(added);
    n.sw.ver = copyVer(k.first);
    n.note = note;
    g_rswaps[k] = std::move(n);
    syncRoutines(engine, ents);
    g_sel_cache[0] = Sel{};
    g_sel_cache[1] = Sel{};
    RSwap& now = g_rswaps[k];
    if (now.dead) {
      why = now.sw.no;
      g_rswaps.erase(k);
      if (had) {
        g_rswaps[k] = std::move(was);
        syncRoutines(engine, ents);
        if (const RSwap& back = g_rswaps[k]; back.sw.held && back.sw.at.empty())
          for (auto& [dk, ded] : dropped) {
            ded.at.clear();
            auto in = g_sedits.emplace(dk, std::move(ded)).first;
            applySEdit(in->first, in->second, ents);
          }
      }
      return -1;
    }
    return now.sw.at.empty() ? 0 : 1;
  }
  const uint8_t* moveStart(int side, const std::string& chr, const std::string& name) {
    if (isShared(name)) return sharedStart(sharedName(name));
    if (const auto r = g_rswaps.find({chr, name}); r != g_rswaps.end())
      if (const uint8_t* p = loadedRoutineStart(r->second)) return p;
    if (const uint8_t* w = waitingCopy(chr, name)) return w;
    if (isObj(name)) return efStart(chr, objName(name));
    return side >= 0 && side < 2 ? stateStart(g_states[side], name) : nullptr;
  }
  bool copyOff(const std::string& chr, const std::string& name) {
    if (const auto r = g_rswaps.find(isShared(name) ? RKey{"CMN", sharedName(name)} : RKey{chr, name}); r != g_rswaps.end() && !r->second.sw.code.empty())
      return loadedStarts(r->second).empty();
    if (const auto sw = g_swaps.find({chr, name}); sw != g_swaps.end() && !sw->second.code.empty()) return sw->second.at.empty();
    return false;
  }
  std::vector<const uint8_t*> copyStarts(const std::string& chr, const std::string& name) {
    const auto r = g_rswaps.find(isShared(name) ? RKey{"CMN", sharedName(name)} : RKey{chr, name});
    return r == g_rswaps.end() ? std::vector<const uint8_t*>() : loadedStarts(r->second);
  }

  enum { kMove = 0, kRoutine = 1, kObjState = 2, kObjRoutine = 3, kShared = 4 };
  bool routineKind(int kind) { return kind == kRoutine || kind == kObjRoutine || kind == kShared; }
  int unitKind(int side, const std::string& chr, const std::string& state) {
    if (isShared(state)) return kShared;
    if (isObj(state)) {
      const auto it = g_ef.find(chr);
      if (it != g_ef.end()) {
        for (const States& f : it->second.files) if (std::find(f.names.begin(), f.names.end(), objName(state)) != f.names.end()) return kObjState;
        for (const States& f : it->second.files) if (std::find(f.routines.begin(), f.routines.end(), objName(state)) != f.routines.end()) return kObjRoutine;
      }
      return kObjState;
    }
    if (side < 0 || side > 1) return kMove;
    const States& st = g_states[side];
    if (std::find(st.names.begin(), st.names.end(), state) != st.names.end()) return kMove;
    if (std::find(st.routines.begin(), st.routines.end(), state) != st.routines.end()) return kRoutine;
    return kMove;
  }
  RKey rkeyOf(const std::string& chr, const std::string& state) { return isShared(state) ? RKey{"CMN", sharedName(state)} : RKey{chr, state}; }
  const char* kindWord(int kind) { return kind == kRoutine ? "routine" : kind == kObjState ? "object state" : kind == kObjRoutine ? "object routine" : kind == kShared ? "shared routine" : "move"; }
  std::filesystem::path unitDir(const std::string& chr, const std::string& state) { return isShared(state) ? g_dir / "CMN" : isObj(state) ? g_dir / chr / "objects" : g_dir / chr; }
  std::string roundNote(int kind, const std::string& state) {
    return kind == kRoutine && state == "RoundInit" ? " RoundInit runs when a round starts, so this shows from the next round start or training reset." : std::string();
  }
  std::vector<const uint8_t*> codesFor(int side, const std::string& chr, const std::string& state) {
    std::vector<const uint8_t*> out;
    if (isShared(state)) {
      CommonNow c;
      std::string why;
      if (commonNow(asw_engine::get(), c, why)) out.push_back(c.code);
    } else if (isObj(state)) {
      if (auto it = g_ef.find(chr); it != g_ef.end()) for (const States& f : it->second.files) out.push_back(f.code);
    } else if (side >= 0 && side < 2 && g_states[side].code) out.push_back(g_states[side].code);
    return out;
  }
  bool knownName(int side, const std::string& chr, const std::string& state, int op, int field, int need, const std::string& name, asw_entity* const ents[2], std::string& why) {
    const bool obj = isObj(state);
    if (op == 2 && !obj && !isShared(state)) {
      const Col* col = ents[side] ? colOf(ents[side]) : nullptr;
      if (col && col->sprites.count(name)) return true;
    }
    const std::vector<const uint8_t*> codes = codesFor(side, chr, state);
    const bool call = (op == 17 || op == 75) && field == 4;
    const bool go = ops::gotoOp(op) && field == ops::labelAt(op);
    if (call) {
      if (sharedRoutines(asw_engine::get()).count(name)) return true;
      if (!isShared(state) && chr != "CMN" && side >= 0 && side < 2)
        for (const std::string& r : g_states[side].routines) { int o = 0; if (ops::baseName(r, o) == name) return true; }
      if (obj) if (auto it = g_ef.find(chr); it != g_ef.end()) for (const States& f : it->second.files) for (const std::string& r : f.routines) { int o = 0; if (ops::baseName(r, o) == name) return true; }
    }
    if (!go && !call && op != 2) {
      if (chr != "CMN" && side >= 0 && side < 2 && std::find(g_states[side].names.begin(), g_states[side].names.end(), name) != g_states[side].names.end()) return true;
      if (auto it = g_ef.find(chr); it != g_ef.end())
        for (const States& f : it->second.files)
          if (std::find(f.names.begin(), f.names.end(), name) != f.names.end()) return true;
    }
    for (const uint8_t* code : codes) {
      bool hit = false;
      walkCode(code, [&](const uint8_t* p, int o, int size) {
        if (go ? (o == 11 && size >= 36 && cstr(p + 4, 32) == name) : (o == op && field + need <= size && cstr(p + field, static_cast<size_t>(need)) == name)) hit = true;
        return !hit;
      });
      if (hit) return true;
    }
    const std::string where = isShared(state) ? std::string("the common script") : chr + (obj ? "'s object script" : "'s script");
    if (go) why = "No label called " + name + " in " + where + ": a goto can only go to a label.";
    else if (call) why = "No routine called " + name + " in " + where + " or the common script.";
    else if (op == 2) why = "No sprite called " + name + " in " + chr + "'s box file or script.";
    else why = "Nothing in " + where + " is called " + name + ".";
    return false;
  }
  std::string firstName(int side, const std::string& chr, const std::string& state, int op, int field, int need) {
    std::string out;
    for (const uint8_t* code : codesFor(side, chr, state)) {
      walkCode(code, [&](const uint8_t* p, int o, int size) {
        if (o == op && field + need <= size) out = cstr(p + field, static_cast<size_t>(need));
        return out.empty();
      });
      if (!out.empty()) return out;
    }
    if ((op == 17 || op == 75) && field == 4) {
      if (!isShared(state) && side >= 0 && side < 2 && !g_states[side].routines.empty()) { int o = 0; return ops::baseName(g_states[side].routines.front(), o); }
      const auto& sh = sharedRoutines(asw_engine::get());
      if (!sh.empty()) return sh.begin()->first;
    }
    return out;
  }

  std::string reqWords(const SReq& r) {
    static const char* const kNames[] = {"frames", "level", "reset", "value", "load", "replace", "reset line", "name", "insert", "copy", "remove", "value kind", "paste"};
    const int t = static_cast<int>(r.t);
    std::string w = std::string(t >= 0 && t < 13 ? kNames[t] : "request") + " +" + std::to_string(r.offset);
    if (r.t == SReq::Insert || r.t == SReq::Replace || r.t == SReq::Arg || r.t == SReq::Tag || r.t == SReq::Frames || r.t == SReq::Level) w += " value " + std::to_string(r.value);
    if (r.t == SReq::Arg || r.t == SReq::Tag || r.t == SReq::Name) w += " field " + std::to_string(r.field);
    if (!r.folder.empty()) w += " from " + r.folder;
    if (!r.text.empty()) {
      std::string t2 = r.text.substr(0, 400);
      for (char& ch : t2) if (ch == '\n' || ch == '\r') ch = '|';
      w += " text '" + t2 + "'" + (r.text.size() > 400 ? "..." : "");
    }
    return w;
  }
  void refuse(const SReq& r, const std::string& chr, const std::string& why) {
    g_smsg = why;
    COMBO_LOG("script & boxes: refused " + chr + " " + r.state + " (" + reqWords(r) + "): " + why);
  }
  struct UnitCode { int kind = 0; const uint8_t* start = nullptr; std::vector<uint8_t> code; std::vector<std::pair<SKey, SEdit>> kept; bool on_copy = false; };
  bool unitNow(int side, const std::string& chr, const std::string& state, UnitCode& u, std::string& why) {
    u.kind = unitKind(side, chr, state);
    u.start = moveStart(side, chr, state);
    if (!u.start) {
      why = isShared(state) ? "The common script isn't loaded, so " + sharedName(state) + " can't be read."
                            : isObj(state) && (g_ef.find(chr) == g_ef.end() || g_ef[chr].files.empty()) ? std::string(kEfWait)
                                                                                                      : "No " + std::string(kindWord(u.kind)) + " called " + objName(state) + " in the loaded script.";
      return false;
    }
    u.code = unitBytes(u.start);
    if (u.code.size() < 40) { why = unitBytesLong(u.start) ? std::string(kTooLong) : std::string("This script couldn't be read (it doesn't walk to its last line)."); return false; }
    const bool on_copy = startOnCopy(side, chr, state, u.start);
    u.on_copy = on_copy;
    for (const auto& [k, ed] : g_sedits) {
      if (k.chr != chr || k.state != state) continue;
      if (!routineKind(u.kind) && k.copy != on_copy) continue;
      u.kept.push_back({k, ed});
      const size_t at = static_cast<size_t>(k.offset) + static_cast<size_t>(k.field);
      if (k.offset < 0 || k.field < 0 || ed.orig.size() != ed.now.size() || at + ed.now.size() > u.code.size()) continue;
      if (std::memcmp(u.code.data() + at, ed.now.data(), ed.now.size()) == 0) std::memcpy(u.code.data() + at, ed.orig.data(), ed.orig.size());
    }
    return true;
  }
  struct LastPaste { std::string chr, state; size_t from = 0, to = 0; int id = 0; };
  LastPaste g_last_paste;
  int g_paste_ids = 0;
  std::vector<uint8_t> todayBytes(int side, const std::string& chr, const std::string& state, int kind);
  std::vector<char> flagsNow(int side, const std::string& chr, const std::string& state, int kind, const std::vector<uint8_t>& code) {
    const Swap* cur = nullptr;
    if (routineKind(kind)) { if (const auto r = g_rswaps.find(rkeyOf(chr, state)); r != g_rswaps.end()) cur = &r->second.sw; }
    else if (const auto sw = g_swaps.find({chr, state}); sw != g_swaps.end()) cur = &sw->second;
    const size_t n = ops::lineStarts(ctx(), code).size();
    if (!cur) return std::vector<char>(n, 0);
    if (cur->added.size() == n) return cur->added;
    const std::vector<uint8_t> today = todayBytes(side, chr, state, kind);
    std::vector<char> f = today.empty() ? std::vector<char>() : ops::addedLines(ctx(), today, code);
    if (f.size() != n) f.assign(n, 0);
    return f;
  }
  struct MarkCache { std::string key; std::vector<uint8_t> code; std::vector<char> marks; };
  struct InlCache { const uint8_t* start = nullptr; std::vector<uint8_t> code; Cache c; MarkCache marks; unsigned long long gen = 0; ULONGLONG read = 0; };
  std::map<std::string, InlCache> g_inl_cache[2];
  void unitMarks(MarkCache& mc, int side, const std::string& chr, const std::string& name, const std::vector<uint8_t>& code, const std::vector<InstrView>& script, std::vector<char>& added, int& paste_id) {
    const std::string mk = chr + "/" + name;
    if (name.empty() || code.empty()) { mc.key.clear(); mc.marks.clear(); }
    else if (mc.key != mk || mc.code != code) {
      mc.key = mk;
      mc.code = code;
      const std::vector<uint8_t> today = todayBytes(side, chr, name, unitKind(side, chr, name));
      mc.marks = today.empty() ? std::vector<char>() : ops::addedLines(ctx(), today, code);
    }
    const std::vector<size_t> starts = code.empty() ? std::vector<size_t>() : ops::lineStarts(ctx(), code);
    const std::vector<char>* use = &mc.marks;
    if (!name.empty() && !code.empty()) {
      const Swap* own = nullptr;
      if (const auto r = g_rswaps.find(rkeyOf(chr, name)); r != g_rswaps.end()) own = &r->second.sw;
      else if (const auto sw = g_swaps.find({chr, name}); sw != g_swaps.end()) own = &sw->second;
      if (own && !own->added.empty() && own->added.size() == starts.size()) use = &own->added;
    }
    const bool last = g_last_paste.chr == chr && g_last_paste.state == name && g_last_paste.id;
    added.assign(script.size(), 0);
    for (size_t k = 0; k < script.size(); k++) {
      const size_t off = static_cast<size_t>(script[k].offset);
      if (!use->empty()) {
        const auto it = std::lower_bound(starts.begin(), starts.end(), off);
        if (it != starts.end() && *it == off && static_cast<size_t>(it - starts.begin()) < use->size() && (*use)[static_cast<size_t>(it - starts.begin())]) added[k] = 1;
      }
      if (last && off >= g_last_paste.from && off < g_last_paste.to) added[k] = 2;
    }
    if (last) paste_id = g_last_paste.id;
  }
  void commitUnit(const SReq& r, const std::string& chr, UnitCode& u, std::vector<uint8_t> out, std::string done, const std::vector<ops::Splice>& sp, asw_entity* const ents[2]) {
    done += shapeNote(u.code, out);
    bool ok = true;
    std::string wait;
    bool on_risc = false;
    if (isShared(r.state) && !g_rswaps.count(rkeyOf(chr, r.state)))
      if (const auto f = g_risc_rswaps.find(sharedName(r.state)); f != g_risc_rswaps.end()) on_risc = !f->second.sw.at.empty();
    std::vector<char> added = ops::carryFlags(ctx(), u.code, flagsNow(r.side, chr, r.state, u.kind, u.code), sp, out);
    std::string replaced;
    std::vector<std::pair<SKey, SEdit>> other;
    std::vector<uint8_t> other_code;
    bool old_held = false;
    int old_ver = -1, lost = 0;
    const auto keepOther = [&]() {
      const auto cur = g_swaps.find({chr, r.state});
      const bool copy_there = cur != g_swaps.end() && !cur->second.code.empty() && cur->second.code == other_code;
      for (auto& [k, ed] : other) {
        if (k.copy && !copy_there) { lost++; continue; }
        ed.at.clear();
        const auto in = g_sedits.emplace(k, std::move(ed));
        if (in.second) applySEdit(k, in.first->second, ents);
        else if (ok) lost++;
      }
      other.clear();
      if (lost) COMBO_LOG("script & boxes: " + chr + " " + r.state + ": " + std::to_string(lost) + " value edit(s) made on the earlier copy's lines couldn't be kept");
    };
    if (!routineKind(u.kind)) {
      for (const auto& [k, ed] : g_sedits)
        if (k.chr == chr && k.state == r.state && k.copy != u.on_copy) other.push_back({k, ed});
      Swap sw;
      sw.code = std::move(out);
      sw.added = std::move(added);
      sw.ver = copyVer(chr);
      auto old = g_swaps.find({chr, r.state});
      if (old != g_swaps.end()) {
        other_code = old->second.code;
        old_held = old->second.held;
        old_ver = old->second.ver;
      }
      const std::string was = old != g_swaps.end() ? old->second.folder : std::string();
      sw.folder = "edit:" + (was.rfind("file:", 0) == 0 || was.rfind("edit:", 0) == 0 ? was.substr(5) : std::string());
      const bool had = old != g_swaps.end();
      PutReq pr;
      pr.req = &r;
      pr.head = "Couldn't apply the change: ";
      pr.line = true;
      pr.on_copy = u.on_copy;
      ok = putSwap(r.side, chr, r.state, std::move(sw), ents, "a line edit", pr);
      if (!ok) {
        refuse(r, chr, "Couldn't apply the change: " + (g_swap_no.empty() ? std::string("see combo_log.txt.") : g_swap_no));
        if (had && !g_swaps.count({chr, r.state})) {
          if (!u.kept.empty()) {
            g_smsg += " The move is back to the game's own lines, and its " + std::to_string(u.kept.size()) + " value edit(s) went with the copy.";
            COMBO_LOG("script & boxes: " + chr + " " + r.state + ": " + std::to_string(u.kept.size()) + " value edit(s) dropped with the copy that couldn't be put back");
          }
          keepOther();
          if (lost) g_smsg += " " + std::to_string(lost) + (lost == 1 ? " value edit" : " value edits") + " made on the earlier copy's lines couldn't be kept.";
          return;
        }
      } else if (const auto now = g_swaps.find({chr, r.state}); now != g_swaps.end() && now->second.at.empty())
        wait = isObj(r.state) && (g_ef.find(chr) == g_ef.end() || g_ef[chr].files.empty()) ? std::string(" ") + kEfWait : " It goes on once memory near the script is found.";
    } else {
      const RKey k = rkeyOf(chr, r.state);
      const auto old = g_rswaps.find(k);
      if (old != g_rswaps.end() && old->second.sw.held && old->second.sw.at.empty()) replaced = " It replaces your copy changed on " + verWords(old->second.sw.ver) + ", which isn't on in this fight.";
      const std::string was = old != g_rswaps.end() ? old->second.sw.folder : std::string();
      std::string why;
      const int placed = putRSwap(k, std::move(out), "edit:" + (was.rfind("file:", 0) == 0 || was.rfind("edit:", 0) == 0 ? was.substr(5) : std::string()), "changed in the editor", ents, why, std::move(added));
      ok = placed >= 0;
      if (!ok) refuse(r, chr, "Couldn't apply the change: " + why);
      if (placed == 0) wait = u.kind == kObjRoutine && g_ef[chr].files.empty() ? std::string(" ") + kEfWait : " It goes on once memory near the script is found.";
    }
    if (ok) g_smsg = done + wait + roundNote(u.kind, r.state) + replaced;
    if (ok && on_risc) {
      g_smsg += " It was built on the 4.06 RISC values that were on this routine, and it keeps them.";
      COMBO_LOG("script & boxes: " + sharedName(r.state) + " was edited while the 4.06 RISC values were on it: the edit keeps those values");
    }
    if (ok && (r.t == SReq::Insert || r.t == SReq::Copy || r.t == SReq::Paste) && sp.size() == 1 && sp[0].cut == 0 && !sp[0].add.empty())
      g_last_paste = LastPaste{chr, r.state, sp[0].at, sp[0].at + sp[0].add.size(), ++g_paste_ids};
    for (int i = 0; i < 2; i++)
      if (ents[i] && !g_states[i].buf) readStates(ents[i], g_states[i]);
    for (auto& [k, ed] : u.kept) {
      SKey nk = k;
      if (ok) {
        const long long m = ops::movedTo(sp, static_cast<size_t>(k.offset));
        if (m < 0) continue;
        nk.offset = static_cast<int>(m);
        if (!routineKind(u.kind)) nk.copy = true;
      }
      ed.at.clear();
      auto it = g_sedits.emplace(nk, std::move(ed)).first;
      applySEdit(nk, it->second, ents);
    }
    if (!routineKind(u.kind)) {
      keepOther();
      if (ok && old_held)
        g_smsg += " It replaces your copy changed on " + verWords(old_ver) + ", which isn't on in this fight" +
                  (lost ? ", and its " + std::to_string(lost) + (lost == 1 ? " value edit went" : " value edits went") + " with it." : std::string("."));
      else if (lost)
        g_smsg += " " + std::to_string(lost) + (lost == 1 ? " value edit" : " value edits") + " made on the earlier copy's lines couldn't be kept.";
    }
    g_sel_cache[r.side] = Sel{};
  }
  std::vector<uint8_t> todayBytes(int side, const std::string& chr, const std::string& state, int kind);
  void lineEdit(const SReq& r, const std::string& chr, asw_entity* const ents[2]) {
    auto fail = [&](const std::string& why) { refuse(r, chr, why); };
    UnitCode u;
    std::string why;
    if (!unitNow(r.side, chr, r.state, u, why)) { fail(why); return; }
    const ops::Ctx c = ctx();
    const size_t off = r.offset < 0 ? ops::npos : static_cast<size_t>(r.offset);
    const ops::Known known = [&](int op, int field, int need) { return firstName(r.side, chr, r.state, op, field, need); };
    ops::Change ch;
    if (r.t == SReq::Insert) ch = ops::insertLine(c, u.code, off, r.value, known);
    else if (r.t == SReq::Copy) ch = ops::copyLine(c, u.code, off);
    else if (r.t == SReq::Remove) ch = ops::removeLine(c, u.code, off);
    else if (r.t == SReq::Replace) ch = ops::replaceLine(c, u.code, off, r.value, known);
    else if (r.t == SReq::Name) ch = ops::renameLabel(c, u.code, off, r.text);
    else if (r.t == SReq::Paste) ch = ops::pasteLines(c, u.code, off, r.text);
    else return;
    if (!ch.err.empty()) { fail(ch.err); return; }
    if (ch.sp.empty()) return;
    if (off + 36 <= u.code.size() && ops::rd(u.code.data() + off) == 11 && (r.t == SReq::Name || r.t == SReq::Remove || r.t == SReq::Replace)) {
      const std::string label = cstr(u.code.data() + off + 4, 32);
      int in_file = 0, here = 0;
      for (const uint8_t* code : codesFor(r.side, chr, r.state))
        walkCode(code, [&](const uint8_t* p, int op, int size) {
          if (ops::gotoOp(op) && size >= ops::labelAt(op) + 32 && cstr(p + ops::labelAt(op), 32) == label) in_file++;
          return true;
        });
      for (const auto& g : ops::gotosIn(c, todayBytes(r.side, chr, r.state, u.kind))) if (g.second == label) here++;
      if (in_file > here)
        ch.done += " " + std::to_string(in_file - here) + (in_file - here == 1 ? " line" : " lines") + " elsewhere in the script also go to a label called " + label + " and aren't changed with it.";
    }
    std::vector<uint8_t> out = ops::applied(u.code, ch.sp);
    if (out.size() < 8 || out.size() > kMaxCode) { fail("Not changed: the result would be " + std::string(out.size() < 8 ? "empty" : "bigger than 1 MB, the most the editor reads or writes for one move or routine") + "."); return; }
    COMBO_LOG("script & boxes: " + chr + " " + r.state + ": " + ch.done + " (" + std::to_string(u.code.size()) + " -> " + std::to_string(out.size()) + " bytes)");
    commitUnit(r, chr, u, std::move(out), ch.done, ch.sp, ents);
  }
  void patchUnit(const SReq& r, const std::string& chr, int at, const std::vector<uint8_t>& bytes, const std::string& done, asw_entity* const ents[2]) {
    UnitCode u;
    std::string why;
    if (!unitNow(r.side, chr, r.state, u, why)) { refuse(r, chr, why); return; }
    if (at < 0 || static_cast<size_t>(at) + bytes.size() > u.code.size()) { refuse(r, chr, ops::kGone); return; }
    std::vector<uint8_t> out = u.code;
    std::memcpy(out.data() + at, bytes.data(), bytes.size());
    if (out == u.code) return;
    if (routineKind(u.kind)) {
      const auto rs = g_rswaps.find(rkeyOf(chr, r.state));
      if (rs != g_rswaps.end() && rs->second.sw.code.size() == out.size()) {
        const std::vector<const uint8_t*> live = loadedStarts(rs->second);
        bool wrote = !live.empty();
        for (const uint8_t* c0 : live)
          if (!writable(c0 + at, bytes.size())) wrote = false;
        if (wrote) {
          for (const uint8_t* c0 : live) std::memcpy(const_cast<uint8_t*>(c0) + at, bytes.data(), bytes.size());
          std::memcpy(rs->second.sw.code.data() + at, bytes.data(), bytes.size());
          const std::string was = rs->second.sw.folder;
          rs->second.sw.folder = "edit:" + (was.rfind("file:", 0) == 0 || was.rfind("edit:", 0) == 0 ? was.substr(5) : std::string());
          rs->second.sw.ver = copyVer(rs->first.first);
          rs->second.note = "changed in the editor";
          g_sel_cache[0] = Sel{};
          g_sel_cache[1] = Sel{};
          g_smsg = done + shapeNote(u.code, out) + roundNote(u.kind, r.state);
          return;
        }
      }
    }
    ops::Splice s;
    s.at = static_cast<size_t>(at);
    s.cut = bytes.size();
    s.add = bytes;
    commitUnit(r, chr, u, std::move(out), done, {s}, ents);
  }
  std::vector<uint8_t> todayBytes(int side, const std::string& chr, const std::string& state, int kind) {
    if (routineKind(kind)) {
      const uint8_t* p = nullptr;
      if (kind == kShared) { const auto& all = sharedRoutines(asw_engine::get()); const auto it = all.find(sharedName(state)); if (it != all.end()) p = it->second; }
      else if (kind == kObjRoutine) p = efStart(chr, objName(state));
      else if (side >= 0 && side < 2) p = stateStart(g_states[side], state);
      return p ? unitBytes(p) : std::vector<uint8_t>();
    }
    const States* st = nullptr;
    if (kind == kObjState) { if (auto it = g_ef.find(chr); it != g_ef.end() && !it->second.files.empty()) st = &it->second.files.front(); }
    else if (side >= 0 && side < 2) st = &g_states[side];
    if (!st || !st->buf || !bbscript::instruction_sizes) return {};
    const std::string name = objName(state);
    const uint32_t count = *reinterpret_cast<const uint32_t*>(st->buf);
    const uint8_t* code = st->buf + 4 + static_cast<size_t>(count) * 36;
    for (uint32_t k = 0; k < count && k < st->names.size(); k++) {
      if (st->names[k] != name) continue;
      const uint32_t* e = reinterpret_cast<const uint32_t*>(st->buf + 4 + k * 36 + 32);
      uint32_t off = *e;
      if (auto sw = g_swaps.find({chr, state}); sw != g_swaps.end()) for (const auto& a : sw->second.at) if (a.entry == e) off = a.orig;
      return unitBytes(code + off);
    }
    return {};
  }
  const std::vector<VerFile>& filesOfKind(const std::string& chr, int kind) {
    if (kind == kShared) return verFiles("CMN", "BBS_CMNEF");
    if (kind == kObjState || kind == kObjRoutine) return verFiles(chr, "BBS_" + chr + "EF");
    return versionFiles(chr);
  }
  std::vector<SideView::VersionView> versionsOf(int side, const std::string& chr, const std::string& state, int kind) {
    std::vector<SideView::VersionView> out;
    const std::vector<VerFile>& files = filesOfKind(chr, kind);
    const std::vector<uint8_t> today = todayBytes(side, chr, state, kind);
    const std::string name = objName(sharedName(state));
    std::vector<uint8_t> prev;
    for (const VerFile& f : files) {
      std::vector<uint8_t> b = routineKind(kind) ? routineBytes(f.payload, name) : stateBytes(f.payload, name);
      if (b.empty()) { prev.clear(); continue; }
      if (!out.empty() && b == prev) { out.back().label = out.back().label.substr(0, out.back().label.find('-')) + "-" + verLabel(f.folder); out.back().now = b == today; continue; }
      out.push_back({verLabel(f.folder), f.folder, b == today});
      prev = std::move(b);
    }
    return out;
  }
  void versionRequest(const SReq& r, const std::string& chr, int kind, asw_entity* const ents[2]) {
    const std::string name = objName(sharedName(r.state));
    const bool routine = routineKind(kind);
    const bool file = r.folder.rfind("file:", 0) == 0;
    const std::string stem = file ? r.folder.substr(5) : std::string();
    if (r.folder.empty()) {
      if (routine) {
        for (auto it = g_sedits.begin(); it != g_sedits.end();) {
          if (it->first.chr == chr && it->first.state == r.state) { putBackSEdit(it->second); it = g_sedits.erase(it); }
          else ++it;
        }
        if (auto old = g_rswaps.find(rkeyOf(chr, r.state)); old != g_rswaps.end()) { putBackRoutine(old->second); g_rswaps.erase(old); }
        g_sel_cache[0] = Sel{};
        g_sel_cache[1] = Sel{};
        g_smsg = name + ": the game's own " + kindWord(kind) + " is back." + roundNote(kind, r.state);
      } else putSwap(r.side, chr, r.state, Swap{}, ents, "today's version");
      return;
    }
    std::vector<uint8_t> code;
    std::vector<int> file_lines;
    std::string err;
    if (file) {
      const std::filesystem::path f = unitDir(chr, r.state) / (stem + ".bbs");
      std::ifstream in(f, std::ios::binary);
      std::stringstream ss;
      ss << in.rdbuf();
      if (!in) err = "couldn't read " + stem + ".bbs";
      else if (assemble(ss.str(), name, routine, code, err, &file_lines) && !routine && cstr(code.data() + 4, 32) != name) err = "the file is " + cstr(code.data() + 4, 32) + ", not " + name;
    } else {
      for (const VerFile& f : filesOfKind(chr, kind)) if (f.folder == r.folder) code = routine ? routineBytes(f.payload, name) : stateBytes(f.payload, name);
      if (code.empty()) err = "version " + verLabel(r.folder) + " has no " + kindWord(kind) + " " + name + " (or its script couldn't be read)";
    }
    if (!err.empty()) {
      refuse(r, chr, (file ? stem + ".bbs: " : std::string()) + err);
      return;
    }
    const std::string warn = shapeNote(todayBytes(r.side, chr, r.state, kind), code, file ? &file_lines : nullptr);
    const std::string from = file ? stem + ".bbs" : "version " + verLabel(r.folder);
    if (!routine) {
      Swap sw;
      sw.folder = r.folder;
      sw.code = std::move(code);
      sw.ver = copyVer(chr);
      PutReq pr;
      pr.req = &r;
      pr.head = from + ": " + name + " couldn't be put on: ";
      const bool ok = putSwap(r.side, chr, r.state, std::move(sw), ents, from, pr);
      const auto now_sw = g_swaps.find({chr, r.state});
      const bool on = now_sw != g_swaps.end() && !now_sw->second.at.empty();
      if (!ok) refuse(r, chr, from + ": " + name + " couldn't be put on: " + (g_swap_no.empty() ? std::string("see combo_log.txt.") : g_swap_no));
      else if (kind == kObjState && on) g_smsg = "Loaded " + from + " as the object state " + name + ". Objects already out keep the old code: the next one put out runs it." + warn;
      else if (kind == kObjState && (g_ef.find(chr) == g_ef.end() || g_ef[chr].files.empty())) g_smsg = "Loaded " + from + " as the object state " + name + ". " + kEfWait + warn;
      else if (!on) g_smsg = "Loaded " + from + " as " + name + ". It goes on once memory near the script is found." + warn;
      else g_smsg = "Loaded " + from + " as " + name + "." + warn;
      return;
    }
    std::string why;
    const int placed = putRSwap(rkeyOf(chr, r.state), std::move(code), r.folder, "loaded from " + from, ents, why);
    if (placed < 0) refuse(r, chr, from + ": " + name + " couldn't be put on: " + why);
    else {
      std::string wait;
      if (placed == 0) wait = kind == kObjRoutine && (g_ef.find(chr) == g_ef.end() || g_ef[chr].files.empty()) ? std::string(" ") + kEfWait
                              : kind == kShared && !(ents[0] && ents[1]) ? std::string(" It goes on when a fight is loaded.")
                                                                         : std::string(" It goes on once memory near the script is found.");
      g_smsg = "Loaded " + from + " as the " + kindWord(kind) + " " + name + "." + wait + roundNote(kind, r.state) + warn;
    }
  }
  void handleUnit(const SReq& r, const std::string& chr, asw_entity* const ents[2]) {
    if (r.side < 0 || r.side > 1 || !ents[r.side]) return;
    if (!bbscript::scriptReaderAvailable() || !bbscript::instruction_sizes) return;
    for (int i = 0; i < 2; i++)
      if (ents[i] && !g_states[i].buf) readStates(ents[i], g_states[i]);
    const int kind = unitKind(r.side, chr, r.state);
    if (g_last_paste.chr == chr && g_last_paste.state == r.state) g_last_paste = LastPaste{};
    if (r.t == SReq::Insert || r.t == SReq::Copy || r.t == SReq::Remove || r.t == SReq::Paste) { lineEdit(r, chr, ents); return; }
    if (r.t == SReq::Version) { versionRequest(r, chr, kind, ents); return; }
    if (r.t == SReq::Reset) {
      for (auto it = g_swaps.begin(); it != g_swaps.end();) {
        if (it->first.first == chr && it->first.second == r.state) { putBackSwap(it->second); it = g_swaps.erase(it); g_states[0] = States{}; g_states[1] = States{}; }
        else ++it;
      }
      bool routine_back = false;
      if (auto rs = g_rswaps.find(rkeyOf(chr, r.state)); rs != g_rswaps.end()) { putBackRoutine(rs->second); g_rswaps.erase(rs); routine_back = true; }
      for (auto it = g_sedits.begin(); it != g_sedits.end();) {
        if (it->first.chr == chr && it->first.state == r.state) { putBackSEdit(it->second); it = g_sedits.erase(it); }
        else ++it;
      }
      g_sel_cache[0] = Sel{};
      g_sel_cache[1] = Sel{};
      if (routine_back) g_smsg = objName(sharedName(r.state)) + ": the game's own " + kindWord(kind) + " is back." + roundNote(kind, r.state);
      else g_sclear = true;
      COMBO_LOG("script & boxes: " + chr + " " + r.state + ": script edits reset");
      return;
    }
    if (r.t == SReq::ResetLine) {
      bool any = false;
      const bool line_copy = startOnCopy(r.side, chr, r.state, moveStart(r.side, chr, r.state));
      for (auto it = g_sedits.lower_bound(SKey{chr, r.state, r.offset, 0});
           it != g_sedits.end() && it->first.chr == chr && it->first.state == r.state && it->first.offset == r.offset;) {
        if (it->first.copy != line_copy) { ++it; continue; }
        putBackSEdit(it->second);
        it = g_sedits.erase(it);
        any = true;
      }
      if (!any) return;
      g_sel_cache[r.side] = Sel{};
      COMBO_LOG("script & boxes: " + chr + " " + r.state + " +" + std::to_string(r.offset) + ": line reset");
      return;
    }
    const uint8_t* start = moveStart(r.side, chr, r.state);
    if (!start) {
      UnitCode u;
      std::string why;
      if (!unitNow(r.side, chr, r.state, u, why)) refuse(r, chr, why);
      return;
    }
    if (!lineStartAt(start, r.offset)) { refuse(r, chr, ops::kGone); return; }
    const uint8_t* line = start + r.offset;
    if (!readable(line, 4)) return;
    const int op = i32(line);
    if (op < 0 || op >= static_cast<int>(bbscript::opcode::MAX) || !bbscript::instruction_sizes) return;
    const int size = bbscript::instruction_sizes[op];
    if (size < 4 || !readable(line, static_cast<size_t>(size))) return;
    const bool shared = kind == kShared;
    const bool on_copy = startOnCopy(r.side, chr, r.state, start);
    if (r.t == SReq::Replace) {
      const SKey rk{chr, r.state, r.offset, 0, on_copy};
      auto rep = g_sedits.find(rk);
      const int was = rep != g_sedits.end() ? rep->second.op : op;
      const bool in_place = !shared && replaceable(was, false) && (r.value == was || (replaceable(r.value, true) && r.value >= 0 && r.value < static_cast<int>(bbscript::opcode::MAX) && bbscript::instruction_sizes[r.value] == size));
      if (!in_place) {
        if (r.value == op) return;
        if (rep != g_sedits.end()) { putBackSEdit(rep->second); g_sedits.erase(rep); }
        lineEdit(r, chr, ents);
        return;
      }
      for (auto it = g_sedits.lower_bound(SKey{chr, r.state, r.offset, 1});
           it != g_sedits.end() && it->first.chr == chr && it->first.state == r.state && it->first.offset == r.offset;) {
        if (it->first.copy != on_copy) { ++it; continue; }
        putBackSEdit(it->second);
        it = g_sedits.erase(it);
      }
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
      g_smsg = std::string(r.value != was ? "Replaced " + ops::nameOf(ctx(), was) + " with " + ops::nameOf(ctx(), r.value) + "." : ops::nameOf(ctx(), was) + " is back.") + roundNote(kind, r.state);
      COMBO_LOG("script & boxes: " + chr + " " + r.state + " +" + std::to_string(r.offset) + ": " + ops::nameOf(ctx(), was) + (r.value != was ? " replaced by " + ops::nameOf(ctx(), r.value) : " put back"));
      return;
    }
    if (r.t != SReq::Arg && r.t != SReq::Name && r.t != SReq::Tag && size < 36) return;
    SKey k{chr, r.state, r.offset, 0, on_copy};
    std::vector<uint8_t> now;
    std::string done;
    bool loud = false;
    if (r.t == SReq::Arg) {
      const int at = ops::argAt(ctx(), op, r.field);
      if (at != 0 && at != 1) return;
      k.field = r.field;
      const int32_t v = op == 2 && r.field == 36 ? std::clamp(r.value, 1, 999) : r.value;
      now.resize(4);
      std::memcpy(now.data(), &v, 4);
      done = ops::nameOf(ctx(), op) + ": value set to " + std::to_string(v) + ".";
    } else if (r.t == SReq::Tag) {
      if (ops::argAt(ctx(), op, r.field - 4) != 2 || (r.value != 0 && r.value != 2)) return;
      k.field = r.field - 4;
      now.resize(4);
      std::memcpy(now.data(), &r.value, 4);
      done = ops::nameOf(ctx(), op) + (r.value == 2 ? ": that value reads a variable now." : ": that value is a plain number now.");
    } else if (r.t == SReq::Frames) {
      if (op != 2 || size < 40) return;
      k.field = 36;
      const int32_t f = std::clamp(r.value, 1, 999);
      now.resize(4);
      std::memcpy(now.data(), &f, 4);
      done = "Sprite frames set to " + std::to_string(f) + ".";
    } else if (r.t == SReq::Name) {
      if (ops::argAt(ctx(), op, r.field) != 3) return;
      int need = 0, fat = 4;
      if (const Instr* d = ops::def(ctx(), op))
        for (const ArgDef& a : d->args) {
          const int n = ops::argBytes(a.kind);
          if (fat + n > size) break;
          if (fat == r.field) need = n;
          fat += n;
        }
      if (!need) return;
      std::string odd;
      if (r.offset == 0 && ops::unitBegin(op))
        odd += " This changes only this line's text: the game finds moves and routines by the name in its own table, built when the file loads, so it keeps its name there.";
      if (r.text.empty()) odd += " The name is empty: the game may ignore this line, crash or freeze.";
      if (static_cast<int>(r.text.size()) >= need) {
        refuse(r, chr, "\"" + r.text + "\" is " + std::to_string(r.text.size()) + " letters long, and this name holds " + std::to_string(need - 1) + " at most, so it doesn't fit.");
        return;
      }
      for (char c : r.text)
        if (c <= ' ' || c > '~' || c == '\'' || c == '\\') { odd += " \"" + r.text + "\" has a space, accent, ' or \\, which no name in the game's files has: the game may ignore this line, crash or freeze."; break; }
      if (op == 11) {
        SReq rn = r;
        lineEdit(rn, chr, ents);
        if (!odd.empty() && g_smsg.rfind("Renamed", 0) == 0) g_smsg += odd;
        return;
      }
      auto prev = g_sedits.find(SKey{chr, r.state, r.offset, r.field, on_copy});
      const std::string game_name = prev != g_sedits.end() && prev->second.orig.size() == static_cast<size_t>(need) ? cstr(prev->second.orig.data(), static_cast<size_t>(need))
                                                                                                                  : cstr(line + r.field, static_cast<size_t>(need));
      std::string why, unknown;
      if (r.text != game_name && !knownName(r.side, chr, r.state, op, r.field, need, r.text, ents, why)) {
        bool here = false;
        if (ops::gotoOp(op)) { UnitCode u; std::string w2; if (unitNow(r.side, chr, r.state, u, w2)) for (const auto& l : ops::labelsIn(ctx(), u.code)) if (l.second == r.text) here = true; }
        if (!here) unknown = " " + why + " It is set anyway: the game may ignore this line, crash or freeze.";
      }
      k.field = r.field;
      now.assign(static_cast<size_t>(need), 0);
      std::memcpy(now.data(), r.text.data(), r.text.size());
      done = ops::nameOf(ctx(), op) + ": name set to " + r.text + "." + odd + unknown;
      loud = !odd.empty() || !unknown.empty();
    } else {
      if ((op != 17 && op != 75) || cstr(line + 4, 32).rfind("cmn_AtkLv", 0) != 0) return;
      k.field = 4;
      char name[32] = {};
      snprintf(name, sizeof(name), "cmn_AtkLv%d", std::clamp(r.value, 0, 4));
      now.assign(name, name + 32);
      done = std::string("Attack level set to ") + name + ".";
    }
    if (loud) COMBO_LOG("script & boxes: warned " + chr + " " + r.state + " (" + reqWords(r) + "): " + done);
    if (shared) { patchUnit(r, chr, r.offset + k.field, now, done, ents); return; }
    if (k.field > 0) {
      auto rep = g_sedits.find(SKey{chr, r.state, r.offset, 0, on_copy});
      if (rep != g_sedits.end()) {
        SEdit& ed = rep->second;
        if (i32(line) != i32(ed.now.data()) || k.field + now.size() > ed.now.size()) return;
        putBackSEdit(ed);
        std::memcpy(ed.now.data() + k.field, now.data(), now.size());
        applySEdit(rep->first, ed, ents);
        g_sel_cache[r.side] = Sel{};
        if (loud || !roundNote(kind, r.state).empty()) g_smsg = done + roundNote(kind, r.state);
        COMBO_LOG("script & boxes: " + chr + " " + r.state + " +" + std::to_string(r.offset) + ": (replaced line) value at +" + std::to_string(r.field) + " = " + std::to_string(r.value));
        return;
      }
    }
    auto it = g_sedits.find(k);
    if (it == g_sedits.end()) {
      SEdit ed;
      ed.op = op;
      ed.field = k.field;
      if (op == 2 && k.field != 4) {
        auto named = g_sedits.find(SKey{chr, r.state, r.offset, 4, on_copy});
        if (named != g_sedits.end() && named->second.orig.size() == 32) ed.sig = named->second.orig;
        else ed.sig.assign(line + 4, line + 36);
      }
      ed.orig.assign(line + k.field, line + k.field + now.size());
      it = g_sedits.emplace(k, std::move(ed)).first;
    } else putBackSEdit(it->second);
    it->second.now = now;
    if (it->second.now == it->second.orig) g_sedits.erase(it);
    else applySEdit(k, it->second, ents);
    g_sel_cache[r.side] = Sel{};
    g_sclear = true;
    if (loud || !roundNote(kind, r.state).empty()) { g_sclear = false; g_smsg = done + roundNote(kind, r.state); }
    if (copyOff(chr, r.state)) {
      g_sclear = false;
      g_smsg = done + (swapHasCode(chr, r.state) && !on_copy ? " Your changed copy of this " + std::string(kindWord(kind)) + " isn't on, so this changes the game's own " + kindWord(kind) + ", not the copy."
                                                              : " Your changed copy of this " + std::string(kindWord(kind)) + " isn't on yet, so this goes on together with it.");
    }
    COMBO_LOG("script & boxes: " + chr + " " + r.state + " +" + std::to_string(r.offset) + ": " + done);
  }
  void handleS(const SReq& r, asw_entity* const ents[2]) {
    if (r.side < 0 || r.side > 1 || !ents[r.side]) return;
    if (!isCmnObj(r.state)) { handleUnit(r, sideChr(ents[r.side]), ents); return; }
    SReq u = r;
    u.state = unitOf(std::string(), r.state).state;
    handleUnit(u, "CMN", ents);
  }
  std::vector<std::string> g_today_live;
  std::vector<std::string> g_ver_said;
  std::string g_ver_note;
  std::map<std::string, int> g_files_live;
  int filesState(const std::string& chr) {
    if (!combo_trials::trainingOld()) return 1;
    if (chr == "CMN" || !combo_trials::fileNamesSeen()) return 0;
    const auto it = g_files_live.find(chr);
    if (it != g_files_live.end()) return it->second;
    return combo_trials::runsToday(chr) ? 1 : -1;
  }
  bool todayFiles(const std::string& chr) { return chr != "CMN" && combo_trials::trainingOld() && filesState(chr) == 1; }
  int copyVer(const std::string& chr) {
    if (!combo_trials::trainingOld()) return 0;
    const int f = filesState(chr);
    return f == 1 ? 0 : f == 0 ? combo_trials::battleVersion() : -1;
  }
  std::string verWords(int v) { return v > 0 ? battleVersionName(v) : std::string("the current version"); }
  bool copyHeld(const std::string& chr, const std::string& name, Swap& sw) {
    sw.held = false;
    if (sw.ver < 0 || (sw.folder.rfind("file:", 0) != 0 && sw.folder.rfind("edit:", 0) != 0)) return false;
    const int now = copyVer(chr);
    if (now < 0 || sw.ver == now || todayFiles(chr)) return false;
    sw.held = true;
    sw.no = "changed on " + verWords(sw.ver) + ", so it isn't put on " + verWords(now) + ".";
    const std::string key = chr + "/" + name;
    if (std::find(g_ver_said.begin(), g_ver_said.end(), key) == g_ver_said.end()) {
      g_ver_said.push_back(key);
      g_ver_note += (g_ver_note.empty() ? "" : " ") + objName(sharedName(name)) + ": " + sw.no;
      COMBO_LOG("script & boxes: " + chr + " " + name + ": " + sw.no + " The game's own stays in this fight, and the copy is kept.");
    }
    return true;
  }
  std::string g_old_script_note;
  std::vector<std::string> editedChars() {
    std::vector<std::string> out;
    const auto add = [&](const std::string& c) {
      if (c.size() == 3 && c != "CMN" && std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
    };
    for (const auto& kv : g_edits) add(kv.first.chr);
    for (const auto& kv : g_sedits) add(kv.first.chr);
    for (const auto& kv : g_swaps) add(kv.first.first);
    for (const auto& kv : g_rswaps) add(kv.first.first);
    return out;
  }
  void keepTodayTick(asw_entity* const ents[2]) {
    const std::vector<std::string> ed = editedChars();
    combo_trials::setEditedChars(ed);
    g_old_script_note.clear();
    if (!combo_trials::trainingOld() || !combo_trials::keepTodayOn()) return;
    for (int i = 0; i < 2; i++) {
      const std::string chr = sideChr(ents[i]);
      if (chr.empty() || std::find(ed.begin(), ed.end(), chr) == ed.end() || std::find(g_today_live.begin(), g_today_live.end(), chr) != g_today_live.end()) continue;
      g_old_script_note = chr + " runs on the old version's own script in this fight, so some edits don't fit. Reload the fight: with edits loaded it keeps today's script.";
      return;
    }
  }
  constexpr int kOpEndIf = 5, kOpIfOperation = 6, kOpTensionOnConnect = 1117, kOpRiscGain = 1122;
  const char* const kRiscRoutines[] = {"cmn_AtkLv1", "cmn_AtkLv2", "cmn_AtkLv3", "cmn_AtkLv4", "cmn_DustAttack", "cmnAtk_GB_TGG"};
  const char* const kRiscTable = "cmnAtk_GB_TGG";
  std::vector<size_t> linesOf(const std::vector<uint8_t>& c, int want) {
    std::vector<size_t> at;
    if (!bbscript::instruction_sizes) return at;
    for (size_t p = 0; p + 4 <= c.size();) {
      const int op = i32(c.data() + p);
      if (op < 0 || op >= static_cast<int>(bbscript::opcode::MAX)) break;
      const int size = bbscript::instruction_sizes[op];
      if (size < 4 || p + static_cast<size_t>(size) > c.size()) break;
      if (op == want && size >= 8) at.push_back(p);
      p += static_cast<size_t>(size);
    }
    return at;
  }
  std::vector<uint8_t> riscRoutine(const std::string& name, const std::vector<uint8_t>& now, const std::vector<uint8_t>& old) {
    std::vector<uint8_t> out;
    if (!bbscript::instruction_sizes) return out;
    if (name == kRiscTable) {
      out = old;
      int next = 0;
      int32_t at5 = 0;
      bool have5 = false;
      for (size_t p = 0; p + 4 <= out.size();) {
        const int op = i32(out.data() + p);
        if (op < 0 || op >= static_cast<int>(bbscript::opcode::MAX)) return {};
        const int size = bbscript::instruction_sizes[op];
        if (size < 4 || p + static_cast<size_t>(size) > out.size()) return {};
        if (op == kOpIfOperation && size >= 24) {
          const uint8_t* q = out.data() + p;
          const bool tension = i32(q + 4) == 12 && i32(q + 8) == 2 && i32(q + 12) == 330 && i32(q + 16) == 0;
          next = tension && i32(q + 20) == 5 ? 5 : tension && i32(q + 20) == 10 ? 10 : 0;
          if (next != 10) have5 = false;
        } else if (op == kOpTensionOnConnect && size >= 8) {
          if (next == 5) { at5 = i32(out.data() + p + 4); have5 = true; }
          else if (next == 10 && have5) std::memcpy(out.data() + p + 4, &at5, 4);
          else if (next == 10) return {};
          next = 0;
        } else if (op == kOpEndIf) {
          next = 0;
        }
        p += static_cast<size_t>(size);
        if (p == out.size()) return out;
      }
      return {};
    }
    const std::vector<size_t> a = linesOf(now, kOpRiscGain), b = linesOf(old, kOpRiscGain);
    if (a.empty() || a.size() != b.size()) return out;
    out = now;
    for (size_t i = 0; i < a.size(); i++) std::memcpy(out.data() + a[i] + 4, old.data() + b[i] + 4, 4);
    return out;
  }
  std::vector<uint8_t> liveRoutine(const CommonNow& c, const std::string& name) {
    const uint8_t* start = nullptr;
    const uint8_t* end = nullptr;
    walkCode(c.code, [&](const uint8_t* q, int op, int size) {
      if (!start) {
        if (op == 15 && size >= 36 && cstr(q + 4, 32) == name) start = q;
        return true;
      }
      if (op == 16) end = q + size;
      return op != 0 && op != 1 && op != 15 && op != 16;
    });
    if (!start || !end || end - start > 0x40000) return {};
    return std::vector<uint8_t>(start, end);
  }
  void syncRiscRoutines(const asw_engine* engine, asw_entity* const ents[2], bool want) {
    const bool fight = ents[0] && ents[1];
    if (!want) {
      if (!g_risc_rswaps.empty()) putBackRiscRoutines();
      return;
    }
    if (!engine || !fight) return;
    CommonNow c;
    std::string why;
    if (!commonNow(engine, c, why)) return;
    if (c.file != g_risc_file) {
      putBackRiscRoutines();
      g_risc_file = c.file;
    }
    for (const char* n : kRiscRoutines) {
      const std::string name = n;
      RSwap& r = g_risc_rswaps[name];
      if (g_rswaps.count({"CMN", name})) {
        if (!r.sw.at.empty()) putBackRoutine(r);
        continue;
      }
      if (!r.sw.at.empty()) {
        if (rswapLive(r)) continue;
        r = RSwap{};
      }
      if (r.dead) continue;
      if (r.sw.code.empty()) {
        std::vector<uint8_t> old;
        std::string bad = loadRoutine("CMN", name, "406", old);
        const std::vector<uint8_t> now = bad.empty() ? liveRoutine(c, name) : std::vector<uint8_t>();
        if (bad.empty() && now.empty()) bad = "today's common script has no routine of that name";
        if (bad.empty()) {
          r.sw.code = riscRoutine(name, now, old);
          if (r.sw.code.empty()) bad = "its RISC lines don't line up with 4.06's";
          else if (r.sw.code == now) bad = "it already has those values";
          else routineFormed(r.sw.code, bad);
        }
        if (!bad.empty()) {
          r.sw.code.clear();
          r.dead = true;
          COMBO_LOG("battle rules: the 4.06 RISC values aren't put on " + name + ": " + bad);
          continue;
        }
        r.sw.folder = "406";
        r.note = name == kRiscTable ? "the 4.06 routine with today's tension values" : "today's routine with the 4.06 RISC gain";
      }
      std::string no;
      if (applyRSwap({"CMN", name}, r, ents, engine, no)) continue;
      if (no.empty() && ++r.tries < 120) continue;
      r.dead = true;
      COMBO_LOG("battle rules: the 4.06 RISC values aren't put on " + name + ": " + (no.empty() ? std::string("no memory near the common script") : no));
    }
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
      for (auto& [k, sw] : g_swaps)
        if (isObj(k.second) && sw.at.empty() && inFight(k.first, ents)) wantEf(k.first, ents);
      for (auto& [k, sw] : g_swaps) applySwap(k.first, k.second, sw, ents);
      for (int i = 0; i < 2; i++) if (ents[i]) readStates(ents[i], g_states[i]);
    }
    for (auto& [k, ed] : g_sedits) {
      if (isObj(k.state)) {
        if (!inFight(k.chr, ents)) continue;
        wantEf(k.chr, ents);
      }
      applySEdit(k, ed, ents);
    }
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
    const std::filesystem::path dst = editsDir() / (name + ".txt");
    std::filesystem::path tmp = dst;
    tmp += ".new";
    std::ofstream f(tmp, std::ios::trunc);
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
      if (sw.folder.rfind("file:", 0) == 0 || sw.folder.rfind("edit:", 0) == 0) f << "code\t" << k.first << "\t" << k.second << "\t" << sw.folder << "\t" << hexOf(sw.code) << "\t" << ops::flagsText(sw.added) << "\t" << sw.ver << "\n";  // (a file's move: its bytes)
      else f << "version\t" << k.first << "\t" << k.second << "\t" << sw.folder << "\n";
      nv++;
    }
    int nr = 0;
    for (const auto& [k, r] : g_rswaps) {
      f << "routine\t" << k.first << "\t" << k.second << "\t" << r.sw.folder;
      if (r.sw.folder.rfind("file:", 0) == 0 || r.sw.folder.rfind("edit:", 0) == 0) f << "\t" << hexOf(r.sw.code) << "\t" << ops::flagsText(r.sw.added) << "\t" << r.sw.ver;
      f << "\n";
      nr++;
    }
    for (const auto& [k, ed] : g_sedits) {
      f << "script\t" << k.chr << "\t" << k.state << "\t" << k.offset << "\t" << k.field << "\t" << ed.op << "\t" << (ed.sig.empty() ? "-" : hexOf(ed.sig))
        << "\t" << hexOf(ed.orig) << "\t" << hexOf(ed.now) << "\t" << (k.copy ? 1 : 0) << "\n";
      ns++;
    }
    f.flush();
    if (!f.good()) return "Couldn't write " + name + ".txt";
    f.close();
    if (f.fail()) return "Couldn't write " + name + ".txt";
    std::filesystem::rename(tmp, dst, ec);
    if (ec) return "Couldn't write " + name + ".txt";
    const std::string msg = "Saved " + name + ": " + std::to_string(nb) + " boxes, " + std::to_string(ns) + " script lines, " + std::to_string(nv) + " versions" +
                            (nr ? ", " + std::to_string(nr) + (nr == 1 ? " shared routine" : " shared routines") : std::string());
    COMBO_LOG("script & boxes: " + msg);
    return msg;
  }
  std::string ioLoad(const std::string& name, asw_entity* const ents[2]) {
    std::ifstream f(editsDir() / (name + ".txt"));
    if (!f) return "Couldn't read " + name + ".txt";
    int nb = 0, ns = 0, nv = 0, bad = 0, off = 0;
    std::vector<std::vector<std::string>> rows;
    std::string l;
    while (std::getline(f, l)) {
      if (!l.empty() && l.back() == '\r') l.pop_back();
      if (l.empty() || l[0] == '#') continue;
      std::vector<std::string> c;
      for (size_t a = 0;;) { const size_t b = l.find('\t', a); c.push_back(l.substr(a, b == std::string::npos ? std::string::npos : b - a)); if (b == std::string::npos) break; a = b + 1; }
      rows.push_back(std::move(c));
    }
    std::vector<std::string> chrs;
    std::vector<std::pair<std::string, std::string>> named;
    for (const std::vector<std::string>& c : rows) {
      if (c.size() < 3 || (c[0] != "box" && c[0] != "script" && c[0] != "version" && c[0] != "code")) continue;
      chrs.push_back(c[1]);
      if (c[0] == "version" || c[0] == "code") named.push_back({c[1], c[2]});
    }
    for (auto it = g_swaps.begin(); it != g_swaps.end();) {
      if (std::find(chrs.begin(), chrs.end(), it->first.first) == chrs.end() || std::find(named.begin(), named.end(), it->first) != named.end()) { ++it; continue; }
      for (auto e = g_sedits.begin(); e != g_sedits.end();) {
        if (e->first.chr == it->first.first && e->first.state == it->first.second) { putBackSEdit(e->second); e = g_sedits.erase(e); }
        else ++e;
      }
      putBackSwap(it->second);
      COMBO_LOG("script & boxes: " + it->first.first + " " + it->first.second + ": " + name + " doesn't name a version for it, back to the game's own");
      it = g_swaps.erase(it);
      off++;
    }
    for (const std::vector<std::string>& c : rows) {
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
          if (c.size() >= 6) sw.added = ops::flagsOf(c[5]);
          if (c.size() >= 7) sw.ver = std::atoi(c[6].c_str());
          if (sw.code.size() < 40 || i32(sw.code.data()) != 0 || cstr(sw.code.data() + 4, 32) != objName(c[2]) || !codeWalks(sw.code)) sw.code.clear();
        } else {
          for (const VerFile& vf : verFiles(c[1], "BBS_" + c[1] + (isObj(c[2]) ? "EF" : ""))) if (vf.folder == c[3]) sw.code = stateBytes(vf.payload, objName(c[2]));
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
        const SKey k{c[1], c[2], std::atoi(c[3].c_str()), ed.field, c.size() >= 10 ? c[9] == "1" : swapHasCode(c[1], c[2])};
        if (k.offset < 0 || ed.field < 0) { bad++; continue; }
        if (ed.field == 0) {  // (a replaced line: only a replace the UI allows, both instructions the saved size)
          const int to = ed.now.size() >= 4 ? i32(ed.now.data()) : -1;
          if (!replaceable(ed.op, false) || (to != ed.op && !replaceable(to, true)) || bbscript::instruction_sizes[ed.op] != static_cast<int>(ed.orig.size()) ||
              bbscript::instruction_sizes[to] != static_cast<int>(ed.now.size())) { bad++; continue; }
        }
        if (ed.field == 0)  // (a replaced line: the line's other edits go, as when replacing in the UI)
          for (auto it = g_sedits.lower_bound(SKey{k.chr, k.state, k.offset, 1}); it != g_sedits.end() && it->first.chr == k.chr && it->first.state == k.state && it->first.offset == k.offset;) {
            if (it->first.copy != k.copy) { ++it; continue; }
            putBackSEdit(it->second); it = g_sedits.erase(it);
          }
        auto old = g_sedits.find(k);
        if (old != g_sedits.end()) putBackSEdit(old->second);
        g_sedits[k] = std::move(ed);
        ns++;
      } else if (c[0] == "routine" && c.size() >= 4) {
      } else bad++;
    }
    int nr = 0;
    {
      std::vector<std::pair<std::string, std::string>> named_routines;
      for (const std::vector<std::string>& c : rows) if (c[0] == "routine" && c.size() >= 4) named_routines.push_back({c[1], c[2]});
      for (const auto& nr_key : named_routines) chrs.push_back(nr_key.first);
      for (auto it = g_rswaps.begin(); it != g_rswaps.end();) {
        if (std::find(named_routines.begin(), named_routines.end(), it->first) != named_routines.end() ||
            (it->first.first != "CMN" && std::find(chrs.begin(), chrs.end(), it->first.first) == chrs.end())) { ++it; continue; }
        putBackRoutine(it->second);
        COMBO_LOG("script & boxes: routine " + it->first.second + " (" + it->first.first + "): " + name + " doesn't name a version for it, back to the game's own");
        it = g_rswaps.erase(it);
      }
      for (const std::vector<std::string>& c : rows) {
        if (c[0] != "routine" || c.size() < 4) continue;
        RSwap r;
        r.sw.folder = c[3];
        std::string err;
        if (c[3].rfind("file:", 0) == 0 || c[3].rfind("edit:", 0) == 0) {
          int want_op = 15;
          const std::string base = ops::baseName(objName(c[2]), want_op);
          if (c.size() < 5) err = "its line in the edits file isn't complete";
          else {
            r.sw.code = bytesOf(c[4]);
            if (c.size() >= 6) r.sw.added = ops::flagsOf(c[5]);
            if (c.size() >= 7) r.sw.ver = std::atoi(c[6].c_str());
            r.note = c[3].rfind("file:", 0) == 0 ? "loaded from " + c[3].substr(5) + ".bbs (kept in the edits file)" : std::string("changed in the editor (kept in the edits file)");
            if (r.sw.code.size() < 40 || i32(r.sw.code.data()) != want_op || cstr(r.sw.code.data() + 4, 32) != base) err = "its bytes in the edits file aren't a routine called " + base;
            else if (!codeWalks(r.sw.code)) err = "its bytes in the edits file are cut off or damaged";
          }
        } else err = loadRoutine(c[1], c[2], c[3], r.sw.code);
        if (!err.empty()) { bad++; COMBO_LOG("script & boxes: routine " + c[2] + " (" + c[1] + "): " + err); continue; }
        auto old = g_rswaps.find({c[1], c[2]});
        if (old != g_rswaps.end()) {
          if (old->second.sw.folder == r.sw.folder && old->second.sw.code == r.sw.code) { old->second.dead = false; old->second.tries = 0; old->second.sw.ver = r.sw.ver; nr++; continue; }
          putBackRoutine(old->second);
        }
        g_rswaps[{c[1], c[2]}] = std::move(r);
        nr++;
      }
      syncRoutines(asw_engine::get(), ents);
    }
    g_states[0] = States{};  // (versions first, then the starts read again, then the lines: as after a new fight)
    g_states[1] = States{};
    for (int i = 0; i < 2; i++) if (ents[i]) readStates(ents[i], g_states[i]);
    linkSigs();
    syncScripts(ents);
    g_sel_cache[0] = Sel{};
    g_sel_cache[1] = Sel{};
    const std::string msg = "Loaded " + name + ": " + std::to_string(nb) + " boxes, " + std::to_string(ns) + " script lines, " + std::to_string(nv) + " versions" +
                            (nr ? ", " + std::to_string(nr) + (nr == 1 ? " shared routine" : " shared routines") : std::string()) +
                            (bad ? " (" + std::to_string(bad) + " lines not understood)" : std::string()) +
                            (off ? ", " + std::to_string(off) + " earlier " + (off == 1 ? "version" : "versions") + " taken off" : std::string()) +
                            ". Each goes on only where the game's value still matches.";
    COMBO_LOG("script & boxes: " + msg);
    return msg;
  }

  std::string exportScriptPak(int side, asw_entity* const ents[2]) {
    if (side < 0 || side > 1 || !ents[side]) return "No fighter to export.";
    if (!pak_mount::found()) return "Can't export: the game's pak mount wasn't found (see combo_log.txt).";
    if (!bbscript::scriptReaderAvailable() || !bbscript::instruction_sizes) return "Can't export: the game's script reader wasn't found.";
    const std::string chr = sideChr(ents[side]);
    if (chr.empty()) return "No fighter to export.";
    struct File { std::string folder, base; std::vector<script_pak::Block> swaps; std::vector<script_pak::LineEdit> edits; };
    File files[3] = {{chr, "BBS_" + chr, {}, {}}, {chr, "BBS_" + chr + "EF", {}, {}}, {"CMN", "BBS_CMNEF", {}, {}}};
    for (const auto& [k, sw] : g_swaps) {
      if (k.first == chr && !sw.code.empty()) files[isObj(k.second) ? 1 : 0].swaps.push_back({0, objName(k.second), sw.code});
      else if (k.first == "CMN" && chr != "CMN" && isObj(k.second) && !sw.code.empty()) files[2].swaps.push_back({0, objName(k.second), sw.code});
    }
    int other_layout = 0, no_copy = 0;
    for (const auto& [k, ed] : g_sedits) {
      const bool common_obj = k.chr == "CMN" && chr != "CMN" && isObj(k.state);
      if (k.chr != chr && !common_obj) continue;
      if (k.copy != swapHasCode(k.chr, k.state)) { (k.copy ? no_copy : other_layout)++; continue; }
      script_pak::LineEdit e;
      e.name = objName(k.state);
      e.offset = k.offset;
      e.field = k.field;
      e.op = ed.op;
      e.sig = ed.sig;
      e.sig_alt = ed.sig_alt;
      e.orig = ed.orig;
      e.now = ed.now;
      files[common_obj ? 2 : isObj(k.state) ? 1 : 0].edits.push_back(std::move(e));
    }
    int prepost = 0;
    for (const auto& [k, r] : g_rswaps) {
      if (r.sw.code.size() < 40 || (k.first != "CMN" && k.first != chr)) continue;
      int op = 15;
      const std::string base = ops::baseName(objName(k.second), op);
      if (op != 15) { prepost++; continue; }
      if (k.first == "CMN") files[2].swaps.push_back({op, base, r.sw.code});
      else files[isObj(k.second) ? 1 : 0].swaps.push_back({op, base, r.sw.code});
    }
    const std::filesystem::path out = g_dir / "export" / chr;
    std::error_code ec;
    std::filesystem::create_directories(out, ec);
    std::vector<bake::PakFile> pak;
    std::vector<std::string> log;
    int moves = 0, lines = 0, routines = 0, left = 0;
    if (prepost) log.push_back(std::to_string(prepost) + " pre/post routines left out: the pak export only rebuilds moves and plain routines");
    if (other_layout) log.push_back(std::to_string(other_layout) + " value edits left out: they were made on the game's own lines of a move whose changed copy is exported");
    if (no_copy) log.push_back(std::to_string(no_copy) + " value edits left out: they were made on the lines of a changed copy that isn't loaded any more, and the game's own move is exported");
    for (File& f : files) {
      if (f.swaps.empty() && f.edits.empty()) continue;
      const int asked = static_cast<int>(f.swaps.size() + f.edits.size());
      const std::string rel = "Chara/" + f.folder + "/Common/Data/" + f.base;
      const std::wstring w = L"../../../RED/Content/" + std::wstring(rel.begin(), rel.end());
      std::vector<uint8_t> u, a;
      if (!pak_mount::readFile(w + L".uexp", u) || !pak_mount::readFile(w + L".uasset", a)) {
        log.push_back(f.base + ": the game's file couldn't be read, " + std::to_string(asked) + " changes left out");
        left += asked;
        continue;
      }
      script_pak::Result r;
      std::string err;
      if (!script_pak::rebuild(u, a, bbscript::instruction_sizes, static_cast<int>(bbscript::opcode::MAX), f.swaps, f.edits, r, err)) {
        log.push_back(f.base + ": " + err + ", " + std::to_string(asked) + " changes left out");
        left += asked;
        continue;
      }
      for (const std::string& s : r.left_out) log.push_back(f.base + ": left out: " + s);
      left += static_cast<int>(r.left_out.size());
      for (const std::string& s : r.notes) log.push_back(f.base + ": " + s);
      if (r.blocks + r.lines == 0) continue;
      (&f == &files[2] ? routines : moves) += r.blocks;
      lines += r.lines;
      log.push_back(f.base + ": " + std::to_string(r.blocks) + (&f == &files[2] ? " shared routines" : " moves and routines") + " replaced, " + std::to_string(r.lines) + " lines changed, " +
                    std::to_string(u.size()) + " -> " + std::to_string(r.uexp.size()) + " bytes");
      const std::filesystem::path loose = out / "RED" / "Content" / std::filesystem::path(rel).parent_path();
      std::filesystem::create_directories(loose, ec);
      for (const auto& [ext, data] : {std::pair<const char*, const std::vector<uint8_t>*>{".uexp", &r.uexp}, {".uasset", &r.uasset}}) {
        std::ofstream o(loose / (f.base + ext), std::ios::binary | std::ios::trunc);
        o.write(reinterpret_cast<const char*>(data->data()), static_cast<std::streamsize>(data->size()));
      }
      pak.push_back({"RED/Content/" + rel + ".uasset", r.uasset});
      pak.push_back({"RED/Content/" + rel + ".uexp", r.uexp});
    }
    auto writeLog = [&]() {
      std::ofstream o(out / "export_log.txt", std::ios::trunc);
      for (const std::string& l : log) o << l << "\n";
    };
    if (pak.empty()) {
      writeLog();
      for (const std::string& l : log) COMBO_LOG("script & boxes: export pak: " + l);
      return left ? "Nothing exported: " + std::to_string(left) + " changes didn't fit the game's file (see StriveLabs_scripts/export/" + chr + "/export_log.txt)."
                  : "Nothing to export: " + chr + " has no script changes.";
    }
    const std::filesystem::path file = out / (chr + "_script_P.pak");
    std::string why;
    if (!bake::writePak(file.wstring(), pak, why) || !bake::checkPak(file.wstring(), pak, why)) {
      std::filesystem::remove(file, ec);
      COMBO_LOG("script & boxes: export pak: " + why);
      return "The pak couldn't be written: " + why;
    }
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::filesystem::path sig = std::filesystem::path(exe).parent_path().parent_path().parent_path() / "Content" / "Paks" / "pakchunk0-WindowsNoEditor.sig";
    std::filesystem::copy_file(sig, std::filesystem::path(file).replace_extension(".sig"), std::filesystem::copy_options::overwrite_existing, ec);
    const bool no_sig = static_cast<bool>(ec);
    if (no_sig) log.push_back("the game's .sig couldn't be copied: copy pakchunk0-WindowsNoEditor.sig next to the pak as " + chr + "_script_P.sig");
    writeLog();
    for (const std::string& l : log) COMBO_LOG("script & boxes: export pak: " + l);
    return "Exported " + chr + "_script_P.pak to StriveLabs_scripts/export/" + chr + "/: " + std::to_string(moves) + (moves == 1 ? " move or routine, " : " moves and routines, ") + std::to_string(lines) +
           (lines == 1 ? " line" : " lines") + (routines ? ", " + std::to_string(routines) + (routines == 1 ? " shared routine (changes every character)" : " shared routines (change every character)") : std::string()) +
           (left ? ". " + std::to_string(left) + " left out (see export_log.txt)" : std::string()) +
           (prepost ? ". " + std::to_string(prepost) + " pre/post routines aren't in it (the pak export doesn't rebuild those)" : std::string()) +
           (other_layout ? ". " + std::to_string(other_layout) + (other_layout == 1 ? " value edit isn't" : " value edits aren't") + " in it: made on the game's own lines of a move, while the pak has your changed copy of it" : std::string()) +
           (no_copy ? ". " + std::to_string(no_copy) + (no_copy == 1 ? " value edit isn't" : " value edits aren't") + " in it: made on a changed copy that isn't loaded any more, while the pak has the game's own move" : std::string()) +
           (no_sig ? ". The .sig wasn't copied (see export_log.txt)" : std::string()) +
           ". Box edits aren't in it.";
  }

  bool sEdited(const std::string& chr, const std::string& state, int offset, bool copy) {
    for (auto it = g_sedits.lower_bound(SKey{chr, state, offset, 0}); it != g_sedits.end() && it->first.chr == chr && it->first.state == state && it->first.offset == offset; ++it)
      if (it->first.copy == copy) return true;
    return false;
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
        {
          bool old_boxes = gameBoxes(reinterpret_cast<const uint8_t*>(e->hitboxes)) != reinterpret_cast<const uint8_t*>(e->hitboxes);
          if (g_old_live.load(std::memory_order_relaxed))
            for (const std::string& name : r.to) {
              const auto sp = col->sprites.find(name);
              if (name != s.sprite && sp != col->sprites.end() && g_old_map.count(sp->second.rec)) old_boxes = true;
            }
          if (old_boxes) {
            v.status = "Set Hurtboxes When Hit Or Blocking to Current to copy boxes of hit or block sprites: they use the Ver. 4.09 hurtboxes now, so nothing was copied.";
            COMBO_LOG("script & boxes: " + v.status);
            return;
          }
        }
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
      case Req::Undo:
      case Req::Redo:
      case Req::Goto:
        return;
    }
  }

  struct Snap {
    struct Box { BoxView orig, now; std::string label; };
    struct Code { std::string folder, note; std::vector<uint8_t> code; std::vector<char> added; int ver = -1; };
    std::map<Key, Box> boxes;
    std::map<SKey, SEdit> sedits;
    std::map<std::pair<std::string, std::string>, Code> swaps, routines;
  };
  struct HistStep { std::string text, key; ULONGLONG when = 0; Snap after; };
  std::vector<HistStep> g_hist;
  int g_hist_at = 0;
  bool g_hist_dirty = false;
  std::string g_hist_text, g_hist_key;
  Snap takeSnap() {
    Snap s;
    for (const auto& [k, ed] : g_edits)
      if (!sameBox(ed->now, ed->orig)) s.boxes[k] = {ed->orig, ed->now, ed->label};
    for (const auto& [k, ed] : g_sedits) {
      SEdit c = ed;
      c.at.clear();
      c.sig_alt.clear();
      s.sedits[k] = std::move(c);
    }
    for (const auto& [k, sw] : g_swaps) s.swaps[k] = {sw.folder, std::string(), sw.code, sw.added, sw.ver};
    for (const auto& [k, r] : g_rswaps) s.routines[k] = {r.sw.folder, r.note, r.sw.code, r.sw.added, r.sw.ver};
    return s;
  }
  bool sameCodes(const std::map<std::pair<std::string, std::string>, Snap::Code>& a, const std::map<std::pair<std::string, std::string>, Snap::Code>& b) {
    if (a.size() != b.size()) return false;
    for (auto x = a.begin(), y = b.begin(); x != a.end(); ++x, ++y)
      if (x->first != y->first || x->second.folder != y->second.folder || x->second.code != y->second.code) return false;
    return true;
  }
  bool sameSnap(const Snap& a, const Snap& b) {
    if (a.boxes.size() != b.boxes.size() || a.sedits.size() != b.sedits.size()) return false;
    for (auto x = a.boxes.begin(), y = b.boxes.begin(); x != a.boxes.end(); ++x, ++y)
      if (x->first < y->first || y->first < x->first || !sameBox(x->second.now, y->second.now)) return false;
    for (auto x = a.sedits.begin(), y = b.sedits.begin(); x != a.sedits.end(); ++x, ++y)
      if (x->first < y->first || y->first < x->first || x->second.op != y->second.op || x->second.orig != y->second.orig || x->second.now != y->second.now) return false;
    return sameCodes(a.swaps, b.swaps) && sameCodes(a.routines, b.routines);
  }
  void histStart() {
    if (!g_hist.empty()) return;
    g_hist.push_back({"Start", std::string(), 0, takeSnap()});
    g_hist_at = 0;
  }
  void histMark(const std::string& text, const std::string& key = std::string()) {
    g_hist_dirty = true;
    g_hist_text = text;
    g_hist_key = key;
  }
  void histRecord() {
    if (!g_hist_dirty) return;
    g_hist_dirty = false;
    histStart();
    Snap now = takeSnap();
    if (sameSnap(now, g_hist[static_cast<size_t>(g_hist_at)].after)) return;
    const ULONGLONG t = GetTickCount64();
    HistStep& cur = g_hist[static_cast<size_t>(g_hist_at)];
    if (g_hist_at > 0 && g_hist_at == static_cast<int>(g_hist.size()) - 1 && !g_hist_key.empty() && cur.key == g_hist_key && t - cur.when < 800) {
      cur.text = g_hist_text;
      cur.when = t;
      cur.after = std::move(now);
      return;
    }
    g_hist.resize(static_cast<size_t>(g_hist_at) + 1);
    g_hist.push_back({g_hist_text.empty() ? std::string("Change") : g_hist_text, g_hist_key, t, std::move(now)});
    if (g_hist.size() > 60) {
      g_hist[0].after = std::move(g_hist[1].after);
      g_hist.erase(g_hist.begin() + 1);
    }
    g_hist_at = static_cast<int>(g_hist.size()) - 1;
  }
  void restoreSnap(const Snap& t, asw_entity* const ents[2]) {
    g_last_paste = LastPaste{};
    std::map<Edit*, BoxView> to;
    for (auto& [k, ed] : g_edits) {
      auto w = t.boxes.find(k);
      if (w != t.boxes.end()) to[ed.get()] = w->second.now;
      else to.emplace(ed.get(), ed->orig);
    }
    for (auto& [ed, b] : to) apply(*ed, b);
    for (const auto& [k, b] : t.boxes) {
      if (g_edits.count(k)) continue;
      auto ed = std::make_shared<Edit>();
      ed->orig = b.orig;
      ed->now = b.now;
      ed->label = b.label;
      g_edits[k] = std::move(ed);
    }
    for (auto& kv : g_sedits) putBackSEdit(kv.second);
    g_sedits.clear();
    for (auto it = g_swaps.begin(); it != g_swaps.end();) {
      auto w = t.swaps.find(it->first);
      if (w != t.swaps.end() && w->second.folder == it->second.folder && w->second.code == it->second.code) { ++it; continue; }
      putBackSwap(it->second);
      it = g_swaps.erase(it);
    }
    for (const auto& [k, c] : t.swaps) {
      if (g_swaps.count(k)) continue;
      Swap sw;
      sw.folder = c.folder;
      sw.code = c.code;
      sw.added = c.added;
      sw.ver = c.ver;
      g_swaps[k] = std::move(sw);
    }
    const asw_engine* engine = asw_engine::get();
    for (auto it = g_rswaps.begin(); it != g_rswaps.end();) {
      auto w = t.routines.find(it->first);
      if (w != t.routines.end() && w->second.folder == it->second.sw.folder && w->second.code == it->second.sw.code) { ++it; continue; }
      putBackRoutine(it->second);
      it = g_rswaps.erase(it);
    }
    for (const auto& [k, c] : t.routines) {
      if (g_rswaps.count(k)) continue;
      RSwap r;
      r.sw.folder = c.folder;
      r.sw.code = c.code;
      r.sw.added = c.added;
      r.sw.ver = c.ver;
      r.note = c.note;
      g_rswaps[k] = std::move(r);
    }
    syncRoutines(engine, ents);
    g_states[0] = States{};
    g_states[1] = States{};
    for (int i = 0; i < 2; i++) if (ents[i]) readStates(ents[i], g_states[i]);
    for (const auto& [k, ed] : t.sedits) g_sedits[k] = ed;
    linkSigs();
    syncScripts(ents);
    g_sel_cache[0] = Sel{};
    g_sel_cache[1] = Sel{};
  }
  void histGo(int to, asw_entity* const ents[2], View& v) {
    histRecord();
    histStart();
    if (to < 0 || to >= static_cast<int>(g_hist.size()) || to == g_hist_at) return;
    const int from = g_hist_at;
    restoreSnap(g_hist[static_cast<size_t>(to)].after, ents);
    g_hist_at = to;
    g_hist_dirty = false;
    if (to == from - 1) v.status = "Undid: " + g_hist[static_cast<size_t>(from)].text;
    else if (to == from + 1) v.status = "Redid: " + g_hist[static_cast<size_t>(to)].text;
    else v.status = to == 0 ? std::string("Back to the start: every change since is undone.") : "Back to: " + g_hist[static_cast<size_t>(to)].text;
    COMBO_LOG("script & boxes: history: " + v.status + " (step " + std::to_string(to) + " of " + std::to_string(g_hist.size() - 1) + ")");
  }
  std::string histTextS(const SReq& r, const std::string& msg) {
    const std::string name = objName(sharedName(r.state));
    switch (r.t) {
      case SReq::Frames: return name + ": frames " + std::to_string(r.value);
      case SReq::Level: return name + ": attack level " + std::to_string(r.value);
      case SReq::Arg: return name + ": value " + std::to_string(r.value);
      case SReq::Name: return name + ": name " + r.text;
      case SReq::Replace: return name + ": line replaced";
      case SReq::ResetLine: return name + ": line reset";
      case SReq::Reset: return name + ": reset";
      case SReq::Version:
        if (r.folder.empty()) return name + ": today's version";
        if (r.folder.rfind("file:", 0) == 0) return name + ": loaded " + r.folder.substr(5) + ".bbs";
        return name + ": version " + verLabel(r.folder);
      default: return name + ": " + (msg.empty() ? std::string("line change") : msg);
    }
  }
  std::string histKeyS(const SReq& r) {
    if (r.t != SReq::Frames && r.t != SReq::Level && r.t != SReq::Arg) return {};
    return "s|" + std::to_string(r.side) + "|" + r.state + "|" + std::to_string(r.offset) + "|" + std::to_string(r.t == SReq::Arg ? r.field : -static_cast<int>(r.t) - 1);
  }
  bool sameKey(const SKey& a, const SKey& b) { return !(a < b) && !(b < a); }
  void refusePending(const std::pair<std::string, std::string>& key, Pending& p, asw_entity* const ents[2]) {
    const std::string& chr = key.first;
    const std::string& state = key.second;
    int waited = 0;
    for (auto it = g_sedits.begin(); it != g_sedits.end();) {
      if (it->first.chr != chr || it->first.state != state) { ++it; continue; }
      bool known = false;
      for (const SKey& a : p.after) if (sameKey(a, it->first)) known = true;
      if (!known) waited++;
      putBackSEdit(it->second);
      it = g_sedits.erase(it);
    }
    std::string no;
    if (auto cur = g_swaps.find(key); cur != g_swaps.end()) {
      no = cur->second.no;
      putBackSwap(cur->second);
      g_swaps.erase(cur);
    }
    for (int i = 0; i < 2; i++)
      if (ents[i] && !g_states[i].buf) readStates(ents[i], g_states[i]);
    bool back = false, kept = false;
    if (p.had) {
      Swap& b = g_swaps[key] = std::move(p.was);
      applySwap(chr, state, b, ents, true);
      kept = b.at.empty() && b.held;
      back = !b.at.empty() || kept;
      if (!back) g_swaps.erase(key);
      COMBO_LOG("script & boxes: " + chr + " " + state + (kept ? ": the copy loaded before is kept with its value edits (it isn't on in this fight)"
                                                                 : back ? ": the script loaded before is back in place" : ": the script loaded before couldn't be put back, the move is the game's own"));
    }
    g_states[0] = States{};
    g_states[1] = States{};
    for (int i = 0; i < 2; i++)
      if (ents[i]) readStates(ents[i], g_states[i]);
    const auto restore = [&](std::pair<SKey, SEdit>& d) {
      d.second.at.clear();
      const auto in = g_sedits.emplace(d.first, d.second);
      if (in.second) applySEdit(in.first->first, in.first->second, ents);
    };
    int shown = 0, lost = 0;
    if (p.line) {
      if (!p.had || back) for (auto& d : p.dropped) restore(d);
      else
        for (auto& d : p.dropped) {
          if (d.first.copy == p.on_copy) shown++;
          else if (!d.first.copy) restore(d);
          else lost++;
        }
    } else if (kept) for (auto& d : p.dropped) restore(d);
    const std::string why = no.empty() ? std::string("see combo_log.txt.") : no;
    refuse(p.req, chr, p.head + why);
    if (shown) {
      g_smsg += " The move is back to the game's own lines, and its " + std::to_string(shown) + " value edit(s) went with the copy.";
      COMBO_LOG("script & boxes: " + chr + " " + state + ": " + std::to_string(shown) + " value edit(s) dropped with the copy that couldn't be put back");
    }
    if (lost) g_smsg += " " + std::to_string(lost) + (lost == 1 ? " value edit" : " value edits") + " made on the earlier copy's lines couldn't be kept.";
    if (p.more) g_smsg += " " + std::to_string(p.more) + (p.more == 1 ? " later change" : " later changes") + " made while it waited went with it.";
    if (waited) g_smsg += " " + std::to_string(waited) + (waited == 1 ? " value edit" : " value edits") + " made while it waited went with it.";
    COMBO_LOG("script & boxes: " + chr + " " + state + ": the change that waited is refused: " + why +
              (p.more ? " (" + std::to_string(p.more) + " later change(s) dropped)" : std::string()) + (waited ? " (" + std::to_string(waited) + " value edit(s) dropped)" : std::string()));
    bool listed = false, popped = false;
    for (int i = 0; i <= g_hist_at && i < static_cast<int>(g_hist.size()); i++) {
      const auto h = g_hist[static_cast<size_t>(i)].after.swaps.find(key);
      if (h != g_hist[static_cast<size_t>(i)].after.swaps.end() && h->second.code == p.code) listed = true;
    }
    if (listed && !g_hist_dirty && !p.more && !waited && g_hist_at >= 1 && g_hist_at + 1 == static_cast<int>(g_hist.size())) {
      const auto last = g_hist.back().after.swaps.find(key);
      if (last != g_hist.back().after.swaps.end() && last->second.code == p.code && sameSnap(takeSnap(), g_hist[static_cast<size_t>(g_hist_at) - 1].after)) {
        g_hist.pop_back();
        g_hist_at--;
        popped = true;
      }
    }
    if (listed && !popped) {
      g_smsg += " Undo still lists this change.";
      COMBO_LOG("script & boxes: " + chr + " " + state + ": the refused change is still in the undo history (it shares its step with other changes)");
    }
    g_sel_cache[0] = Sel{};
    g_sel_cache[1] = Sel{};
    g_code_gen++;
  }
  void resolvePending(asw_entity* const ents[2]) {
    for (auto it = g_pending.begin(); it != g_pending.end();) {
      const auto sw = g_swaps.find(it->first);
      if (sw == g_swaps.end() || sw->second.code != it->second.code || !sw->second.at.empty()) { it = g_pending.erase(it); continue; }
      if (sw->second.near_wait && nearGoing()) { ++it; continue; }
      Pending p = std::move(it->second);
      const auto key = it->first;
      it = g_pending.erase(it);
      refusePending(key, p, ents);
    }
  }
  void keepPending(const char* when) {
    for (const auto& [key, p] : g_pending)
      COMBO_LOG("script & boxes: " + key.first + " " + key.second + ": still waiting for memory near the script " + when + ": kept like a change that is on");
    g_pending.clear();
  }
  void notePendingAfter() {
    for (auto& [key, p] : g_pending) {
      if (!p.fresh) continue;
      p.fresh = false;
      p.after.clear();
      for (const auto& [k, ed] : g_sedits) if (k.chr == key.first && k.state == key.second) p.after.push_back(k);
    }
  }
}  // namespace

const char* kindName(int kind) {
  static const char* const k[19] = {"Hurtbox", "Hitbox", "ExPoint", "ExRect", "ExVector", "Push", "TempCenter", "Neck", "Abdominal", "SPGuard",
                                    "RLeg", "LLeg", "Private0", "Private1", "Private2", "Private3", "ExtendJon", "Unknown", "ComboHitbox"};
  return kind >= 0 && kind < 19 ? k[kind] : "?";
}
bool editable(int kind) { return kind >= 0 && kind < 19; }  // every kind the box file has, not only hurt and hit

namespace {
  std::mutex g_label_m;
  std::map<std::string, std::map<std::string, std::string>> g_move_labels;
  std::filesystem::path moveLabelsFile() { return g_dir / "move_labels.txt"; }
  void readMoveLabels() {
    std::map<std::string, std::map<std::string, std::string>> out;
    try {
      std::ifstream f(moveLabelsFile(), std::ios::binary);
      std::string line;
      while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t a = line.find('\t');
        const size_t b = a == std::string::npos ? a : line.find('\t', a + 1);
        if (b == std::string::npos) continue;
        const std::string chr = line.substr(0, a), state = line.substr(a + 1, b - a - 1), label = line.substr(b + 1);
        if (chr.empty() || state.empty() || label.empty()) continue;
        out[state.rfind(kSharedPrefix, 0) == 0 || state.rfind(kCommonObjPrefix, 0) == 0 ? std::string("CMN") : chr][state] = label;
      }
    } catch (...) {}
    std::lock_guard<std::mutex> lk(g_label_m);
    g_move_labels.swap(out);
  }
  std::vector<lists::Entry> g_favs, g_recent;
  std::filesystem::path favFile() { return g_dir / "favourites.txt"; }
  std::filesystem::path recentFile() { return g_dir / "recent.txt"; }
  std::vector<lists::Entry> readList(const std::filesystem::path& p) {
    try {
      std::ifstream f(p, std::ios::binary);
      std::stringstream ss;
      ss << f.rdbuf();
      return lists::read(ss.str());
    } catch (...) {
      return {};
    }
  }
  bool writeList(const std::filesystem::path& f, const std::vector<lists::Entry>& v) {
    try {
      std::error_code ec;
      std::filesystem::create_directories(g_dir, ec);
      std::filesystem::path tmp = f;
      tmp += ".new";
      {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        o << lists::write(v);
        o.flush();
        if (!o.good()) return false;
      }
      std::filesystem::rename(tmp, f, ec);
      return !ec;
    } catch (...) {
      return false;
    }
  }
  void noteRecent(const std::string& fighter, const std::string& key) {
    if (fighter.empty() || key.empty()) return;
    const lists::Entry e{isShared(key) || isCmnObj(key) ? std::string("CMN") : fighter, key};
    std::lock_guard<std::mutex> lk(g_label_m);
    if (!g_recent.empty() && g_recent.front().chr == e.chr && g_recent.front().key == e.key) return;
    lists::touch(g_recent, e, 400);
    if (!writeList(recentFile(), g_recent)) COMBO_LOG("script & boxes: couldn't write StriveLabs_scripts/recent.txt");
  }
  bool writeMoveLabels() {
    try {
      std::error_code ec;
      std::filesystem::create_directories(g_dir, ec);
      const std::filesystem::path f = moveLabelsFile();
      std::filesystem::path tmp = f;
      tmp += ".new";
      {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        for (const auto& [chr, m] : g_move_labels)
          for (const auto& [state, label] : m) o << chr << '\t' << state << '\t' << label << '\n';
        o.flush();
        if (!o.good()) return false;
      }
      std::filesystem::rename(tmp, f, ec);
      return !ec;
    } catch (...) {
      return false;
    }
  }
}

bool setMoveLabel(const std::string& fighter, const std::string& state, const std::string& label) {
  if (fighter.empty() || state.empty()) return true;
  const std::string chr = isShared(state) || isCmnObj(state) ? std::string("CMN") : fighter;
  std::string clean;
  for (char ch : label) clean += ch == '\t' || ch == '\n' || ch == '\r' ? ' ' : ch;
  const size_t first = clean.find_first_not_of(' ');
  clean = first == std::string::npos ? std::string() : clean.substr(first, clean.find_last_not_of(' ') - first + 1);
  std::lock_guard<std::mutex> lk(g_label_m);
  const auto of_chr = g_move_labels.find(chr);
  if (clean.empty()) {
    if (of_chr == g_move_labels.end() || !of_chr->second.erase(state)) return true;
    if (of_chr->second.empty()) g_move_labels.erase(of_chr);
  } else {
    if (of_chr != g_move_labels.end()) {
      const auto it = of_chr->second.find(state);
      if (it != of_chr->second.end() && it->second == clean) return true;
    }
    g_move_labels[chr][state] = clean;
  }
  const bool ok = writeMoveLabels();
  if (!ok) COMBO_LOG("script & boxes: couldn't write StriveLabs_scripts/move_labels.txt");
  return ok;
}
bool isFavourite(const std::string& chr, const std::string& key) {
  const std::string c = isShared(key) || isCmnObj(key) ? std::string("CMN") : chr;
  std::lock_guard<std::mutex> lk(g_label_m);
  for (const lists::Entry& e : g_favs) if (e.chr == c && e.key == key) return true;
  return false;
}
void setFavourite(const std::string& chr, const std::string& key, bool on) {
  const lists::Entry e{isShared(key) || isCmnObj(key) ? std::string("CMN") : chr, key};
  std::lock_guard<std::mutex> lk(g_label_m);
  lists::toggle(g_favs, e, on);
  if (!writeList(favFile(), g_favs)) COMBO_LOG("script & boxes: couldn't write StriveLabs_scripts/favourites.txt");
}
std::vector<std::string> favourites(const std::string& chr) {
  std::lock_guard<std::mutex> lk(g_label_m);
  return lists::keysFor(g_favs, chr, 100000);
}
std::vector<std::string> recent(const std::string& chr) {
  std::lock_guard<std::mutex> lk(g_label_m);
  return lists::keysFor(g_recent, chr, 20);
}
std::map<std::string, std::string> moveLabels(const std::string& chr) {
  std::lock_guard<std::mutex> lk(g_label_m);
  const auto it = g_move_labels.find(chr);
  std::map<std::string, std::string> out = it == g_move_labels.end() ? std::map<std::string, std::string>() : it->second;
  if (const auto cm = g_move_labels.find("CMN"); cm != g_move_labels.end() && chr != "CMN") out.insert(cm->second.begin(), cm->second.end());
  return out;
}

void init(const std::filesystem::path& ue4ss_dir) {
  g_dir = ue4ss_dir / "StriveLabs_scripts";
  keepNearRoom();
  try {
    const uintptr_t p = sigscan::get().scan("\x48\x83\xEC\x08\x4C\x8B\xC1\x48\x85\xD2\x0F\x85\x00\x00\x00\x00\x89\x91\x40\x01\x00\x00\xB0\x01\x48\x89\x91\x00\x01\x00\x00",
                                            "xxxxxxxxxxxx????xxxxxxxxxxxxxxx");
    auto* d = new PLH::x64Detour(p, reinterpret_cast<uint64_t>(&hook_ReadBoxes), reinterpret_cast<uint64_t*>(&orig_ReadBoxes));
    if (!d->hook()) orig_ReadBoxes = nullptr;
  } catch (...) {
    orig_ReadBoxes = nullptr;
  }
  COMBO_LOG(orig_ReadBoxes ? "old hurtboxes: the game's box reader found" : "old hurtboxes: the game's box reader not found, the 4.09 hurtboxes option does nothing");
  readMoveLabels();
  {
    std::vector<lists::Entry> f = readList(favFile()), r = readList(recentFile());
    std::lock_guard<std::mutex> lk(g_label_m);
    g_favs = std::move(f);
    g_recent = std::move(r);
  }
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
  if (!g_open) { std::lock_guard<std::mutex> lk(g_sreq_m); g_sreqs.clear(); }
  std::lock_guard<std::mutex> lk(g_m);
  g_view.open = g_open;
}
bool isOpen() { return g_open && GetTickCount64() - g_ticked < 1000; }
View view() { std::lock_guard<std::mutex> lk(g_m); return g_view; }
void exportState(int side) { exportMove(side, std::string()); }
void exportMove(int side, const std::string& name) {
  if (side < 0 || side > 1) return;
  { std::lock_guard<std::mutex> lk(g_export_m); g_export_name[side] = name; }
  g_export_state = side;
}
void selectState(int side, const std::string& name) {
  if (side < 0 || side > 1) return;
  std::lock_guard<std::mutex> lk(g_sel_m);
  g_sel_want[side] = name;
}
void setInline(int side, const std::vector<std::string>& keys) {
  if (side < 0 || side > 1) return;
  std::lock_guard<std::mutex> lk(g_sel_m);
  g_inl_want[side] = keys;
  if (g_inl_want[side].size() > 16) g_inl_want[side].resize(16);
}
void exportBoxes(int side) { g_export_boxes = side; }
void exportPak(int side) { g_export_pak = side; }
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
namespace {
  std::atomic<unsigned long long> g_reload_at{0};
  std::atomic<int> g_side_shown{0};
}
void shownSide(int side) { g_side_shown = side; }
void reloadMoveFiles() { g_reload_at = GetTickCount64(); }
void setName(int side, const std::string& state, int offset, int field, const std::string& name) {
  std::string t = name;
  const auto trim = [&] {
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == ',')) t.pop_back();
    size_t a = 0;
    while (a < t.size() && (t[a] == ' ' || t[a] == '\t')) a++;
    t.erase(0, a);
  };
  trim();
  if (t.rfind("s32", 0) == 0 && t.size() > 3 && (t[3] == '\'' || t[3] == '"')) t.erase(0, 3);
  if (t.size() >= 2 && (t.front() == '\'' || t.front() == '"') && t.back() == t.front()) t = t.substr(1, t.size() - 2);
  trim();
  SReq r; r.t = SReq::Name; r.side = side; r.state = state; r.offset = offset; r.field = field; r.text = t; sask(std::move(r));
}
void insertLine(int side, const std::string& state, int offset, int op) { SReq r; r.t = SReq::Insert; r.side = side; r.state = state; r.offset = offset; r.value = op; sask(std::move(r)); }
void copyLine(int side, const std::string& state, int offset) { SReq r; r.t = SReq::Copy; r.side = side; r.state = state; r.offset = offset; sask(std::move(r)); }
void removeLine(int side, const std::string& state, int offset) { SReq r; r.t = SReq::Remove; r.side = side; r.state = state; r.offset = offset; sask(std::move(r)); }
void pasteLines(int side, const std::string& state, int offset, const std::string& text) {
  SReq r; r.t = SReq::Paste; r.side = side; r.state = state; r.offset = offset; r.text = text; sask(std::move(r));
}
std::vector<int> removalPreview(const std::vector<uint8_t>& code, int offset) {
  std::vector<int> out;
  if (offset < 0 || !bbscript::instruction_sizes) return out;
  for (size_t p : ops::removedLines(ctx(), code, static_cast<size_t>(offset))) out.push_back(static_cast<int>(p));
  return out;
}
std::string linesText(const std::vector<uint8_t>& code, int offset) {
  if (offset < 0 || !bbscript::instruction_sizes) return {};
  return ops::linesText(ctx(), code, static_cast<size_t>(offset));
}
std::vector<std::pair<int, std::string>> insertable() {
  std::vector<std::pair<int, std::string>> out;
  if (!bbscript::instruction_sizes) return out;
  const ops::Ctx c = ctx();
  for (const auto& [o, in] : g_db.instr) {
    if (in.name.empty() || ops::sizeOf(c, o) < 4) continue;
    const int e = in.block > 0 ? ops::endFor(c, o) : -1;
    out.push_back({o, in.name + (e >= 0 ? "  (and " + ops::nameOf(c, e) + ")" : std::string())});
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
  return out;
}
void setValueKind(int side, const std::string& state, int offset, int field, int tag) {
  SReq r; r.t = SReq::Tag; r.side = side; r.state = state; r.offset = offset; r.field = field; r.value = tag; sask(std::move(r));
}
std::vector<std::pair<int, std::string>> variables() {
  std::vector<std::pair<int, std::string>> out(g_db.vars.begin(), g_db.vars.end());
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
  return out;
}
std::vector<std::pair<std::string, std::string>> moveFiles(const std::string& chr) {
  std::vector<std::pair<std::string, std::string>> out;
  std::error_code ec;
  try {  // (a name the system code page can't hold is left out, never a throw out of the menu)
    const bool shared = chr == "CMN";
    const bool obj = chr.size() > 8 && chr.compare(chr.size() - 8, 8, "/objects") == 0;
    const bool cmn_obj = chr == "CMN/objects";
    for (const auto& e : std::filesystem::directory_iterator(g_dir / chr, ec)) {
      if (e.path().extension() != ".bbs") continue;
      try {
        const std::string stem = e.path().stem().string();
        for (const std::string& n : moveFileNames(e.path())) out.push_back({stem, shared ? kSharedPrefix + n : cmn_obj ? kCommonObjPrefix + n : obj ? kObjectPrefix + n : n});
      } catch (...) {}
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
  std::vector<std::pair<int, std::string>> out = insertable();
  out.erase(std::remove_if(out.begin(), out.end(), [&](const auto& x) { return x.first == op; }), out.end());
  return out;
}
void resetMove(int side, const std::string& state) { SReq r; r.t = SReq::Reset; r.side = side; r.state = state; sask(std::move(r)); }
void resetLine(int side, const std::string& state, int offset) { SReq r; r.t = SReq::ResetLine; r.side = side; r.state = state; r.offset = offset; sask(std::move(r)); }
void undo() { Req r; r.t = Req::Undo; ask(std::move(r)); }
void redo() { Req r; r.t = Req::Redo; ask(std::move(r)); }
void historyGo(int step) { Req r; r.t = Req::Goto; r.index = step; ask(std::move(r)); }
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

void setOldHurtboxes(bool on) { g_old_want = on; }

namespace { bool g_holding = false; }
void outsideTraining() {
  if (g_old_live.load(std::memory_order_relaxed) || g_old_base[0] || g_old_base[1]) oldBoxesOff();
  { std::lock_guard<std::mutex> lk(g_io_m); g_io_save.clear(); g_io_load.clear(); }  // (not carried into the next training)
  g_reload_at = 0;
  { std::lock_guard<std::mutex> lk(g_sreq_m); g_sreqs.clear(); }
  // In case a COL file outlives training (into an online fight, a replay, versus): the game's boxes go back before the
  // fight runs. Kept edits are picked up again in training, on the copies they find there. The same for script edits.
  if (!g_pending.empty()) keepPending("when training was left");
  for (auto& kv : g_sedits) putBackSEdit(kv.second);
  for (auto& kv : g_swaps) putBackSwap(kv.second);
  putBackRoutines();
  if (!g_keep && !g_holding && !combo_trials::stockFight()) { g_sedits.clear(); g_swaps.clear(); g_rswaps.clear(); g_hist.clear(); g_hist_at = 0; g_hist_dirty = false; }
  dropScans();  // (the game may build its move lists again: found again in training)
  g_ef.clear();
  g_shared = SharedCache{};
  g_code_len.clear();
  if (g_edits.empty()) return;
  for (auto& kv : g_edits) {
    Edit& ed = *kv.second;
    for (hitbox* p : ed.at) if (writable(p, sizeof(hitbox)) && static_cast<int>(p->type) == ed.orig.kind && same(*p, ed.now)) put(p, ed.orig);
    ed.at.clear();
  }
  g_cols.clear();
  if (!g_keep && !g_holding && !combo_trials::stockFight()) { g_edits.clear(); g_undo.clear(); }
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
  {
    static bool held = false;
    const bool hold = combo_trials::stockFight();
    if (hold && !held) {
      g_holding = true;
      outsideTraining();
      g_holding = false;
    }
    held = hold;
    if (hold) return;
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
      {
        static ULONGLONG taken = 0;
        std::vector<std::string> got = combo_trials::takeServedToday();
        if (!got.empty() || GetTickCount64() - taken > 5000) g_today_live = std::move(got);
        taken = GetTickCount64();
      }
      { std::lock_guard<std::mutex> lk(g_sreq_m); g_sreqs.clear(); }
      g_ver_said.clear();
      dropScans();  // (a new fight's files: their copies are found again)
      for (auto& [b, s] : g_near_search)
        if (s.job.valid()) g_near_orphans.push_back(std::move(s.job));
      g_near_search.clear();
      g_no_room = false;
      g_cols.clear();
      g_col_failed.clear();
      g_col_tried[0].clear();
      g_col_tried[1].clear();
      g_ef.clear();
      g_shared = SharedCache{};
      g_code_len.clear();
      if (!g_pending.empty()) keepPending("when the fight was loaded again");
      // Put back first, where the memory still holds our values (a file that outlived the fight must not keep them),
      // then the records go: picked up again on the new files
      for (auto& kv : g_sedits) putBackSEdit(kv.second);
      for (auto& kv : g_swaps) putBackSwap(kv.second);
      putBackRoutines();
      for (auto& kv : g_edits) {
        Edit& ed = *kv.second;
        for (hitbox* p : ed.at) if (writable(p, sizeof(hitbox)) && static_cast<int>(p->type) == ed.orig.kind && same(*p, ed.now)) put(p, ed.orig);
        ed.at.clear();
      }
      if (!g_keep) { g_edits.clear(); g_undo.clear(); g_sedits.clear(); g_swaps.clear(); g_rswaps.clear(); g_hist.clear(); g_hist_at = 0; g_hist_dirty = false; }
    }
    {
      static unsigned files_load = ~0u;
      const unsigned load = combo_trials::loadNumber();
      if (fresh || load != files_load) {
        files_load = load;
        std::map<std::string, int> got = combo_trials::filesThisLoad();
        if (got != g_files_live) {
          std::string l;
          for (const auto& [c, f] : got) l += (l.empty() ? "" : ", ") + c + (f == 1 ? " today's" : f == 0 ? " the version's own" : " both seen");
          COMBO_LOG("script & boxes: script files this load: " + (l.empty() ? std::string("none seen") : l));
        }
        g_files_live = std::move(got);
      }
    }
    static ULONGLONG synced = 0;  // (script edits onto the copies: after a new fight, and now and then)
    pollScans(ents);  // (a background search of the move list that's in: its copies written)
    const bool placed = stepNearSearches();
    static bool risc_was = false;
    const bool risc = combo_trials::oldRiscRules();
    if (fresh || placed || risc != risc_was || GetTickCount64() - synced > 1000) {
      syncScripts(ents);
      syncRoutines(engine, ents);
      syncRiscRoutines(engine, ents, risc);
      keepTodayTick(ents);
      risc_was = risc;
      synced = GetTickCount64();
      g_code_gen++;
    }
    if (!g_pending.empty()) resolvePending(ents);
    oldBoxesTick(ents, fresh);
  }
  histStart();
  {  // Script edits asked for by the UI
    std::vector<SReq> sreqs;
    { std::lock_guard<std::mutex> lk(g_sreq_m); sreqs.swap(g_sreqs); }
    size_t reload_from = sreqs.size();
    bool reloading = false;
    int reload_kept = 0;
    if (const unsigned long long t = g_reload_at.exchange(0); t && GetTickCount64() - t < 1000) {
      reloading = true;
      std::vector<std::pair<std::string, std::string>> done;
      const int shown = g_side_shown.load();
      std::string shown_chr, shown_want;
      if (g_open && (shown == 0 || shown == 1) && ents[shown]) {
        shown_chr = sideChr(ents[shown]);
        std::lock_guard<std::mutex> lk(g_sel_m);
        shown_want = g_sel_want[shown];
      }
      for (int i = 0; i < 2; i++) {
        if (!ents[i]) continue;
        const std::string chr = sideChr(ents[i]);
        for (const auto& [k, sw] : g_swaps) {
          const std::string& f = sw.folder;
          if (k.first != chr || (f.rfind("file:", 0) != 0 && f.rfind("edit:", 0) != 0) || f.size() <= 5) continue;
          if (std::find(done.begin(), done.end(), k) != done.end()) continue;
          done.push_back(k);
          bool touched = f.rfind("edit:", 0) == 0;
          if (!touched)
            for (const auto& [sk, ed] : g_sedits) if (sk.chr == chr && sk.state == k.second) { touched = true; break; }
          if (touched && !(chr == shown_chr && k.second == shown_want)) { reload_kept++; continue; }
          SReq r;
          r.t = SReq::Version; r.side = i; r.state = k.second; r.folder = "file:" + f.substr(5);
          sreqs.push_back(std::move(r));
        }
      }
      const int any_side = ents[0] ? 0 : 1;
      for (const auto& [k, sw] : g_swaps) {
        const std::string& f = sw.folder;
        if (k.first != "CMN" || !isObj(k.second) || (f.rfind("file:", 0) != 0 && f.rfind("edit:", 0) != 0) || f.size() <= 5 || !ents[any_side]) continue;
        bool touched = f.rfind("edit:", 0) == 0;
        if (!touched)
          for (const auto& [sk, ed] : g_sedits) if (sk.chr == "CMN" && sk.state == k.second) { touched = true; break; }
        const URef shown_unit = unitOf(shown_chr, shown_want);
        if (touched && !(shown_unit.chr == "CMN" && shown_unit.state == k.second)) { reload_kept++; continue; }
        SReq r;
        r.t = SReq::Version; r.side = any_side; r.state = kCommonObjPrefix + objName(k.second); r.folder = "file:" + f.substr(5);
        sreqs.push_back(std::move(r));
      }
      for (const auto& [k, rs] : g_rswaps) {
        const bool edited = rs.sw.folder.rfind("edit:", 0) == 0;
        if ((rs.sw.folder.rfind("file:", 0) != 0 && !edited) || rs.sw.folder.size() <= 5) continue;
        int side = k.first == "CMN" && ents[any_side] ? any_side : -1;
        for (int i = 0; i < 2 && side < 0; i++) if (ents[i] && sideChr(ents[i]) == k.first) side = i;
        if (side < 0) continue;
        SReq r;
        r.t = SReq::Version; r.side = side; r.state = unitState(k); r.folder = "file:" + rs.sw.folder.substr(5);
        if (edited) {
          const URef shown_unit = unitOf(shown_chr, shown_want);
          if (shown_want.empty() || rkeyOf(shown_unit.chr, shown_unit.state) != k) { reload_kept++; continue; }
          if (k.first == "CMN" && isObj(k.second)) r.state = kCommonObjPrefix + objName(k.second);
        }
        sreqs.push_back(std::move(r));
      }
    }
    const std::string kept_note = reload_kept == 0 ? std::string()
                                                   : " " + std::to_string(reload_kept) + (reload_kept == 1 ? " move has" : " moves have") + " changes made in the editor and stayed as they are (Reload file in the move's panel reloads one).";
    int reloaded = 0, reload_failed = 0;
    std::string reload_err;
    for (size_t k = 0; k < sreqs.size(); k++) {
      const bool mine = reloading && k >= reload_from;
      if (mine) g_smsg.clear();
      const std::string msg_was = g_smsg;
      g_smsg.clear();
      handleS(sreqs[k], ents);
      notePendingAfter();
      if (mine) histMark("Reloaded files");
      else histMark(histTextS(sreqs[k], g_smsg), histKeyS(sreqs[k]));
      if (g_smsg.empty()) g_smsg = msg_was;
      if (!mine) continue;
      if (g_smsg.rfind("Loaded ", 0) == 0 && g_smsg.find("couldn't") == std::string::npos) reloaded++;
      else { reload_failed++; if (reload_err.empty()) reload_err = g_smsg.empty() ? objName(sharedName(sreqs[k].state)) + " wasn't loaded." : g_smsg; }
    }
    if (reloading) {
      const int n = reloaded + reload_failed;
      if (n == 0) g_smsg = "Reload: no move or routine is loaded from a file." + kept_note;
      else if (n > 1 || reload_failed || reload_kept) g_smsg = "Reloaded " + std::to_string(reloaded) + " of " + std::to_string(n) + (n == 1 ? " file" : " files") + (reload_err.empty() ? "." : ". " + reload_err) + kept_note;
      COMBO_LOG("script & boxes: reload: " + g_smsg);
      imgui_overlay::note(g_smsg);
      if (!g_open) g_smsg.clear();
    }
    if (!sreqs.empty()) { linkSigs(); g_code_gen++; }
  }
  if (g_open || std::any_of(g_sedits.begin(), g_sedits.end(), [](const auto& kv) { return isObj(kv.first.state); }) ||
      std::any_of(g_swaps.begin(), g_swaps.end(), [](const auto& kv) { return isObj(kv.first.second); }) ||
      std::any_of(g_rswaps.begin(), g_rswaps.end(), [](const auto& kv) { return isObj(kv.first.second); })) {
    for (int i = 0; i < 2; i++) if (ents[i]) wantEf(sideChr(ents[i]), ents);
    wantCommonEf(engine);
    if (engine) {
      const std::string chrs[2] = {sideChr(ents[0]), sideChr(ents[1])};
      const int n = (std::min)(engine->entity_count, 107);
      for (int k = 0; k < n; k++) {
        asw_entity* o = engine->entities[k];
        if (!o || o->is_player) continue;
        for (int i = 0; i < 2; i++) {
          if (!ents[i] || o->parent_ply != ents[i] || chrs[i].empty()) continue;
          const std::string& chr = chrs[i];
          EfFiles& ef = g_ef[chr];
          const void* h = o->bbs_file;
          if (!h || std::find(ef.holders.begin(), ef.holders.end(), h) != ef.holders.end()) continue;
          if (ef.holders.size() < 256) ef.holders.push_back(h);
          addEf(chr, fileOf(o), ents, "an object out", false);
        }
      }
    }
    if (g_ef_added) { g_ef_added = false; syncScripts(ents); syncRoutines(engine, ents); g_code_gen++; }
  }
  if (!g_ver_note.empty()) {
    imgui_overlay::note(g_ver_note);
    if (g_open) g_smsg = g_smsg.empty() ? g_ver_note : g_smsg + " " + g_ver_note;
    g_ver_note.clear();
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
  for (const auto& kv : sharedRoutines(engine)) v.shared_routines.push_back(kv.first);
  if (const auto ce = g_ef.find("CMN"); ce != g_ef.end() && !ce->second.files.empty()) v.shared_objects = ce->second.files.front().names;
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
      asw_entity* const fe = ents[i];
      const std::string fchr = sideChr(fe);
      const uint8_t* buf = fileOf(fe);
      static const void* states_of[2] = {};
      if (st.buf != buf || st.names.empty() || states_of[i] != fe) { readStates(fe, st); states_of[i] = fe; }  // (a new fighter too)
      s.states = st.names;
      s.routines = st.routines;
      wantEf(fchr, ents);
      if (auto ef = g_ef.find(fchr); ef != g_ef.end() && !ef->second.files.empty()) {
        s.obj_states = ef->second.files.front().names;
        s.obj_routines = ef->second.files.front().routines;
      } else s.obj_note = ef != g_ef.end() && ef->second.tries > 1 ? std::string(kEfWait) : std::string("Looking for this fighter's object script...");
      std::string want;
      { std::lock_guard<std::mutex> lk(g_sel_m); want = g_sel_want[i]; }
      Sel& sel = g_sel_cache[i];
      const URef un = unitOf(fchr, want);
      const uint8_t* start = want.empty() ? nullptr : moveStart(i, un.chr, un.state);
      if (!start) { sel = Sel{}; }
      else if (sel.name != want || sel.start != start) {
        if (sel.name != want) noteRecent(fchr, want);
        sel.name = want;
        sel.start = start;
        readScriptAt(start, sel.c);
      }
      s.selected = sel.name;
      s.sel_script = sel.c.script;
      if (sel.start) {
        const ULONGLONG now_ms = GetTickCount64();
        if (sel.code_at != sel.start || sel.gen != g_code_gen || now_ms - sel.read >= 250) {
          sel.code = unitBytes(sel.start);
          sel.code_at = sel.start;
          sel.gen = g_code_gen;
          sel.read = now_ms;
        }
        s.sel_code = sel.code;
      }
      {
        static MarkCache sel_marks[2];
        unitMarks(sel_marks[i], i, un.chr, sel.name.empty() ? std::string() : un.state, s.sel_code, s.sel_script, s.sel_added, s.sel_paste_id);
      }
      if (!s.sel_script.empty()) {
        const auto& shared = sharedRoutines(engine);
        if (!shared.empty())
          for (InstrView& in : s.sel_script)
            if ((in.op == 17 || in.op == 75) && !in.call.empty()) in.call_shared = shared.count(in.call) > 0;
      }
      {
        static std::string vers_for[2];
        static std::vector<SideView::VersionView> vers[2];
        const std::string key = fchr + "/" + sel.name;
        const int kind = sel.name.empty() ? kMove : unitKind(i, un.chr, un.state);
        s.sel_kind = kind;
        if (!sel.name.empty() && vers_for[i] != key) { vers_for[i] = key; vers[i] = versionsOf(i, un.chr, un.state, kind); }
        if (!sel.name.empty()) s.sel_versions = vers[i];
        if (const auto rs = sel.name.empty() ? g_rswaps.end() : g_rswaps.find(rkeyOf(un.chr, un.state)); rs != g_rswaps.end()) {
          const std::string& f = rs->second.sw.folder;
          const std::string off = rs->second.sw.at.empty() ? std::string(" (not on)") : std::string();
          if (!off.empty()) s.sel_off = rs->second.dead || rs->second.sw.held ? rs->second.sw.no : std::string("Not on yet: it goes on once memory near the script (or the object script) is found.");
          if (f.rfind("file:", 0) == 0) { s.sel_file = f.substr(5); s.sel_loaded = s.sel_file + ".bbs" + off; }
          else if (f.rfind("edit:", 0) == 0) { s.sel_file = f.substr(5); s.sel_loaded = "lines changed" + off; }
          else s.sel_loaded = verLabel(f) + off;
        } else if (isShared(sel.name) && g_risc_rswaps.count(sharedName(sel.name))) s.sel_loaded = "4.06 R.I.S.C. rules";
        auto sw = sel.name.empty() ? g_swaps.end() : g_swaps.find({un.chr, un.state});
        if (sw != g_swaps.end()) {
          const std::string& f = sw->second.folder;
          const std::string off = sw->second.at.empty() ? std::string(" (not on)") : std::string();
          if (!off.empty() && !sw->second.no.empty()) s.sel_off = sw->second.no;
          if (f.rfind("file:", 0) == 0) { s.sel_file = f.substr(5); s.sel_loaded = s.sel_file + ".bbs" + off; }
          else if (f.rfind("edit:", 0) == 0) { s.sel_file = f.substr(5); s.sel_loaded = "lines changed" + off; }
          else s.sel_loaded = verLabel(f) + off;
        }
      }
      const bool sel_copy = startOnCopy(i, un.chr, un.state, sel.start);
      for (InstrView& in : s.sel_script) {
        in.edited = sEdited(un.chr, un.state, in.offset, sel_copy);
        auto rep = in.edited ? g_sedits.find(SKey{un.chr, un.state, in.offset, 0, sel_copy}) : g_sedits.end();
        if (rep != g_sedits.end()) { auto d = g_db.instr.find(rep->second.op); in.replaced_from = d != g_db.instr.end() ? d->second.name : std::to_string(rep->second.op); in.replaced_op = rep->second.op; }
      }
      if (!sel.name.empty()) {
        std::vector<std::string> keys;
        { std::lock_guard<std::mutex> lk(g_sel_m); keys = g_inl_want[i]; }
        auto& cache = g_inl_cache[i];
        for (auto it = cache.begin(); it != cache.end();) it = std::find(keys.begin(), keys.end(), it->first) == keys.end() ? cache.erase(it) : std::next(it);
        const auto& shared = sharedRoutines(engine);
        for (const std::string& key : keys) {
          SideView::UnitView u;
          u.key = key;
          InlCache& ic = cache[key];
          const URef ku = unitOf(fchr, key);
          const uint8_t* start = moveStart(i, ku.chr, ku.state);
          if (!start) { ic = InlCache{}; s.inl.push_back(std::move(u)); continue; }
          const ULONGLONG now_ms = GetTickCount64();
          if (ic.start != start || ic.gen != g_code_gen || now_ms - ic.read >= 250) {
            u.code = unitBytes(start);
            ic.gen = g_code_gen;
            ic.read = now_ms;
            if (ic.start != start || ic.code != u.code) { ic.start = start; ic.code = u.code; readScriptAt(start, ic.c); }
          } else u.code = ic.code;
          u.found = true;
          u.script = ic.c.script;
          unitMarks(ic.marks, i, ku.chr, ku.state, u.code, u.script, u.added, u.paste_id);
          const bool inl_copy = startOnCopy(i, ku.chr, ku.state, start);
          for (InstrView& in : u.script) {
            if ((in.op == 17 || in.op == 75) && !in.call.empty()) in.call_shared = shared.count(in.call) > 0;
            in.edited = sEdited(ku.chr, ku.state, in.offset, inl_copy);
            auto rep = in.edited ? g_sedits.find(SKey{ku.chr, ku.state, in.offset, 0, inl_copy}) : g_sedits.end();
            if (rep != g_sedits.end()) { auto d = g_db.instr.find(rep->second.op); in.replaced_from = d != g_db.instr.end() ? d->second.name : std::to_string(rep->second.op); in.replaced_op = rep->second.op; }
          }
          s.inl.push_back(std::move(u));
        }
        static std::vector<std::string> inl_said[2];
        static std::string inl_said_for[2];
        if (keys != inl_said[i] || (!keys.empty() && inl_said_for[i] != fchr + "/" + sel.name)) {
          inl_said[i] = keys;
          inl_said_for[i] = fchr + "/" + sel.name;
          std::string l;
          for (const SideView::UnitView& u : s.inl) l += (l.empty() ? "" : ", ") + (isCmnObj(u.key) ? "shared " + objName(unitOf(fchr, u.key).state) : objName(u.key)) + (u.found ? " (" + std::to_string(u.script.size()) + " lines)" : " (not found)");
          COMBO_LOG("script & boxes: its lines open in " + fchr + " " + sel.name + ": " + (l.empty() ? std::string("none") : l));
        }
      }
      if (sel.start && reinterpret_cast<const char*>(sel.start) == e->first_script_cmd)  // it's the move playing
        for (size_t k = 0; k < s.sel_script.size(); k++) if (s.sel_script[k].offset == next) { s.sel_current = static_cast<int>(k); break; }
      if (s.sel_current >= 0) {
        const int next_line = s.sel_current;
        int named_before = -1, named_last = -1, any_before = -1;
        for (int k = 0; k < static_cast<int>(s.sel_script.size()); k++) {
          const InstrView& in = s.sel_script[static_cast<size_t>(k)];
          if (in.op != 2) continue;
          if (k < next_line) any_before = k;
          if (in.sprite == s.sprite) { named_last = k; if (k < next_line) named_before = k; }
        }
        s.sel_current = named_before >= 0 ? named_before : named_last >= 0 ? named_last : any_before >= 0 ? any_before : next_line;
      }
      if (!s.sel_script.empty() && !g_db.ok) s.sel_frames.note = "no instruction database";
      else if (!s.sel_script.empty()) {  // its frames, and the one playing
        // (the box file: from the cache; looked for once per move and fighter, and again after a new fight, so a failed
        // lookup isn't repeated on every sprite)
        const Col* col = nullptr;
        const uint8_t* q = gameBoxes(reinterpret_cast<const uint8_t*>(e->hitboxes));
        for (const Col& c : g_cols) if (q >= c.base && q < c.base + c.size) col = &c;
        char who[40];
        snprintf(who, sizeof(who), "|%p", static_cast<const void*>(e));
        const std::string tried = s.selected + who;
        if (!col && q && g_col_tried[i] != tried) { g_col_tried[i] = tried; col = colOf(e); }
        s.sel_frames = buildFrames(s.sel_script, col);
        if (s.sel_current >= 0) {
          const CellView* at = nullptr;
          for (const CellView& c : s.sel_frames.cells) if (c.line == s.sel_current) at = &c;
          if (!at) for (const CellView& c : s.sel_frames.cells) if (c.line < s.sel_current && (!at || c.line > at->line)) at = &c;
          if (at) s.sel_frames.now = at->first + (std::min)((std::max)(static_cast<int>(e->sprite_duration), 0), at->dur - 1);
        }
      }
    }
  }
  {  // Box edits asked for by the UI, then the boxes as they are now
    std::vector<Req> reqs;
    { std::lock_guard<std::mutex> lk(g_req_m); reqs.swap(g_reqs); }
    for (const Req& r : reqs) {
      if (r.t == Req::Undo || r.t == Req::Redo || r.t == Req::Goto) {
        const int at_was = g_hist_at;
        histRecord();
        histStart();
        if (r.t == Req::Goto && g_hist_at != at_was) continue;
        histGo(r.t == Req::Undo ? g_hist_at - 1 : r.t == Req::Redo ? g_hist_at + 1 : r.index, ents, v);
        g_code_gen++;
        g_smsg.clear();
        g_sclear = false;
        continue;
      }
      const std::string status_was = v.status;
      handle(r, shown, v);
      if (r.t == Req::Set) histMark("Box changed: " + r.sprite, "b|" + r.sprite + "|" + std::to_string(r.index));
      else if (r.t == Req::Reset) histMark("Box reset: " + r.sprite);
      else if (r.t == Req::Copy) histMark(v.status != status_was && v.status.rfind("Copied", 0) == 0 ? v.status : "Box copied: " + r.sprite, r.join ? "b|" + r.sprite + "|" + std::to_string(r.index) : std::string());
      else if (r.t == Req::ResetSprite) histMark("Boxes reset: " + r.sprite);
      else histMark("All boxes reset");
    }
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
    if (!load.empty()) { v.status = ioLoad(load, ents); histMark("Loaded edits: " + load); g_code_gen++; }
    if (g_sclear) { v.status.clear(); g_sclear = false; }
    if (!g_smsg.empty()) { v.status = g_smsg; g_smsg.clear(); }  // (a script request's result)
    histRecord();
    v.can_undo = g_hist_at > 0;
    v.can_redo = g_hist_at + 1 < static_cast<int>(g_hist.size());
    v.history_at = g_hist_at;
    for (const HistStep& h : g_hist) v.history.push_back(h.text);
  }
  if (const int side = g_export_pak.exchange(-1); side >= 0) v.status = exportScriptPak(side, ents);
  // Exports, asked for by the UI
  for (int which = 0; which < 2; which++) {
    const int side = (which ? g_export_boxes : g_export_state).exchange(-1);
    if (side < 0 || side > 1 || !v.side[side].present) continue;
    const SideView& s = v.side[side];
    if (which == 0) {
      std::string state_name;
      { std::lock_guard<std::mutex> lk(g_export_m); state_name.swap(g_export_name[side]); }
      if (state_name.empty()) state_name = s.selected;
      if (state_name.empty()) { v.status = "Nothing is picked to export: click a move in the list once, then Export."; continue; }
      const bool opened = state_name == s.selected;
      const std::string fchr = sideChr(ents[side]);
      const URef eu = unitOf(fchr, state_name);
      const uint8_t* start = moveStart(side, eu.chr, eu.state);
      const std::vector<uint8_t> code = start ? unitBytes(start) : std::vector<uint8_t>();
      if (code.empty()) { v.status = "No script to export for " + state_name + "."; continue; }
      const std::string text = ops::textOf(ctx(), code);
      const std::filesystem::path dir = isShared(state_name) ? g_dir / "CMN" : isObj(eu.state) ? g_dir / eu.chr / "objects" : g_dir / fchr;
      const std::string file_name = isShared(state_name) ? sharedName(state_name) : objName(eu.state);
      const std::filesystem::path bbs = dir / ((file_name.empty() ? std::string("state") : file_name) + ".bbs");
      std::string kept;
      if (!keepOld(bbs, text, kept)) {
        v.status = "Couldn't keep a copy of the old " + bbs.filename().string() + ", so it wasn't written over: nothing exported.";
        COMBO_LOG("script & boxes: " + v.status);
        continue;
      }
      writeFile(bbs, text, v.status);
      if (!kept.empty() && v.status.rfind("Saved", 0) == 0) v.status += " (the old file is kept as " + kept + ")";
      if (opened && !s.sel_frames.cells.empty()) {  // (and the opened move's frames, as the strip shows them)
        std::string st;
        writeFile(dir / (file_name + "_frames.txt"), framesText(s.sel_frames, state_name), st);
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
  {
    static std::string was;
    std::string note = g_no_room ? kNoRoomNote : "";
    if (nearGoing()) note += std::string(note.empty() ? "" : " ") + "Looking for memory near the script: changes waiting for it go on once it is found.";
    if (!g_old_script_note.empty()) note += (note.empty() ? "" : " ") + g_old_script_note;
    for (const auto& [k, sw] : g_swaps) {
      if (!isObj(k.second) || !sw.at.empty() || sw.no.empty() || sw.no == kNoRoomNote || !inFight(k.first, ents)) continue;
      const std::string what = objName(k.second) + (sw.folder.rfind("file:", 0) == 0 ? " (" + sw.folder.substr(5) + ".bbs)" : std::string());
      const auto ef = g_ef.find(k.first);
      note += std::string(note.empty() ? "" : " ") + (ef == g_ef.end() || ef->second.files.empty()
                                                         ? what + ": " + kEfWait
                                                         : what + " isn't on: " + sw.no);
    }
    if (!was.empty() && v.status.rfind(was, 0) == 0) v.status.erase(0, (std::min)(v.status.size(), was.size() + 1));
    if (!note.empty() && v.status.rfind(note, 0) != 0) v.status = v.status.empty() ? note : note + " " + v.status;
    was = note;
  }
  std::lock_guard<std::mutex> lk(g_m);
  g_view = std::move(v);
}
}  // namespace box_editor

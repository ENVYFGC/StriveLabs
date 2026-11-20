// Script & boxes: a window showing each fighter's state, sprite, boxes and the state's BBScript, read live from the
// game's loaded files (training only). Instruction names and argument layouts come from BBScriptDX's database
// (StriveLabs_scripts/db/GGSTRonRemake.ron, with idNameWithDescription.json for the descriptions). A move can be
// exported as .bbs and the sprite's boxes as BBScriptDX's COL JSON. Every kind of box, and the move's script,
// can be changed live (written into the loaded files: a box edit changes every frame showing that sprite).
// Edits last for the game session, also with the window closed: kept over new training fights if asked; Reset and
// Reset all put the game's values back.
#pragma once
#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace box_editor {
  struct BoxView {
    int kind = 0;
    float x = 0, y = 0, w = 0, h = 0;  // COL units: x,y is a corner, w,h reach from it; +y is down, the feet at 0
    bool edited = false;
    float ox = 0, oy = 0, ow = 0, oh = 0;  // the game's own values, when edited
  };
  bool editable(int kind);  // every box kind (0-18)
  struct EditedView { std::string chr, sprite, boxes; };  // boxes: "Hurt 2, Hit 1"
  struct InstrView {
    int offset = 0;     // from the state's start, in bytes
    int op = 0;
    int depth = 0;      // inside how many blocks
    std::string name;   // "damage", or "Unknown712" without the database
    std::string args;   // as BBScriptDX writes them: "Val(100)"
    std::string desc;   // the database's description, "" if none
    std::string sprite, call;  // a sprite line's sprite, a subroutine call's routine
    int sprite_time = -1;      // a sprite line's frames
    bool edited = false;       // changed by a script edit
    bool call_shared = false;  // a call to a routine of the common script
    std::string replaced_from; // replaced by another instruction: the instruction it was, "" if not
    int replaced_op = -1;      // (and its number)
    // its numbers, for editing: kind 0 a number, 1 a value (only a literal is editable, not Mem(...)), 2 a
    // named value; field: its place in the line, in bytes; label / tip from labels.json, else the database's
    struct ArgView { int kind = 0, field = 0, value = 0; bool editable = false; std::string label, tip, value_name; std::string text; int size = 0; int tag = -1; };
    std::vector<ArgView> argv;
  };
  // A move as frames: its top-level sprite lines in order
  struct CellView {
    std::string sprite;
    int line = -1;          // its sprite line, an index into the script
    int dur = 0, first = 0; // frames, and its first frame (0-based)
    int hurts = -1, hits = -1;  // the sprite's boxes (-1: the box file wasn't found)
    int phase = 0;          // 0 startup, 1 active, 2 attack on without a hitbox, 3 recovery, 4 can act again
    bool held = false;      // shown until an event (a huge time)
  };
  struct FramesView {
    std::vector<CellView> cells;
    int total = 0;
    bool linear = true;
    std::string note;       // why the strip stops (a label, a goto...), "" if it doesn't
    int now = -1;           // the frame playing, while it's the move playing
    // its frame data, -1 where not known
    int startup = -1, active = -1, recovery = -1;
    int last_startup = -1, last_active = -1, last_recovery = -1;  // the last cell of each (an index into cells): where a change goes
    struct Level { int line = -1, level = -1; std::string event; };  // a cmn_AtkLvN call; event: the upon block it's in ("" in the move's own setup, "HIT"...)
    std::vector<Level> levels;
    bool launches = false;  // an attack angle: on hit the opponent floats, so on hit isn't the table's
    bool multi = false;     // more than one hit (or level): on block / on hit not worked out (the frame bar has them)
    bool has_adv = false;
    int on_block = 0, on_hit = 0;  // (has_adv) the table's blockstun / standing hitstun for the first level, minus active - 1 + recovery
  };
  struct ObjView { const void* id = nullptr; std::string sprite, state; };  // an object of the fighter's (a projectile...)
  struct SideView {
    bool present = false;
    std::vector<ObjView> objects;    // the fighter's objects out now
    int object = -1;                 // the one shown instead of the fighter (an index into objects), -1 the fighter
    std::string chr, state, sprite;  // "sol", "NmlAtk5A", "sol100_02"
    int sprite_frame = 0, sprite_frames = 0;
    int sx = 1, sy = 1;              // the signs of the fighter's scale (which way +x and +y point on screen)
    std::vector<BoxView> boxes;      // the sprite's boxes, as the game reads them
    bool push_on = false;
    int push[4] = {};                // the collision (push) box the game works out: left, top, right, bottom
    int throw_range = -1;            // -1 when the move can't throw
    bool strike_invuln = false, throw_invuln = false;
    std::vector<InstrView> script;   // the state's instructions
    int current = -1;                // the one the game runs next, an index into script
    std::vector<std::string> states; // every move (state) in the fighter's script file, in its order
    std::vector<std::string> routines;  // its routines (RoundInit holds the move definitions: inputs, follow-ups), openable like moves
    std::vector<std::string> obj_states, obj_routines;
    std::string obj_note;
    std::string selected;            // the move opened in the small panel ("" none)
    std::vector<InstrView> sel_script;
    int sel_current = -1;            // its running line while it's the move playing, else -1
    FramesView sel_frames;           // its frames
    // the game's battle versions of the opened move: each group of versions with the same script
    struct VersionView { std::string label, folder; bool now = false; };  // "3.06-3.07", "306"; now: today's script
    std::vector<VersionView> sel_versions;
    std::string sel_loaded;          // the version loaded into it ("" the game's own)
    std::string sel_file;
    int sel_kind = 0;
    std::vector<uint8_t> sel_code;
    std::vector<char> sel_added;
    int sel_paste_id = 0;
    std::string sel_off;
    struct UnitView { std::string key; bool found = false; std::vector<InstrView> script; std::vector<uint8_t> code; std::vector<char> added; int paste_id = 0; };
    std::vector<UnitView> inl;
    std::string note;                // why something couldn't be read
  };
  struct View {
    bool open = false;
    bool db = false;       // the instruction database was read
    std::vector<std::string> shared_routines;  // the common script's routines, by name
    std::vector<std::string> shared_objects;
    std::string db_note;   // where it was looked for, or how many instructions it has
    SideView side[2];
    std::vector<EditedView> edits;  // every sprite with changed boxes
    bool keep = true;               // edits are put back on new training fights
    bool can_undo = false, can_redo = false;
    std::vector<std::string> history;
    int history_at = 0;
    std::string status;
  };

  void init(const std::filesystem::path& ue4ss_dir);
  void toggle();          // Ctrl+Alt+B, or the window's close box
  bool isOpen();
  void tick();            // game thread, in a training fight
  void setOldHurtboxes(bool on);
  void outsideTraining(); // game thread, each battle update outside training: the game's own boxes go back
  View view();
  void showObject(int side, const void* id);  // one of the side's objects instead of the fighter (nullptr: the fighter)
  void selectState(int side, const std::string& name);  // the move shown in the small panel ("" closes it)
  void setInline(int side, const std::vector<std::string>& keys);
  void exportState(int side);  // the opened move as .bbs, into StriveLabs_scripts/<CHR>/
  void exportMove(int side, const std::string& name);  // a move, routine or object state by its list name
  void exportBoxes(int side);  // the sprite's boxes as COL JSON
  void exportPak(int side);
  const char* kindName(int kind);  // "Hurtbox", "Hitbox"...
  // Box edits (the UI asks, the game thread writes). index is into SideView::boxes; sprite says which one it was
  // asked for, so a request that comes after the sprite changed is dropped. undo_point: the start of a drag or a typed
  // value (one Ctrl+Z puts back the whole drag).
  void editBox(int side, const std::string& sprite, int index, const BoxView& b, bool undo_point);
  void resetBox(int side, const std::string& sprite, int index);
  // The box's values (fields: 1 X, 2 Y, 4 W, 8 H) to the same box (same kind, same number) of other sprites of the
  // fighter, on screen or not. One undo step for all of them; join: the same step as the last edit (a drag, then this).
  void copyBox(int side, const std::string& sprite, int index, int fields, const std::vector<std::string>& to, bool join = false);
  void resetSprite(const std::string& chr, const std::string& sprite);
  // Script edits on the move opened in the Move panel (side, its name), by the line's offset in the state: a sprite
  // line's frames, an attack level (0-4, a cmn_AtkLvN call). Same size, so nothing moves; they last like box edits.
  void setFrames(int side, const std::string& state, int offset, int frames);
  void setLevel(int side, const std::string& state, int offset, int level);
  void setArg(int side, const std::string& state, int offset, int field, int value);  // a number of a line (an ArgView's field)
  void resetMove(int side, const std::string& state);
  void resetLine(int side, const std::string& state, int offset);
  // A line replaced by another instruction of the same size (its numbers start at 0; set them with setArg). Replacing
  // it with the instruction it was puts it back. replacements: the instructions that can take the place of op's line
  // (op: the line's own, or the one it was if replaced), {op, name}, by name.
  void replaceLine(int side, const std::string& state, int offset, int op);
  std::vector<std::pair<int, std::string>> replacements(int op);
  // Everything edited (boxes, script lines, versions loaded into moves), saved under a name / put back on
  // (StriveLabs_scripts/edits/<name>.txt). A loaded edit goes on only where the game's value is still the one it changed.
  void saveEdits(const std::string& name);
  void loadEdits(const std::string& name);
  std::vector<std::string> savedEdits();  // the saved names (UI thread)
  // (experimental) the move as it was in another battle version (a folder of Data/), "" back to today's
  void loadVersion(int side, const std::string& state, const std::string& folder);
  // The move from a .bbs text file in StriveLabs_scripts/<CHR>/ (as Export state writes them; edit it, then
  // load it): moveFiles lists {file name, the move its first line names}
  void loadMoveFile(int side, const std::string& state, const std::string& file);
  std::vector<std::pair<std::string, std::string>> moveFiles(const std::string& chr);
  void reloadMoveFiles();
  void shownSide(int side);
  inline constexpr char kObjectPrefix[] = "obj:";
  inline constexpr char kSharedPrefix[] = "cmn:";
  inline constexpr char kCommonObjPrefix[] = "cmnobj:";
  void setName(int side, const std::string& state, int offset, int field, const std::string& name);
  void insertLine(int side, const std::string& state, int offset, int op);
  void copyLine(int side, const std::string& state, int offset);
  void removeLine(int side, const std::string& state, int offset);
  void setValueKind(int side, const std::string& state, int offset, int field, int tag);
  void pasteLines(int side, const std::string& state, int offset, const std::string& text);
  std::string linesText(const std::vector<uint8_t>& code, int offset);
  std::vector<int> removalPreview(const std::vector<uint8_t>& code, int offset);
  bool isFavourite(const std::string& chr, const std::string& key);
  void setFavourite(const std::string& chr, const std::string& key, bool on);
  std::vector<std::string> favourites(const std::string& chr);
  std::vector<std::string> recent(const std::string& chr);
  std::vector<std::pair<int, std::string>> variables();
  std::vector<std::pair<int, std::string>> insertable();
  bool setMoveLabel(const std::string& chr, const std::string& state, const std::string& label);
  std::map<std::string, std::string> moveLabels(const std::string& chr);
  void resetAll();
  void undo();
  void redo();
  void historyGo(int step);
  void setKeep(bool keep);
  void setHighlight(int side, int index);  // the box hovered or picked in the table, -1 none
  bool highlighted(const void* entity, int& index);  // for the hitbox drawing: is a box of this entity highlighted
  // Where a fighter's box units land on screen, as parts of the viewport: box point (x, y) is at
  // (ox + ax*x + bx*y, oy + ay*x + by*y). Handed over by the hitbox drawing, so only while Hitboxes is on.
  struct ScreenMap { float ox = 0, oy = 0, ax = 0, ay = 0, bx = 0, by = 0; };
  void setScreenMap(const void* entity, const ScreenMap& m);
  bool screenMap(int side, ScreenMap& m);  // false unless it was drawn in the last 200 ms
}

namespace box_editor::lists {
  struct Entry { std::string chr, key; };
  inline bool entryOk(const Entry& e) {
    if (e.key.empty() || e.key.size() > 48) return false;
    if (e.chr != "CMN" && (e.chr.size() != 3 || !std::all_of(e.chr.begin(), e.chr.end(), [](char ch) { return ch >= 'A' && ch <= 'Z'; }))) return false;
    for (char ch : e.key) if (ch < 0x20 || ch > 0x7E) return false;
    return (e.chr == "CMN") == (e.key.rfind("cmn:", 0) == 0 || e.key.rfind("cmnobj:", 0) == 0);
  }
  inline std::string write(const std::vector<Entry>& v) {
    std::string out;
    for (const Entry& e : v) if (entryOk(e)) out += e.chr + "\t" + e.key + "\n";
    return out;
  }
  inline std::vector<Entry> read(const std::string& text) {
    std::vector<Entry> out;
    std::istringstream is(text);
    std::string l;
    while (std::getline(is, l)) {
      if (!l.empty() && l.back() == '\r') l.pop_back();
      const size_t t = l.find('\t');
      if (t == std::string::npos || l.find('\t', t + 1) != std::string::npos) continue;
      Entry e{l.substr(0, t), l.substr(t + 1)};
      if (!entryOk(e)) continue;
      bool dup = false;
      for (const Entry& o : out) if (o.chr == e.chr && o.key == e.key) dup = true;
      if (!dup) out.push_back(std::move(e));
    }
    return out;
  }
  inline void touch(std::vector<Entry>& v, const Entry& e, size_t cap) {
    if (!entryOk(e)) return;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const Entry& o) { return o.chr == e.chr && o.key == e.key; }), v.end());
    v.insert(v.begin(), e);
    if (v.size() > cap) v.resize(cap);
  }
  inline void toggle(std::vector<Entry>& v, const Entry& e, bool on) {
    if (!entryOk(e)) return;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const Entry& o) { return o.chr == e.chr && o.key == e.key; }), v.end());
    if (on) v.push_back(e);
  }
  inline std::vector<std::string> keysFor(const std::vector<Entry>& v, const std::string& chr, size_t most) {
    std::vector<std::string> out;
    for (const Entry& e : v)
      if ((e.chr == chr || e.chr == "CMN") && out.size() < most) out.push_back(e.key);
    return out;
  }
}

namespace box_editor::ops {
  enum class Arg { Number, String16, String32, Value, Enum };
  struct ArgDef { Arg kind = Arg::Number; std::string enum_name; };
  struct Instr { int size = 0; std::string name; int block = 0; std::vector<ArgDef> args; std::string desc; };
  struct Db {
    bool ok = false;
    std::map<int, Instr> instr;
    std::map<int, std::string> vars;
    std::map<std::string, std::map<int, std::string>> enums;
    std::map<std::string, int> by_name;
    std::map<int, int> ends;
  };
  struct Ctx { const short* sizes = nullptr; int max = 0; const Db* db = nullptr; };
  using Bytes = std::vector<uint8_t>;
  constexpr size_t npos = static_cast<size_t>(-1);
  inline constexpr char kFirstLine[] = "The first line is where the game's table points: without it there is no start, so it stays.";
  inline constexpr char kLastLine[] = "The last line is where the game stops reading: without it the game would run on into memory that isn't script, so it stays.";
  inline constexpr char kAfterLast[] = "Nothing runs after the last line (the game stops reading there): pick an earlier line.";
  inline constexpr char kGone[] = "That line isn't there any more: pick it again.";

  inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == ',')) b--;
    return s.substr(a, b - a);
  }
  inline std::string quoted(const std::string& s) {
    const size_t a = s.find('"'), b = a == std::string::npos ? a : s.find('"', a + 1);
    return a == std::string::npos || b == std::string::npos ? std::string() : s.substr(a + 1, b - a - 1);
  }
  inline void nameOps(Db& db) {
    db.by_name.clear();
    for (const auto& [o, in] : db.instr) if (!in.name.empty()) db.by_name.emplace(in.name, o);
    db.by_name.emplace("tensionGainReduce", 309);
    db.by_name.emplace("riscGainPercent", 1329);
    db.ends.clear();
    for (const auto& [o, in] : db.instr) {
      if (in.block <= 0 || in.name.empty() || o == 0 || o == 15 || o == 77 || o == 78) continue;
      std::vector<std::string> cands;
      std::string cap = in.name;
      cap[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(cap[0])));
      cands.push_back("end" + cap);
      if (in.name.rfind("if", 0) == 0) cands.push_back("endIf");
      if (in.name.size() > 5 && in.name.compare(in.name.size() - 5, 5, "Begin") == 0) cands.push_back(in.name.substr(0, in.name.size() - 5) + "End");
      size_t k = 0;
      while (k < in.name.size() && std::islower(static_cast<unsigned char>(in.name[k]))) k++;
      if (k > 0 && k < in.name.size()) cands.push_back("end" + in.name.substr(k));
      for (const std::string& n : cands) {
        const auto it = db.by_name.find(n);
        if (it == db.by_name.end()) continue;
        const auto e = db.instr.find(it->second);
        if (e != db.instr.end() && e->second.block < 0) { db.ends[o] = it->second; break; }
      }
    }
  }
  inline bool readRon(const std::filesystem::path& file, Db& db) {
    std::ifstream f(file);
    if (!f) return false;
    std::string line, map_name;
    enum { None, Vars, Maps, Map, Instrs, Args } where = None;
    int id = -1;
    Instr cur;
    auto flush = [&] { if (id >= 0) db.instr[id] = cur; id = -1; cur = Instr{}; };
    while (std::getline(f, line)) {
      const std::string t = trim(line);
      if (t.empty()) continue;
      if (t.rfind("named_variables:", 0) == 0) { where = Vars; continue; }
      if (t.rfind("named_value_maps:", 0) == 0) { where = Maps; continue; }
      if (t.rfind("instructions:", 0) == 0) { where = Instrs; continue; }
      switch (where) {
        case Vars:
          if (t == "}") { where = None; break; }
          if (const size_t c = t.find(':'); c != std::string::npos) db.vars[std::atoi(t.c_str())] = quoted(t.substr(c));
          break;
        case Maps:
          if (t == "}") { where = None; break; }
          if (t.back() == '{') { map_name = quoted(t); where = Map; }
          break;
        case Map:
          if (t == "}") { where = Maps; break; }
          if (const size_t c = t.find(':'); c != std::string::npos) db.enums[map_name][std::atoi(t.c_str())] = quoted(t.substr(c));
          break;
        case Instrs:
          if (t.size() > 1 && t.back() == '(' && t.find(':') != std::string::npos && t[0] >= '0' && t[0] <= '9') { flush(); id = std::atoi(t.c_str()); }
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
            if (t.find(']') != std::string::npos) where = Instrs;
          }
          break;
        default: break;
      }
    }
    flush();
    nameOps(db);
    return !db.instr.empty();
  }
  inline int32_t rd(const uint8_t* p) { int32_t v; std::memcpy(&v, p, 4); return v; }
  inline void wr(uint8_t* p, int32_t v) { std::memcpy(p, &v, 4); }
  inline std::string cstr(const uint8_t* p, size_t n) { size_t k = 0; while (k < n && p[k]) k++; return std::string(reinterpret_cast<const char*>(p), k); }
  inline int argBytes(Arg k) { return k == Arg::String32 ? 32 : k == Arg::String16 ? 16 : k == Arg::Value ? 8 : 4; }
  inline const Instr* def(const Ctx& c, int op) {
    if (!c.db) return nullptr;
    const auto it = c.db->instr.find(op);
    return it == c.db->instr.end() ? nullptr : &it->second;
  }
  inline int sizeOf(const Ctx& c, int op) { return c.sizes && op >= 0 && op < c.max && c.sizes[op] >= 4 && c.sizes[op] % 4 == 0 ? c.sizes[op] : -1; }
  inline int blockOf(const Ctx& c, int op) { const Instr* d = def(c, op); return d ? d->block : 0; }
  inline std::string nameOf(const Ctx& c, int op) { const Instr* d = def(c, op); return d && !d->name.empty() ? d->name : "Unknown" + std::to_string(op); }
  inline bool unitBegin(int op) { return op == 0 || op == 15 || op == 77 || op == 78; }
  inline bool unitEnd(int op) { return op == 1 || op == 16; }
  inline bool gotoOp(int op) { return op == 12 || op == 13 || op == 14 || op == 24 || op == 25 || op == 31 || op == 35; }
  inline int labelAt(int op) { return op == 35 ? 8 : 4; }
  inline int endFor(const Ctx& c, int op) {
    if (!c.db) return -1;
    const auto it = c.db->ends.find(op);
    return it == c.db->ends.end() ? -1 : it->second;
  }
  inline std::string argsOf(const Db& db, const uint8_t* p, int size, const Instr* d) {
    std::string out;
    int at = 4;
    auto add = [&](const std::string& s) { if (!out.empty()) out += ", "; out += s; };
    auto esc = [](const std::string& s) { std::string o; for (char ch : s) { if (ch == '\'') o += '\\'; o += ch; } return o; };
    if (d)
      for (const ArgDef& a : d->args) {
        const int need = argBytes(a.kind);
        if (at + need > size) break;
        switch (a.kind) {
          case Arg::Number: add(std::to_string(rd(p + at))); break;
          case Arg::String16: add("s16'" + esc(cstr(p + at, 16)) + "'"); break;
          case Arg::String32: add("s32'" + esc(cstr(p + at, 32)) + "'"); break;
          case Arg::Value: {
            const int tag = rd(p + at), v = rd(p + at + 4);
            if (tag == 0) add("Val(" + std::to_string(v) + ")");
            else if (tag == 2) { auto it = db.vars.find(v); add("Mem(" + (it != db.vars.end() ? it->second : std::to_string(v)) + ")"); }
            else add("BadTag(" + std::to_string(tag) + ", " + std::to_string(v) + ")");
            break;
          }
          case Arg::Enum: {
            const int v = rd(p + at);
            auto m = db.enums.find(a.enum_name);
            bool once = false;
            std::string nm;
            if (m != db.enums.end())
              if (auto it = m->second.find(v); it != m->second.end()) {
                int n = 0;
                for (const auto& kv : m->second) if (kv.second == it->second) n++;
                once = n == 1;
                nm = it->second;
              }
            add(once ? "(" + nm + ")" : std::to_string(v));
            break;
          }
        }
        at += need;
      }
    if (at < size) {
      std::string h = "0x";
      char b[3];
      for (int k = at; k < size; k++) { snprintf(b, sizeof(b), "%02X", p[k]); h += b; }
      add(h);
    }
    return out;
  }
  inline bool parseInt(const std::string& t, int& v) {
    if (t.empty()) return false;
    char* end = nullptr;
    const long long x = std::strtoll(t.c_str(), &end, 10);
    if (!end || *end != 0 || x < INT32_MIN || x > 0xFFFFFFFFll) return false;
    v = static_cast<int>(static_cast<uint32_t>(x));
    return true;
  }
  inline bool walks(const Ctx& c, const Bytes& b) {
    if (!c.sizes || b.size() < 4) return false;
    const int end_op = rd(b.data()) != 0 ? 16 : 1;
    int last = -1;
    size_t p = 0;
    while (p + 4 <= b.size()) {
      const int op = rd(b.data() + p);
      const int sz = sizeOf(c, op);
      if (sz < 4 || p + static_cast<size_t>(sz) > b.size()) return false;
      last = op;
      p += static_cast<size_t>(sz);
    }
    return p == b.size() && last == end_op;
  }
  inline bool formed(const Ctx& c, const Bytes& b, std::string& err) {
    if (!c.sizes) { err = "no instruction sizes"; return false; }
    if (b.size() < 4) { err = "no lines at all"; return false; }
    const int first = rd(b.data());
    const bool routine = first != 0;
    const int end_op = routine ? 16 : 1;
    int depth = 0;
    for (size_t p = 0; p + 4 <= b.size();) {
      const int op = rd(b.data() + p);
      const int sz = sizeOf(c, op);
      if (sz < 0) { err = "an instruction the game doesn't know (" + std::to_string(op) + ")"; return false; }
      if (op != end_op && op != first) depth = (std::max)(0, depth + blockOf(c, op));
      p += static_cast<size_t>(sz);
      if (op == end_op) {
        if (p != b.size()) { err = std::string("lines after ") + (routine ? "endSubroutine" : "endState"); return false; }
        if (depth == 1) { err = "a block (an if, upon...) without its end line"; return false; }
        if (depth > 1) { err = std::to_string(depth) + " blocks (if, upon...) without their end lines"; return false; }
        return true;
      }
    }
    err = routine ? "no endSubroutine at the end" : "no endState at the end";
    return false;
  }
  inline std::vector<size_t> lineStarts(const Ctx& c, const Bytes& b) {
    std::vector<size_t> out;
    for (size_t p = 0; p + 4 <= b.size();) {
      const int sz = sizeOf(c, rd(b.data() + p));
      if (sz < 0 || p + static_cast<size_t>(sz) > b.size()) break;
      out.push_back(p);
      p += static_cast<size_t>(sz);
    }
    return out;
  }
  inline std::string textOf(const Ctx& c, const Bytes& b) {
    std::string out;
    int depth = 0;
    for (size_t p : lineStarts(c, b)) {
      const int op = rd(b.data() + p);
      const int blk = blockOf(c, op);
      if (blk < 0 && depth > 0) depth--;
      out += std::string(static_cast<size_t>(op == 1 ? 0 : depth) * 2, ' ') + nameOf(c, op) + ": " + argsOf(*c.db, b.data() + p, sizeOf(c, op), def(c, op)) + "\n";
      if (blk > 0) depth++;
    }
    return out;
  }
  inline bool assembleRange(const Ctx& c, const std::string& text, int from, int to, Bytes& out, std::string& err, std::vector<int>* lines = nullptr) {
    if (!c.sizes || !c.db || c.db->instr.empty()) { err = "no instruction database"; return false; }
    const Db& db = *c.db;
    std::istringstream is(text);
    std::string l;
    int ln = 0;
    while (std::getline(is, l)) {
      ln++;
      if (ln < from || ln > to) continue;
      l = trim(l);
      if (l.empty() || l[0] == '#' || l.rfind("//", 0) == 0) continue;
      auto bad = [&](const std::string& why) { err = "line " + std::to_string(ln) + ": " + why; return false; };
      const size_t colon = l.find(':');
      if (colon == std::string::npos) return bad("a line is 'name: values', and this one has no ':'.");
      const std::string name = trim(l.substr(0, colon));
      int op = -1;
      auto it = db.by_name.find(name);
      if (it != db.by_name.end()) op = it->second;
      else if (name.rfind("Unknown", 0) == 0 && !parseInt(name.substr(7), op)) op = -1;
      if (name.empty() || op < 0 || op >= c.max) return bad("'" + name + "' is not an instruction name.");
      const int size = sizeOf(c, op);
      if (size < 4) return bad(name + ": the game has no size for this instruction, so it can't be used.");
      int want = 0;
      {
        int a2 = 4;
        if (const Instr* d = def(c, op))
          for (const ArgDef& a : d->args) {
            if (a2 + argBytes(a.kind) > size) break;
            want++;
            a2 += argBytes(a.kind);
          }
        if (a2 < size) want++;
      }
      std::vector<std::string> tok;
      {
        const std::string rest = l.substr(colon + 1);
        std::string cur;
        int paren = 0;
        bool quote = false;
        for (size_t i = 0; i < rest.size(); i++) {
          const char ch = rest[i];
          if (quote) { cur += ch; if (ch == '\\' && i + 1 < rest.size()) cur += rest[++i]; else if (ch == '\'') quote = false; continue; }
          if (ch == '\'') quote = true;
          if (ch == '(') paren++;
          if (ch == ')') paren--;
          if (ch == ',' && paren == 0) { tok.push_back(trim(cur)); cur.clear(); continue; }
          cur += ch;
        }
        if (!trim(cur).empty() || !tok.empty()) tok.push_back(trim(cur));
      }
      auto counted = [&]() { return name + " takes " + std::to_string(want) + (want == 1 ? " value" : " values") + ", the line has " + std::to_string(tok.size()) + "."; };
      Bytes b(static_cast<size_t>(size), 0);
      wr(b.data(), op);
      int at = 4;
      size_t ti = 0;
      if (const Instr* d = def(c, op))
        for (const ArgDef& a : d->args) {
          const int need = argBytes(a.kind);
          if (at + need > size) break;
          if (ti >= tok.size()) return bad(counted());
          const std::string t = tok[ti++];
          int v = 0;
          switch (a.kind) {
            case Arg::Number:
              if (!parseInt(t, v)) return bad(name + ", value " + std::to_string(ti) + ": '" + t + "' should be a whole number.");
              wr(b.data() + at, v);
              break;
            case Arg::String16:
            case Arg::String32: {
              const std::string pre = a.kind == Arg::String16 ? "s16'" : "s32'";
              if (t.rfind(pre, 0) != 0 || t.size() < 5 || t.back() != '\'') return bad(name + ", value " + std::to_string(ti) + ": '" + t + "' should be a name written " + pre + "...'.");
              std::string s;
              for (size_t i = 4; i + 1 < t.size(); i++) { if (t[i] == '\\' && i + 2 < t.size()) i++; s += t[i]; }
              if (static_cast<int>(s.size()) >= need) return bad(name + ", value " + std::to_string(ti) + ": '" + s + "' is " + std::to_string(s.size()) + " letters, and this name holds " + std::to_string(need - 1) + " at most.");
              std::memcpy(b.data() + at, s.data(), s.size());
              break;
            }
            case Arg::Value: {
              int tag = 0;
              if (t.rfind("Val(", 0) == 0 && t.back() == ')') { if (!parseInt(t.substr(4, t.size() - 5), v)) return bad(name + ", value " + std::to_string(ti) + ": '" + t + "' should be Val(a whole number)."); }
              else if (t.rfind("Mem(", 0) == 0 && t.back() == ')') {
                tag = 2;
                const std::string vn = t.substr(4, t.size() - 5);
                if (!parseInt(vn, v)) {
                  bool found = false;
                  for (const auto& [id, n] : db.vars) if (n == vn) { v = id; found = true; break; }
                  if (!found) return bad(name + ", value " + std::to_string(ti) + ": there is no variable called " + vn + ".");
                }
              } else if (t.rfind("BadTag(", 0) == 0 && t.back() == ')') {
                const size_t cm = t.find(',');
                if (cm == std::string::npos || !parseInt(trim(t.substr(7, cm - 7)), tag) || !parseInt(trim(t.substr(cm + 1, t.size() - cm - 2)), v)) return bad(name + ", value " + std::to_string(ti) + ": '" + t + "' should be BadTag(a whole number, a whole number).");
              } else if (parseInt(t, tag) && ti < tok.size() && parseInt(tok[ti], v)) ti++;
              else return bad(name + ", value " + std::to_string(ti) + ": '" + t + "' should be Val(a whole number) or Mem(a variable name or number).");
              wr(b.data() + at, tag);
              wr(b.data() + at + 4, v);
              break;
            }
            case Arg::Enum: {
              if (t.size() > 2 && t.front() == '(' && t.back() == ')') {
                const std::string en = t.substr(1, t.size() - 2);
                auto m = db.enums.find(a.enum_name);
                bool found = false;
                if (m != db.enums.end()) for (const auto& [id, n] : m->second) if (n == en) { v = id; found = true; break; }
                if (!found) return bad(name + ", value " + std::to_string(ti) + ": there is no " + a.enum_name + " called " + en + ".");
              } else if (!parseInt(t, v)) return bad(name + ", value " + std::to_string(ti) + ": '" + t + "' should be a whole number or (a " + a.enum_name + " name).");
              wr(b.data() + at, v);
              break;
            }
          }
          at += need;
        }
      if (at < size) {
        if (ti >= tok.size()) return bad(counted());
        const std::string t = tok[ti++];
        if (t.rfind("0x", 0) != 0 || static_cast<int>(t.size()) != 2 + (size - at) * 2) return bad(name + ", value " + std::to_string(ti) + ": '" + t + "' should be 0x and " + std::to_string((size - at) * 2) + " hex digits (its last bytes).");
        for (int k = 0; k < size - at; k++) {
          const std::string h = t.substr(2 + static_cast<size_t>(k) * 2, 2);
          char* end = nullptr;
          const long x = std::strtol(h.c_str(), &end, 16);
          if (!end || *end) return bad(name + ", value " + std::to_string(ti) + ": '" + h + "' isn't a hex digit pair.");
          b[static_cast<size_t>(at + k)] = static_cast<uint8_t>(x);
        }
      }
      if (ti < tok.size()) return bad(counted());
      out.insert(out.end(), b.begin(), b.end());
      if (lines) lines->push_back(ln);
    }
    return true;
  }
  inline std::string baseName(const std::string& n, int& op) {
    op = 15;
    for (const auto& [suf, o] : {std::pair<const char*, int>{" (pre)", 77}, {" (post)", 78}}) {
      const size_t k = std::strlen(suf);
      if (n.size() > k && n.compare(n.size() - k, k, suf) == 0) { op = o; return n.substr(0, n.size() - k); }
    }
    return n;
  }
  inline bool assemble(const Ctx& c, const std::string& text, const std::string& want, bool routine, Bytes& out, std::string& err, std::vector<int>* lines = nullptr) {
    if (!c.sizes || !c.db || c.db->instr.empty()) { err = "no instruction database"; return false; }
    int begin_op = 0;
    const std::string base = routine ? baseName(want, begin_op) : want;
    if (!routine) begin_op = 0;
    const int end_op = routine ? 16 : 1;
    struct Block { int from = 0, to = 0; std::string name; };
    std::vector<Block> blocks;
    {
      std::istringstream is(text);
      std::string l;
      int ln = 0;
      size_t open = 0;
      while (std::getline(is, l)) {
        ln++;
        l = trim(l);
        if (l.empty() || l[0] == '#' || l.rfind("//", 0) == 0) continue;
        const size_t colon = l.find(':');
        if (colon == std::string::npos) continue;
        const auto it = c.db->by_name.find(trim(l.substr(0, colon)));
        if (it == c.db->by_name.end()) continue;
        const int op = it->second;
        if (unitBegin(op) && op != begin_op) {
          for (; open < blocks.size(); open++) blocks[open].to = ln - 1;
        } else if (op == begin_op) {
          const size_t a = l.find("s32'", colon);
          const size_t b = a == std::string::npos ? a : l.find('\'', a + 4);
          Block k;
          k.from = ln;
          if (b != std::string::npos) k.name = l.substr(a + 4, b - a - 4);
          blocks.push_back(k);
        } else if (op == end_op) {
          for (; open < blocks.size(); open++) blocks[open].to = ln;
        }
      }
      for (; open < blocks.size(); open++) blocks[open].to = ln;
    }
    const std::string what = routine ? "routine" : "move";
    const std::string begin_name = nameOf(c, begin_op), end_name = nameOf(c, end_op);
    const Block* pick = nullptr;
    for (const Block& b : blocks) if (b.name == base) { pick = &b; break; }
    if (!pick) {
      if (blocks.empty()) err = "the file has no " + begin_name + ": s32'" + base + "' line";
      else if (blocks.size() == 1) err = "the file is " + blocks[0].name + ", not " + base;
      else err = "no " + what + " called " + base + " in the file (it has " + std::to_string(blocks.size()) + " " + what + "s)";
      return false;
    }
    if (!assembleRange(c, text, pick->from, pick->to, out, err, lines)) return false;
    const int end_size = sizeOf(c, end_op);
    if (out.size() < 36 || rd(out.data()) != begin_op) { err = "the " + what + " should start with " + begin_name + ": s32'" + base + "'"; return false; }
    if (end_size < 4 || out.size() < static_cast<size_t>(end_size) || rd(out.data() + out.size() - static_cast<size_t>(end_size)) != end_op) {
      err = "line " + std::to_string(pick->to) + ": the " + what + " should end with " + end_name + ":";
      return false;
    }
    return true;
  }
  inline size_t blockEnd(const Ctx& c, const Bytes& b, size_t at) {
    int depth = 0;
    for (size_t p = at; p + 4 <= b.size();) {
      const int op = rd(b.data() + p);
      const int sz = sizeOf(c, op);
      if (sz < 0 || unitEnd(op) || (p != at && unitBegin(op))) return npos;
      const int blk = blockOf(c, op);
      if (blk > 0) depth++;
      else if (blk < 0) depth--;
      p += static_cast<size_t>(sz);
      if (depth <= 0) return p;
    }
    return npos;
  }
  inline size_t ownerOf(const Ctx& c, const Bytes& b, size_t at) {
    std::vector<size_t> open;
    for (size_t p : lineStarts(c, b)) {
      if (p == at) return open.empty() ? npos : open.back();
      const int op = rd(b.data() + p);
      if (unitBegin(op) || unitEnd(op)) continue;
      const int blk = blockOf(c, op);
      if (blk > 0) open.push_back(p);
      else if (blk < 0 && !open.empty()) open.pop_back();
    }
    return npos;
  }
  inline size_t elseEnd(const Ctx& c, const Bytes& b, size_t at) {
    while (at != npos && at + 4 <= b.size() && rd(b.data() + at) == 9) {
      const size_t e = blockEnd(c, b, at);
      if (e == npos) break;
      at = e;
    }
    return at;
  }
  inline size_t constructStart(const Ctx& c, const Bytes& b, size_t at) {
    for (int guard = 0; guard < 64 && at != npos && rd(b.data() + at) == 9; guard++) {
      size_t prev = npos;
      for (size_t p : lineStarts(c, b)) { if (p >= at) break; prev = p; }
      if (prev == npos || blockOf(c, rd(b.data() + prev)) >= 0) break;
      const size_t o = ownerOf(c, b, prev);
      if (o == npos) break;
      at = o;
    }
    return at;
  }
  inline std::vector<std::pair<size_t, std::string>> labelsIn(const Ctx& c, const Bytes& b) {
    std::vector<std::pair<size_t, std::string>> out;
    for (size_t p : lineStarts(c, b))
      if (rd(b.data() + p) == 11 && sizeOf(c, 11) >= 36) out.push_back({p, cstr(b.data() + p + 4, 32)});
    return out;
  }
  inline std::vector<std::pair<size_t, std::string>> gotosIn(const Ctx& c, const Bytes& b) {
    std::vector<std::pair<size_t, std::string>> out;
    for (size_t p : lineStarts(c, b)) {
      const int op = rd(b.data() + p);
      if (gotoOp(op) && sizeOf(c, op) >= labelAt(op) + 32) out.push_back({p, cstr(b.data() + p + labelAt(op), 32)});
    }
    return out;
  }
  inline std::set<std::string> missingLabels(const Ctx& c, const Bytes& b) {
    std::set<std::string> have, out;
    for (const auto& l : labelsIn(c, b)) have.insert(l.second);
    for (const auto& g : gotosIn(c, b)) if (!have.count(g.second)) out.insert(g.second);
    return out;
  }
  struct Problem { size_t line = 0; std::string key, text; };
  inline std::string withArticle(const std::string& n) {
    return (!n.empty() && std::strchr("aeiouAEIOU", n[0]) ? "an " : "a ") + n;
  }
  inline std::string lineRef(size_t i, const std::vector<int>* fl) {
    std::string r = "Line " + std::to_string(i + 1);
    if (fl && i < fl->size() && (*fl)[i] > 0) r += " (file line " + std::to_string((*fl)[i]) + ")";
    return r;
  }
  inline std::vector<int> openersOf(const Ctx& c, int end_op) {
    std::vector<int> out;
    if (c.db) for (const auto& [o, e] : c.db->ends) if (e == end_op) out.push_back(o);
    return out;
  }
  inline std::vector<Problem> problems(const Ctx& c, const Bytes& b, const std::vector<int>* fl = nullptr) {
    std::vector<Problem> out;
    const std::vector<size_t> ls = lineStarts(c, b);
    if (ls.empty()) {
      out.push_back({0, "empty", "Line 1: the script has no line the game can read."});
      return out;
    }
    const int first = rd(b.data());
    const bool routine = first != 0;
    const int end_op = routine ? 16 : 1;
    const std::string unit = routine ? "routine" : "move";
    std::set<std::string> labels;
    for (const auto& l : labelsIn(c, b)) labels.insert(l.second);
    struct Open { size_t i; int op; };
    std::vector<Open> open;
    bool ended = false;
    size_t covered = 0;
    for (size_t i = 0; i < ls.size(); i++) {
      const size_t p = ls[i];
      const int op = rd(b.data() + p);
      const std::string nm = nameOf(c, op);
      covered = p + static_cast<size_t>(sizeOf(c, op));
      if (ended) {
        out.push_back({i, "after", lineRef(i, fl) + ", " + nm + ": it comes after an " + nameOf(c, end_op) + ", where the game stops reading, so it never runs (and the editor shows only up to that " + nameOf(c, end_op) + ")."});
        break;
      }
      if (i == 0) continue;
      if (op == end_op) { ended = true; continue; }
      if (op == first) {
        out.push_back({i, "inner", lineRef(i, fl) + ", " + nm + ": a second " + nm + " inside starts another " + unit + " here, so the game may run the lines after it as that one."});
        continue;
      }
      const int blk = blockOf(c, op);
      if (op == 9) {
        const int prev = rd(b.data() + ls[i - 1]);
        if (prev != 5)
          out.push_back({i, "else", lineRef(i, fl) + ", else: an else has to come right after an endIf (the end of an if), and line " + std::to_string(i) + " is " + nameOf(c, prev) + "."});
      }
      if (blk > 0) open.push_back({i, op});
      else if (blk < 0) {
        if (open.empty()) {
          const std::vector<int> ops = openersOf(c, op);
          const std::string who = ops.empty() ? std::string("block") : nameOf(c, ops.front());
          std::string also;
          if (ops.size() > 1) also = " (or another line it ends, like " + nameOf(c, ops[1]) + ")";
          out.push_back({i, "stray:" + std::to_string(op), lineRef(i, fl) + ", " + nm + ": this closes " + withArticle(who) + also + ", but no " + who + " is open before it."});
        } else {
          const Open o = open.back();
          open.pop_back();
          const int want = endFor(c, o.op);
          if (want >= 0 && want != op)
            out.push_back({i, "pair:" + std::to_string(o.op) + ":" + std::to_string(op),
                           lineRef(i, fl) + ", " + nm + ": this closes the " + nameOf(c, o.op) + " on line " + std::to_string(o.i + 1) + ", which needs " + withArticle(nameOf(c, want)) + " instead."});
        }
      }
      if (gotoOp(op) && sizeOf(c, op) >= labelAt(op) + 32) {
        const std::string t = cstr(b.data() + p + labelAt(op), 32);
        if (!labels.count(t)) out.push_back({i, "goto:" + t, lineRef(i, fl) + ", " + nm + " " + t + ": there is no label called " + t + " in this " + unit + "."});
      }
    }
    if (covered < b.size() && !ended)
      out.push_back({ls.size(), "unknown", lineRef(ls.size(), fl) + ": instruction " + (covered + 4 <= b.size() ? std::to_string(rd(b.data() + covered)) : std::string("?")) +
                                               " isn't one the game knows, so nothing from here on can be read."});
    else if (!ended)
      out.push_back({ls.size() - 1, "noend", lineRef(ls.size() - 1, fl) + ", " + nameOf(c, rd(b.data() + ls.back())) + ": a " + unit + " has to end with " + nameOf(c, end_op) + ", and this one doesn't."});
    for (const Open& o : open) {
      const int want = endFor(c, o.op);
      out.push_back({o.i, "open:" + std::to_string(o.op),
                     lineRef(o.i, fl) + ", " + nameOf(c, o.op) + ": every " + nameOf(c, o.op) + " needs " + (want >= 0 ? withArticle(nameOf(c, want)) : std::string("an end line")) + " after it, and there is none."});
    }
    std::stable_sort(out.begin(), out.end(), [](const Problem& x, const Problem& y) { return x.line < y.line; });
    return out;
  }
  inline std::string warning(const Ctx& c, const Bytes& before, const Bytes& after, const std::vector<int>* fl = nullptr) {
    std::map<std::string, int> had, seen;
    for (const Problem& p : problems(c, before)) had[p.key]++;
    std::vector<std::string> list;
    for (const Problem& p : problems(c, after, fl)) {
      const auto h = had.find(p.key);
      const int before_n = h == had.end() ? 0 : h->second;
      const bool shape = before_n > 0 && (p.key == "else" || p.key.rfind("pair:", 0) == 0 || p.key.rfind("goto:", 0) == 0);
      if (seen[p.key]++ >= before_n && !shape) list.push_back(p.text);
    }
    if (list.empty()) return {};
    std::string w;
    for (size_t k = 0; k < list.size() && k < 3; k++) w += (w.empty() ? "" : " ") + list[k];
    if (list.size() > 3) w += " And " + std::to_string(list.size() - 3) + " more.";
    return w + " The game may crash or freeze.";
  }
  struct Splice { size_t at = 0, cut = 0; Bytes add; };
  struct Change { std::vector<Splice> sp; std::string done, err; };
  inline Bytes applied(const Bytes& b, std::vector<Splice> sp) {
    std::stable_sort(sp.begin(), sp.end(), [](const Splice& x, const Splice& y) { return x.at < y.at; });
    Bytes out;
    size_t at = 0;
    for (const Splice& s : sp) {
      if (s.at < at || s.at + s.cut > b.size()) return {};
      out.insert(out.end(), b.begin() + static_cast<ptrdiff_t>(at), b.begin() + static_cast<ptrdiff_t>(s.at));
      out.insert(out.end(), s.add.begin(), s.add.end());
      at = s.at + s.cut;
    }
    out.insert(out.end(), b.begin() + static_cast<ptrdiff_t>(at), b.end());
    return out;
  }
  inline long long movedTo(const std::vector<Splice>& sp, size_t off) {
    long long d = 0;
    for (const Splice& s : sp) {
      if (s.cut && off >= s.at && off < s.at + s.cut) return -1;
      if (s.cut == 0) { if (s.at <= off) d += static_cast<long long>(s.add.size()); }
      else if (off >= s.at + s.cut) d += static_cast<long long>(s.add.size()) - static_cast<long long>(s.cut);
    }
    return static_cast<long long>(off) + d;
  }
  inline void putName(uint8_t* p, size_t need, const std::string& n) {
    std::memset(p, 0, need);
    std::memcpy(p, n.data(), (std::min)(n.size(), need - 1));
  }
  inline std::string uniqueLabel(std::set<std::string>& taken) {
    for (int k = 1;; k++) {
      const std::string n = "label" + std::to_string(k);
      if (taken.insert(n).second) return n;
    }
  }
  inline std::set<std::string> labelNames(const Ctx& c, const Bytes& b) {
    std::set<std::string> s;
    for (const auto& l : labelsIn(c, b)) s.insert(l.second);
    return s;
  }
  inline std::string nameNear(const Ctx& c, const Bytes& b, size_t at, int op, int field, int need) {
    std::string before, after;
    for (size_t p : lineStarts(c, b)) {
      if (rd(b.data() + p) != op || static_cast<int>(sizeOf(c, op)) < field + need) continue;
      const std::string n = cstr(b.data() + p + field, static_cast<size_t>(need));
      if (n.empty()) continue;
      if (p < at) before = n;
      else if (after.empty()) after = n;
    }
    return !before.empty() ? before : after;
  }
  using Known = std::function<std::string(int op, int field, int need)>;
  inline bool newLine(const Ctx& c, const Bytes& code, size_t at, size_t from, int op, const Known& known, std::set<std::string>& taken, Bytes& line, Bytes& after, std::string& note,
                      std::string& err) {
    const int sz = sizeOf(c, op);
    if (sz < 0) { err = nameOf(c, op) + " has no size in the game, so it can't go in."; return false; }
    line.assign(static_cast<size_t>(sz), 0);
    wr(line.data(), op);
    after.clear();
    int fat = 4;
    if (const Instr* d = def(c, op))
      for (const ArgDef& a : d->args) {
        const int need = argBytes(a.kind);
        if (fat + need > sz) break;
        if (a.kind == Arg::String16 || a.kind == Arg::String32) {
          std::string n;
          if (op == 11 && fat == 4) {
            n = uniqueLabel(taken);
            note = " (named " + n + ")";
          } else if (gotoOp(op) && fat == labelAt(op)) {
            for (const auto& l : labelsIn(c, code)) if (l.first >= from) { n = l.second; break; }
            if (!n.empty()) note = " (going to the label " + n + ", the next one after it)";
            else if (sizeOf(c, 11) >= 36) {
              n = uniqueLabel(taken);
              Bytes lb(static_cast<size_t>(sizeOf(c, 11)), 0);
              wr(lb.data(), 11);
              putName(lb.data() + 4, 32, n);
              after.insert(after.end(), lb.begin(), lb.end());
              note = " and a label " + n + " right after it for it to go to (no label comes after it)";
            }
          } else {
            n = nameNear(c, code, at, op, fat, need);
            if (n.empty() && known) n = known(op, fat, need);
            note = n.empty() ? std::string(" (its name is empty: no line of the loaded scripts names one there, type one in its name box)") : " (with " + n + ": change it in its name box)";
          }
          putName(line.data() + fat, static_cast<size_t>(need), n);
        }
        fat += need;
      }
    if (op == 2 && sz >= 40) wr(line.data() + 36, 1);
    if (blockOf(c, op) > 0 && !unitBegin(op)) {
      const int e = endFor(c, op);
      if (e >= 0 && sizeOf(c, e) > 0) {
        Bytes eb(static_cast<size_t>(sizeOf(c, e)), 0);
        wr(eb.data(), e);
        after.insert(after.begin(), eb.begin(), eb.end());
      }
    }
    return true;
  }
  inline bool lineOk(const Ctx& c, const Bytes& code, size_t off, Change& ch) {
    const std::vector<size_t> ls = lineStarts(c, code);
    if (!std::binary_search(ls.begin(), ls.end(), off)) { ch.err = kGone; return false; }
    return true;
  }
  inline int linesIn(const Ctx& c, const Bytes& b, size_t from, size_t to) {
    int n = 0;
    for (size_t p : lineStarts(c, b)) if (p >= from && p < to) n++;
    return n;
  }
  inline void cutGotos(const Ctx& c, const Bytes& code, const std::string& label, size_t except, Change& ch, int& n) {
    n = 0;
    int same = 0;
    for (const auto& l : labelsIn(c, code)) if (l.second == label && l.first != except) same++;
    if (same) return;
    for (const auto& g : gotosIn(c, code))
      if (g.second == label) {
        ch.sp.push_back({g.first, static_cast<size_t>(sizeOf(c, rd(code.data() + g.first))), {}});
        n++;
      }
  }
  inline std::string gotoWord(int n) { return n == 1 ? "the goto to it" : "the " + std::to_string(n) + " gotos to it"; }
  inline Change removeLine(const Ctx& c, const Bytes& code, size_t off) {
    Change ch;
    if (!lineOk(c, code, off, ch)) return ch;
    const int op = rd(code.data() + off);
    const size_t sz = static_cast<size_t>(sizeOf(c, op));
    if (off == 0 && unitBegin(op)) { ch.err = kFirstLine; return ch; }
    if (unitEnd(op)) { ch.err = kLastLine; return ch; }
    size_t from = off, to = off + sz;
    const int blk = blockOf(c, op);
    if (blk != 0) {
      const size_t b = blk > 0 ? off : ownerOf(c, code, off);
      const size_t e = b == npos ? npos : blockEnd(c, code, b);
      if (e != npos) {
        from = b;
        const int bop = rd(code.data() + b);
        to = bop == 9 ? e : elseEnd(c, code, e);
        ch.done = "Removed the " + nameOf(c, bop) + " block (" + std::to_string(linesIn(c, code, from, to)) + " lines" + (to != e ? ", with its else part" : "") + ").";
      } else ch.done = "Removed " + nameOf(c, op) + ".";
      ch.sp.push_back({from, to - from, {}});
      return ch;
    }
    ch.sp.push_back({off, sz, {}});
    if (op == 11) {
      const std::string label = cstr(code.data() + off + 4, 32);
      int n = 0;
      cutGotos(c, code, label, off, ch, n);
      ch.done = "Removed the label " + label + (n ? " and " + gotoWord(n) : std::string()) + ".";
    } else ch.done = "Removed " + nameOf(c, op) + ".";
    return ch;
  }
  inline std::vector<char> carryFlags(const Ctx& c, const Bytes& code, const std::vector<char>& flags, const std::vector<Splice>& sp, const Bytes& out) {
    const std::vector<size_t> lo = lineStarts(c, code), ln = lineStarts(c, out);
    std::vector<char> res(ln.size(), 1);
    for (size_t i = 0; i < lo.size(); i++) {
      const long long m = movedTo(sp, lo[i]);
      if (m < 0) continue;
      const auto it = std::lower_bound(ln.begin(), ln.end(), static_cast<size_t>(m));
      if (it != ln.end() && *it == static_cast<size_t>(m)) res[static_cast<size_t>(it - ln.begin())] = i < flags.size() ? flags[i] : 0;
    }
    return res;
  }
  inline std::string flagsText(const std::vector<char>& f) {
    std::string t;
    for (char x : f) t += x ? '1' : '0';
    return t;
  }
  inline std::vector<char> flagsOf(const std::string& t) {
    std::vector<char> f;
    for (char x : t) {
      if (x != '0' && x != '1') return {};
      f.push_back(x == '1' ? 1 : 0);
    }
    return f;
  }
  inline std::vector<size_t> removedLines(const Ctx& c, const Bytes& code, size_t off) {
    std::vector<size_t> out;
    const Change ch = removeLine(c, code, off);
    if (!ch.err.empty()) return out;
    for (size_t p : lineStarts(c, code))
      for (const Splice& s : ch.sp)
        if (s.cut && p >= s.at && p < s.at + s.cut) { out.push_back(p); break; }
    return out;
  }
  inline Change copyLine(const Ctx& c, const Bytes& code, size_t off) {
    Change ch;
    if (!lineOk(c, code, off, ch)) return ch;
    const int op = rd(code.data() + off);
    const size_t sz = static_cast<size_t>(sizeOf(c, op));
    if (unitEnd(op)) { ch.err = kAfterLast; return ch; }
    size_t s = off, e = off + sz;
    Bytes closers;
    const int blk = blockOf(c, op);
    size_t b = blk > 0 ? off : blk < 0 ? ownerOf(c, code, off) : npos;
    if (blk != 0 && !unitBegin(op) && b != npos) {
      {
        b = constructStart(c, code, b);
        const size_t be = blockEnd(c, code, b);
        if (be != npos) { s = b; e = elseEnd(c, code, be); }
        else {
          const std::vector<size_t> ls = lineStarts(c, code);
          if (!ls.empty() && ls.back() > b) {
            s = b;
            e = ls.back();
            std::vector<int> open;
            for (size_t p : ls) {
              if (p < s || p >= e) continue;
              const int o = rd(code.data() + p);
              const int k = blockOf(c, o);
              if (k > 0 && !unitBegin(o)) open.push_back(o);
              else if (k < 0 && !open.empty()) open.pop_back();
            }
            for (auto it = open.rbegin(); it != open.rend(); ++it)
              if (const int eo = endFor(c, *it); eo >= 0 && sizeOf(c, eo) > 0) {
                Bytes eb(static_cast<size_t>(sizeOf(c, eo)), 0);
                wr(eb.data(), eo);
                closers.insert(closers.end(), eb.begin(), eb.end());
              }
          }
        }
      }
    }
    Bytes add(code.begin() + static_cast<ptrdiff_t>(s), code.begin() + static_cast<ptrdiff_t>(e));
    add.insert(add.end(), closers.begin(), closers.end());
    std::set<std::string> taken = labelNames(c, code);
    std::map<std::string, std::string> ren;
    for (size_t p : lineStarts(c, add))
      if (rd(add.data() + p) == 11 && add.size() >= p + 36) {
        const std::string was = cstr(add.data() + p + 4, 32), now = uniqueLabel(taken);
        if (!ren.count(was)) ren[was] = now;
        putName(add.data() + p + 4, 32, ren[was]);
      }
    for (size_t p : lineStarts(c, add)) {
      const int o = rd(add.data() + p);
      if (!gotoOp(o)) continue;
      const auto it = ren.find(cstr(add.data() + p + labelAt(o), 32));
      if (it != ren.end()) putName(add.data() + p + static_cast<size_t>(labelAt(o)), 32, it->second);
    }
    ch.sp.push_back({e, 0, add});
    if (e - s > sz || s != off)
      ch.done = "Duplicated the " + nameOf(c, rd(code.data() + s)) + " block (" + std::to_string(linesIn(c, code, s, e)) + " lines)" + (ren.empty() ? std::string() : ", its labels renamed") +
                (closers.empty() ? std::string() : ". It has no end line in the game's script, so the copy runs to the last line and gets end lines of its own") + ".";
    else if (op == 11) ch.done = "Duplicated the label " + cstr(code.data() + off + 4, 32) + " as " + ren.begin()->second + ".";
    else ch.done = "Duplicated " + nameOf(c, op) + ".";
    return ch;
  }
  constexpr int kDiffMaxD = 1000;
  constexpr long long kDiffBudget = 2000000;
  inline std::vector<char> addedLines(const Ctx& c, const Bytes& game, const Bytes& now, int max_d = kDiffMaxD) {
    const std::vector<size_t> la = lineStarts(c, game), lb = lineStarts(c, now);
    std::vector<char> out(lb.size(), 0);
    if (game == now) return out;
    std::map<std::string, int> ids;
    auto key = [&](const Bytes& b, size_t p) {
      const size_t sz = static_cast<size_t>(sizeOf(c, rd(b.data() + p)));
      return ids.emplace(std::string(reinterpret_cast<const char*>(b.data() + p), sz), static_cast<int>(ids.size())).first->second;
    };
    std::vector<int> A(la.size()), B(lb.size());
    for (size_t i = 0; i < la.size(); i++) A[i] = key(game, la[i]);
    for (size_t j = 0; j < lb.size(); j++) B[j] = key(now, lb[j]);
    const int n0 = static_cast<int>(A.size()), m0 = static_cast<int>(B.size());
    int pre = 0;
    while (pre < n0 && pre < m0 && A[static_cast<size_t>(pre)] == B[static_cast<size_t>(pre)]) pre++;
    int suf = 0;
    while (suf < n0 - pre && suf < m0 - pre && A[static_cast<size_t>(n0 - 1 - suf)] == B[static_cast<size_t>(m0 - 1 - suf)]) suf++;
    const int n = n0 - pre - suf, m = m0 - pre - suf;
    auto a = [&](int x) { return A[static_cast<size_t>(pre + x)]; };
    auto b = [&](int y) { return B[static_cast<size_t>(pre + y)]; };
    std::vector<char> matchA(static_cast<size_t>(n), 0), matchB(static_cast<size_t>(m), 0);
    if (n > 0 && m > 0) {
      max_d = static_cast<int>((std::min)(static_cast<long long>(max_d), (std::max)(64LL, kDiffBudget / (static_cast<long long>(n) + m + 1))));
      const int off = max_d + 1;
      std::vector<int> v(static_cast<size_t>(2 * max_d + 3), 0);
      std::vector<std::vector<int>> trace;
      int found = -1;
      for (int d = 0; d <= max_d && found < 0; d++) {
        trace.emplace_back(v.begin() + (off - d - 1), v.begin() + (off + d + 2));
        for (int k = -d; k <= d; k += 2) {
          int x = (k == -d || (k != d && v[static_cast<size_t>(off + k - 1)] < v[static_cast<size_t>(off + k + 1)])) ? v[static_cast<size_t>(off + k + 1)] : v[static_cast<size_t>(off + k - 1)] + 1;
          int y = x - k;
          while (x < n && y < m && a(x) == b(y)) { x++; y++; }
          v[static_cast<size_t>(off + k)] = x;
          if (x >= n && y >= m) { found = d; break; }
        }
      }
      if (found < 0) return {};
      int x = n, y = m;
      for (int d = found; d > 0; d--) {
        const std::vector<int>& tv = trace[static_cast<size_t>(d)];
        auto at = [&](int k) { return tv[static_cast<size_t>(k + d + 1)]; };
        const int k = x - y;
        const int pk = (k == -d || (k != d && at(k - 1) < at(k + 1))) ? k + 1 : k - 1;
        const int px = at(pk), py = px - pk;
        while (x > px && y > py) { matchA[static_cast<size_t>(x - 1)] = 1; matchB[static_cast<size_t>(y - 1)] = 1; x--; y--; }
        x = px;
        y = py;
      }
      while (x > 0 && y > 0) { matchA[static_cast<size_t>(x - 1)] = 1; matchB[static_cast<size_t>(y - 1)] = 1; x--; y--; }
    }
    for (int j = 0; j < m; j++) if (!matchB[static_cast<size_t>(j)]) out[static_cast<size_t>(pre + j)] = 1;
    int i = 0, j = 0;
    while (i < n || j < m) {
      std::vector<int> del, ins;
      while (i < n && !matchA[static_cast<size_t>(i)]) del.push_back(i++);
      while (j < m && !matchB[static_cast<size_t>(j)]) ins.push_back(j++);
      std::vector<char> used(del.size(), 0);
      for (int bj : ins) {
        const int op = rd(now.data() + lb[static_cast<size_t>(pre + bj)]);
        for (size_t q = 0; q < del.size(); q++)
          if (!used[q] && rd(game.data() + la[static_cast<size_t>(pre + del[q])]) == op) { used[q] = 1; out[static_cast<size_t>(pre + bj)] = 0; break; }
      }
      if (i < n && j < m) { i++; j++; }
      else break;
    }
    return out;
  }
  inline bool lineSpan(const Ctx& c, const Bytes& code, size_t off, size_t& s, size_t& e) {
    const std::vector<size_t> ls = lineStarts(c, code);
    if (!std::binary_search(ls.begin(), ls.end(), off)) return false;
    const int op = rd(code.data() + off);
    s = off;
    e = off + static_cast<size_t>(sizeOf(c, op));
    if (off == 0 && unitBegin(op)) { e = code.size(); return true; }
    const int blk = blockOf(c, op);
    if (blk == 0 || unitBegin(op) || unitEnd(op)) return true;
    size_t b = blk > 0 ? off : ownerOf(c, code, off);
    if (b == npos) return true;
    b = constructStart(c, code, b);
    const size_t be = blockEnd(c, code, b);
    if (be == npos) return true;
    s = b;
    e = elseEnd(c, code, be);
    return true;
  }
  inline std::string linesText(const Ctx& c, const Bytes& code, size_t off) {
    size_t s = 0, e = 0;
    if (!lineSpan(c, code, off, s, e)) return {};
    return textOf(c, Bytes(code.begin() + static_cast<ptrdiff_t>(s), code.begin() + static_cast<ptrdiff_t>(e)));
  }
  inline Change pasteLines(const Ctx& c, const Bytes& code, size_t off, const std::string& text) {
    Change ch;
    if (!lineOk(c, code, off, ch)) return ch;
    const int at_op = rd(code.data() + off);
    if (unitEnd(at_op)) { ch.err = kAfterLast; return ch; }
    Bytes add;
    std::vector<int> lines;
    std::string err;
    if (!assembleRange(c, text, 1, INT_MAX, add, err, &lines)) {
      ch.err = (err.rfind("line ", 0) == 0 ? "Pasted l" + err.substr(1) : err) + " Nothing was pasted.";
      return ch;
    }
    if (add.empty()) { ch.err = "The clipboard holds no script lines (one instruction per line, as in an exported .bbs file)."; return ch; }
    const std::vector<size_t> ps = lineStarts(c, add);
    size_t at = off + static_cast<size_t>(sizeOf(c, at_op));
    std::string moved;
    if (at_op == 5 && at + 4 <= code.size() && rd(code.data() + at) == 9) {
      const size_t e = elseEnd(c, code, at);
      if (e != at) { at = e; moved = " (after the else part, since an else has to follow its endIf right away)"; }
    }
    ch.sp.push_back({at, 0, add});
    ch.done = "Pasted " + std::to_string(ps.size()) + (ps.size() == 1 ? " line" : " lines") + " after " + nameOf(c, at_op) + moved + ".";
    return ch;
  }
  inline Change insertLine(const Ctx& c, const Bytes& code, size_t off, int op, const Known& known) {
    Change ch;
    if (!lineOk(c, code, off, ch)) return ch;
    const int at_op = rd(code.data() + off);
    if (unitEnd(at_op)) { ch.err = kAfterLast; return ch; }
    size_t at = off + static_cast<size_t>(sizeOf(c, at_op));
    std::string moved;
    if (at_op == 5 && at + 4 <= code.size() && rd(code.data() + at) == 9) {
      const size_t e = elseEnd(c, code, at);
      if (e != at) { at = e; moved = " (after the else part, since an else has to follow its endIf right away)"; }
    }
    std::set<std::string> taken = labelNames(c, code);
    Bytes line, after;
    std::string note;
    if (!newLine(c, code, at, at, op, known, taken, line, after, note, ch.err)) return ch;
    const bool with_end = blockOf(c, op) > 0 && !after.empty();
    line.insert(line.end(), after.begin(), after.end());
    ch.sp.push_back({at, 0, line});
    ch.done = "Added " + nameOf(c, op) + (with_end ? " and " + nameOf(c, endFor(c, op)) : std::string()) + note + " after " + nameOf(c, at_op) + moved + ".";
    return ch;
  }
  inline Change replaceLine(const Ctx& c, const Bytes& code, size_t off, int op, const Known& known) {
    Change ch;
    if (!lineOk(c, code, off, ch)) return ch;
    const int was = rd(code.data() + off);
    const size_t wsz = static_cast<size_t>(sizeOf(c, was));
    if (off == 0 && unitBegin(was)) { ch.err = kFirstLine; return ch; }
    if (unitEnd(was)) { ch.err = kLastLine; return ch; }
    std::set<std::string> taken = labelNames(c, code);
    Bytes line, after;
    std::string note;
    if (!newLine(c, code, off, off + wsz, op, known, taken, line, after, note, ch.err)) return ch;
    const bool new_block = blockOf(c, op) > 0;
    const int wblk = blockOf(c, was);
    std::string extra;
    if (wblk > 0) {
      const size_t e = blockEnd(c, code, off);
      size_t end_at = npos;
      if (e != npos) for (size_t p : lineStarts(c, code)) if (p > off && p < e) end_at = p;
      if (new_block) {
        ch.sp.push_back({off, wsz, line});
        if (end_at != npos && endFor(c, op) >= 0 && sizeOf(c, endFor(c, op)) > 0) {
          Bytes eb(static_cast<size_t>(sizeOf(c, endFor(c, op))), 0);
          wr(eb.data(), endFor(c, op));
          ch.sp.push_back({end_at, e - end_at, eb});
        } else if (end_at == npos) {
          ch.sp.back().add.insert(ch.sp.back().add.end(), after.begin(), after.end());
        }
      } else {
        line.insert(line.end(), after.begin(), after.end());
        ch.sp.push_back({off, wsz, line});
        if (end_at != npos) {
          const size_t ee = was == 9 ? e : elseEnd(c, code, e);
          ch.sp.push_back({end_at, ee - end_at, {}});
          extra = ee != e ? ", its end line and its else part removed (the lines inside stay)" : ", its end line removed (the lines inside stay)";
        }
      }
    } else {
      line.insert(line.end(), after.begin(), after.end());
      ch.sp.push_back({off, wsz, line});
      if (was == 11) {
        int n = 0;
        cutGotos(c, code, cstr(code.data() + off + 4, 32), off, ch, n);
        if (n) extra = ", " + gotoWord(n) + " removed with the label";
      }
    }
    ch.done = "Replaced " + nameOf(c, was) + " with " + nameOf(c, op) + note + extra + ".";
    return ch;
  }
  inline Change renameLabel(const Ctx& c, const Bytes& code, size_t off, const std::string& name) {
    Change ch;
    if (!lineOk(c, code, off, ch)) return ch;
    if (rd(code.data() + off) != 11) { ch.err = "That line isn't a label."; return ch; }
    const std::string was = cstr(code.data() + off + 4, 32);
    if (was == name) return ch;
    Bytes nb(32, 0);
    putName(nb.data(), 32, name);
    ch.sp.push_back({off + 4, 32, nb});
    int n = 0, same = 0;
    for (const auto& l : labelsIn(c, code)) if (l.second == was && l.first != off) same++;
    if (!same)
      for (const auto& g : gotosIn(c, code))
        if (g.second == was) { ch.sp.push_back({g.first + static_cast<size_t>(labelAt(rd(code.data() + g.first))), 32, nb}); n++; }
    ch.done = "Renamed the label " + was + " to " + name + (n ? (n == 1 ? " and the goto to it" : " and the " + std::to_string(n) + " gotos to it") : std::string()) + ".";
    return ch;
  }
  inline int argAt(const Ctx& c, int op, int field) {
    const Instr* d = def(c, op);
    const int sz = sizeOf(c, op);
    if (!d || sz < 0) return -1;
    int fat = 4;
    for (const ArgDef& a : d->args) {
      const int need = argBytes(a.kind);
      if (fat + need > sz) break;
      if ((a.kind == Arg::Number || a.kind == Arg::Enum) && field == fat) return 0;
      if (a.kind == Arg::Value && field == fat + 4) return 1;
      if (a.kind == Arg::Value && field == fat) return 2;
      if ((a.kind == Arg::String16 || a.kind == Arg::String32) && field == fat) return 3;
      fat += need;
    }
    return -1;
  }
}

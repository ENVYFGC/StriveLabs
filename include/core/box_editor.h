// Script & boxes: a window showing each fighter's state, sprite, boxes and the state's BBScript, read live from the
// game's loaded files (training only). Instruction names and argument layouts come from BBScriptDX's database
// (StriveLabs_scripts/db/GGSTRonRemake.ron, with idNameWithDescription.json for the descriptions). A move can be
// exported as .bbs and the sprite's boxes as BBScriptDX's COL JSON. Every kind of box, and the move's script,
// can be changed live (written into the loaded files: a box edit changes every frame showing that sprite).
// Edits last for the game session, also with the window closed: kept over new training fights if asked; Reset and
// Reset all put the game's values back.
#pragma once
#include <filesystem>
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
    std::string replaced_from; // replaced by another instruction: the instruction it was, "" if not
    int replaced_op = -1;      // (and its number)
    // its numbers, for editing: kind 0 a number, 1 a value (only a literal is editable, not Mem(...)), 2 a
    // named value; field: its place in the line, in bytes; label / tip from labels.json, else the database's
    struct ArgView { int kind = 0, field = 0, value = 0; bool editable = false; std::string label, tip, value_name; };
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
    std::string selected;            // the move opened in the small panel ("" none)
    std::vector<InstrView> sel_script;
    int sel_current = -1;            // its running line while it's the move playing, else -1
    FramesView sel_frames;           // its frames
    // the game's battle versions of the opened move: each group of versions with the same script
    struct VersionView { std::string label, folder; bool now = false; };  // "3.06-3.07", "306"; now: today's script
    std::vector<VersionView> sel_versions;
    std::string sel_loaded;          // the version loaded into it ("" the game's own)
    std::string note;                // why something couldn't be read
  };
  struct View {
    bool open = false;
    bool db = false;       // the instruction database was read
    std::string db_note;   // where it was looked for, or how many instructions it has
    SideView side[2];
    std::vector<EditedView> edits;  // every sprite with changed boxes
    bool keep = true;               // edits are put back on new training fights
    bool can_undo = false;
    std::string status;
  };

  void init(const std::filesystem::path& ue4ss_dir);
  void toggle();          // Ctrl+Alt+B, or the window's close box
  bool isOpen();
  void tick();            // game thread, in a training fight
  void outsideTraining(); // game thread, each battle update outside training: the game's own boxes go back
  View view();
  void showObject(int side, const void* id);  // one of the side's objects instead of the fighter (nullptr: the fighter)
  void selectState(int side, const std::string& name);  // the move shown in the small panel ("" closes it)
  void exportState(int side);  // the state as .bbs (the opened move's, else the running one), into StriveLabs_scripts/<CHR>/
  void exportBoxes(int side);  // the sprite's boxes as COL JSON
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
  void resetAll();
  void undo();
  void setKeep(bool keep);
  void setHighlight(int side, int index);  // the box hovered or picked in the table, -1 none
  bool highlighted(const void* entity, int& index);  // for the hitbox drawing: is a box of this entity highlighted
  // Where a fighter's box units land on screen, as parts of the viewport: box point (x, y) is at
  // (ox + ax*x + bx*y, oy + ay*x + by*y). Handed over by the hitbox drawing, so only while Hitboxes is on.
  struct ScreenMap { float ox = 0, oy = 0, ax = 0, ay = 0, bx = 0, by = 0; };
  void setScreenMap(const void* entity, const ScreenMap& m);
  bool screenMap(int side, ScreenMap& m);  // false unless it was drawn in the last 200 ms
}

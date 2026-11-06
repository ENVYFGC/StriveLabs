// Live color editor for the fighters in training. Regions come from every official color of the character,
// read from the running game. Edits are applied to the game's own material as new textures.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <cstdlib>

namespace recolor_test {
  // Turns "Color05" into "Color 5" for display only. Files, keys and calls keep "Color05".
  inline std::string slotLabel(const std::string& s) {
    if (s.size() > 5 && s.compare(0, 5, "Color") == 0 && s[5] >= '0' && s[5] <= '9') return "Color " + std::to_string(std::atoi(s.c_str() + 5));
    return s;
  }
  inline constexpr const char* kCamHelp = "WASD move, Q/E up/down, arrows turn, Shift faster, R shows the whole fighter. Left drag orbits, right drag pans, wheel zooms.";
  struct RegionView {
    uint32_t lit = 0, shadow = 0;            // current colors, 0xRRGGBB
    uint32_t orig_lit = 0, orig_shadow = 0;  // the worn color's own colors
    float share = 0;                         // share of the texture, 0..1
    bool edited = false;
    bool unseen = false;                     // the mapping never saw it, so the camera can't find it
    bool detail = false;                     // a small region (a thin line, a small mark), listed in its own closed group
    std::string name;                        // the user's own name for it, "" if none
    int lead = -1;                           // the row it's listed under, either itself or a region that looks exactly the same in this color
    int twins = 1;                           // for a leading row, how many regions it stands for
    bool undrawn = false;                    // the model never draws it (from the mesh's UVs, when known)
    bool listed = true;                      // has its own row (not a twin, not off the model)
    int parent = -1;                         // for a small region, the row of the big region it sits on, -1 if none
    std::string only_on;                     // drawn only by a model the game keeps hidden, and this is that part's name ("DI")
  };
  // A part of the outfit: regions that share their color in nearly every official color.
  struct GroupView {
    std::vector<int> regions;
    float share = 0;
    bool line = false;     // near black in every color, so lines and details are left alone
    bool skin = false;     // skin toned in most colors
    bool neutral = false;  // low saturation in most colors (metal, white cloth, grey)
  };
  // A color the part's materials draw with besides their textures (Sol's Dragon Install glow).
  struct MaterialColor { std::string label; float rgba[4] = {}; bool edited = false; };
  // An effect color of the worn color slot (Sol's fire). Only its hue is set, the game's brightness is kept.
  struct EffectUse { std::string effect, says; float rgb[3] = {}; bool edited = false; };  // one effect that takes it. says is what it is, "" if not known
  struct EffectColor { std::string label, uses, used_by; float rgb[3] = {}; bool edited = false; std::vector<EffectUse> per; };  // used_by lists the effects that take it by name
  // A color of an effect that played during a capture, shared by every color slot. Hue only.
  struct CapturedColor { std::string effect, label; float rgb[3] = {}; bool edited = false; };
  struct View {
    bool open = false;
    uint32_t cap_gen = 0;     // version of the captured list this view shows, sent back with a row's edit
    std::string fx_slot;      // the slot the listed effect colors belong to ("SOL/Color08")
    std::string title;        // "SOL  Color01"
    std::string status;
    float progress = -1;      // 0..1 while the character's colors are read
    float mapping = -1;       // 0..1 while the parts of the model are mapped
    bool setup = false;       // the first-time setup is running (its "please wait" is up and the editor can't be used)
    std::string setup_part;   // the part it's on ("Weapon")
    bool can_switch = false;  // another fighter is on screen
    std::vector<std::string> parts;  // this fighter's color materials: "MAIN" (the body), "WEP" (the weapon), others by name
    int picked = -1, pick_serial = 0;  // the region last picked on the fighter, with a new serial each time
    std::vector<int> pick_choices;     // every region right around the click, nearest first (picked is the first)
    bool picking = false;              // a pick is being looked for
    bool decal_lines = false;          // the shown part has decal lines (the texture view can show them)
    int tex_w = 0, tex_h = 0;          // the shown part's texture size (texels)
    bool painted = false;              // the shown part has paint on it
    bool exporting = false;            // Export textures is running, so what it reads can't change (Reload colors, Remap, imports, loads)
    std::vector<int> part_ids;       // each part's id, for showPart
    std::vector<char> part_hidden;   // a model the game keeps hidden (Sol's Dragon Install), shown in its place so it can be edited
    std::string drawn_from;          // for a hidden part, another part whose texture colors some of this model ("Body"), "" if none
    int part = -1;                   // the part shown
    bool shadow_follows = true;
    bool blackout = true;       // selecting turns the fighter black and the selection blinks white
    bool free_cam = false;      // our camera orbits the fighter
    bool follow = true;         // selecting a part flies the camera to it
    std::string cam_info;
    std::vector<int> selected;  // regions blinking on the fighter
    std::vector<RegionView> regions;
    std::vector<GroupView> groups;  // biggest first
    std::vector<MaterialColor> material_colors;
    std::vector<EffectColor> effect_colors;
    std::vector<CapturedColor> captured;  // grouped by effect, in the order they played
    std::vector<std::string> on_screen;       // "What's on screen": the effects playing at the time (empty means the whole list)
    std::vector<std::string> hidden_effects;  // effects hidden to find one (Hide), "*" means all of them
    std::vector<CapturedColor> captured_mats;  // the effects' material colors (label "MI_x  noise_color"), shared by every slot
    std::vector<std::pair<std::string, std::string>> captured_named;  // (effect, palette color it takes by name), set in Effect colors
    std::string slot;                         // the color slot shown ("Color08")
    bool unsaved = false;                     // changes not saved yet
    bool confirm_close = false;               // close was asked with unsaved changes, offering Save / Discard / Cancel
    std::string offer_recovery;               // a character with unsaved changes kept from last time, offering Restore / Discard
    bool can_undo = false, can_redo = false;
    bool group_same = true;
    std::vector<std::string> slots;           // this character's color slots (after listRecolors)
    std::vector<std::string> recolor_files;   // exported recolors of this character (after listRecolors)
    struct Palette { std::string file, name, author, note, from, when; };
    std::vector<Palette> palettes;            // this character's saved palettes (after listRecolors)
    std::string palette_author;               // the author name last used
    bool capturing = false;
  };
  // A region's new colors. For the shadow, 0 means as the switch says (it follows the lit color or stays),
  // 1 means exactly this shadow and 2 means it always follows the lit color.
  struct Pick { int region = -1; uint32_t lit = 0; int shadow_mode = 0; uint32_t shadow = 0; };  // shadow_mode 3 goes back to the game's color (no edit)
  View view();  // safe from any thread, returns a copy
  bool isOpen();
  bool holdsFight();    // game thread: the editor is scanning the fighter or running its first-time setup, so the fight is held
  bool setupRunning();  // any thread: the first-time setup's "please wait" is up, so the keys and the fight are held
  bool setupShown();    // the same for drawing it, allowing a slow frame
  // Called from the UI (any thread), done on the game thread.
  void toggle();
  void setLit(int region, uint32_t rgb);
  void setShadow(int region, uint32_t rgb);
  void select(int region);  // -1 = none
  void selectMany(const std::vector<int>& regions);
  // Several regions in one update. Mode 1: a row's twins take its pick too, so a gradient's steps move alike.
  // Mode 2: the twins take exactly the same color (chroma keys). Mode 0: each region alone (auto colors).
  void setPairs(const std::vector<Pick>& picks, int mode = 1);
  void reset(int region);   // -1 = everything on the shown part
  void resetColor();        // the whole color slot: every part, paint and effect color
  void nextTarget();        // the other fighter
  void showPart(int id);    // one of View::part_ids
  void remap();             // map where the regions are again
  std::string partName(const std::string& kind);  // "MAIN" -> "Body", "DI_FLARE_UNIQUEC" -> "DI wing" (the UI's and the export's file names)
  void exportTextures();  // exports every part of the shown slot into export/<CHR>/<ColorNN>/<date_time>/ (worker, progress in the status)
  std::wstring textureDir();  // where Import's window opens (the slot's newest export, else the export folder)
  std::wstring exportsDir();  // the export folder
  void importTextureFiles(const std::vector<std::wstring>& files);  // edited textures in, each on the part its name says (changed texels become paint)
  void groupSame(bool on);  // one row for regions that look exactly the same in the shown color (an edit goes to all of them)
  void logDrawn();  // logs every mesh drawn now (effects too), with its materials and their parameters
  void renameRegion(int region, const std::string& name);  // "" clears it. Kept per character and part
  void setMaterialColor(int i, const float rgba[4]);
  void resetMaterialColor(int i);  // -1: all
  void setEffectColor(int i, const float rgb[3], const std::string& slot);  // slot is View::fx_slot, the list's slot
  void resetEffectColor(int i, const std::string& slot);
  void setEffectColorFor(int i, const std::string& effect, const float rgb[3], const std::string& slot);  // (one effect's own color)
  void resetEffectColorFor(int i, const std::string& effect, const std::string& slot);
  void captureEffects();  // lists the effects that play in the next 5 s
  void listAllEffects();  // every loaded effect and material of the character into the captured list (the "All colors" view)
  void hideEffect(const std::string& effect, bool hide);  // "*" means every effect
  void onScreen(bool on);  // narrows the list to the effects playing now (off shows the whole list)
  // Recolors: everything saved for the shown slot (regions, material and effect colors, region names).
  void listRecolors();                          // fills View::slots and View::recolor_files
  void saveRecolor();                           // writes the changes to disk (nothing is saved until this)
  void discardAndClose();                       // goes back to what's saved and closes the editor
  void cancelClose();
  void restoreRecovery(bool restore);           // brings the kept unsaved changes back (still unsaved), or throws them away
  void pickAt(float x, float y);  // selects the region under this point of the screen (0..1) when one is found
  // The flat texture view shows the shown part's texture with edits on, the selection lit and the rest dimmed.
  // It's made on the game thread while the view is open, and the UI takes it when its serial moves on. box is
  // the selection's bounds (0..1), or all 0.
  void textureViewOpen(bool open);
  void textureViewMap(int map);  // 0 lit (BaseMap), 1 shadow (SssMap), 2 the decal's lines
  void textureViewPlain(bool plain);  // true: no dimming or box, the picture as it is (a paint tool is out)
  // Painting on the shown part, with positions 0..1 across its texture. map: 0 lit, 1 shadow, 2 both (the
  // shadow takes the same stroke). blend: 0 normal, 1 color, 2 multiply, 3 screen.
  struct PaintBrush {
    int map = 0, blend = 0;
    float rgb[3] = {1, 1, 1};
    float size = 16, hardness = 0.8f, opacity = 1, flow = 1;  // size in texels
    bool eraser = false, keep_region = false;
  };
  void paintBegin(float u, float v, const PaintBrush& b);
  void paintTo(float u, float v);
  void paintEnd();
  void paintFill(float u, float v, const PaintBrush& b, int tolerance, bool contiguous);
  void paintClear(int map);  // 0 lit, 1 shadow, 2 both
  // A shape from (u0, v0) to (u1, v1), as one undo step. kind: 0 line, 1 rectangle, 2 ellipse (in that box).
  // An outline is a brush stroke along the edge. Filled (not for a line) sets the texels inside at the brush's
  // opacity.
  void paintShape(float u0, float v0, float u1, float v1, const PaintBrush& b, int kind, bool filled);
  bool textureView(int& serial, std::vector<uint8_t>& rgba, int& w, int& h, float box[4]);  // returns true with rgba empty when the view should be cleared
  int textureRegionAt(float u, float v);  // the region at this point of the view's texture (0..1), -1 if none (any thread)
  void undo();
  void redo();
  void exportRecolor();                         // one file, in StriveLabs_colors/recolors
  // Palettes: a whole recolor of the shown slot kept by name (with author and note), per character.
  void savePalette(const std::string& file, const std::string& name, const std::string& author, const std::string& note);  // file "" makes a new one
  void loadPalette(const std::string& file);    // onto the shown color slot
  void editPalette(const std::string& file, const std::string& name, const std::string& author, const std::string& note);
  void deletePalette(const std::string& file);  // moves it to the palettes' .trash folder
  void importRecolor(const std::string& file);  // onto the shown slot
  void copyToSlot(const std::string& slot);     // the shown slot's recolor onto another slot of the character
  void readColorsAgain();                       // reads the character's colors from the game again (after a game patch)
  void setCapturedColor(int i, const float rgb[3], uint32_t gen);  // gen is View::cap_gen, the list the row is in
  void resetCapturedColor(int i, uint32_t gen);
  void setCapturedMatColor(int i, const float rgb[3], uint32_t gen);  // edits captured_mats[i]
  void resetCapturedMatColor(int i, uint32_t gen);
  void setShadowFollows(bool on);
  void setBlackout(bool on);
  // Free camera: orbit the fighter. Input comes from the UI (pixels dragged, wheel steps).
  void setFreeCamera(bool on);
  void cameraInput(float dx, float dy, float wheel);
  void cameraPan(float dx, float dy);  // right drag moves what the camera looks at
  bool freeCameraKeys();               // the free camera owns WASD, Q/E, R and the arrows
  void overlayFrame(bool typing, int screen_w, int screen_h);  // called each frame with typing (the camera's keys are off) and the frame's size
  void setFollow(bool on);
  bool tagAt(float& x, float& y);      // where the selection is on screen (0..1), while the free camera shows it
  // Scans run on the render thread, before our overlay draws. For a few frames the fighter's regions are lit
  // in a binary code (frame 0 is all black, frame k lights the regions whose number has bit k). Each frame is
  // grabbed once it has been on screen. After the last, the pixels that decode to a region number come back
  // as samples (a few dozen per region), in screen pixels.
  struct ScanSample { uint16_t id, x, y; };
  bool scanWanted(int& id, int& frame, int& frames, int& groups);
  bool scanWhole();  // the scan is of a part that's one region by itself (Sol's DI wing flare), read even as it flickers
  void scanGrabbed(int id, int frame);
  void scanResult(int id, int screen_w, int screen_h, std::vector<ScanSample>& samples);
  void scanFailed(int id, const std::string& why);  // the screen couldn't be read, and why is told to the user
  bool scanPoint(float& x, float& y);  // (render thread) a pick's point (0..1). Every pixel around it is sent back too
  void afterBattleUpdate();  // game thread, right after the game's own battle update (the camera is written here)
  void battleGone();         // game thread, once no fight is running. Effect colors go back to the game's
  // Game thread, every system update outside training. Puts saved recolors on the models shown: the menus'
  // models, and in any fight outside training the fighters' textures, material colors and the slot's effect
  // colors.
  void menuUpdate(bool in_fight);
  // Recolor sharing. Game thread unless said otherwise.
  void shareNoteSides(int local_side);  // at round 1's start, which fighter is ours (P1 starts on the left of the screen)
  // Our fighter's recolor for the opponent: 0 = ready (in out), 1 = not yet (textures still being made), 2 = nothing to send.
  int sharePayload(std::vector<uint8_t>& out, std::string& why);
  bool shareTake(const std::vector<uint8_t>& payload, std::string& why);  // the opponent's recolor, checked and then put on their fighter
  std::string shareInfo();           // any thread. The received recolor ("SOL Color03"), "" if none
  std::string shareSaveStatus();     // any thread. The last save's outcome
  bool slotHasRecolor(const std::string& chr, const std::string& color);  // any thread
  void shareSave(const std::string& slot);  // any thread. Saves the received recolor into one of our slots (done on the game thread)
  void menuRelease();        // when a fight starts, the menus' recolors are let go

  void init(const std::filesystem::path& ue4ss_dir);
  void update(bool round_started);  // game thread, every battle update in training
}

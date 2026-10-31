// Dear ImGui drawn inside the game, through a hook on the DirectX 11 swap chain.
// If the hook cannot be set up (DirectX 12, a failed device, ...), install()
// returns false and the mod keeps using its HUD-drawn menu.
#pragma once
#include <string>
#include <vector>

namespace imgui_overlay {
  bool install();   // call once at startup; true if the Present hook is in place
  bool running();   // true once ImGui has drawn its first frame
  // Game thread: the tension overlay's lines for this frame ("P1 Burst: 1.07"...).
  void setTensionLines(const std::vector<std::wstring>& lines);
  // Game thread: the game's training damage panel for one side (0 = P1). A row with no
  // label is the gap between its two parts. Rects are where the game draws that text, in
  // screen pixels (x, y, w, h), when placed.
  struct DamageRow {
    std::string label, value;
    float label_rect[4] = {}, value_rect[4] = {};
    bool placed = false;
  };
  void setDamageRows(int side, const std::vector<DamageRow>& rows);
  // Game thread, every battle frame: the battle version's name, for its small window.
  void setBattleVersion(const std::string& name);
  // Game thread: the game's own pause menu is open (the overlay steps out of its way).
  void setGameMenuOpen(bool open);
  // The online version window (outside fights): open or closed, and arrow clicks in it
  // for the game thread to apply (-1, +1, 0 = none).
  void setOnlinePicker(bool open);
  bool onlinePickerOpen();
  int takeOnlinePickStep();

  // Game thread: the frame bar's cells and stats lines for this frame.
  struct FrameBarView {
    struct Cell {
      unsigned color = 0; unsigned char mods = 0; int trunc = 0; bool on = false;
      unsigned char dir = 0;                         // input bar: numpad, 0 = no data
      unsigned short held = 0, pressed = 0;          // input bar: RECFLG button bits
      unsigned char keep = 0;                        // one block: last frames of the stretch kept as cells
      int gap = 0;                                   // gaps: free frames between two stuns, on the last of them
      unsigned short frames = 0;                     // frames the cell holds; more than 1 = folded, drawn wider
      bool free = false;                             // the player could act (neutral or dash)
      bool stun = false, recovering = false;         // in blockstun or hitstun; in a move's recovery
    };
    Cell rows[2][120];
    std::wstring info[2];
    unsigned bg = 0;  // packed RGBA, same layout as IM_COL32
    bool delim = false, cancels = false, crossups = false, inputs = false;
    bool show[2] = {true, true};          // frame bar rows shown
    bool show_inputs[2] = {false, false}; // input rows shown (only under a shown bar)
    bool compact = false;  // folded compact mode: one number per cell, brackets, punish window
    bool blocks = false;   // stretches drawn as one block, but their last few cells
    bool perfect = false;  // mark frame perfect moves
    bool hits = false;     // mark the active frame a move connected on
    bool gaps = false;     // number the free frames between two stuns
    int newest = 0;              // index of the newest cell
    int left[2] = {-1, -1};      // frames of stun or recovery still to come per row, -1 = not shown
    bool both = false;           // both rows stuck: adv holds who gets out first
    int adv[2] = {0, 0};
  };
  void setFrameBar(const FrameBarView& view);

  // Game thread: put every window back to its default place and size.
  void resetWindows();
}

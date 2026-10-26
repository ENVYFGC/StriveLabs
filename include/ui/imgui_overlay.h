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

  // Game thread: the frame bar's cells and stats lines for this frame.
  struct FrameBarView {
    struct Cell {
      unsigned color = 0; unsigned char mods = 0; int trunc = 0; bool on = false;
      unsigned char dir = 0;                         // input bar: numpad, 0 = no data
      unsigned short held = 0, pressed = 0;          // input bar: RECFLG button bits
    };
    Cell rows[2][120];
    std::wstring info[2];
    unsigned bg = 0;  // packed RGBA, same layout as IM_COL32
    bool delim = false, cancels = false, crossups = false, inputs = false;
    bool show[2] = {true, true};          // frame bar rows shown
    bool show_inputs[2] = {false, false}; // input rows shown (only under a shown bar)
    bool blocks = false;   // long stretches drawn as one block, but their last `keep` cells
    bool perfect = false;  // mark frame perfect moves
    int keep = 0;
  };
  void setFrameBar(const FrameBarView& view);

  // Game thread: put every window back to its default place and size.
  void resetWindows();
}

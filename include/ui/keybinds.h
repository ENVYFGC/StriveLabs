// Rebindable inputs. Two tables:
//   hotkeys  - the mod's own actions (menu, pause, takeover...), one VK + one pad mask each
//   game     - takeover inputs (directions, P/K/S/HS/D, Dash, RC, FD, Respect), same shape
// The menu starts a capture; the input layer (keyboard hook drain, pad poll) offers
// inputs to it; the first one wins.
#pragma once
#include <ostream>
#include <string>

namespace binds {
  enum Action { FRAMEBAR, HITBOXES, PAUSE, ADVANCE, MENU, SAVE, TAKEOVER_P1, TAKEOVER_P2, LOAD, UP, DOWN, LEFT, RIGHT, COUNT };
  extern const char* const NAMES[COUNT];      // config ids
  extern const wchar_t* const LABELS[COUNT];  // menu labels
  extern int key[COUNT];                      // VK code, 0 = none
  extern unsigned short pad[COUNT];           // XINPUT_GAMEPAD_* mask, 0 = none

  // Takeover (game) inputs. Pad masks may include the pseudo bits PAD_LT / PAD_RT.
  enum GameAction { G_UP, G_DOWN, G_LEFT, G_RIGHT, G_P, G_K, G_S, G_HS, G_D, G_DASH, G_RC, G_FD, G_RESPECT, G_COUNT };
  extern const char* const G_NAMES[G_COUNT];
  extern const wchar_t* const G_LABELS[G_COUNT];
  extern int gkey[G_COUNT];
  extern unsigned short gpad[G_COUNT];
  extern int use_mod_binds;                   // 0 = game's own mapping in takeover, 1 = these binds
  constexpr unsigned short PAD_LT = 0x0400, PAD_RT = 0x0800;  // unused XInput bits, used for triggers

  unsigned short heldPadMask();               // all pads OR'd, with stick->dpad and triggers folded in
  unsigned int takeoverRecFlag();             // RECFLG bits from the current keyboard + pad state

  bool loadLine(const std::string& k, const std::string& v);  // returns true if consumed
  void save(std::ostream& out);
  std::wstring keyName(int vk);
  std::wstring padName(unsigned short mask);

  // capture ids: 0..COUNT-1 = hotkey row, GAME_BASE+i = game row
  constexpr int GAME_BASE = 100;
  extern int capturing;                       // capture id, -1 = idle
  void beginCapture(int id);
  void cancelCapture();
  void offerKey(int vk);                      // keyboard hook drain, while capturing
  void offerPad(unsigned short heldMask);     // pad poll, on a new button edge, while capturing
  bool consumeDirty();                        // true once after a bind changed (menu saves config)
}

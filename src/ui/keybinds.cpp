#include "keybinds.h"
#include "bind_watcher.h"

#include <Windows.h>
#include <Xinput.h>
#include <cstring>

namespace binds {
  const char* const NAMES[COUNT] = {"framebar", "hitboxes", "pause", "advance", "menu", "save",
                                    "takeover_1p", "takeover_2p", "load", "up", "down", "left", "right"};
  const wchar_t* const LABELS[COUNT] = {L"Toggle Frame Bar", L"Toggle Hitboxes", L"Pause", L"Advance Frame",
                                        L"Toggle Menu", L"Rewind Point + Takeover", L"Takeover 1P", L"Takeover 2P",
                                        L"Rewind", L"Menu Up", L"Menu Down", L"Menu Left", L"Menu Right"};
  int key[COUNT] = {VK_F1, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT};
  unsigned short pad[COUNT] = {0, 0, 0, 0, XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START, 0, 0, 0, 0,
                               XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT};

  const char* const G_NAMES[G_COUNT] = {"up", "down", "left", "right", "p", "k", "s", "hs", "d", "dash", "rc", "fd", "respect"};
  const wchar_t* const G_LABELS[G_COUNT] = {L"Up", L"Down", L"Left", L"Right", L"Punch", L"Kick", L"Slash",
                                            L"Heavy Slash", L"Dust", L"Dash", L"Roman Cancel", L"Faultless Defense", L"Respect"};
  // Defaults: game's keyboard layout (WASD + U I O J K L) and a common pad layout.
  int gkey[G_COUNT] = {'W', 'S', 'A', 'D', 'U', 'I', 'O', 'J', 'K', 0, 'L', 0, 0};
  unsigned short gpad[G_COUNT] = {XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT,
                                  XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_Y, XINPUT_GAMEPAD_B,
                                  XINPUT_GAMEPAD_RIGHT_SHOULDER, 0, XINPUT_GAMEPAD_LEFT_SHOULDER, 0, 0};
  int use_mod_binds = 0;

  // RECFLG bits (see dllmain RECFLG_ENUM; FD is 0x8000)
  static const unsigned int G_RECFLG[G_COUNT] = {0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80, 0x100, 0x400, 0x800, 0x8000, 0x200};

  int capturing = -1;
  static unsigned long long capture_start = 0;
  static bool dirty = false;

  unsigned short heldPadMask() {
    unsigned short m = 0;
    for (DWORD i = 0; i < 4; i++) {
      XINPUT_STATE st{};
      if (XInputGetState(i, &st) != ERROR_SUCCESS) continue;
      const auto& g = st.Gamepad;
      m |= g.wButtons & ~(PAD_LT | PAD_RT);
      if (g.bLeftTrigger  > 100) m |= PAD_LT;
      if (g.bRightTrigger > 100) m |= PAD_RT;
      const int dz = 12000;  // left stick folds into the d-pad bits
      if (g.sThumbLY >  dz) m |= XINPUT_GAMEPAD_DPAD_UP;
      if (g.sThumbLY < -dz) m |= XINPUT_GAMEPAD_DPAD_DOWN;
      if (g.sThumbLX < -dz) m |= XINPUT_GAMEPAD_DPAD_LEFT;
      if (g.sThumbLX >  dz) m |= XINPUT_GAMEPAD_DPAD_RIGHT;
    }
    return m;
  }

  unsigned int takeoverRecFlag() {
    const unsigned short held = heldPadMask();
    unsigned int flag = 0;
    for (int i = 0; i < G_COUNT; i++) {
      const bool kb = gkey[i] && (GetAsyncKeyState(gkey[i]) & 0x8000);
      const bool pd = gpad[i] && (held & gpad[i]) == gpad[i];
      if (kb || pd) flag |= G_RECFLG[i];
    }
    return flag;
  }

  static int indexOf(const std::string& k, const char* prefix, const char* const* names, int count) {
    const size_t n = strlen(prefix);
    if (k.compare(0, n, prefix) != 0) return -1;
    for (int i = 0; i < count; i++) if (k.compare(n, std::string::npos, names[i]) == 0) return i;
    return -1;
  }

  bool loadLine(const std::string& k, const std::string& v) {
    int i;
    try {
      if (k == "takeover_use_mod_binds") { use_mod_binds = std::stoi(v) ? 1 : 0; return true; }
      if ((i = indexOf(k, "bind_", NAMES, COUNT)) >= 0) { key[i] = std::stoi(v) & 0xFF; BindWatcherI::addToFilter(key[i]); return true; }
      if ((i = indexOf(k, "pad_", NAMES, COUNT)) >= 0)  { pad[i] = static_cast<unsigned short>(std::stoi(v)); return true; }
      if ((i = indexOf(k, "gbind_", G_NAMES, G_COUNT)) >= 0) { gkey[i] = std::stoi(v) & 0xFF; return true; }
      if ((i = indexOf(k, "gpad_", G_NAMES, G_COUNT)) >= 0)  { gpad[i] = static_cast<unsigned short>(std::stoi(v)); return true; }
    } catch (...) { return true; }
    return false;
  }

  void save(std::ostream& out) {
    out << "takeover_use_mod_binds=" << use_mod_binds << "\n";
    for (int i = 0; i < COUNT; i++) out << "bind_" << NAMES[i] << "=" << key[i] << "\n";
    for (int i = 0; i < COUNT; i++) out << "pad_" << NAMES[i] << "=" << pad[i] << "\n";
    for (int i = 0; i < G_COUNT; i++) out << "gbind_" << G_NAMES[i] << "=" << gkey[i] << "\n";
    for (int i = 0; i < G_COUNT; i++) out << "gpad_" << G_NAMES[i] << "=" << gpad[i] << "\n";
  }

  std::wstring keyName(int vk) {
    if (vk == 0) return L"none";
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    switch (vk) {  // extended keys need the extended bit for a correct name
      case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_INSERT: case VK_DELETE:
      case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT: case VK_DIVIDE: case VK_NUMLOCK:
      case VK_LWIN: case VK_RWIN: case VK_APPS: case VK_RCONTROL: case VK_RMENU:
        sc |= 0x100; break;
      default: break;
    }
    wchar_t buf[64];
    if (GetKeyNameTextW(static_cast<LONG>(sc << 16), buf, 64) > 0) return buf;
    return L"VK " + std::to_wstring(vk);
  }

  std::wstring padName(unsigned short mask) {
    static const struct { unsigned short bit; const wchar_t* name; } T[] = {
      {XINPUT_GAMEPAD_DPAD_UP, L"DUp"}, {XINPUT_GAMEPAD_DPAD_DOWN, L"DDown"}, {XINPUT_GAMEPAD_DPAD_LEFT, L"DLeft"},
      {XINPUT_GAMEPAD_DPAD_RIGHT, L"DRight"}, {XINPUT_GAMEPAD_START, L"Start"}, {XINPUT_GAMEPAD_BACK, L"Back"},
      {XINPUT_GAMEPAD_LEFT_THUMB, L"LS"}, {XINPUT_GAMEPAD_RIGHT_THUMB, L"RS"}, {XINPUT_GAMEPAD_LEFT_SHOULDER, L"LB"},
      {XINPUT_GAMEPAD_RIGHT_SHOULDER, L"RB"}, {PAD_LT, L"LT"}, {PAD_RT, L"RT"}, {XINPUT_GAMEPAD_A, L"A"},
      {XINPUT_GAMEPAD_B, L"B"}, {XINPUT_GAMEPAD_X, L"X"}, {XINPUT_GAMEPAD_Y, L"Y"}};
    if (mask == 0) return L"none";
    std::wstring s;
    for (const auto& t : T) if (mask & t.bit) { if (!s.empty()) s += L"+"; s += t.name; }
    return s.empty() ? std::wstring(L"?") : s;
  }

  void beginCapture(int id) {
    capturing = id;
    capture_start = GetTickCount64();
    BindWatcherI::setCaptureAll(true);
  }
  void cancelCapture() { capturing = -1; BindWatcherI::setCaptureAll(false); }

  // The first 300 ms are ignored so the confirm key's own repeat does not get captured.
  static bool tooEarly() { return GetTickCount64() - capture_start < 300; }

  void offerKey(int vk) {
    if (capturing < 0 || tooEarly()) return;
    if (vk == VK_ESCAPE) { cancelCapture(); return; }
    if (vk == VK_BACK) {  // clear both binds of this row
      if (capturing >= GAME_BASE) { gkey[capturing - GAME_BASE] = 0; gpad[capturing - GAME_BASE] = 0; }
      else { key[capturing] = 0; pad[capturing] = 0; }
      dirty = true; cancelCapture(); return;
    }
    if (capturing >= GAME_BASE) gkey[capturing - GAME_BASE] = vk;
    else { key[capturing] = vk; BindWatcherI::addToFilter(vk); }
    dirty = true;
    cancelCapture();
  }
  void offerPad(unsigned short heldMask) {
    if (capturing < 0 || tooEarly() || heldMask == 0) return;
    if (capturing >= GAME_BASE) gpad[capturing - GAME_BASE] = heldMask;
    else pad[capturing] = heldMask;
    dirty = true;
    cancelCapture();
  }
  bool consumeDirty() { const bool d = dirty; dirty = false; return d; }
}

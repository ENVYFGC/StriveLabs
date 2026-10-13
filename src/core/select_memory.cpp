// Remember character select picks across restarts.
//
// Character select keeps the last picks of both sides (character, color, costume,
// stage, music...) in one static record in memory and starts the next select on
// them. That record is not saved anywhere, so it is empty after every restart.
// This saves it to ue4ss/StriveLabs_last_picks.txt when a match starts, and writes
// it back at startup, before the first select.
//
// Layout, as read from the running game:
//   1P picks, 2P picks   8 x int32 each: chara, color, costume, script,
//                        stage, bgm, sp flag, skill set
//   uint8 valid          +0x40
//   int32 main side      +0x44 (side that picked stage and music)
#include "sigscan.h"
#include "combo_log.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {
  struct DecideInfo { int32_t v[8]; };
  struct History {
    DecideInfo side[2];
    uint8_t valid;
    uint8_t pad[3];
    int32_t main_side;
  };
  static_assert(sizeof(History) == 0x48, "picks record layout");

  History* g_history = nullptr;
  std::filesystem::path g_file;
  History g_last_saved{};

  bool sane(const History& h) {
    if (h.valid != 1 || (h.main_side != 0 && h.main_side != 1)) return false;
    for (const auto& s : h.side) {
      const int32_t* v = s.v;
      if (v[0] < 0 || v[0] > 80 || v[1] < 0 || v[1] > 64 || v[2] < 0 || v[2] > 16 || v[3] < 0 || v[3] > 32 ||
          v[4] < 0 || v[4] > 255 || v[5] < -1 || v[5] > 4096 || v[6] < 0 || v[6] > 255 || v[7] < 0 || v[7] > 64)
        return false;
    }
    return true;
  }
}

namespace select_memory {
  void init(const std::filesystem::path& dir) {
    g_file = dir / "StriveLabs_last_picks.txt";
    try {
      // The code that fills the record on entering a match: mov byte ptr [valid], 1 ; lea rsi,[rcx+picks] ; movups xmm0,[rsi]
      const uintptr_t hit = sigscan::get().scan("\xC6\x05\x00\x00\x00\x00\x01\x48\x8D\xB1\x00\x00\x00\x00\x0F\x10\x06",
                                                "xx????xxxx????xxx");
      const uintptr_t is_valid = hit + 7 + *reinterpret_cast<const int32_t*>(hit + 2);
      g_history = reinterpret_cast<History*>(is_valid - 0x40);
    } catch (...) {
      COMBO_LOG("last picks: NOT found");
      return;
    }

    std::ifstream in(g_file);
    if (!in) { COMBO_LOG("last picks: found, no saved picks yet"); return; }
    History h{};
    std::string line;
    int sides_read = 0;
    while (std::getline(in, line)) {
      const auto eq = line.find('=');
      if (eq == std::string::npos) continue;
      const std::string key = line.substr(0, eq);
      std::istringstream vals(line.substr(eq + 1));
      if (key == "p1" || key == "p2") {
        auto& s = h.side[key == "p1" ? 0 : 1];
        char comma;
        for (int i = 0; i < 8; i++) { vals >> s.v[i]; if (i < 7) vals >> comma; }
        if (vals) sides_read++;
      } else if (key == "main_side") {
        vals >> h.main_side;
      }
    }
    h.valid = 1;
    if (sides_read != 2 || !sane(h)) { COMBO_LOG("last picks: saved file ignored (unexpected values)"); return; }
    if (g_history->valid) return;  // the game already has picks from this session
    std::memcpy(g_history, &h, sizeof(History));
    g_last_saved = h;
    COMBO_LOG("last picks: restored");
  }

  // Call when a match starts: the record now holds this select's picks.
  void save() {
    if (!g_history || g_file.empty()) return;
    History h;
    std::memcpy(&h, g_history, sizeof(History));
    if (!sane(h) || std::memcmp(&h, &g_last_saved, sizeof(History)) == 0) return;
    std::ofstream out(g_file, std::ios::trunc);
    if (!out) return;
    out << "# StriveLabs: last character select picks, restored at startup. Delete to reset.\n";
    out << "# chara, color, costume, script, stage, bgm, sp flag, skill set\n";
    for (int s = 0; s < 2; s++) {
      out << (s == 0 ? "p1=" : "p2=");
      for (int i = 0; i < 8; i++) out << h.side[s].v[i] << (i < 7 ? "," : "\n");
    }
    out << "main_side=" << h.main_side << "\n";
    g_last_saved = h;
  }
}

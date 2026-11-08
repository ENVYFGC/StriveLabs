// Recolor sharing between StriveLabs players. The two games find each other's Steam account through the
// handshake's intro pattern, then talk over Steam's networking messages on a channel of our own, next to the
// match (which runs on EOS, not Steam). Step one: a hello each way, logged. Off unless turned on (it tells the
// opponent's game your Steam account). Off by itself when Steam networking isn't there (Game Pass).
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace recolor_share {
  void init(const std::filesystem::path& ue4ss_dir);  // Loads the setting and finds Steam's functions
  bool available();           // false: sharing is switched off in this build (recolors show by baking)
  bool enabled();             // the setting
  void setEnabled(bool on);   // any thread; saved
  uint32_t myAccount();       // our Steam account id (low 32 bits of the SteamID), 0 = no Steam networking
  bool sharing();             // On, with Steam networking available, so the handshake sends our account
  // Game thread: the opponent's account, read from their intro pattern.
  void opponentFound(uint32_t account);
  // Game thread, every system update.
  void update(bool battle_running);
  std::string status();       // one line for the panel (any thread)
  bool fightRunning();        // Any thread. True while a fight runs (the online version can't be picked then)
}

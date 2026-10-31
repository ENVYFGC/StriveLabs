// Online version handshake. With the online version in use, both games must run the
// same one or the match quietly plays out differently on each screen (the game itself
// never checks). In round 1, before anyone can act, the mod sends a short pattern of
// directions carrying its version through the game's own input stream (neutral, up and
// forward only: the game ignores them then), reads the opponent's, and when the round
// starts leaves the match through the game's own abort if they differ or none came.
// Game thread only.
#pragma once
#include <cstdint>

namespace handshake {
  void install();  // finds what it needs in the game; logs what's missing
  // Battle update, every frame, any mode (online fights aren't the mod's other modes).
  void update(void* game_state);
  // Every analyzer's input as the battle takes it (rollback re-simulation too).
  void onBattleInput(const void* analyzer, uint32_t recflag);
}

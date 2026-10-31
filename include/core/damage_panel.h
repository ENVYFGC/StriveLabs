// The game's training damage panel (Damage, Combo Damage, Max Combo Damage, Total
// Action, Attack Startup, Frame Advantage, Follow-up Window, Attack Interval), read
// from the game's own text each frame and shown in StriveLabs windows, one per side,
// that can be moved and resized. The game's own panel can be hidden. Game thread only.
#pragma once

namespace damage_panel {
  // Battle update, in training: finds the game's panel at the start of a fight, then
  // sends its text to the overlay every frame.
  void update(bool round_started);
  // No battle running any more: the game's widgets go away, so let go of them.
  void release();
}

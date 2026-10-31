// Whether the game's own pause menu is open in a fight, so the overlay can step out of
// its way. Game thread only.
#pragma once

namespace game_menu {
  // Battle update, every frame (paused too): looks for the game's pause menu once per
  // fight, then checks whether it's on screen, and tells the overlay.
  void update();
  // No battle running any more: the menu's widgets aren't ours to keep.
  void release();
}

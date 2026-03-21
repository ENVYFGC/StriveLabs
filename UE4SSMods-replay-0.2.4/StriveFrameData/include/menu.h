#pragma once

#include "draw_utils.h"
#include "combo_menu.h"

struct Palette {
  FLinearColor projectile_color;
  FLinearColor background_color;
  FLinearColor state_colors[8];
};

struct CurrentOptions {
  const Palette& palette;
  bool show_fade;
  bool show_delim;
  bool show_cancels;
  bool show_crossups;
};

enum class PlayerDisplayMode {
  Disabled = 0,
  P1 = 1,
  P2 = 2,
  Both = 3,
};

struct TensionOptions {
  bool enabled;
  PlayerDisplayMode gains;
  bool show_advantage;
  bool show_adv_change;
  PlayerDisplayMode bursts;
  PlayerDisplayMode pulses;
  PlayerDisplayMode current_tension;
};

// Positions for the two repositionable overlays.
//
// Coordinate systems are intentionally kept distinct here:
//
//   framebar_center_x_ratio  — screen-space anchor, 0.0 = left edge, 1.0 = right edge
//   framebar_center_y_ratio  — screen-space anchor, 0.0 = top edge,  1.0 = bottom edge
//
//   tension_left_offset      — draw-space horizontal offset from the framebar anchor
//   tension_bottom_gap       — draw-space vertical gap between the tension box bottom
//                              edge and the top of the framebar background rect.
//                              Positive = further above the bar.
//
// The tension box uses the same DrawContext anchor as the framebar, so both
// overlays move together when the bar is repositioned.  tension_left_offset and
// tension_bottom_gap only describe the tension box's position *relative to the bar*.
struct LayoutPositions {
  double framebar_center_x_ratio;
  double framebar_center_y_ratio;
  int tension_left_offset;   // draw-space units, negative = left of center
  int tension_bottom_gap;    // draw-space units, positive = above bar top edge
};

struct PressedKeys {
  bool toggle_framebar;
  bool toggle_hitbox;
  bool toggle_menu;
  bool go_up;
  bool go_down;
  bool rotate_right;
  bool rotate_left;
};


class ModMenu {
  DrawContext tool;

  bool is_showing = false;
  int cursor_position = 0;
  size_t current_page = 0;

  ComboMenu combo_menu_;

  void changeSetting(size_t idx, bool right=true);

  ModMenu();
public:
  ~ModMenu();
  static ModMenu& instance();

  void update(PressedKeys data);
  void draw();

  bool barEnabled() const;
  bool hitboxEnabled() const;
  bool fadeEnabled() const;
  bool delimEnabled() const;
  bool cancelEnabled() const;
  bool dashEnabled() const;
  int pauseType() const;
  int delayAmount() const;
  bool crossupEnabled() const;
  TensionOptions getTensionOptions() const;
  LayoutPositions getLayoutPositions() const;

  CurrentOptions getScheme() const;

  void initComboMenu(const std::filesystem::path& data_dir);
  ComboMenu& comboMenu() { return combo_menu_; }
};

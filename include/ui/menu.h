#pragma once

#include "draw_utils.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

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
  PlayerDisplayMode penalties;
  PlayerDisplayMode current_tension;
  PlayerDisplayMode wall_damage;
  bool right_side;  // mirror the box to the other side of the frame bar anchor
  int decimals;     // digits after the decimal point for gain, burst, tension...
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
  double detail_margin_t;    // draw-space units from screenTopEdge()  (Y position)
  double detail_margin_r;    // draw-space units from screenRightEdge() (right inset)
  int detail_panel_width;    // draw-space width of the detail panel (default 903)
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


// What the ImGui menu draws. Built on the game thread at the end of each menu
// update and copied under a lock by the render thread, so ImGui never touches
// live menu state. Strings are UTF-8.
struct MenuRowView {
  enum class Kind { Back, Open, Choice, Action, Number, Bind } kind = Kind::Open;
  std::string label;
  std::string section;               // heading before this row; not a selectable row
  std::string value;                 // Action/Number/Bind/Open text
  std::vector<std::string> choices;  // Choice options, cleaned of the old "<  >" padding
  int choice = -1;                   // selected index for Choice
  bool capturing = false;            // Bind row waiting for an input
};

// A page popped out into its own ImGui window.
struct MenuPanelView {
  int page = 0;
  std::string title;
  std::vector<MenuRowView> rows;
  std::string status;
};

struct MenuView {
  bool showing = false;
  int page = 0;              // page shown in the right pane (MenuPage value)
  bool poppable = false;     // that page can be popped out into its own window
  bool tension_page = false; // its header button toggles the tension overlay window instead
  bool tension_on = false;   // Tension Overlay setting
  bool meters_right = false; // Overlay Side setting: put the meters window on the right
  bool meters_snap = false;  // Layout: keep the meters window on top of the frame bar
  // Appearance
  bool hide_window_decorations = false; // hide passive overlay chrome while the menu is closed
  unsigned accent_rgb = 0xFF0000;  // 0xRRGGBB
  unsigned custom_rgb = 0xFF0000;
  float opacity = 0.94f;           // panel background alpha
  int picker_seq = 0;              // changes when the custom color picker should open
  // Timeline window
  int mode = 0;              // 0 = other, 1 = training, 2 = replay
  bool timeline_on = true;
  bool paused = false;
  bool recording = false;
  unsigned rec_frames = 0;
  std::string rec_status;
  std::vector<MenuRowView> rec_rows;  // Recordings page rows, for the timeline window
  std::vector<MenuPanelView> panels;
  int nav = 0;               // highlighted entry in the side menu
  bool nav_focus = true;     // true on the root page: the cursor is in the side menu
  bool back_focus = false;   // in a page, on the hidden Back row: shown on the side menu entry
  std::vector<std::string> nav_items;
  std::string title;
  std::vector<MenuRowView> rows;
  int cursor = 0;            // selected row in the right pane, -1 = none (rows never include Back)
  std::string status;
  float x_ratio = 0.5f, y_ratio = 0.5f, extra_width = 0.0f;
};

// Mouse actions from the ImGui menu, applied on the game thread in update().
struct MenuClick {
  enum class Type { OpenNav, Row, SetChoice, Move, TensionWindow, CloseMenu,
                    TlRewind, TlSetPoint, TlPlay, TlStep, TlRecord, TlShow, BarWindow, SetColor } type;  // SetColor: a = 0xRRGGBB  // Move: a, b = center x, y ratio * 10000
  int a = 0;          // nav index or row index
  int b = 0;          // Row: 1 = right / activate, 0 = left; SetChoice: choice index
  int page = -1;      // Row / SetChoice in a popped-out window: that window's page
};

class ModMenu {
  DrawContext tool;

  bool is_showing = false;
  int cursor_position = 0;
  size_t current_page = 0;
  std::atomic<uint64_t> rewind_point_notification_until{0};

  void changeSetting(size_t idx, bool right=true);
  void buildView();
  void applyClick(const MenuClick& click);
  void updateInner(PressedKeys data);
  void drawLayoutGhost(int page);   // layout sub-page preview boxes, drawn on the HUD
  bool hud_panel = true;            // false while ImGui draws the menu

  ModMenu();
public:
  ~ModMenu();
  static ModMenu& instance();

  void update(PressedKeys data);
  void draw();

  void notifyRewindPointCreated();
  bool isRewindPointNotificationVisible() const;

  bool isShowing() const { return is_showing; }
  void hide();                           // game thread: close the menu (left training / replay)
  MenuView view() const;                 // render thread: copy of the latest snapshot
  void click(const MenuClick& click);    // render thread: queue a mouse action
  void setPopped(unsigned mask);         // render thread: bit per MenuPage shown in its own window
  static std::filesystem::path dataDir(); // ue4ss folder (fonts, logo, config)
  static float uiScale(int which = 0);    // size setting: 0 menu, 1 meters, 2 frame bar, 3 timeline (any thread)
  static std::string overlayFont();       // meters and frame bar font file in StriveLabs_fonts, "" = Barlow (any thread)
  bool barEnabled() const;
  bool hitboxEnabled() const;
  bool fadeEnabled() const;
  bool delimEnabled() const;
  bool cancelEnabled() const;
  bool dashEnabled() const;
  int pauseType() const;
  int delayAmount() const;
  int rewindDelaySeconds() const;
  bool crossupEnabled() const;
  TensionOptions getTensionOptions() const;
  LayoutPositions getLayoutPositions() const;

  CurrentOptions getScheme() const;
};

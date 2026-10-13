// ──────────────────────────────────────────────────────────────────────────────
// TENSION_DEBUG_SCAN
// Define this to replace the tension overlay with a raw memory scanner.
// It dumps the int values at each known old offset ± SCAN_RADIUS ints so you
// can identify which offsets changed after a game update.
//
// Steps:
//   1. Build with TENSION_DEBUG_SCAN defined (add to xmake.lua defines or here)
//   2. Load the mod in training mode
//   3. Enable the tension overlay in the F5 menu so the scanner is visible
//   4. Watch which rows change when you gain/spend tension, burst, etc.
//   5. Note the hex offset shown on the left next to the matching value
//   6. Report those offsets back so arcsys.h can be updated
//   7. Remove this #define and rebuild for the normal overlay
// ──────────────────────────────────────────────────────────────────────────────
// #define TENSION_DEBUG_SCAN

#include "tension_overlay.h"

#include "arcsys.h"
#include "menu.h"
#include "imgui_overlay.h"

#include <UnrealDef.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <string>
#include <vector>

namespace {
  constexpr int BOX_WIDTH = 620;
  constexpr int BOX_PADDING = 16;
  constexpr int LINE_HEIGHT = 40;
  constexpr double TEXT_SCALE = 1.2;

  // Wall damage lives on the character being hit, at +0xD860 (read from the game's
  // wall code). Once it is at 3000 or more, the next wall hit sticks the defender
  // and the game adds 10000; at 10000 or more the next hit breaks the wall, and it
  // resets to 0.
  // Thresholds are the game's own values, hardcoded. Read them from the game if a patch changes them.
  constexpr int WALL_DAMAGE_OFFSET = 0xD860;
  constexpr int WALL_STICK = 3000;
  constexpr int WALL_BREAK = 10000;

  int read_wall_damage(const void* player) {
    return player ? *reinterpret_cast<const int*>(reinterpret_cast<const char*>(player) + WALL_DAMAGE_OFFSET) : 0;
  }

  std::wstring format_wall(const int value) {
    if (value >= WALL_BREAK) return L"STICK (next hit breaks)";
    if (value >= WALL_STICK) return std::to_wstring(value) + L" (next hit sticks)";
    return std::to_wstring(value) + L" / " + std::to_wstring(WALL_STICK);
  }

  void append_wall_lines(std::vector<std::wstring>& lines, const PlayerDisplayMode mode, const int p1, const int p2) {
    if (mode == PlayerDisplayMode::P1 || mode == PlayerDisplayMode::Both) lines.emplace_back(L"P1 Wall Damage: " + format_wall(p1));
    if (mode == PlayerDisplayMode::P2 || mode == PlayerDisplayMode::Both) lines.emplace_back(L"P2 Wall Damage: " + format_wall(p2));
  }

  FLinearColor overlay_background{0.05f, 0.05f, 0.05f, 0.75f};

  std::wstring format_number(const double value) {
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%.2f", value);
    return buffer;
  }

  std::wstring format_integer(const int value) {
    return std::to_wstring(value);
  }

  void append_player_lines(
      std::vector<std::wstring>& lines,
      const PlayerDisplayMode mode,
      const wchar_t* p1_label,
      const double p1_value,
      const wchar_t* p2_label,
      const double p2_value) {
    switch (mode) {
    case PlayerDisplayMode::P1:
      lines.emplace_back(std::wstring(p1_label) + format_number(p1_value));
      break;
    case PlayerDisplayMode::P2:
      lines.emplace_back(std::wstring(p2_label) + format_number(p2_value));
      break;
    case PlayerDisplayMode::Both:
      lines.emplace_back(std::wstring(p1_label) + format_number(p1_value));
      lines.emplace_back(std::wstring(p2_label) + format_number(p2_value));
      break;
    case PlayerDisplayMode::Disabled:
    default:
      break;
    }
  }

  void append_player_lines(
      std::vector<std::wstring>& lines,
      const PlayerDisplayMode mode,
      const wchar_t* p1_label,
      const int p1_value,
      const wchar_t* p2_label,
      const int p2_value) {
    switch (mode) {
    case PlayerDisplayMode::P1:
      lines.emplace_back(std::wstring(p1_label) + format_integer(p1_value));
      break;
    case PlayerDisplayMode::P2:
      lines.emplace_back(std::wstring(p2_label) + format_integer(p2_value));
      break;
    case PlayerDisplayMode::Both:
      lines.emplace_back(std::wstring(p1_label) + format_integer(p1_value));
      lines.emplace_back(std::wstring(p2_label) + format_integer(p2_value));
      break;
    case PlayerDisplayMode::Disabled:
    default:
      break;
    }
  }
}

struct TensionOverlay::Data {
  DrawContext tool{0.5, 0.8};

  double p1_tension{};
  double p2_tension{};
  double p1_base_tension{};
  double p2_base_tension{};
  double p1_tension_gain{};
  double p2_tension_gain{};
  double p1_burst{};
  double p2_burst{};
  int p1_tension_pulse{};
  int p2_tension_pulse{};
  int p1_tension_penalty{};
  int p2_tension_penalty{};
  int p1_wall_damage{};
  int p2_wall_damage{};
  double p2_base_burst{};
  double p2_burst_gain{};
  double tension_advantage{};
  double previous_tension_advantage{};
  double tension_advantage_old{};
  double tension_slope{};

  // History arrays store [frame-1, frame-2, frame-3] values.
  // They are shifted before reading the new engine value so history[0]
  // always holds the previous frame's reading.
  std::array<double, 3> p1_tension_history{};
  std::array<double, 3> p2_tension_history{};
  std::array<double, 3> p2_burst_history{};

  std::wstring p1_damage{L"0"};
  std::wstring p2_damage{L"0"};
  std::wstring p1_single_damage{L"0"};
  std::wstring p1_damage_repeat_check{L"0"};

  int zero_check{};

  void reset_runtime() {
    p1_tension = 0.0;
    p2_tension = 0.0;
    p1_base_tension = 0.0;
    p2_base_tension = 0.0;
    p1_tension_gain = 0.0;
    p2_tension_gain = 0.0;
    p1_burst = 0.0;
    p2_burst = 0.0;
    p1_tension_pulse = 0;
    p2_tension_pulse = 0;
    p1_tension_penalty = 0;
    p2_tension_penalty = 0;
    p1_wall_damage = 0;
    p2_wall_damage = 0;
    p2_base_burst = 0.0;
    p2_burst_gain = 0.0;
    tension_advantage = 0.0;
    previous_tension_advantage = 0.0;
    tension_advantage_old = 0.0;
    tension_slope = 0.0;
    p1_tension_history = {};
    p2_tension_history = {};
    p2_burst_history = {};
    p1_damage = L"0";
    p2_damage = L"0";
    p1_single_damage = L"0";
    p1_damage_repeat_check = L"0";
    zero_check = 0;
  }

  void reset() {
    reset_runtime();
  }

  bool update_values() {
    auto* engine = asw_engine::get();
    if (!engine) {
      return false;
    }

    p1_tension_history[2] = p1_tension_history[1];
    p1_tension_history[1] = p1_tension_history[0];
    p1_tension_history[0] = p1_tension;

    p2_tension_history[2] = p2_tension_history[1];
    p2_tension_history[1] = p2_tension_history[0];
    p2_tension_history[0] = p2_tension;

    p2_burst_history[2] = p2_burst_history[1];
    p2_burst_history[1] = p2_burst_history[0];
    p2_burst_history[0] = p2_burst;

    p1_tension = static_cast<double>(engine->P1Tension) / 100.0;
    p2_tension = static_cast<double>(engine->P2Tension) / 100.0;
    p1_burst = static_cast<double>(engine->P1BurstRaw) / 150.0;
    p2_burst = static_cast<double>(engine->P2BurstRaw) / 150.0;
    p1_tension_pulse = engine->P1TensionPulse;
    p2_tension_pulse = engine->P2TensionPulse;
    p1_tension_penalty = engine->P1TensionPenalty;
    p2_tension_penalty = engine->P2TensionPenalty;
    p1_wall_damage = read_wall_damage(engine->players[0].entity);
    p2_wall_damage = read_wall_damage(engine->players[1].entity);
    auto* p1 = engine->players[0].entity;
    auto* p2 = engine->players[1].entity;

    if (p1_damage == L"0" && zero_check == 0) {
      p1_tension_gain = 0.0;
      p2_tension_gain = 0.0;
      tension_advantage = 0.0;
      previous_tension_advantage = 0.0;
      tension_slope = 0.0;
      p1_base_tension = p1_tension;
      p2_base_tension = p2_tension;
      tension_advantage_old = 0.0;
      p1_tension_history[2] = p1_tension;
      p2_tension_history[2] = p2_tension;
      p1_tension_history[1] = p1_tension;
      p2_tension_history[1] = p2_tension;
      zero_check = 1;
    } else if (p1_damage == p1_single_damage && p1_single_damage != p1_damage_repeat_check) {
      p1_base_tension = p1_tension_history[2];
      p2_base_tension = p2_tension_history[2];
      p2_base_burst = p2_burst_history[2];
      p1_damage_repeat_check = p1_single_damage;
    } else if (p1_damage != p1_single_damage) {
      p1_damage_repeat_check = L"0";
    }

    p1_tension_gain = p1_tension - p1_base_tension;
    p2_tension_gain = p2_tension - p2_base_tension;
    p2_burst_gain = p2_burst - p2_base_burst;

    tension_advantage_old = tension_advantage;
    tension_advantage = p1_tension_gain - p2_tension_gain;
    if (tension_advantage != tension_advantage_old) {
      previous_tension_advantage = tension_advantage_old;
    }
    tension_slope = tension_advantage - previous_tension_advantage;

    return true;
  }

#ifdef TENSION_DEBUG_SCAN
  // ── Debug scanner ─────────────────────────────────────────────────────────
  // Prints raw int values at old known tension offsets ± SCAN_RADIUS ints.
  // Look for values that move when you gain/spend tension or burst in training.
  static constexpr int SCAN_RADIUS = 6; // show ±6 ints (±0x18 bytes) per anchor

  struct ScanAnchor {
    const wchar_t* label;
    size_t old_offset; // old offset inside asw_engine
  };

  static constexpr ScanAnchor ANCHORS[] = {
    { L"P1Tension",        0x060  },
    { L"P1TensionPenalty", 0x158  },
    { L"P1TensionPulse",   0x164  },
    { L"P1Danger",         0x170  },
    { L"P1PositiveBonus",  0x178  },
    { L"P2Tension",        0x1F8  },
    { L"P2TensionPenalty", 0x2F0  },
    { L"P2TensionPulse",   0x2FC  },
    { L"P2Danger",         0x308  },
    { L"P2PositiveBonus",  0x310  },
    { L"P1BurstRaw",       0x14C8 },
    { L"P2BurstRaw",       0x14CC },
  };

  void draw_debug_scan() {
    auto* engine = asw_engine::get();
    if (!engine) return;

    const auto* base = reinterpret_cast<const uint8_t*>(engine);

    const auto options = ModMenu::instance().getTensionOptions();
    if (!options.enabled) return;

    const auto layout = ModMenu::instance().getLayoutPositions();
    tool.setRatios(0.02, 0.05); // top-left anchor for the scanner panel
    tool.update();

    FLinearColor bg     { 0.04f, 0.04f, 0.08f, 0.88f };
    FLinearColor hdr_col{ 1.00f, 0.80f, 0.20f, 1.00f };
    FLinearColor val_col{ 0.90f, 0.90f, 0.90f, 1.00f };
    FLinearColor anc_col{ 0.40f, 1.00f, 0.40f, 1.00f }; // highlights the anchor row

    constexpr int COL_W   = 460;
    constexpr int ROW_H   = 22;
    constexpr int PAD     = 8;
    constexpr double SCALE = 0.75;

    const int rows_per_anchor = SCAN_RADIUS * 2 + 2; // label + values
    const int total_rows      = static_cast<int>(std::size(ANCHORS)) * rows_per_anchor;
    const int panel_h         = PAD * 2 + total_rows * ROW_H;
    const int panel_w         = COL_W + PAD * 2;

    tool.drawRect(0, 0, panel_w, panel_h, bg);

    int cy = PAD;
    wchar_t buf[128];

    for (const auto& anchor : ANCHORS) {
      // anchor label header
      tool.drawColoredText(PAD, cy, RC::Unreal::FString(anchor.label), hdr_col, SCALE);
      cy += ROW_H;

      for (int i = -SCAN_RADIUS; i <= SCAN_RADIUS; ++i) {
        const ptrdiff_t byte_off = static_cast<ptrdiff_t>(anchor.old_offset) + i * 4;
        if (byte_off < 0 || byte_off + 4 > 0x20000) {
          cy += ROW_H;
          continue;
        }
        int val = 0;
        std::memcpy(&val, base + byte_off, sizeof(val));
        const bool is_anchor = (i == 0);
        swprintf_s(buf, L"  [%+04d / 0x%04zX]  %d", i * 4, static_cast<size_t>(byte_off), val);
        tool.drawColoredText(PAD, cy, RC::Unreal::FString(buf), is_anchor ? anc_col : val_col, SCALE);
        cy += ROW_H;
      }
    }
  }
  // ── End debug scanner ─────────────────────────────────────────────────────
#endif // TENSION_DEBUG_SCAN

  void draw() {
    const auto options = ModMenu::instance().getTensionOptions();
    if (!options.enabled) {
      return;
    }

#ifdef TENSION_DEBUG_SCAN
    draw_debug_scan();
    return;
#endif

    if (!update_values()) {
      return;
    }

    // The tension box shares the framebar's DrawContext anchor so that moving
    // the bar moves the tension box with it.
    const auto layout = ModMenu::instance().getLayoutPositions();
    tool.setRatios(layout.framebar_center_x_ratio, layout.framebar_center_y_ratio);
    tool.update();

    // Derive draw-space position relative to the shared anchor.
    // BAR_ONE_TOP mirrors the framebar.cpp constant.
    constexpr int BAR_ONE_TOP = -48;
    // Right side: mirror the box around the anchor, keeping the same distance from it.
    const int box_left = options.right_side ? -layout.tension_left_offset - BOX_WIDTH : layout.tension_left_offset;

    std::vector<std::wstring> lines;
    append_player_lines(lines, options.gains, L"P1 Tension Gain: ", p1_tension_gain, L"P2 Tension Gain: ", p2_tension_gain);
    if (options.show_advantage) lines.emplace_back(L"Tension Advantage: " + format_number(tension_advantage));
    if (options.show_adv_change) lines.emplace_back(L"Tension Adv. Change: " + format_number(tension_slope));
    append_player_lines(lines, options.bursts, L"P1 Burst: ", p1_burst, L"P2 Burst: ", p2_burst);
    append_player_lines(lines, options.pulses, L"P1 Tension Pulse: ", p1_tension_pulse, L"P2 Tension Pulse: ", p2_tension_pulse);
    append_player_lines(lines, options.penalties, L"P1 Tension Penalty: ", p1_tension_penalty, L"P2 Tension Penalty: ", p2_tension_penalty);
    append_player_lines(lines, options.current_tension, L"P1 Tension: ", p1_tension, L"P2 Tension: ", p2_tension);
    append_wall_lines(lines, options.wall_damage, p1_wall_damage, p2_wall_damage);

    // With ImGui running, the lines go to the tension overlay window instead.
    imgui_overlay::setTensionLines(lines);
    if (imgui_overlay::running()) return;

    if (lines.empty()) {
      return;
    }

    const int box_height = BOX_PADDING * 2 + static_cast<int>(lines.size()) * LINE_HEIGHT;
    const int box_top = BAR_ONE_TOP - layout.tension_bottom_gap - box_height;
    tool.drawRect(box_left, box_top, BOX_WIDTH, box_height, overlay_background);

    for (size_t idx = 0; idx < lines.size(); ++idx) {
      const int top = box_top + BOX_PADDING + static_cast<int>(idx) * LINE_HEIGHT;
      tool.drawOutlinedText(box_left + BOX_PADDING, top, RC::Unreal::FString(lines[idx].c_str()), TEXT_SCALE);
    }
  }
};

TensionOverlay::~TensionOverlay() = default;
TensionOverlay::TensionOverlay()
    : data(new Data()) {}

void TensionOverlay::reset() { d().reset(); }
void TensionOverlay::draw() { d().draw(); }

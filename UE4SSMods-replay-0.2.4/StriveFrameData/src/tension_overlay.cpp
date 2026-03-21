#include "tension_overlay.h"

#include "arcsys.h"
#include "menu.h"

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

  void draw() {
    const auto options = ModMenu::instance().getTensionOptions();
    if (!options.enabled) {
      return;
    }

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
    const int box_left = layout.tension_left_offset;

    std::vector<std::wstring> lines;
    append_player_lines(lines, options.gains, L"P1 Tension Gain: ", p1_tension_gain, L"P2 Tension Gain: ", p2_tension_gain);
    if (options.show_advantage) lines.emplace_back(L"Tension Advantage: " + format_number(tension_advantage));
    if (options.show_adv_change) lines.emplace_back(L"Tension Adv. Change: " + format_number(tension_slope));
    append_player_lines(lines, options.bursts, L"P1 Burst: ", p1_burst, L"P2 Burst: ", p2_burst);
    append_player_lines(lines, options.pulses, L"P1 Tension Pulse: ", p1_tension_pulse, L"P2 Tension Pulse: ", p2_tension_pulse);
    append_player_lines(lines, options.current_tension, L"P1 Tension: ", p1_tension, L"P2 Tension: ", p2_tension);

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

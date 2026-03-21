#include "menu.h"
#include "video_player.h"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

// Color Stuff
float OneOver255 = 1.0f / 255.0f;

// Standard sRGB-to-linear conversion formula:
// c/12.92 for low values, ((c + 0.055) / 1.055)^2.4 for high values.
// 0.0521327 is precomputed as 0.055 / 1.055.
float sRGBToLinear(int pc) {
  float c = pc * OneOver255;
  return c > 0.04045f ? pow(c * (1.0f / 1.055f) + 0.0521327f, 2.4f) : c * (1.0f / 12.92f);
}

FLinearColor convSRGB(int r, int g, int b, int a) {
  return FLinearColor{sRGBToLinear(r), sRGBToLinear(g), sRGBToLinear(b), a * OneOver255};
}

// Option settings (integer index into a fixed option list)

struct OptionData {
  const wchar_t *title;
  size_t count;
  std::array<const wchar_t *, 5> values;
};

struct SettingsEntry {
  const OptionData display;
  const std::string id_;
  size_t value;
};

// Float settings
// Used for continuous values such as overlay positions. Stored in the same
// config file but written with fixed precision and parsed explicitly as doubles
// to avoid config drift or integer-parse failures.
//
// The id_ string encodes the coordinate system to prevent future misuse:
//   *_ratio  - a screen-space ratio in [0.0, 1.0]
//   *_offset - a draw-space integer offset in local units
//   *_gap    - a draw-space positive distance in local units
struct FloatSetting {
  const std::string id_;
  double value;
  const double default_value;
  const double min_value;
  const double max_value;
  const double step;

  void reset() { value = default_value; }
};

// Write a double to the config file with fixed 6-decimal precision.
// Using a stringstream with fixed+setprecision avoids locale-dependent
// formatting and guarantees round-trip fidelity.
std::string formatConfigFloat(double v) {
  std::ostringstream ss;
  ss << std::fixed << std::setprecision(6) << v;
  return ss.str();
}

namespace Settings {
  SettingsEntry FRAMEBAR = SettingsEntry{
      OptionData{L"Frame Bar:", 2, {L"<  Disabled  >", L"<  Enabled   >"}},
      "framebar", 1};
  SettingsEntry HITBOXES = SettingsEntry{
      OptionData{L"Hitboxes:", 2, {L"<  Disabled  >", L"<  Enabled   >"}},
      "hitboxes", 0};
  SettingsEntry FADE = SettingsEntry{
      OptionData{L"Fade Effect:", 2, {L"<  Disabled  >", L"<  Enabled   >"}},
      "fade", 1};
  SettingsEntry DELIM = SettingsEntry{
      OptionData{L"Delim Segments:", 2, {L"<  Disabled  >", L"<  Enabled   >"}},
      "delim", 0};
  SettingsEntry COLOR_SCHEME = SettingsEntry{
      OptionData{L"Color Scheme:", 4,
                 {L"<    SF6     >", L"<  Classic   >", L"<  Dustloop  >", L"< Colorblind >"}},
      "color_scheme", 1};
  SettingsEntry SHOW_DASH_FRAMES = SettingsEntry{
      OptionData{L"Show Dash:", 2, {L"<  Disabled  >", L"<  Enabled   >"}},
      "show_dash", 0};
  // PAUSE_TYPE: WIP option hidden until feature is ready
  SettingsEntry PAUSE_TYPE = SettingsEntry{
      OptionData{L"Pause Type: ", 1, {L"<   Default   >"}},
      "pause_type", 0};
  SettingsEntry DELAY_AMOUNT = SettingsEntry{
      OptionData{L"Delay Frames: ", 4,
                 {L"<     0ms     >", L"<     20ms    >", L"<     30ms    >", L"<     60ms    >"}},
      "delay_amount", 0};
  SettingsEntry CROSSUP_ENABLED = SettingsEntry{
      OptionData{L"Show Crossup: ", 2, {L"<  Disabled  >", L"<  Enabled   >"}},
      "crossup_enabled", 0};
  SettingsEntry TENSION_ENABLED = SettingsEntry{
      OptionData{L"Tension Overlay:", 2, {L"<  Disabled  >", L"<  Enabled   >"}},
      "tension_enabled", 0};
  SettingsEntry TENSION_GAIN = SettingsEntry{
      OptionData{L"Tension Gain:", 4,
                 {L"<  Disabled  >", L"<     1P     >", L"<     2P     >", L"<  1P & 2P   >"}},
      "tension_gain", 3};
  SettingsEntry TENSION_ADVANTAGE = SettingsEntry{
      OptionData{L"Tension Advantage:", 2, {L"<  Disabled  >", L"<  Enabled   >"}},
      "tension_advantage", 0};
  SettingsEntry TENSION_ADV_CHANGE = SettingsEntry{
      OptionData{L"Tension Adv Change:", 2, {L"<  Disabled  >", L"<  Enabled   >"}},
      "tension_adv_change", 0};
  SettingsEntry TENSION_BURST = SettingsEntry{
      OptionData{L"Burst:", 4,
                 {L"<  Disabled  >", L"<     1P     >", L"<     2P     >", L"<  1P & 2P   >"}},
      "tension_burst", 0};
  SettingsEntry TENSION_PULSE = SettingsEntry{
      OptionData{L"Tension Pulse:", 4,
                 {L"<  Disabled  >", L"<     1P     >", L"<     2P     >", L"<  1P & 2P   >"}},
      "tension_pulse", 0};
  SettingsEntry TENSION_CURRENT = SettingsEntry{
      OptionData{L"Tension Now:", 4,
                 {L"<  Disabled  >", L"<     1P     >", L"<     2P     >", L"<  1P & 2P   >"}},
      "tension_current", 0};
  std::array<SettingsEntry *, 16> settings = {
      &FRAMEBAR, &HITBOXES, &FADE, &DELIM, &COLOR_SCHEME,
      &SHOW_DASH_FRAMES, &PAUSE_TYPE, &DELAY_AMOUNT, &CROSSUP_ENABLED,
      &TENSION_ENABLED, &TENSION_GAIN, &TENSION_ADVANTAGE,
      &TENSION_ADV_CHANGE, &TENSION_BURST, &TENSION_PULSE, &TENSION_CURRENT,
  };

  // Layout float settings
  //
  // id_ names encode coordinate system explicitly:
  //   framebar_center_x_ratio / framebar_center_y_ratio
  //       Screen-space anchor ratios [0.0, 1.0] passed to DrawContext.
  //       0.5 / 0.8 are the original hardcoded defaults.
  //
  //   tension_left_offset
  //       Draw-space horizontal offset from the shared framebar anchor.
  //       Original hardcoded value: -1442.
  //
  //   tension_bottom_gap
  //       Actual draw-space vertical gap between the tension box bottom and
  //       BAR_ONE_TOP. The old visible layout had:
  //           BAR_ONE_TOP = -48
  //           box bottom  = -98
  //       So the original effective gap was 50 draw-space units.
  FloatSetting BAR_CENTER_X_RATIO  = {"framebar_center_x_ratio", 0.5,    0.5,    0.0,    1.0,    0.01};
  FloatSetting BAR_CENTER_Y_RATIO  = {"framebar_center_y_ratio", 0.8,    0.8,    0.0,    1.0,    0.01};
  FloatSetting TENSION_LEFT_OFFSET = {"tension_left_offset",    -1442.0, -1442.0, -2000.0, 2000.0, 10.0};
  FloatSetting TENSION_BOTTOM_GAP  = {"tension_bottom_gap",      50.0,   50.0,    0.0,    500.0,  4.0};
  FloatSetting MENU_CENTER_X_RATIO = {"menu_center_x_ratio",     0.58,   0.58,   0.0,    1.0,    0.01};
  FloatSetting MENU_CENTER_Y_RATIO = {"menu_center_y_ratio",     0.5,    0.5,    0.0,    1.0,    0.01};

  std::array<FloatSetting *, 6> float_settings = {
      &BAR_CENTER_X_RATIO,
      &BAR_CENTER_Y_RATIO,
      &TENSION_LEFT_OFFSET,
      &TENSION_BOTTOM_GAP,
      &MENU_CENTER_X_RATIO,
      &MENU_CENTER_Y_RATIO,
  };

  const std::filesystem::path WORKING_DIRECTORY = UE4SSProgram::get_program().get_working_directory();
  const auto CONFIG_PATH = WORKING_DIRECTORY / "StriveLabs.txt";

  size_t FRAMEBAR_INDEX = 0;
  size_t HITBOX_INDEX = 1;

  int indexById(const std::string &id_) {
    for (int i = 0; i < static_cast<int>(settings.size()); i++) {
      if (id_ == settings[i]->id_) return i;
    }
    return -1;
  }

  int floatIndexById(const std::string &id_) {
    for (int i = 0; i < static_cast<int>(float_settings.size()); i++) {
      if (id_ == float_settings[i]->id_) return i;
    }
    return -1;
  }

  void readConfig() {
    std::ifstream configFile(CONFIG_PATH);
    if (!configFile) return;

    std::string line;
    while (std::getline(configFile, line)) {
      size_t pos = line.find('=');
      if (pos == std::string::npos) continue;

      std::string key = line.substr(0, pos);
      std::string val_str = line.substr(pos + 1);

      {
        int index = indexById(key);
        if (index != -1) {
          try {
            const auto parsed = static_cast<size_t>(std::stoi(val_str));
            settings[index]->value = std::min(parsed, settings[index]->display.count - 1);
          }
          catch (...) {}
          continue;
        }
      }

      {
        int index = floatIndexById(key);
        if (index != -1) {
          try {
            double v = std::stod(val_str);
            auto *f = float_settings[index];
            f->value = std::max(f->min_value, std::min(f->max_value, v));
          } catch (...) {}
          continue;
        }
      }
    }

    configFile.close();
  }

  void saveConfig() {
    std::ofstream configFile(CONFIG_PATH);
    if (!configFile) {
      RC::Output::send<LogLevel::Error>(STR("Unable to save config file\n"));
      return;
    }

    for (auto &entry : settings) {
      configFile << entry->id_ << "=" << entry->value << "\n";
    }
    for (auto &entry : float_settings) {
      configFile << entry->id_ << "=" << formatConfigFloat(entry->value) << "\n";
    }

    configFile.close();
    RC::Output::send<LogLevel::Verbose>(STR("Saved config file\n"));
  }

  void resetLayout() {
    for (auto *f : float_settings) f->reset();
    saveConfig();
  }
}

namespace {
  size_t OPTION_COUNT = Settings::settings.size();

  // Menu position defaults (overridden by MENU_CENTER_X/Y_RATIO float settings)
  constexpr double CENTER_X_RATIO = 0.58;
  constexpr double CENTER_Y_RATIO = 0.5;

  constexpr double PANEL_PADDING_X = 36;
  constexpr double PANEL_PADDING_TOP = 48;
  constexpr double PANEL_PADDING_BOTTOM = 28;
  constexpr double COL_SPACING = 32;
  constexpr double OPTION_HEIGHT = 52;
  constexpr double OPTION_SPACING = 6;
  constexpr double SECTION_GAP = 20;
  constexpr double HEADER_HEIGHT = 38;
  constexpr double TITLE_SCALE = 2.4;
  constexpr double LABEL_SCALE = 1.5;
  constexpr double VALUE_SCALE = 1.5;
  constexpr double HEADER_SCALE = 1.2;
  constexpr double TITLE_BOTTOM_MARGIN = 14;

  FLinearColor PANEL_BG{0.001f, 0.0015f, 0.005f, 0.94f};
  FLinearColor BORDER_COLOR{0.015f, 0.020f, 0.045f, 1.00f};
  FLinearColor BORDER_ACCENT{sRGBToLinear(255), 0.0f, 0.0f, 0.30f};
  FLinearColor CURSOR_BG{sRGBToLinear(255), 0.0f, 0.0f, 0.15f};
  FLinearColor CURSOR_STRIPE{sRGBToLinear(255), sRGBToLinear(60), sRGBToLinear(60), 0.60f};
  FLinearColor SECTION_BG{0.006f, 0.010f, 0.025f, 0.90f};
  FLinearColor ACCENT_COLOR{sRGBToLinear(255), 0.0f, 0.0f, 1.00f};
  FLinearColor DIVIDER_COLOR{1.0f, 1.0f, 1.0f, 0.03f};
  constexpr double ACCENT_W = 3.0;
  constexpr double STRIPE_H = 2.0;

  struct SectionDef { const wchar_t* label; size_t start; size_t end; };
  constexpr SectionDef SECTIONS[] = {
    {L"-- DISPLAY --",        0,  6},
    {L"-- TIMING & INPUT --", 6,  9},
    {L"-- TENSION --",        9, 16},
  };
  constexpr size_t SECTION_COUNT = sizeof(SECTIONS) / sizeof(SECTIONS[0]);

  enum class MenuPage : size_t {
    Root = 0,
    Display,
    TimingInput,
    Tension,
    Combos,
    Layout,
  };

  struct RootPageDef {
    MenuPage page;
    const wchar_t* label;
  };

  constexpr RootPageDef ROOT_PAGES[] = {
    {MenuPage::Display, L"Display"},
    {MenuPage::TimingInput, L"Timing & Input"},
    {MenuPage::Tension, L"Tension"},
    {MenuPage::Combos, L"Combos"},
    {MenuPage::Layout, L"Layout"},
  };
  constexpr size_t ROOT_PAGE_COUNT = sizeof(ROOT_PAGES) / sizeof(ROOT_PAGES[0]);

  struct LayoutRowDef { const wchar_t* label; size_t float_idx; };
  constexpr LayoutRowDef LAYOUT_ROWS[] = {
    {L"Bar Anchor X (ratio)",     0},
    {L"Bar Anchor Y (ratio)",     1},
    {L"Tension Left (offset)",    2},
    {L"Tension Gap (above bar)",  3},
    {L"Menu Position X (ratio)",  4},
    {L"Menu Position Y (ratio)",  5},
  };
  constexpr size_t LAYOUT_ROW_COUNT = sizeof(LAYOUT_ROWS) / sizeof(LAYOUT_ROWS[0]);

  int buildTitleWidth() {
    int w = 0;
    for (const auto &e : Settings::settings)
      w = std::max<int>(w, static_cast<int>(std::wstring_view(e->display.title).size()));
    for (const auto &r : LAYOUT_ROWS)
      w = std::max<int>(w, static_cast<int>(std::wstring_view(r.label).size()));
    // Combo menu rows are dynamic, but we ensure minimum width
    w = std::max<int>(w, 40);
    return w;
  }

  int buildValueWidth() {
    int w = 0;
    for (const auto &e : Settings::settings)
      for (size_t i = 0; i < e->display.count; i++)
        w = std::max<int>(w, static_cast<int>(std::wstring_view(e->display.values[i]).size()));
    w = std::max<int>(w, static_cast<int>(std::wstring_view(L"<< open >>").size()));
    w = std::max<int>(w, static_cast<int>(std::wstring_view(L"<< back >>").size()));
    w = std::max<int>(w, static_cast<int>(std::wstring_view(L"<< disabled >>").size()));
    w = std::max<int>(w, static_cast<int>(std::wstring_view(L"<< press >>").size()));
    w = std::max<int>(w, static_cast<int>(std::wstring_view(L"<< RECORDING >>").size()));
    return w;
  }

  constexpr double CHAR_PX = 20.0;

  double TITLE_WIDTH = buildTitleWidth() * CHAR_PX * (LABEL_SCALE / 2.0);
  double VALUE_WIDTH = std::max(buildValueWidth() * CHAR_PX * (VALUE_SCALE / 2.0), 430.0);
  double MENU_WIDTH = TITLE_WIDTH + COL_SPACING + VALUE_WIDTH + 2 * PANEL_PADDING_X;

  std::wstring formatMenuFloat(double v) {
    wchar_t buf[32]{};
    swprintf_s(buf, L"%.4f", v);
    return buf;
  }

  static const Palette color_palettes[] = {
      Palette{// SF6
              convSRGB(201, 128, 0, 255), convSRGB(0, 0, 0, 255),
              {convSRGB(26, 26, 26, 255), convSRGB(253, 177, 46, 255),
               convSRGB(253, 245, 46, 255), convSRGB(1, 182, 149, 255),
               convSRGB(205, 43, 103, 255), convSRGB(1, 111, 188, 255),
               convSRGB(1, 111, 188, 255), convSRGB(26, 26, 26, 255)}},
      Palette{// CLASSIC
              FLinearColor{.8f, .1f, .1f, 1.f},
              FLinearColor{0.05f, 0.05f, 0.05f, 0.7f},
              {FLinearColor{.2f, .2f, .2f, .9f}, FLinearColor{.1f, .1f, .8f, .9f},
               FLinearColor{.1f, .6f, .1f, .9f}, FLinearColor{.7f, .7f, .1f, .9f},
               FLinearColor{.8f, .1f, .1f, .9f}, FLinearColor{.8f, .4f, .1f, .9f},
               FLinearColor{.8f, .4f, .1f, .9f}, FLinearColor{.2f, .2f, .2f, .9f}}},
      Palette{// DUSTLOOP
              convSRGB(255, 0, 0, 255), convSRGB(23, 28, 38, 255),
              {convSRGB(128, 128, 128, 255), convSRGB(233, 172, 4, 255),
               convSRGB(233, 215, 4, 255), convSRGB(54, 179, 126, 255),
               convSRGB(255, 93, 93, 255), convSRGB(0, 105, 182, 255),
               convSRGB(0, 105, 182, 255), convSRGB(128, 128, 128, 255)}},
      Palette{// COLORBLIND
              convSRGB(0, 114, 178, 255), convSRGB(23, 28, 38, 255),
              {convSRGB(100, 100, 100, 255), convSRGB(230, 159, 0, 255),
               convSRGB(240, 228, 66, 255), convSRGB(0, 158, 115, 255),
               convSRGB(0, 114, 178, 255), convSRGB(86, 180, 233, 255),
               convSRGB(86, 180, 233, 255), convSRGB(100, 100, 100, 255)}},
  };

  const wchar_t* pageLabel(MenuPage page) {
    switch (page) {
      case MenuPage::Root: return L"-- MENU --";
      case MenuPage::Display: return L"-- DISPLAY --";
      case MenuPage::TimingInput: return L"-- TIMING & INPUT --";
      case MenuPage::Tension: return L"-- TENSION --";
      case MenuPage::Combos: return L"-- COMBOS --";
      case MenuPage::Layout: return L"-- LAYOUT --";
    }
    return L"-- MENU --";
  }

  size_t settingsStartForPage(MenuPage page) {
    switch (page) {
      case MenuPage::Display: return SECTIONS[0].start;
      case MenuPage::TimingInput: return SECTIONS[1].start;
      case MenuPage::Tension: return SECTIONS[2].start;
      default: return 0;
    }
  }

  size_t settingsCountForPage(MenuPage page) {
    switch (page) {
      case MenuPage::Display: return SECTIONS[0].end - SECTIONS[0].start;
      case MenuPage::TimingInput: return SECTIONS[1].end - SECTIONS[1].start;
      case MenuPage::Tension: return SECTIONS[2].end - SECTIONS[2].start;
      default: return 0;
    }
  }

  bool isSettingsPage(MenuPage page) {
    return page == MenuPage::Display || page == MenuPage::TimingInput ||
           page == MenuPage::Tension;
  }

  size_t rowCountForPage(MenuPage page) {
    switch (page) {
      case MenuPage::Root: return ROOT_PAGE_COUNT;
      case MenuPage::Display:
      case MenuPage::TimingInput:
      case MenuPage::Tension:
        return settingsCountForPage(page) + 1;
      case MenuPage::Combos: return 1 + ModMenu::instance().comboMenu().rowCount();
      case MenuPage::Layout: return LAYOUT_ROW_COUNT + 2;
    }
    return ROOT_PAGE_COUNT;
  }

  size_t rootIndexForPage(MenuPage page) {
    for (size_t i = 0; i < ROOT_PAGE_COUNT; i++) {
      if (ROOT_PAGES[i].page == page) return i;
    }
    return 0;
  }

  double computePageHeight(MenuPage page) {
    double base = PANEL_PADDING_TOP + PANEL_PADDING_BOTTOM + OPTION_HEIGHT + TITLE_BOTTOM_MARGIN +
                  HEADER_HEIGHT + OPTION_SPACING;
    if (page == MenuPage::Combos) {
      // Back row + combo menu content (variable height)
      double fixed_rows = 1 * (OPTION_HEIGHT + OPTION_SPACING);
      double combo_content = ModMenu::instance().comboMenu().contentHeight();
      return base + fixed_rows + combo_content - OPTION_SPACING;
    }
    const size_t row_count = rowCountForPage(page);
    return base + row_count * (OPTION_HEIGHT + OPTION_SPACING) - OPTION_SPACING;
  }
}

size_t rotateVal(size_t val, bool positive, size_t max) {
  if (positive)      return (val + 1) % max;
  else if (val == 0) return max - 1;
  else               return val - 1;
}

void ModMenu::changeSetting(size_t idx, bool right) {
  auto &setting = Settings::settings[idx];
  setting->value = rotateVal(setting->value, right, setting->display.count);
  Settings::saveConfig();
}

void changeLayoutFloat(size_t layout_idx, bool right) {
  auto &row = LAYOUT_ROWS[layout_idx];
  auto *f = Settings::float_settings[row.float_idx];
  f->value += right ? f->step : -f->step;
  f->value = std::max(f->min_value, std::min(f->max_value, f->value));
  Settings::saveConfig();
}

ModMenu &ModMenu::instance() {
  static ModMenu me;
  return me;
}

ModMenu::~ModMenu() = default;
ModMenu::ModMenu()
: tool(CENTER_X_RATIO, CENTER_Y_RATIO) {
  Settings::readConfig();
}

void ModMenu::update(PressedKeys data) {
  if (data.toggle_framebar) changeSetting(Settings::FRAMEBAR_INDEX);
  if (data.toggle_hitbox)   changeSetting(Settings::HITBOX_INDEX);
  if (data.toggle_menu) {
    is_showing = !is_showing;
    if (is_showing) {
      current_page = static_cast<size_t>(MenuPage::Root);
      cursor_position = 0;
    }
  }

  if (!is_showing) return;

  const auto page = static_cast<MenuPage>(current_page);

  // On the Combos page, when cursor is in the combo menu area, let the combo
  // menu handle all navigation and keep the main cursor pinned.
  if (page == MenuPage::Combos) {
    const size_t cp = static_cast<size_t>(cursor_position);

    if (cp == 0) {
      // Back row
      if (data.go_up) cursor_position = 1; // wrap to combo menu
      if (data.go_down) cursor_position = 1;
    } else {
      // In combo menu zone — don't move main cursor, delegate to combo menu
      if (data.go_up && combo_menu_.rowCount() == 0) {
        cursor_position = 0;
      }
      // combo_menu_.update handles up/down/left/right later
    }
  } else {
    const size_t row_count = rowCountForPage(page);
    if (data.go_up)   cursor_position = static_cast<int>(rotateVal(static_cast<size_t>(cursor_position), false, row_count));
    if (data.go_down) cursor_position = static_cast<int>(rotateVal(static_cast<size_t>(cursor_position), true,  row_count));
  }

  const size_t cp = static_cast<size_t>(cursor_position);
  if (page == MenuPage::Root) {
    if ((data.rotate_left || data.rotate_right) && cp < ROOT_PAGE_COUNT) {
      current_page = static_cast<size_t>(ROOT_PAGES[cp].page);
      cursor_position = 0;
    }
    return;
  }

  if (cp == 0) {
    if (data.rotate_left || data.rotate_right) {
      current_page = static_cast<size_t>(MenuPage::Root);
      cursor_position = static_cast<int>(rootIndexForPage(page));
    }
    return;
  }

  const size_t local_idx = cp - 1;
  if (isSettingsPage(page)) {
    const size_t settings_idx = settingsStartForPage(page) + local_idx;
    if (data.rotate_left)  changeSetting(settings_idx, false);
    if (data.rotate_right) changeSetting(settings_idx, true);
    return;
  }

  if (page == MenuPage::Combos) {
    auto& vp = VideoPlayer::instance();
    auto vs = vp.state();
    bool videoActive = vp.isActive() || vs == VideoState::Finished;

    // Intercept left to stop video before navigating back
    bool leftForMenu = data.rotate_left;
    if (data.rotate_left && videoActive &&
        combo_menu_.currentPage() == ComboMenu::Page::ComboList) {
      vp.stop();
      leftForMenu = false; // absorb the press
    }

    // Suppress right from combo menu when video is playing/paused
    // (right arrow toggles fullscreen/windowed via onPresent key handler)
    bool rightForMenu = data.rotate_right;
    if (data.rotate_right && (vs == VideoState::Playing || vs == VideoState::Paused) &&
        combo_menu_.currentPage() == ComboMenu::Page::ComboList) {
      rightForMenu = false; // absorbed by video player
    }

    bool wants_back = combo_menu_.update(data.go_up, data.go_down, leftForMenu, rightForMenu);
    if (wants_back) {
      // Combo menu at top level pressed left — go back to main menu
      cursor_position = 0;
    }

    // Handle video request: start new video (only fires when no video is active)
    if (combo_menu_.consumeVideoRequest()) {
      auto link = combo_menu_.selectedVideoLink();
      if (!link.empty()) {
        vp.startVideo(link);
      }
    }
    return;
  }

  if (page == MenuPage::Layout) {
    if (local_idx < LAYOUT_ROW_COUNT) {
      if (data.rotate_left)  changeLayoutFloat(local_idx, false);
      if (data.rotate_right) changeLayoutFloat(local_idx, true);
    } else if (data.rotate_left || data.rotate_right) {
      Settings::resetLayout();
    }
  }
}

void ModMenu::draw() {
  if (!is_showing) return;

  // Apply configurable menu position
  tool.setRatios(Settings::MENU_CENTER_X_RATIO.value, Settings::MENU_CENTER_Y_RATIO.value);
  tool.update();

  const auto page = static_cast<MenuPage>(current_page);
  const double menu_height = computePageHeight(page);
  const double menu_top = -menu_height / 2.0;
  const double menu_left = -MENU_WIDTH / 2.0;
  const double label_left = menu_left + PANEL_PADDING_X;
  const double value_left = label_left + TITLE_WIDTH + COL_SPACING;

  constexpr double BORDER_INSET = 3.0;
  // Outer border
  tool.drawRect(menu_left - BORDER_INSET, menu_top - BORDER_INSET,
                MENU_WIDTH + BORDER_INSET * 2, menu_height + BORDER_INSET * 2, BORDER_COLOR);
  // Accent border (top + left)
  tool.drawRect(menu_left - BORDER_INSET, menu_top - BORDER_INSET,
                MENU_WIDTH + BORDER_INSET * 2, BORDER_INSET, BORDER_ACCENT);
  tool.drawRect(menu_left - BORDER_INSET, menu_top - BORDER_INSET,
                BORDER_INSET, menu_height + BORDER_INSET * 2, BORDER_ACCENT);
  // Panel background
  tool.drawRect(menu_left, menu_top, MENU_WIDTH, menu_height, PANEL_BG);
  // Top accent line
  tool.drawRect(menu_left, menu_top, MENU_WIDTH, 1.0, BORDER_ACCENT);

  double cy = menu_top + PANEL_PADDING_TOP;
  // Accent mark next to title
  tool.drawRect(label_left - 8.0, cy + 4.0, ACCENT_W, OPTION_HEIGHT - 16.0, ACCENT_COLOR);
  tool.drawOutlinedText(label_left, cy, FString(L"STRIVE LABS"), TITLE_SCALE);
  cy += OPTION_HEIGHT + TITLE_BOTTOM_MARGIN;
  // Divider under title
  tool.drawRect(menu_left, cy - TITLE_BOTTOM_MARGIN / 2.0, MENU_WIDTH, 1.0, DIVIDER_COLOR);

  // Section header
  tool.drawRect(menu_left, cy, MENU_WIDTH, HEADER_HEIGHT, SECTION_BG);
  tool.drawRect(menu_left, cy, ACCENT_W, HEADER_HEIGHT, ACCENT_COLOR);
  tool.drawRect(menu_left, cy + HEADER_HEIGHT - 1.0, MENU_WIDTH, 1.0, DIVIDER_COLOR);
  tool.drawOutlinedText(label_left + 8.0, cy + 4.0, FString(pageLabel(page)), HEADER_SCALE);
  cy += HEADER_HEIGHT + OPTION_SPACING;

  if (page == MenuPage::Root) {
    for (size_t i = 0; i < ROOT_PAGE_COUNT; i++) {
      const bool sel = (static_cast<size_t>(cursor_position) == i);
      if (sel) {
        tool.drawRect(menu_left, cy, MENU_WIDTH, OPTION_HEIGHT, CURSOR_BG);
        tool.drawRect(menu_left, cy, MENU_WIDTH, STRIPE_H, CURSOR_STRIPE);
        tool.drawRect(menu_left, cy, ACCENT_W, OPTION_HEIGHT, ACCENT_COLOR);
      }
      tool.drawOutlinedText(label_left, cy + 8.0, FString(ROOT_PAGES[i].label), LABEL_SCALE);
      tool.drawOutlinedText(value_left, cy + 8.0, FString(L"<< open >>"), VALUE_SCALE);
      cy += OPTION_HEIGHT + OPTION_SPACING;
    }
    return;
  }

  const bool back_sel = (cursor_position == 0);
  if (back_sel) {
    tool.drawRect(menu_left, cy, MENU_WIDTH, OPTION_HEIGHT, CURSOR_BG);
    tool.drawRect(menu_left, cy, MENU_WIDTH, STRIPE_H, CURSOR_STRIPE);
    tool.drawRect(menu_left, cy, ACCENT_W, OPTION_HEIGHT, ACCENT_COLOR);
  }
  tool.drawOutlinedText(label_left, cy + 8.0, FString(L"Back"), LABEL_SCALE);
  tool.drawOutlinedText(value_left, cy + 8.0, FString(L"<< back >>"), VALUE_SCALE);
  cy += OPTION_HEIGHT + OPTION_SPACING;

  if (isSettingsPage(page)) {
    const size_t start = settingsStartForPage(page);
    const size_t count = settingsCountForPage(page);
    for (size_t i = 0; i < count; i++) {
      const size_t settings_idx = start + i;
      const bool sel = (static_cast<size_t>(cursor_position) == i + 1);
      if (sel) {
        tool.drawRect(menu_left, cy, MENU_WIDTH, OPTION_HEIGHT, CURSOR_BG);
        tool.drawRect(menu_left, cy, MENU_WIDTH, STRIPE_H, CURSOR_STRIPE);
        tool.drawRect(menu_left, cy, ACCENT_W, OPTION_HEIGHT, ACCENT_COLOR);
      }
      auto &relevant = Settings::settings[settings_idx]->display;
      tool.drawOutlinedText(label_left, cy + 8.0, FString(relevant.title), LABEL_SCALE);
      tool.drawOutlinedText(value_left, cy + 8.0,
                            FString(relevant.values[Settings::settings[settings_idx]->value]), VALUE_SCALE);
      cy += OPTION_HEIGHT + OPTION_SPACING;
    }
    return;
  }

  if (page == MenuPage::Combos) {
    // Delegate combo menu drawing
    const size_t first_custom_row = 1;
    combo_menu_.draw(tool, menu_left, MENU_WIDTH, label_left, value_left, cy,
                     cursor_position, first_custom_row);

    // Show video player status and controls hint
    {
      auto& vp = VideoPlayer::instance();
      auto vs = vp.state();
      if (vs != VideoState::Idle) {
        auto status = vp.statusText();
        std::wstring hint;
        if (vs == VideoState::Playing)
          hint = L"  [Space: Pause | Right: Window | ESC: Stop]";
        else if (vs == VideoState::Paused)
          hint = L"  [Space: Resume | Right: Window | ESC: Stop]";
        else if (vs == VideoState::Finished)
          hint = L"  [Space: Replay | ESC: Dismiss]";
        else if (vs == VideoState::Downloading)
          hint = L"  [ESC: Cancel]";

        auto full = status + hint;
        if (!full.empty()) {
          tool.drawOutlinedText(label_left, cy + 4.0, FString(full.c_str()), 0.9);
          cy += 36.0 + 6.0;
        }
      }
    }
    return;
  }

  if (page == MenuPage::Layout) {
    for (size_t i = 0; i < LAYOUT_ROW_COUNT; i++) {
      bool sel = (static_cast<size_t>(cursor_position) == i + 1);
      if (sel) {
        tool.drawRect(menu_left, cy, MENU_WIDTH, OPTION_HEIGHT, CURSOR_BG);
        tool.drawRect(menu_left, cy, MENU_WIDTH, STRIPE_H, CURSOR_STRIPE);
        tool.drawRect(menu_left, cy, ACCENT_W, OPTION_HEIGHT, ACCENT_COLOR);
      }
      auto &row = LAYOUT_ROWS[i];
      auto val_str = formatMenuFloat(Settings::float_settings[row.float_idx]->value);
      tool.drawOutlinedText(label_left, cy + 8.0, FString(row.label), LABEL_SCALE);
      tool.drawOutlinedText(value_left, cy + 8.0, FString(val_str.c_str()), VALUE_SCALE);
      cy += OPTION_HEIGHT + OPTION_SPACING;
    }

    bool sel_reset = (static_cast<size_t>(cursor_position) == LAYOUT_ROW_COUNT + 1);
    if (sel_reset) {
      tool.drawRect(menu_left, cy, MENU_WIDTH, OPTION_HEIGHT, CURSOR_BG);
      tool.drawRect(menu_left, cy, MENU_WIDTH, STRIPE_H, CURSOR_STRIPE);
      tool.drawRect(menu_left, cy, ACCENT_W, OPTION_HEIGHT, ACCENT_COLOR);
    }
    tool.drawOutlinedText(label_left, cy + 8.0, FString(L"Reset Layout to Defaults"), LABEL_SCALE);
    tool.drawOutlinedText(value_left, cy + 8.0, FString(L"<< press >>"), VALUE_SCALE);
  }
}

bool ModMenu::barEnabled() const { return Settings::FRAMEBAR.value; }
bool ModMenu::hitboxEnabled() const { return Settings::HITBOXES.value; }
bool ModMenu::fadeEnabled() const { return Settings::FADE.value; }
bool ModMenu::delimEnabled() const { return Settings::DELIM.value; }
bool ModMenu::cancelEnabled() const { return false; }
bool ModMenu::dashEnabled() const { return Settings::SHOW_DASH_FRAMES.value; }
bool ModMenu::crossupEnabled() const { return Settings::CROSSUP_ENABLED.value; }
int ModMenu::pauseType() const { return Settings::PAUSE_TYPE.value; }

TensionOptions ModMenu::getTensionOptions() const {
  return TensionOptions{
      static_cast<bool>(Settings::TENSION_ENABLED.value),
      static_cast<PlayerDisplayMode>(Settings::TENSION_GAIN.value),
      static_cast<bool>(Settings::TENSION_ADVANTAGE.value),
      static_cast<bool>(Settings::TENSION_ADV_CHANGE.value),
      static_cast<PlayerDisplayMode>(Settings::TENSION_BURST.value),
      static_cast<PlayerDisplayMode>(Settings::TENSION_PULSE.value),
      static_cast<PlayerDisplayMode>(Settings::TENSION_CURRENT.value),
  };
}

LayoutPositions ModMenu::getLayoutPositions() const {
  return LayoutPositions{
      Settings::BAR_CENTER_X_RATIO.value,
      Settings::BAR_CENTER_Y_RATIO.value,
      static_cast<int>(Settings::TENSION_LEFT_OFFSET.value),
      static_cast<int>(Settings::TENSION_BOTTOM_GAP.value),
  };
}

constexpr int delayAmounts[4] = {0, 20, 30, 60};
int ModMenu::delayAmount() const { return delayAmounts[Settings::DELAY_AMOUNT.value]; }

CurrentOptions ModMenu::getScheme() const {
  return CurrentOptions{color_palettes[Settings::COLOR_SCHEME.value],
                        fadeEnabled(), delimEnabled(), cancelEnabled(), crossupEnabled()};
}

void ModMenu::initComboMenu(const std::filesystem::path& data_dir) {
  combo_menu_.init(data_dir);
}

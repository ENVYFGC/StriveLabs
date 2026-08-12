#include "menu.h"
#include "recordings.h"
#include "combo_log.h"

#include <vector>
#include <string>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <ctime>
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
  SettingsEntry TENSION_WALL_DAMAGE = SettingsEntry{
      OptionData{L"Wall Damage:", 4,
                 {L"<  Disabled  >", L"<     1P     >", L"<     2P     >", L"<  1P & 2P   >"}},
      "tension_wall_damage", 3};
  std::array<SettingsEntry *, 17> settings = {
      &FRAMEBAR, &HITBOXES, &FADE, &DELIM, &COLOR_SCHEME,
      &SHOW_DASH_FRAMES, &PAUSE_TYPE, &DELAY_AMOUNT, &CROSSUP_ENABLED,
      &TENSION_ENABLED, &TENSION_GAIN, &TENSION_ADVANTAGE,
      &TENSION_ADV_CHANGE, &TENSION_BURST, &TENSION_PULSE, &TENSION_CURRENT,
      &TENSION_WALL_DAMAGE,
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
  // Detail overlay (combo info panel) position in draw-space units.
  //   detail_margin_t: offset from screenTopEdge()  → 118 ≈ Y 68px at 1080p
  //   detail_margin_r: inset from screenRightEdge() → 200 ≈ 115px from right at 1080p
  FloatSetting DETAIL_MARGIN_T     = {"detail_margin_t",         118.0,  118.0,  0.0,  937.0,   5.0};
  FloatSetting DETAIL_MARGIN_R     = {"detail_margin_r",         200.0,  200.0,  0.0, 1667.0,  10.0};
  FloatSetting DETAIL_PANEL_WIDTH  = {"detail_panel_width",      903.0,  903.0, 300.0, 1600.0, 10.0};
  // Manual widening on top of the auto-computed menu width.
  FloatSetting MENU_WIDTH_EXTRA    = {"menu_width_extra",          0.0,    0.0,   0.0,  900.0,  20.0};

  std::array<FloatSetting *, 10> float_settings = {
      &BAR_CENTER_X_RATIO,    // 0
      &BAR_CENTER_Y_RATIO,    // 1
      &TENSION_LEFT_OFFSET,   // 2
      &TENSION_BOTTOM_GAP,    // 3
      &MENU_CENTER_X_RATIO,   // 4
      &MENU_CENTER_Y_RATIO,   // 5
      &DETAIL_MARGIN_T,       // 6
      &DETAIL_MARGIN_R,       // 7
      &DETAIL_PANEL_WIDTH,    // 8
      &MENU_WIDTH_EXTRA,      // 9
  };

  const std::filesystem::path WORKING_DIRECTORY = UE4SSProgram::get_program().get_working_directory();
  const auto CONFIG_PATH = WORKING_DIRECTORY / "StriveLabs.txt";

  // Recording folder, created on first use so it is discoverable.
  std::filesystem::path recordingsDir() {
    auto dir = WORKING_DIRECTORY / "recordings";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
  }

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
    {L"-- TENSION --",        9, 17},
  };
  constexpr size_t SECTION_COUNT = sizeof(SECTIONS) / sizeof(SECTIONS[0]);

  enum class MenuPage : size_t {
    Root = 0,
    Display,
    TimingInput,
    Tension,
    Layout,
    LayoutBar,
    LayoutTension,
    LayoutDetail,
    LayoutMenu,
    Recordings,
  };

  // ---- Recordings page state ----------------------------------------------
  // Rows: 0 Back | 1 File | 2 Slot | 3 Side | 4 Load
  constexpr size_t RECORDINGS_ROW_COUNT = 5;
  // Rows: 0 Slot | 1 Save | 2 File | 3 Side | 4 Load

  std::vector<std::filesystem::path> g_rec_files;
  size_t       g_rec_file_idx = 0;
  int          g_rec_slot     = 0;               // 0-based; shown as 1-5
  int32_t      g_rec_side     = recordings::kDirLeft;   // dummy is P2 by default
  std::wstring g_rec_status   = L"";

  // buildValueWidth() only measures static strings, so long filenames overrun.
  // Elide the front: the tail identifies the recording.
  std::wstring fitValue(const std::wstring& in, size_t max_chars = 26) {
    if (in.size() <= max_chars) return in;
    return L"…" + in.substr(in.size() - (max_chars - 1));
  }

  void refreshRecordingFiles() {
    g_rec_files = recordings::scanDir(Settings::recordingsDir());
    if (g_rec_file_idx >= g_rec_files.size()) g_rec_file_idx = 0;
  }

  // Slot -> .txt. Independent of GGST's save system, which only flushes on exit.
  void saveSelectedSlot() {
    // Self-describing name: frames first, then the opening inputs, so the
    // folder is readable without opening files. Falls back to the slot number
    // if the slot is empty or unreadable.
    // Timestamp first so the folder sorts chronologically.
    char stamp[32] = "";
    {
      const std::time_t t = std::time(nullptr);
      std::tm tm{};
      if (localtime_s(&tm, &t) == 0)
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H%M%S", &tm);
    }
    std::string desc = recordings::suggestName(g_rec_slot);
    if (desc.empty()) desc = "slot" + std::to_string(g_rec_slot + 1);
    std::string stem = std::string(stamp[0] ? stamp : "recording") + "_" + desc;

    auto dir = Settings::recordingsDir();
    auto path = dir / (stem + ".txt");
    for (int n = 2; std::filesystem::exists(path) && n < 100; n++)
      path = dir / (stem + "_" + std::to_string(n) + ".txt");
    const std::wstring name = path.filename().wstring();
    std::wstring err;
    if (!recordings::exportSlot(g_rec_slot, path, err)) { g_rec_status = err; return; }
    g_rec_status = L"saved " + name;
    refreshRecordingFiles();
  }

  void loadSelectedRecording() {
    char dbg[512];
    if (g_rec_files.empty()) {
      COMBO_LOG("LOAD: no .txt files");
      g_rec_status = L"no .txt files found"; return;
    }
    const auto& path = g_rec_files[g_rec_file_idx];
    snprintf(dbg, sizeof(dbg), "LOAD: file=%s slot=%d side=%d slots=%llx",
             path.filename().string().c_str(), g_rec_slot + 1, (int)g_rec_side,
             (unsigned long long)(uintptr_t)recordings::findSlots());
    COMBO_LOG(dbg);

    auto parsed = recordings::parseFile(path, g_rec_side);
    if (!parsed.ok()) {
      snprintf(dbg, sizeof(dbg), "LOAD: parse failed");
      COMBO_LOG(dbg);
      g_rec_status = parsed.error; return;
    }
    snprintf(dbg, sizeof(dbg), "LOAD: parsed %d frames, first=%04X %04X %04X",
             (int)parsed.frames.size(),
             parsed.frames.size() > 0 ? parsed.frames[0] : 0,
             parsed.frames.size() > 1 ? parsed.frames[1] : 0,
             parsed.frames.size() > 2 ? parsed.frames[2] : 0);
    COMBO_LOG(dbg);

    std::wstring err;
    if (!recordings::writeSlot(g_rec_slot, parsed.frames, g_rec_side,
                               recordings::kKeepCharaId, err)) {
      COMBO_LOG("LOAD: writeSlot returned false");
      g_rec_status = err;
      return;
    }
    g_rec_status = L"loaded " + std::to_wstring(parsed.frames.size()) +
                   L"f into slot " + std::to_wstring(g_rec_slot + 1);
  }

  struct RootPageDef {
    MenuPage page;
    const wchar_t* label;
  };

  constexpr RootPageDef ROOT_PAGES[] = {
    {MenuPage::Display, L"Display"},
    {MenuPage::TimingInput, L"Timing & Input"},
    {MenuPage::Tension, L"Tension"},
    {MenuPage::Layout, L"Layout"},
    {MenuPage::Recordings, L"Recordings"},
  };
  constexpr size_t ROOT_PAGE_COUNT = sizeof(ROOT_PAGES) / sizeof(ROOT_PAGES[0]);

  struct LayoutRowDef { const wchar_t* label; size_t float_idx; };

  constexpr LayoutRowDef LAYOUT_BAR_ROWS[] = {
    {L"X Position (screen ratio)",  0},
    {L"Y Position (screen ratio)",  1},
  };
  constexpr size_t LAYOUT_BAR_ROW_COUNT = 2;

  constexpr LayoutRowDef LAYOUT_TENSION_ROWS[] = {
    {L"X Offset (draw units)",  2},
    {L"Y Gap above bar",        3},
  };
  constexpr size_t LAYOUT_TENSION_ROW_COUNT = 2;

  constexpr LayoutRowDef LAYOUT_DETAIL_ROWS[] = {
    {L"Y from top edge",   6},
    {L"X from right edge", 7},
    {L"Width",             8},
  };
  constexpr size_t LAYOUT_DETAIL_ROW_COUNT = 3;

  constexpr LayoutRowDef LAYOUT_MENU_ROWS[] = {
    {L"X Position (screen ratio)", 4},
    {L"Y Position (screen ratio)", 5},
    {L"Extra Width",               9},
  };
  constexpr size_t LAYOUT_MENU_ROW_COUNT = 3;

  struct LayoutSubDef { MenuPage page; const wchar_t* label; };
  constexpr LayoutSubDef LAYOUT_SUBS[] = {
    {MenuPage::LayoutBar,      L"Frame Bar"},
    {MenuPage::LayoutTension,  L"Tension Box"},
    {MenuPage::LayoutDetail,   L"Detail Panel"},
    {MenuPage::LayoutMenu,     L"Mod Menu"},
  };
  constexpr size_t LAYOUT_SUB_COUNT = 4;

  bool isLayoutSubPage(MenuPage page) {
    return page == MenuPage::LayoutBar || page == MenuPage::LayoutTension ||
           page == MenuPage::LayoutDetail || page == MenuPage::LayoutMenu;
  }

  MenuPage parentPage(MenuPage page) {
    return isLayoutSubPage(page) ? MenuPage::Layout : MenuPage::Root;
  }

  const LayoutRowDef* layoutRowsForPage(MenuPage page, size_t& count) {
    switch (page) {
      case MenuPage::LayoutBar:     count = LAYOUT_BAR_ROW_COUNT;     return LAYOUT_BAR_ROWS;
      case MenuPage::LayoutTension: count = LAYOUT_TENSION_ROW_COUNT; return LAYOUT_TENSION_ROWS;
      case MenuPage::LayoutDetail:  count = LAYOUT_DETAIL_ROW_COUNT;  return LAYOUT_DETAIL_ROWS;
      case MenuPage::LayoutMenu:    count = LAYOUT_MENU_ROW_COUNT;    return LAYOUT_MENU_ROWS;
      default: count = 0; return nullptr;
    }
  }

  size_t layoutSubIndexForPage(MenuPage page) {
    for (size_t i = 0; i < LAYOUT_SUB_COUNT; i++)
      if (LAYOUT_SUBS[i].page == page) return i + 1;
    return 0;
  }

  int buildTitleWidth() {
    int w = 0;
    for (const auto &e : Settings::settings)
      w = std::max<int>(w, static_cast<int>(std::wstring_view(e->display.title).size()));
    for (const auto &r : LAYOUT_BAR_ROWS)
      w = std::max<int>(w, static_cast<int>(std::wstring_view(r.label).size()));
    for (const auto &r : LAYOUT_TENSION_ROWS)
      w = std::max<int>(w, static_cast<int>(std::wstring_view(r.label).size()));
    for (const auto &r : LAYOUT_DETAIL_ROWS)
      w = std::max<int>(w, static_cast<int>(std::wstring_view(r.label).size()));
    for (const auto &r : LAYOUT_MENU_ROWS)
      w = std::max<int>(w, static_cast<int>(std::wstring_view(r.label).size()));
    for (const auto &s : LAYOUT_SUBS)
      w = std::max<int>(w, static_cast<int>(std::wstring_view(s.label).size()));
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
  double MENU_WIDTH_BASE = std::max(TITLE_WIDTH + COL_SPACING + VALUE_WIDTH + 2 * PANEL_PADDING_X, 900.0);

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
      case MenuPage::Root:          return L"-- MENU --";
      case MenuPage::Display:       return L"-- DISPLAY --";
      case MenuPage::TimingInput:   return L"-- TIMING & INPUT --";
      case MenuPage::Tension:       return L"-- TENSION --";
      case MenuPage::Layout:        return L"-- LAYOUT --";
      case MenuPage::LayoutBar:     return L"-- FRAME BAR --";
      case MenuPage::LayoutTension: return L"-- TENSION BOX --";
      case MenuPage::LayoutDetail:  return L"-- DETAIL PANEL --";
      case MenuPage::LayoutMenu:    return L"-- MOD MENU --";
      case MenuPage::Recordings:    return L"-- RECORDINGS --";
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
      // Layout root: Back + 4 subs + Reset = 6
      case MenuPage::Layout:        return LAYOUT_SUB_COUNT + 2;
      case MenuPage::LayoutBar:     return LAYOUT_BAR_ROW_COUNT + 1;
      case MenuPage::LayoutTension: return LAYOUT_TENSION_ROW_COUNT + 1;
      case MenuPage::LayoutDetail:  return LAYOUT_DETAIL_ROW_COUNT + 1;
      case MenuPage::LayoutMenu:    return LAYOUT_MENU_ROW_COUNT + 1;
      case MenuPage::Recordings:    return RECORDINGS_ROW_COUNT + 1;
    }
    return ROOT_PAGE_COUNT;
  }

  size_t rootIndexForPage(MenuPage page) {
    for (size_t i = 0; i < ROOT_PAGE_COUNT; i++) {
      if (ROOT_PAGES[i].page == page) return i;
    }
    return 0;
  }

  constexpr double DESC_LINE_H  = 26.0;
  constexpr double DESC_SCALE   = 1.05;
  constexpr double DESC_LINES   = 2.0; // description line + disclaimer line
  constexpr double DESC_BLOCK_H = DESC_LINES * DESC_LINE_H + 10.0; // +padding below

  constexpr double CREDIT_LINE_H = 28.0; // extra height reserved for the WistfulHopes credit footer

  double computePageHeight(MenuPage page) {
    double base = PANEL_PADDING_TOP + PANEL_PADDING_BOTTOM + OPTION_HEIGHT + TITLE_BOTTOM_MARGIN +
                  HEADER_HEIGHT + OPTION_SPACING;
    if (page == MenuPage::Root) {
      return base + ROOT_PAGE_COUNT * (OPTION_HEIGHT + OPTION_SPACING) - OPTION_SPACING + CREDIT_LINE_H;
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

void changeLayoutFloat(size_t float_idx, bool right) {
  auto *f = Settings::float_settings[float_idx];
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

  {
    const size_t row_count = rowCountForPage(page);
    if (data.go_up)   cursor_position = static_cast<int>(rotateVal(static_cast<size_t>(cursor_position), false, row_count));
    if (data.go_down) cursor_position = static_cast<int>(rotateVal(static_cast<size_t>(cursor_position), true,  row_count));
  }

  const size_t cp = static_cast<size_t>(cursor_position);
  if (page == MenuPage::Root) {
    if ((data.rotate_left || data.rotate_right) && cp < ROOT_PAGE_COUNT) {
      current_page = static_cast<size_t>(ROOT_PAGES[cp].page);
      cursor_position = 0;
      // Create + scan on entry so the folder exists to drop files into.
      if (static_cast<MenuPage>(current_page) == MenuPage::Recordings)
        refreshRecordingFiles();
    }
    return;
  }

  if (cp == 0) {
    if (data.rotate_left || data.rotate_right) {
      const MenuPage parent = parentPage(page);
      current_page = static_cast<size_t>(parent);
      if (parent == MenuPage::Layout)
        cursor_position = static_cast<int>(layoutSubIndexForPage(page));
      else
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

  if (page == MenuPage::Recordings) {
    const bool right = data.rotate_right;
    const bool left  = data.rotate_left;
    if (!left && !right) return;
    switch (local_idx) {
      case 0:                                   // Slot 1-5
        g_rec_slot = static_cast<int>(
            rotateVal(static_cast<size_t>(g_rec_slot), right, recordings::kSlotCount));
        break;
      case 1:                                   // Save slot -> file
        saveSelectedSlot();
        break;
      case 2:                                   // File
        if (g_rec_files.empty()) { refreshRecordingFiles(); break; }
        g_rec_file_idx = rotateVal(g_rec_file_idx, right, g_rec_files.size());
        break;
      case 3:                                   // Side
        g_rec_side = (g_rec_side == recordings::kDirLeft) ? recordings::kDirRight
                                                          : recordings::kDirLeft;
        break;
      case 4:                                   // Load file -> slot
        loadSelectedRecording();
        break;
      default: break;
    }
    return;
  }

  if (page == MenuPage::Layout) {
    if (local_idx < LAYOUT_SUB_COUNT) {
      // Enter the submenu
      if (data.rotate_left || data.rotate_right) {
        current_page = static_cast<size_t>(LAYOUT_SUBS[local_idx].page);
        cursor_position = 0;
      }
    } else if (data.rotate_left || data.rotate_right) {
      Settings::resetLayout();
    }
    return;
  }

  if (isLayoutSubPage(page)) {
    size_t count = 0;
    const LayoutRowDef* rows = layoutRowsForPage(page, count);
    if (rows && local_idx < count) {
      if (data.rotate_left)  changeLayoutFloat(rows[local_idx].float_idx, false);
      if (data.rotate_right) changeLayoutFloat(rows[local_idx].float_idx, true);
    }
    return;
  }
}

void ModMenu::draw() {
  if (!is_showing) return;

  // Apply configurable menu position
  tool.setRatios(Settings::MENU_CENTER_X_RATIO.value, Settings::MENU_CENTER_Y_RATIO.value);
  tool.update();

  const auto page = static_cast<MenuPage>(current_page);

  // Per-frame width: MENU_WIDTH_BASE only measures static strings, so measure
  // this page's actual content and grow to fit, then add the Layout offset.
  double MENU_WIDTH = ::MENU_WIDTH_BASE + Settings::MENU_WIDTH_EXTRA.value;
  if (page == MenuPage::Recordings) {
    const double label_px = CHAR_PX * (LABEL_SCALE / 2.0);
    const double value_px = CHAR_PX * (VALUE_SCALE / 2.0);
    const std::wstring slot_no = std::to_wstring(g_rec_slot + 1);
    const std::wstring longest_label = L"Load into Game's Slot " + slot_no;
    std::wstring longest_value = L"<     2P     >";
    if (!g_rec_files.empty()) {
      const auto f = L"< " + fitValue(g_rec_files[g_rec_file_idx].stem().wstring()) + L" >";
      if (f.size() > longest_value.size()) longest_value = f;
    }
    // CHAR_PX is nominal and real glyphs are wider, so pad the measurement.
    constexpr double GLYPH_SLACK = 1.25;
    const double needed = 2 * PANEL_PADDING_X
                        + std::max(TITLE_WIDTH, longest_label.size() * label_px * GLYPH_SLACK)
                        + COL_SPACING
                        + longest_value.size() * value_px * GLYPH_SLACK;
    MENU_WIDTH = std::max(MENU_WIDTH, needed);
  }
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
    // Credit footer
    {
      constexpr double CREDIT_SCALE = 0.82;
      FLinearColor CREDIT_COL{0.55f, 0.55f, 0.55f, 0.70f};
      tool.drawColoredText(static_cast<int>(label_left), static_cast<int>(cy + 6.0),
        FString(L"Foundation by WistfulHopes  \u2014  github.com/WistfulHopes"), CREDIT_COL, CREDIT_SCALE);
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

  if (page == MenuPage::Recordings) {
    const std::wstring file_val =
        g_rec_files.empty()
            ? std::wstring(L"< no .txt files >")
            : (L"< " + fitValue(g_rec_files[g_rec_file_idx].stem().wstring()) + L" >");
    const std::wstring slot_no = std::to_wstring(g_rec_slot + 1);
    const std::wstring rows[RECORDINGS_ROW_COUNT][2] = {
      {L"Slot",                        L"<   " + slot_no + L"   >   " +
                                       recordings::describeSlot(g_rec_slot)},
      {L"Save Slot " + slot_no + L" into txt", L">>  save"},
      {L"File Selection",              file_val},
      {L"Player Side",                 g_rec_side == recordings::kDirLeft
                                           ? L"<     2P     >" : L"<     1P     >"},
      {L"Load into Game's Slot " + slot_no, L">>  load"},
    };
    for (size_t i = 0; i < RECORDINGS_ROW_COUNT; i++) {
      const bool sel = (static_cast<size_t>(cursor_position) == i + 1);
      if (sel) {
        tool.drawRect(menu_left, cy, MENU_WIDTH, OPTION_HEIGHT, CURSOR_BG);
        tool.drawRect(menu_left, cy, MENU_WIDTH, STRIPE_H, CURSOR_STRIPE);
        tool.drawRect(menu_left, cy, ACCENT_W, OPTION_HEIGHT, ACCENT_COLOR);
      }
      tool.drawOutlinedText(label_left, cy + 8.0, FString(rows[i][0].c_str()), LABEL_SCALE);
      tool.drawOutlinedText(value_left, cy + 8.0, FString(rows[i][1].c_str()), VALUE_SCALE);
      cy += OPTION_HEIGHT + OPTION_SPACING;
    }
    if (!g_rec_status.empty()) {
      FLinearColor STATUS_COL{0.65f, 0.65f, 0.65f, 0.85f};
      tool.drawColoredText(static_cast<int>(label_left), static_cast<int>(cy + 6.0),
                           FString(g_rec_status.c_str()), STATUS_COL, 0.85);
    }
    return;
  }

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

  if (page == MenuPage::Layout) {
    // List the 4 submenus
    for (size_t i = 0; i < LAYOUT_SUB_COUNT; i++) {
      const bool sel = (static_cast<size_t>(cursor_position) == i + 1);
      if (sel) {
        tool.drawRect(menu_left, cy, MENU_WIDTH, OPTION_HEIGHT, CURSOR_BG);
        tool.drawRect(menu_left, cy, MENU_WIDTH, STRIPE_H, CURSOR_STRIPE);
        tool.drawRect(menu_left, cy, ACCENT_W, OPTION_HEIGHT, ACCENT_COLOR);
      }
      tool.drawOutlinedText(label_left, cy + 8.0, FString(LAYOUT_SUBS[i].label), LABEL_SCALE);
      tool.drawOutlinedText(value_left, cy + 8.0, FString(L"<< open >>"), VALUE_SCALE);
      cy += OPTION_HEIGHT + OPTION_SPACING;
    }
    // Reset All row
    const bool sel_reset = (static_cast<size_t>(cursor_position) == LAYOUT_SUB_COUNT + 1);
    if (sel_reset) {
      tool.drawRect(menu_left, cy, MENU_WIDTH, OPTION_HEIGHT, CURSOR_BG);
      tool.drawRect(menu_left, cy, MENU_WIDTH, STRIPE_H, CURSOR_STRIPE);
      tool.drawRect(menu_left, cy, ACCENT_W, OPTION_HEIGHT, ACCENT_COLOR);
    }
    tool.drawOutlinedText(label_left, cy + 8.0, FString(L"Reset All to Defaults"), LABEL_SCALE);
    tool.drawOutlinedText(value_left, cy + 8.0, FString(L"<< press >>"), VALUE_SCALE);
    return;
  }

  if (isLayoutSubPage(page)) {
    size_t count = 0;
    const LayoutRowDef* rows = layoutRowsForPage(page, count);
    if (rows) {
      for (size_t i = 0; i < count; i++) {
        const bool sel = (static_cast<size_t>(cursor_position) == i + 1);
        if (sel) {
          tool.drawRect(menu_left, cy, MENU_WIDTH, OPTION_HEIGHT, CURSOR_BG);
          tool.drawRect(menu_left, cy, MENU_WIDTH, STRIPE_H, CURSOR_STRIPE);
          tool.drawRect(menu_left, cy, ACCENT_W, OPTION_HEIGHT, ACCENT_COLOR);
        }
        auto val_str = formatMenuFloat(Settings::float_settings[rows[i].float_idx]->value);
        tool.drawOutlinedText(label_left, cy + 8.0, FString(rows[i].label), LABEL_SCALE);
        tool.drawOutlinedText(value_left, cy + 8.0, FString(val_str.c_str()), VALUE_SCALE);
        cy += OPTION_HEIGHT + OPTION_SPACING;
      }
    }

    // ── Always-on ghost preview for this sub-page ────────────────────
    const auto layout = getLayoutPositions();
    FLinearColor GHOST_FILL  {1.0f, 1.0f, 1.0f, 0.12f};
    FLinearColor GHOST_BORDER{sRGBToLinear(255), 0.0f, 0.0f, 0.85f};

    if (page == MenuPage::LayoutBar || page == MenuPage::LayoutTension) {
      tool.setRatios(layout.framebar_center_x_ratio, layout.framebar_center_y_ratio);
      tool.update();
      constexpr int BAR_L = -1442, BAR_W = 2884, BAR_H = 44;
      constexpr int ONE_TOP = -48, TWO_TOP = 4;
      tool.drawRect(BAR_L, ONE_TOP, BAR_W, BAR_H, GHOST_FILL);
      tool.drawRect(BAR_L, TWO_TOP, BAR_W, BAR_H, GHOST_FILL);
      tool.drawRect(BAR_L, ONE_TOP, BAR_W, 2, GHOST_BORDER);
      tool.drawRect(BAR_L, TWO_TOP, BAR_W, 2, GHOST_BORDER);
      if (page == MenuPage::LayoutTension) {
        constexpr int BOX_W = 620, BOX_H = 72;
        const int box_top = ONE_TOP - layout.tension_bottom_gap - BOX_H;
        tool.drawRect(layout.tension_left_offset, box_top, BOX_W, BOX_H, GHOST_FILL);
        tool.drawRect(layout.tension_left_offset, box_top, BOX_W, 2, GHOST_BORDER);
        tool.drawRect(layout.tension_left_offset, box_top + BOX_H - 2, BOX_W, 2, GHOST_BORDER);
      }
      tool.setRatios(Settings::MENU_CENTER_X_RATIO.value, Settings::MENU_CENTER_Y_RATIO.value);
      tool.update();
    }
    else if (page == MenuPage::LayoutDetail) {
      const double pw = static_cast<double>(layout.detail_panel_width);
      const double ph = 132.0;
      const double px = tool.screenRightEdge() - layout.detail_margin_r - pw;
      const double py = tool.screenTopEdge()   + layout.detail_margin_t;
      tool.drawRect(static_cast<int>(px), static_cast<int>(py),
                    static_cast<int>(pw), static_cast<int>(ph), GHOST_FILL);
      tool.drawRect(static_cast<int>(px), static_cast<int>(py),
                    static_cast<int>(pw), 2, GHOST_BORDER);
      tool.drawRect(static_cast<int>(px + pw - 2), static_cast<int>(py),
                    2, static_cast<int>(ph), GHOST_BORDER);
      tool.drawRect(static_cast<int>(px), static_cast<int>(py + ph - 2),
                    static_cast<int>(pw), 2, GHOST_BORDER);
    }
    else if (page == MenuPage::LayoutMenu) {
      // Crosshair at current menu anchor
      tool.drawRect(-1, -30, 2, 60, GHOST_BORDER);
      tool.drawRect(-30, -1, 60, 2, GHOST_BORDER);
    }
    return;
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
      static_cast<PlayerDisplayMode>(Settings::TENSION_WALL_DAMAGE.value),
  };
}

LayoutPositions ModMenu::getLayoutPositions() const {
  return LayoutPositions{
      Settings::BAR_CENTER_X_RATIO.value,
      Settings::BAR_CENTER_Y_RATIO.value,
      static_cast<int>(Settings::TENSION_LEFT_OFFSET.value),
      static_cast<int>(Settings::TENSION_BOTTOM_GAP.value),
      Settings::DETAIL_MARGIN_T.value,
      Settings::DETAIL_MARGIN_R.value,
      static_cast<int>(Settings::DETAIL_PANEL_WIDTH.value),
  };
}

constexpr int delayAmounts[4] = {0, 20, 30, 60};
int ModMenu::delayAmount() const { return delayAmounts[Settings::DELAY_AMOUNT.value]; }

CurrentOptions ModMenu::getScheme() const {
  return CurrentOptions{color_palettes[Settings::COLOR_SCHEME.value],
                        fadeEnabled(), delimEnabled(), cancelEnabled(), crossupEnabled()};
}

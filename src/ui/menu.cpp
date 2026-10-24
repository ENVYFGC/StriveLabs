#include "menu.h"
#include "keybinds.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include "recordings.h"
#include "replay_manager.h"
#include "timeline.h"
#include "combo_log.h"
#include "imgui_overlay.h"
#include <mutex>
#include <atomic>
#include <thread>
#include <commdlg.h>

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
  std::array<const wchar_t *, 8> values;
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
      OptionData{L"Meters Overlay:", 2, {L"<  Disabled  >", L"<  Enabled   >"}},
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
  SettingsEntry TENSION_PENALTY = SettingsEntry{
      OptionData{L"Tension Penalty:", 4,
                 {L"<  Disabled  >", L"<     1P     >", L"<     2P     >", L"<  1P & 2P   >"}},
      "tension_penalty", 0};
  SettingsEntry WALL_DAMAGE = SettingsEntry{
      OptionData{L"Wall Damage:", 4,
                 {L"<  Disabled  >", L"<     1P     >", L"<     2P     >", L"<  1P & 2P   >"}},
      "wall_damage", 0};
  SettingsEntry TENSION_SIDE = SettingsEntry{
      OptionData{L"Overlay Side:", 2, {L"<    Left    >", L"<   Right    >"}},
      "tension_side", 0};
  SettingsEntry METERS_DECIMALS = SettingsEntry{
      OptionData{L"Decimal Places:", 4, {L"<     0      >", L"<     1      >", L"<     2      >", L"<     3      >"}},
      "meters_decimals", 2};
  SettingsEntry TENSION_CURRENT = SettingsEntry{
      OptionData{L"Tension Now:", 4,
                 {L"<  Disabled  >", L"<     1P     >", L"<     2P     >", L"<  1P & 2P   >"}},
      "tension_current", 0};
  SettingsEntry UI_ACCENT = SettingsEntry{
      OptionData{L"Accent Color:", 7, {L"Red", L"Blue", L"Teal", L"Purple", L"Gold", L"White", L"Custom"}},
      "ui_accent", 0};
  SettingsEntry TIMELINE_ON = SettingsEntry{
      OptionData{L"Timeline Window:", 2, {L"<  Disabled  >", L"<  Enabled   >"}},
      "timeline_on", 0};  // off until you turn it on; remembered across restarts
  SettingsEntry METERS_SNAP = SettingsEntry{
      OptionData{L"Snap Meters to Frame Bar:", 2, {L"<  Disabled  >", L"<  Enabled   >"}},
      "meters_snap", 0};
  SettingsEntry UI_HIDE_WINDOW_DECORATIONS = SettingsEntry{
      OptionData{L"Hide Decorations When Menu Is Closed:", 2, {L"<  Disabled  >", L"<  Enabled   >"}},
      "ui_hide_window_decorations", 0};
  std::array<SettingsEntry *, 24> settings = {
      &FRAMEBAR, &HITBOXES, &FADE, &DELIM, &COLOR_SCHEME,
      &SHOW_DASH_FRAMES, &CROSSUP_ENABLED, &PAUSE_TYPE, &DELAY_AMOUNT,
      &TENSION_ENABLED, &TENSION_GAIN, &TENSION_ADVANTAGE,
      &TENSION_ADV_CHANGE, &TENSION_BURST, &TENSION_PULSE, &TENSION_PENALTY, &TENSION_CURRENT, &WALL_DAMAGE, &TENSION_SIDE, &METERS_DECIMALS, &METERS_SNAP, &UI_ACCENT, &TIMELINE_ON,
      &UI_HIDE_WINDOW_DECORATIONS,
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
  // Appearance (ImGui menu). "ui_" settings are not touched by resetLayout().
  FloatSetting UI_OPACITY  = {"ui_opacity",  0.94, 0.94, 0.50, 1.00, 0.02};
  FloatSetting UI_SCALE    = {"ui_scale",    1.00, 1.00, 0.75, 1.50, 0.05};
  FloatSetting UI_CUSTOM_R = {"ui_custom_r", 1.00, 1.00, 0.00, 1.00, 0.01};
  FloatSetting UI_CUSTOM_G = {"ui_custom_g", 0.00, 0.00, 0.00, 1.00, 0.01};
  FloatSetting UI_CUSTOM_B = {"ui_custom_b", 0.00, 0.00, 0.00, 1.00, 0.01};
  // Per window sizes. UI_SCALE above is the menu's.
  FloatSetting UI_SCALE_METERS   = {"ui_scale_meters",   1.00, 1.00, 0.50, 2.00, 0.05};
  FloatSetting UI_SCALE_BAR      = {"ui_scale_bar",      1.00, 1.00, 0.50, 2.00, 0.05};
  FloatSetting UI_SCALE_TIMELINE = {"ui_scale_timeline", 1.00, 1.00, 0.50, 2.00, 0.05};

  FloatSetting REWIND_DELAY_SECONDS = {"rewind_delay_seconds", 0.0, 0.0, 0.0, 60.0, 1.0};

  std::array<FloatSetting *, 19> float_settings = {
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
      &UI_OPACITY,            // 10
      &UI_SCALE,              // 11
      &UI_CUSTOM_R,           // 12
      &UI_CUSTOM_G,           // 13
      &UI_CUSTOM_B,           // 14
      &UI_SCALE_METERS,       // 15
      &UI_SCALE_BAR,          // 16
      &UI_SCALE_TIMELINE,     // 17
      &REWIND_DELAY_SECONDS,  // 18
  };

  const std::filesystem::path WORKING_DIRECTORY = UE4SSProgram::get_program().get_working_directory();
  const auto CONFIG_PATH = WORKING_DIRECTORY / "StriveLabs.txt";

  // Font for the meters and frame bar: a file the player picked, kept in
  // ue4ss/StriveLabs_fonts. Empty means Barlow. Read by the render thread.
  std::mutex overlay_font_mutex;
  std::string overlay_font;  // UTF-8 file name
  void setOverlayFont(const std::string& name) {
    std::lock_guard<std::mutex> lock(overlay_font_mutex);
    overlay_font = name;
  }
  std::string overlayFont() {
    std::lock_guard<std::mutex> lock(overlay_font_mutex);
    return overlay_font;
  }

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

      if (key == "overlay_font") { setOverlayFont(val_str); continue; }

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

      if (binds::loadLine(key, val_str)) continue;

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
    binds::save(configFile);
    configFile << "overlay_font=" << overlayFont() << "\n";
    configFile.close();
    RC::Output::send<LogLevel::Verbose>(STR("Saved config file\n"));
  }

  void resetLayout() {
    for (auto *f : float_settings)
      if (f != &REWIND_DELAY_SECONDS && f->id_.rfind("ui_", 0) != 0) f->reset();  // positions only, not the look
    saveConfig();
  }
}

namespace {
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

  enum class MenuPage : size_t {
    Root = 0,
    FrameBar,
    TimingInput,
    Tension,
    Layout,
    LayoutBar,
    LayoutTension,
    LayoutDetail,
    LayoutMenu,
    Recordings,
    Keybinds,
    Takeover,
    Hitboxes,
    Timeline,
  };

  // ---- Recordings page state ----------------------------------------------
  // Slot, save, file, browse, side, load, stream, capture side, capture.
  constexpr size_t RECORDINGS_ROW_COUNT = 9;

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

  // ---- Browse: Windows file picker for a recording ------------------------
  // Runs on its own thread so the game keeps running while it is open. The pick
  // is handed back and applied on the game thread in ModMenu::update().
  std::mutex g_browse_mutex;
  std::filesystem::path g_browse_pick;
  std::atomic<bool> g_browse_open{false};
  std::atomic<bool> g_browse_font{false};  // picking the overlay font, not a recording

  std::filesystem::path fontsDir() { return Settings::WORKING_DIRECTORY / "StriveLabs_fonts"; }

  void startBrowse(bool font = false) {
    if (g_browse_open.exchange(true)) return;  // already open
    g_browse_font = font;
    const std::wstring dir = (font ? fontsDir() : Settings::recordingsDir()).wstring();
    HWND owner = GetForegroundWindow();
    std::thread([dir, owner, font] {
      wchar_t file[MAX_PATH] = L"";
      OPENFILENAMEW ofn{};
      ofn.lStructSize = sizeof(ofn);
      ofn.hwndOwner = owner;
      ofn.lpstrFilter = font ? L"Fonts (*.ttf;*.otf)\0*.ttf;*.otf\0All files\0*.*\0"
                             : L"Recordings (*.txt)\0*.txt\0All files\0*.*\0";
      ofn.lpstrFile = file;
      ofn.nMaxFile = MAX_PATH;
      ofn.lpstrInitialDir = dir.c_str();
      ofn.lpstrTitle = font ? L"Pick a font for the meters and frame bar" : L"Pick a recording";
      ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
      if (GetOpenFileNameW(&ofn)) {
        std::lock_guard<std::mutex> lock(g_browse_mutex);
        g_browse_pick = file;
      }
      g_browse_open = false;
    }).detach();
  }

  std::string utf8(const std::wstring& w);

  // Game thread: use the picked font for the meters and frame bar. A font from
  // elsewhere is copied into StriveLabs_fonts, so the setting keeps working.
  void applyFontPick(const std::filesystem::path& pick) {
    std::error_code ec;
    std::filesystem::create_directories(fontsDir(), ec);
    if (!std::filesystem::equivalent(pick.parent_path(), fontsDir(), ec)) {
      ec.clear();
      std::filesystem::copy_file(pick, fontsDir() / pick.filename(),
                                 std::filesystem::copy_options::overwrite_existing, ec);
      if (ec) { COMBO_LOG("overlay font: could not copy the picked file"); return; }
    }
    Settings::setOverlayFont(utf8(pick.filename().wstring()));
    Settings::saveConfig();
  }

  // Game thread: select the picked file. Files from elsewhere are copied into the
  // recordings folder so they stay in the File list.
  void applyBrowsePick() {
    std::filesystem::path pick;
    {
      std::lock_guard<std::mutex> lock(g_browse_mutex);
      pick.swap(g_browse_pick);
    }
    if (pick.empty()) return;
    if (g_browse_font) { applyFontPick(pick); return; }
    const auto dir = Settings::recordingsDir();
    std::error_code ec;
    auto target = pick;
    if (!std::filesystem::equivalent(pick.parent_path(), dir, ec)) {
      target = dir / pick.filename();
      if (!std::filesystem::exists(target, ec))
        std::filesystem::copy_file(pick, target, ec);
      if (ec) { g_rec_status = L"Could not copy " + pick.filename().wstring(); return; }
    }
    refreshRecordingFiles();
    for (size_t i = 0; i < g_rec_files.size(); i++)
      if (g_rec_files[i].filename() == target.filename()) { g_rec_file_idx = i; break; }
    g_rec_status = L"Picked " + target.filename().wstring();
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

  // Feed the selected file to the dummy through the input hook instead of a
  // slot: no 2700-frame ceiling, and still frame-exact.
  // Pull the opponent's inputs for the current replay round straight out of the
  // replay buffer and save them as a .txt, ready to stream at the dummy.
  // ---- Capture from a replay: start / stop, 40 seconds at most -------------
  int g_cap_side = 1;  // 0 = P1, 1 = P2
  // Timeline window switch, saved in the config
  bool timelineOn() { return Settings::TIMELINE_ON.value != 0; }
  void setTimelineOn(bool on) { Settings::TIMELINE_ON.value = on ? 1 : 0; Settings::saveConfig(); }

  std::wstring secondsText(uint32_t frames) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.1f s", frames / 60.0);
    return buf;
  }

  void finishCapture() {
    std::wstring err;
    auto frames = ReplayManager::instance().stopCapture(err);
    if (frames.empty()) { g_rec_status = err.empty() ? L"Nothing was captured" : err; return; }
    char stamp[32] = "";
    { const std::time_t t = std::time(nullptr); std::tm tm{};
      if (localtime_s(&tm, &t) == 0) std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H%M%S", &tm); }
    const auto path = Settings::recordingsDir() /
        (std::string(stamp[0] ? stamp : "capture") + "_replay_" + (g_cap_side == 0 ? "P1_" : "P2_") +
         std::to_string(frames.size()) + "f.txt");
    std::wstring werr;
    if (!recordings::writeFramesToFile(frames, path, g_cap_side == 0 ? recordings::kDirRight : recordings::kDirLeft, werr)) {
      g_rec_status = werr;
      return;
    }
    refreshRecordingFiles();
    for (size_t i = 0; i < g_rec_files.size(); i++)
      if (g_rec_files[i].filename() == path.filename()) { g_rec_file_idx = i; break; }
    g_rec_status = L"Saved " + path.filename().wstring() + L" (" + secondsText(static_cast<uint32_t>(frames.size())) +
                   L"). Load it into a slot above.";
  }

  void toggleCapture() {
    if (ReplayManager::instance().captureActive()) { finishCapture(); return; }
    std::wstring err;
    if (!ReplayManager::instance().startCapture(g_cap_side, err)) { g_rec_status = err; return; }
    g_rec_status = std::wstring(L"Capturing ") + (g_cap_side == 0 ? L"P1" : L"P2") +
                   L". Press again to stop, or it stops at 40 s.";
  }

  void streamSelectedRecording() {
    if (recordings::streamActive() || recordings::streamArmed()) {
      recordings::streamStop();
      g_rec_status = L"stream stopped";
      return;
    }
    if (g_rec_files.empty()) { g_rec_status = L"no .txt files found"; return; }
    auto parsed = recordings::parseFile(g_rec_files[g_rec_file_idx], g_rec_side);
    if (!parsed.ok()) { g_rec_status = parsed.error; return; }
    const size_t n = parsed.frames.size();
    recordings::streamArm(std::move(parsed.frames));
    g_rec_status = L"armed " + std::to_wstring(n) + L"f - starts on round call";
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
    {MenuPage::Layout, L"Appearance"},
    {MenuPage::Keybinds, L"Controls"},
    {MenuPage::FrameBar, L"Frame Bar"},
    {MenuPage::Hitboxes, L"Hitboxes"},
    {MenuPage::Tension, L"Meters"},
    {MenuPage::Recordings, L"Recordings"},
    {MenuPage::Takeover, L"Takeover"},
    {MenuPage::Timeline, L"Timeline"},
    {MenuPage::TimingInput, L"Training"},
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
    {MenuPage::LayoutTension,  L"Tension Box"},
    {MenuPage::LayoutDetail,   L"Detail Panel"},
    {MenuPage::LayoutMenu,     L"Mod Menu"},
  };
  constexpr size_t LAYOUT_SUB_COUNT = sizeof(LAYOUT_SUBS) / sizeof(LAYOUT_SUBS[0]);

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

  std::wstring formatMenuNumber(const FloatSetting& setting) {
    if (&setting == &Settings::REWIND_DELAY_SECONDS)
      return std::to_wstring(static_cast<int>(setting.value));
    return formatMenuFloat(setting.value);
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
      case MenuPage::FrameBar:      return L"-- FRAME BAR --";
      case MenuPage::Hitboxes:      return L"-- HITBOXES --";
      case MenuPage::TimingInput:   return L"-- TRAINING --";
      case MenuPage::Tension:       return L"-- METERS --";
      case MenuPage::Layout:        return L"-- APPEARANCE --";
      case MenuPage::LayoutBar:     return L"-- FRAME BAR --";
      case MenuPage::LayoutTension: return L"-- TENSION BOX --";
      case MenuPage::LayoutDetail:  return L"-- DETAIL PANEL --";
      case MenuPage::LayoutMenu:    return L"-- MOD MENU --";
      case MenuPage::Recordings:    return L"-- RECORDINGS --";
      case MenuPage::Keybinds:      return L"-- CONTROLS --";
      case MenuPage::Takeover:      return L"-- TAKEOVER --";
      case MenuPage::Timeline:      return L"-- TIMELINE --";
    }
    return L"-- MENU --";
  }

  // Row order, section headings and actions share one definition for mouse,
  // keyboard/controller navigation and both menu renderers. Config IDs stay put.
  enum class SettingsAction { None, ResetWindows, PickColor, PickFont, ResetFont, TakeoverToggle };
  struct SettingsRowDef {
    const wchar_t* section = L"";
    const wchar_t* label = L"";
    SettingsEntry* setting = nullptr;
    FloatSetting* number = nullptr;
    SettingsAction action = SettingsAction::None;
    int capture = -1;  // stable binds::Action or GAME_BASE + GameAction ID
  };

  constexpr binds::Action TAKEOVER_HOTKEYS[] = {binds::SAVE, binds::TAKEOVER_P1, binds::TAKEOVER_P2, binds::LOAD};

  bool isTakeoverHotkey(int action) {
    return std::find(std::begin(TAKEOVER_HOTKEYS), std::end(TAKEOVER_HOTKEYS), action) != std::end(TAKEOVER_HOTKEYS);
  }

  std::vector<SettingsRowDef> settingsRowsForPage(MenuPage page) {
    using namespace Settings;
    if (page == MenuPage::FrameBar) {
      std::vector<SettingsRowDef> rows = {
        {L"Settings", L"Frame Bar", &FRAMEBAR},
        {L"", L"Show Dash", &SHOW_DASH_FRAMES},
        {L"", L"Show Crossup", &CROSSUP_ENABLED},
        {L"Appearance", L"Color scheme", &COLOR_SCHEME},
        {L"", L"Fade effect", &FADE},
        {L"", L"Delimit segments", &DELIM},
      };
      if (imgui_overlay::running())
        rows.push_back({L"", L"Frame bar size", nullptr, &UI_SCALE_BAR});
      else {
        rows.push_back({L"", L"X position (screen ratio)", nullptr, &BAR_CENTER_X_RATIO});
        rows.push_back({L"", L"Y position (screen ratio)", nullptr, &BAR_CENTER_Y_RATIO});
      }
      return rows;
    }
    if (page == MenuPage::Hitboxes)
      return {{L"", L"Hitboxes", &HITBOXES}};
    if (page == MenuPage::TimingInput)
      return {{L"", PAUSE_TYPE.display.title, &PAUSE_TYPE},
              {L"", DELAY_AMOUNT.display.title, &DELAY_AMOUNT}};
    if (page == MenuPage::Tension) {
      std::vector<SettingsRowDef> rows = {
        {L"Settings", L"Meters overlay", &TENSION_ENABLED},
        {L"", L"Tension now", &TENSION_CURRENT},
        {L"", L"Tension gain", &TENSION_GAIN},
        {L"", L"Tension advantage", &TENSION_ADVANTAGE},
        {L"", L"Tension advantage change", &TENSION_ADV_CHANGE},
        {L"", L"Tension pulse", &TENSION_PULSE},
        {L"", L"Tension penalty", &TENSION_PENALTY},
        {L"", L"Burst", &TENSION_BURST},
        {L"", L"Wall damage", &WALL_DAMAGE},
        {L"Appearance", L"Overlay side", &TENSION_SIDE},
        {L"", L"Snap meters to frame bar", &METERS_SNAP},
        {L"", L"Decimal places", &METERS_DECIMALS},
      };
      if (imgui_overlay::running())
        rows.push_back({L"", L"Meters size", nullptr, &UI_SCALE_METERS});
      return rows;
    }
    if (page == MenuPage::Timeline) {
      std::vector<SettingsRowDef> rows = {{L"Settings", L"Timeline window", &TIMELINE_ON}};
      if (imgui_overlay::running())
        rows.push_back({L"Appearance", L"Timeline size", nullptr, &UI_SCALE_TIMELINE});
      return rows;
    }
    if (page == MenuPage::Keybinds || page == MenuPage::Takeover) {
      std::vector<SettingsRowDef> rows;
      auto hotkey = [&](int action) {
        SettingsRowDef row; row.label = binds::LABELS[action]; row.capture = action;
        rows.push_back(row);
      };
      if (page == MenuPage::Keybinds) {
        for (int action = 0; action < binds::COUNT; action++)
          if (!isTakeoverHotkey(action)) hotkey(action);
      } else {
        rows.push_back({L"Rewind", L"Playback delay (seconds)", nullptr, &REWIND_DELAY_SECONDS});
        const auto hotkeysBegin = rows.size();
        for (int action : TAKEOVER_HOTKEYS) hotkey(action);
        rows[hotkeysBegin].section = L"Hotkeys";
        rows.push_back({L"Replay takeover", L"Use these buttons in takeover", nullptr, nullptr, SettingsAction::TakeoverToggle});
        for (int action = 0; action < binds::G_COUNT; action++) {
          SettingsRowDef row; row.label = binds::G_LABELS[action]; row.capture = binds::GAME_BASE + action;
          rows.push_back(row);
        }
      }
      return rows;
    }
    if (page == MenuPage::Layout && imgui_overlay::running())
      return {
        {L"Style", L"Accent color", &UI_ACCENT},
        {L"", L"Custom color", nullptr, nullptr, SettingsAction::PickColor},
        {L"", L"Panel opacity", nullptr, &UI_OPACITY},
        {L"Font", L"Meters and frame bar font", nullptr, nullptr, SettingsAction::PickFont},
        {L"", L"Back to Barlow", nullptr, nullptr, SettingsAction::ResetFont},
        {L"Windows", L"Menu size", nullptr, &UI_SCALE},
        {L"", L"Hide decorations when menu is closed", &UI_HIDE_WINDOW_DECORATIONS},
        {L"", L"Reset all window positions", nullptr, nullptr, SettingsAction::ResetWindows},
      };
    return {};
  }

  bool isSettingsPage(MenuPage page) {
    return page == MenuPage::FrameBar || page == MenuPage::Hitboxes ||
           page == MenuPage::TimingInput || page == MenuPage::Tension ||
           page == MenuPage::Timeline || page == MenuPage::Keybinds || page == MenuPage::Takeover ||
           (page == MenuPage::Layout && imgui_overlay::running());
  }

  size_t rowCountForPage(MenuPage page) {
    switch (page) {
      case MenuPage::Root: return ROOT_PAGE_COUNT;
      case MenuPage::FrameBar:
      case MenuPage::Hitboxes:
      case MenuPage::TimingInput:
      case MenuPage::Tension:
      case MenuPage::Timeline:
      case MenuPage::Keybinds:
      case MenuPage::Takeover:
        return settingsRowsForPage(page).size() + 1;
      case MenuPage::Layout:        return imgui_overlay::running() ? settingsRowsForPage(page).size() + 1 : LAYOUT_SUB_COUNT + 2;
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

  // Headings belong to the following control and do not consume a cursor index.
  const wchar_t* sectionForRow(MenuPage page, size_t row) {
    if (isSettingsPage(page)) {
      const auto rows = settingsRowsForPage(page);
      return row < rows.size() ? rows[row].section : L"";
    }
    return L"";
  }

  constexpr double DESC_LINE_H  = 26.0;
  constexpr double DESC_SCALE   = 1.05;
  constexpr double DESC_LINES   = 2.0; // description line + disclaimer line
  constexpr double DESC_BLOCK_H = DESC_LINES * DESC_LINE_H + 10.0; // +padding below

  constexpr double CREDIT_LINE_H = 28.0; // extra height reserved for the WistfulHopes credit footer

  // Bottom-right logo watermark, drawn inside the panel under the text.
  constexpr int LOGO_SIZE = 320;
  constexpr int LOGO_PAD = 12;
  constexpr float LOGO_OPACITY = 0.70f;
  Unreal::UObject* g_logo = nullptr;
  bool g_logo_tried = false;
  // Loads ue4ss/StriveLabs_logo.png once through the engine's own PNG importer and roots it against GC.
  Unreal::UObject* logoTexture(Unreal::UObject* world_context) {
    if (g_logo || g_logo_tried || !world_context) return g_logo;
    g_logo_tried = true;
    auto* lib = UObjectGlobals::StaticFindObject<Unreal::UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetRenderingLibrary"));
    if (!lib) return nullptr;
    static auto fn_name = Unreal::FName(STR("ImportFileAsTexture2D"), Unreal::FNAME_Add);
    auto* fn = lib->GetFunctionByNameInChain(fn_name);
    if (!fn) return nullptr;
    struct { Unreal::UObject* WorldContextObject; Unreal::FString Filename; Unreal::UObject* ReturnValue; } params{
        world_context, Unreal::FString((Settings::WORKING_DIRECTORY / "StriveLabs_logo.png").wstring().c_str()), nullptr};
    lib->ProcessEvent(fn, &params);
    if (params.ReturnValue) {
      // Root it against GC. The shipped UE4SS.dll exports UObject::SetRootSet as a function, while the
      // headers we build against inline it through FUObjectArray::IndexToObject, which that dll does not
      // export. Resolve the export by name so the mod loads on the shipped UE4SS.
      using SetRootSetFn = void (*)(Unreal::UObject*);
      static auto set_root = reinterpret_cast<SetRootSetFn>(
          GetProcAddress(GetModuleHandleA("UE4SS.dll"), "?SetRootSet@UObject@Unreal@RC@@QEAAXXZ"));
      if (set_root) set_root(params.ReturnValue);
      g_logo = params.ReturnValue;
    }
    return g_logo;
  }

  double computePageHeight(MenuPage page) {
    double base = PANEL_PADDING_TOP + PANEL_PADDING_BOTTOM + OPTION_HEIGHT + TITLE_BOTTOM_MARGIN +
                  HEADER_HEIGHT + OPTION_SPACING;
    if (page == MenuPage::Root) {
      return base + ROOT_PAGE_COUNT * (OPTION_HEIGHT + OPTION_SPACING) - OPTION_SPACING + CREDIT_LINE_H;
    }
    const size_t row_count = rowCountForPage(page);
    double headings = 0;
    for (size_t i = 1; i < row_count; i++)
      if (*sectionForRow(page, i - 1)) headings += HEADER_HEIGHT + OPTION_SPACING;
    return base + row_count * (OPTION_HEIGHT + OPTION_SPACING) - OPTION_SPACING + headings;
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

int g_picker_seq = 0;  // bumped to open the custom color picker

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

void changeLayoutFloat(size_t float_idx, bool right);
extern int g_picker_seq;

namespace {
  std::mutex g_view_mutex;
  MenuView g_view;
  std::vector<MenuClick> g_clicks;
  std::atomic<unsigned> g_popped{0};
}

void ModMenu::update(PressedKeys data) {
  std::vector<MenuClick> clicks;
  {
    std::lock_guard<std::mutex> lock(g_view_mutex);
    clicks.swap(g_clicks);
  }
  for (const auto& c : clicks)
    if (c.type == MenuClick::Type::Move || c.type >= MenuClick::Type::TlRewind ||
        (binds::capturing < 0 && (is_showing || c.page >= 0))) applyClick(c);
  applyBrowsePick();
  if (ReplayManager::instance().captureFull()) finishCapture();
  updateInner(data);
  buildView();
}

void ModMenu::updateInner(PressedKeys data) {
  if (data.toggle_framebar) changeSetting(Settings::FRAMEBAR_INDEX);
  if (data.toggle_hitbox)   changeSetting(Settings::HITBOX_INDEX);
  if (data.toggle_menu) {
    // Keep the current page and cursor when reopening the menu.
    is_showing = !is_showing;
  }

  if (!is_showing) return;

  if (binds::consumeDirty()) Settings::saveConfig();
  if (binds::capturing >= 0) return;  // waiting for a key/button; inputs go to the capture

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
      // Start on the first setting, not on Back.
      cursor_position = rowCountForPage(static_cast<MenuPage>(current_page)) > 1 ? 1 : 0;
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
    if (!(data.rotate_left || data.rotate_right)) return;
    const auto rows = settingsRowsForPage(page);
    if (local_idx >= rows.size()) return;
    const auto& row = rows[local_idx];
    if (row.capture >= 0)
      binds::beginCapture(row.capture);
    else if (row.setting)
      changeSetting(static_cast<size_t>(Settings::indexById(row.setting->id_)), data.rotate_right);
    else if (row.number)
      changeLayoutFloat(static_cast<size_t>(Settings::floatIndexById(row.number->id_)), data.rotate_right);
    else switch (row.action) {
      case SettingsAction::ResetWindows: Settings::resetLayout(); imgui_overlay::resetWindows(); break;
      case SettingsAction::PickColor: Settings::UI_ACCENT.value = 6; Settings::saveConfig(); g_picker_seq++; break;
      case SettingsAction::PickFont: startBrowse(true); break;
      case SettingsAction::ResetFont: Settings::setOverlayFont(""); Settings::saveConfig(); break;
      case SettingsAction::TakeoverToggle: binds::use_mod_binds = !binds::use_mod_binds; Settings::saveConfig(); break;
      default: break;
    }
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
      case 3:                                   // Browse for a file
        startBrowse();
        break;
      case 4:                                   // Side
        g_rec_side = (g_rec_side == recordings::kDirLeft) ? recordings::kDirRight
                                                          : recordings::kDirLeft;
        break;
      case 5:                                   // Load file -> slot
        loadSelectedRecording();
        break;
      case 6:                                   // Stream file -> dummy
        streamSelectedRecording();
        break;
      case 7:                                   // Capture side
        g_cap_side ^= 1;
        break;
      case 8:                                   // Capture from replay: start / stop
        toggleCapture();
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
        cursor_position = rowCountForPage(static_cast<MenuPage>(current_page)) > 1 ? 1 : 0;
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
  if (!imgui_overlay::running()) {
    const int seconds = ReplayManager::instance().rewindCountdownSeconds();
    if (seconds > 0) {
      DrawContext countdown(0.5, 0.25);
      countdown.update();
      const std::wstring label = L"Playback in " + std::to_wstring(seconds) + L" s";
      countdown.drawRect(-180, -12, 360, 60, FLinearColor{0.01f, 0.02f, 0.04f, 0.9f});
      countdown.drawOutlinedText(-165, 0, FString(label.c_str()), 1.4);
    }
  }
  if (!is_showing) return;
  hud_panel = !imgui_overlay::running();
  if (!hud_panel) {
    if (isLayoutSubPage(static_cast<MenuPage>(current_page))) drawLayoutGhost(static_cast<int>(current_page));
    return;
  }

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
  // Logo watermark, bottom-right, under the text
  if (auto* logo = logoTexture(tool.hud())) {
    const int size = static_cast<int>(std::min<double>(LOGO_SIZE, menu_height - 2 * LOGO_PAD));
    tool.drawTexture(logo,
                     static_cast<int>(menu_left + MENU_WIDTH) - size - LOGO_PAD,
                     static_cast<int>(menu_top + menu_height) - size - LOGO_PAD,
                     size, size, LOGO_OPACITY);
  }

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

  auto drawSection = [&](size_t row) {
    const auto* title = sectionForRow(page, row);
    if (!*title) return;
    tool.drawRect(menu_left, cy, MENU_WIDTH, HEADER_HEIGHT, SECTION_BG);
    tool.drawOutlinedText(label_left, cy + 4.0, FString(title), HEADER_SCALE);
    cy += HEADER_HEIGHT + OPTION_SPACING;
  };

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
      {L"Browse for a file",           L">>  browse"},
      {L"Player Side",                 g_rec_side == recordings::kDirLeft
                                           ? L"<     2P     >" : L"<     1P     >"},
      {L"Load into Game's Slot " + slot_no, L">>  load"},
      {L"Stream to Dummy",             (recordings::streamActive() || recordings::streamArmed())
                                           ? (L">>  stop  (" + std::to_wstring(recordings::streamPos())
                                              + L"/" + std::to_wstring(recordings::streamLen()) + L")")
                                           : std::wstring(L">>  start  (no length limit)")},
      {L"Capture Side",                g_cap_side == 0 ? L"<     1P     >" : L"<     2P     >"},
      {L"Capture from Replay",         ReplayManager::instance().captureActive()
                                           ? (L">>  stop  (" + secondsText(ReplayManager::instance().captureFrames()) + L" / 40 s)")
                                           : std::wstring(L">>  start  (40 s max)")},
    };
    for (size_t i = 0; i < RECORDINGS_ROW_COUNT; i++) {
      drawSection(i);
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
    const auto rows = settingsRowsForPage(page);
    for (size_t i = 0; i < rows.size(); i++) {
      drawSection(i);
      const bool sel = (static_cast<size_t>(cursor_position) == i + 1);
      if (sel) {
        tool.drawRect(menu_left, cy, MENU_WIDTH, OPTION_HEIGHT, CURSOR_BG);
        tool.drawRect(menu_left, cy, MENU_WIDTH, STRIPE_H, CURSOR_STRIPE);
        tool.drawRect(menu_left, cy, ACCENT_W, OPTION_HEIGHT, ACCENT_COLOR);
      }
      const auto& row = rows[i];
      std::wstring value;
      if (row.capture >= 0) {
        const bool game = row.capture >= binds::GAME_BASE;
        const int id = game ? row.capture - binds::GAME_BASE : row.capture;
        value = binds::capturing == row.capture ? L"< press input | Esc: cancel | Bksp: clear >"
          : binds::keyName(game ? binds::gkey[id] : binds::key[id]) + L"  |  " +
            binds::padName(game ? binds::gpad[id] : binds::pad[id]);
      } else if (row.setting) value = row.setting->display.values[row.setting->value];
      else if (row.number) value = formatMenuNumber(*row.number);
      else if (row.action == SettingsAction::TakeoverToggle)
        value = binds::use_mod_binds ? L"<  Enabled   >" : L"<  Disabled  >";
      tool.drawOutlinedText(label_left, cy + 8.0, FString(row.label), LABEL_SCALE);
      tool.drawOutlinedText(value_left, cy + 8.0, FString(value.c_str()), VALUE_SCALE);
      cy += OPTION_HEIGHT + OPTION_SPACING;
    }
    if (page == MenuPage::FrameBar) drawLayoutGhost(static_cast<int>(MenuPage::LayoutBar));
    return;
  }

  if (page == MenuPage::Layout) {
    // Layout submenus for the HUD renderer.
    for (size_t i = 0; i < LAYOUT_SUB_COUNT; i++) {
      drawSection(i);
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
        drawSection(i);
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

    drawLayoutGhost(static_cast<int>(page));
    return;
  }
}


void ModMenu::drawLayoutGhost(int page_i) {
  const auto page = static_cast<MenuPage>(page_i);
  tool.setRatios(Settings::MENU_CENTER_X_RATIO.value, Settings::MENU_CENTER_Y_RATIO.value);
  tool.update();
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
}

// ---- ImGui menu snapshot ------------------------------------------------------
namespace {
  std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
  }
  // "<  Enabled   >" -> "Enabled", "Frame Bar:" -> "Frame Bar"
  std::string clean(const wchar_t* w) {
    std::wstring s = w ? w : L"";
    for (wchar_t& c : s) if (c == L'<' || c == L'>') c = L' ';
    const auto a = s.find_first_not_of(L' ');
    const auto b = s.find_last_not_of(L' ');
    s = (a == std::wstring::npos) ? L"" : s.substr(a, b - a + 1);
    if (!s.empty() && s.back() == L':') s.pop_back();
    return utf8(s);
  }
}

std::filesystem::path ModMenu::dataDir() { return Settings::WORKING_DIRECTORY; }
std::string ModMenu::overlayFont() { return Settings::overlayFont(); }
float ModMenu::uiScale(int which) {
  const FloatSetting* sizes[] = {&Settings::UI_SCALE, &Settings::UI_SCALE_METERS,
                                           &Settings::UI_SCALE_BAR, &Settings::UI_SCALE_TIMELINE};
  const auto* f = sizes[(which >= 0 && which < 4) ? which : 0];
  return static_cast<float>(std::clamp(f->value, f->min_value, f->max_value));
}

void ModMenu::hide() {
  if (!is_showing) return;
  is_showing = false;
  buildView();
}

MenuView ModMenu::view() const {
  std::lock_guard<std::mutex> lock(g_view_mutex);
  return g_view;
}

void ModMenu::click(const MenuClick& c) {
  std::lock_guard<std::mutex> lock(g_view_mutex);
  if (g_clicks.size() < 16) g_clicks.push_back(c);
}

void ModMenu::setPopped(unsigned mask) { g_popped = mask; }

void ModMenu::applyClick(const MenuClick& c) {
  if (c.type == MenuClick::Type::CloseMenu) { is_showing = false; return; }
  if (c.type == MenuClick::Type::SetColor) {
    Settings::UI_CUSTOM_R.value = ((c.a >> 16) & 0xFF) / 255.0;
    Settings::UI_CUSTOM_G.value = ((c.a >> 8) & 0xFF) / 255.0;
    Settings::UI_CUSTOM_B.value = (c.a & 0xFF) / 255.0;
    Settings::UI_ACCENT.value = 6;
    Settings::saveConfig();
    return;
  }
  if (c.type == MenuClick::Type::BarWindow) {
    Settings::FRAMEBAR.value = c.b ? 1 : 0;
    Settings::saveConfig();
    return;
  }
  switch (c.type) {  // timeline window
    case MenuClick::Type::TlRewind:
      Timeline::instance().pressHotkey(binds::LOAD);
      return;
    case MenuClick::Type::TlSetPoint: Timeline::instance().pressHotkey(binds::SAVE); return;
    case MenuClick::Type::TlPlay:
      if (!ReplayManager::instance().isRewindDelayActive())
        Timeline::instance().setPaused(!Timeline::instance().isPaused());
      return;
    case MenuClick::Type::TlStep:
      if (!ReplayManager::instance().isRewindDelayActive() && Timeline::instance().isPaused())
        Timeline::instance().advanceFrame();
      return;
    case MenuClick::Type::TlRecord:   toggleCapture(); return;
    case MenuClick::Type::TlShow:     setTimelineOn(c.b != 0); return;
    default: break;
  }
  if (c.type == MenuClick::Type::TensionWindow) {
    Settings::TENSION_ENABLED.value = c.b ? 1 : 0;
    Settings::saveConfig();
    return;
  }
  // A click in a popped-out window acts on that window's page, then the main
  // menu goes back to where it was.
  if (c.page >= 0 && (c.type == MenuClick::Type::Row || c.type == MenuClick::Type::SetChoice)) {
    const size_t saved_page = current_page;
    const int saved_cursor = cursor_position;
    const bool saved_showing = is_showing;
    current_page = static_cast<size_t>(c.page);
    is_showing = true;  // the timeline works with the F5 menu closed
    MenuClick inner = c;
    inner.page = -1;
    applyClick(inner);
    current_page = saved_page;
    cursor_position = saved_cursor;
    is_showing = saved_showing;
    return;
  }
  auto page = static_cast<MenuPage>(current_page);
  if (c.type == MenuClick::Type::Move) {  // dragged in ImGui: keep it there
    Settings::MENU_CENTER_X_RATIO.value = std::clamp(c.a / 10000.0, 0.0, 1.0);
    Settings::MENU_CENTER_Y_RATIO.value = std::clamp(c.b / 10000.0, 0.0, 1.0);
    Settings::saveConfig();
    return;
  }
  if (c.type == MenuClick::Type::OpenNav) {
    if (c.a < 0 || static_cast<size_t>(c.a) >= ROOT_PAGE_COUNT) return;
    current_page = static_cast<size_t>(MenuPage::Root);
    cursor_position = c.a;
    updateInner(PressedKeys{false, false, false, false, false, true, false});  // enter that page
    return;
  }
  if (page == MenuPage::Root) return;
  if (c.a < 0 || static_cast<size_t>(c.a) >= rowCountForPage(page)) return;
  cursor_position = c.a;
  if (c.type == MenuClick::Type::Row) {
    updateInner(PressedKeys{false, false, false, false, false, c.b == 1, c.b == 0});
    return;
  }
  // SetChoice: jump straight to that option
  if (page == MenuPage::Recordings) {
    if (c.a == 1) g_rec_slot = std::clamp(c.b, 0, recordings::kSlotCount - 1);
    if (c.a == 8) g_cap_side = c.b ? 1 : 0;
    if (c.a == 5) g_rec_side = c.b == 0 ? recordings::kDirRight : recordings::kDirLeft;
    return;
  }
  if (isSettingsPage(page) && c.a >= 1) {
    const auto rows = settingsRowsForPage(page);
    const auto& row = rows[static_cast<size_t>(c.a - 1)];
    if (row.action == SettingsAction::TakeoverToggle) {
      binds::use_mod_binds = c.b ? 1 : 0;
      Settings::saveConfig();
      return;
    }
    auto* setting = row.setting;
    if (setting && c.b >= 0 && static_cast<size_t>(c.b) < setting->display.count) {
      setting->value = static_cast<size_t>(c.b);
      Settings::saveConfig();
    }
  }
}

void ModMenu::buildView() {
  MenuView v;
  v.showing = is_showing;
  const auto page = static_cast<MenuPage>(current_page);
  for (size_t i = 0; i < ROOT_PAGE_COUNT; i++) v.nav_items.push_back(clean(ROOT_PAGES[i].label));
  v.nav_focus = (page == MenuPage::Root);
  // On the root page the right pane previews the highlighted page.
  const MenuPage shown = v.nav_focus ? ROOT_PAGES[static_cast<size_t>(cursor_position) % ROOT_PAGE_COUNT].page : page;
  v.nav = static_cast<int>(v.nav_focus ? static_cast<size_t>(cursor_position) : rootIndexForPage(isLayoutSubPage(page) ? MenuPage::Layout : page));
  // The ImGui menu has no Back row: the side menu entry stands in for it.
  v.back_focus = !v.nav_focus && cursor_position == 0;
  v.cursor = v.nav_focus ? -1 : cursor_position - 1;
  v.title = clean(pageLabel(shown));
  for (auto& ch : v.title) ch = static_cast<char>(ch == '-' ? ' ' : ch);
  { const auto a = v.title.find_first_not_of(' '); const auto b = v.title.find_last_not_of(' ');
    v.title = a == std::string::npos ? "" : v.title.substr(a, b - a + 1); }
  v.x_ratio = static_cast<float>(Settings::MENU_CENTER_X_RATIO.value);
  v.y_ratio = static_cast<float>(Settings::MENU_CENTER_Y_RATIO.value);
  v.extra_width = static_cast<float>(Settings::MENU_WIDTH_EXTRA.value);

  // Rows of one page; used for the right pane and for every popped-out window.
  auto buildRows = [&](MenuPage shown, std::vector<MenuRowView>& out, std::string& status) {
  auto row = [&](MenuRowView::Kind k, const std::string& label, const std::string& value = {}) {
    MenuRowView r; r.kind = k; r.label = label; r.value = value; out.push_back(r);
  };

  if (isSettingsPage(shown)) {
    for (const auto& def : settingsRowsForPage(shown)) {
      MenuRowView r; r.label = clean(def.label); r.section = utf8(def.section);
      if (def.capture >= 0) {
        const bool game = def.capture >= binds::GAME_BASE;
        const int id = game ? def.capture - binds::GAME_BASE : def.capture;
        r.kind = MenuRowView::Kind::Bind;
        r.capturing = binds::capturing == def.capture;
        r.value = utf8(binds::keyName(game ? binds::gkey[id] : binds::key[id]));
        r.choices = {utf8(binds::padName(game ? binds::gpad[id] : binds::pad[id]))};
      } else if (def.action == SettingsAction::TakeoverToggle) {
        r.kind = MenuRowView::Kind::Choice;
        r.choices = {"Disabled", "Enabled"}; r.choice = binds::use_mod_binds ? 1 : 0;
      } else if (const auto* s = def.setting) {
        r.kind = MenuRowView::Kind::Choice;
        for (size_t c = 0; c < s->display.count; c++) r.choices.push_back(clean(s->display.values[c]));
        r.choice = static_cast<int>(s->value);
      } else if (const auto* f = def.number) {
        r.kind = MenuRowView::Kind::Number;
        if (f->id_.rfind("ui_", 0) == 0) {
          r.value = std::to_string(static_cast<int>(std::lround(f->value * 100))) + "%";
        } else r.value = utf8(formatMenuNumber(*f));
      } else {
        r.kind = MenuRowView::Kind::Action;
        r.value = def.action == SettingsAction::PickColor ? "Pick" : "Reset";
        if (def.action == SettingsAction::PickFont) {
          std::string f = Settings::overlayFont();
          f = f.empty() ? "Barlow" : f.substr(0, f.find_last_of('.'));
          r.value = f.size() > 22 ? f.substr(0, 21) + "..." : f;
        }
      }
      out.push_back(std::move(r));
    }
  } else if (shown == MenuPage::Recordings) {
    const std::wstring slot_no = std::to_wstring(g_rec_slot + 1);
    { MenuRowView r; r.kind = MenuRowView::Kind::Choice; r.label = "Slot";
      for (int i = 1; i <= recordings::kSlotCount; i++) r.choices.push_back(std::to_string(i));
      r.choice = g_rec_slot; r.value = utf8(recordings::describeSlot(g_rec_slot)); out.push_back(r); }
    row(MenuRowView::Kind::Action, utf8(L"Save slot " + slot_no + L" to a file"), "Save");
    row(MenuRowView::Kind::Number, "File", g_rec_files.empty() ? "no .txt files" : utf8(fitValue(g_rec_files[g_rec_file_idx].stem().wstring(), 40)));
    row(MenuRowView::Kind::Action, "Browse for a file", "Browse");
    { MenuRowView r; r.kind = MenuRowView::Kind::Choice; r.label = "Player side";
      r.choices = {"1P", "2P"}; r.choice = g_rec_side == recordings::kDirLeft ? 1 : 0; out.push_back(r); }
    row(MenuRowView::Kind::Action, utf8(L"Load file into slot " + slot_no), "Load");
    row(MenuRowView::Kind::Action, "Stream to dummy",
        (recordings::streamActive() || recordings::streamArmed())
          ? "Stop (" + std::to_string(recordings::streamPos()) + "/" + std::to_string(recordings::streamLen()) + ")" : "Start");
    { MenuRowView r; r.kind = MenuRowView::Kind::Choice; r.label = "Capture side";
      r.choices = {"P1", "P2"}; r.choice = g_cap_side; out.push_back(r); }
    row(MenuRowView::Kind::Action, "Capture from replay",
        ReplayManager::instance().captureActive() ? utf8(L"Stop (" + secondsText(ReplayManager::instance().captureFrames()) + L" / 40 s)") : std::string("Start"));
    status = utf8(g_rec_status);
  } else if (shown == MenuPage::Layout) {
    for (size_t i = 0; i < LAYOUT_SUB_COUNT; i++) row(MenuRowView::Kind::Open, utf8(LAYOUT_SUBS[i].label), "Open");
    row(MenuRowView::Kind::Action, "Reset all to defaults", "Reset");
  } else if (isLayoutSubPage(shown)) {
    size_t count = 0;
    const LayoutRowDef* rows = layoutRowsForPage(shown, count);
    for (size_t i = 0; rows && i < count; i++)
      row(MenuRowView::Kind::Number, utf8(rows[i].label), utf8(formatMenuFloat(Settings::float_settings[rows[i].float_idx]->value)));
  }

  if (!isSettingsPage(shown))
    for (size_t i = 0; i < out.size(); i++) out[i].section = utf8(sectionForRow(shown, i));
  };
  buildRows(shown, v.rows, v.status);
  v.page = static_cast<int>(shown);
  v.tension_page = shown == MenuPage::Tension;
  v.poppable = isSettingsPage(shown) || shown == MenuPage::Recordings;
  v.tension_on = Settings::TENSION_ENABLED.value != 0;
  v.meters_right = Settings::TENSION_SIDE.value == 1;
  v.meters_snap = Settings::METERS_SNAP.value != 0;
  v.hide_window_decorations = Settings::UI_HIDE_WINDOW_DECORATIONS.value != 0;
  {
    static constexpr unsigned kAccents[6] = {0xFF0000, 0x2B8CFF, 0x00C8AA, 0xA05AFF, 0xFFBE00, 0xE6E6EB};
    auto byte = [](double x) { return static_cast<unsigned>(std::lround(std::clamp(x, 0.0, 1.0) * 255.0)); };
    v.custom_rgb = (byte(Settings::UI_CUSTOM_R.value) << 16) | (byte(Settings::UI_CUSTOM_G.value) << 8) | byte(Settings::UI_CUSTOM_B.value);
    v.accent_rgb = Settings::UI_ACCENT.value < 6 ? kAccents[Settings::UI_ACCENT.value] : v.custom_rgb;
    v.opacity = static_cast<float>(Settings::UI_OPACITY.value);
    v.picker_seq = g_picker_seq;
  }
  v.mode = Timeline::instance().mode();
  v.timeline_on = timelineOn();
  v.paused = Timeline::instance().isPaused();
  v.recording = ReplayManager::instance().captureActive();
  v.rec_frames = ReplayManager::instance().captureFrames();
  v.rec_status = utf8(g_rec_status);
  if (v.mode != 0) {
    static bool scanned = false;  // the timeline lists files before the Recordings page is ever opened
    if (!scanned) { scanned = true; refreshRecordingFiles(); }
    std::string unused;
    buildRows(MenuPage::Recordings, v.rec_rows, unused);
  }
  const unsigned mask = g_popped;
  for (size_t i = 0; i < 32; i++) {
    if (!(mask & (1u << i))) continue;
    MenuPanelView pv;
    pv.page = static_cast<int>(i);
    pv.title = clean(pageLabel(static_cast<MenuPage>(i)));
    for (auto& ch : pv.title) ch = static_cast<char>(ch == '-' ? ' ' : ch);
    { const auto a2 = pv.title.find_first_not_of(' '); const auto b2 = pv.title.find_last_not_of(' ');
      pv.title = a2 == std::string::npos ? "" : pv.title.substr(a2, b2 - a2 + 1); }
    buildRows(static_cast<MenuPage>(i), pv.rows, pv.status);
    v.panels.push_back(std::move(pv));
  }

  std::lock_guard<std::mutex> lock(g_view_mutex);
  g_view = std::move(v);
}

bool ModMenu::barEnabled() const { return Settings::FRAMEBAR.value; }
bool ModMenu::hitboxEnabled() const { return Settings::HITBOXES.value; }
bool ModMenu::fadeEnabled() const { return Settings::FADE.value; }
bool ModMenu::delimEnabled() const { return Settings::DELIM.value; }
bool ModMenu::cancelEnabled() const { return false; }
bool ModMenu::dashEnabled() const { return Settings::SHOW_DASH_FRAMES.value; }
bool ModMenu::crossupEnabled() const { return Settings::CROSSUP_ENABLED.value; }
int ModMenu::rewindDelaySeconds() const { return static_cast<int>(Settings::REWIND_DELAY_SECONDS.value); }

int ModMenu::pauseType() const { return Settings::PAUSE_TYPE.value; }

TensionOptions ModMenu::getTensionOptions() const {
  return TensionOptions{
      static_cast<bool>(Settings::TENSION_ENABLED.value),
      static_cast<PlayerDisplayMode>(Settings::TENSION_GAIN.value),
      static_cast<bool>(Settings::TENSION_ADVANTAGE.value),
      static_cast<bool>(Settings::TENSION_ADV_CHANGE.value),
      static_cast<PlayerDisplayMode>(Settings::TENSION_BURST.value),
      static_cast<PlayerDisplayMode>(Settings::TENSION_PULSE.value),
      static_cast<PlayerDisplayMode>(Settings::TENSION_PENALTY.value),
      static_cast<PlayerDisplayMode>(Settings::TENSION_CURRENT.value),
      static_cast<PlayerDisplayMode>(Settings::WALL_DAMAGE.value),
      Settings::TENSION_SIDE.value == 1,
      static_cast<int>(Settings::METERS_DECIMALS.value),
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

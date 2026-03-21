#include "combo_menu.h"
#include "combo_log.h"

#include <algorithm>
#include <set>
#include <sstream>

// sRGB to linear conversion (same as menu.cpp)
static float sRGBToLinear(int pc) {
  float c = pc / 255.0f;
  return c > 0.04045f ? powf(c * (1.0f / 1.055f) + 0.0521327f, 2.4f) : c * (1.0f / 12.92f);
}

// Drawing constants (matching menu.cpp style)
namespace {
  constexpr double OPTION_HEIGHT = 52;
  constexpr double COMBO_ROW_HEIGHT = 72;  // taller rows for two-line combo entries
  constexpr double OPTION_SPACING = 6;
  constexpr double LABEL_SCALE = 1.5;
  constexpr double VALUE_SCALE = 1.5;
  constexpr double NOTATION_SCALE = 1.1;
  constexpr double NOTES_SCALE = 0.9;
  constexpr size_t MAX_NOTATION_CHARS = 120;
  constexpr size_t MAX_NOTES_CHARS = 80;

  FLinearColor CURSOR_BG{sRGBToLinear(255), 0.0f, 0.0f, 0.15f};
  FLinearColor CURSOR_STRIPE{sRGBToLinear(255), sRGBToLinear(60), sRGBToLinear(60), 0.60f};
  FLinearColor ACCENT_CLR{sRGBToLinear(255), 0.0f, 0.0f, 1.0f};
  FLinearColor NOTES_COLOR{0.45f, 0.55f, 0.45f, 1.0f};
  FLinearColor FILTER_COLOR{0.55f, 0.45f, 0.65f, 1.0f};
  FLinearColor TAG_BG{sRGBToLinear(0), sRGBToLinear(180), sRGBToLinear(220), 0.10f};
  FLinearColor TAG_BORDER{sRGBToLinear(0), sRGBToLinear(180), sRGBToLinear(220), 0.30f};
  FLinearColor DIVIDER{1.0f, 1.0f, 1.0f, 0.03f};
  constexpr double ACCENT_W = 3.0;
  constexpr double STRIPE_H = 2.0;
}

// ── Helpers ─────────────────────────────────────────────────────────

std::wstring ComboMenu::toWide(const std::string& s) const {
  std::wstring result(s.begin(), s.end());
  return result;
}

std::wstring ComboMenu::getCharacterDisplayName(const std::string& id) const {
  for (auto& ch : db_.registry.characters) {
    if (ch.id == id) return toWide(ch.name);
  }
  return toWide(id);  // fallback to raw ID
}

const std::vector<ComboEntry>* ComboMenu::currentCombos() const {
  auto cit = db_.characters.find(current_character_);
  if (cit == db_.characters.end()) return nullptr;
  auto vit = cit->second.versions.find(current_version_);
  if (vit == cit->second.versions.end()) return nullptr;
  auto sit = vit->second.find(current_starter_);
  if (sit == vit->second.end()) return nullptr;
  return &sit->second.combos;
}

// ── Init ────────────────────────────────────────────────────────────

void ComboMenu::init(const std::filesystem::path& data_dir) {
  COMBO_LOG("ComboMenu::init loading from " + data_dir.string());

  auto result = loadComboDatabase(data_dir);
  if (!result.success) {
    status_text_ = L"Load failed: " + toWide(result.error_message);
    COMBO_LOG("ComboMenu::init FAILED: " + result.error_message);
    return;
  }

  db_ = std::move(result.database);
  loaded_ = true;

  // Build character list — only characters that have data, in registry order
  character_ids_.clear();
  for (auto& ch : db_.registry.characters) {
    if (db_.characters.count(ch.id) && !db_.characters.at(ch.id).empty()) {
      character_ids_.push_back(ch.id);
    }
  }

  // If only one character, skip straight to versions
  if (character_ids_.size() == 1) {
    enterCharacter(0);
  } else {
    page_ = Page::Characters;
    cursor_ = 0;
  }

  status_text_ = L"Loaded " + std::to_wstring(character_ids_.size()) + L" character(s)";
  COMBO_LOG("ComboMenu::init OK, " + std::to_string(character_ids_.size()) + " character(s)");
}

void ComboMenu::reload() {
  status_text_ = L"Use game restart to reload";
}

// ── Row count / height ──────────────────────────────────────────────

size_t ComboMenu::rowCount() const {
  if (!loaded_) return 1;

  switch (page_) {
    case Page::Characters:
      return character_ids_.size();
    case Page::Versions:
      return version_keys_.size();
    case Page::Starters:
      return current_starters_.size();
    case Page::ComboList: {
      size_t visible = comboPageEnd() - comboPageStart();
      size_t extra = 0;
      extra += 1;  // filter row
      if (comboPageCount() > 1) extra += 1;  // page indicator
      return visible + extra;
    }
  }
  return 0;
}

double ComboMenu::contentHeight() const {
  if (!loaded_) return OPTION_HEIGHT + OPTION_SPACING;

  if (page_ != Page::ComboList) {
    return rowCount() * (OPTION_HEIGHT + OPTION_SPACING);
  }

  // ComboList: filter row (normal) + combo entries (tall) + optional page indicator (normal)
  double h = OPTION_HEIGHT + OPTION_SPACING;  // filter row
  size_t combo_rows = comboPageEnd() - comboPageStart();
  h += combo_rows * (COMBO_ROW_HEIGHT + OPTION_SPACING);
  if (comboPageCount() > 1) h += OPTION_HEIGHT + OPTION_SPACING;
  return h;
}

// ── Combo pagination (uses filtered list) ───────────────────────────

size_t ComboMenu::visibleComboCount() const {
  if (active_filter_idx_ > 0 && !filtered_indices_.empty()) {
    return filtered_indices_.size();
  }
  if (active_filter_idx_ > 0 && filtered_indices_.empty()) {
    return 0;  // filter active but no matches
  }
  auto* combos = currentCombos();
  return combos ? combos->size() : 0;
}

size_t ComboMenu::comboPageCount() const {
  size_t total = visibleComboCount();
  if (total == 0) return 1;
  return (total + COMBOS_PER_PAGE - 1) / COMBOS_PER_PAGE;
}

size_t ComboMenu::comboPageStart() const {
  return combo_page_ * COMBOS_PER_PAGE;
}

size_t ComboMenu::comboPageEnd() const {
  size_t end = comboPageStart() + COMBOS_PER_PAGE;
  size_t total = visibleComboCount();
  return (end > total) ? total : end;
}

// ── Filtering ───────────────────────────────────────────────────────

void ComboMenu::rebuildFilter() {
  filtered_indices_.clear();
  available_tags_.clear();
  active_filter_idx_ = 0;

  auto* combos = currentCombos();
  if (!combos) return;

  // Collect all unique tags from this starter's combos
  std::set<std::string> tag_set;
  for (auto& combo : *combos) {
    for (auto& tag : combo.tags) {
      tag_set.insert(tag);
    }
  }
  available_tags_.assign(tag_set.begin(), tag_set.end());
}

void ComboMenu::cycleFilter() {
  if (available_tags_.empty()) return;

  active_filter_idx_ = (active_filter_idx_ + 1) % (available_tags_.size() + 1);
  combo_page_ = 0;

  // Rebuild filtered indices
  filtered_indices_.clear();
  if (active_filter_idx_ == 0) return;  // ALL — no filtering

  auto* combos = currentCombos();
  if (!combos) return;

  const auto& tag = available_tags_[active_filter_idx_ - 1];
  for (size_t i = 0; i < combos->size(); i++) {
    for (auto& t : (*combos)[i].tags) {
      if (t == tag) {
        filtered_indices_.push_back(i);
        break;
      }
    }
  }
}

// ── Update (input handling) ─────────────────────────────────────────

bool ComboMenu::update(bool up, bool down, bool left, bool right) {
  if (!loaded_) return left;

  const size_t count = rowCount();
  if (count == 0) return left;

  if (up && cursor_ > 0) cursor_--;
  else if (up && cursor_ == 0) cursor_ = count - 1;
  if (down) cursor_ = (cursor_ + 1) % count;

  if (right) {
    switch (page_) {
      case Page::Characters:
        if (cursor_ < character_ids_.size()) enterCharacter(cursor_);
        break;
      case Page::Versions:
        if (cursor_ < version_keys_.size()) enterVersion(cursor_);
        break;
      case Page::Starters:
        if (cursor_ < current_starters_.size()) enterStarter(cursor_);
        break;
      case Page::ComboList: {
        // Row 0 = filter row
        if (cursor_ == 0) {
          cycleFilter();
          break;
        }

        size_t combo_local = cursor_ - 1;  // offset by filter row
        size_t page_combo_count = comboPageEnd() - comboPageStart();

        if (combo_local < page_combo_count) {
          // On a combo entry — request video playback
          auto link = selectedVideoLink();
          if (!link.empty()) {
            video_requested_ = true;
          }
        } else if (comboPageCount() > 1) {
          // On the page indicator row
          combo_page_ = (combo_page_ + 1) % comboPageCount();
          cursor_ = 1;  // go to first combo after filter row
        }
        break;
      }
    }
  }

  if (left) {
    if (page_ == Page::Characters) {
      return true;  // exit to main menu
    }
    // Single-char shortcut: if on Versions and only 1 character, exit to main menu
    if (page_ == Page::Versions && character_ids_.size() == 1) {
      return true;
    }
    if (page_ == Page::ComboList) {
      // Check if on page indicator row
      size_t page_combo_count = comboPageEnd() - comboPageStart();
      size_t combo_local = cursor_ - 1;  // offset by filter row
      if (cursor_ > 0 && comboPageCount() > 1 && combo_local == page_combo_count) {
        // Navigate pages backward
        combo_page_ = (combo_page_ == 0) ? comboPageCount() - 1 : combo_page_ - 1;
        cursor_ = 1;
      } else {
        goBack();
      }
    } else {
      goBack();
    }
  }

  return false;
}

// ── Draw ────────────────────────────────────────────────────────────

void ComboMenu::draw(DrawContext& tool, double menu_left, double menu_width,
                     double label_left, double value_left, double& cy,
                     int cursor_position, size_t first_custom_row) {
  if (!loaded_) {
    tool.drawOutlinedText(label_left, cy + 8.0, FString(status_text_.c_str()), LABEL_SCALE);
    cy += OPTION_HEIGHT + OPTION_SPACING;
    return;
  }

  auto drawRow = [&](size_t local_idx, const std::wstring& label, const std::wstring& value,
                     double lscale = LABEL_SCALE, double vscale = VALUE_SCALE) {
    const bool sel = (local_idx == cursor_);
    if (sel) {
      tool.drawRect(menu_left, cy, menu_width, OPTION_HEIGHT, CURSOR_BG);
      tool.drawRect(menu_left, cy, menu_width, STRIPE_H, CURSOR_STRIPE);
      tool.drawRect(menu_left, cy, ACCENT_W, OPTION_HEIGHT, ACCENT_CLR);
    }
    tool.drawOutlinedText(label_left, cy + 8.0, FString(label.c_str()), lscale);
    if (!value.empty())
      tool.drawOutlinedText(value_left, cy + 8.0, FString(value.c_str()), vscale);
    cy += OPTION_HEIGHT + OPTION_SPACING;
  };

  switch (page_) {
    case Page::Characters: {
      for (size_t i = 0; i < character_ids_.size(); i++) {
        auto display = getCharacterDisplayName(character_ids_[i]);
        auto& char_data = db_.characters.at(character_ids_[i]);
        auto ver_count = char_data.versions.size();
        auto info = std::to_wstring(ver_count) + L" version(s)";
        drawRow(i, display, L"< " + info + L" >");
      }
      break;
    }

    case Page::Versions: {
      for (size_t i = 0; i < version_keys_.size(); i++) {
        auto label = toWide(version_keys_[i]);
        auto& char_data = db_.characters.at(current_character_);
        auto& starters = char_data.versions.at(version_keys_[i]);
        auto info = std::to_wstring(starters.size()) + L" starters";
        drawRow(i, label, L"< " + info + L" >");
      }
      break;
    }

    case Page::Starters: {
      for (size_t i = 0; i < current_starters_.size(); i++) {
        auto label = toWide(current_starters_[i]);
        auto& char_data = db_.characters.at(current_character_);
        auto it = char_data.versions.at(current_version_).find(current_starters_[i]);
        size_t count = (it != char_data.versions.at(current_version_).end()) ? it->second.combos.size() : 0;
        auto info = std::to_wstring(count) + L" combo" + (count != 1 ? L"s" : L"");
        drawRow(i, label, L"< " + info + L" >");
      }
      break;
    }

    case Page::ComboList: {
      auto* combos = currentCombos();
      if (!combos) break;

      // ── Filter row (row 0) ──
      std::wstring filter_label = L"Filter:";
      std::wstring filter_value;
      if (available_tags_.empty()) {
        filter_value = L"no tags";
      } else if (active_filter_idx_ == 0) {
        filter_value = L"< ALL >";
      } else if (active_filter_idx_ - 1 < available_tags_.size()) {
        filter_value = L"< " + toWide(available_tags_[active_filter_idx_ - 1]) + L" >";
      }
      drawRow(0, filter_label, filter_value);

      // ── Combo entries ──
      size_t start = comboPageStart();
      size_t end = comboPageEnd();
      size_t total = visibleComboCount();

      for (size_t vi = start; vi < end; vi++) {
        size_t draw_row_idx = (vi - start) + 1;  // +1 for filter row

        // Map through filter if active
        size_t actual_idx = vi;
        if (active_filter_idx_ > 0 && vi < filtered_indices_.size()) {
          actual_idx = filtered_indices_[vi];
        }
        if (actual_idx >= combos->size()) break;

        auto& combo = (*combos)[actual_idx];
        auto notation = toWide(combo.notation);
        auto notes = toWide(combo.notes);

        // Two-line combo row
        const bool sel = (draw_row_idx == cursor_);
        if (sel) {
          tool.drawRect(menu_left, cy, menu_width, COMBO_ROW_HEIGHT, CURSOR_BG);
          tool.drawRect(menu_left, cy, menu_width, STRIPE_H, CURSOR_STRIPE);
          tool.drawRect(menu_left, cy, ACCENT_W, COMBO_ROW_HEIGHT, ACCENT_CLR);
        }
        // Row divider
        tool.drawRect(menu_left, cy + COMBO_ROW_HEIGHT, menu_width, 1.0, DIVIDER);

        // Line 1: combo number + notation
        auto line1 = std::to_wstring(actual_idx + 1) + L". " + notation;
        if (line1.size() > MAX_NOTATION_CHARS)
          line1 = line1.substr(0, MAX_NOTATION_CHARS - 3) + L"...";
        tool.drawOutlinedText(label_left, cy + 4.0, FString(line1.c_str()), NOTATION_SCALE);

        // Line 2: notes + contributor
        std::wstring line2 = notes;
        if (!combo.contributor.empty()) {
          if (!line2.empty()) line2 += L"  |  ";
          line2 += L"by " + toWide(combo.contributor);
        }
        if (!line2.empty()) {
          if (line2.size() > MAX_NOTES_CHARS)
            line2 = line2.substr(0, MAX_NOTES_CHARS - 3) + L"...";
          tool.drawOutlinedText(label_left + 24.0, cy + 32.0, FString(line2.c_str()), NOTES_SCALE);
        }

        cy += COMBO_ROW_HEIGHT + OPTION_SPACING;
      }

      // ── Page indicator ──
      if (comboPageCount() > 1) {
        size_t page_row_idx = (end - start) + 1;  // +1 for filter row
        drawRow(page_row_idx,
                L"Page " + std::to_wstring(combo_page_ + 1) + L"/" +
                std::to_wstring(comboPageCount()),
                L"< L/R >");
      }
      break;
    }
  }
}

// ── Video request ───────────────────────────────────────────────────

bool ComboMenu::consumeVideoRequest() {
  if (video_requested_) {
    video_requested_ = false;
    return true;
  }
  return false;
}

std::string ComboMenu::selectedVideoLink() const {
  if (!loaded_ || page_ != Page::ComboList) return {};
  if (cursor_ == 0) return {};  // on filter row

  auto* combos = currentCombos();
  if (!combos) return {};

  size_t combo_local = cursor_ - 1;  // offset by filter row
  size_t start = comboPageStart();
  size_t vi = start + combo_local;

  if (vi >= visibleComboCount()) return {};

  // Map through filter
  size_t actual_idx = vi;
  if (active_filter_idx_ > 0 && vi < filtered_indices_.size()) {
    actual_idx = filtered_indices_[vi];
  }
  if (actual_idx >= combos->size()) return {};

  return (*combos)[actual_idx].link;
}

// ── Navigation ──────────────────────────────────────────────────────

void ComboMenu::enterCharacter(size_t idx) {
  if (idx >= character_ids_.size()) return;
  current_character_ = character_ids_[idx];

  auto& char_data = db_.characters.at(current_character_);
  version_keys_.clear();

  // Prioritize V148 first, then PREVIOUS, then alphabetical
  if (char_data.versions.count("V148")) version_keys_.push_back("V148");
  if (char_data.versions.count("PREVIOUS")) version_keys_.push_back("PREVIOUS");
  for (auto& [key, _] : char_data.versions) {
    if (key != "V148" && key != "PREVIOUS") {
      version_keys_.push_back(key);
    }
  }

  page_ = Page::Versions;
  cursor_ = 0;
  COMBO_LOG("Entered character '" + current_character_ + "' with " +
            std::to_string(version_keys_.size()) + " versions");
}

void ComboMenu::enterVersion(size_t idx) {
  if (idx >= version_keys_.size()) return;
  current_version_ = version_keys_[idx];

  // Build ordered starters list
  current_starters_.clear();
  auto& order = starterOrderFor(current_character_, current_version_);
  auto& char_data = db_.characters.at(current_character_);
  auto& starters_map = char_data.versions.at(current_version_);

  if (!order.empty()) {
    for (auto& name : order) {
      if (starters_map.count(name)) current_starters_.push_back(name);
    }
    for (auto& [key, _] : starters_map) {
      if (std::find(order.begin(), order.end(), key) == order.end()) {
        current_starters_.push_back(key);
      }
    }
  } else {
    for (auto& [key, _] : starters_map) current_starters_.push_back(key);
  }

  page_ = Page::Starters;
  cursor_ = 0;
  COMBO_LOG("Entered version '" + current_version_ + "' with " +
            std::to_string(current_starters_.size()) + " starters");
}

void ComboMenu::enterStarter(size_t idx) {
  if (idx >= current_starters_.size()) return;
  current_starter_ = current_starters_[idx];
  page_ = Page::ComboList;
  cursor_ = 0;
  combo_page_ = 0;
  rebuildFilter();
  COMBO_LOG("Entered starter '" + current_starter_ + "' with " +
            std::to_string(visibleComboCount()) + " combos, " +
            std::to_string(available_tags_.size()) + " tags");
}

void ComboMenu::goBack() {
  switch (page_) {
    case Page::Characters:
      break;
    case Page::Versions:
      if (character_ids_.size() == 1) {
        // Single char — can't go back to characters, handled by update() returning true
        break;
      }
      page_ = Page::Characters;
      cursor_ = 0;
      break;
    case Page::Starters:
      page_ = Page::Versions;
      cursor_ = 0;
      break;
    case Page::ComboList:
      page_ = Page::Starters;
      cursor_ = 0;
      active_filter_idx_ = 0;
      filtered_indices_.clear();
      break;
  }
}

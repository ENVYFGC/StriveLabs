#pragma once

#include "combo_data.h"
#include "draw_utils.h"
#include <set>
#include <string>
#include <vector>

class ComboMenu {
public:
  enum class Page { Characters, Versions, Starters, ComboList };

  void init(const std::filesystem::path& data_dir);
  void reload();

  // Input: up/down move cursor, right enters, left goes back
  // Returns true if the combo menu wants to go back past the root (exit to main menu)
  bool update(bool up, bool down, bool left, bool right);

  // Draw the current page contents (called after the Back row is drawn)
  void draw(DrawContext& tool, double menu_left, double menu_width,
            double label_left, double value_left, double& cy,
            int cursor_position, size_t first_custom_row);

  // How many custom rows the combo menu needs (excluding Back)
  size_t rowCount() const;

  // Total draw height of combo menu content in draw-space units
  double contentHeight() const;

  // Current status text for display
  const std::wstring& statusText() const { return status_text_; }

  // Get the YouTube link of the currently selected combo (empty if none)
  std::string selectedVideoLink() const;

  // Returns true once after the user pressed right on a combo entry (consumed on read)
  bool consumeVideoRequest();

  // Current page (for external status display)
  Page currentPage() const { return page_; }

private:
  ComboDatabase db_;
  bool loaded_ = false;
  Page page_ = Page::Characters;
  size_t cursor_ = 0;
  size_t combo_page_ = 0;  // pagination for combo list

  std::wstring status_text_ = L"Not loaded";

  // Character navigation
  std::vector<std::string> character_ids_;    // ordered list of chars with data
  std::string current_character_;

  // Version/starter navigation
  std::vector<std::string> version_keys_;
  std::string current_version_;
  std::vector<std::string> current_starters_;
  std::string current_starter_;

  // Tag filtering
  std::vector<std::string> available_tags_;   // tags found in current starter
  size_t active_filter_idx_ = 0;              // 0 = ALL
  std::vector<size_t> filtered_indices_;      // combo indices matching filter

  static constexpr size_t COMBOS_PER_PAGE = 7;

  // Combo pagination (uses filtered list when filter active)
  size_t visibleComboCount() const;
  size_t comboPageCount() const;
  size_t comboPageStart() const;
  size_t comboPageEnd() const;

  // Navigation
  void enterCharacter(size_t idx);
  void enterVersion(size_t idx);
  void enterStarter(size_t idx);
  void goBack();

  // Filtering
  void rebuildFilter();
  void cycleFilter();

  // Helpers
  std::wstring toWide(const std::string& s) const;
  std::wstring getCharacterDisplayName(const std::string& id) const;
  const std::vector<ComboEntry>* currentCombos() const;

  bool video_requested_ = false;
};

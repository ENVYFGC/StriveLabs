#pragma once

#include "trial_core.h"
#include "trial_store.h"
#include "draw_utils.h"

#include <memory>
#include <string>
#include <vector>
#include <filesystem>

namespace menu {
struct TrialMenuAction {
  enum Value {
    None,
    StartTrial,
    StopTrial,
    ToggleRecordSteps,
    SaveTrial,
    DeleteTrial,
    ReloadTrials
  };
};

struct TrialMenuView {
  size_t trial_count = 1;
  std::wstring selected_label = L"<New Trial>";
  std::wstring status_label = L"Idle";
  bool steps_recording = false;
};

struct TrialMenuState {
  bool enabled = false;
  int retry_delay_frames = 30;
  size_t selected_trial_index = 0;
  std::wstring trial_name = L"";
  TrialMenuAction::Value action = TrialMenuAction::None;
};
}  // namespace menu

class TrialManager {
public:
  static TrialManager& instance();

  void initialize();
  void resetRuntime();
  void onRoundReset();

  void updatePreBattle();
  void updatePostBattle(std::string move_name, bool combo_alive);
  void updatePostBattleFromEngine(asw_engine* engine);
  void draw();

  bool consumeSetupCaptureRequest(std::filesystem::path& blob_path);
  bool consumeSetupRestoreRequest(std::filesystem::path& blob_path);

  void onSetupCaptured(bool success, std::filesystem::path blob_path);
  void onSetupRestored(bool success);

  void consumeTrialMenuState(const menu::TrialMenuState& state);
  menu::TrialMenuView getTrialMenuView() const;

private:
  TrialManager();

  void reloadCatalog();
  void updateMenuView();
  void handleMenuAction();
  void stopCurrentTrial(const std::wstring& reason);
  void ensureLoaded();

  TrialDefinition selectedOrDraft();
  bool hasActiveTrial() const;

  std::unique_ptr<TrialStore> store_;
  std::vector<TrialDefinition> catalog_;
  TrialDefinition draft_trial_;
  TrialDefinition current_trial_;
  std::unique_ptr<TrialSequenceTracker> tracker_;

  menu::TrialMenuState menu_state_;
  menu::TrialMenuView menu_view_;

  TrialStatus runtime_status_ = TrialStatus::Idle;
  std::wstring status_text_ = L"Idle";
  int retry_countdown_ = 0;
  bool playback_active_ = false;
  bool steps_recording_ = false;
  size_t playback_frame_index_ = 0;
  std::vector<unsigned short> playback_frames_;

  bool initialized_ = false;
  bool catalog_load_failed_ = false;

  std::filesystem::path pending_setup_capture_;
  std::filesystem::path pending_setup_restore_;
  bool awaiting_restore_confirmation_ = false;
};

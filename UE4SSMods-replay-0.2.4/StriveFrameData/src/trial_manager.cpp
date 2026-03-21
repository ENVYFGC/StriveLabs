#include "trial_manager.h"

#include <UE4SSProgram.hpp>
#include <UnrealDef.hpp>
#include <arcsys.h>
#include <bbscript.h>

#include <algorithm>
#include <sstream>

namespace {
FLinearColor color_white{1.f, 1.f, 1.f, 1.f};
FLinearColor color_bg{0.01f, 0.01f, 0.03f, 0.85f};

std::string read_trial_move_name(const asw_player* p) {
  if (!p) return {};
  const char* state = p->get_BB_state();
  if (!state || !state[0]) return {};
  std::string_view sv{state};
  // Filter out idle/movement states — these aren't meaningful moves
  if (sv == "CmnActStand" || sv == "CmnActCrouch" || sv == "CmnActJump" ||
      sv == "CmnActJumpLanding" || sv == "CmnActLandingStiff" ||
      sv == "CmnActFWalk" || sv == "CmnActBWalk" ||
      sv == "CmnActFDash" || sv == "CmnActFDashStop" ||
      sv == "CmnActBDash" || sv == "CmnActBDashStop" ||
      sv == "CmnActStandTurnA" || sv == "CmnActCrouchTurnA" ||
      sv == "CmnActNeutral" || sv == "Idling" ||
      sv == "WalkF" || sv == "WalkB" || sv == "Jumping" || sv == "Landing") {
    return {};
  }
  return std::string{sv};
}

bool is_player_in_hitstun(const asw_player* p) {
  if (!p) return false;
  return p->is_in_hitstun() || p->is_stagger() || p->is_guard_crush();
}

bool is_player_in_combo(const asw_player* p) {
  if (!p) return false;
  return p->is_in_hitstun() || p->is_in_blockstun() || p->is_knockdown();
}

bool is_player_idle(const asw_player* p) {
  if (!p) return true;
  return p->can_act() || p->is_stance_idle();
}

void copy_if_exists(const std::filesystem::path& source, const std::filesystem::path& destination) {
  if (!std::filesystem::exists(source)) return;
  std::filesystem::create_directories(destination.parent_path());
  std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing);
}
}  // namespace

TrialManager& TrialManager::instance() {
  static TrialManager manager;
  return manager;
}

TrialManager::TrialManager() = default;

void TrialManager::initialize() {
  if (initialized_) {
    return;
  }
  reloadCatalog();
  updateMenuView();
  initialized_ = true;
}

void TrialManager::resetRuntime() {
  runtime_status_ = TrialStatus::Idle;
  status_text_ = L"Idle";
  retry_countdown_ = 0;
  playback_active_ = false;
  playback_frame_index_ = 0;
  steps_recording_ = false;
  draft_trial_ = TrialDefinition{};
  current_trial_ = TrialDefinition{};
  tracker_.reset();
  pending_setup_capture_.clear();
  pending_setup_restore_.clear();
  awaiting_restore_confirmation_ = false;
}

void TrialManager::onRoundReset() {
  if (steps_recording_) {
    // During step recording, reset clears captured steps and keeps recording
    draft_trial_.steps.clear();
    status_text_ = L"Recording steps...";
  }
  stopCurrentTrial(L"Round Reset");
}

void TrialManager::ensureLoaded() {
  if (!store_) {
    auto mod_root = UE4SSProgram::get_program().get_working_directory();
    store_ = std::make_unique<TrialStore>(mod_root);
  }
}

void TrialManager::reloadCatalog() {
  ensureLoaded();
  auto result = store_->loadTrialCatalog();
  if (!result.success) {
    catalog_load_failed_ = true;
    status_text_ = L"Catalog load failed";
    catalog_.clear();
    return;
  }
  catalog_load_failed_ = false;
  catalog_ = std::move(result.trials);
}

TrialDefinition TrialManager::selectedOrDraft() {
  if (menu_state_.selected_trial_index == 0 || menu_state_.selected_trial_index > catalog_.size()) {
    return draft_trial_;
  }
  return catalog_[menu_state_.selected_trial_index - 1];
}

bool TrialManager::hasActiveTrial() const {
  return runtime_status_ == TrialStatus::Running || runtime_status_ == TrialStatus::Restoring ||
         runtime_status_ == TrialStatus::Cleared || runtime_status_ == TrialStatus::Failed;
}

void TrialManager::stopCurrentTrial(const std::wstring& reason) {
  if (hasActiveTrial()) {
    runtime_status_ = TrialStatus::Idle;
    status_text_ = reason;
    tracker_.reset();
    playback_active_ = false;
    playback_frame_index_ = 0;
  }
}

void TrialManager::updateMenuView() {
  menu_view_.trial_count = catalog_.size() + 1;
  menu_view_.status_label = status_text_;
  menu_view_.steps_recording = steps_recording_;

  if (menu_state_.selected_trial_index == 0) {
    menu_view_.selected_label = L"<New Trial>";
  } else if (menu_state_.selected_trial_index <= catalog_.size()) {
    auto& trial = catalog_[menu_state_.selected_trial_index - 1];
    menu_view_.selected_label = std::wstring(trial.name.begin(), trial.name.end());
  } else {
    menu_view_.selected_label = L"<New Trial>";
  }
}

void TrialManager::handleMenuAction() {
  if (!menu_state_.enabled) {
    menu_state_.action = menu::TrialMenuAction::None;
    return;
  }

  switch (menu_state_.action) {
    case menu::TrialMenuAction::StartTrial: {
      if (!hasActiveTrial()) {
        current_trial_ = selectedOrDraft();
        if (current_trial_.steps.empty()) {
          status_text_ = L"No steps to run";
          break;
        }
        // Start directly into Running — use rollback restore via dllmain
        pending_setup_restore_ = store_ ? store_->setupPathForId(current_trial_.id) : std::filesystem::path{};
        retry_countdown_ = current_trial_.retry_delay_frames;
        runtime_status_ = TrialStatus::Restoring;
        awaiting_restore_confirmation_ = true;
        status_text_ = L"Restoring...";
      }
      break;
    }

    case menu::TrialMenuAction::StopTrial: {
      stopCurrentTrial(L"Stopped");
      pending_setup_capture_.clear();
      pending_setup_restore_.clear();
      break;
    }

    case menu::TrialMenuAction::ToggleRecordSteps: {
      steps_recording_ = !steps_recording_;
      if (steps_recording_) {
        draft_trial_.steps.clear();
        status_text_ = L"Recording steps...";
      } else {
        std::wstringstream ss;
        ss << L"Recorded " << draft_trial_.steps.size() << L" steps";
        status_text_ = ss.str();
      }
      break;
    }

    case menu::TrialMenuAction::SaveTrial: {
      ensureLoaded();
      if (menu_state_.selected_trial_index == 0) {
        // Save as new trial
        std::string name;
        for (wchar_t wc : menu_state_.trial_name) name += static_cast<char>(wc);
        if (name.empty()) name = "Trial";
        // Auto-number: count existing trials with same base name
        int count = 1;
        for (auto& t : catalog_) {
          if (t.name.find(name) == 0) count++;
        }
        draft_trial_.name = name + " " + std::to_string(count);
        draft_trial_.id = std::to_string(std::time(nullptr));
        draft_trial_.retry_delay_frames = menu_state_.retry_delay_frames;
        catalog_.push_back(draft_trial_);
        store_->saveTrialCatalog(catalog_);
        menu_state_.selected_trial_index = catalog_.size();
        std::wstringstream ss;
        ss << L"Saved: " << std::wstring(draft_trial_.name.begin(), draft_trial_.name.end());
        status_text_ = ss.str();
        draft_trial_ = TrialDefinition{};
      } else {
        // Overwrite existing trial's steps
        size_t idx = menu_state_.selected_trial_index - 1;
        if (idx < catalog_.size()) {
          catalog_[idx].steps = draft_trial_.steps;
          catalog_[idx].retry_delay_frames = menu_state_.retry_delay_frames;
          store_->saveTrialCatalog(catalog_);
          status_text_ = L"Trial updated";
        }
      }
      break;
    }

    case menu::TrialMenuAction::DeleteTrial: {
      if (menu_state_.selected_trial_index > 0 && menu_state_.selected_trial_index <= catalog_.size()) {
        size_t idx = menu_state_.selected_trial_index - 1;
        std::wstring deleted_name(catalog_[idx].name.begin(), catalog_[idx].name.end());
        catalog_.erase(catalog_.begin() + static_cast<ptrdiff_t>(idx));
        ensureLoaded();
        store_->saveTrialCatalog(catalog_);
        menu_state_.selected_trial_index = 0;
        status_text_ = L"Deleted: " + deleted_name;
      } else {
        status_text_ = L"Nothing to delete";
      }
      break;
    }

    case menu::TrialMenuAction::ReloadTrials: {
      reloadCatalog();
      menu_state_.selected_trial_index = 0;
      resetRuntime();
      status_text_ = L"Trials reloaded";
      break;
    }

    default:
      break;
  }

  menu_state_.action = menu::TrialMenuAction::None;
}

void TrialManager::updatePreBattle() {
  if (!initialized_) {
    initialize();
  }

  handleMenuAction();

  if (runtime_status_ == TrialStatus::Failed && retry_countdown_ > 0) {
    --retry_countdown_;
    if (retry_countdown_ == 0) {
      runtime_status_ = TrialStatus::Restoring;
      awaiting_restore_confirmation_ = true;
      status_text_ = L"Retrying...";
    }
  }

  if (runtime_status_ == TrialStatus::Cleared) {
    tracker_.reset();
    runtime_status_ = TrialStatus::Idle;
    status_text_ = L"Cleared!";
  }
}

void TrialManager::updatePostBattle(std::string move_name, bool combo_alive) {
  if (runtime_status_ == TrialStatus::Running && tracker_) {
    tracker_->observe({move_name, combo_alive});

    if (tracker_->status() == TrialStatus::Failed) {
      runtime_status_ = TrialStatus::Failed;
      retry_countdown_ = current_trial_.retry_delay_frames;
      status_text_ = L"Failed";
    } else if (tracker_->status() == TrialStatus::Cleared) {
      runtime_status_ = TrialStatus::Cleared;
      playback_active_ = false;
      status_text_ = L"Cleared!";
    }
  }
}

void TrialManager::updatePostBattleFromEngine(asw_engine* engine) {
  if (!engine) {
    if (steps_recording_) status_text_ = L"DBG: engine null";
    return;
  }
  if (!engine->players[0].entity || !engine->players[1].entity) {
    if (steps_recording_) status_text_ = L"DBG: player null";
    return;
  }

  auto* p1 = engine->players[0].entity;
  auto* p2 = engine->players[1].entity;

  auto p1_move = read_trial_move_name(p1);
  auto p2_move = read_trial_move_name(p2);

  // Step recording: works independently of trial Running state
  // Captures all non-idle moves from P1 (combo validation happens separately during trial run)
  if (steps_recording_) {
    std::string move = p1_move;
    if (move.empty()) move = p2_move;
    if (!move.empty()) {
      // Only skip if same as last recorded step (avoids frame-repeat, allows intentional repeats)
      bool same_as_last = !draft_trial_.steps.empty() && draft_trial_.steps.back().move == move;
      if (!same_as_last) {
        draft_trial_.steps.push_back({move, ""});
      }
      // Update status with live count and last move
      std::wstringstream ss;
      ss << L"Rec " << draft_trial_.steps.size() << L"s: ";
      for (char c : draft_trial_.steps.back().move) ss << static_cast<wchar_t>(c);
      status_text_ = ss.str();
    } else {
      // Show that the function IS running but moves are empty
      std::wstringstream ss;
      ss << L"Rec " << draft_trial_.steps.size() << L"s (no move)";
      status_text_ = ss.str();
    }
  }

  // Trial sequence tracking
  bool p1_is_target = is_player_in_combo(p2);
  bool p2_is_target = is_player_in_combo(p1);

  if (runtime_status_ == TrialStatus::Running) {
    if (p1_is_target) {
      updatePostBattle(p1_move, true);
    } else if (p2_is_target) {
      updatePostBattle(p2_move, true);
    } else {
      bool combo_alive = !is_player_idle(p1) || !is_player_idle(p2);
      std::string move = p1_move.empty() ? p2_move : p1_move;
      updatePostBattle(move, combo_alive);
    }
  } else if (runtime_status_ == TrialStatus::Restoring) {
    bool combo_alive = !is_player_idle(p1) || !is_player_idle(p2);
    updatePostBattle({}, combo_alive);
  }
}

void TrialManager::draw() {
  updateMenuView();
}

bool TrialManager::consumeSetupCaptureRequest(std::filesystem::path& blob_path) {
  if (!pending_setup_capture_.empty()) {
    blob_path = pending_setup_capture_;
    return true;
  }
  return false;
}

bool TrialManager::consumeSetupRestoreRequest(std::filesystem::path& blob_path) {
  if (awaiting_restore_confirmation_ && !pending_setup_restore_.empty()) {
    blob_path = pending_setup_restore_;
    return true;
  }
  return false;
}

void TrialManager::onSetupCaptured(bool success, std::filesystem::path blob_path) {
  pending_setup_capture_.clear();
  if (!success) {
    status_text_ = L"Setup capture failed";
  }
}

void TrialManager::onSetupRestored(bool success) {
  awaiting_restore_confirmation_ = false;
  if (!success) {
    runtime_status_ = TrialStatus::Idle;
    status_text_ = L"Restore failed";
    return;
  }

  if (current_trial_.steps.empty()) {
    runtime_status_ = TrialStatus::Cleared;
    status_text_ = L"Cleared";
  } else {
    tracker_ = std::make_unique<TrialSequenceTracker>(current_trial_);
    tracker_->start();
    runtime_status_ = TrialStatus::Running;
    playback_active_ = !current_trial_.playback_frames.empty();
    playback_frame_index_ = 0;
    playback_frames_ = current_trial_.playback_frames;
    status_text_ = L"Running";
  }
}

void TrialManager::consumeTrialMenuState(const menu::TrialMenuState& state) {
  menu_state_ = state;
}

menu::TrialMenuView TrialManager::getTrialMenuView() const {
  return menu_view_;
}

#include "trial_core.h"

TrialSequenceTracker::TrialSequenceTracker(const TrialDefinition& definition)
    : definition_(&definition) {}

void TrialSequenceTracker::start() {
  completed_steps_ = 0;
  status_ = definition_->steps.empty() ? TrialStatus::Cleared : TrialStatus::Running;
}

void TrialSequenceTracker::reset() {
  completed_steps_ = 0;
  status_ = TrialStatus::Idle;
}

void TrialSequenceTracker::observe(const FrameObservation& observation) {
  if (status_ != TrialStatus::Running) {
    return;
  }

  if (completed_steps_ < definition_->steps.size()) {
    const auto& expected = definition_->steps[completed_steps_].move;
    if (!observation.move.empty() && observation.move == expected) {
      ++completed_steps_;
      if (is_complete()) {
        status_ = TrialStatus::Cleared;
      }
      return;
    }
  }

  if (!observation.combo_alive && completed_steps_ > 0 && !is_complete()) {
    status_ = TrialStatus::Failed;
  }
}

TrialStatus TrialSequenceTracker::status() const {
  return status_;
}

std::size_t TrialSequenceTracker::completed_steps() const {
  return completed_steps_;
}

bool TrialSequenceTracker::is_complete() const {
  return completed_steps_ >= definition_->steps.size();
}

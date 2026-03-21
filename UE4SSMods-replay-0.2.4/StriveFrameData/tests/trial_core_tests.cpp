#include "trial_core.h"
#include <cassert>
#include <iostream>

void test_empty_trial_clears_immediately() {
  TrialDefinition empty_trial;
  empty_trial.id = "test_empty";
  empty_trial.name = "Empty Trial";
  empty_trial.steps = {};

  TrialSequenceTracker tracker(empty_trial);
  tracker.start();

  assert(tracker.status() == TrialStatus::Cleared);
  assert(tracker.is_complete());
  std::cout << "[PASS] test_empty_trial_clears_immediately\n";
}

void test_single_step_trial() {
  TrialDefinition trial;
  trial.id = "test_single";
  trial.name = "Single Step Trial";
  trial.steps = {{"Move1", ""}};

  TrialSequenceTracker tracker(trial);
  tracker.start();
  assert(tracker.status() == TrialStatus::Running);
  assert(tracker.completed_steps() == 0);

  tracker.observe({"Move1", true});
  assert(tracker.status() == TrialStatus::Cleared);
  assert(tracker.is_complete());
  std::cout << "[PASS] test_single_step_trial\n";
}

void test_multi_step_trial_ordered() {
  TrialDefinition trial;
  trial.id = "test_multi";
  trial.name = "Multi Step Trial";
  trial.steps = {{"Move1", ""}, {"Move2", ""}, {"Move3", ""}};

  TrialSequenceTracker tracker(trial);
  tracker.start();

  tracker.observe({"Move1", true});
  assert(tracker.status() == TrialStatus::Running);
  assert(tracker.completed_steps() == 1);

  tracker.observe({"Move2", true});
  assert(tracker.completed_steps() == 2);

  tracker.observe({"Move3", true});
  assert(tracker.status() == TrialStatus::Cleared);
  std::cout << "[PASS] test_multi_step_trial_ordered\n";
}

void test_combo_drop_fails_trial() {
  TrialDefinition trial;
  trial.id = "test_fail";
  trial.name = "Fail Trial";
  trial.steps = {{"Move1", ""}, {"Move2", ""}};

  TrialSequenceTracker tracker(trial);
  tracker.start();

  tracker.observe({"Move1", true});
  assert(tracker.completed_steps() == 1);

  tracker.observe({"", false});
  assert(tracker.status() == TrialStatus::Running);

  tracker.observe({"Move2", false});
  assert(tracker.status() == TrialStatus::Failed);
  std::cout << "[PASS] test_combo_drop_fails_trial\n";
}

void test_wrong_move_does_not_advance() {
  TrialDefinition trial;
  trial.id = "test_wrong";
  trial.name = "Wrong Move Trial";
  trial.steps = {{"Move1", ""}, {"Move2", ""}};

  TrialSequenceTracker tracker(trial);
  tracker.start();

  tracker.observe({"WrongMove", true});
  assert(tracker.completed_steps() == 0);
  assert(tracker.status() == TrialStatus::Running);

  tracker.observe({"Move1", true});
  assert(tracker.completed_steps() == 1);

  tracker.observe({"AnotherWrong", true});
  assert(tracker.completed_steps() == 1);
  std::cout << "[PASS] test_wrong_move_does_not_advance\n";
}

void test_tracker_reset() {
  TrialDefinition trial;
  trial.id = "test_reset";
  trial.name = "Reset Trial";
  trial.steps = {{"Move1", ""}, {"Move2", ""}};

  TrialSequenceTracker tracker(trial);
  tracker.start();
  tracker.observe({"Move1", true});
  assert(tracker.completed_steps() == 1);

  tracker.reset();
  assert(tracker.status() == TrialStatus::Idle);
  assert(tracker.completed_steps() == 0);
  std::cout << "[PASS] test_tracker_reset\n";
}

int main() {
  std::cout << "Running trial_core tests...\n";

  test_empty_trial_clears_immediately();
  test_single_step_trial();
  test_multi_step_trial_ordered();
  test_combo_drop_fails_trial();
  test_wrong_move_does_not_advance();
  test_tracker_reset();

  std::cout << "\nAll tests passed!\n";
  return 0;
}

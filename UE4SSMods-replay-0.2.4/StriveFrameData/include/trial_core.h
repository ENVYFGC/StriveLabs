#pragma once

#include <string>
#include <vector>
#include <filesystem>

enum class TrialStatus {
  Idle,
  DraftEditing,
  Restoring,
  Running,
  Failed,
  Cleared
};

struct TrialStep {
  std::string move;
  std::string notes;
};

struct FrameObservation {
  std::string move;
  bool combo_alive = false;
};

struct TrialDefinition {
  std::string id;
  std::string name;
  std::string character;
  std::string notes;
  int retry_delay_frames = 30;
  std::string setup_blob_path;
  std::vector<TrialStep> steps;
  std::vector<unsigned short> playback_frames;
};

class TrialSequenceTracker {
public:
  explicit TrialSequenceTracker(const TrialDefinition& definition);

  void start();
  void reset();
  void observe(const FrameObservation& observation);

  TrialStatus status() const;
  std::size_t completed_steps() const;
  bool is_complete() const;

private:
  const TrialDefinition* definition_ = nullptr;
  std::size_t completed_steps_ = 0;
  TrialStatus status_ = TrialStatus::Idle;
};

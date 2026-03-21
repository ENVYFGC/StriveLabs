#pragma once

#include "trial_core.h"

#include <filesystem>
#include <string>
#include <vector>

struct TrialCatalogLoadResult {
  bool success = false;
  std::string error_message;
  std::vector<TrialDefinition> trials;
};

struct TrialCatalogSaveResult {
  bool success = false;
  std::string error_message;
};

class TrialStore {
public:
  explicit TrialStore(std::filesystem::path root_dir);

  TrialCatalogLoadResult loadTrialCatalog();
  TrialCatalogSaveResult saveTrialCatalog(const std::vector<TrialDefinition>& trials);

  std::filesystem::path trialsRoot() const;
  std::filesystem::path catalogPath() const;
  std::filesystem::path setupPathForId(const std::string& id) const;
  std::filesystem::path draftSetupPath() const;

private:
  std::filesystem::path root_dir_;
};

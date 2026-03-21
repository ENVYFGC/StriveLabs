#include "trial_store.h"

#include <fstream>
#include <sstream>
#include <glaze/json.hpp>

struct TrialCatalogJson {
  std::vector<TrialDefinition> trials;
};

TrialStore::TrialStore(std::filesystem::path root_dir) : root_dir_(root_dir) {}

std::filesystem::path TrialStore::trialsRoot() const {
  return root_dir_ / "trials";
}

std::filesystem::path TrialStore::catalogPath() const {
  return trialsRoot() / "catalog.json";
}

std::filesystem::path TrialStore::setupPathForId(const std::string& id) const {
  return trialsRoot() / "setups" / (id + ".bin");
}

std::filesystem::path TrialStore::draftSetupPath() const {
  return trialsRoot() / "setups" / "_draft_setup.bin";
}

TrialCatalogLoadResult TrialStore::loadTrialCatalog() {
  TrialCatalogLoadResult result;

  auto path = catalogPath();
  if (!std::filesystem::exists(path)) {
    result.success = true;
    return result;
  }

  std::ifstream file(path);
  if (!file.is_open()) {
    result.success = false;
    result.error_message = "Failed to open catalog file";
    return result;
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string json_str = buffer.str();

  TrialCatalogJson catalog{};
  auto ec = glz::read_json(catalog, json_str);
  if (ec) {
    result.success = false;
    result.error_message = "JSON parse error: " + glz::format_error(ec, json_str);
    return result;
  }

  result.trials = std::move(catalog.trials);
  result.success = true;
  return result;
}

TrialCatalogSaveResult TrialStore::saveTrialCatalog(const std::vector<TrialDefinition>& trials) {
  TrialCatalogSaveResult result;

  auto root = trialsRoot();
  if (!std::filesystem::exists(root)) {
    std::filesystem::create_directories(root);
  }

  auto path = catalogPath();

  TrialCatalogJson catalog{trials};
  std::string json_str;
  auto ec = glz::write<glz::opts{.prettify = true}>(catalog, json_str);
  if (ec) {
    result.success = false;
    result.error_message = "JSON serialization error";
    return result;
  }

  std::ofstream file(path);
  if (!file.is_open()) {
    result.success = false;
    result.error_message = "Failed to open catalog file for writing";
    return result;
  }

  file << json_str;
  result.success = true;
  return result;
}

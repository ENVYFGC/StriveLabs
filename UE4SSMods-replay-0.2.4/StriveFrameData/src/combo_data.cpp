#include "combo_data.h"
#include "combo_log.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <glaze/json.hpp>

// ── Glaze reflection ────────────────────────────────────────────────

template <>
struct glz::meta<ComboEntry> {
  using T = ComboEntry;
  static constexpr auto value = object(
    "notation", &T::notation,
    "notes", &T::notes,
    "link", &T::link,
    "tags", &T::tags,
    "contributor", &T::contributor
  );
};

template <>
struct glz::meta<StarterGroup> {
  using T = StarterGroup;
  static constexpr auto value = object("note", &T::note, "combos", &T::combos);
};

template <>
struct glz::meta<CharacterInfo> {
  using T = CharacterInfo;
  static constexpr auto value = object("id", &T::id, "name", &T::name);
};

template <>
struct glz::meta<CharacterRegistry> {
  using T = CharacterRegistry;
  static constexpr auto value = object("characters", &T::characters);
};

// ── Starter order ───────────────────────────────────────────────────

static std::vector<std::string> alphabetical_order_;

const std::vector<std::string>& starterOrderFor(const std::string& character_id,
                                                  const std::string& version) {
  // Sol has hardcoded orders
  if (character_id == "sol") {
    if (version == "V148") return V148_STARTERS;
    if (version == "PREVIOUS") return PREVIOUS_STARTERS;
  }
  // All other characters: alphabetical fallback
  return alphabetical_order_;
}

// ── Auto-tagger ─────────────────────────────────────────────────────

static std::string toLower(const std::string& s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return out;
}

static void autoTagCombo(ComboEntry& combo) {
  if (!combo.tags.empty()) return;  // explicit tags take priority

  // Scan both notes and notation for keywords
  auto lower = toLower(combo.notes) + " " + toLower(combo.notation);

  if (lower.find("corner") != std::string::npos)
    combo.tags.push_back("corner");
  if (lower.find("midscreen") != std::string::npos)
    combo.tags.push_back("midscreen");
  if (lower.find("meterless") != std::string::npos)
    combo.tags.push_back("meterless");
  if (lower.find("50%") != std::string::npos ||
      lower.find("tension") != std::string::npos)
    combo.tags.push_back("meter");

  // CH at word boundary (start of string or after space)
  if (lower.substr(0, 3) == "ch " ||
      lower.find(" ch ") != std::string::npos)
    combo.tags.push_back("ch");

  if (lower.find("ws!") != std::string::npos ||
      lower.find("wall") != std::string::npos)
    combo.tags.push_back("wallsplat");
}

static void autoTagAllCombos(CharacterCombos& char_data) {
  for (auto& [ver_key, starters] : char_data.versions) {
    for (auto& [starter_key, group] : starters) {
      for (auto& combo : group.combos) {
        autoTagCombo(combo);
      }
    }
  }
}

// ── Parse a single character JSON file ──────────────────────────────

static bool parseCharacterFile(const std::filesystem::path& path,
                                CharacterCombos& out,
                                std::string& error) {
  std::ifstream file(path);
  if (!file.is_open()) {
    error = "Failed to open " + path.filename().string();
    return false;
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string json_str = buffer.str();

  if (json_str.empty() || json_str == "{}") {
    // Empty file — valid but no data
    return true;
  }

  // Parse top level as raw JSON to skip unknown keys (like RESOURCES)
  std::map<std::string, glz::raw_json> top_level;
  auto ec = glz::read_json(top_level, json_str);
  if (ec) {
    error = "JSON parse error in " + path.filename().string() + ": " +
            glz::format_error(ec, json_str);
    return false;
  }

  for (auto& [version_key, raw_value] : top_level) {
    if (version_key == "RESOURCES") continue;

    std::map<std::string, StarterGroup> starters;
    auto ver_ec = glz::read_json(starters, raw_value.str);
    if (ver_ec) {
      COMBO_LOG("WARNING: Failed to parse version '" + version_key +
                "' in " + path.filename().string());
      continue;
    }

    size_t combo_count = 0;
    for (const auto& [k, v] : starters) combo_count += v.combos.size();
    COMBO_LOG("  " + version_key + ": " + std::to_string(starters.size()) +
              " starters, " + std::to_string(combo_count) + " combos");

    out.versions[version_key] = std::move(starters);
  }

  return true;
}

// ── Main loader ─────────────────────────────────────────────────────

ComboLoadResult loadComboDatabase(const std::filesystem::path& data_dir) {
  ComboLoadResult result;
  auto combo_dir = data_dir / "combo_data";

  // ── 1. Load character registry ──

  auto reg_path = combo_dir / "_characters.json";
  if (std::filesystem::exists(reg_path)) {
    std::ifstream reg_file(reg_path);
    if (reg_file.is_open()) {
      std::stringstream buf;
      buf << reg_file.rdbuf();
      std::string reg_str = buf.str();

      auto ec = glz::read_json(result.database.registry, reg_str);
      if (ec) {
        COMBO_LOG("WARNING: Failed to parse _characters.json, using auto-discovery");
      } else {
        COMBO_LOG("Loaded character registry with " +
                  std::to_string(result.database.registry.characters.size()) + " characters");
      }
    }
  } else {
    COMBO_LOG("No _characters.json found, will auto-discover from files");
  }

  // ── 2. Load per-character JSON files ──

  if (!std::filesystem::exists(combo_dir) || !std::filesystem::is_directory(combo_dir)) {
    // Fallback: try loading old single-file combo_data.json
    auto legacy_path = data_dir / "combo_data.json";
    if (std::filesystem::exists(legacy_path)) {
      COMBO_LOG("combo_data/ directory not found, falling back to combo_data.json");
      CharacterCombos sol_data;
      std::string err;
      if (parseCharacterFile(legacy_path, sol_data, err)) {
        if (!sol_data.empty()) {
          autoTagAllCombos(sol_data);
          result.database.characters["sol"] = std::move(sol_data);
          // Ensure Sol is in registry
          bool found = false;
          for (auto& ch : result.database.registry.characters) {
            if (ch.id == "sol") { found = true; break; }
          }
          if (!found) {
            result.database.registry.characters.push_back({"sol", "Sol Badguy"});
          }
        }
      } else {
        result.error_message = err;
        COMBO_LOG("ERROR: " + err);
        return result;
      }
    } else {
      result.error_message = "No combo_data/ directory or combo_data.json found";
      COMBO_LOG("ERROR: " + result.error_message);
      return result;
    }
  } else {
    // Scan combo_data/ for *.json files (skip _ prefixed)
    for (auto& entry : std::filesystem::directory_iterator(combo_dir)) {
      if (!entry.is_regular_file()) continue;
      if (entry.path().extension() != ".json") continue;

      auto filename = entry.path().filename().string();
      if (filename.empty() || filename[0] == '_') continue;

      auto char_id = entry.path().stem().string();
      COMBO_LOG("Loading character: " + char_id + " from " + filename);

      CharacterCombos char_data;
      std::string err;
      if (!parseCharacterFile(entry.path(), char_data, err)) {
        COMBO_LOG("WARNING: " + err);
        continue;
      }

      if (char_data.empty()) {
        COMBO_LOG("  (empty, skipping)");
        continue;
      }

      autoTagAllCombos(char_data);
      result.database.characters[char_id] = std::move(char_data);

      // Ensure character is in registry (auto-discover if not in _characters.json)
      bool found = false;
      for (auto& ch : result.database.registry.characters) {
        if (ch.id == char_id) { found = true; break; }
      }
      if (!found) {
        result.database.registry.characters.push_back({char_id, char_id});
      }
    }
  }

  if (result.database.empty()) {
    result.error_message = "No character data found";
    COMBO_LOG("ERROR: " + result.error_message);
    return result;
  }

  result.success = true;
  COMBO_LOG("ComboDatabase loaded: " +
            std::to_string(result.database.characters.size()) + " character(s)");
  return result;
}

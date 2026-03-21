#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

// ── Character registry ──────────────────────────────────────────────

struct CharacterInfo {
  std::string id;    // filename stem, e.g. "sol"
  std::string name;  // display name, e.g. "Sol Badguy"
};

struct CharacterRegistry {
  std::vector<CharacterInfo> characters;
};

// ── Combo entry ─────────────────────────────────────────────────────

struct ComboEntry {
  std::string notation;
  std::string notes;
  std::string link;
  std::vector<std::string> tags;   // e.g. ["midscreen","meterless"]
  std::string contributor;          // e.g. "ENVY"
};

struct StarterGroup {
  std::string note;
  std::vector<ComboEntry> combos;
};

// ── Per-character combo data ────────────────────────────────────────

// version key -> starter key -> StarterGroup
// e.g. "V148" -> "cS" -> StarterGroup{...}
struct CharacterCombos {
  std::map<std::string, std::map<std::string, StarterGroup>> versions;

  bool empty() const { return versions.empty(); }
};

// ── Full database (all characters) ──────────────────────────────────

struct ComboDatabase {
  CharacterRegistry registry;
  std::map<std::string, CharacterCombos> characters;  // "sol" -> CharacterCombos

  bool empty() const { return characters.empty(); }
};

struct ComboLoadResult {
  bool success = false;
  std::string error_message;
  ComboDatabase database;
};

// ── Starter display order ───────────────────────────────────────────

// Sol-specific starter orders (other characters fall back to alphabetical)
inline const std::vector<std::string> V148_STARTERS = {
    "cS", "fS", "5H", "2K", "2D", "6S", "2S", "6H", "236KK", "214K",
    "Miscellaneous"};

inline const std::vector<std::string> PREVIOUS_STARTERS = {
    "2D",
    "2S",
    "fS",
    "cS",
    "5H",
    "2K > 2D",
    "fS > 5H > FAFNIR",
    "CH 6H",
    "BANDIT BRINGER",
    "CH RS 50% TENSION BUILD",
    "DP Punishes",
    "MISCELLANEOUS",
    "v1.48 Combos & Tech"};

// Returns the display order for starters given a character and version.
// Sol has hardcoded orders; other characters fall back to alphabetical.
const std::vector<std::string>& starterOrderFor(const std::string& character_id,
                                                 const std::string& version);

// Load all per-character JSON files from data_dir/combo_data/
ComboLoadResult loadComboDatabase(const std::filesystem::path& data_dir);

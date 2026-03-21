# Community Combo Expansion Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expand the combo system from Sol-only to all characters with community contribution support, searchable tags, and contributor credits.

**Architecture:** Per-character JSON files replace the single `combo_data.json`. A new Characters page is added before Versions in the combo menu. Tags are parsed from combo entries for filtering. A public GitHub repo hosts the data files with a contribution guide and validation script.

**Tech Stack:** C++20, Glaze JSON, UE4SS, Python (validation tooling)

---

## File Structure

```
combo_data/                          (replaces single combo_data.json)
  sol.json                           (Sol combos — migrated from combo_data.json)
  ky.json                            (Ky combos — community contributed)
  may.json                           (etc.)
  ...
  _characters.json                   (character registry: display names, order, icons)

StriveFrameData/
  include/
    combo_data.h                     (MODIFY: add tags, contributor, character registry structs)
    combo_menu.h                     (MODIFY: add Characters page, filter state)
  src/
    combo_data.cpp                   (MODIFY: load per-character files, parse tags)
    combo_menu.cpp                   (MODIFY: character selection, tag filtering)

tools/                               (new — lives in the GitHub repo root)
  validate_combos.py                 (validates JSON schema, required fields, link format)
  CONTRIBUTING.md                    (how to add combos for a character)
  combo_template.json                (empty template for new character)
```

---

## Phase 1: Multi-Character Data & Navigation

This is foundational — everything else builds on it.

### Task 1: Design the per-character JSON schema and character registry

**Files:**
- Create: `combo_data/sol.json` (migrated from `combo_data.json`)
- Create: `combo_data/_characters.json`

**New JSON schemas:**

`_characters.json`:
```json
{
  "characters": [
    { "id": "sol",       "name": "Sol Badguy" },
    { "id": "ky",        "name": "Ky Kiske" },
    { "id": "may",       "name": "May" },
    { "id": "axl",       "name": "Axl Low" },
    { "id": "chipp",     "name": "Chipp Zanuff" },
    { "id": "potemkin",  "name": "Potemkin" },
    { "id": "faust",     "name": "Faust" },
    { "id": "millia",    "name": "Millia Rage" },
    { "id": "zato",      "name": "Zato-1" },
    { "id": "ramlethal", "name": "Ramlethal Valentine" },
    { "id": "leo",       "name": "Leo Whitefang" },
    { "id": "nagoriyuki","name": "Nagoriyuki" },
    { "id": "giovanna",  "name": "Giovanna" },
    { "id": "anji",      "name": "Anji Mito" },
    { "id": "ino",       "name": "I-No" },
    { "id": "goldlewis", "name": "Goldlewis Dickinson" },
    { "id": "jacko",     "name": "Jack-O" },
    { "id": "happychaos","name": "Happy Chaos" },
    { "id": "baiken",    "name": "Baiken" },
    { "id": "testament", "name": "Testament" },
    { "id": "bridget",   "name": "Bridget" },
    { "id": "sin",       "name": "Sin Kiske" },
    { "id": "bedman",    "name": "Bedman?" },
    { "id": "asuka",     "name": "Asuka R. Kreutz" },
    { "id": "johnny",    "name": "Johnny" },
    { "id": "elphelt",   "name": "Elphelt Valentine" },
    { "id": "aba",       "name": "A.B.A" },
    { "id": "slayer",    "name": "Slayer" },
    { "id": "dizzy",     "name": "Dizzy" },
    { "id": "venom",     "name": "Venom" },
    { "id": "unika",     "name": "Unika" },
    { "id": "queen",     "name": "Queen Dizzy" }
  ]
}
```

Per-character file (`sol.json`) — same schema as current, just one character per file:
```json
{
  "V148": {
    "cS": {
      "note": "Combos with cS as starter",
      "combos": [
        {
          "notation": "cS > 6H > 236K~K > 5K > cS > HSVV WS!",
          "notes": "Midscreen Meterless",
          "link": "https://www.youtube.com/watch?v=...",
          "tags": ["midscreen", "meterless"],
          "contributor": "ENVY"
        }
      ]
    }
  }
}
```

- [ ] **Step 1: Create `_characters.json`** with the full GGST roster

- [ ] **Step 2: Migrate `combo_data.json` → `combo_data/sol.json`**
  - Copy the existing file content as-is (no schema changes yet, tags/contributor added in Phase 2)
  - Remove the `RESOURCES` section during migration

- [ ] **Step 3: Create empty placeholder files** for a few characters to test multi-char loading
  - `combo_data/ky.json` with `{}` (empty, but valid JSON)

- [ ] **Step 4: Commit**
  ```bash
  git add combo_data/
  git commit -m "feat: migrate combo data to per-character JSON files"
  ```

---

### Task 2: Update combo_data.h/cpp to load per-character files

**Files:**
- Modify: `include/combo_data.h`
- Modify: `src/combo_data.cpp`

The key change: `ComboDatabase` now holds data for **all characters**, and the loader scans a directory for `*.json` files (skipping `_characters.json`).

- [ ] **Step 1: Update structs in `combo_data.h`**

Add character registry types and restructure ComboDatabase:

```cpp
// Character registry entry
struct CharacterInfo {
  std::string id;    // filename without .json, e.g. "sol"
  std::string name;  // display name, e.g. "Sol Badguy"
};

struct CharacterRegistry {
  std::vector<CharacterInfo> characters;
};

// Per-character data (same as old ComboDatabase)
struct CharacterCombos {
  std::map<std::string, std::map<std::string, StarterGroup>> versions;
  bool empty() const { return versions.empty(); }
};

// Full database: character_id -> CharacterCombos
struct ComboDatabase {
  CharacterRegistry registry;
  std::map<std::string, CharacterCombos> characters;  // "sol" -> CharacterCombos

  bool empty() const { return characters.empty(); }
};
```

Also add per-character starter orders (replace the current global ones):
```cpp
// Returns the display order for starters. Checks character-specific orders first,
// then version-specific fallbacks, then alphabetical.
const std::vector<std::string>& starterOrderFor(const std::string& character_id,
                                                  const std::string& version);
```

- [ ] **Step 2: Update `combo_data.cpp` loader**

Replace `loadComboDatabase(path)` with `loadComboDatabase(directory)`:

```cpp
ComboLoadResult loadComboDatabase(const std::filesystem::path& data_dir) {
  ComboLoadResult result;

  // 1. Load _characters.json registry
  auto reg_path = data_dir / "_characters.json";
  // ... load and parse registry ...

  // 2. Scan for per-character JSON files
  for (auto& entry : std::filesystem::directory_iterator(data_dir)) {
    if (!entry.is_regular_file()) continue;
    auto filename = entry.path().filename().string();
    if (filename.starts_with("_") || entry.path().extension() != ".json") continue;

    auto char_id = entry.path().stem().string();  // "sol" from "sol.json"

    // Parse same as before (version -> starter -> StarterGroup)
    // Store into result.database.characters[char_id]
  }

  return result;
}
```

- [ ] **Step 3: Update Glaze reflection** for new/renamed types

- [ ] **Step 4: Update `starterOrderFor()`** to take character_id

For now, Sol's orders stay hardcoded. Other characters fall back to alphabetical. Later, starter orders can be embedded in each character's JSON or in the registry.

- [ ] **Step 5: Commit**
  ```bash
  git add include/combo_data.h src/combo_data.cpp
  git commit -m "feat: load per-character combo JSON files from directory"
  ```

---

### Task 3: Add Characters page to ComboMenu

**Files:**
- Modify: `include/combo_menu.h`
- Modify: `src/combo_menu.cpp`

Navigation becomes: **Characters → Versions → Starters → ComboList**

- [ ] **Step 1: Add `Page::Characters` to the enum**

```cpp
enum class Page { Characters, Versions, Starters, ComboList };
```

- [ ] **Step 2: Add character navigation state to ComboMenu**

```cpp
// New members:
std::vector<std::string> character_ids_;    // ordered list of char IDs with data
std::string current_character_;             // selected character ID
```

- [ ] **Step 3: Update `init()`**

Change from loading a single JSON file to loading the `combo_data/` directory:
```cpp
void ComboMenu::init(const std::filesystem::path& data_dir) {
  auto combo_dir = data_dir / "combo_data";
  auto result = loadComboDatabase(combo_dir);
  // ...
  // Build character list — only characters that have data
  character_ids_.clear();
  for (auto& ch : db_.registry.characters) {
    if (db_.characters.count(ch.id) && !db_.characters.at(ch.id).empty()) {
      character_ids_.push_back(ch.id);
    }
  }
  page_ = Page::Characters;
}
```

- [ ] **Step 4: Update `rowCount()` and `contentHeight()`**

Add `Page::Characters` case:
```cpp
case Page::Characters:
  return character_ids_.size();
```

- [ ] **Step 5: Update `update()` — navigation logic**

- Characters page: right enters character (calls `enterCharacter(cursor_)`)
- Characters page: left returns true (exit to main menu)
- Versions page: left goes back to Characters (not exit)

```cpp
void ComboMenu::enterCharacter(size_t idx) {
  current_character_ = character_ids_[idx];
  // Build version_keys_ from this character's data
  auto& char_data = db_.characters.at(current_character_);
  version_keys_.clear();
  // Same priority logic as before: V148, PREVIOUS, then alphabetical
  if (char_data.versions.count("V148")) version_keys_.push_back("V148");
  if (char_data.versions.count("PREVIOUS")) version_keys_.push_back("PREVIOUS");
  for (auto& [key, _] : char_data.versions) {
    if (key != "V148" && key != "PREVIOUS") version_keys_.push_back(key);
  }
  page_ = Page::Versions;
  cursor_ = 0;
}
```

- [ ] **Step 6: Update `draw()` — Characters page rendering**

```cpp
case Page::Characters: {
  for (size_t i = 0; i < character_ids_.size(); i++) {
    auto& id = character_ids_[i];
    auto display_name = getCharacterDisplayName(id);  // lookup from registry
    auto& char_data = db_.characters.at(id);
    auto version_count = char_data.versions.size();
    auto info = std::to_wstring(version_count) + L" version(s)";
    drawRow(i, display_name, L"< " + info + L" >");
  }
  break;
}
```

- [ ] **Step 7: Update `goBack()`**

```cpp
case Page::Versions:
  page_ = Page::Characters;
  cursor_ = 0;
  break;
```

- [ ] **Step 8: Update all references from `db_.versions` to `db_.characters.at(current_character_).versions`**

Every place that currently does `db_.versions.at(current_version_)` now needs to go through the current character first.

- [ ] **Step 9: Update `combo_menu.cpp init()` path** — pass `data_dir` not `data_dir / "combo_data.json"`

- [ ] **Step 10: Update `dllmain.cpp` or `menu.cpp`** if `initComboMenu()` path needs adjusting

- [ ] **Step 11: Build and test**

Expected behavior:
- Open Combos menu → see "Sol Badguy" (only char with data)
- Right → see V148, PREVIOUS (same as before)
- Everything else works as before
- If Ky/May have empty JSONs, they don't appear (filtered in init)

- [ ] **Step 12: Commit**
  ```bash
  git add include/combo_menu.h src/combo_menu.cpp include/combo_data.h src/combo_data.cpp
  git commit -m "feat: add character selection page to combo menu"
  ```

---

## Phase 2: Tags & Filtering

### Task 4: Add tags to combo schema

**Files:**
- Modify: `include/combo_data.h`
- Modify: `src/combo_data.cpp`

- [ ] **Step 1: Add `tags` and `contributor` fields to ComboEntry**

```cpp
struct ComboEntry {
  std::string notation;
  std::string notes;
  std::string link;
  std::vector<std::string> tags;        // ["midscreen", "meterless", "corner", "ch"]
  std::string contributor;               // "ENVY"
};
```

- [ ] **Step 2: Update Glaze reflection**

```cpp
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
```

Glaze handles missing fields gracefully — old JSONs without `tags`/`contributor` will just get empty defaults.

- [ ] **Step 3: Auto-parse tags from notes field** (for backward compatibility)

Add a post-load step that extracts common keywords from `notes` into `tags` if `tags` is empty:

```cpp
void autoTagCombo(ComboEntry& combo) {
  if (!combo.tags.empty()) return;  // explicit tags take priority

  auto lower_notes = toLower(combo.notes);
  if (lower_notes.find("corner") != std::string::npos) combo.tags.push_back("corner");
  if (lower_notes.find("midscreen") != std::string::npos) combo.tags.push_back("midscreen");
  if (lower_notes.find("meterless") != std::string::npos) combo.tags.push_back("meterless");
  if (lower_notes.find("50%") != std::string::npos || lower_notes.find("tension") != std::string::npos)
    combo.tags.push_back("meter");
  if (lower_notes.starts_with("ch ") || lower_notes.find(" ch ") != std::string::npos)
    combo.tags.push_back("ch");
  if (lower_notes.find("wall") != std::string::npos || lower_notes.find("ws!") != std::string::npos)
    combo.tags.push_back("wallsplat");
}
```

- [ ] **Step 4: Commit**
  ```bash
  git add include/combo_data.h src/combo_data.cpp
  git commit -m "feat: add tags and contributor fields to combo entries"
  ```

---

### Task 5: Tag filter UI in combo menu

**Files:**
- Modify: `include/combo_menu.h`
- Modify: `src/combo_menu.cpp`

- [ ] **Step 1: Add filter state to ComboMenu**

```cpp
// Known tag set (built from all loaded combos)
std::vector<std::string> all_tags_;       // discovered unique tags
std::set<std::string> active_filters_;    // currently active tag filters
bool filter_mode_ = false;                // true when filter panel is showing
std::vector<size_t> filtered_indices_;    // indices into combos that match filters
```

- [ ] **Step 2: Build `all_tags_` during `init()`**

Scan all combos across all characters/versions and collect unique tags.

- [ ] **Step 3: Add filter toggle**

When on the ComboList page, a dedicated key (e.g., Tab via GetAsyncKeyState in the Present hook, or a menu row at the top) opens a filter overlay showing all tags as toggleable checkboxes.

Simpler approach — add a **"Filter: [ALL]"** row at the top of the ComboList page:
- Right on it cycles through: ALL → corner → midscreen → meterless → meter → ch → wallsplat → ALL
- When a filter is active, `filtered_indices_` only contains combos matching that tag
- Update pagination to use `filtered_indices_` instead of the full combo list

- [ ] **Step 4: Update `comboCount()` and pagination** to use filtered list when filter is active

- [ ] **Step 5: Update `draw()` ComboList** to show current filter and use filtered indices

- [ ] **Step 6: Build and test** — cycle through filters, verify pagination still works

- [ ] **Step 7: Commit**
  ```bash
  git add include/combo_menu.h src/combo_menu.cpp
  git commit -m "feat: add tag filtering to combo list"
  ```

---

## Phase 3: Contributor Display

### Task 6: Show contributor name in combo list

**Files:**
- Modify: `src/combo_menu.cpp`

This is a small visual addition — show contributor on the notes line.

- [ ] **Step 1: Update ComboList drawing**

In the two-line combo row, append contributor to line 2:

```cpp
// Line 2: notes + contributor
std::wstring line2 = notes;
if (!combo.contributor.empty()) {
  if (!line2.empty()) line2 += L"  |  ";
  line2 += L"by " + toWide(combo.contributor);
}
```

- [ ] **Step 2: Build and test** — combos with contributor show "notes  |  by ENVY", combos without just show notes

- [ ] **Step 3: Commit**
  ```bash
  git add src/combo_menu.cpp
  git commit -m "feat: display contributor name on combo entries"
  ```

---

## Phase 4: Community Contribution Pipeline

### Task 7: Set up GitHub repo structure for combo data

**Files:**
- Create: `tools/validate_combos.py`
- Create: `tools/CONTRIBUTING.md`
- Create: `tools/combo_template.json`

This is tooling that lives alongside the combo data repo — it could be the same repo as the mod, or a separate `strive-combo-data` repo.

- [ ] **Step 1: Create `combo_template.json`**

```json
{
  "V148": {
    "cS": {
      "note": "Combos starting with close Slash",
      "combos": [
        {
          "notation": "cS > ...",
          "notes": "Description (Midscreen/Corner, Meterless/50% Tension, etc.)",
          "link": "https://www.youtube.com/watch?v=YOUR_VIDEO_ID",
          "tags": ["midscreen", "meterless"],
          "contributor": "YourName"
        }
      ]
    }
  }
}
```

- [ ] **Step 2: Create `validate_combos.py`**

A Python script that validates all JSON files in `combo_data/`:

```python
#!/usr/bin/env python3
"""Validates combo data JSON files for schema correctness."""

import json, sys, re
from pathlib import Path

VALID_TAGS = {"corner", "midscreen", "meterless", "meter", "ch", "wallsplat",
              "oki", "safejump", "burst-safe", "reversal-safe"}
YT_PATTERN = re.compile(r"https://(www\.)?youtube\.com/watch\?v=[\w-]+")

def validate_character_file(path: Path) -> list[str]:
    errors = []
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        return [f"{path.name}: Invalid JSON - {e}"]

    if not isinstance(data, dict):
        return [f"{path.name}: Root must be an object"]

    for version_key, starters in data.items():
        if not isinstance(starters, dict):
            errors.append(f"{path.name}/{version_key}: Must be an object")
            continue
        for starter_key, group in starters.items():
            prefix = f"{path.name}/{version_key}/{starter_key}"
            if "combos" not in group:
                errors.append(f"{prefix}: Missing 'combos' array")
                continue
            for i, combo in enumerate(group["combos"]):
                cp = f"{prefix}/combos[{i}]"
                if not combo.get("notation"):
                    errors.append(f"{cp}: Missing 'notation'")
                if not combo.get("notes"):
                    errors.append(f"{cp}: Missing 'notes'")
                if combo.get("link") and not YT_PATTERN.match(combo["link"]):
                    errors.append(f"{cp}: Invalid YouTube link: {combo.get('link')}")
                for tag in combo.get("tags", []):
                    if tag not in VALID_TAGS:
                        errors.append(f"{cp}: Unknown tag '{tag}' (valid: {VALID_TAGS})")
    return errors

def main():
    combo_dir = Path(__file__).parent.parent / "combo_data"
    all_errors = []
    for f in sorted(combo_dir.glob("*.json")):
        if f.name.startswith("_"):
            continue
        all_errors.extend(validate_character_file(f))

    if all_errors:
        print(f"FAILED: {len(all_errors)} error(s):")
        for e in all_errors:
            print(f"  - {e}")
        sys.exit(1)
    else:
        print("All combo data files are valid.")

if __name__ == "__main__":
    main()
```

- [ ] **Step 3: Create `CONTRIBUTING.md`**

```markdown
# Contributing Combos

## How to add combos for a character

1. Fork this repo
2. Find your character's JSON file in `combo_data/` (e.g. `ky.json`)
   - If it doesn't exist, copy `tools/combo_template.json` and rename it
3. Add your combos under the appropriate version and starter
4. Each combo needs:
   - `notation` — the input sequence (use standard notation: cS, fS, 5H, 2K, jH, etc.)
   - `notes` — short description (position, meter cost, key properties)
   - `link` — YouTube link to a video demo (optional but strongly encouraged)
   - `tags` — array of tags (see below)
   - `contributor` — your name/tag
5. Run `python tools/validate_combos.py` to check for errors
6. Submit a Pull Request

## Valid Tags
- `corner` — requires corner position
- `midscreen` — works midscreen
- `meterless` — no tension required
- `meter` — requires tension
- `ch` — requires counter hit
- `wallsplat` — leads to wall splat
- `oki` — has okizeme follow-up
- `safejump` — includes safe jump setup
- `burst-safe` — safe against burst
- `reversal-safe` — safe against reversal

## Notation Guide
Standard Guilty Gear notation:
- Numbers = directions (numpad: 5=neutral, 6=forward, 2=down, etc.)
- P/K/S/H/D = buttons
- cS/fS = close/far Slash
- jX = jumping X
- dl = delay
- dc = dash cancel
- > = link/cancel into
- WS! = wall splat
- CH = counter hit
```

- [ ] **Step 4: Add GitHub Actions CI** (optional but recommended)

`.github/workflows/validate.yml`:
```yaml
name: Validate Combo Data
on: [push, pull_request]
jobs:
  validate:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-python@v5
        with:
          python-version: '3.12'
      - run: python tools/validate_combos.py
```

- [ ] **Step 5: Commit**
  ```bash
  git add tools/ .github/
  git commit -m "feat: add community contribution tooling and guidelines"
  ```

---

## Phase 5: 2.0 Update Resilience

### Task 8: Abstract game-dependent values

**Files:**
- Modify: various source files

This is about making the post-update process as smooth as possible.

- [ ] **Step 1: Audit all hardcoded offsets/addresses**

Search for hex literals, FIELD() macros, and AOB patterns across the codebase. Document each one in a central `game_offsets.h` or similar.

- [ ] **Step 2: Create `game_version.h`**

```cpp
#pragma once
#include <string>

namespace GameVersion {
  // Current known game version this mod was built against
  constexpr const char* SUPPORTED_VERSION = "1.48";

  // If the mod can detect the game version at runtime, check compatibility
  bool isCompatible();

  // Log a warning if version mismatch detected
  void checkAndWarn();
}
```

- [ ] **Step 3: Add version detection** (if possible)

Try to read the game's version string from memory or from the exe's file version info. If the detected version doesn't match `SUPPORTED_VERSION`, log a prominent warning but still attempt to load.

- [ ] **Step 4: Ensure combo data is version-independent**

The combo JSON files already have version keys (V148, PREVIOUS). When 2.0 drops:
- Add a new version key (e.g., "V200") in each character's JSON
- Old combos remain under their version keys
- The character selection page now just shows the character, regardless of which game version is running

- [ ] **Step 5: Commit**
  ```bash
  git add include/game_version.h
  git commit -m "feat: add game version detection and compatibility check"
  ```

---

## Execution Order & Dependencies

```
Phase 1 (foundational)
  Task 1: per-character JSON files          ← do first
  Task 2: update loader                     ← depends on Task 1
  Task 3: Characters page in menu           ← depends on Task 2

Phase 2 (schema extension)
  Task 4: add tags/contributor to schema    ← depends on Task 2
  Task 5: filter UI                         ← depends on Task 4

Phase 3 (display)
  Task 6: contributor display               ← depends on Task 4

Phase 4 (tooling — independent of C++ work)
  Task 7: GitHub repo setup                 ← can run in parallel with Phase 2/3

Phase 5 (resilience — independent)
  Task 8: version abstraction               ← can run anytime
```

**Recommended order:** Tasks 1 → 2 → 3 → 4 → 5+6 (parallel) → 7 → 8

---

## Estimated Effort

| Phase | Tasks | Estimate |
|-------|-------|----------|
| Phase 1: Multi-Character | Tasks 1-3 | 2-3 sessions |
| Phase 2: Tags & Filters | Tasks 4-5 | 1-2 sessions |
| Phase 3: Contributor Display | Task 6 | 30 min |
| Phase 4: Community Pipeline | Task 7 | 1 session |
| Phase 5: 2.0 Resilience | Task 8 | 1 session |
| **Total** | | **~5-7 sessions** |

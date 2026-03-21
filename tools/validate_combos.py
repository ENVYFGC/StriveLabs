#!/usr/bin/env python3
"""Validate combo data JSON files for the GGST Frame Viewer mod.

Usage:
    python validate_combos.py [combo_data_dir]

Defaults to ../combo_data relative to this script's location.
Exit code 0 on success, 1 on any errors.
"""

import json
import re
import sys
from pathlib import Path

VALID_TAGS = {"corner", "midscreen", "meterless", "meter", "ch", "wallsplat"}
KNOWN_COMBO_KEYS = {"notation", "notes", "link", "tags", "contributor"}
YOUTUBE_RE = re.compile(
    r"^https?://(www\.)?youtube\.com/watch\?v=[\w-]+"
    r"|^https?://youtu\.be/[\w-]+"
)


def resolve_combo_dir(args):
    if len(args) > 1:
        return Path(args[1])
    return Path(__file__).resolve().parent.parent / "combo_data"


def validate_characters_json(combo_dir, errors):
    """Load and validate _characters.json. Returns set of valid character IDs."""
    path = combo_dir / "_characters.json"
    if not path.exists():
        errors.append(f"ERROR [_characters.json]: file not found in {combo_dir}")
        return set()

    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        errors.append(f"ERROR [_characters.json]: invalid JSON — {e}")
        return set()

    if not isinstance(data, dict) or "characters" not in data:
        errors.append('ERROR [_characters.json]: missing "characters" key')
        return set()

    characters = data["characters"]
    if not isinstance(characters, list) or len(characters) == 0:
        errors.append('ERROR [_characters.json]: "characters" must be a non-empty array')
        return set()

    valid_ids = set()
    for i, entry in enumerate(characters):
        ctx = f"_characters.json > character #{i + 1}"
        if not isinstance(entry, dict):
            errors.append(f"ERROR [{ctx}]: entry must be an object")
            continue
        cid = entry.get("id")
        name = entry.get("name")
        if not isinstance(cid, str) or not cid:
            errors.append(f"ERROR [{ctx}]: missing or empty \"id\"")
            continue
        if not re.match(r"^[a-z0-9_-]+$", cid):
            errors.append(f"ERROR [{ctx}]: id \"{cid}\" must be lowercase alphanumeric (plus _ or -)")
            continue
        if not isinstance(name, str) or not name:
            errors.append(f"ERROR [{ctx}]: missing or empty \"name\" for id \"{cid}\"")
            continue
        valid_ids.add(cid)

    return valid_ids


def validate_combo_entry(combo, ctx, errors, warnings):
    """Validate a single combo entry object."""
    if not isinstance(combo, dict):
        errors.append(f"ERROR [{ctx}]: combo entry must be an object")
        return

    # Required fields
    notation = combo.get("notation")
    if not isinstance(notation, str) or not notation.strip():
        errors.append(f"ERROR [{ctx}]: \"notation\" is missing or empty")

    notes = combo.get("notes")
    if not isinstance(notes, str) or not notes.strip():
        errors.append(f"ERROR [{ctx}]: \"notes\" is missing or empty")

    # Optional: link
    link = combo.get("link")
    if link is not None:
        if not isinstance(link, str):
            errors.append(f"ERROR [{ctx}]: \"link\" must be a string")
        elif link and not YOUTUBE_RE.match(link):
            errors.append(f"ERROR [{ctx}]: \"link\" is not a valid YouTube URL — got \"{link[:80]}\"")

    # Optional: tags
    tags = combo.get("tags")
    if tags is not None:
        if not isinstance(tags, list):
            errors.append(f"ERROR [{ctx}]: \"tags\" must be an array")
        else:
            for tag in tags:
                if not isinstance(tag, str):
                    errors.append(f"ERROR [{ctx}]: tag must be a string, got {type(tag).__name__}")
                elif tag not in VALID_TAGS:
                    errors.append(f"ERROR [{ctx}]: unknown tag \"{tag}\" — valid: {', '.join(sorted(VALID_TAGS))}")

    # Optional: contributor
    contributor = combo.get("contributor")
    if contributor is not None:
        if not isinstance(contributor, str):
            errors.append(f"ERROR [{ctx}]: \"contributor\" must be a string")
        elif not contributor.strip():
            errors.append(f"ERROR [{ctx}]: \"contributor\" is present but empty")

    # Warn on unknown keys
    unknown = set(combo.keys()) - KNOWN_COMBO_KEYS
    if unknown:
        warnings.append(f"WARN  [{ctx}]: unknown keys: {', '.join(sorted(unknown))}")


def validate_character_file(path, errors, warnings):
    """Validate a single character combo file."""
    filename = path.name

    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        errors.append(f"ERROR [{filename}]: invalid JSON — {e}")
        return

    if not isinstance(data, dict):
        errors.append(f"ERROR [{filename}]: top-level must be an object (version keys)")
        return

    for version_key, starters in data.items():
        vctx = f"{filename} > {version_key}"

        if not isinstance(starters, dict):
            errors.append(f"ERROR [{vctx}]: version value must be an object (starter keys)")
            continue

        for starter_key, starter_data in starters.items():
            sctx = f"{vctx} > {starter_key}"

            if not isinstance(starter_data, dict):
                errors.append(f"ERROR [{sctx}]: starter value must be an object")
                continue

            # note field
            note = starter_data.get("note")
            if note is not None and not isinstance(note, str):
                errors.append(f"ERROR [{sctx}]: \"note\" must be a string")

            # combos array
            combos = starter_data.get("combos")
            if combos is None:
                errors.append(f"ERROR [{sctx}]: missing \"combos\" array")
                continue
            if not isinstance(combos, list):
                errors.append(f"ERROR [{sctx}]: \"combos\" must be an array")
                continue

            for i, combo in enumerate(combos):
                cctx = f"{sctx} > combo #{i + 1}"
                validate_combo_entry(combo, cctx, errors, warnings)


def main():
    combo_dir = resolve_combo_dir(sys.argv)

    if not combo_dir.exists():
        print(f"ERROR: combo_data directory not found: {combo_dir}")
        sys.exit(1)

    errors = []
    warnings = []

    # Step 1: Validate character registry
    valid_ids = validate_characters_json(combo_dir, errors)

    # Step 2: Discover and validate character files
    json_files = sorted(combo_dir.glob("*.json"))
    seen_ids = set()

    for path in json_files:
        if path.name.startswith("_"):
            continue  # skip registry and other meta files

        char_id = path.stem
        seen_ids.add(char_id)

        if valid_ids and char_id not in valid_ids:
            errors.append(f"ERROR [{path.name}]: filename \"{char_id}\" does not match any character ID in _characters.json")
            continue

        validate_character_file(path, errors, warnings)

    # Step 3: Cross-check — warn about characters without combo files
    for cid in sorted(valid_ids - seen_ids):
        warnings.append(f"WARN  [_characters.json]: no combo file found for character \"{cid}\"")

    # Report
    print()
    for msg in errors:
        print(msg)
    for msg in warnings:
        print(msg)

    print()
    print(f"{len(errors)} error(s), {len(warnings)} warning(s).", end=" ")
    if errors:
        print("Validation FAILED.")
        sys.exit(1)
    else:
        print("Validation PASSED.")
        sys.exit(0)


if __name__ == "__main__":
    main()

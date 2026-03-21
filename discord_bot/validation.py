"""Combo entry validation — ported from tools/validate_combos.py."""

import re
from config import VALID_TAGS

YOUTUBE_RE = re.compile(
    r"^https?://(www\.)?youtube\.com/watch\?v=[\w-]+"
    r"|^https?://youtu\.be/[\w-]+"
)


def auto_tag(notation: str, notes: str) -> list[str]:
    """Generate tags from notation + notes text (mirrors C++ autoTagCombo)."""
    text = (notes + " " + notation).lower()
    tags = []
    if "corner" in text:
        tags.append("corner")
    if "midscreen" in text:
        tags.append("midscreen")
    if "meterless" in text:
        tags.append("meterless")
    if "50%" in text or "tension" in text:
        tags.append("meter")
    if text.startswith("ch ") or " ch " in text:
        tags.append("ch")
    if "ws!" in text or "wall" in text:
        tags.append("wallsplat")
    return tags


def validate_combo(notation: str, notes: str, link: str,
                   character_ids: set[str], character_id: str) -> list[str]:
    """Validate a combo submission. Returns list of error strings (empty = valid)."""
    errors = []

    if not notation or not notation.strip():
        errors.append("Notation is required.")

    if not notes or not notes.strip():
        errors.append("Notes are required (e.g. 'Midscreen Meterless').")

    if link and link.strip() and not YOUTUBE_RE.match(link.strip()):
        errors.append(f"Invalid YouTube URL: {link[:80]}")

    if character_id not in character_ids:
        errors.append(f"Unknown character: {character_id}")

    return errors


def build_combo_entry(notation: str, notes: str, link: str,
                      contributor: str) -> dict:
    """Build a combo entry dict ready for JSON insertion."""
    entry = {
        "notation": notation.strip(),
        "notes": notes.strip(),
        "link": link.strip() if link else "",
    }

    tags = auto_tag(entry["notation"], entry["notes"])
    if tags:
        entry["tags"] = tags

    if contributor:
        entry["contributor"] = contributor.strip()

    return entry

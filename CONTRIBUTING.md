# Contributing Combos

## Quick Start — Adding Combos to an Existing Character

1. Fork this repo
2. Open `combo_data/<character_id>.json` (e.g. `sol.json`)
3. Find the version section (e.g. `"V148"`) and the appropriate starter
4. Add your combo entry to the `"combos"` array
5. Run validation: `python tools/validate_combos.py`
6. Submit a PR

## Adding a New Character

1. Check `combo_data/_characters.json` — if the character is already listed, create `<id>.json` in `combo_data/`
2. If the character is NOT listed, add them to `_characters.json` first
3. Copy `tools/combo_template.json` as your starting point
4. Rename to `<character_id>.json` (use the `id` from `_characters.json`)

## Combo Entry Format

```json
{
    "notation": "cS > 6H > 236K~K > 5K > cS > HSVV WS!",
    "notes": "Midscreen 50% Tension",
    "link": "https://www.youtube.com/watch?v=...",
    "tags": ["midscreen", "meter"],
    "contributor": "YourName"
}
```

| Field | Required | Description |
|-------|----------|-------------|
| `notation` | Yes | Combo input sequence using GGST notation |
| `notes` | Yes | Brief description — position, meter cost, conditions |
| `link` | No | YouTube video demonstration URL |
| `tags` | No | Auto-generated from notes if omitted |
| `contributor` | No | Your name or handle |

## Notation Conventions

| Input | Meaning |
|-------|---------|
| `5` | Neutral |
| `6` / `4` | Forward / Back |
| `2` / `8` | Down / Up |
| `1` `3` `7` `9` | Diagonal directions (numpad) |
| `P` `K` `S` `H` `D` | Punch, Kick, Slash, Heavy Slash, Dust |
| `cS` / `fS` | Close Slash / Far Slash |
| `236K` | Quarter circle forward + K |
| `623H` | Dragon punch motion + H |
| `214S` | Quarter circle back + S |
| `j.H` | Jumping Heavy Slash |
| `dl.` | Delay |
| `>` | Separator between hits |
| `CH` | Counter hit (prefix) |
| `WS!` / `WB!` | Wall splat / Wall break |
| `RRC` `BRC` `PRC` | Red / Blue / Purple Roman Cancel |

## Valid Tags

| Tag | Meaning |
|-----|---------|
| `corner` | Requires corner position |
| `midscreen` | Works midscreen |
| `meterless` | No tension required |
| `meter` | Requires tension (50% or more) |
| `ch` | Counter hit starter |
| `wallsplat` | Leads to wall splat |

If you omit `tags`, they will be auto-generated from the `notes` text at runtime.

## Version Keys

- `"PREVIOUS"` — combos from before the current patch
- `"V148"` — patch v1.48 combos
- Future patches get new keys (e.g. `"V150"`)

## PR Guidelines

- One character per PR when possible
- Run `python tools/validate_combos.py` locally before submitting
- Include video links when available
- Use descriptive notes (position + meter cost at minimum)
- Keep notation consistent with existing entries

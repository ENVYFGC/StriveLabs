# StriveLabs

A UE4SS C++ mod for Guilty Gear Strive that runs in the game process and draws overlays directly onto the HUD. Built as a native DLL injected via UE4SS, using the game's own HUD rendering pipeline.

---

## Features

### Frame Bar
Displays a real-time frame advantage bar for both players, showing active, recovery, startup, and idle frames color-coded per scheme. Supports multiple color palettes (SF6, Classic, Dustloop, Colorblind), dash frame display, crossup detection, and a configurable fade effect between segments.

### Tension Overlay
An optional overlay that shows tension gain, burst meter, and current tension values per player. Each stat is independently configurable to show for Player 1, Player 2, both, or neither.

### Hitbox Viewer
Toggleable hitbox rendering that draws attack and hurt boxes on top of the gameplay view.

### Combo Browser (Local)
A built-in combo browser that lets you look up character-specific combo routes directly in training mode. Each combo shows the full input notation and links to a video demonstration you can watch without leaving the game. Combo data is stored locally in JSON format and ships with the mod.

### Combo Browser (API)
A live combo browser that pulls data directly from the in-game Combo Maker, letting you browse community-created combos in real time. Requires an internet connection. New combos created by players become available automatically without needing a mod update. Shows combo number for direct lookup, author, download count, likes, and clear rate.

### Detail Overlay
A floating panel that appears during combo browsing showing the selected combo's title, character tag, full move notation, author, recipe number, download count, and likes. The panel position, right margin, and width are all configurable. Notation wraps dynamically and the panel height adjusts to fit.

### Mod Menu
An in-game overlay menu navigated entirely with controller input. Organized into sections: Display, Timing and Input, Tension, Combos, Combo Browser, and Layout. Each section is a submenu with a Back option.

### Layout System
Every overlay element can be repositioned and resized from within the Layout submenu. Each element (Frame Bar, Tension Box, Detail Panel, Mod Menu) has its own submenu with a live ghost preview that updates in real time as values are adjusted. Settings persist across sessions via a plain-text config file.

---

## Technical Notes

- Written in C++ as a UE4SS CppMod, compiled as a DLL loaded by the game at startup
- Uses the game's native HUD `DrawText`, `DrawRect`, and `DrawLine` functions via UE4SS reflection
- All coordinates are in draw-space units scaled by `screen_width * 0.0003` so layouts are resolution-independent
- Combo notation is decoded from internal move names (e.g. `NmlAtk5D` to `5D`, `236K` specials) with CamelCase splitting for unrecognized names and suppression of internal property tokens
- API combos are fetched from the in-game Combo Maker endpoint using the player's active session token, parsed from `ggst_session.json`
- Config is persisted to `StriveLabs.txt` in the UE4SS working directory as key-value pairs

---

## Installation

Extract the contents of `StriveLabs.zip` into:

```
GUILTY GEAR STRIVE/RED/Binaries/Win64/
```

No additional setup required. Launch the game, enter training mode, and press F5 to open the menu.

---

## Controls

| Input | Action |
|---|---|
| F5 | Toggle menu |
| Up / Down | Navigate |
| Left / Right | Change value or enter submenu |
| Left on Back | Return to previous page |

---

## Build

Built with xmake against the UE4SS SDK. To compile:

```
xmake build StriveLabs
```

Output DLL is at `Binaries/Game__Shipping__Win64/StriveLabs/StriveLabs.dll`. Deploy by copying to `ue4ss/Mods/StriveLabs/dlls/main.dll` in the game directory. A packaging script at `D:/dev/package_release.py` produces a distributable zip matching the repository structure.

---

## Status

Early phase. Combo data coverage is limited and expanding. API features are experimental.

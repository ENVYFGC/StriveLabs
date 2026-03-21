# StriveLabs

A UE4SS mod for Guilty Gear Strive that gives you frame data, hitbox visualization, and a full combo library — all in-game, no alt-tabbing.

## What it does

- **Frame data overlay** — see startup, active, recovery frames in real time
- **Hitbox viewer** — visualize hurt/hitboxes during gameplay
- **Tension & gauge info** — track meter, burst, RISC
- **Combo browser** — browse community-contributed combos organized by character, version, and starter
- **Video playback** — watch combo demos without leaving the game
- **Tag filtering** — filter combos by position (corner/midscreen), meter usage, counter hit, etc.
- **Combo trials** — record and practice custom trial sequences

## Setup

1. Install [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) for GGST
2. Drop `StriveLabs.dll` into `ue4ss/Mods/StriveLabs/dlls/main.dll`
3. Add `StriveLabs : 1` to `Mods/mods.txt`
4. Place the `combo_data/` folder in your UE4SS directory
5. (Optional) Add `yt-dlp.exe` to the mod folder for video playback

Press **F5** in training mode to open the menu.

## Contributing combos

We're building a combo database for the full GGST roster and could use your help. Check out [CONTRIBUTING.md](CONTRIBUTING.md) for how to add combos — no coding required, just fill in some JSON fields.

There's also a Discord bot (`/addcombo`) that lets you submit combos without touching any files directly.

## Building from source

Requires [xmake](https://xmake.io/) and the UE4SS SDK.

```
xmake build StriveLabs
```

## License

This project is provided as-is for the GGST community. Not affiliated with Arc System Works.

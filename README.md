# StriveLabs

A UE4SS mod for Guilty Gear Strive that gives you frame data, hitbox visualization, and a full combo library — all in-game, no alt-tabbing.

## What it does

- **Frame data overlay** — see startup, active, recovery frames in real time
- **Hitbox viewer** — visualize hurt/hitboxes during gameplay
- **Tension & gauge info** — track meter, burst, RISC
- **Combo browser** — browse community-contributed combos organized by character, version, and starter
- **Video playback** — watch combo demos without leaving the game
- **Tag filtering** — filter combos by position (corner/midscreen), meter usage, counter hit, etc.

## Setup

1. Download the latest release from [Releases](https://github.com/ENVYFGC/StriveLabs/releases)
2. Extract the zip into your GGST install at `GUILTY GEAR STRIVE/RED/Binaries/Win64/`
3. That's it — UE4SS and everything else is included

Press **F5** in training mode to open the menu.

> **Optional:** Drop `yt-dlp.exe` into the `ue4ss/Mods/StriveLabs/` folder to enable in-game video playback for combo demos.

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

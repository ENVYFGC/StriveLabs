# StriveLabs

A UE4SS mod for Guilty Gear Strive that puts frame data, hitboxes, meter info, wall damage, replay takeover and recordings right inside the game, so you never have to alt tab.

It's built on top of WistfulHopes' [GGST Frame Viewer + Replay Takeover 0.2.4](https://github.com/WistfulHopes/UE4SSMods/releases/tag/replay-0.2.4).

## What's in it

- **Frame data overlay** that shows startup, active and recovery frames as you play.
- **Hitbox viewer** for hurtboxes and hitboxes.
- **Tension overlay** with meter gain, burst, tension pulse, tension penalty and wall damage.
- **Replay takeover** so you can jump in and play either side of a replay.
- **Recordings** that turn your training slots into .txt files you can edit, share and load back in.

## Installing

Grab the zip from the [Releases](../../releases) page. Works on the Steam and Xbox PC versions.

**Steam:** extract the zip into your `GUILTY GEAR STRIVE` folder (the one with `RED` in it) and that's it.

**Xbox PC / Game Pass:** grab the xbox zip and extract it into `C:\XboxGames\GUILTY GEAR STRIVE\`.

**Linux / Steam Deck:** same files as Steam, then add this to the game's launch options:

```
WINEDLLOVERRIDES="sensapi=n,b" %command%
```

Please don't use an `xinput1_3` override, it breaks Steam Input.

**Unverum:** StriveLabs works fine next to your Unverum pak mods, just install it by hand instead of using 1-Click.

## Controls

Open the menu with **F5**, or **Back(select) + Start** on a controller. Up and Down to move, Right to select, Left to go back.

| Key | What it does |
|-----|--------------|
| F1 | Frame meter |
| F2 | Hitboxes |
| F3 | Freeze |
| F4 | Advance one frame (hold it to step) |
| F5 | Menu |
| F6 | Set a rewind point and start takeover |
| F7 | Take over P1 (press again to stop) |
| F8 | Take over P2 (press again to stop) |
| F9 | Rewind to your saved point |

Don't like these? Change them on the **Keybinds** page. Keyboard, pad buttons, triggers and the left stick all work, and Backspace clears a bind. The **Takeover Binds** page lets you pick the buttons you play with during replay takeover.

## Recordings

**Save Slot N into txt** saves a training slot as a text file named after the date, time, frame count and opening inputs. **Load into Game's Slot N** puts a .txt back into a slot, and you play it with the game's own playback button. **Player Side** picks 1P or 2P. Your files go in `RED/Binaries/Win64/ue4ss/recordings/`.

The format is Eddienput notation: `W` wait, `X` tap, `[X]` hold, `]X[` release, `+` same frame, `#` comment. Use numpad 1 to 9, P K S H D, Dash, RC, FD and Respect. Loops work too, and each slot holds up to 2400 frames (40 seconds).

## Building

It builds as a UE4SS C++ mod with xmake inside an RE-UE4SS checkout. The DLL goes in `ue4ss/Mods/StriveLabs/dlls/main.dll`.

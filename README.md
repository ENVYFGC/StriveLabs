# StriveLabs

StriveLabs is a UE4SS mod for Guilty Gear Strive. It adds frame data, hitboxes, meter info, wall damage, replay takeover and recordings to the game itself, so you don't need anything open on the side.

It started as WistfulHopes' [GGST Frame Viewer + Replay Takeover 0.2.4](https://github.com/WistfulHopes/UE4SSMods/releases/tag/replay-0.2.4) and grew from there.

> StriveLabs is developed with AI assistance (Claude). Everything is tested in game before release.

## What it does

- A frame bar that shows startup, active and recovery frames as you play, with an optional row of inputs under it.
- Hitboxes and hurtboxes.
- Meters for tension gain, burst, tension pulse, tension penalty and wall damage.
- Replay takeover, so you can jump into either side of a replay and play it out yourself. Replay Assist can start you a moment early, so you're already in the flow when control is handed to you.
- Recordings. Save your training slots as .txt files you can edit, share and load back in, or record a player straight from a replay.
- A timeline with rewind, pause, frame step and record.
- A menu you can set up how you like. Move, resize, minimize and pop out any window, and pick your own colors and size.
- Character select remembers your last character, color, stage and music after a restart.

## Installing

Get the zip from the [Releases](../../releases) page. It works on both the Steam and Xbox PC versions.

On Steam, extract the zip into your `GUILTY GEAR STRIVE` folder (the one with `RED` in it).

On Xbox PC / Game Pass, get the xbox zip and extract it into `C:\XboxGames\GUILTY GEAR STRIVE\`.

On Linux or Steam Deck, use the same files as Steam and put this in the game's launch options:

```
WINEDLLOVERRIDES="sensapi=n,b" %command%
```

Please don't use an `xinput1_3` override, it breaks Steam Input.

If you use Unverum, StriveLabs works fine next to your pak mods. Just install it by hand instead of using 1-Click.

## Controls

F5 opens the menu, or Back(select) + Start on a controller. Up and Down to move, Right to select, Left to go back. The mouse works too.

| Key | What it does |
|-----|--------------|
| F1 | Frame bar |
| F2 | Hitboxes |
| F3 | Freeze |
| F4 | Advance one frame (hold it to keep stepping) |
| F5 | Menu |
| F6 | Set a rewind point |
| F7 | Take over P1 (asks first, press again to stop) |
| F8 | Take over P2 (asks first, press again to stop) |
| F9 | Rewind to your saved point |

You can rebind all of them on the Controls and Takeover pages. Keyboard, pad buttons, triggers and the left stick all work, and Backspace clears a bind. The buttons you play with during replay takeover are set on the Takeover page. Training Record, Training Playback and Training Reset Position (unbound by default) do the same as the game's own buttons, so you can put them on a key or a button combo.

## Replay takeover

In a replay, F7 or F8 pauses on the current frame and asks before taking over. Press the same key again for yes, or Esc for no. On a pad, Right on the d-pad is yes and Left is no. Stopping asks the same way, and takes you back to your rewind point if you set one.

With Replay Assist on (Takeover page, in seconds), F9 goes back a little before your rewind point and lets the replay play both sides. A bar counts down, and at the exact frame you picked, control is handed to you.

A small status line shows what's going on: replay or training, the rewind point, Replay Assist and who you're taking over. Drag it anywhere while the menu is open, or turn it off on the Takeover page.

## Recordings

Save Slot N into txt saves a training slot as a text file, named after the date, time, length and the first inputs. Load into Game's Slot N puts a .txt back into a slot, and you play it with the game's own playback button. Player Side picks 1P or 2P. Your files go in `RED/Binaries/Win64/ue4ss/recordings/`.

They use Eddienput notation: `W` wait, `X` tap, `[X]` hold, `]X[` release, `+` same frame and `#` for comments. Inputs are numpad 1 to 9, P K S H D, Dash, RC, FD and Respect. Loops work too. A slot holds up to 2400 frames, which is 40 seconds.

## Building

It's a UE4SS C++ mod, built with xmake inside an RE-UE4SS checkout. The DLL goes in `ue4ss/Mods/StriveLabs/dlls/main.dll`. The menu uses [Dear ImGui](https://github.com/ocornut/imgui) (MIT, in `third_party/imgui`) and the Barlow fonts (SIL Open Font License, in `ue4ss/StriveLabs_fonts`).

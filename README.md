# MechSounds — thocky keyboard sounds for Windows

Your keyboard is thocky. Your noise-canceling headphones don't know that.
MechSounds plays a mechanical switch sound through your headphones on
every keypress, in every app, on any Windows PC with any keyboard.

This is the Windows answer to MechPad (a Mac-only app that makes switch
sounds through a MacBook trackpad). Same idea, different trick: instead
of trackpad haptics, MechSounds listens to your key presses system-wide and plays a recording of
a real mechanical switch to your normal audio output.

## Get it

Download the latest release (exe + sounds, ready to run) from the
[Releases](../../releases) page, or build it yourself from the source
below. The exe needs a `sounds/` folder next to it — see
[Sound packs](#sound-packs).

## Build it

The app is a single C file. Build it with Zig (any recent version):

```sh
zig cc -target x86_64-windows-gnu -O2 -Wl,--subsystem,windows -o MechSounds.exe MechSounds.c -lwinmm -lgdi32
```

That produces a standalone `MechSounds.exe` — no installer, no
dependencies beyond stock Windows. Put it in a folder next to a
`sounds/` directory (see below) and double-click it.

## Use it

A chooser window opens first. The presets are grouped by switch
type — **Linear** (Rainy 75 Pro, NK Cream, Everglide Oreo, Leobog
Reaper), **Tactile** (Crystal Panda), **Topre** (HHKB Topre), **Clicky**
(IBM Model M) — click one to hear it. Or pick **Custom build** and
tune your own: switch type (linear / tactile / clicky), lubed or
not, case material (plastic / aluminum / brass), and mods (tape mod,
o-rings, both, neither). The custom build starts from a real
recording of that switch type and shapes it to match — lube and tape
soften it, aluminum adds a bright ping, brass deepens it, o-rings
shorten the tail. Pick a volume and hit **Start**. The window hides
to the tray icon, and the sounds play from then on. Your pick and
volume are remembered for next time.

- **Start with Windows:** tick the checkbox in the chooser (or
  right-click the tray icon → "Start with Windows"). When it starts
  with Windows it skips the chooser and just starts playing with
  your last saved switch and volume.
- **Change switches later:** double-click the tray icon to reopen
  the chooser, or right-click it and pick a switch from the menu.
  `Ctrl+Alt+P` cycles through them too.

## Controls

| Hotkey | What it does |
|---|---|
| `Ctrl+Alt+M` | Toggle sounds on/off |
| `Ctrl+Alt+P` | Cycle switch profile (shows the name on screen) |
| `Ctrl+Alt+Q` | Quit |
| Tray icon right-click | Profiles, toggle, chooser, start-with-Windows, exit |
| Tray icon double-click | Reopen the chooser |

## Switch profiles

All seven are **real recordings** from [Mechvibes](https://mechvibes.com/sound-packs).
It starts on **Rainy 75 Pro** (the fullest sound).
`Ctrl+Alt+P` cycles through:

| Profile | Character |
|---|---|
| Rainy 75 Pro | Kailh Cocoa on a WOB Rainy 75 Pro — creamy thock |
| NK Cream | NovelKeys Cream — deep, smooth, marbly thock |
| Crystal Panda | Keychron V1 Crystal Panda — crisp tactile thock |
| Everglide Oreo | Oreo linear — clean, medium-deep thock |
| HHKB Topre 45g | Real Topre — soft, muted, deep thock |
| IBM Model M | Buckling spring — loud, metallic click-clack |
| Leobog Reaper | Aula F75 Leobog Reaper — creamy, full linear |

One click per press: a key plays one recording when you press it
and nothing when you let go — holding a key never re-triggers.
Each key has its own sound: every profile has 6 slightly different
recordings, and each key is assigned one of them, so the same key
always sounds the same and different keys sound like themselves
(pressing A over and over never changes its sound).
Spacebar, Enter, Backspace and Tab play the spacebar's own, deeper
recording (each pitch-shifted so the big keys sound distinct).

## Latency

This is built so the sound never lags behind your fingers:

- Playback is MCI: one `play <alias> from 0` command per keypress
  (no separate seek) on a pool of pre-opened aliases.
- The keyboard hook does one thing (pass the key along) and returns
  immediately, so the keystroke itself is never delayed by the sound.
- Every sound plays out fully and blends: each alias plays one
  sound at a time, so a keystroke never cuts off the previous
  sound mid-tail — fast typing blends instead of chopping,
  and no keypress is ever silently dropped.
- The sounds are rendered at 0.4 peak (-8 dBFS) so overlapping
  keypresses don't sum past 0 dBFS and distort in the mixer, and
  the exe has a volume control (chooser window) if you want them
  quieter still.
- The exe shows the measured hook-to-play dispatch time in its tray
  tooltip (avg/max ms). The rest is Windows audio output, typically
  10–30 ms — total stays well under the ~50 ms where typing starts
  to feel disconnected from its sound.

## Sound packs

The recordings are **not in this repo** — the MechVibes packs are
free for personal use only, and their licenses don't allow
redistribution. To get sounds:

1. Download a pack from [Mechvibes](https://mechvibes.com/sound-packs).
2. Drop the zip in `real-packs/`, add a line to
   `build_real_profiles.py`, and run it — it writes
   `sounds/<Name>/down1-6.wav` and `space1.wav` (44.1 kHz mono WAVs).
3. Add the name to the profile list in `MechSounds.c`
   (and `MechSounds.py` / `MechSounds.ahk` if you use those).

Packs used in the release build:
- Rainy 75 Pro (Kailh Cocoa) — WOB Rainy 75 Pro
- NK Cream — NovelKeys Cream
- Crystal Panda — Keychron V1 Crystal Panda
- Everglide Oreo — EG Oreo
- HHKB Topre 45g — HHKB Pro 2 with stock Topre 45g
- IBM Model M — IBM Model M 1985, buckling spring
- Leobog Reaper — Aula F75 Max, Leobog Reaper switches

Each clip is cut around its loudest point, since some packs hide the
click 100–200 ms into the window.

## Notes

- **Sound comes out of your default audio device.** If your
  noise-canceling headphones are the default output, that's where the
  thock goes. If sound comes out of your speakers instead, switch the
  default output to your headphones in Windows sound settings.
- **Any keyboard works** — USB, Bluetooth, laptop keyboard. It hears
  key events, not hardware, so swapping keyboards changes nothing.
- **Admin windows:** keys typed into apps running as administrator
  (Task Manager, some installers) won't make sounds unless you also
  run the app as administrator (right-click → Run as administrator).
- **Games with anticheat:** keyboard hooks can look suspicious to
  anticheat. Toggle it off with `Ctrl+Alt+M` before launching a
  competitive game.
- The sounds are real recordings, not synthesized: the first
  version synthesized its switch sounds, and the loudest one had
  44% of its attack energy in a single 480 Hz tone — which is why
  it sounded like a Windows error ding. Real recordings don't have
  that problem, and they're full-spectrum instead of thin.

## Files

- `MechSounds.c` — the app. Builds with Zig (see above).
- `MechSounds.py` — same app as a Python script (stdlib only).
  Run `python MechSounds.py` next to a `sounds/` folder. Starts on
  Rainy 75 Pro with no chooser window; the chooser and
  start-with-Windows options are exe-only.
- `MechSounds.ahk` — same app as an AutoHotkey v2 script.
  Install AutoHotkey v2, then double-click it. Same as the Python
  version: starts on Rainy 75 Pro, no chooser window.
- `build_real_profiles.py` — converts Mechvibes packs in
  `real-packs/` into the `sounds/` profiles
- `synth_sounds.py` — the old script that synthesized switch sounds;
  kept for reference, no longer used (the synth profiles were
  replaced by real recordings in v6)

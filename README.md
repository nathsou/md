# Dread Recomp

A static recompilation of **Metroid Dread** (Nintendo Switch, 2021, version 1.0.0) into a native **macOS** application
(Apple Silicon, Metal). The game's own ARM64 code is translated to C ahead of time and compiled for your Mac; the
Switch's operating system, GPU API (NVN), audio, video decoding and input are replaced by host implementations. It is
not an emulator: the game runs as a native program.

> [!IMPORTANT]
> **No Nintendo files are included in this repository** - no game code, no assets, no keys, no generated C. You need
> your own legally obtained copy of Metroid Dread **1.0.0** (the cartridge / base version). The game code is
> recompiled on your machine from your own files. Instructions for dumping games or keys will not be provided.

> [!NOTE]
> This project was built almost entirely with AI (Anthropic's Claude, through Claude Code), directed and reviewed by a
> human. It is an early, experimental work in progress: expect bugs.

![The title screen](docs/screenshots/title.jpg)

| | |
|---|---|
| ![Artaria, the opening area](docs/screenshots/artaria.jpg) | ![Cataris](docs/screenshots/cataris.jpg) |
| ![Ghavoran](docs/screenshots/ghavoran.jpg) | ![Ferenia](docs/screenshots/ferenia.jpg) |
| ![Itorash, near the end of the game](docs/screenshots/itorash.jpg) | |

<sub>Screenshots of the game running in Dread Recomp on an M4 Pro Mac (from the opening to the last area, captured by
`tools/checkpoints.sh`). Metroid Dread © Nintendo.</sub>

## Status

- Boots, plays its movies, runs the title screen, save files and the whole opening, and is playable with a gamepad or
  the keyboard.
- Every one of the 832 unique shader stages the game ships translates to Metal and compiles (`make corpus`).
- Checked against a ladder of 54 save files spanning the whole game (every area, from the opening to Raven Beak): each
  one loads and runs three minutes of scripted running, jumping and shooting without crashing, and renders correctly
  (`tools/checkpoints.sh`, see [Testing](#testing)). This is not a full playthrough: nobody has finished the game on
  it yet.
- Not done: an in-game menu, audio polish (the I3DL2 reverb effect the game uses is not implemented yet), Linux and
  Windows (the Vulkan backend from LASRecomp is there but has not been built), version 2.1.0.

See [PLAN.md](PLAN.md) for the design, the full status and the known problems.

## Building

Requirements: an Apple Silicon Mac, Xcode command line tools, [Homebrew](https://brew.sh) `llvm` (clang), `sdl3`,
`ffmpeg`, Python 3 and [Bun](https://bun.sh) (only for extracting the game).

```sh
brew install llvm sdl3 ffmpeg
```

1. **Your game files.** Put your own dump of Metroid Dread 1.0.0 (`.xci`) in the repository folder, with your own
   `prod.keys` (and `title.keys` if you have one) next to it, then extract it:

   ```sh
   bun extract-xci.mts "Metroid Dread.xci"
   ```

   This writes `exefs/` (the game code: `main`, `sdk`, `subsdk0`, ...) and `romfs/` (the game data) next to it. If you
   already have extracted `exefs/` and `romfs/` folders, put those there instead. All of these stay out of git
   (`.gitignore`).

2. **The recompiler's Python environment** (once):

   ```sh
   python3 -m venv .venv && .venv/bin/pip install unicorn capstone lz4
   ```

3. **Recompile the game code** from your `exefs/` into C (`generated/`, about 25 seconds), then **build** the runtime
   and the game library (a few minutes the first time):

   ```sh
   make gen
   make -j10
   ```

4. **Play:**

   ```sh
   ./build/dreadrecomp .
   ```

   Saves go to `save/`, settings to `dreadrecomp.ini`, shader caches to `cache/` - all in the repository folder.

## Controls

Any controller SDL recognises (Nintendo layout by position: right face button = A, bottom = B, top = X, left = Y; in
the game B jumps and Y shoots), or the keyboard:

| Switch | Keyboard |
|---|---|
| Left stick | W A S D |
| D-pad | Arrow keys |
| A / B / X / Y | K / J / L / I |
| L / R | Q / E |
| ZL / ZR | 1 / 3 |
| Plus / Minus | Enter / Backspace |
| Stick clicks | F / keypad 0 |
| Right stick | Keypad 8 / 5 / 4 / 6 |

Rebind in `dreadrecomp.ini` (`bind_key_*` / `bind_pad_*` lines). F11 toggles fullscreen. A controller SDL doesn't know
can be added with a mapping line in `gamecontrollerdb.txt` ([SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB)).

## Testing

- `make corpus` - translates every shader in `romfs/system/shd` and compiles it with Metal.
- `tools/checkpoints.sh <dir> [jobs]` - boots each folder of a save-file ladder (folders holding
  `profile0/*.bmssv`; for example the 54 checkpoints of
  [ZacharyACoon/MetroidDreadSaveFiles](https://github.com/ZacharyACoon/MetroidDreadSaveFiles)) from a scratch copy of
  its save, continues it, explores for a few minutes (`EXPLORE=<seconds>`) and captures frames to `build/cp/<NNN>/`;
  `tools/checkpoint_sheet.sh <dir>` puts them next to each checkpoint's own screenshot.
- `LARECOMP_RECORD=<file>` records your inputs while you play; `LARECOMP_REPLAY=<file>` plays them back.
- More test hooks (scripted input, screenshots, per-pass frame dumps, a virtual gamepad) are listed in PLAN.md.

## How it works

- `tools/gen.py` (with `lifter.py` and `simd_c.py`) translates each module of the game (`main`, `sdk`, `subsdk0`) to
  C: one C function per guest function, guest registers in a `Cpu` struct, guest addresses equal to host addresses,
  indirect branches through a dispatch table, FP/SIMD as inline ARM64 assembly.
- `runtime/` replaces the Switch: a small Horizon kernel (threads, synchronisation, memory), IPC services (file
  system over `romfs/`, saves, HID, audio renderer, time, settings, applets), NVN on Metal with a Maxwell shader
  translator, and video playback on FFmpeg.
- See [PLAN.md](PLAN.md) for the details and the bugs found along the way.

## Credits and licences

- Built on [LASRecomp](https://github.com/peiikai/LASRecomp) by Peiikai (MIT; see `LICENSE-LASRecomp` and
  `UPSTREAM.md`), the static recompilation of Link's Awakening this project started from.
- [nxnative](https://github.com/seedds/nxnative)'s notes on Metroid Dread pointed at several of the bugs fixed here.
- Third-party code in `third_party/`: Dear ImGui (MIT), SMAA (MIT), BIZ UDPGothic (SIL OFL) and DejaVu fonts (free
  licence) as stand-ins for the Switch's system fonts.
- Metroid Dread is © Nintendo. This project is not affiliated with or endorsed by Nintendo.

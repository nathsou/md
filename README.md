# nx

A static recompilation engine for **Nintendo Switch** games, producing native **macOS** applications (Apple Silicon,
Metal). A game's own ARM64 code is translated to C ahead of time and compiled for your Mac; the Switch's operating
system, GPU API (NVN), audio, video decoding and input are replaced by host implementations. It is not an emulator:
the game runs as a native program.

| Game | Versions | State |
|---|---|---|
| **Metroid Dread** | 1.0.0, 2.1.0 | Playable from the opening to the last area (54 save checkpoints checked) |
| **Animal Crossing: New Horizons** | 1.0.0, 2.0.7 | Playable: the intro, character creation, the opening movie, arriving on the island and playing there |

Any other game boots through the same engine without a profile (its title ID and code come from its own files); a
profile under `games/<id>/` adds what a game needs on top (see [Adding a game](#adding-a-game)).

> [!IMPORTANT]
> **No Nintendo files are included in this repository** - no game code, no assets, no keys, no generated C. You need
> your own legally obtained copies of the games. The game code is recompiled on your machine from your own files.
> Instructions for dumping games or keys will not be provided.

> [!NOTE]
> This project was built almost entirely with AI (Anthropic's Claude, through Claude Code), directed and reviewed by a
> human. It is an early, experimental work in progress: expect bugs.

![Metroid Dread's title screen](docs/screenshots/title.jpg)

| | |
|---|---|
| ![Artaria, the opening area](docs/screenshots/artaria.jpg) | ![Cataris](docs/screenshots/cataris.jpg) |
| ![Ghavoran](docs/screenshots/ghavoran.jpg) | ![Ferenia](docs/screenshots/ferenia.jpg) |
| ![Itorash, near the end of the game](docs/screenshots/itorash.jpg) | |

<sub>Metroid Dread running in nx on an M4 Pro Mac (from the opening to the last area, captured by
`tools/checkpoints.sh`). Metroid Dread © Nintendo.</sub>

## Status

- **Metroid Dread** boots, plays its movies, runs the title screen, save files and the whole game's areas, and is
  playable with a gamepad or the keyboard at your display's refresh rate (120 fps on a ProMotion Mac). Every one of
  the 832 unique shader stages it ships translates to Metal and compiles. Checked against a ladder of 54 save files
  spanning the whole game: each loads and runs minutes of scripted play without crashing and renders correctly
  (`tools/checkpoints.sh`). Nobody has finished the game on it yet.
- **Animal Crossing: New Horizons** boots to the title screen, runs the whole intro (Tom Nook's questions, name and
  birthday, the character creator, the island choice), plays the opening movie with its audio and lands on the island,
  where you can walk around and play, at 120 fps.
- **Game updates** are applied when extracting (`--update`): the update's code and its romfs patch over the base
  game's.
- Not done: an in-game menu, online features, Linux and Windows (the Vulkan backend
  from LASRecomp is there but has not been built).

See [PLAN.md](PLAN.md) for the design, the full status and the known problems.

## Building

Requirements: an Apple Silicon Mac, Xcode command line tools, [Homebrew](https://brew.sh) `llvm` (clang), `sdl3`,
`ffmpeg`, `zstd`, Python 3 and [Bun](https://bun.sh) (only for extracting games).

```sh
brew install llvm sdl3 ffmpeg zstd
python3 -m venv .venv && .venv/bin/pip install unicorn capstone lz4
```

1. **Extract your game files** into `data/<name>/` (any name; it is what you pass as `GAME` below). Put your own
   `prod.keys` (and `title.keys` if you have one) in the repository folder or next to the dump:

   ```sh
   bun extract.mts "Metroid Dread.xci" data/dread
   bun extract.mts "Metroid Dread.xci" data/dread-2.1.0 --update "Metroid Dread Update 2.1.0.nsp"
   bun extract.mts "Animal Crossing New Horizons.nsp" data/acnh
   bun extract.mts "Animal Crossing New Horizons.nsp" data/acnh-2.0.7 --update "Animal Crossing New Horizons v2.0.7.nsp"
   ```

   Each writes `exefs/` (the game code: `main`, `sdk`, `subsdk0`, ...) and `romfs/` (the game data). XCI (cartridge)
   and NSP (eShop) dumps both work; `--update` applies an update NSP on top. `data/` can be a symlink to another disk.
   All of these stay out of git (`.gitignore`).

2. **Recompile the game code** into C (`generated/<name>/`, under a minute), then **build** the app and the game's
   library (`build/nx`, shared by every game, and `build/<name>/game.dylib`; a few minutes the first time):

   ```sh
   make gen GAME=acnh
   make -j10 GAME=acnh
   ```

3. **Optional, recommended: prebuild the shaders** (`make corpus GAME=acnh`, see [Shaders](#shaders)).

4. **Play:**

   ```sh
   ./build/nx data/acnh
   ```

   The app picks the game library built for those game files. Saves go to `save/<game>/` (photos taken in a game,
   such as Animal Crossing's camera app, to `save/<game>/album/`), settings to `nx.ini`, shader caches to
   `cache/<game>/`, all in the repository folder.

## Settings

`nx.ini` (written on first launch, in the repository folder) holds the options; there is no in-game menu yet.

- `frame_generation = off | 60 | 120`: for a game that runs at 30 fps (Animal Crossing), images are generated between
  its frames from the motion between them, so the window shows 60 or 120 a second (at most the display's refresh
  rate). The game itself still runs at 30, so its speed stays right. It costs about one display frame of latency. No
  effect on a game already presenting at that rate (Metroid Dread).
- `frame_generation_method = metalfx | flow`: how. `metalfx` (default, macOS 26 or later): at 60, Apple's MetalFX frame
  interpolation, fed with the game's depth and with motion vectors nx draws for it (each object's vertex shader runs
  again with the previous frame's data) and the HUD apart, so it stays still; the cleanest images, and lighter on the
  GPU than `flow`; drawing the motion vectors costs the game about 5% of its frame rate. `flow`, and 120 in any case: images from the optical flow between the frames; fast-moving details
  (legs while running) can ghost a little.
- `resolution`: the internal rendering resolution. `match` (default) follows the window's size; `1x` to `4x` render
  at 1080, 2160 (4K), 3240 or 4320 lines whatever the window, scaled to fit it; `dynamic`, `75%`, `50%` and `25%`
  lower it from `match` for slower GPUs. The game still lays out its screen for 1080p, so the HUD and text are
  rendered sharper, not smaller.

## Controls

Any controller SDL recognises (Nintendo layout by position: right face button = A, bottom = B, top = X, left = Y), or
the keyboard:

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

Rebind in `nx.ini` (`bind_key_*` / `bind_pad_*` lines). F11 toggles fullscreen. A controller SDL doesn't know can be
added with a mapping line in `gamecontrollerdb.txt` ([SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB)).

## Shaders

Switch games ship compiled Maxwell GPU shaders; nx translates them to Metal when a draw first needs them, which can
hitch on a first visit. To avoid that:

- `games/<id>/shader_variants.txt` lists the pipeline variants (shader pairs and the render state Metal needs at
  compile time) recorded while playing (`LARECOMP_RECORD_KEYS=games/<id>/shader_variants.txt`, for instance during
  the checkpoint runs). The lists hold hashes and render state only, no game code.
- At startup nx finds each listed shader in your game files (an index of their shader containers, built once per
  game version: about a minute for Animal Crossing's 100,000 files, then cached in
  `cache/<game>/shader_index-<build ID>.txt`) and builds the pipelines before the first frame (under a second once
  cached). Anything not listed, or not found in the files, still compiles on first use.
- `make corpus GAME=<name>` translates and compiles every shader the game ships (a check of the translator: about
  40 minutes for Animal Crossing's 9,558, `SKIP_CHECK=1` skips it), then builds the listed variants into a Metal
  binary archive (`cache/<game>/shaders-<build ID>.metalarchive`) that later launches load instead of compiling.

## Testing

- `make corpus GAME=<name>`: translates every shader in the game's files and compiles it with Metal (Metroid Dread:
  all 832; Animal Crossing: all but 7 of 9,558, agl debug shaders using a multisample fetch and a per-texel-offset
  gather the translator doesn't do yet).
- `tools/checkpoints.sh <dir> [jobs]` (Metroid Dread): boots each folder of a save-file ladder (folders holding
  `profile0/*.bmssv`; for example the 54 checkpoints of
  [ZacharyACoon/MetroidDreadSaveFiles](https://github.com/ZacharyACoon/MetroidDreadSaveFiles)) from a scratch copy of
  its save, continues it, explores for a few minutes (`EXPLORE=<seconds>`) and captures frames to `build/cp/<NNN>/`;
  `GAME_DIR=data/dread-2.1.0` runs another version.
- `tools/acnh_saves.sh <dir> [jobs]` (Animal Crossing): boots each save folder (`main.dat`, `mainHeader.dat`,
  `Villager*/`: a save dump, or a copy of `save/acnh/save_0`) from a scratch copy, in the day and at night, walks the
  island, opens the phone and the pockets, and captures frames to `build/acnh_cp/<save>-{day,night}/`. A resident
  belongs to the Switch account that made it: the runner plays as that account (`LARECOMP_USER_ID`, which
  `tools/acnh_save.py <save dir>` prints), so the game doesn't start a new resident instead.
- `LARECOMP_INPUT="frame:BUTTON:frames,..."` scripts the controller, `LARECOMP_SHOT_AT=frame,...` saves screenshots;
  `LARECOMP_RECORD=<file>` records your inputs while you play and `LARECOMP_REPLAY=<file>` plays them back.
  `LARECOMP_TIME_OFFSET=<seconds>` shifts the clock the game sees (Animal Crossing's time of day).
- More test hooks (per-pass frame dumps, a virtual gamepad, shader debugging) are listed in PLAN.md.

## How it works

- `tools/gen.py` (with `lifter.py` and `simd_c.py`) translates each module of a game (`main`, `sdk`, `subsdk*`) to
  C: one C function per guest function, guest registers in a `Cpu` struct, guest addresses equal to host addresses,
  indirect branches through a dispatch table, FP/SIMD as inline ARM64 assembly.
- `runtime/` replaces the Switch: a small Horizon kernel (threads, synchronisation, memory), IPC services (file
  system over `romfs/`, saves, HID, audio renderer, time, settings, applets), NVN on Metal with a Maxwell shader
  translator, and video playback (VP9, Vorbis, Opus) on FFmpeg.
- `games/<id>/` holds what is specific to one game: its versions (by the main program's build ID), the addresses its
  patches and hooks use for each version, and engine behaviours it opts into.

## Adding a game

1. Extract it (`bun extract.mts <dump> data/<name>`), `make gen GAME=<name>` and `make GAME=<name>`, and run it. With
   no profile it runs on the engine alone, keyed by its title ID.
2. When it needs game-specific work, add `games/<id>/`: `profile.h` (its name and versions, by the build ID `make gen`
   prints), `game.c` (the profile: hooks, boot fixes, flags), `patches.py` (per-instruction patches and hook sites for
   the generator) and one `patches_<version>.h` / `runtime_<version>.h` address table per version. `games/acnh` is a
   minimal example, `games/dread` one with hooks.
3. Most fixes belong in the engine, not the profile: missing services, NVN features and shader instructions are
   shared by every game.

## Credits and licences

- Built on [LASRecomp](https://github.com/peiikai/LASRecomp) by Peiikai (MIT; see `LICENSE-LASRecomp` and
  `UPSTREAM.md`), the static recompilation of Link's Awakening this project started from.
- [nxnative](https://github.com/seedds/nxnative)'s notes on Metroid Dread pointed at several of the bugs fixed here.
- Third-party code in `third_party/`: Dear ImGui (MIT), SMAA (MIT), BIZ UDPGothic (SIL OFL) and DejaVu fonts (free
  licence) as stand-ins for the Switch's system fonts.
- Metroid Dread and Animal Crossing: New Horizons are © Nintendo. This project is not affiliated with or endorsed by
  Nintendo.

# Metroid Dread static recompilation: plan

Goal: run *Metroid Dread* (Switch, 2021) natively on macOS (Apple Silicon, Metal) and Linux (Vulkan), then
Windows (Vulkan), by statically recompiling the game's AArch64 code to C and replacing the Switch OS, GPU API,
audio and input with host implementations. No Nintendo code or assets are ever committed or distributed: like
LASRecomp, the game code is generated and compiled on the player's machine from their own dump.

## 0. Status (2026-10-25)

**Dread 1.0.0 boots, plays its movies, reaches the title screen and the save files, plays the opening, and is
controllable in the first area on macOS (Apple Silicon, Metal).** Rendering is not right yet (see below).

Build and run (game files in `exefs/` and `romfs/` here, from `bun extract-xci.mts "Metroid Dread.xci"`):

```
python3 -m venv .venv && .venv/bin/pip install unicorn capstone lz4
make gen          # lift main, sdk, subsdk0 to C (about 25 s)
make -j10         # runtime + game library (about 2-3 min the first time)
./build/dreadrecomp .
```

Test hooks inherited from LASRecomp: `LARECOMP_INPUT="frame:A:frames,..."` (scripted pad),
`LARECOMP_SHOT_AT=frame,...` (build/frames/), `LARECOMP_DEBUG_FRAME=n` (per-draw log and per-pass images),
`LARECOMP_SAVE_MSL=1` (translated shaders to build/msl/), `LARECOMP_NO_AUDIO=1`.

Done (phases 0 to 2, part of phase 3):
- LASRecomp's core vendored, and the Link's Awakening layer removed or stubbed (`runtime/nvn/la_leftovers.c` and the
  no-op `texpatch.h` / `recolour.h` / `screen_filter.h` still hold its dead hooks).
- Generator: `.eh_frame` FDE ranges stop ADR/ADRP-derived jump-table bases from splitting functions. Jump tables went
  from 41 to 519 resolved in `sdk` (27 to 361 in `main`) and fixed musl's `vfprintf`. All three modules lift with
  **zero unsupported instructions**, and the 400 MB of C compiles without errors.
- Three modules (`main`, `sdk`, `subsdk0` at 0x7076000000). The memory layout moved `sdk` past `main`'s 1.8 GB
  bss. SDK 11's `nn::init::Start` takes three callbacks.
- Kernel/services: "used memory" counts module images; the program id is Dread's; `audren`'s work-buffer size comes
  from its parameters (a flat 16 MB had overflowed Dread's 6 MB sound heap); the romfs marker is `system/`.
- Movies: `movie::Decoder` replaced on FFmpeg (VP9 into the game's NVN texture as NV12, sync signalled); `subsdk0`'s
  own extractor (libwebm), Buffer and MediaData run recompiled.
- NVN/Metal: the float `ClearColor` kind was garbage (an upstream bug); RGBX textures get alpha 1 through sampling
  views (Metal refuses swizzle plus render target); `BindProgram` per stage (null unbinds); partial `WriteTexels` in
  place; vertex shaders without fast math or FP contraction; depth `EQUAL` without writes is tested as biased `LEQUAL`
  (the prepass and G-buffer vertex shaders disagree in the last bits).

Graphics fixes since (commits after the first snapshot):
- **LDC's offset is signed** with an index register. Read as unsigned, every static mesh's x row came from past its
  buffer, which caused the horizontal streaks, the black near geometry and the prepass/G-buffer depth mismatch. Exact
  depth `EQUAL` and fast-math vertex shaders work again (the biased-`LEQUAL` workaround is now opt-in:
  `LARECOMP_EQUAL_LE=1`).
- TXD (sampling with explicit derivatives), AL2P and indexed vertex attribute loads, and the −16 self-branch at the
  end of code.
- Depth-only Metal pipelines for `BindProgram(null, fragment)`.
- **Shader corpus:** `make corpus` translates and compiles with Metal all 832 unique shader stages in
  `system/shd`. All pass; the GLSL translation passes with `CORPUS_ARGS=--glsl`.

Rendering now matches reference screenshots of the opening area (the Chozo statue room) and looks right in the
first caves, at 60 fps (120 presents a second on a 120 Hz display).

Known problems, in priority order:
1. **Audio:** the I3DL2 reverb effect (audio renderer effect type 5), which NintendoWare Atk uses, isn't implemented
   (only type 4 reverb is), and nothing has been checked by ear.
2. **Graphics, unverified areas:** only the opening, its caves and the cutscenes have been looked at. EMMI zones,
   other regions, the hologram videos (several at once) and boss fights haven't. Suit colours in dark caves look
   bluish; this isn't confirmed against the real game.
3. Unhandled `am` commands (logged as ok), `ectx:aw` is a logging stub, and `nfp` (amiibo) is a stub.
4. No menu or start screen yet (`runtime/ui/menu.mm` is a stub; F11 toggles fullscreen); the on-device game-code
   build (`runtime/gamebuild.c`) still knows only two modules.
5. Vulkan (Linux/Windows) hasn't been built. It shares the translator fixes but not the depth-only pipelines, and
   neither its NV12 movie upload nor its RGBX handling has been checked against the Metal changes.

## 1. What LASRecomp does (the model to follow)

[peiikai/LASRecomp](https://github.com/peiikai/LASRecomp) (MIT, created 2026-10-08, about 76k lines of runtime and
tools, beta) ships *Link's Awakening* on macOS, Windows and Linux this way. It was read at commit `9e8efeb`
(2026-10-25). Its architecture, layer by layer:

| Layer | How LASRecomp does it | Where |
|---|---|---|
| Function discovery | `.eh_frame_hdr` FDEs + defined dynsym functions + `R_AARCH64_RELATIVE` targets + every `bl` target + code addresses built by `ADR` / `ADRP+ADD`; split functions at any outside branch target until a fixpoint | `tools/gen.py`, `tools/analyze.py` |
| CPU lifting | Python lifter, one C function per guest function (`void f_<addr>(Cpu *c)`); registers and NZCV live in a `Cpu` struct; direct calls are C calls, tail branches use `musttail` | `tools/lifter.py`, `runtime/cpu.h` |
| Indirect control flow | `blr`/`br` go through `rt_dispatch` (address → function table, binary search plus a per-thread cache, always tail-called). Jump tables are resolved at generation time by single-stepping the bounds-checked lookup in Unicorn; unresolved `br` falls back to a `switch` over every instruction in the function | `tools/gen.py`, `runtime/dispatch.c` |
| FP/SIMD | On arm64 hosts: batches of the original instructions re-emitted as inline asm (bit-exact). Elsewhere: portable C lowering of all ARMv8.0 FP/AdvSIMD/crypto/CRC32 (x86 builds need AVX2+FMA) | `tools/simd_c.py`, `runtime/simd.h` |
| Atomics | `LDXR/STXR` emulated with compare-and-swap; `DMB/DSB` become `__atomic_thread_fence` | `runtime/cpu.h` |
| Memory | Guest address == host address: a 12.5 GB window reserved at `0x70_0000_0000` (fits the 39-bit guest space; avoids macOS's reserved range), so loads and stores are plain pointer accesses | `runtime/rt.h`, `runtime/main.c` |
| Modules | Loads `main` and `sdk` NSOs itself (LZ4, relocations, symbol binding); **both** are recompiled. `rtld` is not used. libc `mem*`/`str*` are swapped for host versions | `runtime/loader.c`, `runtime/hle/libc.c` |
| OS | A small Horizon kernel at the SVC level: handles, guest threads on host pthreads (256 MB host stacks), mutex/condvar/address arbitration, memory mapping, events. HIPC/CMIF IPC to host services: `sm:`, `fsp-srv` (romfs from a host folder, saves), `hid`, `audren:u`/`audout:u`, `time`, `set`, `acc`, `appletOE`, `nfp`, `pctl`, `aoc`, `apm`, `lm` | `runtime/kernel.c`, `runtime/ipc.c`, `runtime/services/` |
| Graphics | **NVN replaced at the API level**: `nvnBootstrapLoader` / `nvnDeviceGetProcAddress` hand out host stubs (~517 NVN functions). Command buffers are recorded into guest memory as compact records and replayed by Metal or Vulkan backends. An in-house Maxwell (SM5) shader translator emits MSL or GLSL→SPIR-V; shaders are translated ahead of time; ASTC decode, Tegra block-linear swizzling | `runtime/nvn/` |
| Video | `movie::` (nnMedia) API reimplemented on FFmpeg | `runtime/hle/movie.c` |
| Game changes | Per-instruction patches and runtime hooks, each checked against the original instruction word; per-version address tables keyed by the NSO build ID; host overrides of guest functions | `tools/patches.py`, `runtime/hle/`, `runtime/game_addresses/` |
| Distribution | Release builds contain no game code: the start screen runs the bundled generator and a bundled `zig cc` on the player's files to build `lasgame.*`, a shared library compiled at `-O1`, with a resumable build cache | `runtime/gamebuild.c`, `runtime/build_cache.cpp` |

Takeaways for Dread: the CPU, kernel, IPC, NVN and build-pipeline layers are game-agnostic. The game-specific parts
are `runtime/hle/*` (randomizer and features), `tools/patches.py`, the address tables, the UI and the randomizer.

## 2. What Dread looks like (measured from our dump)

Dump: base game **1.0.0** cartridge, `main` build ID `49161d9ccbc15df944d0b6278a3c446c006b0be8`, SDK 11.4.3.
Internal name *cazadora* (`…\apps\gameapp\Retail\cazadora.nss`).

| Module | Code | Functions (eh_frame) | Symbols | Notes |
|---|---|---|---|---|
| `main` | 17.96 MiB (4.71 M instructions) | 102,288 | stripped: 30 exported functions, 547 imports | **bss is 1.80 GB** (a static engine arena). 43,786 `blr`, 5,246 `br`, 11,353 exclusive loads, about 4% FP/SIMD. 2,751 static initialisers. Links NintendoWare **Atk** (audio) statically, plus an embedded Lua 5.1 VM and Box2D strings |
| `sdk` | 5.55 MiB | 29,434 | 23,099 named functions | nnSdk + NVN + libz 11.4.3 |
| `subsdk0` | 3.29 MiB | 9,844 | 8,794 named functions | NintendoSDK **movie** library 11.4.3 (video playback) |
| `rtld` | tiny | 26 | | not needed |

What `main` imports: `nn::audio` 144, `nn::os` 55, `nn::hid` 31, `nn::nfp` 26 (amiibo), `nn::fs` 22,
`movie::` 36, `nn::oe` 13, `nn::prepo` 6, `nn::account` 4, `nn::time` 4, `nn::vi` 4, plus libc and the NVN entry
points.

Comparison with what LASRecomp already handles:

- **NVN**: Dread looks up 524 NVN function names. All but 7 already appear in LASRecomp's implementation; the 7 are
  debug "checkpoint" calls that can be no-ops. Having a function by name is not the same as covering Dread's use of it,
  though (see the risks section).
- **movie::**: Dread calls a *different* set of movie functions (direct `Extractor`/`Decoder` methods) from the ones
  LASRecomp implements (virtual-method based), so none of Dread's 36 imports match. The FFmpeg approach carries over,
  but the binding layer needs writing. The videos are 101 **WebM** files (`romfs/system/video`, 130 MB).
- **Other imports**: 39 of Dread's non-NVN imports are already implemented as host functions in LASRecomp. The rest
  resolve into the recompiled `sdk`, so they work once the services under them do.

Romfs (4.1 GiB, 9,654 files):

- **Shaders**: 478 `.bshdat` files in `system/shd` (MSHD containers, 130 MB) holding precompiled Maxwell binaries,
  plus 442 `.bsmat` materials. They can be translated ahead of time by parsing MSHD instead of discovering shaders at
  draw time.
- **Scripts**: compiled Lua 5.1 (`.lc`, `LuaQ` header) using 32-bit floats as the number type. They run inside the
  recompiled game unchanged, and the VM is a good place to hook mods.
- **Assets**: 5,700 `.bctex` textures, 1,276 `.bfstm` streams plus `bfsar`/`bfgrp` (NintendoWare sound), and
  2.2 GiB of `.pkg` packs.

## 2b. Prior art found (checked 2026-10-25)

- **[Metroid-Dread-Recomp](https://github.com/yozander2009-stack/Metroid-Dread-Recomp)** (Apache-2.0; its EXE is
  GPL-3.0 because it vendors ScarletNX's renderer). Created 2026-09-15, v0.4.0 on 2026-10-24.
  - Targets 2.1.0 on **Windows and Linux only**; no macOS.
  - Design: lifts `main`, `sdk` and `subsdk0`, then emulates the GPU at the command level (OpenGL, Vulkan opt-in).
  - Open issues: no sound, crashes, QTE timing.
  - Our project is still worth doing: a macOS-first build, NVN replaced at the API level (faster), and 1.0.0
    support.
- **[nxnative](https://github.com/seedds/nxnative)** (MIT): runs Dread natively on iPhone and iPad by rewriting the
  NSO, not by recompiling it; a Mac path is documented. Its
  [Dread notes](https://github.com/seedds/nxnative/tree/main/dm/games/010093801237C000) (for 2.1.0) are a list of
  issues we will meet:
  - Heap sizing: "used memory" must count `main`'s 1.8 GB bss, or `SetHeapSize` fails.
  - Movies:
    - `movie::Decoder` runs in NVN-texture output mode.
    - The game calls the `D1` destructor directly.
    - Its `RegisterClientEvents` pointers get overwritten.
    - The movie threads crash unless posted requests are handled first, as same-core preemption would do on the
      Switch.
  - Graphics:
    - `BindProgram` binds **per stage**: a null bind unbinds only those stages. Without this the depth prepass draws
      nothing and the scene renders black.
    - 1×1 `WriteTexels` calls must write in place.
    - Inline `UpdateUniformBuffer` calls must not end the render pass.
  - Timing: cutscenes are paced by a 30 Hz sound thread, so macOS sleep slack needs a precise sleep.
  - Files: Atk stream paths contain `..`, which must resolve lexically.
  - Services: SDK 11 names (`fs::CommitSaveData`); `oe::FinishStartupLogo` and `hid::SendVibrationValues` need
    host versions.
  - Shaders: 478 files hold 1,342 stages (1,010 unique). The control sections carry NV assembly text and the SASS
    follows each code area.
- **Other Switch recomps:** ssbu-recomp/smo-recomp (GPL), nxrecomp (GPL), nxrt (Unlicense, verified on macOS
  arm64), ScarletNX (GPL), and LASRecomp's own derivatives (PokeZ-ARecomp, Android ports).
- **Modding:** [open-dread-rando](https://github.com/randovania/open-dread-rando) and its exlaunch module,
  [mercury-engine-data-structures](https://github.com/randovania/mercury-engine-data-structures) (formats), and
  DreadGhidraPlugin (reflection and Lua-binding analyzers). The community supports only 1.0.0 and 2.1.0. The 2.1.0
  `main` build ID is `646761F643AFEBB379EDD5E6A5151AF2CEF93DC1`; its `sdk` and `subsdk0` are the same builds as
  1.0.0's.

## 3. Strategy

**Build on LASRecomp's core rather than starting over.** Fork it (MIT), split it into a game-agnostic core (lifter,
generator, loader, kernel, IPC, services, NVN and backends, shader translator, platform, build pipeline) and a Dread
game layer (address tables, patches, hooks, movie bindings, UI). Since LASRecomp is two and a half weeks old and
changes daily, keep the core as a tracked vendor branch, and propose the core/game split upstream so both projects can
share fixes.

Alternatives considered and rejected:

- **Our own lifter (for example via LLVM IR or remill).** It would cost months to reach what `gen.py`, `lifter.py`
  and `simd_c.py` already do, including bit-exact SIMD on arm64 and portable SIMD on x86.
- **Emulating the GPU at the Maxwell command level** (yuzu/Ryujinx style) instead of replacing NVN. It would be more
  general but slower, and a much bigger job. LASRecomp shows that replacing NVN works for a shipped game. The
  emulators stay useful as references for shader semantics and as a source of known-good renders.
- **Reimplementing the SDK function by function** instead of recompiling `sdk` and replacing the OS underneath it. The
  SVC/IPC surface is much smaller and better documented than nnSdk's tens of thousands of functions.

Tooling stays as in LASRecomp: Python generator (Unicorn, Capstone), C/Objective-C runtime, clang or `zig cc`, SDL3,
FFmpeg. Small host-side utilities can be Bun `.mts` scripts like `extract-xci.mts`.

## 4. Phases

Each phase has an exit test. The phases are rough, not dated; graphics will take the longest.

### Phase 0: Groundwork
- `git init`; ignore `exefs/`, `romfs/`, `generated/`, `*.xci`, `*.keys`.
- Bring in LASRecomp's core and remove the Link's Awakening layer (`hle/` features, `rando`, `ui` tabs, patches,
  address tables) so it builds and links with no game code.
- Add a version table keyed by `main` build ID, starting with 1.0.0 (`49161d9c…`).
- Extend `extract-xci.mts` to apply update NSPs (BKTR patch romfs and exefs), for 2.1.0 later.
- **Exit:** the runtime builds on macOS with an empty game library.

### Phase 1: Lift the whole game
- Run the generator on `main` and `sdk`. Record the counts it reports: functions, unsupported instructions, resolved
  and unresolved jump tables, `br` fallbacks. Fix lifter gaps until there are no unsupported instructions on hot
  paths.
- Memory layout: LASRecomp puts `sdk` 64 MB after `main`. Dread's `main` needs about 1.83 GB with bss, so move the
  module bases and check the heap, alias and stack regions still fit the 39-bit window. Load three modules if
  `subsdk0` is recompiled.
- Compile at `-O1`. Measure compile time and library size: on-device builds need to stay reasonable for 4.7 M
  instructions.
- **Exit:** generated code compiles and links, and the dispatch table covers every discovered function.

### Phase 2: Boot to the first presented frame
- Run the static initialisers and `nnMain` with full logging of unknown SVCs, IPC commands and unimplemented host
  functions. Fill gaps in services; expect `fsp-srv` save creation, `hid` NPad styles, an `audren` revision for SDK
  11, `set` language and region, `appletOE` focus and performance mode, `account`, `prepo` (stub) and `nfp`
  (stub, or amiibo emulation later).
- `movie::` for Dread's API on FFmpeg, decoding the WebM videos. Alternatively recompile `subsdk0` and replace the
  lower-level multimedia service, which is harder.
- Reuse LASRecomp's debugging tools: function-entry profiler, crash explainer, page watchpoints, `LARECOMP_*`-style
  environment hooks.
- **Exit:** the game reaches its title logic and presents frames, even if they render wrongly.

### Phase 3: Graphics correctness (the critical path)
- **Shader corpus:** parse MSHD (see the mercury-engine-data-structures format notes), extract every Maxwell
  program, and run the translator over all of them offline. Required: zero translation failures, Metal compiles
  everything, and `spirv-val` passes. This finds translator gaps without having to play the game.
- **Features Link's Awakening never used:** Dread is a heavier 3D renderer (deferred-style lighting, shadow maps,
  post-processing, particles, cutscene effects, EMMI-zone effects). Audit Dread's real use of compute dispatch,
  indirect draws, image load/store and atomics, queries, conditional rendering, MSAA and resolves, texture views and
  aliasing, and formats (BCn, ASTC, R11G11B10F, depth/stencil).
- **CPU/GPU coherency:** NVN memory pools are shared between CPU and GPU. Check how Dread streams uniforms and
  vertices (persistently mapped pools), and size LASRecomp's pool-write tracking and page watches for it.
- **Reference images:** scripted positions captured on an emulator running the same version, compared against
  ours.
- **Exit:** title, menus, and the first area (Artaria) render correctly at 60 fps on an M1.

### Phase 4: Audio, input, saves, video (alongside phase 3)
- `audren`: NintendoWare Atk depends heavily on renderer effects (reverb, delay, biquad, limiters) and voice
  features. Extend LASRecomp's `audio.c` to cover them.
- Input through SDL3 (keyboard and controllers, remapping); collapse HD rumble to simple rumble.
- Saves through `fsp-srv` to a host folder, using the Switch save layout so real saves can be imported.
- **Exit:** the first two areas play through with correct sound, video and saving.

### Phase 5: Complete and stable
- A test harness of saves at checkpoints, plus scripted input, covering every area, boss, EMMI zone and cutscene.
- Performance: profile dispatch overhead (43 k `blr` sites, mostly virtual calls), hot-function optimisation, and
  thread and job-system scheduling (Dread runs work across three cores).
- **Exit:** the game can be finished on macOS without crashes or visual bugs.

### Phase 6: Platforms, versions, enhancements
- Linux and Windows on Vulkan, and x86 FP/SIMD correctness: rounding, flush-to-zero, NaN propagation. Physics and
  scripts may be sensitive to these.
- Version 2.1.0 (the latest update) through LASRecomp's version mapping (addresses mapped through identical
  functions).
- Enhancements: resolution scaling, MetalFX/FSR, widescreen and ultrawide (a 2.5D camera, so check the culling
  bounds), higher frame rates (needs a look at Dread's frame timing), and shaders prepared ahead of time.
- Mods: romfs overlay (LayeredFS-style), code mods through hooks, and compatibility with community randomizer
  output.

## 5. Risks

1. **Graphics depth and performance (highest).** Dread uses NVN far more heavily than Link's Awakening. Translator
   coverage, memory-pool coherency and Vulkan synchronisation are where most of the effort will go.
   *Mitigation:* the offline shader-corpus tests, the reference-image comparisons, and running Vulkan validation from
   early on.
2. **Code size.** 4.7 M instructions mean long on-device compiles and a large library. *Mitigation:* chunking, the
   build cache and `-O1` (all in LASRecomp already); measure in phase 1.
3. **Indirect control flow in a stripped binary.** `.eh_frame` gives 102 k functions, but leaf functions with no FDE,
   and vtable targets only reachable through data, can be missed. *Mitigation:* scan `.rodata`/`.data` for pointers
   into code (vtables), fail loudly on an unknown dispatch target, and treat each miss as a generator bug.
4. **Concurrency.** 11 k exclusive-access sites, and a job system across cores. Compare-and-swap emulation of the
   exclusive monitor has ABA edge cases. *Mitigation:* stress tests, and a review of the lock-free paths that keep
   showing up.
5. **Floating-point behaviour on x86.** Portable SIMD has to match the guest's FPCR behaviour. *Mitigation:* test
   on arm64 (bit-exact asm) first; differential testing of `simd.h` against the asm path.
6. **Upstream churn.** LASRecomp changes daily. *Mitigation:* the vendor branch, small local changes to the core,
   and upstreaming fixes.
7. **Legal.** Never commit or ship game code, generated C, assets or keys. Generate on the user's machine. Do not use
   leaked SDK or NVN material.

## 6. First steps

1. `git init` this folder with the ignore list above.
2. Set up a Python environment with `unicorn` and `capstone` and run LASRecomp's `analyze.py` and `gen.py` on Dread's
   `main` and `sdk` *as they are*, for real lifting statistics (unsupported instructions, jump tables). This is the
   cheapest way to find out how far the existing lifter gets.
3. Write an MSHD parser and dump shader-corpus statistics, then run LASRecomp's `translate.c` over every shader.
4. Decide fork versus vendored core with upstream in mind, then start phase 0.

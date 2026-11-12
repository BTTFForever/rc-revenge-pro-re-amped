# RC Revenge Pro (SLUS-20153): static recompilation

A native Linux build of the PS2 game, made with PS2Recomp. Start it with `./run.sh`.

Status on 2026-11-09: boots, plays video and music, renders the menus and races correctly, and runs
at a steady 60 fps at the main menu and mostly 50-60 fps in the attract-mode race. Game speed follows the frame rate.

## Where to pick up (2026-11-09)
`./run.sh` uses the default renderer at native 240 lines and 60 fps at the menu. `PS2X_GS_HW=1 PS2X_GS_HW_SCALE=3 ./run.sh` gives the sharper God of War renderer; enlarge the window to see it.
Open decisions and work:
1. Make the God of War renderer at 3x the default, and decide whether to lock heavy levels to a steady 30 fps. With `PS2X_VU1_TIMING=1`, level 18 runs at about 30 fps, so a real PS2 probably did too.
2. A real race and a memory card save have not been tested. The memory card stubs are unreviewed.
3. The rain-level freeze has not recurred since `rand` was fixed, but that is not proven. `RC_BOOT_LEVEL=3` reaches that level directly.
4. Dynamic resolution and widescreen are still to do. Vulkan (`PS2X_GS_VK=1`) renders correctly but is slow.
5. A faster VU1 generator without the cycle-accurate timing might gain 20-30%.

## The two branches
`main` is the static recompilation, a playable build and the fallback. `decomp` is the decompilation. It still contains the whole recompilation, which it runs beside the C as its reference and which fills in every function not decompiled yet.
They share the runtime checkout in `~/PS2Recomp-sotc`. `decomp` turns on the fibers with `PS2X_EE_FIBERS=1`; without that define the runtime behaves as `main` expects. `main`'s patch file is older than `decomp`'s and `decomp`'s includes everything in it. Switch with `git switch main` or `git switch decomp`, then rebuild. Each branch's `NOTES.md` says what that branch does.

## Decompilation (started 2026-11-10)
The goal is a full behaviour-matching decompilation in the style of ico-pc (`../ico-pc`): native C with no recompiled code left. Decompiled functions replace recompiled ones one at a time in the running game.
- `tools/stabs2c.py ../disc/SLUS_201.53 decomp/generated` writes the types, globals and per-file signatures from the ELF's debug data. `decomp/generated/symbols/<file>.txt` is the reference to read while decompiling.
- `tools/decomp_order.py ../disc/SLUS_201.53 functions_filtered.csv decomp/generated/symbols.json decomp/generated/order.txt` lists the functions that cannot block, leaf first. These can be compared call by call.
- `decomp/src/*.cpp` holds the decompiled code. Add `RC_DECOMP_FN(name, 0xaddress)` under each function, or `RC_DECOMP_FN_BLOCKING` for one that can block. Call code not yet decompiled with `guest_call`. Build with `ninja -C build-decomp rc_revenge_pro`; `build/` stays the profile-guided recompiled build.
- Rules: structs keep the PS2 layout (`ptr32`, `ee_float`) and globals are `G(name)`. A loop that polls memory set by an interrupt must call guest code or it never sees the change, because interrupts are serviced only at guest scheduling points.
- Threads: each EE thread runs on an ico-pc fiber (`third_party/ico_platform`, MIT). `EeScheduler` still decides what runs. A guest wait unwinds only as far as the nearest native frame and then yields, so a native frame survives blocking (`callGuestFromNative`). Blocking functions cannot be compared call by call, since that runs them twice; they currently have only whole-game checks.
- Native code charges no EE cycles yet, so the game's emulated clock will drift as more code goes native. Charge an estimate in `Thunk::run` before large areas are converted.
- Checking: `RC_SHADOW=1` compares sampled live calls against the recompiled originals, including all of guest RAM; `RC_SHADOW_FUZZ=<n>` also runs each function on n random inputs; `RC_SHADOW_LOG=<file>` keeps totals; `RC_DECOMP=0` turns every decompiled function off. Pass `RC_BOOT_LEVEL=<n>` to reach a race quickly.
- The random test fills guest pointers with random bytes, so a function that follows pointers stored in its data needs a hand-written setup instead.
- Function sizes in `functions_filtered.csv` are cut short at internal labels. Use `symbols.json`.
- VU1: `tools/vu1dis.py ../disc/SLUS_201.53 decomp/generated/vusrc` writes a labelled listing of each microprogram, keeping the labels in the ELF. The plan follows ico-pc's: a checked C reference of each program, then one shader per program driven by the data the game already uploads. Programs and their recompiled files: `Rdr_VUShapeProg` 6ec8b9ff8fa44221, `TRdr_VUShapeProg` (track) c3952c0faeb52ead, `TERdr_VUShapeProg` bac581270f262987, `EnvRdr_VUShapeProg` ab0e7743a9ba7393, `Rdr_VUSkinProg` ec08b22243d4dc8c, `Rdr_VUWaterProg` e96770e4143d2da7.
- `decomp/vu1/trdr_shape.cpp` is the track program in C++. `RC_VU1_REF=1` runs it beside the original on every launch and compares each GS packet and the final VU memory; `decomp/vu1/vu1_check.cpp` does the comparing, and the game still draws from the original. On 2026-11-10 levels 1, 3 and 18 matched with no differences over about 32 million packets. Two rules matter for the other programs: the VU rounds a multiply-add's product to single precision before the sum, and a program can store registers it left set in an earlier run, so C versions start from the real register state.
- Track shader: `decomp/shaders/trdr_shape.vert` reads the triangle data as the game sends it to VU1. `tools/shader_check.cpp` compares it with `trdr_shape.cpp` on runs captured with `RC_VU1_REF=1 RC_VU1_CAPTURE=<file>`, on the GPU through transform feedback. On 2026-11-10, over levels 1, 3 and 18: 156,497 triangles, no facing test disagreements, and on the 48,057 comparable triangles the worst errors were 1.13/16 of a pixel, 7 GS depth units out of 16.7 million, and no colour difference. Triangles cut by the near plane or crossing the guard band are left to the GPU's clipper and are not compared; the original keeps the colours of cut corners unchanged, which the GPU will interpolate. `RC_TRACK_SHADER=1` with `PS2X_GS_HW=1` draws track geometry with it in the game (`decomp/vu1/track_draw.cpp`): non-triangle packets still go to the GS as before, each texture block's triangles reach the render-target backend in order as one write to an unused GS register, and consecutive batches with the same state are merged. On 2026-11-10 at 2x, level 18 ran at 36-42 fps against 28-36 fps through VU1, with the VU1 thread falling from about 80% to under 50% of a core and the GS thread becoming the limit. The other five VU1 programs still run as before.
- Ghidra: `../ghidra_ee` is a project made with ghidra-emotionengine-reloaded v2.1.39, installed under `~/.config/ghidra/ghidra_12.1.2_PUBLIC/Extensions`. Run it with `JAVA_HOME=~/tools/jdk21`. Its STABS analyzer gives typed pseudo-C with the original names, which is a starting point only: it read `Sqr32`'s unsigned divide as signed, for example.
- Track viewer: `python3 -I tools/track_viewer/build.py ../disc decomp/generated/gs_tables.bin ../viewer`, then open `../viewer/index.html`. It reads the disc's `BB/<n>/<n>.BBK` files directly and does not need the game to run; the output stays outside the repository because it is disc data. `decomp/generated/gs_tables.bin` comes from building and running `tools/gs_tables_export.cpp`. A BBK is a 0x14EC-byte header, then zlib sections in order: sound, a GS memory image uploaded as 64x64 32-bit tiles from 0x1E0000, a game data image with absolute addresses already applied, and handles. Level 98 is the front-end world. `?shot` on the page's URL is for automatic screenshots.
- File formats stay as they are during the decompilation. Modern assets can come later as an optional override layer.

## What lives where
| Path | Contents | Keep? |
|---|---|---|
| `../RC Revenge Pro (USA).bin`, `.cue` | Original disc image | Yes, the source of everything |
| `../RC Revenge Pro (USA).iso` | ISO made from the bin; the game reads the disc through it | Required at run time |
| `../disc/` | Files extracted from the disc, including the ELF | Required at run time |
| `../ghidra/` | Ghidra project and `functions.csv` | Slow to regenerate |
| `CMakeLists.txt`, `config.toml`, `run.sh`, `overrides/`, `tools/` | Hand-written project files | Tracked in git |
| `patches/ps2recomp-sotc-local.patch` | Every local change to the runtime fork | Tracked in git |
| `patches/upstream_attempt_archive.tar.gz` | Config and overrides from the first attempt on upstream PS2Recomp, plus the older stubbed config | Reference only |
| `functions_filtered.csv`, `output/`, `vu1/`, `vu1_images/` | Generated by the steps below | Regenerable |
| `build/` | Build output, including the profile data for the profile-guided build | Regenerable with `tools/pgo.sh` |
| `gs_shaders/` | Compiled shader cache | Regenerates as you play |

Outside this folder the build needs `~/PS2Recomp-sotc` and the third-party sources in `~/PS2Recomp/out/build/_deps`.

## Setting up the runtime on a new machine
```
git clone https://github.com/LightVelox/PS2Recomp ~/PS2Recomp-sotc
git -C ~/PS2Recomp-sotc checkout d06a4ab
git -C ~/PS2Recomp-sotc apply "$PWD/patches/ps2recomp-sotc-local.patch"
```
Refresh the patch after changing the runtime (this leaves the runtime's git index untouched):
```
cd ~/PS2Recomp-sotc && export GIT_INDEX_FILE=/tmp/rc_index && git read-tree HEAD && git add -A && \
  git diff --cached --binary HEAD > "<this folder>/patches/ps2recomp-sotc-local.patch"; unset GIT_INDEX_FILE; rm -f /tmp/rc_index
```

## Rebuilding
Run from this folder. Steps 1-3 were checked on 2026-11-09 and reproduce the existing files byte for byte.
1. `tools/filter_function_map.py ../disc/SLUS_201.53 ../ghidra/functions.csv functions_filtered.csv`
2. `~/PS2Recomp-sotc/out/build/ps2xRecomp/ps2_recomp config.toml`
3. `cmake -S . -B build -G Ninja && ninja -C build vu1_recomp`, then
   `tools/make_vu1_images.py ../disc/SLUS_201.53 vu1_images` and `build/vu1_recomp vu1_images vu1`
4. `ninja -C build rc_revenge_pro`
5. `tools/pgo.sh` rebuilds `build/` with a GCC profile taken from a scripted run of the game. Repeat it after larger
   runtime or VU1 changes. `-DRC_PGO=off` returns to a plain build.

## Compiler findings
- GCC with the profile: menu 45-55 fps. GCC without: 40-43 fps. With VU1 flag skipping added: a steady 60 fps at the menu.
- Clang without a profile: 34-36 fps, and one run froze during a load. Not used.
- `-O2` game code with `-O3` VU1 code gave no gain over `-O1` and `-O2`.
- VU1 flag skipping (`planDeadFlags` in `vu1_recomp`, `PS2X_VU1_DEAD_FLAGS=0` when generating disables it): 744 of 890 FMACs drop
  flags no instruction can see. It applies to programs that never read STATUS. Check it with `PS2X_VU1_VERIFY=1 PS2X_VU1_VERIFY_NOFLAGS=1`.
- Keep `-ffp-contract=off`: the PS2's float results depend on each operation being rounded separately.

## Run-time switches
Set by `run.sh`: `PS2X_GS_GPU=1`, `PS2X_GS_THREAD=1`, `PS2X_MTVU=1`, `PS2X_VU1_TIMING=0`, `PS2X_EE_HOST_PACING=1`,
`PS2X_GS_SHADER_PRELOAD=0`, `PS2X_GS_SHADER_CACHE=1`, `PS2X_GS_SHADER_CACHE_DIR`.

Do not enable `PS2X_GS_SHADER_PRELOAD`: compiling every cached shader at startup makes the opening video stutter.

Debugging:
- `RC_DEBUG_DUMP=<file>` writes threads, heap, GS registers and `gameframes` lines; `tools/fps.py <file>` turns them into fps.
- `PS2X_VU1_VERIFY=1` runs each VU1 program natively and in the interpreter and compares them. It runs at 2-3 fps by design.
- `PS2X_SCREENSHOT_DIR` and `PS2X_SCREENSHOT_FIELDS` save frames; `PS2X_AUDIO_STATS=1` prints audio rates.
- `PS2X_PAD_SCRIPT` plays scripted input as comma-separated `start[v]:BTN+BTN[:hold]`, where `v` means fields rather than seconds.
- `PS2X_DEBUG_PANEL=1` shows the debugger.
- `eeCycle` in the dump is not a reliable speed gauge. Use `gameframes`.
- In zsh, unquoted variables do not word-split, so pass env settings one by one.

## Local changes to the runtime
All are in `patches/ps2recomp-sotc-local.patch`, against d06a4ab.
- `gs_frontend.*`: a GIF tag can continue across DMA transfers. Without this, image pixels were read as GS registers.
- `gs_frontend.cpp` (`latchHostPresentationFrame`): reads the display registers after the GS queue drains and retries if
  a swap landed during the copy. Without this, a just-cleared buffer is presented as a black frame.
- `ps2_runtime.cpp` (`UploadFrame`): waits for the VU1 thread before presenting.
- `EeScheduler.cpp` (`hostCatchUp`): host pacing no longer depends on the VU1 thread existing.
- `gs_gl.cpp`, `gs_gl_api_load.inl`: Linux OpenGL context through EGL.
- `gs_gpu_backend.cpp`: the `PS2X_GS_SHADER_PRELOAD=0` switch.
- `iop_memory.h`: IOP heap base 0xA0000, set for this game's modules and sound banks.
- `iop_spu2.*`, `iop_emulator.cpp`: SPU2 IRQ address, which the streamed music relies on.
- `iop_pad.h`: missing `<cstddef>`. `state_io.h`: a form Clang also accepts. `ps2_debug_panel.h`: `PS2X_DEBUG_PANEL=0` hides the debugger.
- Experimental, off by default: God of War's render-target renderer (`gs_hw_backend.cpp`, `gs_texture_store.*`, `gs_gpu_helpers.h`,
  `gs_tri_groups.h`, `gs_upload24.h`, `gs_colour_runs.h`, additions to `gs_gl.h`, `gs_gl_api_load.inl` and `ps2_gs_memory.*`).
  Enable with `PS2X_GS_HW=1 PS2X_GS_HW_SCALE=<n>`.

## Game overrides (`overrides/rc_revenge_pro_overrides.cpp`)
Real disc positions for CD reads, reserved heap and stack layout, MPEG streaming and end detection, `sceGsSyncV` through
the runtime stub (the real one polls INTC_STAT, which is not modelled), `sceGsSetHalfOffset` without the field offset, and
a frame counter on `GfxP2_SwapBuffers`. A wrapper around a function that can block must set `ctx->pc = ra` first.

## Where the time goes (menu, VU1 on its own thread)
VU1 thread about 62% of a core, GS thread about 50% (much of it waiting), game thread about 25%, GIF thread about 18%.
No core is saturated; each frame waits for VU1 and GS work to finish, and a frame that misses the 16.7 ms refresh waits
for the next one.

## Known limits and open questions
- Output is the game's native 240 lines per frame, scaled to the window. The half-line bob is removed.
- Memory card saving has not been exercised: `mc0` and `mc1` are empty.
- Attract-mode freeze, parked 2026-11-09: one rainy jungle demo (level 3) stops the game in `VfxCallHandlers`. The effects list
  loops because a block is handed out while still on the heap's free list. No double free was detected; a free-list link goes bad
  first, around the `VfxUpdate` and `Vfx2RenderTSFX` temporary allocations. Forcing level 3 alone loads a boat variant without
  rain and does not freeze, so the rain and vehicle choice comes from elsewhere. Next step if it matters in real play: walk every
  heap block's header at each allocation to find the block overwritten, and the allocation just before it.
- Measured performance comes from the attract-mode demo race, not from driven races.
- Game speed is tied to frame rate, so the game runs slower below 60 fps (reported by the user).

### Stub audit (2026-11-09)
`config.toml` replaces 304 game functions with runtime stubs. A counting run through the intro, menu and demo called 52 of them. `sqrt` is the only math stub left: its stub reads a float from f12 while the game's takes a soft-float double in $a0/$a1, but no generated code calls it. `memcpy`, `memset`, `memmove` and `strncpy` were read and look right; the pad, CD, memory card and SIF stubs were not reviewed line by line, and memory card saving is still untested.

### Graphical bugs to check later
- Main menu butterflies and random effect values: fixed 2026-11-09. The runtime `rand` stub returned 15 bits while the game expects 31 and uses `rand() >> 15`, so every random value was stuck. `rand` and `srand` now run the game's own code.
- Races: 2D effects behind vehicles, such as water spray, tire dirt and skid marks, are not drawn properly.
- Other minor graphical bugs the user has seen since the start; details still to be collected.

### State of `PS2X_GS_HW=1`
The God of War render-target renderer now draws this game correctly at 1x and 3x: videos, logos, menu and races. Two fixes were needed:
PABE support (the game's zero-alpha textures rely on it to draw opaque), and presenting from video memory when the displayed buffer was last written by an upload.
After caching uniforms, streaming vertices through one buffer and reading frames back through pixel buffers: about 60 fps at the menu at 1x and 55-59 fps at 3x (one 1x race ran at 23-35 fps). The profile-guided build was trained on the default renderer, so `tools/pgo.sh` has not profiled this code. The window opens at 640x480 and is resizable;
it must be enlarged to show the extra detail. One run stopped at 0 fps during a demo with the heap reading 576 bytes free, like the parked freeze.

### State of `PS2X_GS_VK=1` (ps2-port-engine's Vulkan renderer)
`third_party/port_engine` holds a subset of that engine (MIT), and `rc_backend.cpp` feeds it the raw GIF packets. `PS2X_GS_VK_SCALE=<n>` sets the
internal resolution. On 2026-11-09 the logos, video, loading screen, menu and a race rendered correctly at 1x and 2x.
It is too slow to use yet. With a synchronous read-back: about 22 fps at 1x, 4 fps at 2x. With the asynchronous read-back now in place
(`ReadTargetAsync`): about 27 fps at 1x, 5 fps at 2x, with audio starving at 2x. Each present still blocks for 12 ms at 1x and 46 ms at 2x, so the copy was
not the main cost; where that time goes (recording, or waiting for the GPU) has not been measured. Set aside on 2026-11-09 until more bugs are fixed.
Presenting straight to the window was not tried. The window is a GLFW X11 window under Wayland, so a Vulkan surface would need a child X11 window.

### Level testing (2026-11-09)
`RC_BOOT_LEVEL=<n> ./run.sh` skips the intro and menus and loops the attract-mode demo on level n. `tools/level_sweep.sh <first> <last> <seconds> [VAR=value ...]` prints each level's frame rate.
Sweep of levels 1-24 on the God of War renderer at 3x: 1, 3, 4, 10, 15, 16 and 22 hold about 55-60 fps; 6, 8, 9, 12, 17, 18 and 24 ran about 25-35 fps before the blend and palette fixes. On level 18 the default renderer reaches 35-43 fps with VU1 87% busy at 14.5 ms per field, so those levels are limited by VU1 on both renderers. About half the VU time is one block of program C3952C0FAEB52EAD; its cost is spread evenly over its instructions.

## Other projects consulted
- github.com/LightVelox/sotc-vibe-pc: settings and performance notes. Most switches are already on by default in the fork.
- github.com/KIexster/god-of-war-recomp: `patches/` holds the renderer, PGO method and VU1 patches.
- github.com/danielgomesvieira2000/ps2-port-engine (MIT): `docs/PORTING.md` flag liveness ("fast mode"), `docs/GPU.md`
  scaling, HUD anchors for widescreen. Its Vulkan renderer is the fallback if the God of War renderer does not fit.
- github.com/exoteric33/ps2-native-recomp (MIT): `scripts/pnach.py` applies PCSX2 widescreen patches; generic 2x rendering.
- github.com/Ninjacornix/reo, github.com/TheBozos/TMB_Recomp: not used so far.

## Credits
- PS2Recomp: github.com/ran-j/PS2Recomp (GPL-3.0)
- Shadow of the Colossus port and PS2Recomp fork, by LightVelox: VU1 recompiler, GPU GS backend, VU1/GIF threads,
  the policy of keeping GS/DMA libraries as original code (GPL-3.0)
- God of War recomp, by KIexster: Linux EGL context code and the render-target renderer (GPL-3.0)

### GPU

Always run on the discrete RTX 2060 (`prime-run`-style environment, set by `tools/play.sh` and `tools/shadow.sh`). Every frame-rate figure in these notes up to 2026-11-10 was measured on the integrated Vega 10, because nothing selected the 2060 until then.

On the 2060 the X11 window's GLX context stops EGL contexts being made current (any EGL display type, any thread), so `gs_gl.cpp` falls back to a GLX pbuffer context when EGL refuses. The first run on the 2060 gave 30-37 fps on level 18 at 2x and 4x, against about 48 on the Vega, with `GSThread` at 99% of one core. The renderer sends all its state for each of about 85,000 draws a second. Fixes, in `gs_gl.cpp` and `gs_hw_backend.cpp`: a state cache that skips unchanged GL calls (`statecache`), persistently mapped streaming buffers (`PersistentRing`, no map and unmap per draw), and track batches written straight into the mapped buffer.

Level 18 is now 40-51 fps at both 2x and 4x; levels 3 and 16 hold 60 at 3x-4x; levels 5 and 8 are 33-59. On the heavy levels `nvidia-smi` shows the GPU at 95-100%, and the speed doesn't change with resolution, so the limit is not pixels. Untested guess: the vertex shader pulling every triangle from a buffer in system memory over PCIe. The Vulkan renderer is the natural place to fix that.

**Device-local geometry test (2026-11-12): negative.** The track rings (`trackTriangleBuffer` 64 MB SSBO, `trackConstantBuffer` 4 MB UBO, plus `vertexRing` for ordinary sprites) are `glBufferStorage` with WRITE|PERSISTENT|COHERENT only, no client-storage bit, mapped once; the track vertex shader pulls triangles from the SSBO. `PS2X_GS_DEVICE_VERTS=1` copies each written range into a device-local twin (`glBufferStorage` flags 0, `glCopyBufferSubData` per flush) and binds that. Measured with the decomp build, `PS2X_GS_HW=1 SCALE=2 RC_TRACK_SHADER=1` (these are required: `run.sh`'s environment alone does not use the HW renderer or the track shader), 60 s, two runs each, steady-state fps per 5 s: level 18 about 45 off vs 37 on; level 5 and 8 also lower with it on (noisy, 5-10 fps swings from load/pauses); GPU busy 86-91% either way. So the geometry is not limited by PCIe reads; the extra per-draw copy costs more than it saves, and it stays off by default. Not tried: dropping the coherent bit or changing flags to coax the driver into VRAM.

`perf` is not allowed without lowering `kernel.perf_event_paranoid`. `tools/profile/sampler.cpp` is an LD_PRELOAD sampling profiler that needs no permissions: build as a shared library, run with `SAMPLE_THREAD=GSThread SAMPLE_OUT=<file>`, and read the result with `tools/profile/analyse.py <file> <binary>`.

### VU1 programs converted (2026-11-10)

All shape programs share their clipping text at different addresses. `decomp/vu1/vu_shape.{h,cpp}` holds it, and `ShapeLayout` records how each program differs (addresses, buffers, which registers leak `w` lanes into GS packets, wrap behaviour).

| Program | C++ | Check against VU1 | In the game (`RC_TRACK_SHADER=1`) |
|---|---|---|---|
| TRdr (track) | `trdr_shape.cpp` | 0 differences | vertex shader |
| EnvRdr (shiny pass) | `env_shape.cpp` | 0 differences, 33M packets, env map on in 19,710 runs | vertex shader, `shapeMode` 1 and 2 |
| Water | `water_shape.cpp` | 0 differences, 18.8M packets, 13,380 water runs | C++ in place of VU1 |
| Skin | `skin_shape.cpp` | 0 differences, 18.6M packets, 31,607 skin runs | vertex shader, `shapeMode` 3 (the buffer is all of VU memory, for the bone matrices) |
| TERdr (Track Editor pieces) | `terdr_shape.cpp` | 0 differences on synthetic runs only | still VU1 |
| Rdr_VUShape (bombs, effects, some world shapes) | `rdr_shape.cpp` | 0 differences on synthetic runs only | still VU1 |

- Water stays C++ because each triangle is three passes with GS state changes between them. Batching those for a shader would change how overlapping water blends.
- EnvRdr's reflection is per-component (`n*n*eye*2-eye`, then each lane times the sum of that column), not a textbook reflection. Kept as is.
- The runtime gives a VU1 run 65,536 cycles and never resumes one that hits the limit (`ps2_runtime.cpp`, MSCAL callback). The largest water batches are cut short by this in the recomp: 76 of 13,380 runs on level 18. The C++ version finishes them, as hardware would. The checker counts these as "cut short".
- `RC_VU1_REF=1` prints runs per program. `RC_VU1_CAPTURE_HASH=<hex>` picks which program `RC_VU1_CAPTURE` records; `tools/shader_check --env` checks EnvRdr captures.
- `RC_VU1_SYNTH=decomp/generated/vusrc` (with `RC_VU1_REF=1`) rearranges real track data into TERdr's and Rdr_VUShape's input formats and compares the original programs with the C++ versions. Lighting values, normals and the second matrix are generated, so this checks the instructions, not the game's real inputs. `tools/vu1dis.py` writes the program images it needs. With `RC_VU1_REF=1`, a real run of either program is compared as soon as one happens.
- TERdr was compared on real runs by playing the Track Editor: 284,151 runs, 9M packets, 0 differences. It is still on VU1 in the game.
- Rdr_VUShape effectively never runs: every caller uses it only for shapes of version 2.0, and all 2,079 shapes found across the 53 level files are version 3.0. Only the synthetic check covers it.
- `tools/play.sh record|replay <name>` plays from a clean memory card and records or replays the controller (`decomp/recordings/`). Circle and Square are on E and R there, because the mouse bindings need a captured pointer. `RC_VU1_REF=2` compares only the two programs above, cheap enough for play. Recording format: epoch, frame, buttons, axes; the epoch advances when a level finishes loading so load times can't shift input. Known problem: the epoch never advanced in a real session (`test.pad` is all epoch 0), so the hook on `MainGameLevelInit` isn't taking effect and replay would still drift at loading screens. Replay is unfinished and was set aside; session1 and session2 predate the format.
- Memory-card saving works: a Track Editor track saved to the virtual card. Loading it back hasn't been tried.

**Bulk C decompilation** (started 2026-11-10). `tools/decomp_progress.py --files` shows progress by source unit: 1,713 game functions, about 119,000 lines of Ghidra output. The other 773 are SDK, libc and zlib, to be replaced by host libraries. `tools/shadow.sh <level> <seconds> [fuzz]` compares every native function with its recompiled original during a demo race and on random inputs (`RC_SHADOW_ONLY=<name>,<name>` limits the random inputs to those functions). The random inputs for every function at the default 2000 take well over ten minutes, so pass a list, or 1 as the count when only the race matters.

`tools/asm_facts.py NAME...` (or `--unit RDR_SHAPE`) prints what the instructions say and Ghidra's pseudo-C can lose: frame size, locals passed by address, absolute and $gp-relative addresses resolved to globals/functions (or `?`), calls with their constant or stack-address arguments, madd/sqrt use. Read it next to `decomp_extract.py` output before writing a function. Its warnings flag a Ghidra label (`_dma_N`) that stands for different addresses in different functions, an empty Ghidra body that stores, and a store in the delay slot of the final `jr $ra`; `--unit ALL --warnings-only` scans the whole program (last output in `decomp/generated/asm_warnings.txt`). Straight-line register tracking only, so a constant set in one branch arm can leak into another.

Things Ghidra's output hides, all found by the comparison:
- `4096.0 / SQRT(x)` is a single `rsqrt.s`, which rounds differently: use `ee_rsqrt`.
- `add` traps on overflow, so random inputs that overflow have no meaningful answer: those functions use `RC_DECOMP_FN_NOFUZZ`.
- Native code must not call recompiled code directly. A checkpoint inside it returns early. `sinf` and `cosf` are native for that reason (`src/libm_float.cpp`). Their huge-argument path (`__kernel_rem_pio2f`) is not ported: arguments past 2^7*pi/2 go to the recompiled originals through `callOriginalFloat`, which runs an original to its end on a copy of the context with interrupts held. Firing a weapon passes 32768; before this the game aborted there (play test, 2026-11-12). Both now pass the random test (2,000 inputs each, huge ones included).
- Ghidra can flatten float additions: it printed `a + b + c` for code that computes `a + (b + c)`, and rounding differs. When a float function mismatches in its low bits, read the instruction order in `output/<name>.cpp`.
- Open: `FMatNormalise` differs on a few random garbage inputs (about 1 in 500), never on a real call. Not explained.
- A function can copy stack words it never set. `AColCellCastCell` copies a 16-byte point whose padding is unset, and `TColBuildGrid` copies `min.y` and `max.y` from an unset local. Work out what the stack holds on the game's real path (for `TColBuildGrid` it is zero: the upper halves of registers `TColMakePolyList` saved) and write that, or leave the destination alone, and make the random test's setup put the same value on the guest stack below `$sp`.
- `sceVu0InnerProduct` and the other VU0 macro routines clamp after each step: use the runtime's `PS2_VMUL` and `PS2_VADD` (`vu0Dot` in `collision.cpp`), not `ee_float`.
- `max.s` and `min.s` are plain `std::max` and `std::min` of the two operands in the instruction's order. Ghidra prints them as a bit-select expression.
- Ghidra shows `ptr + index * 0x60` done with `madd` as arithmetic on `in_hi`. Only the low word is used, so it is ordinary pointer arithmetic.
- Recompiled code returns early from its next call or loop once an interrupt or thread switch is due, and the random test never lets the scheduler run. It used to skip such inputs silently, which once the clock had run far enough meant every input of every function that makes a call. The test now runs in steps between the game's own calls and resumes from the input that was cut short. Each `fuzz` line in the log says how many inputs returned non-zero, wrote memory or were cut short: check that the inputs reach the code before trusting "0 mismatches".
- Functions that follow pointers get valid random data from a setup function: `RC_DECOMP_FN_SETUP(name, address, setup)`, with the collision ones in `src/collision_fuzz.cpp` (a random 4 by 4 cell level, cars, Track Editor modules). `RC_DECOMP_FN_SETUP_HEAP` is for functions that allocate from the game's heap: it compares all of RAM on 40 inputs. One that runs longer than the gap between interrupts (the 800,000-byte clear in `TColComputeCellResidency`) needs many tries per input.
- After a reboot the first run on the 2060 spends a minute or two compiling shaders before the game starts, with `GSThread` at 100%. `tools/shadow.sh` kills the game after its time limit, so the first run can end with no results; run it again.

Fuzz re-check with the fixed harness (2026-11-11, level 18, 500 inputs, one run, `fuzz finished` reached, race phase 1,127 calls and 0 mismatches). 73 functions in maths, math_core, psxdefs, physics, libm_float and gfx_p2 have random inputs; 22 are `RC_DECOMP_FN_NOFUZZ` (all the car physics from `FMatRotateY` to `FCalcCarBodyForces`, `Vec32MulMatPsx(T)`, `sinf`, `cosf`) and `GfxP2_SwapBuffers` is BLOCKING, so none of those has any fuzz, only the race comparison. Nothing was cut short except `Math_InitInvTable` (231 of 500 cut short, 135 wrote memory: weak check). Three functions differ: `FMatNormalise` 2 of 500 (known). New: `ArcTan` 45 of 500 and `PS2C_Vec16Normalise` 110 of 500, both hidden before because they call GetMSB / Sqr32 and were skipped. `ArcTan`: the original divides with `divu` (0x19feac) and the native uses signed `ee_div`, so it differs when `small << 12` or the shifted `large` is 2^31 or more (small of 0x80000 or more, or |y| = 0x80000000). `PS2C_Vec16Normalise`: the original returns the zero-extended `Sqr32` result in v0, the native sign-extends it through `int16_t` (only the upper 32 bits of v0 differ, when bit 15 is set; memory identical). Neither has shown in the race. Both fixed the same day (unsigned `ee_divu`; return the `uint16_t`): 2000 random inputs each, 0 mismatches. Several void or init functions show 0 non-zero and 0 wrote (`Math_Init`, `Math_Term`, `Trig_Init`, `Trig_Term`, `FPhysLibInit`, `FPhysLibTerm`, `PS2C_GteMatrixMulVec16`, `PS2C_NullTransMatrix`; `PS2C_GteVecMatrixMul` and `PS2C_NulGteMatrix` 1 of 500): the random inputs cannot show a difference there.

Done so far: RANDOM, VECTOR, MATHS, MATRIX, TRIG, MATHTRIG, F3DMATH, PSXDEFS, 3DMATH, `GfxP2_SwapBuffers`, the car physics (FPHYSLIB, FUPDATE, FRESPONSE, FFORCES, FINITCAR, FBOAT), and the collision (ACOL, TCOL, and CARCOL except three functions), AINODE except two functions, the smaller AI and PLAYER functions, the AI driving loop, the renderer's state and DMA-chain layer, and the polygon builder with the SDK's VIF packet library, and the shape, skin and water renderers. `FPhysLibInitTESTCarData` and `FPhysLibInitTESTCarSpheres` are left: nothing calls them.

Collision (2026-11-11, `src/collision.cpp`): all of ACOL and TCOL, and CARCOL's `CarColDetect`, the sci-fi door triggers and the small functions. Left recompiled in CARCOL: `CarColPostDetectProcess` (calls `VfxAddNew`, `RevSfxPlay3D`, `AnimFindByTSOTag`, `RevSfxSpecialTrigger`, `PlayerSetVibrate`, none native yet), `CarColColDetCars` (calls it) and `ColDrawColPoly` (debug drawing through `RdrPoly`; nothing calls it). Ported for them: `MemBlockAllocLo`, `MemBlockFree`, `MemBlockSet32`, `MemOutOfMemory` (`src/mem.cpp`), `ResFindType`, `ResTypeSetConstructDaemon`, `ResTypeUnSetConstructDaemon` and `TSOFindByTag` (`src/res.cpp`). TCOL only runs when a Track Editor track is loaded (`LevelLoadTrackLevel`), which no demo does, so it is checked on random tracks only; `AColGeneralRayCast`, `AColCellCastCell`, `AColIsPositionUnderWorld`, `AColPointInPoly_RayPoly` and the `TColGetPolyId` family have no callers at all. The first real Track Editor race under `RC_SHADOW=1` is still worth doing.

Route nodes (2026-11-11, `src/ai.cpp`, Opus): AINODE except `AINodeRender` (debug lines and text through `LineRender3D`, `FontPrint`) and `AINodeUpdateFinishDistance` (lap counting: calls sound, music, effects, vibration and the Track Editor's lap records). Also native for them: `rand` and `srand` (the SDK's newlib LCG, state at `*0x3213f4 + 0x58`), `VecNormalise`, `VfxAreWeInCar` and the other small VISFX and INPUT accessors. Real calls compared with 0 differences on levels 5, 17 and 18, including every call of `AINodeUpdateCurrent` (2,280 to 4,816 per level); the random test uses real cars and nodes from the loaded level (`src/ai_fuzz.cpp`, needs `RC_SHADOW_FUZZ_DELAY=25`). `AINodeConstructDaemon` never runs in a demo race (levels take their nodes from the level file, `AINodeSetupClass`); it is unchecked.
- `VfxAreWeInCar` compares the object type unsigned (`sltiu`).
- Some originals read stack words they never wrote: `AINodeUpdateRacingLineAhead` copies its local vector's unwritten fourth short into `svGlob.pad`. `stale_stack<T>(frameSize, offset)` reads the same word. `frame_local<T>(frameSize, offset)` places a local the original passes by address (to a native function taking `ptr32`) at its address in the original's frame.
- Harness fixes (2026-11-11): (1) a recompiled function that calls itself does so with a jump inside the same host function, so the inner return leaves the host function early, with pc at the outer call's return address; the comparison took that for a run cut short by an interrupt and skipped it, so recursive functions and their callers were never compared. `runOriginal` now re-dispatches until pc reaches the caller. (2) While the comparison runs an original, the scheduler holds its checkpoints (`EeScheduler::holdCheckpoints`, runtime patch), so long originals finish instead of being cut short. Functions earlier reported with inputs "cut short" (`TColComputeCellResidency`, `Math_InitInvTable`) should be rechecked. (3) `RC_DECOMP_SKIP=name,name` leaves functions recompiled, so a native function's callees can be compared through a recompiled caller. (4) `RC_SHADOW_FUZZ_DELAY=<seconds>` delays the random test, for setups that need a level loaded.

AI and PLAYER (2026-11-11, `src/ai.cpp`, Opus): `AIInit`, `AIAdjustForTooCloseToCarAhead`, `AIPlayCatchup`, `AIUpdateObjectNode` (calls the recompiled `AINodeUpdateFinishDistance` through `guest_call`), `AISetupCars`, `AIAdjustForTrackEdges`, `PlayerInit`, `PlayerTerm`, `PlayerDetectBraking`, `PlayerPhysicsVibrations`, `PlayerSetVibrate`, `ProcessControlInput`, `PlayerCatchup`. 0 differences on levels 5 and 17 (real calls and 1,000 random inputs each). The player functions don't run in demo races, so they are checked on random inputs built from the race's cars only. Left recompiled: `PlayerControl`, `PlayerGetInput`, `PlayerSpawnWeapon`, `PlayerTestWeapon`: they call weapons, objects, the camera, effects and sound, which are not native yet.
- An index the original never checks (a player number into a two-entry array) must be turned into an address, not used on the C++ array: past the end, the compiler assumes the access cannot overlap the next field and keeps values in registers that the original reloads (`PlayerSetVibrate`'s motor clamp).
- Harness fixes: a compared native that calls guest code no longer starts nested comparisons (they reused the outer one's RAM snapshots), and the random test puts the game's registers back after a native that used `guest_call`, which works on the running context, not the test's copy. Before this, live mismatches with `ra=fffffff0` and odd arguments appeared in unrelated functions.

AI driving loop (2026-11-11/12, `src/ai.cpp`, Opus): `AIControl`, `AIDoRaceOrder`, `AIRespawnFurther`, `AIWeaponDecideToFire`, `AITestWeapon` are native, calling the physics, effects, weapons, sound, music, camera and task code through `guest_call`. Every call compared on levels 17 and 5: `AIControl` 8,000 to 16,000 calls, `AIDoRaceOrder` 750 to 2,800, with 0 differences of their own. About 1 `AIControl` call in 4,000 shows two floats (-1.0 against 0.0) in the heap near 0x10cd000, or a halfword at 0x4c2a54 near the music driver's SIF data. `RC_SHADOW_SELFTEST=1`, which compares the original with itself, shows the same at the same rate, so something outside the function (another emulation thread) writes there between the two runs. `AIWeaponDecideToFire` has random inputs; `AIControl`, `AITestWeapon` and `AIRespawnFurther` do not (they would put copied cars into the real task list, spawn real weapons, move the camera), and `AIRespawnFurther` and the weapon paths of `AITestWeapon` never ran in the races compared.
- `GuestFrame frame(size)` lowers $sp by the original's frame size for the whole native function, so guest callees build their frames where they would under the original, and `frame.local<T>(offset)` places locals passed by address (to `WeaponSpawn`, `AColDownCast`). Use it in every native function that calls guest code.
- Harness: the compared native now also runs with checkpoints held; `RC_SHADOW_DIFFS=1` lists every differing byte range of a live mismatch; `RC_SHADOW_SELFTEST=1` compares the original against itself, to tell noise from bugs. Functions that reach into the task list, weapon spawning or the camera cannot use the random test.

Renderer, first layer (2026-11-12, `src/rdr.cpp`, Opus): all of RDRSTATE except `RdrState_PkForceApplyStateBlock` (SDK packet calls), RDR's DMA chain (`Rdr_AddDmaCall`, `Rdr_UploadProg`, `Rdr_StartFrame`, camera selection, the empty debug draws, `Rdr_Term`), `Rdr_WhichProg`, `GfxP2_BeginScene`/`EndScene`, and the small RENDER and TEXTURE functions. Every call compared on level 18 plus 500 random inputs each: 0 differences.
- Ghidra printed `GfxP2_BeginScene` and `GfxP2_EndScene` as empty: their only work was a store in a delay slot and a gp-relative global write (clearing `zWriteEnabled`; setting the EOP bit on the last GIF tag and clearing `gLastTex`). Check the instructions of any function Ghidra shows as empty before writing it as such.

Polygons (2026-11-12, `src/rdr.cpp`, `src/vifpk.cpp`, Opus): all of RDRPOLY (HUD, menu and effect polygons batched into VIF packets) and `RdrState_PkForceApplyStateBlock`, on top of native versions of the SDK's VIF packet builder (`sceVif1PkInit`, `Reset`, `Terminate`, `Ret`, `AddCode`, `AddUpkData32/32N/64/128`, `OpenUpkCode`, `CloseUpkCode`) and three libvu0 helpers (`sceVu0CopyVector`, `ScaleVector`, `ApplyMatrix`). Level 18: `RdrPoly` 2,545 calls, `RdrPolyTexture` 2,284, `sceVu0ApplyMatrix` 177, all 0 differences. `RdrTri` never ran.
- The game builds polygons in the scratchpad (0x70000000), which is separate memory. `guest_host` and `guest_addr` now map it; before, native code read main RAM there instead. The comparison still checks only main RAM, so writes to the scratchpad are not compared.
- Random inputs that follow fully random pointers still differ now and then (about 1 in 100 for the packet builders): addresses such as 0xd2233e44 are hardware ranges the runtime routes to `Load`/`Store`, while `guest_host` masks them into RAM. Real data never points there.
- Unchecked indices again: the polygon channel number is used as given, so a random one walked off the C++ array into host memory and crashed the test. Channels are now addressed as the original does.

Shapes (2026-11-12, `src/shape.cpp`, Opus): all of RDR_SHAPE. `TRdr_VUShape` (cars and scenery) compared on levels 18 and 5, about 620 sampled calls, 0 differences; `Rdr_ShapeInit` on every call. The shiny-pass branches (`gEnvMap` set), `TRdr_VUShapeCell`, `TERdr_VUShape` (Track Editor), `Rdr_VUShape`/`Rdr_VUShapeSetup` (version 2 shapes) and the load-time fix-ups (`Rdr_FixVUShape`, `Rdr_FixupShapeTex`, `Rdr_FixVU*Dma`, `Rdr_ShapeConstruct`) never ran in the races compared.
- The Track Editor renderer uses its own VU1 program at 0x1dfc50, which Ghidra also calls `_dma_48`; drawing with the shape program showed no pieces in the editor (play test 2026-11-12, every `TERdr_VUShape` call differed). Fixed; 500 random inputs now match. `Rdr_VUShape`, `Rdr_VUShapeSetup` and `TERdr_VUShape` have random inputs.
- Ghidra names two different DMA templates `_dma_1`: the shape ring copies 0x1d8880, the Track Editor ring 0x1dec80. Check addresses of symbols with a numeric suffix in the instructions.
- `FlushCache(0)` (a syscall) is left out of native code: the runtime models no cache.
- RDRALPHA (transparent triangles, sorted back to front once a frame) is native in the same file: `RdrAlphaAddToDmaChain` matched all 1,880 calls on level 18 and `RdrAlphaAddTriangles` 65 sampled ones. The sort still calls the game's own `qsort` through `guest_call`, so equal distances keep the original's order.
- Play testing with the comparison on: use `RC_SHADOW_FIRST=0` (no forced comparison of each function's first 64 calls), a low `RC_SHADOW_RATE` and `RC_SHADOW_ALWAYS` only for rare functions. With the defaults and a hot function in the list, the game ran at 1-2 fps (2026-11-12). `PLAY_BINARY=<copy>` lets `tools/play.sh` run a copy of the build, so rebuilding does not disturb the session.
- RDR_PARTICLE (`src/particle.cpp`): rain only, and never drawn (the renderer half was left empty in the game). Two original bugs kept: all three axes move by velocity.x, and removing the first system reads the next pointer through $a1, which the caller never sets. GFX_P2's register helpers (`GfxP2_DefRenderState`, `GfxP2_Scissor`, `GfxP2_ZWriteEnable/Disable`, `GfxP2_RenderDmaSend`) are in `src/rdr.cpp`. All pass 500 random inputs; the rain systems never ran in the races compared.
- RDR_SKIN and RDR_WATER are native too (same file). `Rdr_Water` matched every call on levels 5 and 18 (5,180 calls). `Rdr_VUSkin` (animated drivers) is never called in demo races; its only caller is the animation code (podium, menus), so it is unchecked, as are the skin fix-ups. Skin and water use different programs and templates although Ghidra calls both `_dma_51` and `_dma_1`.

`RC_SHADOW_ALWAYS=name,name` compares every call of those functions, for reaching rare paths; `RC_SHADOW_RATE=n` changes the sampling rate. In demo races boats and water run on levels 5, 8, 9, 11, 12, 17, 20, 21, 23 and 24; "other object" collisions on 5, 17 and 25; car-on-car collisions on nearly all.
- Measured load before conversion, 30 s demo, level 18: TRdr 14.81 s, Water 2.36 s, Env 2.24 s, Skin 0.96 s. Rdr_VUShape and TERdr did not run in the demos tried, so they need another way to exercise them.

Sprites (2026-11-12, `src/sprite.cpp`, Opus): all of SPRITE (HUD, menu and font sprites drawn straight to the GS through GFX_P2's packet buffer) and GFX_P2's texture setters (`GfxP2_Tex`, `GfxP2_TexExt`, `GfxP2_TexFX`, in `src/rdr.cpp`). Every sprite variant passes 2,000 random inputs; `SpriteRenderFont` matched all 924 live calls on level 18. One shared vertex emitter (`SpriteStrip`) and one quad builder cover the variants; they differ in screen size (half height, 8.8 scale, double width), texture flags and depth constants (each has its own, at 0x327974-0x327998). Left in GFX_P2: `GfxP2_Init`, `GfxP2_Term`, `GfxP2_LoadTex`, `GfxP2_LoadTexMip` and `GfxP2_RenderDmaSendActual`, which mostly call the SDK's GS library.

### Open bug: speedometer bar flashes to full (2026-11-12)

A region-only live comparison of every polygon call over a 5-minute demo race on level 17 matched all of them (100,311 `RdrPoly`, 38,165 `RdrPolyTexture`, 1,648 `RdrPolyCamera`), so the data they write is right; the cause lies outside it. Seen in play tests with the native renderer: the speedometer bar briefly shows full length, a few times a minute, more often when firing weapons but also without. Not seen with `RC_DECOMP=0`, nor with the functions in `src/rdr.cpp` and `src/vifpk.cpp` skipped (`RC_DECOMP_SKIP`), nor with only the RDRPOLY/VIF-packet half skipped; still seen with only the three VU0 helpers skipped, with `RC_DECOMP_CHARGE=2000` (extra emulated cycles per native call) and with `PS2X_MTVU=0`. Every comparison of these functions has matched, including all calls of `RdrZedsWriteOff/On` and `RdrPolyFlush` in a play session, and the code was checked against the instructions by hand. Caveat: several no-flicker runs were short and had little or no firing (the recordings `decomp/recordings/flicker_*.pad` show the buttons; 0x2000 is fire). The speedometer is drawn by `HudBottomRight` -> `HudRotSpriteHI` (a rotated mask sprite using the depth buffer, through GfxP2, plus `sceVu0ApplyMatrix`). Next experiment: `PS2X_GS_THREAD=0`. The HUD sprites go through GfxP2's double-buffered packet (`GfxP2_RenderDmaSendActual` swaps `gpGfx_DmaBuff0/1` and sends the full one with `sceDmaSendN`); if the GS thread still reads a buffer while the game refills it, faster native code would expose that race with no data difference. Tools for the next attempt: `registerRegions` gives a function a cheap region-only live comparison (the polygon functions have one), `RC_SHADOW_FIRST=0`/`RC_SHADOW_RATE` keep a play test playable, `PLAY_BINARY` runs a copy.

## Where to resume (2026-11-11)

Next: the rest of PLAYER (`PlayerControl`, `PlayerGetInput`, `PlayerSpawnWeapon`, `PlayerTestWeapon`; they only run with a human playing, so they need a recorded session, see `tools/play.sh record`), and the rest of CARCOL once the effects and sound it calls are native, then the main loop, then the Vulkan renderer. Undecided: whether to chase the remaining slowdown on heavy levels (GPU at 95-100%, not resolution-bound) before that or leave it for the Vulkan stage; the recommendation was to leave it. `tools/decomp_extract.py UNIT...` prints a unit's Ghidra output, `tools/shadow.sh` checks it.

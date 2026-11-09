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
- Track shader: `decomp/shaders/trdr_shape.vert` reads the triangle data as the game sends it to VU1. `tools/shader_check.cpp` compares it with `trdr_shape.cpp` on runs captured with `RC_VU1_REF=1 RC_VU1_CAPTURE=<file>`, on the GPU through transform feedback. On 2026-11-10, over levels 1, 3 and 18: 156,497 triangles, no facing test disagreements, and on the 48,057 comparable triangles the worst errors were 1.13/16 of a pixel, 7 GS depth units out of 16.7 million, and no colour difference. Triangles cut by the near plane or crossing the guard band are left to the GPU's clipper and are not compared; the original keeps the colours of cut corners unchanged, which the GPU will interpolate. The shader is not used by the game yet.
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

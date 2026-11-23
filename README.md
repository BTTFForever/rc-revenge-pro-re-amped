# RC Revenge Pro: Re-Amped

A native decompilation PC port of the PlayStation 2 game *RC Revenge Pro* (2000). It runs on Linux (x86-64) as an
ordinary program built from C++ source. The USA (SLUS-20153, NTSC) and Europe (SLES-50077, PAL) releases are both
supported.

> **Project direction (November 2026).** This repository now has two lines of work:
> - **`main`: the enhanced recomp.** The goal is to play the game well on a modern PC: high frame rates and
>   resolutions, widescreen, efficient performance, new features and removed limits. Exact PS2 accuracy is no longer
>   a requirement here. Bugs are fixed as they appear, and the verification tools below are kept but no longer gate
>   changes. The decompiled native code and the native Vulkan renderer are kept and built on.
> - **`decomp-exact`: the accuracy-first decompilation**, frozen at tag `decomp-exact-2026-11-22`. Every native
>   function is behaviour-matching and oracle-checked, with an exact native Vulkan renderer (P1, P2 through batch 6).
>   It is the base for anyone who wants to continue towards a fully native, PS2-free PC port.
>
> The description below was written for the decompilation and still applies to the code both lines share.

## Enhanced recomp: playing with the enhancements (`main`)

Run `tools/play.sh` (on a hybrid-graphics laptop it uses the discrete NVIDIA GPU) and set things up in the game:
**Game Options > Enhancements**, or in a race **Pause Menu > Enhancements** (start, then the last item). Changes apply
at once, mid-race too (resolution at the next start), and are saved to `~/.config/rc-revenge-pro/settings.ini`:

| Setting | Choices | What it does |
|---|---|---|
| Aspect ratio | 4:3, 16:9, 16:10, 21:9 | Hor+ widescreen: the 3D view widens, nothing is stretched |
| HUD and menus | Corrected, Stretched | In widescreen: HUD pinned to the screen's corners and menus kept in shape (pillarboxed), or the original layout stretched |
| Display | Window, Fullscreen | True fullscreen on the current monitor (F11 toggles too) |
| Resolution (restart) | 1x to 6x | Internal resolution (3x-4x for 1080p-1440p) |
| Race frame rate | 60, 90, 120, 144, 165, 240 | Races run up to this rate (match your screen); menus keep the original rate; game speed unchanged |
| Smooth motion | On, Off | Cars, wheels and camera drawn between the game's 50 Hz physics steps: even motion at any frame rate (also at 60) |
| V-Sync | Off, On, Adaptive | The window's swap interval. Adaptive (late frames tear instead of waiting) needs `GLX_EXT_swap_control_tear` from the X server; under XWayland with PRIME offload it is not offered, so it acts as On |
| Variable refresh | Off, On | For a VRR (FreeSync/G-SYNC/Adaptive Sync) screen: each new picture is shown when it is ready (menus at the game's 50/60 Hz without judder), races capped 3 below the screen's rate (141 on 144 Hz). The desktop must allow VRR for the window (KDE: Display Configuration, Adaptive sync "Always", or fullscreen with "Automatic") |

The same Game Options menu also shows the developers' **Debug Menus**, left hidden on the disc. Apart from Mirror
Mode (the game's normal mirrored tracks), its items are not connected to anything in the released game.

All settings work in both versions. In Europe (PAL, 50 Hz video) the menus keep 50 and races run at the chosen
rate; the physics step is the same 20 ms in both, so game speed is unchanged.

**Controls**: each player is a virtual DualShock 2 read from the keyboard and/or a gamepad. In **Game Options >
Controls** (the game's own Controller configuration page, which keeps its button layouts and vibration settings) the
items **Player 1 PC Controls...** and **Player 2 PC Controls...** were added; in a race, **Pause Menu > Controls** leads
to the same pages. Each has the player's device (None, Keyboard, Gamepad 1-4, Keyboard + Gamepad 1/2, with the
connected gamepads' names; defaults: player 1 keyboard + first gamepad, player 2 the second gamepad), **Buttons...** and
**D-pad and sticks...** pages, and Reset to defaults. To rebind, press left or right on a control, then the new key or
gamepad button, stick direction or trigger (Escape cancels, Delete clears, after 6 s it gives up; an input another
control of that player already had moves to that control). Each control shows what it does in the player's chosen
layout ("Cross - Accelerate"). Gamepads go through SDL3 (its controller database, hot-plugging, rumble for the game's
vibration, sticks analog; a button bound to a trigger is pressure-sensitive, as the game reads pressure for accelerate
and handbrake). Valve controllers (Steam Controller 2015 and 2026, Steam Deck) are opened through SDL's HIDAPI drivers,
which switch off their keyboard-and-mouse "lizard mode"; that needs Steam's udev rules (the `steam-devices` package,
`60-steam-input.rules`), and the game logs it if such a controller is present but its hidraw node is not readable.
Keyboard defaults for player 1: arrows, F cross, E circle, R square, Space triangle, Q L1, Left Shift R1, Left Ctrl L2,
Right Shift R2, Enter start, Tab select, WASD left stick; player 2 (when set to the keyboard) IJKL, N, M, B, U, O, P, Y, 7,
0, 9. Saved in the same settings file (`p1_device=keyboard+pad1`, `p1_key_cross=F`, `p1_pad_cross=a`, ...).
`PS2X_GAMEPAD_LOG=1` logs every SDL joystick and gamepad (name, GUID, path, driver, mapping).

For one run, an environment variable overrides the file: `RC_ASPECT=16:9`, `PS2X_GS_HW_SCALE=<n>`,
`PS2X_FULLSCREEN=1`, `PS2X_REFRESH_HZ=<n>`, `RC_INTERP=0`, `PS2X_VSYNC=off|on|adaptive`, `PS2X_VRR=0|1`,
`PS2X_PAD1_DEVICE`/`PS2X_PAD2_DEVICE=keyboard|pad1|...|keyboard+pad1`, `PS2X_BIND_<BUTTON>=<key>` (player 1's keyboard),
`PS2X_GAMECONTROLLERDB=<gamecontrollerdb.txt>` (extra SDL mappings).
`PS2X_PRESENT_LOG=<file>` writes the window's presentation once a second (presents and game fields per second, mean
and longest gap, swap interval, VRR, monitor rate). `PS2X_PAD_LIVE=<file>` drives a running game from a test: lines
`<buttons hex> <hold ms> <gap ms> [port]` appended to the file are pressed in order on pad port 0 (or 1, player 2)
(0x0008 start, 0x4000 cross, 0x1000 triangle, 0x0010/0x0040/0x0080/0x0020 up/down/left/right); with
`PS2X_SDL_VIRTUAL_PADS=<n>` the game also gets n virtual SDL gamepads, pressed by lines `pad<n> <SDL button or
+/-axis> <hold ms> <gap ms>` (`pad1 a 100 300`, `pad2 +righttrigger 2000 100`), through the whole gamepad path.
`PS2X_RUMBLE_LOG=1` logs the game's motor values and what the virtual gamepads receive. Switches for comparing or bypassing the other enhancements
(all on by default): `RC_WATER_SHADER=0` (water on the CPU), `PS2X_GS_DIRECT_PRESENT=0` (frames copied through RAM),
`PS2X_GS_HW_TRACK_MULTITEX=0` and `..._DIRECT=0` (fewer draws), `PS2X_GS_HW_MULTIPASS=0`. `RC_BOOT_LEVEL=<n>` starts
the attract demo on level n, `RC_DEMO_SECS=<n>` lengthens it, `tools/track_sweep.sh` measures every track's frame rate
over a lap. `PERF_INVESTIGATION.md` records each change, how it was checked and what it gained.

## How it works

1. **Static recompilation as the scaffold.** [PS2Recomp](https://github.com/ran-j/PS2Recomp) translates the game's
   MIPS R5900 executable into C++ ahead of time. The result, linked against PS2Recomp's runtime, plays the game
   from start to finish. This gave a working build on day one and a reference for everything that followed.
2. **Function-by-function decompilation.** Each of the game's own functions was then rewritten as readable native
   C++ (`decomp/src/`) and swapped in for its recompiled version (registered with `RC_DECOMP_FN*`). The rewritten
   functions match the original's behaviour, not just its general effect: the same results, the same writes to
   memory, the same float rounding.
3. **Verification.** Every native function is checked against its recompiled original in two ways:
   - an **offline oracle** (`tools/oracle.sh`): both versions run on the same RAM snapshots of real game states with
     random inputs, and their results, all of guest memory and any hardware register writes are compared;
   - **live shadow comparisons** (`RC_SHADOW=1`): while the game runs, sampled calls are run both ways and compared.
4. **USA and Europe from one source.** The native code never writes a raw address. It names symbols, and per-version
   headers generated from each executable give their addresses. Functions that differ between the two releases have
   both variants in one body under `#if RC_VERSION_EU`.

## Status

- **All of the game's code is native**: every function from the game's own source files (1,730 of the executable's
  2,486 functions) is decompiled C++ and checked by the oracle on both versions.
- **What remains is the console SDK and C library** (about 756 functions). About 350 of them are already replaced by
  the runtime's host implementations, and the remaining ~400 are still recompiled.
- **Rendering still goes through the runtime's emulation of the PS2's Graphics Synthesizer** (an OpenGL backend), with
  the VU1 vector unit programs recompiled. The track's VU1 program already has a checked C++ version and a GPU shader.
- **Next: a native Vulkan renderer.** The plan (see [`RENDERER_PLAN.md`](RENDERER_PLAN.md)) is to first read the
  game's own draw packets on the host and draw them with Vulkan, keeping every native function's output unchanged
  so the checks stay valid. Draw paths will then move to a native rendering API one at a time, where PC features
  such as widescreen, HUD anchoring and frame interpolation need it.
- **The long-term goal is no PS2 hardware emulation at all**: native code throughout, host libraries in place of the
  SDK, and the game's data formats read directly.

Known limits: the memory card is emulated as files (saving and loading work); some performance and graphics issues
are listed in [`NOTES.md`](NOTES.md). This is a work in progress, not a finished port.

## Requirements

- Linux on x86-64 with AVX2 (the build uses `-mavx2`). Other platforms are untested.
- CMake 3.21+, Ninja, a C++20 compiler (GCC 13 or later is what this is developed with), Python 3, pkg-config.
- Development packages for FFmpeg (libavcodec, libavformat, libavutil, libswresample, libswscale), OpenGL and X11
  (raylib's dependencies: libX11, libXrandr, libXinerama, libXcursor, libXi, or the Wayland equivalents).
- Network access the first time you configure: CMake downloads raylib, imgui, rlImGui, ELFIO, toml11, fmt, libdwarf,
  rabbitizer, Vulkan-Headers and volk.
- About 2-3 GB of disk space per version (extracted disc, recompiled code and build), and a few GB of RAM to compile.
- **Your own RC Revenge Pro disc image**: USA (SLUS-20153) or Europe (SLES-50077), as a `.iso` or a raw `.bin`/`.cue`
  dumped from your disc. The setup checks that the executable on it is the expected one.

## Building

```sh
tools/setup.sh /path/to/your/RC-Revenge-Pro.iso     # or the .cue / .bin
```

`tools/setup.sh` reads the version from the disc, then:

1. puts the image and its extracted files in `game/usa/` or `game/eu/`;
2. builds the recompiler (`third_party/ps2recomp/ps2xRecomp`) and recompiles the executable into `output/` or `output_eu/`;
3. generates the headers the native code compiles against from the executable's debug data;
4. converts the VU1 microprograms;
5. builds the game in `build-decomp/` (USA) or `build-eu/` (Europe).

The first build takes a while (the recompiled code is large; roughly 15 minutes on 8 cores). Options: `--oracle` also
builds the offline oracle, `--no-build` stops after configuring, `-j N` sets the build jobs. Run it once for each
version you own. Everything it creates is ignored by git. Details and the manual steps are in `NOTES.md`, "Rebuilding".

## Running

```sh
tools/play.sh                     # USA
RC_VERSION=eu tools/play.sh       # Europe
```

The game starts from a clean memory card each time (`<build>/play/`); its output goes to `<build>/play/session.log`.
`tools/play.sh record <name>` and `tools/play.sh replay <name>` record and replay the controller
(`decomp/recordings/`). On a hybrid-graphics laptop with an NVIDIA GPU, `play.sh` runs the game on it through PRIME
render offload.

Useful settings (environment variables): `PS2X_GS_HW_SCALE=<n>` sets the internal resolution multiple (default 2);
`RC_BOOT_LEVEL=<n>` starts the attract-mode demo on level n; `RC_SKIP_INTRO=1` skips the intro movies;
`RC_DECOMP=0` runs the recompiled original of every function instead of the native code. More in `NOTES.md`.

### Controls

Keyboard (a gamepad also works through raylib):

| PS2 | Keyboard |
|---|---|
| D-pad | Arrow keys |
| Left stick | W A S D |
| Cross | F |
| Circle | E (with `play.sh`) |
| Square | R (with `play.sh`) |
| Triangle | Space |
| L1 / R1 | Q / Left Shift |
| L2 / R2 | Left Ctrl / Right Shift |
| Start / Select | Enter / Tab |
| L3 / R3 | H / G |

Any button can be rebound with `PS2X_BIND_<BUTTON>=<key>` (for example `PS2X_BIND_CROSS=SPACE`).

## Checking the decompilation

```sh
tools/setup.sh --oracle <image>   # builds build-oracle (or build-oracle-eu) as well
tools/capture_dumps.sh            # runs the game on a few levels and saves RAM snapshots (game data: kept local)
tools/oracle.sh --all             # every native function against its recompiled original
tools/shadow.sh <level> <seconds> # live comparison during a demo race
```

Prefix with `RC_VERSION=eu` for the Europe version. See `NOTES.md`, "Offline oracle".

## Repository layout

| Path | Contents |
|---|---|
| `decomp/src/` | The decompiled game code, one file per original source unit, plus the oracle setups (`*_fuzz.cpp`) |
| `decomp/vu1/`, `decomp/shaders/` | C++ references of the VU1 programs and the track shader |
| `decomp/oracle/` | The offline oracle |
| `decomp/rc_decomp.*`, `decomp/rc_guest.h` | Registration, guest memory access and the shadow checker |
| `decomp/recordings/` | Controller recordings used in testing |
| `overrides/` | Game-specific runtime overrides (disc access, memory layout, movie streaming) |
| `tools/` | Setup, run, check and analysis scripts |
| `third_party/ps2recomp/` | PS2Recomp's recompiler and runtime, vendored with local changes (`VENDORED.md`) |
| `third_party/ico_platform/` | The fiber layer from ico-pc |
| `third_party/ico_rhi/` | ico-pc's render hardware interface, Vulkan backend (`VENDORED.md`); checked by `tools/rhi_triangle` |
| `third_party/port_engine/` | ps2-port-engine's experimental Vulkan GS renderer (`PS2X_GS_VK=1`) |
| `config.toml`, `config_eu.toml`, `functions_*.csv` | Recompiler configuration and function maps (names and addresses) |
| `NOTES.md` | The working notes: how everything works, findings, history |
| `RENDERER_PLAN.md`, `ICO_RENDER_STUDY.md` | The native renderer plan and the study behind it |
| `AGENT_USAGE.md` | Log of AI-assisted work on the project |

## Licence

GPL-3.0 (see [`LICENSE`](LICENSE)), because the PS2Recomp runtime this links against is GPL-3.0.

Third-party code in this repository keeps its own licence:

- `third_party/ps2recomp/`: PS2Recomp, GPL-3.0.
- `third_party/ico_platform/`: from ico-pc, MIT; `minicoro/` inside it, MIT or public domain (Unlicense).
- `third_party/ico_rhi/`: from ico-pc, MIT.
- `third_party/port_engine/`: from ps2-port-engine, MIT; `gpu/third_party/smaa/`, SMAA, MIT.

Dependencies fetched at build time (raylib, imgui, rlImGui, ELFIO, toml11, fmt, libdwarf, rabbitizer, Vulkan-Headers,
volk; SDL3 and glslang when no system copy is found) are under their own licences.

## Credits

- [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j and contributors: the static recompiler and runtime.
- The Shadow of the Colossus PS2Recomp fork by LightVelox ([github.com/LightVelox/PS2Recomp](https://github.com/LightVelox/PS2Recomp)):
  the VU1 recompiler, GPU GS backend and threading this project builds on.
- The God of War recomp by KIexster: the render-target renderer and Linux EGL context code in the runtime.
- [ico-pc](https://github.com/nathanialf/ico-pc): the reference for the decompilation approach and the renderer
  plan, and the source of the fiber layer and the render hardware interface (RHI).
- [ps2-port-engine](https://github.com/danielgomesvieira2000/ps2-port-engine): the experimental Vulkan GS renderer.
- ghidra-emotionengine-reloaded: Ghidra support for the Emotion Engine, used during analysis (not needed to build).

## Disclaimer

This is an unofficial fan project. It is not affiliated with, endorsed by or connected to the original developers,
publishers or rights holders of *RC Revenge Pro*, or to Sony Interactive Entertainment. "RC Revenge Pro" and
"PlayStation" are trademarks of their respective owners. No copyrighted game data is included or distributed; you
must own the game to use this.

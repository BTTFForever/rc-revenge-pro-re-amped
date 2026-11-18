# RC Revenge Pro: Re-Amped

A native decompilation PC port of the PlayStation 2 game *RC Revenge Pro* (2000). It runs on Linux (x86-64) as an
ordinary program built from C++ source. The USA (SLUS-20153, NTSC) and Europe (SLES-50077, PAL) releases are both
supported.

**No game data is included or distributed.** You need your own legal copy of the game disc, PAL or NTSC. The setup
script reads everything it needs from your disc image.

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
- `third_party/port_engine/`: from ps2-port-engine, MIT; `gpu/third_party/smaa/`, SMAA, MIT.

Dependencies fetched at build time (raylib, imgui, rlImGui, ELFIO, toml11, fmt, libdwarf, rabbitizer, Vulkan-Headers,
volk) are under their own licences.

## Credits

- [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j and contributors: the static recompiler and runtime.
- The Shadow of the Colossus PS2Recomp fork by LightVelox ([github.com/LightVelox/PS2Recomp](https://github.com/LightVelox/PS2Recomp)):
  the VU1 recompiler, GPU GS backend and threading this project builds on.
- The God of War recomp by KIexster: the render-target renderer and Linux EGL context code in the runtime.
- [ico-pc](https://github.com/nathanialf/ico-pc): the reference for the decompilation approach and the renderer
  plan, and the source of the fiber layer.
- [ps2-port-engine](https://github.com/danielgomesvieira2000/ps2-port-engine): the experimental Vulkan GS renderer.
- ghidra-emotionengine-reloaded: Ghidra support for the Emotion Engine, used during analysis (not needed to build).

## Disclaimer

This is an unofficial fan project. It is not affiliated with, endorsed by or connected to the original developers,
publishers or rights holders of *RC Revenge Pro*, or to Sony Interactive Entertainment. "RC Revenge Pro" and
"PlayStation" are trademarks of their respective owners. No copyrighted game data is included or distributed; you
must own the game to use this.

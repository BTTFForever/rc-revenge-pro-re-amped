# Vendored PS2Recomp

A subset of the Shadow of the Colossus fork of PS2Recomp, with this project's local changes.

- Upstream: https://github.com/LightVelox/PS2Recomp.git (a fork of https://github.com/ran-j/PS2Recomp)
- Base commit: `d06a4ab` ("Add an F3 performance overlay to the host window")
- Licence: GPL-3.0 (`LICENSE`), which is why this whole repository is GPL-3.0.

## What is here
Only what this project builds and runs:
- `ps2xRecomp/`: the static recompiler (`ps2_recomp`), run by `tools/setup.sh` on the game's executable.
- `ps2xRuntime/`: the runtime the recompiled and decompiled code links against (without `vita/`, the PS Vita modules).
- `ps2xIOP/`: the IOP emulation the runtime uses.
- `ps2xTest/tools/vu1_recomp.cpp`: the VU1 microcode recompiler (the `vu1_recomp` target in the top-level `CMakeLists.txt`).
- `LICENSE`, `README.md` from upstream.

Left out: `out/` and other build folders, `android/`, `vita/`, `ps2xStudio/`, `ps2xAnalyzer/`, the rest of `ps2xTest/`,
the upstream top-level `CMakeLists.txt` (each component above has its own; `ps2xRecomp/` is configured on its own by
`tools/setup.sh`, and the project's `CMakeLists.txt` adds `ps2xIOP/` and `ps2xRuntime/` as subdirectories).

## Dependencies fetched at configure time
Nothing is a git submodule. CMake's FetchContent downloads, from their public repositories:
- `ps2xRecomp`: ELFIO (Release_3.12), toml11 (v4.4.0), fmt (12.1.0), libdwarf (v2.2.0), rabbitizer (1.14.3).
- `ps2xRuntime`: raylib (5.5), imgui (v1.92.7-docking), rlImGui (Raylib_5_5). FFmpeg comes from the system (pkg-config).

So the first configure needs network access. The project's `RC_FETCH_CACHE` CMake option can point at existing
`<name>-src` checkouts to build offline.

## Local changes
The commit that added this folder ("Vendor PS2Recomp (upstream d06a4ab, unmodified)") holds the upstream files
unchanged, and the commit after it applies the local changes, so
`git diff <that commit> -- third_party/ps2recomp` lists every local change at any later point. What each change does
and why is in `NOTES.md`, "Local changes to the runtime". The earlier `patches/ps2recomp-sotc-local.patch`
(the same changes as a patch against `d06a4ab`, kept while the runtime lived outside the repository) was removed
when the runtime was vendored, since the git history now records them.

Edit the runtime here, in place, and describe the change in `NOTES.md`.

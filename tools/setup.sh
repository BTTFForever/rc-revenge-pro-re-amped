#!/bin/bash
# From your own RC Revenge Pro disc image to a working build. No Ghidra and no game data from anywhere else.
#
#   tools/setup.sh [options] <disc image>
#
# <disc image>: a .iso (2048-byte sectors), or a raw .bin (2352-byte sectors) or its .cue. The version is read from the
# disc: SLUS-20153 (USA, NTSC) or SLES-50077 (Europe, PAL). Run it once per version you own; both can sit side by side.
#
# Options:
#   --oracle      also build the offline oracle (build-oracle or build-oracle-eu); tools/capture_dumps.sh then makes
#                 its RAM dumps and tools/oracle.sh runs it
#   --no-build    stop after generating the code (recompiled code, headers, VU1 code) and configuring
#   -j N          parallel build jobs (default: ninja's)
#
# What it does (all output is ignored by git: game/, output*/, vu1*/, decomp/generated/, build*/):
#   1. game/<version>/: the disc image (a link to a .iso, or a .iso converted from a .bin) and its files in disc/.
#   2. Checks the executable is the one this project was made from.
#   3. Builds the recompiler (third_party/ps2recomp/ps2xRecomp) in build-tools/ps2recomp.
#   4. Recompiles the executable (config.toml or config_eu.toml, with the tracked function map) into output/ or output_eu/.
#   5. Generates the headers the decompiled code compiles against (tools/stabs2c.py, tools/gen_addrs.py).
#   6. Converts the VU1 microprograms (tools/make_vu1_images.py, vu1_recomp) into vu1/.
#   7. Configures and builds the game in build-decomp (USA) or build-eu (Europe).
# Needs: cmake (3.21+), ninja, a C++20 compiler (GCC 13+ or Clang), python3, pkg-config, FFmpeg, OpenGL and X11/Wayland
# development files (for raylib), and network access on the first configure (CMake fetches the dependencies).
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

oracle=0; build=1; jobs=()
while [ $# -gt 0 ]; do
    case "$1" in
        --oracle) oracle=1; shift ;;
        --no-build) build=0; shift ;;
        -j) jobs=(-j "$2"); shift 2 ;;
        -j*) jobs=("$1"); shift ;;
        -h|--help) sed -n '2,28p' "$0"; exit 0 ;;
        -*) echo "unknown option $1" >&2; exit 2 ;;
        *) break ;;
    esac
done
[ $# = 1 ] || { sed -n '2,8p' "$0" >&2; exit 2; }
image="$1"
[ -f "$image" ] || { echo "no such file: $image" >&2; exit 2; }
for tool in cmake ninja python3; do
    command -v "$tool" >/dev/null || { echo "$tool is required" >&2; exit 2; }
done
step() { printf '\n== %s\n' "$*"; }

step "Reading the disc"
elf="$(python3 -I tools/iso_extract.py boot "$image")"
case "$elf" in
    SLUS_201.53) version=usa; iso_name="RC Revenge Pro (USA).iso"; sha1=a3f472d489e688525ea5e5513ce4c9dc6e6acd93
                 out=output; config=config.toml; game_build=build-decomp; oracle_build=build-oracle ;;
    SLES_500.77) version=eu; iso_name="RC Revenge Pro (Europe).iso"; sha1=6957407aeb69c89f614a8a675bfcaae92dca83b3
                 out=output_eu; config=config_eu.toml; game_build=build-eu; oracle_build=build-oracle-eu ;;
    *) echo "this disc boots $elf: not RC Revenge Pro USA (SLUS_201.53) or Europe (SLES_500.77)" >&2; exit 1 ;;
esac
echo "$elf: version $version"
game="$root/game/$version"
mkdir -p "$game"
read -r kind source < <(python3 -I tools/iso_extract.py kind "$image")
rm -f "$game/$iso_name"
if [ "$kind" = raw ]; then
    echo "converting $source to $game/$iso_name"
    python3 -I tools/iso_extract.py to-iso "$source" "$game/$iso_name"
else
    ln -s "$source" "$game/$iso_name" # the game reads its files through the image at run time
    echo "linked $game/$iso_name -> $source"
fi
rm -rf "$game/disc"
python3 -I tools/iso_extract.py extract "$game/$iso_name" "$game/disc"
mkdir -p "$game/disc/mc0" "$game/disc/mc1"
actual="$(python3 -I tools/iso_extract.py sha1 "$game/disc/$elf")"
if [ "$actual" != "$sha1" ]; then
    echo "$elf has SHA-1 $actual, expected $sha1: a different release of the game, which this project does not support" >&2
    exit 1
fi
echo "$elf matches the expected executable"
elf_path="game/$version/disc/$elf"

step "Building the recompiler"
cmake -S third_party/ps2recomp/ps2xRecomp -B build-tools/ps2recomp -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build-tools/ps2recomp "${jobs[@]}" ps2_recomp

step "Recompiling $elf into $out/"
rm -rf "$out"; mkdir -p "$out"
build-tools/ps2recomp/ps2_recomp "$config" > "build-tools/recompile-$version.log" 2>&1 || {
    tail -20 "build-tools/recompile-$version.log" >&2; echo "the recompiler failed: see build-tools/recompile-$version.log" >&2; exit 1; }
echo "$(ls "$out" | wc -l) files (log: build-tools/recompile-$version.log)"

step "Generating the headers for the decompiled code"
python3 tools/stabs2c.py "$elf_path" "decomp/generated/$version"
if [ "$version" = usa ]; then
    python3 tools/stabs2c.py "$elf_path" decomp/generated # the reference listings the decompilation tools read
    python3 tools/gen_addrs.py "$elf_path" "$out" decomp/generated/usa
else
    python3 tools/gen_addrs.py "$elf_path" "$out" decomp/generated/eu decomp/generated/eu/lit4_map.txt \
        decomp/generated/eu/changed_functions.txt
fi

step "Configuring $game_build"
cmake -S . -B "$game_build" -G Ninja -DRC_VERSION="$version"

step "Converting the VU1 microprograms"
ninja -C "$game_build" "${jobs[@]}" vu1_recomp
python3 tools/make_vu1_images.py "$elf_path" vu1_images
rm -rf vu1; mkdir -p vu1
"$game_build/vu1_recomp" vu1_images vu1 > "build-tools/vu1-$version.log" 2>&1 || {
    tail -20 "build-tools/vu1-$version.log" >&2; exit 1; }
cmake -S . -B "$game_build" > /dev/null # picks up vu1/
[ "$oracle" = 1 ] && cmake -S . -B "$oracle_build" -G Ninja -DRC_VERSION="$version" > /dev/null

if [ "$build" = 0 ]; then
    echo; echo "Generated and configured. Build with: ninja -C $game_build rc_revenge_pro"; exit 0
fi
step "Building the game in $game_build (the recompiled code takes a while)"
ninja -C "$game_build" "${jobs[@]}" rc_revenge_pro
if [ "$oracle" = 1 ]; then
    step "Building the offline oracle in $oracle_build"
    ninja -C "$oracle_build" "${jobs[@]}" rc_oracle
fi

echo
echo "Done. Play with: $( [ "$version" = eu ] && echo 'RC_VERSION=eu ' )tools/play.sh"
[ "$oracle" = 1 ] && echo "Oracle: $( [ "$version" = eu ] && echo 'RC_VERSION=eu ' )tools/capture_dumps.sh (CAPTURE_BINARY=$root/$game_build/rc_revenge_pro saves a build), then tools/oracle.sh --all"
exit 0

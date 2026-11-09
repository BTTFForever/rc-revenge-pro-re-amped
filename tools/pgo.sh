#!/bin/bash
# Rebuilds build/ with a profile collected from a scripted run of the game.
# Usage: tools/pgo.sh [training seconds, default 170]
set -e
cd "$(dirname "$0")/.."
secs="${1:-170}"
cmake -S . -B build -DRC_PGO=generate >/dev/null
cmake --build build -j8 --target rc_revenge_pro
find build -name '*.gcda' -delete
RC_PGO_SECONDS="$secs" PS2X_DEBUG_PANEL=0 PS2X_GS_GPU=1 PS2X_GS_THREAD=1 PS2X_MTVU=1 PS2X_VU1_TIMING=0 \
  PS2X_EE_HOST_PACING=1 PS2X_GS_SHADER_PRELOAD=0 PS2X_GS_SHADER_CACHE=1 PS2X_GS_SHADER_CACHE_DIR="$PWD/gs_shaders" \
  timeout $((secs + 30)) build/rc_revenge_pro ../disc/SLUS_201.53 >/dev/null 2>&1
echo "profile files: $(find build -name '*.gcda' | wc -l)"
cmake -S . -B build -DRC_PGO=use >/dev/null
cmake --build build -j8 --target rc_revenge_pro

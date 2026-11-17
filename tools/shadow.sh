#!/bin/bash
# Compares every decompiled function against its recompiled original while a demo race runs.
#   tools/shadow.sh <level> <seconds> [random inputs per function, default 2000]
# Results: build-decomp/shadow/level<n>.txt (per function) and .log (any mismatches); build-eu/shadow with RC_VERSION=eu.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
source "$root/tools/version.sh"
level="${1:?level}"; seconds="${2:?seconds}"; fuzz="${3:-2000}"
out="$RC_GAME_BUILD/shadow"; sandbox="$out/game"
mkdir -p "$out"
rc_make_sandbox "$sandbox"
# The discrete RTX 2060 when it is healthy. The comparison's results don't depend on the GPU.
if nvidia-smi -L >/dev/null 2>&1; then
    export __NV_PRIME_RENDER_OFFLOAD=1 __VK_LAYER_NV_optimus=NVIDIA_only __GLX_VENDOR_LIBRARY_NAME=nvidia
else
    echo "warning: NVIDIA GPU not responding, using the integrated GPU" >&2
fi
cd "$root"
env PS2X_DEBUG_PANEL=0 PS2X_GS_GPU=1 PS2X_GS_THREAD=1 PS2X_MTVU=1 PS2X_VU1_TIMING=0 PS2X_EE_HOST_PACING=1 \
    PS2X_GS_SHADER_PRELOAD=0 PS2X_GS_SHADER_CACHE=1 PS2X_GS_SHADER_CACHE_DIR="$root/build-decomp/shader-cache" \
    RC_BOOT_LEVEL="$level" RC_SHADOW=1 RC_SHADOW_FUZZ="$fuzz" RC_SHADOW_LOG="$out/level$level.txt" \
    timeout "$seconds" "$RC_GAME_BUILD/rc_revenge_pro" "$sandbox/disc/$RC_ELF" > "$out/level$level.log" 2>&1
tail -1 "$out/level$level.txt"
grep -a 'finished' "$out/level$level.log" | tail -1
grep -a 'MISMATCH' "$out/level$level.log" | head -"${SHADOW_SHOW:-12}" | cut -c1-210
awk 'NR>1 && $3>0 {print "mismatches:", $5, $3 " of " $2}' "$out/level$level.txt" | head -24

#!/bin/bash
# Plays the decomp build from a clean memory card, recording or replaying the controller.
#   tools/play.sh record <name>   writes decomp/recordings/<name>.pad
#   tools/play.sh replay <name>   plays it back
# RC_VERSION=eu plays the Europe version (build-eu); the default is usa (build-decomp). See tools/version.sh.
# Any real run of a VU1 program not yet compared in the game is checked as it happens; see build-decomp/play/session.log.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
source "$root/tools/version.sh"
mode="${1:?record or replay}"; name="${2:?recording name}"
pad="$root/decomp/recordings/$name.pad"
sandbox="$RC_GAME_BUILD/play"
rc_make_sandbox "$sandbox"
case "$mode" in
    record) input="PS2X_PAD_RECORD=$pad" ;;
    replay) [ -f "$pad" ] || { echo "no recording $pad"; exit 1; }; input="PS2X_PAD_REPLAY=$pad" ;;
    *) echo "record or replay"; exit 1 ;;
esac
# Always the discrete RTX 2060, never the integrated Vega.
if ! nvidia-smi -L >/dev/null 2>&1 && [ "${PLAY_ALLOW_IGPU:-0}" != 1 ]; then
    echo "The NVIDIA GPU is not responding (nvidia-smi failed), so the game was not started. Rebooting normally clears this." >&2
    echo "Set PLAY_ALLOW_IGPU=1 to run on the integrated GPU anyway." >&2
    exit 1
fi
if nvidia-smi -L >/dev/null 2>&1; then
    export __NV_PRIME_RENDER_OFFLOAD=1 __VK_LAYER_NV_optimus=NVIDIA_only __GLX_VENDOR_LIBRARY_NAME=nvidia
fi
cd "$root"
exec env PS2X_DEBUG_PANEL=0 PS2X_GS_GPU=1 PS2X_GS_HW=1 PS2X_GS_HW_SCALE="${PS2X_GS_HW_SCALE:-2}" PS2X_GS_THREAD=1 PS2X_MTVU="${PS2X_MTVU:-1}" PS2X_VU1_TIMING=0 \
    PS2X_EE_HOST_PACING=1 PS2X_GS_SHADER_PRELOAD="${PS2X_GS_SHADER_PRELOAD:-0}" PS2X_GS_SHADER_CACHE=1 PS2X_GS_SHADER_CACHE_DIR="$root/build-decomp/shader-cache" \
    PS2X_BIND_CIRCLE=E PS2X_BIND_SQUARE=R RC_TRACK_SHADER=1 RC_VU1_REF=2 "$input" \
    "${PLAY_BINARY:-$RC_GAME_BUILD/rc_revenge_pro}" "$sandbox/disc/$RC_ELF" > "$sandbox/session.log" 2>&1

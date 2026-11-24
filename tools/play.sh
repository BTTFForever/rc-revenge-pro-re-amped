#!/bin/bash
# Plays the decomp build from a clean memory card, optionally recording or replaying the controller.
#   tools/play.sh                 plays (no recording)
#   tools/play.sh record <name>   writes decomp/recordings/<name>.pad
#   tools/play.sh replay <name>   plays it back (<name> with a / in it: that .pad file)
# The game's output goes to <build folder>/play/session.log.
# PS2X_GS_GPU, PS2X_GS_HW, PS2X_GS_THREAD, PS2X_MTVU, RC_TRACK_SHADER and RC_VU1_REF can be overridden (tools/frame_record.sh does).
# RC_VERSION=eu plays the Europe version (build-eu); the default is usa (build-decomp). See tools/version.sh.
# The resolution, aspect ratio, fullscreen, race frame rate and smooth motion come from the settings file
# (~/.config/rc-revenge-pro/settings.ini, changed in Game Options > Display); an environment variable overrides it
# for one run (PS2X_GS_HW_SCALE, RC_ASPECT, PS2X_FULLSCREEN, PS2X_REFRESH_HZ, RC_INTERP). RC_VU1_REF=2 checks each VU1
# program not yet compared in the game as it runs (a decompilation check; off by default).
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
source "$root/tools/version.sh"
mode="${1:-play}"
[ "$mode" = play ] || name="${2:?recording name}"
pad="$root/decomp/recordings/${name:-}.pad"
case "${name:-}" in */*) pad="$name" ;; esac # a path to a .pad file outside decomp/recordings
sandbox="$RC_GAME_BUILD/play"
rc_make_sandbox "$sandbox"
[ -x "${PLAY_BINARY:-$RC_GAME_BUILD/rc_revenge_pro}" ] || { echo "no game build at $RC_GAME_BUILD: run tools/setup.sh first" >&2; exit 1; }
case "$mode" in
    play) input="PS2X_PAD_REPLAY=" ;;
    record) input="PS2X_PAD_RECORD=$pad" ;;
    replay) [ -f "$pad" ] || { echo "no recording $pad"; exit 1; }; input="PS2X_PAD_REPLAY=$pad" ;;
    *) echo "play, record or replay"; exit 1 ;;
esac
# On a hybrid-graphics machine with an NVIDIA GPU, always run on it (PRIME render offload), never the integrated GPU.
# (rc_use_nvidia, tools/version.sh: refuses to start when the GPU is listed but not working.)
rc_use_nvidia || exit 1
# RC_RENDERER=vk (NOTES.md): SDL3 window and input, Vulkan presentation through the RHI. RC_VK_VALIDATION=1 adds the
# Khronos validation layer with synchronisation validation (its messages go to session.log as "rhi_vk validation
# error/warning"); with no layer installed, RC_VVL_LIB=<libVkLayer_khronos_validation.so> names one, as for
# tools/rhi_triangle/run.sh.
if [ "${RC_VK_VALIDATION:-0}" = 1 ]; then
    export ICO_VK_VALIDATION=1 VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true
    if [ -n "${RC_VVL_LIB:-}" ]; then
        mkdir -p "$RC_GAME_BUILD/vvl"
        lib="$(cd "$(dirname "$RC_VVL_LIB")" && pwd)/$(basename "$RC_VVL_LIB")"
        printf '{"file_format_version": "1.2.0", "layer": {"name": "VK_LAYER_KHRONOS_validation", "type": "GLOBAL", "library_path": "%s", "api_version": "1.4.300", "implementation_version": "1", "description": "Khronos validation layer (manifest written by tools/play.sh)"}}\n' "$lib" > "$RC_GAME_BUILD/vvl/VkLayer_khronos_validation.json"
        export VK_ADD_LAYER_PATH="$RC_GAME_BUILD/vvl${VK_ADD_LAYER_PATH:+:$VK_ADD_LAYER_PATH}"
    fi
fi
cd "$root"
exec env PS2X_DEBUG_PANEL=0 PS2X_GS_GPU="${PS2X_GS_GPU:-1}" PS2X_GS_HW="${PS2X_GS_HW:-1}" ${PS2X_GS_HW_SCALE:+PS2X_GS_HW_SCALE=$PS2X_GS_HW_SCALE} PS2X_GS_THREAD="${PS2X_GS_THREAD:-1}" PS2X_MTVU="${PS2X_MTVU:-1}" PS2X_VU1_TIMING=0 \
    PS2X_EE_HOST_PACING=1 PS2X_GS_SHADER_PRELOAD="${PS2X_GS_SHADER_PRELOAD:-0}" PS2X_GS_SHADER_CACHE=1 PS2X_GS_SHADER_CACHE_DIR="$RC_GAME_BUILD/shader-cache" \
    RC_TRACK_SHADER="${RC_TRACK_SHADER:-1}" RC_VU1_REF="${RC_VU1_REF:-0}" "$input" \
    "${PLAY_BINARY:-$RC_GAME_BUILD/rc_revenge_pro}" "$sandbox/disc/$RC_ELF" > "$sandbox/session.log" 2>&1

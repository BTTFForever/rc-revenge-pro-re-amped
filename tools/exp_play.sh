#!/bin/bash
# exp/quick-dirty (EXPERIMENT.md): tools/play.sh for the experimental build in build-exp (USA) or build-exp-eu.
#   tools/exp_play.sh [play | record <name> | replay <name>]
# Every experiment switch passes through from the environment (RC_WIDESCREEN, RC_HUD_ANCHOR, RC_INTERP, RC_FPS,
# PS2X_HOST_VSYNC, PS2X_HOST_FPS, PS2X_GS_FORCE_LINEAR, ...); with none set this runs exactly as tools/play.sh would,
# so a baseline and an experiment differ only in the variables given.
# Measurement: EXP_TAG=<name> writes <build>/runs/<name>.frames.csv (PS2X_FRAME_TIMES), <name>.dump (RC_DEBUG_DUMP,
# game frames per second) and <name>.log (session.log); tools/exp_fps.py <build>/runs/<name> summarises them.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
source "$root/tools/version.sh"
case "${RC_VERSION:-usa}" in
    usa) RC_GAME_BUILD="$root/build-exp" ;;
    eu) RC_GAME_BUILD="$root/build-exp-eu" ;;
esac
mode="${1:-play}"
[ "$mode" = play ] || name="${2:?recording name}"
pad="$root/decomp/recordings/${name:-}.pad"
case "${name:-}" in */*) pad="$name" ;; esac
sandbox="$RC_GAME_BUILD/play"
rc_make_sandbox "$sandbox"
binary="${EXP_BINARY:-$RC_GAME_BUILD/rc_revenge_pro}" # EXP_BINARY: another build (main's, read only) in this sandbox
[ -x "$binary" ] || { echo "no game build at $binary" >&2; exit 1; }
case "$mode" in
    play) input="PS2X_PAD_REPLAY=" ;;
    record) input="PS2X_PAD_RECORD=$pad" ;;
    replay) [ -f "$pad" ] || { echo "no recording $pad"; exit 1; }; input="PS2X_PAD_REPLAY=$pad" ;;
    *) echo "play, record or replay"; exit 1 ;;
esac
if command -v nvidia-smi >/dev/null && ! nvidia-smi -L >/dev/null 2>&1 && [ "${PLAY_ALLOW_IGPU:-0}" != 1 ]; then
    echo "The NVIDIA GPU is not responding (nvidia-smi failed), so the game was not started." >&2
    exit 1
fi
if nvidia-smi -L >/dev/null 2>&1; then
    export __NV_PRIME_RENDER_OFFLOAD=1 __VK_LAYER_NV_optimus=NVIDIA_only __GLX_VENDOR_LIBRARY_NAME=nvidia
fi
measure=()
log="$sandbox/session.log"
if [ -n "${EXP_TAG:-}" ]; then
    mkdir -p "$RC_GAME_BUILD/runs"
    base="$RC_GAME_BUILD/runs/$EXP_TAG"
    rm -f "$base.frames.csv" "$base.dump" "$base.log"
    measure=(PS2X_FRAME_HASH=1 PS2X_FRAME_TIMES="$base.frames.csv" RC_DEBUG_DUMP="$base.dump" RC_DEBUG_DUMP_MS="${RC_DEBUG_DUMP_MS:-1000}")
    log="$base.log"
fi
# Experiment defaults (each can be overridden, 0 turns it off): the UI pillarbox (only with RC_WIDESCREEN=1),
# borderless fullscreen, the frame-dip log in the session log, and the NVIDIA driver's shader disk cache kept.
export RC_UI_PILLARBOX="${RC_UI_PILLARBOX:-1}" PS2X_FULLSCREEN="${PS2X_FULLSCREEN:-1}" RC_EXP_DIPS="${RC_EXP_DIPS:-1}"
export __GL_SHADER_DISK_CACHE="${__GL_SHADER_DISK_CACHE:-1}" __GL_SHADER_DISK_CACHE_SKIP_CLEANUP=1
cd "$root"
exec env PS2X_DEBUG_PANEL=0 PS2X_GS_GPU="${PS2X_GS_GPU:-1}" PS2X_GS_HW="${PS2X_GS_HW:-1}" PS2X_GS_HW_SCALE="${PS2X_GS_HW_SCALE:-2}" PS2X_GS_THREAD="${PS2X_GS_THREAD:-1}" PS2X_MTVU="${PS2X_MTVU:-1}" PS2X_VU1_TIMING="${PS2X_VU1_TIMING:-0}" \
    PS2X_EE_HOST_PACING=1 PS2X_GS_SHADER_PRELOAD="${PS2X_GS_SHADER_PRELOAD:-0}" PS2X_GS_SHADER_CACHE=1 PS2X_GS_SHADER_CACHE_DIR="$RC_GAME_BUILD/shader-cache" \
    PS2X_BIND_CIRCLE=E PS2X_BIND_SQUARE=R RC_TRACK_SHADER="${RC_TRACK_SHADER:-1}" RC_VU1_REF="${RC_VU1_REF:-2}" "$input" "${measure[@]}" \
    "$binary" "$sandbox/disc/$RC_ELF" > "$log" 2>&1

#!/bin/bash
# Plays the decomp build from a clean memory card, recording or replaying the controller.
#   tools/play.sh record <name>   writes decomp/recordings/<name>.pad
#   tools/play.sh replay <name>   plays it back
# Any real run of a VU1 program not yet compared in the game is checked as it happens; see build-decomp/play/session.log.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
mode="${1:?record or replay}"; name="${2:?recording name}"
pad="$root/decomp/recordings/$name.pad"
sandbox="$root/build-decomp/play"
rm -rf "$sandbox"
mkdir -p "$sandbox/disc/mc0" "$sandbox/disc/mc1"
for entry in "$root/../disc"/*; do
    case "$(basename "$entry")" in mc0|mc1) ;; *) ln -s "$entry" "$sandbox/disc/" ;; esac
done
ln -s "$root/../RC Revenge Pro (USA).iso" "$sandbox/" # The game reads its files through the disc image.
case "$mode" in
    record) input="PS2X_PAD_RECORD=$pad" ;;
    replay) [ -f "$pad" ] || { echo "no recording $pad"; exit 1; }; input="PS2X_PAD_REPLAY=$pad" ;;
    *) echo "record or replay"; exit 1 ;;
esac
cd "$root"
exec env PS2X_DEBUG_PANEL=0 PS2X_GS_GPU=1 PS2X_GS_HW=1 PS2X_GS_HW_SCALE=2 PS2X_GS_THREAD=1 PS2X_MTVU=1 PS2X_VU1_TIMING=0 \
    PS2X_EE_HOST_PACING=1 PS2X_GS_SHADER_PRELOAD=0 PS2X_GS_SHADER_CACHE=1 PS2X_GS_SHADER_CACHE_DIR="$root/build-decomp/shader-cache" \
    PS2X_BIND_CIRCLE=E PS2X_BIND_SQUARE=R RC_TRACK_SHADER=1 RC_VU1_REF=2 "$input" \
    build-decomp/rc_revenge_pro "$sandbox/disc/SLUS_201.53" > "$sandbox/session.log" 2>&1

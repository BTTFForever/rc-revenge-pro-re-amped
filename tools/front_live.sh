#!/bin/bash
# A live front-end walk: the game started in the front end (RC_SKIP_INTRO=1) with a scripted pad (tools/front_walk.py),
# sampled comparisons of the named functions only (RC_SHADOW_LIVE_ONLY), and screenshots at fixed fields.
#   tools/front_live.sh <seed> <seconds> <names,comma,separated> [native|skip]
# native (default): the named functions native and compared. skip: the same walk with them in RC_DECOMP_SKIP and no
# comparisons, for the screenshots to set beside the native ones.
# FRONT_WALK_DEEPER=<0..1> sets how often the walk goes deeper (front_walk.py's CROSS).
# Results in $RC_GAME_BUILD/frontlive/<mode>-<seed>/: walk.pad, game.log, shadow.txt, shots/. RC_VERSION=eu for Europe.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
source "$root/tools/version.sh"
seed="${1:?seed}"; seconds="${2:?seconds}"; names="${3:?names}"; mode="${4:-native}"
out="$RC_GAME_BUILD/frontlive/$mode-$seed"; sandbox="$RC_GAME_BUILD/frontlive/game"
rm -rf "$out"; mkdir -p "$out/shots"
rc_make_sandbox "$sandbox"
python3 "$root/tools/front_walk.py" "$seed" 300 6000 ${FRONT_WALK_DEEPER:-} > "$out/walk.pad"
rc_use_nvidia || exit 1
fields=$(seq -s, 600 600 6000)
if [ "$mode" = skip ]; then
    check=(RC_DECOMP_SKIP="$names")
else
    check=(RC_SHADOW=1 RC_SHADOW_FUZZ=0 RC_SHADOW_LIVE_ONLY="$names" RC_SHADOW_LOG="$out/shadow.txt")
fi
cd "$root"
env PS2X_DEBUG_PANEL=0 PS2X_GS_GPU=1 PS2X_GS_THREAD=1 PS2X_MTVU=1 PS2X_VU1_TIMING=0 PS2X_EE_HOST_PACING=1 \
    PS2X_GS_SHADER_PRELOAD=0 PS2X_GS_SHADER_CACHE=1 PS2X_GS_SHADER_CACHE_DIR="$root/build-decomp/shader-cache" \
    RC_SKIP_INTRO=1 PS2X_PAD_REPLAY="$out/walk.pad" PS2X_SCREENSHOT_DIR="$out/shots" PS2X_SCREENSHOT_FIELDS="$fields" \
    "${check[@]}" timeout "$seconds" "$RC_GAME_BUILD/rc_revenge_pro" "$sandbox/disc/$RC_ELF" > "$out/game.log" 2>&1
echo "$out"
[ -f "$out/shadow.txt" ] && tail -1 "$out/shadow.txt"
grep -a 'MISMATCH' "$out/game.log" | head -12 | cut -c1-210
[ -f "$out/shadow.txt" ] && awk 'NR>1 && $1!="total" && $2>0 {n++; c+=$2} END {print n " functions compared, " c " compares"}' "$out/shadow.txt"
[ -f "$out/shadow.txt" ] && awk 'NR>1 && $1!="total" && $3>0 {print "mismatches:", $5, $3 " of " $2}' "$out/shadow.txt" | head -24
ls "$out/shots" | wc -l | sed 's/$/ screenshots/'

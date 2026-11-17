#!/bin/bash
# Captures the RAM dumps the offline oracle (tools/oracle.sh) runs on: a demo race on each level, dumped at the start
# of a frame some time after the race began, plus the front end if asked for. Dumps are game data: they stay in
# build-oracle/dumps and are not committed.
#   tools/capture_dumps.sh [level|menu ...]     default: 5 17 18 20
# RC_RAMDUMP_FRAME (default 600) is the frame after the level started (after boot for the menu, default 1500).
# Uses build-oracle/rc_revenge_pro (ninja -C build-oracle rc_revenge_pro), so the build-decomp game is left alone.
# RC_VERSION=eu: the Europe version, build-oracle-eu (see tools/version.sh).
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
source "$root/tools/version.sh"
game="${CAPTURE_BINARY:-$RC_ORACLE_BUILD/rc_revenge_pro}"
out="$RC_ORACLE_BUILD/dumps"; sandbox="$RC_ORACLE_BUILD/capture"
[ -x "$game" ] || { echo "no $game: ninja -C $RC_ORACLE_BUILD rc_revenge_pro" >&2; exit 2; }
mkdir -p "$out"
[ $# -gt 0 ] || set -- 5 17 18 20
if nvidia-smi -L >/dev/null 2>&1; then
    export __NV_PRIME_RENDER_OFFLOAD=1 __VK_LAYER_NV_optimus=NVIDIA_only __GLX_VENDOR_LIBRARY_NAME=nvidia
else
    echo "warning: NVIDIA GPU not responding, using the integrated GPU" >&2
fi
status=0
for what in "$@"; do
    if pgrep -f 'rc_revenge_pro ' >/dev/null; then
        echo "a game is already running (pgrep -fa rc_revenge_pro); not starting another" >&2; exit 2
    fi
    rc_make_sandbox "$sandbox"
    if [ "$what" = menu ]; then
        name=menu; level=(); frame="${RC_RAMDUMP_FRAME:-1500}"
    else
        name="level$what"; level=(RC_BOOT_LEVEL="$what"); frame="${RC_RAMDUMP_FRAME:-600}"
    fi
    rm -f "$out/$name.bin"
    (cd "$root" && env PS2X_DEBUG_PANEL=0 PS2X_GS_GPU=1 PS2X_GS_THREAD=1 PS2X_MTVU=1 PS2X_VU1_TIMING=0 PS2X_EE_HOST_PACING=1 \
        PS2X_GS_SHADER_PRELOAD=0 PS2X_GS_SHADER_CACHE=1 PS2X_GS_SHADER_CACHE_DIR="$root/build-decomp/shader-cache" \
        "${level[@]}" RC_RAMDUMP="$out/$name.bin" RC_RAMDUMP_FRAME="$frame" \
        timeout 150 "$game" "$sandbox/disc/$RC_ELF" > "$out/$name.log" 2>&1)
    if [ -s "$out/$name.bin" ]; then
        grep -a 'RAM dump written' "$out/$name.log" | tail -1
    else
        echo "$name: no dump (see $out/$name.log)" >&2; status=1
    fi
done
rm -rf "$sandbox"
exit $status

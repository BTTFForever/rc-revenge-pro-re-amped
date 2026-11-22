#!/bin/bash
# One frame-rate run of P2's section 10 (RENDERER_P2_DESIGN.md 10, 21): the game in a scene with the native renderer
# (RC_RENDERER=vk RC_VK_PICTURE=native RC_VK_READER=fast, the reader on its thread, play.sh's settings and PRIME offload),
# its 5 s frame counts (RC_DEBUG_DUMP), the [vk-native] lines every 300 cycles, each thread's CPU time
# (tools/perf_sample.py) and nvidia-smi every 2 s beside it.
#   tools/perf_scene.sh <scene> <out-dir> <tag>
# scene: front (replay playtest2, 180 s), l5 l17 l18 l21 (RC_BOOT_LEVEL, 100 s), tedit (tedit_walk.py 1, 150 s),
# tedrace (tedit_walk.py 1 0 loop,race: a closed ring built by hand, then its race; 200 s USA, 215 s Europe).
# Writes <out-dir>/<tag>.{log,dbg,threads,gpu,load}. Anything else in the environment is passed on (PS2X_GS_HW=0
# PS2X_GS_GPU=0 for U3's configuration (ii), RC_GS_NO_RASTER=1 with them for the emulated GS not rasterising, RC_VERSION=eu).
# tools/perf_table.py <out-dir> makes the table.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
scene="${1:?scene}"; out="${2:?out dir}"; tag="${3:?tag}"
mkdir -p "$out"
eu=$([ "${RC_VERSION:-usa}" = eu ] && echo eu)
args=(); envs=()
case "$scene" in
    front) secs=${PERF_SECS:-180}; args=(replay playtest2) ;;
    l5|l17|l18|l21) secs=${PERF_SECS:-100}; envs=(RC_BOOT_LEVEL="${scene#l}"); args=(play) ;;
    tedit) secs=150; python3 "$root/tools/tedit_walk.py" 1 7000 $eu > "$out/$tag.pad"; envs=(RC_SKIP_INTRO=1); args=(replay "$out/$tag.pad") ;;
    tedrace) secs=$([ -n "$eu" ] && echo 215 || echo 200)
             python3 "$root/tools/tedit_walk.py" 1 0 $eu loop,race > "$out/$tag.pad"; envs=(RC_SKIP_INTRO=1); args=(replay "$out/$tag.pad") ;;
    *) echo "scene: front l5 l17 l18 l21 tedit tedrace" >&2; exit 2 ;;
esac
cat /proc/loadavg > "$out/$tag.load"
# PERF_RENDERER=gs: the default renderer (the emulated GS's picture) instead of the native one.
[ "${PERF_RENDERER:-vk}" = gs ] || envs+=(RC_RENDERER=vk RC_VK_PICTURE=native RC_VK_READER=fast)
env "${envs[@]}" RC_VK_NATIVE_REPORT="${RC_VK_NATIVE_REPORT:-300}" \
    RC_DEBUG_DUMP="$out/$tag.dbg" timeout -s TERM "$secs" "$root/tools/play.sh" "${args[@]}" &
tpid=$!
sleep 2
gpid=$(pgrep -P "$tpid" | head -1)
nvidia-smi --query-gpu=timestamp,pstate,clocks.gr,utilization.gpu,power.draw --format=csv,noheader -lms 2000 > "$out/$tag.gpu" 2>/dev/null &
npid=$!
[ -n "$gpid" ] && python3 "$root/tools/perf_sample.py" "$gpid" "$out/$tag.threads" 5 &
spid=$!
sleep $((secs / 2)); top -bn1 -w 200 | head -20 >> "$out/$tag.load"
wait "$tpid"
kill "$npid" 2>/dev/null; wait "$spid" 2>/dev/null
cp "$(source "$root/tools/version.sh"; echo "$RC_GAME_BUILD")/play/session.log" "$out/$tag.log"
cat /proc/loadavg >> "$out/$tag.load"
echo "$out/$tag: $(python3 "$root/tools/fps.py" "$out/$tag.dbg")"

#!/bin/bash
# Profiles one live game session: per-thread CPU (tools/thread_cpu.py), a perf profile and nvidia-smi samples.
#   tools/perf_session.sh <out dir> <warm-up s> <measure s> [play.sh args...]
# Settings pass through the environment as for tools/play.sh (RC_RENDERER, RC_VK_PICTURE, RC_BOOT_LEVEL, ...).
# Writes <out>/threads.txt, <out>/perf.data (task-clock:u, flat), <out>/perf_threads.txt (hottest
# symbols per thread), <out>/gpu.csv and the game's own log and RC_DEBUG_DUMP (fps: tools/fps.py <out>/debug.dbg).
# The game is stopped at the end (SIGTERM to its PID; never pkill -f).
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
source "$root/tools/version.sh"
out="${1:?out dir}"; warm="${2:?warm-up seconds}"; measure="${3:?measure seconds}"; shift 3
mkdir -p "$out"
out="$(cd "$out" && pwd)"
RC_DEBUG_DUMP="$out/debug.dbg" "$root/tools/play.sh" "$@" &
play=$!
# play.sh execs the game, so its PID is the game's.
sleep "$warm"
if ! kill -0 "$play" 2>/dev/null; then echo "game exited early" >&2; exit 1; fi
nvidia-smi --query-gpu=timestamp,utilization.gpu,clocks.gr,power.draw --format=csv -lms 500 > "$out/gpu.csv" 2>/dev/null &
smi=$!
python3 -I "$root/tools/thread_cpu.py" "$play" "$measure" > "$out/threads.txt" &
tc=$!
perf record -e task-clock:u -F 499 -p "$play" -o "$out/perf.data" -- sleep "$measure" 2> "$out/perf_record.log"
wait "$tc"
kill "$smi" 2>/dev/null
kill "$play" 2>/dev/null
wait "$play" 2>/dev/null
perf report -i "$out/perf.data" --no-children --sort comm,dso,symbol --stdio 2>/dev/null | grep -v '^#' | grep -v '^$' \
    | grep -E '^ +[0-9]' | head -150 > "$out/perf_threads.txt"
cp "$RC_GAME_BUILD/play/session.log" "$out/session.log" 2>/dev/null
cat "$out/threads.txt"

#!/bin/bash
# Whole-lap frame-rate sweep: each level's attract demo (RC_BOOT_LEVEL, lengthened by RC_DEMO_SECS) at 3x, with every
# frame's time (PS2X_FRAME_TIMES), the [gs-hw] stats and each thread's CPU time every second.
#   tools/track_sweep.sh <out-dir> <first> <last> [VAR=value ...]     (SWEEP_TAG=usa, SWEEP_SECS=150, RC_VERSION=eu)
# Writes <out-dir>/<tag>.l<level>.{frames.csv,log,threads}; tools/track_sweep_table.py <out-dir> [tag] makes the table.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
out="$(mkdir -p "$1" && cd "$1" && pwd)"; first=$2; last=$3; shift 3
cd "$root"; source tools/version.sh
for level in $(seq "$first" "$last"); do
  tag=${SWEEP_TAG:-usa}.l$level
  env RC_BOOT_LEVEL=$level RC_DEMO_SECS=130 PS2X_GS_HW_SCALE=3 RC_VU1_REF=0 GOW_GS_HW_STATS=1 PS2X_FRAME_TIMES=$out/$tag.frames.csv "$@" \
    timeout -s TERM "${SWEEP_SECS:-150}" tools/play.sh > /dev/null 2>&1 &
  tpid=$!
  sleep 2
  gpid=$(pgrep -P "$tpid" | head -1)
  [ -n "$gpid" ] && python3 tools/perf_sample.py "$gpid" "$out/$tag.threads" 1 &
  spid=$!
  wait "$tpid"; wait "$spid" 2>/dev/null
  cp "$RC_GAME_BUILD/play/session.log" "$out/$tag.log"
  echo "done $tag"
done

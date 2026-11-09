#!/bin/bash
# Runs each level through RC_BOOT_LEVEL and prints its frame rate.
# Usage: tools/level_sweep.sh <first> <last> <seconds> [VAR=value ...]
cd "$(dirname "$0")/.."
first=$1; last=$2; secs=$3; shift 3
dump=$(mktemp)
for level in $(seq "$first" "$last"); do
  rm -f "$dump"
  env "$@" RC_BOOT_LEVEL="$level" RC_DEBUG_DUMP="$dump" PS2X_AUDIO_STATS=1 timeout "$secs" ./run.sh > "$dump.log" 2>&1
  fps=$(python3 -I tools/fps.py "$dump" | tail -1 | sed 's/fps per 5s: //')
  audio=$(grep '^\[audio' "$dump.log" | tail -1 | awk '{print $2}')
  echo "level $level: $fps | audio $audio"
done
rm -f "$dump" "$dump.log"

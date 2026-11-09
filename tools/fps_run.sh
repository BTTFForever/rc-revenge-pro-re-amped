#!/bin/bash
# usage: fps_run.sh <label> <seconds> VAR=value ...; run from the fork directory.
label=$1; seconds=$2; shift 2
env RC_DEBUG_DUMP="$PWD/dump_$label.txt" PS2X_DEBUG_PANEL=0 PS2X_GS_SHADER_CACHE=1 PS2X_GS_SHADER_CACHE_DIR="$PWD/gs_shaders" PS2X_GS_THREAD=1 "$@" \
    timeout "$seconds" build/rc_revenge_pro ../disc/SLUS_201.53 > "run_$label.log" 2>&1
echo "== $label ($*)"
python3 -I tools/fps.py "dump_$label.txt"
grep -E '^\[gs-gl\]' "run_$label.log" | head -1 | cut -c1-80

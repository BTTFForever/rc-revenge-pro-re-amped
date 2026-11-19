#!/bin/bash
# Records a frame record dump in exact mode (RENDERER_FRAME_RECORD.md 3.5; NOTES.md "Frame record"): tools/play.sh with
# the synchronous software GS, so the dump carries the emulated GS's picture per display cycle, its register file
# before every kick, a hash of every kick's GIF output and exact GS memory at keyframes, for `rc_replay --consumer emu`.
#   RC_FRAME_RECORD=<file.rcfr> [RC_FRAME_RECORD_AT=<trigger>] [RC_FRAME_RECORD_REF=<n>|hash|off] tools/frame_record.sh [play.sh arguments]
# RC_VERSION=eu records the Europe version. The game runs slower than real time while it renders in software; pad
# replays are keyed by game frame and stay aligned. RC_FRAME_RECORD_STRICT=1 (the default here) stops the run if exact
# mode is not possible.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
: "${RC_FRAME_RECORD:?set RC_FRAME_RECORD=<file.rcfr>}"
export PS2X_GS_GPU=0 PS2X_GS_HW=0 PS2X_GS_THREAD=0 PS2X_MTVU=0 RC_TRACK_SHADER=0 RC_VU1_REF=0
export RC_FRAME_RECORD_STRICT="${RC_FRAME_RECORD_STRICT:-1}"
exec "$root/tools/play.sh" "$@"

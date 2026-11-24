#!/bin/bash
# The live check of renderer P1 step 8 batch 4 (RENDERER_3D_DESIGN.md sections 12 and 20; NOTES.md "Live 3D"): a session
# with the native renderer live (RC_RENDERER=vk RC_VK_PICTURE=native) in exact mode, recorded from boot with the live
# reader's draw lists (RC_FRAME_RECORD_DRAWLISTS=1), then
#   rc_replay <dump> --consumer dlst          the lists the reader makes off-line from the recorded kicks = the live ones
#   rc_replay <dump> --consumer vk --verify   the chained replay of the dump
# Usage: tools/live_check.sh <name> <cycles> [play.sh arguments]   e.g.  RC_BOOT_LEVEL=17 tools/live_check.sh l17 700
# RC_VERSION=eu for Europe; other variables pass through (RC_SKIP_INTRO, RC_BOOT_LEVEL, ...). LIVE_REF=<n> stores every
# n-th reference picture (default 10), LIVE_SHOTS=<cycle,...> takes native and emulated screenshots (RC_VK_SHOT_*),
# LIVE_SECONDS caps the session (default 1800), LIVE_NO_REPLAY=1 records only. The game is stopped once the dump is
# written and the last screenshot taken. Results in $RC_GAME_BUILD/frame-dumps/live/: <name>.rcfr, <name>.session.log,
# <name>.dlst.txt, <name>.vk.txt, shots in <name>-shots/.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
source "$root/tools/version.sh"
name="${1:?name}"; cycles="${2:?cycles}"; shift 2
out="$RC_GAME_BUILD/frame-dumps/live"; mkdir -p "$out"
dump="$out/$name.rcfr"; shots="$out/$name-shots"; log="$RC_GAME_BUILD/play/session.log"
rm -rf "$dump" "$shots"; mkdir -p "$shots"
want_shots=0
[ -n "${LIVE_SHOTS:-}" ] && want_shots=$(tr ',' '\n' <<< "$LIVE_SHOTS" | grep -c .)
cd "$root"
rm -f "$log"
start=$(date +%s)
RC_RENDERER=vk RC_VK_PICTURE=native RC_FRAME_RECORD="$dump" RC_FRAME_RECORD_AT="boot+$cycles" RC_FRAME_RECORD_DRAWLISTS=1 \
    RC_FRAME_RECORD_REF="${LIVE_REF:-10}" RC_VK_SHOT_DIR="$shots" RC_VK_SHOT_AT="${LIVE_SHOTS:-}" RC_VK_NATIVE_REPORT="${RC_VK_NATIVE_REPORT:-100}" \
    tools/frame_record.sh "$@" &
pid=$!
deadline=$((start + ${LIVE_SECONDS:-1800}))
# Wait on the game's PID (play.sh execs it) for the dump and the last screenshot.
while kill -0 "$pid" 2>/dev/null && [ "$(date +%s)" -lt "$deadline" ]; do
    if grep -aq "^\[frame-record\] wrote" "$log" 2>/dev/null; then
        [ "$(grep -ac 'screenshot: emulated GS picture' "$log")" -ge "$want_shots" ] && break
    fi
    sleep 3
done
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
cp "$log" "$out/$name.session.log"
echo "$name: session $(( $(date +%s) - start )) s"
grep -a "^\[frame-record\] wrote\|draw lists (DLST)" "$out/$name.session.log" | cut -c1-200
[ -f "$dump" ] || { echo "no dump"; exit 2; }
[ "${LIVE_NO_REPLAY:-0}" = 1 ] && exit 0
rc_use_nvidia || exit 1
rr="${LIVE_RC_REPLAY:-$RC_GAME_BUILD/rc_replay}"
"$rr" "$dump" --consumer dlst --quiet > "$out/$name.dlst.txt" 2>&1; d=$?
"$rr" "$dump" --consumer vk --verify --quiet > "$out/$name.vk.txt" 2>&1; v=$?
echo "dlst: $(grep -a '^dlst: [0-9]' "$out/$name.dlst.txt" | cut -c1-200) -> $(tail -1 "$out/$name.dlst.txt") (exit $d)"
echo "vk: $(grep -a '^vk: Gs2dVk' "$out/$name.vk.txt" | cut -c1-160) -> $(tail -1 "$out/$name.vk.txt") (exit $v)"
[ $d = 0 ] && [ $v = 0 ]

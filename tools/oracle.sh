#!/bin/bash
# The offline oracle: compares decompiled functions with their recompiled originals on random inputs, offline, from
# RAM dumps of the game (tools/capture_dumps.sh). Deterministic; no game window, threads or timers.
#   tools/oracle.sh --only a,b,c [--level N]... [--cases N] [--seed S] [--jobs J]
#   tools/oracle.sh --all [--level N]... [--cases N]     the reference suite: every function with a random setup
# Dumps: build-oracle/dumps/level<N>.bin (default: every level dump there; menu.bin and tedit.bin, the Track Editor,
# only if named with --level menu or --level tedit).
# RC_VERSION=eu runs the Europe version's oracle (build-oracle-eu, its dumps and executable; see tools/version.sh).
# Prints one line per function and a summary; functions in tools/oracle_known.txt show as "known" and do not fail. Exit 0 if nothing failed, 1 if something did, 2 on a usage error.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
source "$root/tools/version.sh"
oracle="$RC_ORACLE_BUILD/rc_oracle"
dumps=(); only=""; all=0; cases=""; args=()
while [ $# -gt 0 ]; do
    case "$1" in
        --level) dumps+=("$RC_ORACLE_BUILD/dumps/$(case "$2" in menu|tedit) echo "$2" ;; *) echo "level$2" ;; esac).bin"); shift 2 ;;
        --only) only="$2"; shift 2 ;;
        --all) all=1; shift ;;
        --cases) cases="$2"; shift 2 ;;
        --seed|--jobs|--timeout) args+=("$1" "$2"); shift 2 ;;
        --verbose) args+=("$1"); shift ;;
        *) sed -n '2,8p' "$0" >&2; exit 2 ;;
    esac
done
if [ -z "$only" ] && [ $all = 0 ]; then sed -n '2,8p' "$0" >&2; exit 2; fi
[ -x "$oracle" ] || { echo "no $oracle: ninja -C $RC_ORACLE_BUILD rc_oracle" >&2; exit 2; }
if [ ${#dumps[@]} = 0 ]; then
    for dump in "$RC_ORACLE_BUILD"/dumps/level*.bin; do [ -e "$dump" ] && dumps+=("$dump"); done
fi
[ ${#dumps[@]} -gt 0 ] || { echo "no RAM dumps: tools/capture_dumps.sh" >&2; exit 2; }
dumpArgs=()
for dump in "${dumps[@]}"; do
    [ -e "$dump" ] || { echo "no $dump: tools/capture_dumps.sh" >&2; exit 2; }
    dumpArgs+=(--dump "$dump")
done
[ -n "$only" ] && args+=(--only "$only")
args+=(--cases "${cases:-$( [ $all = 1 ] && echo 20 || echo 300 )}")
export RC_ISO # the disc image of the version, which the loader setups read (res_fuzz.cpp)
cd "$root" && exec "$oracle" --elf "$RC_DISC/$RC_ELF" --known "$root/tools/oracle_known.txt" "${dumpArgs[@]}" "${args[@]}"

#!/bin/bash
# usage: measure.sh <label> <seconds> [VAR=value ...]; run from the fork directory.
label=$1; seconds=$2; shift 2
env RC_DEBUG_DUMP="$PWD/dump_$label.txt" "$@" timeout "$seconds" build/rc_revenge_pro ../disc/SLUS_201.53 > "run_$label.log" 2>&1
code=$?
echo "== $label exit=$code"
grep -E 'gs-gl|gs-hw|gs-present|GL unavailable|OpenGL no|EGL' "run_$label.log" | cut -c1-120 | head -6
grep -iE 'error|exception|unimplemented' "run_$label.log" | grep -v 'INFO' | cut -c1-120 | sort | uniq -c | head -4
python3 -I - "dump_$label.txt" <<'P'
import re,sys
t=open(sys.argv[1]).read().split('=== t=')[1:]
rows=[]
for b in t:
    m=re.match(r'(\d+)s .*eeCycle=(\d+)',b); f=re.search(r'^frame (.*)$',b,re.M)
    th=re.search(r'^thread 1 status=(\d) wait=(\d)',b,re.M)
    rows.append((int(m[1]),int(m[2]),('st'+th[1]+' '+(f[1] if f else 'none')) if th else 'none'))
for (a,c0,_),(b,c1,f) in list(zip(rows,rows[1:]))[-5:]:
    print(f't={b}s vblank/s={(c1-c0)/4915200/(b-a):6.1f}  {f}'[:96])
P

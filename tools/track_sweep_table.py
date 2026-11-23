#!/usr/bin/env python3
"""Table of a tools/track_sweep.sh run: per level the median and worst 1 s frame rate (after the first 15 s),
seconds under 55 fps, frames over 50 ms, peak GS primitives a frame and texture decodes per 5 s.

Usage: track_sweep_table.py <out-dir> [tag]"""
import csv,sys,re,glob,os,statistics
d=sys.argv[1]; tag=sys.argv[2] if len(sys.argv)>2 else 'usa'
print(f"{'lvl':>3} {'med':>4} {'worst1s':>7} {'<55s':>4} {'hitch>50ms':>10} {'maxPrim/f':>9} {'maxTexDec/5s':>12} {'worstAt':>8}")
for f in sorted(glob.glob(f"{d}/{tag}.l*.frames.csv"),key=lambda p:int(re.search(r'\.l(\d+)\.',p).group(1))):
    lv=int(re.search(r'\.l(\d+)\.',f).group(1))
    rows=[r for r in csv.DictReader(open(f)) if r.get('interval_ms')]; b={}
    for r in rows:
        s=int(float(r['seconds'])); b[s]=b.get(s,0)+1
    secs=[s for s in sorted(b) if s>=15 and s<max(b)]  # skip load
    v=[b[s] for s in secs] or [0]
    hitch=sum(1 for r in rows if float(r['seconds'])>15 and float(r['interval_ms'])>50)
    lp=f.replace('.frames.csv','.log'); lg=open(lp,errors='ignore').read() if os.path.exists(lp) else ''
    dr=[int(x) for x in re.findall(r'draws=(\d+)',lg)]; td=[int(x) for x in re.findall(r'texDecodes=(\d+)',lg)]
    worst=min(secs,key=lambda s:b[s]) if secs else 0
    print(f"{lv:>3} {statistics.median(v):>4.0f} {min(v):>7} {sum(1 for x in v if x<55):>4} {hitch:>10} {max(dr or [0])//300:>9} {max(td or [0]):>12} {worst:>7}s")

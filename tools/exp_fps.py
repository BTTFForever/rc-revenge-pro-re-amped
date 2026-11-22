#!/usr/bin/env python3
"""exp/quick-dirty (EXPERIMENT.md): summarises a tools/exp_play.sh run (EXP_TAG=<name>).

Usage: exp_fps.py <build>/runs/<name> [--skip S] [--until S]
  game ticks/s   from <name>.dump (gameframes: GfxP2_SwapBuffers calls, one per game tick)
  pictures/s     from <name>.frames.csv: host presents that showed a new picture (a changed image hash; without
                 hashes, a new source sequence, which also counts repeats), and the spread of the intervals between new pictures
S: seconds of the run to leave out at the start (default 0) / to stop at."""
import csv, re, statistics, sys

base = sys.argv[1]
skip = float(sys.argv[sys.argv.index('--skip') + 1]) if '--skip' in sys.argv else 0.0
until = float(sys.argv[sys.argv.index('--until') + 1]) if '--until' in sys.argv else 1e9

try:
    dump = open(base + '.dump').read()
    counts = [int(x) for x in re.findall(r'^gameframes (\d+)', dump, re.M)]
    ms = 1000
    rates = [(b - a) * 1000 / ms for a, b in zip(counts, counts[1:])]
    rates = rates[int(skip):int(min(until, len(rates)))]
    if rates:
        print(f'game ticks/s: mean {statistics.mean(rates):.1f}  min {min(rates):.0f}  max {max(rates):.0f}  ({len(rates)} s)')
except FileNotFoundError:
    print('no .dump')

try:
    rows = list(csv.DictReader(open(base + '.frames.csv')))
except FileNotFoundError:
    rows = []
times = []
last_src = None
for r in rows:
    t = float(r['seconds'])
    if t < skip or t > until:
        continue
    # A changed image hash when hashing is on (PS2X_FRAME_HASH=1: exp_play.sh sets it with EXP_TAG), else a new source.
    new = r['image_changed'] == '1' if r['hash_valid'] == '1' else r['new_source'] == '1'
    if new:
        times.append(t)
if len(times) > 2:
    span = times[-1] - times[0]
    gaps = [(b - a) * 1000 for a, b in zip(times, times[1:])]
    gaps.sort()
    pct = lambda p: gaps[min(len(gaps) - 1, int(p * len(gaps)))]
    print(f'pictures/s: {(len(times) - 1) / span:.1f} over {span:.1f} s; interval ms p50 {pct(0.5):.1f} '
          f'p90 {pct(0.9):.1f} p99 {pct(0.99):.1f} max {gaps[-1]:.1f}')
    print(f'host loop presents/s: {len([r for r in rows if skip <= float(r["seconds"]) <= until]) / span:.1f}')
else:
    print('no new pictures in frames.csv')
last = None
try:
    for line in open(base + '.log', errors='replace'):
        if line.startswith('[exp]'):
            last = line.strip()
except FileNotFoundError:
    pass
if last:
    print('last exp line:', last)

#!/usr/bin/env python3
"""perf_table.py DIR [TAG...]: the table of tools/perf_scene.sh's runs in DIR (every <tag>.dbg, or the tags named).

For each run, over the window after the load (the 5 s samples after the first 15 s; for tedrace those that start 600
frames after the pad's last press, i.e. in the race): game frames a second (median, worst 5 s sample, samples), the
[vk-native] lines of that window (8.3's waits, the reader thread's ms a cycle, the EE thread's share, K2 a VIF1 kick and
the whole list on the GPU a frame from the presenter's timestamps), nvidia-smi's clock (median, P-state, utilisation),
each named thread's CPU (cores) and the cores everything else on the machine used ("other": a run with more than 0.25
is marked disturbed).  --json writes the rows as JSON lines too."""
import glob, json, os, re, statistics, sys

d = sys.argv[1]
tags = [a for a in sys.argv[2:] if not a.startswith('--')] or sorted(
    os.path.basename(p)[:-4] for p in glob.glob(os.path.join(d, '*.dbg')))
GROUPS = [('EE', 'GameThread'), ('VU1', 'VUThread'), ('GIF', 'GIFThread'), ('GS', 'GSThread'), ('bands', 'GSBand'),
          ('reader', 'rc-reader'), ('main', 'rc_revenge_pro')]


def num(pattern, text, cast=float):
    m = re.search(pattern, text)
    return cast(m.group(1)) if m else None


def row(tag):
    p = os.path.join(d, tag)
    dbg = open(p + '.dbg').read()
    counts = [int(x) for x in re.findall(r'^gameframes (\d+)', dbg, re.M)]
    rates = [(b - a) / 5 for a, b in zip(counts, counts[1:])]
    first = 3
    if 'tedrace' in tag and os.path.exists(p + '.pad'):
        last = int(open(p + '.pad').read().split('\n')[-2].split()[1])
        first = next((i for i, c in enumerate(counts) if c >= last + 600), len(counts))
    win = rates[first:]
    lo_frame, hi_frame = (counts[first], counts[-1]) if first < len(counts) else (0, 0)
    t0, t1 = 5 * (first + 1), 5 * len(counts)  # seconds since start of the window's ends
    r = {'tag': tag, 'fps': statistics.median(win) if win else None, 'worst': min(win) if win else None, 'n': len(win)}
    log = open(p + '.log', errors='replace').read() if os.path.exists(p + '.log') else ''
    rt, ee, waits, wms, k2, gpu = [], [], 0, 0.0, [], []
    for m in re.finditer(r'\[vk-native\] reader thread, cycles (\d+)-(\d+): the EE thread ([\d.]+) ms.*?the reader thread '
                         r'([\d.]+) ms a cycle.*?; (\d+) waits \(([\d.]+) ms', log):
        if int(m.group(1)) >= lo_frame and int(m.group(2)) <= hi_frame:
            ee.append(float(m.group(3))); rt.append(float(m.group(4))); waits += int(m.group(5)); wms += float(m.group(6))
    for m in re.finditer(r'\[vk-native\] presenter cycles (\d+)-(\d+): .*?GPU ([\d.]+) ms a frame.*?K2 ([\d.]+) ms a VIF1 '
                         r'kick \(largest ([\d.]+)', log):
        if int(m.group(1)) >= lo_frame and int(m.group(2)) <= hi_frame:
            gpu.append(float(m.group(3))); k2.append((float(m.group(4)), float(m.group(5))))
    mean = lambda v: sum(v) / len(v) if v else None
    r.update(reader=mean(rt), ee_share=mean(ee), waits=waits, wait_ms=wms, k2=mean([a for a, _ in k2]),
             k2_max=max([b for _, b in k2], default=None), gpu_frame=mean(gpu), lines=len(rt))
    if os.path.exists(p + '.gpu'):
        g = [l.split(', ') for l in open(p + '.gpu') if l.count(',') >= 4]
        g = g[max(0, (t0 - 2) // 2):max(0, (t1 - 2) // 2)] or g
        clocks = [int(x[2].split()[0]) for x in g]
        states = [x[1] for x in g]
        r.update(clock=statistics.median(clocks) if clocks else None,
                 pstate=max(set(states), key=states.count) if states else None,
                 util=mean([int(x[3].split()[0]) for x in g]))
    if os.path.exists(p + '.threads'):
        s = [json.loads(l) for l in open(p + '.threads')]
        if s:
            base = s[0]['t'] - 2.0
            w = [x for x in s if t0 <= x['t'] - base <= t1] or s
            a, b = w[0], w[-1]
            dt = (b['t'] - a['t']) or 1
            hz = os.sysconf('SC_CLK_TCK')
            cores = {}
            for tid, (name, v) in b['threads'].items():
                v0 = a['threads'].get(tid, [name, v])[1]
                key = next((k for k, pre in GROUPS if name.startswith(pre)), 'rest')
                cores[key] = cores.get(key, 0) + (v - v0) / hz / dt
            r['threads'] = cores
            r['proc'] = (b['proc'] - a['proc']) / hz / dt
            r['rss'] = max(x.get('rss_mb', 0) for x in s); r['swap'] = max(x.get('swap_mb', 0) for x in s)
            r['other'] = ((b['busy'] - a['busy']) - (b['proc'] - a['proc'])) / hz / dt
    return r


rows = [row(t) for t in tags]
f = lambda v, n=1: '-' if v is None else f'{v:.{n}f}'
print('| run | fps median (worst 5 s; n) | waits (ms) | reader ms | EE share ms | K2 ms a kick (largest) | list GPU ms a frame '
      '| clock MHz (P, util %) | cores: EE / VU1 / GIF / GS / bands / reader / main / rest = process | other | RSS / swap MB |')
print('|---|---|---|---|---|---|---|---|---|---|---|')
for r in rows:
    t = r.get('threads', {})
    th = ' / '.join(f(t.get(k), 2) for k, _ in GROUPS) + ' / ' + f(t.get('rest'), 2) + ' = ' + f(r.get('proc'), 2)
    other = f(r.get('other'), 2) + (' **disturbed**' if (r.get('other') or 0) > 0.25 else '')
    print(f"| {r['tag']} | {f(r['fps'])} ({f(r['worst'])}; {r['n']}) | {r['waits']} ({f(r['wait_ms'], 0)}) | {f(r['reader'], 2)} | "
          f"{f(r['ee_share'], 2)} | {f(r['k2'], 2)} ({f(r['k2_max'], 2)}) | {f(r['gpu_frame'], 2)} | "
          f"{f(r.get('clock'), 0)} ({r.get('pstate', '-')}, {f(r.get('util'), 0)}) | {th} | {other} | {r.get('rss', '-')} / {r.get('swap', '-')} |")
if '--json' in sys.argv:
    with open(os.path.join(d, 'table.jsonl'), 'w') as o:
        for r in rows:
            o.write(json.dumps(r) + '\n')

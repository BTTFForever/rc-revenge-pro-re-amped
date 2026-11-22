#!/usr/bin/env python3
"""perf_sample.py PID OUT [SECONDS]: samples a running game's threads every SECONDS (default 5) until it exits, for
tools/perf_scene.sh. Each line of OUT is JSON: the time (monotonic s), the machine's CPU ticks (busy, total, from
/proc/stat), the process's own ticks, and every thread's {tid: [name, utime + stime ticks]} (/proc/PID/task/*/stat).
tools/perf_table.py turns two samples into each thread's share of a core over a window and the CPU used by everything
else on the machine (to discard runs other work disturbed)."""
import json, os, sys, time

pid, out = int(sys.argv[1]), sys.argv[2]
every = float(sys.argv[3]) if len(sys.argv) > 3 else 5.0


def stat_fields(path):
    text = open(path).read()
    name = text[text.index('(') + 1:text.rindex(')')]
    rest = text[text.rindex(')') + 2:].split()
    return name, int(rest[11]) + int(rest[12])  # utime, stime (fields 14 and 15)


with open(out, 'w') as f:
    while os.path.exists(f'/proc/{pid}'):
        cpu = [int(x) for x in open('/proc/stat').readline().split()[1:]]
        idle = cpu[3] + cpu[4]
        threads = {}
        try:
            _, proc = stat_fields(f'/proc/{pid}/stat')
            for tid in os.listdir(f'/proc/{pid}/task'):
                try:
                    threads[tid] = stat_fields(f'/proc/{pid}/task/{tid}/stat')
                except (FileNotFoundError, ProcessLookupError, ValueError):
                    pass
        except (FileNotFoundError, ProcessLookupError):
            break
        mem = {l.split(':')[0]: int(l.split()[1]) for l in open(f'/proc/{pid}/status') if l.startswith(('VmRSS', 'VmSwap'))}
        f.write(json.dumps({'t': time.monotonic(), 'busy': sum(cpu) - idle, 'total': sum(cpu), 'proc': proc,
                            'rss_mb': mem.get('VmRSS', 0) // 1024, 'swap_mb': mem.get('VmSwap', 0) // 1024,
                            'threads': threads}) + '\n')
        f.flush()
        time.sleep(every)

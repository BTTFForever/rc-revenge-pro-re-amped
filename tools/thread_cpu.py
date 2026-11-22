#!/usr/bin/env python3
"""Per-thread CPU use of a running process, from /proc (no perf needed).

    tools/thread_cpu.py <pid> [seconds=10] [interval=1]

Samples utime+stime of every thread in /proc/<pid>/task each interval and prints, at the end, each thread's
average share of one core over the whole window (name, tid, %), busiest first, plus the process total.
Threads that appear or vanish during the window are counted for the time they were seen.
"""
import os
import sys
import time

HZ = os.sysconf("SC_CLK_TCK")


def sample(pid):
    out = {}
    base = f"/proc/{pid}/task"
    for tid in os.listdir(base):
        try:
            with open(f"{base}/{tid}/stat") as f:
                s = f.read()
            with open(f"{base}/{tid}/comm") as f:
                name = f.read().strip()
        except OSError:
            continue
        rest = s[s.rfind(")") + 2:].split()
        out[int(tid)] = (name, int(rest[11]) + int(rest[12]))  # utime, stime
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    pid = int(sys.argv[1])
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0
    interval = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0
    first = sample(pid)
    t0 = time.monotonic()
    seen = dict(first)
    last = first
    while time.monotonic() - t0 < seconds:
        time.sleep(interval)
        try:
            last = sample(pid)
        except FileNotFoundError:
            break
        for tid, v in last.items():
            seen.setdefault(tid, (v[0], v[1]))
    elapsed = time.monotonic() - t0
    rows = []
    for tid, (name, end) in last.items():
        start = first.get(tid, seen.get(tid))[1]
        rows.append((100.0 * (end - start) / HZ / elapsed, name, tid))
    rows.sort(reverse=True)
    total = sum(r[0] for r in rows)
    print(f"pid {pid}: {elapsed:.1f} s, total {total:.1f}% of one core")
    for pct, name, tid in rows:
        if pct >= 0.5:
            print(f"  {pct:6.1f}%  {name:<16} {tid}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

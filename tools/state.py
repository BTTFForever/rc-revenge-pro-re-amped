#!/usr/bin/env python3
"""Prints the last main-thread states and frames from an RC_DEBUG_DUMP file."""
import bisect, re, sys
syms = [(int(a, 16), int(b), c) for a, b, c in (l.split() for l in open(sys.argv[1]))]
keys = [x[0] for x in syms]
text = open(sys.argv[2]).read()
blocks = text.split('=== t=')[1:]
for b in blocks[-3:]:
    t = re.match(r'(\d+)s', b)[1]
    m = re.search(r'^thread 1 status=(\d) wait=(\d).*?pc=([0-9a-f]+) ra=([0-9a-f]+).*\n  stack:(.*)$', b, re.M)
    f = re.search(r'^frame (.*)$', b, re.M)
    names = []
    if m:
        for a in [m[3], m[4]] + m[5].split()[:4]:
            v = int(a, 16); names.append(syms[bisect.bisect_right(keys, v) - 1][2])
    print(f't={t} st={m[1] if m else "?"} w={m[2] if m else "?"} ' + ' < '.join(names)[:90])
    print('   ', f[1][:80] if f else 'frame none')

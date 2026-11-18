#!/usr/bin/env python3
"""Prints game frames per second over the samples of an RC_DEBUG_DUMP file.

Usage: fps.py DUMP [--ms N]   (N = RC_DEBUG_DUMP_MS, default 5000; with --ms every sample is printed)"""
import re, sys
ms = int(sys.argv[sys.argv.index('--ms') + 1]) if '--ms' in sys.argv else 5000
counts = [int(x) for x in re.findall(r'^gameframes (\d+)', open(sys.argv[1]).read(), re.M)]
rates = [(b - a) * 1000 / ms for a, b in zip(counts, counts[1:])]
if '--ms' in sys.argv:
    print(' '.join(f'{r:.0f}' for r in rates))
else:
    print('fps per 5s:', ' '.join(f'{r:.1f}' for r in rates)[-110:])

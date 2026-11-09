#!/usr/bin/env python3
"""Prints game frames per second over the last samples of an RC_DEBUG_DUMP file."""
import re, sys
counts = [int(x) for x in re.findall(r'^gameframes (\d+)', open(sys.argv[1]).read(), re.M)]
print('fps per 5s:', ' '.join(f'{(b - a) / 5:.1f}' for a, b in zip(counts, counts[1:]))[-110:])

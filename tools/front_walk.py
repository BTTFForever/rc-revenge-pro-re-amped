#!/usr/bin/env python3
"""front_walk.py SEED START FRAMES > file.pad: a scripted front-end walk for PS2X_PAD_REPLAY (epoch 0, use with RC_SKIP_INTRO=1): into a menu, around it, deeper,
back out, now and then a long wait on a page."""
import random, sys
seed, start, frames = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
r = random.Random(seed)
UP, RIGHT, DOWN, LEFT, TRI, CIR, CROSS, SQ, START = 0x10, 0x20, 0x40, 0x80, 0x1000, 0x2000, 0x4000, 0x8000, 0x8
f = start
out = []
def press(b, hold=6, gap=16):
    global f
    out.append((f, b)); f += hold
    out.append((f, 0)); f += gap
# Past the title (and, for Europe, the language menu: first language).
for _ in range(3):
    press(START, 6, 40); press(CROSS, 6, 60)
while f < start + frames:
    kind = r.random()
    if kind < 0.55:
        for _ in range(r.randint(0, 4)):
            press(r.choice([UP, DOWN, DOWN, LEFT, RIGHT]), 6, r.randint(10, 30))
        press(CROSS, 6, r.randint(30, 90))
    elif kind < 0.8:
        press(TRI, 6, r.randint(30, 80))
    elif kind < 0.9:
        f += r.randint(60, 240)   # watch a page (the turntable turning, the credits)
    else:
        for _ in range(r.randint(2, 6)):
            press(r.choice([LEFT, RIGHT]), 6, 20)
for frame, b in out:
    print(f"0 {frame} {b:04x} 7f 7f 7f 7f")

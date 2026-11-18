#!/usr/bin/env python3
"""tedit_walk.py SEED FRAMES [eu] > file.pad: a scripted Track Editor session for PS2X_PAD_REPLAY (epoch 0, use with
RC_SKIP_INTRO=1). From the front end into the Track Editor (Main Menu, three down, Track Editor, OK), the start grid
turned and placed, then a random edit: pieces chosen (arrows) and added (cross), deleted (circle), the view turned
(L2, R2), the 2D map held (L1), the far view and back (R1), the editor's menu opened and closed (start), now and then
a wait. Europe (eu) first picks the language (the first). The editor's keys are the pad's bits: InputHit(k) is bit k."""
import random, sys
seed, frames = int(sys.argv[1]), int(sys.argv[2])
eu = len(sys.argv) > 3 and sys.argv[3] == 'eu'
r = random.Random(seed)
SELECT, START, UP, RIGHT, DOWN, LEFT = 0x1, 0x8, 0x10, 0x20, 0x40, 0x80
L2, R2, L1, R1, TRI, CIR, CROSS, SQ = 0x100, 0x200, 0x400, 0x800, 0x1000, 0x2000, 0x4000, 0x8000
out = []
f = 300
def press(b, hold=6, gap=16):
    global f
    out.append((f, b)); f += hold
    out.append((f, 0)); f += gap
if eu:
    press(START, 6, 60); press(CROSS, 6, 80)   # the language menu: the first language
press(START, 6, 114)                           # the title: the main menu
for _ in range(3):
    press(DOWN, 6, 34)                         # Race, Best Time Trials, Game Progress, Track Editor
press(CROSS, 6, 1284)                          # the confirmation comes up after the editor's level loads
press(CROSS, 6, 700)                           # OK: into the editor
for _ in range(r.randint(0, 3)):
    press(r.choice([LEFT, RIGHT]), 6, 30)      # turn the start grid
press(CROSS, 6, 60)                            # place it
while f < frames:
    kind = r.random()
    if kind < 0.45:
        for _ in range(r.randint(0, 3)):
            press(r.choice([LEFT, RIGHT, UP, DOWN]), 6, r.randint(8, 24))
        press(CROSS, 6, r.randint(20, 60))
    elif kind < 0.55:
        press(CIR, 6, r.randint(20, 50))
    elif kind < 0.65:
        press(r.choice([L2, R2]), 6, r.randint(30, 60))
    elif kind < 0.72:
        press(L1, r.randint(30, 120), 20)        # the 2D map while held
    elif kind < 0.78:
        press(R1, 6, r.randint(60, 150)); press(R1, 6, 40)
    elif kind < 0.84:
        press(START, 6, r.randint(60, 120))
        for _ in range(r.randint(0, 2)):
            press(r.choice([UP, DOWN]), 6, 30)
        press(START, 6, 60)                    # back to the grid
    elif kind < 0.90:
        press(r.choice([TRI, SQ, SELECT]), 6, r.randint(20, 60))
    else:
        f += r.randint(30, 200)
for frame, b in out:
    print(f"0 {frame} {b:04x} 7f 7f 7f 7f")

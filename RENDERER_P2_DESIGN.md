# K2 at full speed: the fast reader, `SHAPE_BATCH`, the reader's thread (renderer plan P2 step 9: design)

Designed 2026-11-22 for `RENDERER_PLAN.md` step 9 ("shader batches") after P1's live 3D (`RENDERER_3D_DESIGN.md`
20: every picture proven equal, 16 to 41 fps wherever 3D is drawn, because the C++ VU1 references run on the EE
thread). "Build batches" (section 11) is the work order for the Opus build agent. Read with `RENDERER_3D_DESIGN.md`
("the 3D design": sections 3, 5-10, 18.5, 20), `RENDERER_2D_DESIGN.md` (2, 5), `RENDERER_FRAME_RECORD.md` ("the frame
record": 3.2, 5, 9). Paths are relative to the repository. "The reference" is the runtime's software GS, "the
references" (plural) the C++ VU1 programs of `decomp/vu1`, as before. Everything measured here comes from `rc_replay
--consumer shapestat` (section 3), added with this design; nothing in it is the renderer, and no file of the game,
the references, the reader or the runtime was touched.

**The short of it.** The plan expected the references to be replaced by vertex shaders. Measured, the VU1 programs
are not vertex shaders: they cull, drop and **clip** triangles themselves, 1.3% of what they read goes through
clippers whose output a GPU cannot be made to reproduce cheaply, and the exact fragment path and its barrier rule
need every vertex on the CPU anyway. What does work, and is measured here bit for bit, is the same arithmetic on the
host under the right MXCSR, written with SSE: **the track program's common path costs 0.7 to 2.1 ms a cycle on
the race levels instead of the 8 to 25 ms it costs in the references, with 0 of 281.5 M triangles differing.** So
this design reaches the frame rate with a fast exact reader on its own thread, keeps every check of P1 valid by
producing P1's lists byte for byte, and gives the GPU vertex stage to the mode that has no use for exactness: P3's.
That is a change to the plan's item 9, and it is the first decision below.

## Decisions for the user

**Answered by the user, 2026-11-22:** U4 = **A** (the fast exact reader); U5 = **yes** (the shader batches move to
P3); U6 = **not in exact mode** (stays D-3D-2). The user's first choice was C (plain vertex shaders, fast and
simple, not bit-exact, since the emulated GS stays as a reference); after the estimate (C about 6-8 batches with
statistical checks, A 7 batches with today's checks) they chose **A now, then C right after as the first P3 batch**
(the enhanced mode, not bit-exact by design). Build batches 1-7 follow; batch 8 may wait for P3.

**U4. Where exact mode's vertex work runs. Recommended: A, on the CPU, fast.** Does not block batches 1 to 7 (they
are the same under A and B, and C needs none of them undone).

- *Found.* (i) The references are slow for a reason that has nothing to do with the mathematics: every operation is
  checked in double precision and every triangle goes through VU memory, a copied GIF packet and the GIF walker.
  Under `MXCSR` = round toward zero + flush to zero + denormals are zero the host's own float operations are the
  VU's but for a few operand classes that can be detected and handed back (4.1). A build of the references with
  only that change gives **the same 1,045.8 M packets, VU memory and registers on all 67 dumps and takes half the
  time**; the track program's common path in SSE, one VU vector operation to one SSE operation, gives **the same
  nine quadwords for all 281.5 M triangles compared and takes a sixth of that again** (3.6, 3.7). (ii) Of 1,470 M
  triangles the programs read in the dumps, 19.2 M (1.3%; up to 1,173 in one kick) take the programs' own
  near-plane and guard-band clippers, which emit a varying number of triangles and carry register lanes from one
  triangle to the next (3.3). (iii) The exact blend's barrier rule needs each primitive's pixels on the CPU before
  the draw is recorded (3D design 10.2).
- **A (recommended): the fast reader.** The references' arithmetic kept, their overheads removed (section 4):
  SSE paths for the track, env, skin and Track Editor programs, the reference's own routines for the 1.3%, triangles
  written straight into the draw list, the reader on its own thread (section 8). The lists are P1's byte for byte
  (check (e)), so `Gs2dVk`, the shader, the barrier rule and every proof of step 8 stand unchanged, on any device.
  Estimated 9 ms a cycle on level 18 off-line (4.6 to 6.2 ms on the front end and levels 5 and 17), on a thread
  that is not the EE's, against 51 to 55 ms on the EE thread today (section 10). Batches 1 to 7.
- **B: the same arithmetic in a GPU compute pass** writing `Gs2dVk`'s records (7.2). Possible on both devices here
  (`shaderRoundingModeRTZFloat32` is true on the RTX 2060 and on the Vega's RADV; denormals and division need care).
  It cannot take the clipped triangles, anything from a pass's first reading primitive on, or water, and the only
  test that tells the CPU which runs are safe without transforming them (a bounding box against the near plane and
  the guard band) frees **a quarter of the track's triangles** (3.4). It would move about 1 ms a cycle off a thread
  that is no longer the EE's, for a second exact implementation, its proof per device, and three or four more
  batches after A's. Not prototyped. Not recommended; nothing in A closes the door on it.
- **C: a plain vertex shader with the GPU's clipper** (`decomp/shaders/trdr_shape.vert`, ico-pc's way), and a list
  of divergences. Worst vertex error against the references 1.11/16 pixel on the quick path, clipped triangles not
  comparable (7.3). No race picture would equal the reference's and checks (d) and the chained replay would be
  statistics. Wrong for the mode whose point is equality; right for the enhanced mode, where it is planned (U5).
- *Default if there is no answer:* A.

**U5. Plan item 9's "shader batches" move to P3. Recommended: yes.** Step 9 then delivers the frame rate by the
fast reader, and delivers the batches as data: `SHAPE_BATCH` and `CONSTANTS` commands in the same list, each proven
to expand to the primitives that follow it (section 5, check (f), batch 8). The vertex shader that draws them from
GPU buffers uploaded once a level (5.4) becomes P3's first batch, where its float positions and the larger targets
are wanted and its divergences are the mode's. The alternative is to build that shader now as a third picture
(`RC_VK_PICTURE=fast`) judged by difference statistics: two more batches in P2 for a mode nothing in P2 needs.
*Default if there is no answer:* yes; batch 8 can itself wait for P3 if step 10 is wanted sooner.

**U6. Real mip levels on the two mipmapped textures. Recommended: not in exact mode.** Plan item 9 lists them, but
the reference machine never samples a mip level (it filters level 0), so exact mode drawing them would differ from
the reference by design: 177,492 primitives and 0.5 M pixels in all 67 dumps (levels 2 and 14). They belong to the
enhanced mode. The alternative, in U1's spirit, is to teach the reference machine the GS's level selection and
re-reference the dumps; that is a runtime change of some size for half a megapixel. It stays D-3D-2 unless you
say otherwise.

**U7 (U3 carried over). K2's GPU time once the native renderer is the GPU's only work: decided by batch 7's
numbers.** The lists do not change, so the fragment cost does not (0.3 to 1.7 ms a kick with the GPU clocked up,
11.9 ms on level 21 with the driver at 300 MHz). Batch 7 times every scene at the real frame rate both ways
(section 10). If the frame rate holds and only the 8 ms figure is exceeded because the driver keeps the clock low,
the recommendation will be to accept it (a GPU that makes every frame at a low clock is not a fault); if the frame
rate does not hold, the lever is 6.5 (conservative depth: the one change to the shader that provably keeps
equality), which then needs its own short design and `rc_gs2d_test` again. Nothing to answer now.

**Decided here (not the user's), for the record:**

1. **The P1 reader stays, as the strict reader, and is the judge.** The fast reader is proven by producing its
   lists byte for byte (check (e): CPU only, the 67 dumps in about a quarter of an hour), and the live check
   compares the live fast reader with the strict one off-line. No fast path is ever checked against a picture.
2. **The lean arithmetic is a second build of the references, not an edit of the first** (4.1): `RC_VU1_REF`,
   `vifstat` and the strict reader keep the operation-by-operation arithmetic.
3. **The reader leaves the EE thread, whole** (section 8), fed by the frame record's page shadow through a queue.
   When it is more than two cycles behind, the EE thread waits: the game slows, as it does today, rather than the
   picture drifting behind the game.
4. **Water gets no fast path yet**: three passes a triangle with state packets between them, 40% of its runs with a
   clipped triangle. It runs the lean reference (1.5 to 2.1 ms a cycle on the race levels timed; batch 4 reports
   its share of what is left).
5. **No GPU work for exactness and no parallel expansion** in step 9; the budget does not ask for either.
6. **Order is the stream's**: no batch is sorted or merged across a state change in exact mode (6.4).

For information, found while measuring:

1. **A K2 chain is nothing but calls.** Every byte of all 44,200 chains is delivered inside a packet called from the
   frame's tag array; nothing nests. 92% of the runs and 97% of the triangles come from packets whose bytes never
   change while they are in use: 5 to 11.5 MB a level (3.2).
2. **Most of what the track program reads it does not draw:** 41.6% culled by facing, 24.2% wholly behind the near
   plane, 4.4% beyond one guard-band edge, 28.5% drawn by the quick path, 1.4% clipped (3.3).
3. **The constants never change inside a call** (0 of 43.1 M runs), and change 40 times a kick (3.5).
4. **MADD in the references keeps a denormal product for the sum and clamps the sum after an overflowing
   product.** Neither was written down; the operations' self-test found both (4.1, 3.6).
5. **8% of the reader's time is CLUT loads** at TEX0 writes that load what is already loaded (3.8).
6. **Both GPUs of the development machine offer round toward zero for 32-bit floats in shaders** (3.9).

## 1. Summary

- **Measured** (section 3): the units K2 calls, what every triangle does in its program, the lean build of the
  references and the track program's SSE path against the references on all 67 dumps, the reader's profile.
- **The fast reader** (section 4): the mode `RZ + FTZ + DAZ` with a few detected exceptions; per program an SSE
  transcription of the common path and the reference's own step for hard triangles; triangles written into the
  list without a GIF packet; the CLUT memo; UNPACK data read in place. Lists equal to the strict reader's.
- **`SHAPE_BATCH`** (section 5): units by address and page generations, `CONSTANTS`, a batch per texture block
  flagged as expanded by the primitives that follow; the geometry a GPU vertex stage would upload once. Not needed
  for the frame rate; built last.
- **State and blend** (section 6): P1's plans stand because P1's lists stand; the alpha planner, the mip decision,
  order and barriers, and conservative depth as the one fill lever.
- **Exactness** (section 7): what the emulated GS is given and what each of A, B and C can and cannot reproduce.
- **The thread** (section 8): the EE thread ships kicks and changed pages (about 1.5 ms a cycle on level 18), a
  reader thread does the rest.
- **Checks** (section 9): the operations' self-test, the lean library under P1's checks (a) and (b), **(e) list
  equality**, (f) batch expansion, the live check against the strict reader.
- **Targets** (section 10): 60 / 50 fps on the front end, levels 5, 17, 18, 21 and the Track Editor; the reader at
  most 12 ms a cycle; U3 timed two ways.
- **Batches** (section 11): 1 lean references, 2 direct triangles, 3 the track's fast path, 4 env, skin, TERdr,
  5 TOPS views, 6 the thread, 7 frame rates and U3, 8 `SHAPE_BATCH`.

## 2. Code layout

```
decomp/vu1/vu_shape.h, *.cpp     one macro: namespace and the four operations (strict: rc::vu1, lean: rc::vu1l);
                                 each program's per-triangle step exported (batches 3, 4)
decomp/vu1/vu_lean.h             the mode (ScopedMode), the lean operations and their fall-backs (batch 1)
render/shape/fast_trdr.cpp ...   fast_env, fast_skin, fast_terdr: the SSE paths (batches 3, 4)
render/shape/fast_run.{h,cpp}    a run: the path, the run's ShapeProgram for hard triangles, registers, poison, declines
render/shape/units.{h,cpp}       prepared units by address and page generations; CONSTANTS, SHAPE_BATCH, UNIT (batch 8)
render/vif/vif1_reader.*         Options::library (strict | lean), Options::fast; TOPS views (batch 5)
render/gs2d/gif2d_reader.*       path1Triangle(); the CLUT and state-packet memos (batch 2)
render/frame/frame_reader.*      Options::fast
render/record/ram_image.{h,cpp}  the RAM image and page application, out of reader.cpp (batch 6)
render/live/kick_queue.{h,cpp}   rc::live::KickQueue (EE thread) and the reader thread (batch 6)
tools/rc_replay/lists.{h,cpp}    the `lists` consumer: checks (e) and (f)
tools/rc_replay/vu1_test.cpp     rc_vu1_test: check (s)
tools/rc_replay/shape_survey.*, shape_lean.*   `shapestat` (this design's measuring tool; stays)
```
Libraries: `rc_vu1_lean` beside `rc_vu1_ref` (the same flags: `-O2 -fwrapv -fno-strict-aliasing`, the project's
`-ffp-contract=off`, no `-mfma`); `rc_shape` (on `rc_vu1_lean`, `rc_gs2d`); `rc_vif` and `rc_frame` link it. None
includes a runtime header.

## 3. Measurements

### 3.1 Tool and dumps

`rc_replay <dump> --consumer shapestat` (`tools/rc_replay/shape_survey.{h,cpp}`, `shape_lean.{h,cpp}`;
`tools/vif_agg.py <outputs>` sums the `V` lines and takes the maximum of the `X` lines). No emulated machine. It
runs P1's `Vif1Reader` with the strict references and, at every run:

- finds the unit the run belongs to, from a walk of the kick's chain that hashes what each packet called at depth 0
  delivers;
- classifies every triangle the run reads by driving the reference's own routines (`setupTriangle`,
  `writeQuickTriangle`, the skin's transform) on a copy of VU memory;
- repeats the run with the **lean build** of the references (`shape_lean.cpp`: the references' sources included as
  text under another namespace, their four operations replaced; MXCSR `RZ + FTZ + DAZ`) and compares its packets,
  the 16 KB of VU memory it leaves and all 128 register lanes with the reference's;
- for the track program, runs **`fastTrdr`** (the common path in SSE intrinsics, section 4.2) on the run's input,
  and in every run without a hard triangle compares its output with the nine data quadwords of each triangle
  packet the reference sends, and its classification with the classifier's.

Run over **the 67 exact dumps of both versions** (3D design 3.1): 44,200 VIF1 kicks, 43,076,205 runs (the count
`vifstat` found), 1,045.8 M packets, **1,469.9 M triangles read**. Four at a time, 50 minutes. Times in 3.8 are
from single runs on an otherwise idle machine (Ryzen 7 3750H). To repeat: `for d in <dumps>; do rc_replay $d
--consumer shapestat > out/$name.txt; done; tools/vif_agg.py out/*.txt`.

### 3.2 Units: what the chains call

| measured over the 67 dumps | |
|---|---|
| stream bytes at depth 0 (outside any called packet); calls inside a call | **0; 0** |
| DMA tags: `call` / `ret` at depth 0; `cnt` and `ref` inside the called packets | 4,877,996 each; 38,372,032 and 22,569,536 |
| packets called in a kick | 110 on average, 323 at most |

A called packet is **fixed** when its address is called in two or more kicks of a dump and delivers the same bytes
every time, **rewritten** when the bytes change:

| | fixed | rewritten | called in one kick only |
|---|---|---|---|
| calls | 1,883,118 (43 a kick) | 2,989,285 (68 a kick) | 5,592 |
| bytes delivered | 246.0 G (93%) | 18.1 G (7%) | 22 M |
| runs | 39,690,331 (92.1%) | 3,385,559 (7.9%): the track program on the polygon and alpha buffers, and the 731,918 direct entries (state packets) | 315 |
| triangles read | 1,421.7 M (96.7%) | 48.2 M (3.3%) | 2,950 |
| what they are | the shapes' and track cells' triangle lists; the microprogram uploads (no run) | the shape-packet rings (constants), state blocks, the polygon and alpha buffers | loading and scene changes |

- **Size of a level's fixed packets:** 5.1 MB (level 8) to 11.5 MB (level 18, Europe); 33 to 129 addresses. The
  Track Editor: 2.3 to 4.9 MB in 270 to 557 small pieces. This is the plan's "5-12 MB walked by reference every
  frame", now with its boundaries.
- **When they first appear:** on a race dump 0.25 to 3.9 MB of the fixed bytes are first called after the dump's
  first kick, at most 1.2 MB in any one later kick (cars and scenery coming into view); 4.3 MB in one kick where
  the front end gives way to a race.
- **Every skin, water and TERdr run comes from a fixed packet**, and so does every shape's and track cell's list
  whichever program draws it (the same list is drawn by `TRdr` and by `EnvRdr`). What is rewritten and has
  triangles is `RDRPOLY`'s and `RDRALPHA`'s buffers, for the track program.
- Per kick a fixed packet delivers 5.6 MB of the 5.95 MB.

### 3.3 What the triangles do

Per triangle read, by driving the reference's routines. **Quick**: no corner behind the near plane and no
guard-band flag: one packet. **Hard**: one or two corners behind (the near-plane paths), or a guard-band flag
other than "all three corners beyond the same edge" (the Sutherland-Hodgman clipper).

| program | triangles read | culled by facing | all behind the near plane | beyond one edge | **quick** | **hard** | runs with a hard triangle |
|---|---|---|---|---|---|---|---|
| `TRdr` | 1,141,089,355 | 474,676,617 (41.6%) | 275,871,771 (24.2%) | 49,732,330 (4.4%) | 324,858,285 (28.5%) | **15,950,352 (1.40%)** | 4,795,359 of 32,642,628 (14.7%) |
| `EnvRdr` | 203,413,334 | 100,279,527 (49.3%) | 1,337,668 | 160,148 | 101,492,527 (49.9%) | 143,464 (0.07%) | 38,492 of 5,685,356 |
| skin | 87,723,901 | (no test) | 8,853,437 (10.1%) | 1,435,922 | 76,402,866 (87.1%) | 1,031,676 (1.18%) | 180,969 of 2,978,048 |
| water | 37,397,024 | 1,126,752 | 17,216,357 (46.0%) | 2,204,150 | 14,769,128 (39.5%) | **2,080,637 (5.6%)** | **415,805 of 1,028,995 (40%)** |
| `TERdr` | 306,656 | 144,604 | 0 | 0 | 162,052 | 0 | 0 of 9,260 |
| all | 1,469,930,270 | | | | 517,684,858 | **19,206,129 (1.31%)** | |

- The track's hard triangles: 8,756,542 by the near plane, 7,193,810 by the guard band, of which 2,410,980 came
  out as fans of 4,632,993 triangles and 4,782,830 were clipped away.
- **Per kick:** 33,256 triangles read on average, 65,618 at most (level 18); 435 hard on average, **1,173 at
  most** (level 24, Europe). Levels 18, 24, 6 and 12 read 52,000 to 59,000 a kick; the front end 25,000.
- A kick sends 12,585 triangle packets (3D design 3.8): the quick ones, the clippers' output, and water's three
  passes of each.

### 3.4 Plain runs and the box test (for option B)

A run is **plain** when every triangle it reads is culled or quick. The **box test** is what a reader could do
without transforming a triangle: the eight corners of the run's bounding box (with both extremes of the depth
bias) through the run's matrix in doubles, each in front of the near plane by a margin of 10^-4 and inside the
guard band by the same.

| program | runs: plain | runs the box test calls plain | triangles read in those | quick triangles in those | called plain but not |
|---|---|---|---|---|---|
| `TRdr` | 11,143,241 of 32,642,628 (34.1%) | 9,113,033 (27.9%) | 292.6 M of 1,141.1 M (**25.6%**) | 171.1 M of 324.9 M (52.7%) | 0 |
| `EnvRdr` | 5,573,840 of 5,685,356 | 5,560,172 (97.8%) | 198.9 M of 203.4 M | 100.7 M of 101.5 M | 0 |
| water | 172,461 of 1,028,995 | 150,840 (14.7%) | 5.4 M of 37.4 M | 5.2 M of 14.8 M | 0 |
| `TERdr` | all | all | all | all | 0 |
| skin | 2,521,490 of 2,978,048 (84.7%) | not tested (the corners are blends through bone matrices) | | | |

The test is safe (never wrong in 39.4 M runs) but the track does not suit it: a track cell near the camera always
has triangles behind the near plane. By level the share of the track's triangles in box-plain runs is 10% (level
9) to 45% (level 7, Europe).

### 3.5 Constants, blocks, sizes

- **Constants** (VU memory quadwords 7-22, water's 18-33): different from the run before in 1,779,925 of 43,076,205
  runs (40 a kick), **never between two runs of one call**; distinct sets in a kick 39.6 on average, 113 at most.
- **Texture blocks:** 88.9 M (2,012 a kick, 7,230 at most; 1.9 a track run). 3D design 3.8 found 544 state runs a
  kick among the drawn triangles: most consecutive blocks leave the state as it was or draw nothing.
- **Runs:** 975 a kick, 1,882 at most (as `vifstat`).

### 3.6 The lean build: equality and time

| | |
|---|---|
| the operations' self-test (ADD, SUB, MUL, MADD on random bit patterns weighted toward denormals, the smallest and largest binades, zeros, infinities and NaNs; each side under its own MXCSR), run at the start of every dump | **0 of 1,072 M results differ** |
| ... before MADD's two fall-backs were in | 143,570 of 16 M (all MADD: a product in the denormal range, which the reference keeps for the sum; a product that overflowed, after which the reference clamps the sum) |
| packets differing from the references', all programs | **0 of 1,045,771,646** |
| runs with another packet count; leaving other VU memory; leaving another register lane | **0; 0; 0 of 43,076,205** |
| the references' time, strict / lean (four processes at a time) | 949 s / 484 s: **0.51** |
| by program, lean | track 288 s, env 90 s, skin 62 s, water 45 s, TERdr 0.1 s |

The lean build changes four functions. Half of the references' time was the double-precision check of every
result; the other half is what a reference is: one scalar operation at a time through `V` structures, VU memory
loads and stores, a vector per packet.

### 3.7 The track program's common path in SSE (`fastTrdr`)

| | |
|---|---|
| triangles read; classified as the classifier of 3.3 classifies them (culled, all behind, beyond one edge, quick, hard) | 1,141,089,355; **all** (0 runs differ) |
| values the mode cannot treat as the VU does (4.1's third column), which would send a triangle to the reference | **0** |
| runs without a hard triangle, where every triangle packet of the reference is compared | 27,847,269 of 32,642,628 |
| quick triangles compared, nine quadwords each (S T Q with the bias lane, the tinted colours, X Y Z with the register's lane) | **281,521,657; 0 differ** |
| time for all 1,141 M triangles (four processes at a time), against the lean track program / the strict one (estimated from 3.6's ratio) | 49.6 s against 287.7 s / about 560 s: **0.17 / 0.09** |
| per triangle read | **43 ns** |

It was right at its first run; the only correction since is the overflow test on MADD's product that the
self-test asked for.

### 3.8 The reader's time today, and the fast pieces

A cycle with K2, milliseconds, single runs (USA `s5` dumps, 700 cycles each; Europe's level 18 last):

| dump | the references (strict) | lean, all programs | lean by program: track, env, skin, water | **the track, SSE common path** | VIF walk | GIF walk and list (3D design 16.3) | the live reader a cycle (3D design 20.4) | triangles read; hard, a kick |
|---|---|---|---|---|---|---|---|---|
| USA `s5/front` | 15.9 | 8.4 | 4.77, 0.91, 2.01, 0.67 | **0.80** | 1.42 | 4.5 | 21.3-21.8 | 25,221; 463 |
| USA `s5/l5` | 18.5 | 9.7 | 3.83, 2.41, 1.52, 1.94 | **0.65** | 1.51 | 5.1 | 30.9 | 27,396; 491 |
| USA `s5/l17` | 18.7 | 9.8 | 4.41, 2.04, 1.21, 2.11 | **0.75** | 1.61 | 5.3 | 35.1 | 29,555; 456 |
| USA `s5/l18` | 33.2 | 17.4 | 12.54, 1.91, 1.43, 1.50 | **2.14** | 3.15 | 9.2 | 54.7 | 58,776; 768 |
| USA `s5/l21` | 24.3 | 12.7 | 7.73, 1.75, 1.15, 2.05 | **1.34** | 2.26 | - | 41.6 | 41,372; 588 |
| USA `s5/l6` | 28.6 | 15.1 | 11.01, 1.55, 1.02, 1.49 | **1.94** | 2.80 | - | - | 52,114; 769 |
| USA `s5/l24` | 31.9 | 16.4 | 11.49, 2.16, 1.33, 1.46 | **2.00** | 2.93 | - | - | 58,522; 727 |
| USA `s5/tedit` | 0.5 | 0.3 | 0.17, 0.00, 0.00, 0.00 (TERdr 0.10) | **0.02** | 0.07 | - | 7.0 | 597; 91 |
| Europe `s5/l18` | 31.8 | 16.5 | 11.89, 1.74, 1.42, 1.44 | **2.05** | 3.10 | 9.4 | 51.3 | 58,561; 764 |

"GIF walk and list" and the live figures are 3D design 16.3 and 20.4. Where today's time goes (`perf`, the strict
reader on USA `s5/l18`, 35,527 samples): `setupTriangle` 28.7% (three corners through the matrix for every
triangle read, a double-precision check after each of 48 operations, and 42% of them then culled), the other
reference routines about 30%, the GIF walker with the reader's vertex and primitive assembly 14%, **the CLUT
buffer's reload at TEX0 writes 8%** (`Clut::lookup`, `GsMemory::read32`, `Clut::load`), the chain walk's copies 2%.

### 3.9 The devices' float controls (for option B)

`vulkaninfo`, `VkPhysicalDeviceFloatControlsProperties`:

| | RTX 2060 (NVIDIA 620.70.06) | Vega 10 (RADV) |
|---|---|---|
| `shaderRoundingModeRTZFloat32` / `RTEFloat32` | **true** / true | **true** / true |
| `shaderDenormFlushToZeroFloat32` / `PreserveFloat32` | false / false | true / true |
| `shaderSignedZeroInfNanPreserveFloat32` | true | true |
| `shaderFloat64` | true | true |

## 4. The fast reader (exact mode)

The fast reader is the P1 frame reader with four things replaced. Its output is the same draw list, byte for byte
(check (e), section 9). The P1 reader stays as it is, under the name **strict reader**: it is the one that runs
beside the emulated machine, the one checks (a) to (d) of step 8 were passed with, and what the fast reader is
compared with.

### 4.1 The arithmetic mode

`MXCSR = (the kick's) | 0x6000 | 0x8000 | 0x0040`: round toward zero, flush to zero, denormals are zero. Under it
the host's own single-precision operations are the VU's on all but a few operand classes:

| the VU's rule (`vu_shape.h`, 3D design 6.2) | under the mode | left to the reference's operation |
|---|---|---|
| a denormal operand is zero | DAZ | |
| an infinity or NaN operand is the largest finite value with its sign | not given | seen as an infinity or NaN *result* (every such operand gives one in ADD, SUB and MUL) |
| a result below the smallest normal number is a zero with the exact result's sign | FTZ (under round toward zero "tiny" before and after rounding are the same set) | |
| a result above the largest is the largest, with the exact sign | round toward zero does that | |
| MADD: the product is rounded to single precision, then added; the product itself is neither flushed nor clamped, the sum is | not given for a tiny or an overflowing product | a zero product of two non-zero factors; a product of the largest magnitude |
| division, float to integer, integer to float | as the reference writes them (their operands are results) | |
| `reciprocalLength` (ERLENG): plain host operations on normalised operands, squares not flushed | not given for a tiny square | a zero sum of squares with a non-zero component (by argument: never met in the dumps, where 3.6's lean build has no such test and is equal) |

The fall-back runs the reference's operation with FTZ and DAZ cleared for its duration. Evidence: 3.6 (1,072 M
random results of the four operations, 0 differ, once the two MADD cases were found; every run of the 67 dumps
repeated by the lean build: packets, VU memory and all 128 register lanes equal).

**Two builds of the references.** `decomp/vu1` is compiled twice from the same sources: `rc_vu1_ref` as today (the
strict arithmetic, namespace `rc::vu1`: `RC_VU1_REF`, `vifstat`, `tools/shader_check`, the strict reader) and
`rc_vu1_lean` (namespace `rc::vu1l`, the four operations of the table). One macro in `vu_shape.h` chooses the
namespace and the operations; `shape_lean.cpp` shows that nothing else in the sources has to change (it does the
same from outside, by including them as text). `rc::vu1l::run` sets and restores the mode itself, so no caller can
forget it. The lean build is what the fast reader calls wherever it still runs a reference: hard triangles, water,
the direct entries, any run it declines.

### 4.2 The fast paths

A fast path is a transcription of one program's common path into SSE, **one VU vector operation, one SSE
operation, in the program's order**, under the mode of 4.1. It is not a rewrite of the mathematics: the operands and
the order are the reference's, so the bits are. `fastTrdr` in `tools/rc_replay/shape_lean.cpp` is the track
program's, as measured (3.7), and is the pattern for the others.

Per triangle a path computes what the program computes up to the point where the outcomes part:

| outcome | decided by | the fast path |
|---|---|---|
| culled | the facing test's sign against the winding flag (track, env, TERdr; water: the sign alone; skin: none) | nothing |
| all behind | the three near-plane sums' signs | nothing |
| beyond one edge | the guard-band flags of the three corners (the scissor routine's first test) | nothing |
| **quick** | no corner behind, no guard-band flag | the nine quadwords the program stores, as three `GsVertex` (4.4) |
| **hard** | one or two corners behind; a guard-band flag that is not "beyond one edge"; or a value of 4.1's third column anywhere in the triangle (the checks are accumulated in two mask registers and tested once) | the reference's own step for this triangle, lean build |

**Hard triangles.** Each reference file exposes its per-triangle step (the body of its loop: `setupTriangle`, the
tint or the lighting or the environment corners, `drawTriangle`) as a function on a `ShapeProgram`. The fast path
keeps one `ShapeProgram` for the run (the reader's registers, VU memory with the run's input in place, the
constants loaded) and calls the step for a hard triangle at its quadword; the packets the step kicks go to the GIF
reader as today. This is right because a triangle's packets depend on nothing an earlier triangle of the run
computed except: the packet buffer's position (it moves where the bytes are built, not the bytes; the flush packet
it triggers has NLOOP 0), and the register lanes of 3D design 3.4 (`VF25.w`, `VF26`, `VF29`, the quick path's
three scratch registers' `w`), which only the hard paths and the env corners write. The clip flags a hard path
tests are the three it has just computed (the scissor routine's masks force the fourth, older group).

**Registers a fast run leaves.** The lanes that can reach a later packet are those of 3D design 3.4. A fast run
does not touch them unless a step ran, and then they are the step's. One exception: **an env-mapped run** (constant
10.z on) writes `VF26`-`VF28` for every triangle, drawn or not; its fast path hands the run's last triangle to the
reference's step, as if it were hard. Every lane the fast path does not leave as the reference would is poisoned
after the run with P1's signalling pattern (`vu1PoisonBits`), so a dependence nobody knew of shows in check (e) at
once instead of passing by luck.

**VU memory a fast run leaves.** A fast path stores each texture block where its program stores it (quadwords 3-6;
water's 5-8 and 13) and kicks that packet from VU memory as the program does; it writes nothing else. The
programs' other writes (the packet buffer, the clip lists, the link slots) are read back only by the triangle that
wrote them, and the GIF tags between the packet buffer's slots come from the shape's packet, not from a program.
So VU data memory after a fast run is the strict reader's except in those work areas, and check (a)'s comparison
of data memory with a keyframe's `VU1M` belongs to the strict reader only. (Water is the exception that proves the
rule: it rewrites its input triangles between its passes, which is one reason it stays on the reference.)

| program | fast path | notes |
|---|---|---|
| `TRdr` entry 0 (track cells, cars and scenery without the env map, `RDRPOLY`, `RDRALPHA`) | batch 3 | measured: 3.7 |
| `EnvRdr` entry 0 | batch 4 | the track path with the colours converted first; env-mapped runs add the corner pass (two `reciprocalLength`s a corner: scalar, as the reference) |
| `Rdr_VUSkinProg` | batch 4 | six bone transforms and the blend in front of the same quick path; no facing test; the matrices are read from VU memory by the addresses in the triangle |
| `TERdr` entry 0 | batch 4 | the lighting in place of the tint, the bias in the quick path |
| `Rdr_VUWaterProg` | **none** | three passes a triangle with state packets between them, the input rewritten, 5.6% of its triangles hard and 40% of its runs with one (3.3): the lean reference, 1.5 to 2.1 ms a cycle on the race levels timed (3.8). Revisit only if section 10's budget fails on a water level. |
| the direct entries (0x2050, 0x2370, 0x2198) | none needed | one packet copied from VU memory |

### 4.3 Declining a run

The fast reader runs the lean reference for the whole run, as P1 does with the strict one, when: the program has
no fast path; the run's input does not parse within the buffer (the count walk leaves the 0x180 quadwords); the
planted-fault options ask for it. Declines are counted per program and reported; in the dumps only water and the
direct entries decline.

### 4.4 Triangles straight into the list

Today a triangle is three round trips: the reference stores nine quadwords in VU memory, `copyGifPacket` copies
ten into a vector, the GIF walker decodes a PACKED tag into register writes, the reader latches them into a vertex
queue and assembles the triangle. `Gif2dReader::path1Triangle(const uint32_t *nine)` does what the last two steps
do for exactly this packet (one PACKED tag with EOP, NLOOP 3, REGS ST RGBAQ XYZ2: 3D design 3.5). The fast paths
call it with the quadwords they computed; the PATH1 sink calls it for a reference's packet whose tag is exactly
that one (hard triangles, water, and everything in batch 2, before there is a fast path), and drops a packet whose
tag has NLOOP 0:

- per corner: S, T, Q from the first quadword's three floats (Q of 0.0 is 1.0, as the reference: 3D design 18);
  R, G, B, A the low byte of each word of the second; X, Y the low 16 bits of the third's first two words, Z its
  third word; **bit 15 of its fourth word is ADC**: with it set on the third corner the triangle is not drawn, and
  the call goes through the walker instead (never seen: 3D design 3.4);
- the vertex queue must be empty (it is after every PRIM write and every whole triangle; otherwise the walker);
- the register file's ST, RGBAQ and XYZ2 (or XYZ3) take the last corner's values, as the walker leaves them, so
  check (a)'s "every register at every kick" holds for the fast reader too;
- then `primitive()` as now: the state, the cull, the record in `SCREEN_PRIMS`.

The flush packets (209 M in the dumps, a fifth of all packets) are dropped at the sink, and a fast path never makes
one. The texture-block packet and the
register blocks still go through the walker from VU memory, with two memos that change no result: **a TEX0 write
whose value is the register's and since which no texture memory was written does not reload the CLUT buffer** (the
reload would read the same bytes; the profile of 3.8 puts 8% of the reader there), and a state packet whose bytes
equal the last one's with no register write between is skipped.

### 4.5 TOPS views

5.8 MB of UNPACK data a kick are copied into VU memory in 2,400 pieces only to be read once (3D design 3.8). The
VIF decoder instead notes a TOPS-relative UNPACK as a **view** (`{destination quadword, count, pointer into the RAM
view}`; a few per run). At the MSCAL the fast path reads its input through the views when one view covers the walk
(the common case: one UNPACK a run); VU memory is brought up to date from the pending views (**materialised**) when
a hard triangle's step or a declined run needs it, when an absolute UNPACK or a later view would overlap them,
before any comparison or keyframe of VU memory, and at the end of the kick (the RAM view is only promised for the
kick's duration, section 8). Views of at most the last two runs are pending at any time (the double buffer).
Absolute UNPACKs (the constants, the bones: 7% of the UNPACK codes, 3D design 3.2) are copied as now.

### 4.6 What it is expected to cost

Section 10 has the table per scene. The parts on level 18 (the heaviest of the timed scenes), a cycle with K2,
off-line: the references 33 ms today; lean 17.4 ms (measured); with the track's fast path about 8 ms (measured:
2.1 ms for all the track's triangles in place of 12.5; plus the 770 hard triangles a kick through the lean step at
an estimated 1.5 us each; the other programs still lean); with batch 4's paths an estimated 5 to 6 ms, of which water
is 1.5. The GIF walk and the list fall from 9 ms to an estimated 3 ms (no walker for three packets in four, the
CLUT memo), the VIF walk from 3 ms to under 1 ms.

## 5. `SHAPE_BATCH`, units and the geometry cache

In exact mode the fast reader needs none of this section to reach its frame rate: it expands as it reads. The
section is what P3's vertex shader, P3's interpolation and options B and C need from the same list, and it is built
last (batch 8) so that P3 starts from a list whose batches are already proven against their expansion.

### 5.1 Units

A **unit** is a packet the frame's tag array calls: the target of a `call` at depth 0 with everything it delivers
up to its `ret` (3.2: nothing in a K2 chain is outside one, nothing nests). Units are of two kinds by what was
measured, and the reader tells them apart by watching, not by address ranges:

- **fixed**: a shape's or a track cell's triangle list (`pShape->pTri`, the cell's own tags), a microprogram's
  upload packet. The same bytes at every call for as long as the level lives; 33 to 129 addresses and 5 to 11.5 MB
  on a level, 92% of the runs and 97% of the triangles;
- **rewritten**: the 360 shape packets and the Track Editor's ring (the constants of one draw), the state blocks,
  the polygon and alpha buffers, the skin and water packets. 8% of the runs (a fifth of them state packets through
  the direct entries), 3% of the triangles.

Identity and validity come from the RAM view, the same way live and off-line: `RamView::generation(page)` counts
the captures of a page. A unit's **generation** is the sum of its pages' generations when it was last walked; the
unit is fixed while that sum does not move. Nothing is hashed and nothing is compared a second time: the page
shadow already compared the pages (section 8).

A unit is **prepared** the first time it is called and found unchanged since the kick before: one walk records
its extent (the pages it and its `ref` data lie on), its VIF codes as a list of effects (UNPACK views, MSCAL entry
points, MPG with the program it selects, BASE, OFFSET), and per run the texture blocks with the offset and count
of their triangles. Replaying a prepared unit applies the effects to the VIF1 registers without decoding a byte,
and hands each run to its fast path with its blocks already found. The program a run meets is not part of the
unit: the same list is drawn by `TRdr` and by `EnvRdr` (the shiny pass and the pass over it), so the preparation is
per input format (the ten-quadword shapes; the skin's sixteen).

### 5.2 The commands

| op | when | a, b, c, d | payload |
|---|---|---|---|
| `CONSTANTS` (11) | the VU memory a run's program reads as constants differs from the last one sent | a: first quadword, b: count | the quadwords: 0-33 for the shape programs (the packet header with PRIM, the tint, the env rows and settings, the clip range, the viewport, the light position, the matrix; TERdr's lights; water's set eleven further on), 0-511 for a skin run (the bones) |
| `SHAPE_BATCH` (8) | each texture block of a run, after its `STATE` | a: the unit's address, b: its generation, c: program, entry, flags, d: the block's first triangle as a byte offset into what the unit delivers, and the count | none; with flag `inline` (a block of a rewritten unit) the triangles' quadwords as sent |
| `UNIT` (12) | first use of a unit's generation, only when the consumer asked for geometry | a: address, b: generation | what the unit delivers (the UNPACK data in stream order) |

`SHAPE_BATCH` flag `expanded`: the `SCREEN_PRIMS` that follow, up to the next `SHAPE_BATCH`, `STATE` or scope
command, are this batch's expansion. An exact consumer draws them and skips the batch; an enhanced consumer draws
the batch and skips them (or tells the reader not to expand at all, once it needs no exact picture beside it).
That is how **both modes share one list**, and why `rc_replay` can judge an enhanced picture against the exact one
from the same dump with nothing else changed.

**Side tags.** `TRdr_VUShape` and the skin renderer already open a `SpanChain` with `DrawKey{DrawShape | DrawSkin,
the shape's address}` and the object's 3 x 4 matrix; the reader passes it through as `SCOPE_BEGIN` / `SCOPE_END`
around the runs between its anchors (3D design 8), so every `SHAPE_BATCH` of an object lies inside its scope. That is
the key P3 interpolates by (the same object in two consecutive lists: its `CONSTANTS` matrix, or the tag's matrix
with the `ViewTag`'s camera). Step 9 adds no tag. Two are worth having later and are listed, not built: `pass`
(world, objects, alpha, water, effects: has no call site yet) for an enhanced mode that reorders opaque work, and a
geometry notice from the level loader (address range, loaded or freed) that would let the page shadow skip the
fixed lists (section 8).

### 5.3 What flows each frame

| | exact mode (option A) | a GPU vertex stage (P3; options B and C) |
|---|---|---|
| EE thread to the reader | the kicks; the pages that changed: 0.3 to 0.5 MB a cycle (the rings, the polygon and alpha buffers, K1, animated texture frames: frame record 3.2) | the same |
| the level's lists (5 to 11.5 MB) | read in place from the RAM view each frame by the fast paths (no copy) | `UNIT` once a generation: most of a level's 5 to 11.5 MB at its first K2, at most 1.2 MB in any later kick of a race (3.2) |
| reader to presenter | `STATE`, `TEXTURE`, `SCREEN_PRIMS`: 0.6 MB a cycle on average, 2.2 MB at most (3D design 8) | `STATE`, `TEXTURE`, `CONSTANTS` (30 to 60 a kick, 544 bytes each; a skin's 8 KB), `SHAPE_BATCH` (32 bytes a block), the inline blocks of rewritten units (1,090 triangles a kick on average: 0.17 MB) |
| presenter to GPU | the record buffer (44 words a drawn triangle) | per frame: a constants array and the inline triangles; the geometry buffer persists |

### 5.4 Geometry on the GPU (not built in step 9)

One storage buffer per level holds every fixed unit's delivered quadwords as sent (the ten quadwords a triangle
that `decomp/shaders/trdr_shape.vert` already reads as `quad[]`: normal and winding, three positions, three S T Q
with the bias, three colours); a unit's generation owns a range of it, freed when the generation dies. A draw is
`3 x count` vertices from `first` with no vertex attributes, the batch's constants by index, the state's pipeline
and texture. The vertex stage is the existing shader's lineage (`shapeMode` 0 to 3), handing the fragment stage
float positions before the 1/16-pixel rounding, as the plan's P3 asks. What the VU1 programs do with a near plane
and a guard band the GPU's clipper does differently, which is the heart of section 7.

## 6. State and blend rules

Nothing in this section changes what is drawn in exact mode: the fast reader's list is P1's, so `planState`, the
pipelines and the shader are P1's. The rules are written down because plan item 9 asks for them and because the
batches make three of them cheaper to apply.

### 6.1 The plans (3D design 3.6 and 9.1, as built)

| PATH1 state | share of primitives | plan |
|---|---|---|
| ALPHA 0x44 with PABE 1, test LEQUAL 0x2f fail KEEP (the default 3D state), and PRIM without ABE | 79.7% | blend off, depth write; the alpha test as a discard. PABE needs no blend: it would blend a pixel only where As >= 0x80, and the test has already kept every pixel above 0x2f. The guard `PABE_BLEND` fires on any other test with PABE. |
| 0x29 / 0x68 / 0xa1 with FIX 0x80, 0x62 / 0x68 with FIX 0x15 (the env pass, water's passes 2 and 3, additive and subtractive effects) | 15.6% | fixed-function blend, integer exact: no read, no barrier |
| 0x09, 0x01, 0x29 with FIX 0x7f (water's pass 1, the alpha triangles; all with ZMSK) | 4.7% (measured, 3D design 3.6; the plan's earlier figure was 2.7%) | **the exact path: the local read**, barriers by covered pixels |
| ZTST GEQUAL (the env pass, water's passes 2 and 3) | | `GEQUAL` on the `D32F` buffer: exact |
| TEX1 MXL 3 / 4 (two textures, 177,492 primitives, 0.5 M pixels in all dumps) | | level 0, as the reference; U6 |

### 6.2 The alpha planner

The default state's test passes where `As <= 0x2f`. As is the texture's alpha times the vertex alpha (MODULATE, TCC
1: `(At * Av) >> 7`). The cache has each texture's alpha range as decoded (`GsState::texAlphaMin/Max`), and nine
textures in ten are 0 throughout (frame record 17.3). The planner's rule, for a draw (a run of primitives under
one state): **when `min(0xff, (texAlphaMax * AvMax) >> 7) <= AREF` the test cannot fail and the draw's test is
ALWAYS** (no discard in the shader); when `(texAlphaMin * AvMin) >> 7 > AREF` nothing can pass and the draw is left
out. `AvMax` and `AvMin` are the draw's own vertex alphas, which the presenter has when it builds the draw (a
prepared unit knows them per block without looking: the colours' fourth lanes are not tinted). With a texture
alpha of 0 the vertex alpha does not matter. **P1 carries the range in every `GsState` but `planState` does not
read it yet**: every K2 draw of the default state runs the discard.

The rule changes the pipeline a draw takes, not the list and not a pixel, so its proof is `rc_gs2d_test` (a new
case: the same primitives under both variants over a texture whose range sits on either side of AREF) and check
(d) again. What it buys in exact mode is small, a shader variant without the discard, and it is not what stands
between P1 and the frame rate; **it is step 10's first item, not one of step 9's batches**, and comes with 6.5 if
U7 asks for that (a fragment that cannot be discarded can have its depth written early as well as tested early).
In the enhanced mode it is what lets the opaque nine tenths use plain early depth.

### 6.3 Mip levels

Exact mode samples level 0 because the reference machine does (3D design, information item 3). Plan item 9's "real
mip levels on two textures" cannot be had in exact mode without changing the reference: U6.

### 6.4 Order and barriers

- **Order is the stream's.** A batch's triangles are drawn in input order, batches in list order, and nothing is
  sorted or merged across a state change. Under ZTST GREATER the first of two equal depths wins, so even the opaque
  79.7% depend on order. An enhanced mode may reorder opaque batches by texture or front to back; exact mode may
  not, and no batch of this design does.
- **Barriers are `GsCoverMap`'s, unchanged** (3D design 10.2): 16 a K2 kick on average, 85 at most in the 67 dumps,
  171 in one K1 kick on the memory card screens. The rule needs each primitive's pixels **on the CPU before the
  draw is recorded**, from the pass's first reading primitive on (626 primitives a K2 kick on average, 5,181 at
  most: 3D design 3.9). The fast reader has them, as P1 has. A GPU vertex stage has not, and that is one of the two
  things option B has to solve (section 7).

### 6.5 Fill: the one lever that keeps equality

K2's GPU time is its fragments (3D design 18.5): every covered pixel runs the exact shader, because the shader
writes `gl_FragDepth` (the reference's per-pixel Z) and the device therefore tests depth after shading. Level 21
covers the picture 14 times over. If U3's timing (section 10) asks for it, the lever is **conservative depth**:
the vertex stage gives the whole triangle the depth `(max of its three Z + 1) / 2^24` instead of the constant it
gives now, and the shader declares `layout(depth_less) out float gl_FragDepth`. The reference's Z at a pixel is a
convex combination of the three, plus a half, truncated, so it never exceeds that bound; the device may then reject
a fragment whose bound already fails GREATER or GEQUAL before shading it, and what is written is still the exact
value. It changes `gs2d.vert` and one line of `gs2d.frag`, so it needs `rc_gs2d_test` T1-T10 and check (d) again
on all 67 dumps. Not in step 9 unless batch 7 says so.

## 7. Exactness: what the emulated GS is given, and where each option gets it

The reference machine's picture is a function of the PATH1 primitives it is handed: for each drawn triangle three
vertices of X and Y in 1/16 pixel, a 32-bit Z, four colour bytes, S, T and Q as float bit patterns, in the stream's
order, under the register state the programs' packets leave. Step 8 proved the chain from those to the picture
(check (d), the chained replay). So "exact" for step 9 means: **the same vertices, the same triangles, the same
order, the same registers.** What makes them:

| part | the VU1 programs' behaviour | where a mistake of one unit would come from |
|---|---|---|
| the arithmetic | 4.1's table: no denormals, no infinities, truncation, MADD's product rounded first | any correctly rounded (round to nearest) evaluation: the `mxcsr` fault of 3D design 16.4 moves 12,767 packets in four cycles |
| the conversion to 1/16 pixel and to Z | truncation toward zero, saturating at 0x7fffffff | rounding to nearest: a different pixel grid position for half of all vertices |
| the triangles | the programs cull by facing, drop what is behind the near plane or beyond one guard-band edge, and **clip the rest themselves**: at the near plane into one or two triangles, at the guard band into a fan of up to five, each new vertex made by the VU's own interpolation of position, S T Q and colour, then rounded | a GPU clipper cuts at other planes (its own near plane, its own guard band or none) and interpolates in its own precision: the triangles the GS would have rasterised do not exist |
| the registers | the texture blocks' and register blocks' packets, water's state between passes, per triangle that survives | a batch drawn without walking its blocks |

### 7.1 Option A (recommended): the fast reader

Sections 4 and 8. Equality by construction where the transcription is right, and proven where it matters: check
(e) compares every list with the strict reader's. Device-independent. Cost: batches 1 to 6, CPU time of section
10 on a thread that is not the EE's.

### 7.2 Option B: exact vertex work in a compute pass

What it would be: the units' quadwords in a storage buffer (5.4); per K2 kick a compute dispatch that runs the
quick path for every triangle of every batch and writes `Gs2dVk`'s 44-word records (a culled, all-behind or
beyond-one-edge triangle writes a record that covers nothing); the fragment stage unchanged.

- *The arithmetic is available on both devices here.* `VkPhysicalDeviceFloatControlsProperties`:
  `shaderRoundingModeRTZFloat32` is true on the RTX 2060 (driver 620.70.06) and on the Vega 10 (RADV), so a SPIR-V
  module can ask for round toward zero (`RoundingModeRTZ`, with `NoContraction` on every result). The RTX 2060
  reports neither `shaderDenormFlushToZeroFloat32` nor `shaderDenormPreserveFloat32`, so denormals would have to be
  tested on the bit patterns; division and square root are not required to be correctly rounded by the
  specification (2.5 ULP) and would need a remainder step or doubles (`shaderFloat64` is there on both). It is a
  per-device property: a device without the mode would run option A.
- *What it cannot take over.* (i) **Hard triangles**: their output count varies, and they carry register lanes from
  one to the next; they stay on the CPU, and the CPU only knows which they are by doing the transform. The test
  that needs no transform, a run's bounding box against the near plane and the guard band, was measured (3.4): it
  frees **a quarter of the track's triangles** (25.6% of those read; 10% to 45% by level: the track under and
  around the camera always has corners behind the near plane), nearly all of env's, a seventh of water's runs.
  (ii) **The barrier rule** needs pixels on the CPU (6.4): everything from a pass's first reading primitive on
  stays on the CPU as well, or takes the box rule's ten times as many barriers (3D design 3.9). (iii) Water.
- *So* B moves roughly a quarter of the track's transform and all of env's to the GPU: of level 18's estimated
  5 to 6 ms of fast paths and references a cycle, about 1 ms, on a thread that is no longer the EE's. It adds a
  second exact implementation (GLSL), its proof on every device, two producers of one record buffer, and three or
  four batches. Nothing was prototyped on the GPU for this design; the figures are section 3's counts.

### 7.3 Option C: a plain vertex shader, with a list of divergences

`decomp/shaders/trdr_shape.vert` as it stands (ico-pc's way, and what `RC_TRACK_SHADER=1` does in the emulated
GS's hardware renderer today): the device's floats, its clipper, real perspective. Measured against the reference
on captured track runs (`tools/shader_check`, 3D design 16.5): 52,634 triangles of the quick path, worst
position error 1.11/16 pixel, Z 2, colour 0; clipped triangles are not comparable at all. A sixteenth of a pixel
moves coverage along every edge and every interpolant inside, so no picture of a race would equal the
reference's and checks (d) and the chained replay would become difference statistics, which is what U1 was
decided to avoid. As the **default** mode it is not what the project has asked for so far. As the **enhanced**
mode's vertex stage it is exactly right, and it is 5.4.

### 7.4 The two modes on one list

Exact: `SCREEN_PRIMS` (the expansion), `Gs2dVk`, the 640-pixel targets. Enhanced (P3): `SHAPE_BATCH` from GPU
buffers with float positions into larger targets; K1 and everything without a batch still as `SCREEN_PRIMS`,
scaled. One reader, one list (5.2), one set of textures and states. The exact picture of any enhanced session can
be had from its recording, which is what keeps P3's bugs findable.

## 8. The reader's thread

**Yes: in P2 the reader leaves the EE thread, all of it.** After the fast paths the reader is an estimated 9 ms a
cycle on level 18 off-line, and P1's live figures ran 1.2 to 1.4 times the off-line ones (the EE thread shares its
caches with the emulated machine's threads); the game's own frame has to fit beside it in 16.7 ms, and nothing
about the reader needs the EE thread except the bytes.

### 8.1 What stays on the EE thread

A live consumer in the capture's list, `rc::live::KickQueue`, which per event appends a job to a queue and
returns:

- hardware stores, CRT changes, side tags, cycle begin and close: copied as they are;
- a kick (any channel): the `KickView` with its spans and labels, and **the pages its stream touches that are new
  or changed since their last capture**, from a `PageTracker` of its own. That is the frame record's page shadow
  (frame record 3.2), the mechanism every off-line consumer already runs on: the queue is a dump that is never
  written. One `walkChain` over the kick with a visitor that touches the ranges, then `collect`;
- after a save-state load: the keyframe the capture already builds for live consumers, copied.

Cost, from the recorder's measurements (frame record 6.3): the page compare 1.2 ms per 10 MB of stream, and no
stream hash; changed pages copied 0.3 to 0.5 MB a cycle. **About 1.5 ms a cycle on level 18, under 1 ms in the
front end** (to be measured by batch 6; target: 2 ms on level 18). The fixed lists are compared every frame and
never copied. If that compare ever matters, the levers are step 10's: write-protect the pages of prepared units
and compare only after a fault, or the loader's geometry notice (5.2).

### 8.2 The reader thread

Owns a RAM image (as `rc::rec::Reader` does off-line: the same code, factored out as `rc::rec::RamImage`), applies
a job's pages, and calls `rc::live::Reader` with a `RamView` of the image. By the page shadow's guarantee the image
equals guest RAM **at the kick** on every byte of the kick's stream, so the lists are the EE-thread reader's byte
for byte, whatever the game has done to its buffers since. (The game's own rule is weaker: it only keeps a
frame's packets intact until the next `sceGsSyncPath`. The queue does not rely on it.) The thread sets its own
MXCSR from the `KickView`. Lists are published to the `Mailbox` as now; the presenter does not change.

### 8.3 When the reader falls behind

The queue holds whole cycles. **When more than two closed cycles are waiting, the EE thread waits at its next
cycle close until one is taken.** The game then runs slower, as it does today on every frame, instead of the
picture falling ever further behind the game; waits and their time are counted and reported, and section 10's
acceptance is zero waits in steady state. Dropping instead is not possible in general: the reader's state (VU
memory, the register file, texture memory) needs every kick. A reader that keeps the state but skips a late
cycle's expansion and draws is a lever for step 10, not built here.

### 8.4 Details the batch must get right

- **`DLST`.** The lists now close on the reader thread, up to two cycles after their cycle's `CYCL` was written.
  The writer takes a `DLST` chunk from that thread, in any position before the dump closes; the `dlst` consumer
  keeps its own lists until their chunks arrive and matches by the chunk's cycle number. Closing a recording
  drains the queue first.
- **Recording and the live reader together** walk each kick twice (the recorder's tracker and the queue's); only
  the test sessions do that.
- **`RC_VK_READER_THREAD=0`** keeps the reader on the EE thread: the same lists (the live check passes both ways),
  and the way to tell a threading fault from a reader fault.
- Nothing flows back from the reader to the game, so the oracle, `RC_SHADOW` and game behaviour cannot change; the
  only effect the game can feel is 8.3's wait.
- The emulated machine (VIF1, VU1, the GS) keeps running on its own threads as in P1. Turning the emulated GS's
  rasteriser off under `vk` is step 10's question and U7's measurement (section 10).

## 9. Checks

Every batch runs the checks of its row; "the 67" are the exact dumps of both versions (3.1), "the live dumps" the
sixteen sessions of 3D design 20 in `build-*/frame-dumps/live`.

| check | what | how | pass |
|---|---|---|---|
| (s) operations | the lean operations against the strict ones | `rc_vu1_test` (new, small): 4.1's table on random and edge bit patterns, each side under its own MXCSR | 0 differing of 10^8 |
| (a), (b) lean | the strict reader with the lean library in place of the strict one | `rc_replay <dump> --consumer vif --verify --vu1 lean` | the recorded GIF hash and byte count on all 44,200 VIF1 kicks; VIF1 registers, micro and data memory, registers at all keyframes; the register file at every kick. With `--with-emu` on the fourteen dumps of 3D design 11.2: packet by packet |
| **(e) lists** | the fast reader against the strict reader | `rc_replay <dump> --consumer lists` (new): both readers on the dump, seeded alike; each cycle's lists compared as `dlst` compares (commands, arena, with the first differing command, field and payload byte named) | every cycle of the 67 and of the live dumps byte-equal; the register file equal at every kick; the six register lanes of 3D design 3.4 equal after every run; declines as 4.3 predicts |
| (e) faults | planted in the fast paths | `--fault`: the bias left out; one MADD fused; the truncation of X rounded; "beyond one edge" tested on two corners; a hard triangle drawn by the quick path; the env run's last step left out (on a water level); the CLUT memo ignoring a texture write; ADC ignored | each fails (e) on USA `s5/l17` (and `s5/l12`, `s5/tedit` where named) |
| (d), chained replay | Vulkan, unchanged code, fast reader | `rc_replay --consumer vk --verify` | a regression, not the proof: the 67 once at batch 4 and once at batch 7; equal lists make equal pictures |
| (f) batches | `SHAPE_BATCH` carries enough | `rc_replay --consumer lists --batches`: each `expanded` batch expanded again from its `UNIT` or inline payload, its `CONSTANTS` and its `STATE` alone | the `SCREEN_PRIMS` that follow it, byte for byte, on the 67 |
| live | the live reader on its thread | `tools/live_check.sh` (exact mode from boot, `DLST`, then `rc_replay dlst` and `vk --verify`); **`dlst` runs the strict reader off-line** | the ten sessions of 3D design 20.2 and the four new-scene sessions: every live list byte-equal, every flip hash-equal; again with `RC_VK_READER_THREAD=0` |
| the game | nothing else moved | `tools/oracle.sh --all` both versions when a game object was rebuilt; `playtest2` under `gs` (658 at 60 fps, 1167 at 50); `RC_VU1_REF=1` on level 17; validation layer live, 0 errors | as before |

Check (e) is what lets the batches be small: it runs on the CPU alone (about the vif consumer's time, ten to
fifteen minutes for the 67, four at a time), needs no GPU and no emulated machine, and reduces every question about
a fast path to "is the list the same".

## 10. Frame rates and U3

**Targets** (the same runs as 3D design 20.4: `RC_RENDERER=vk RC_VK_PICTURE=native`, RTX 2060 by PRIME offload,
an idle machine, 90 s a level, `tools/fps.py` on the 5 s samples after the load):

| scene | USA | Europe |
|---|---|---|
| front end (`playtest2`'s menus) and its race | 60 | 50 |
| levels 5, 17, 18, 21 (`RC_BOOT_LEVEL`) | 60 | 50 |
| Track Editor (`tedit_walk.py 1`) and its race | 60 | 50 |

Pass: the median at the target and no 5 s sample more than 1 fps under it; 0 waits of 8.3 after the load; the
reader thread at most 12 ms a cycle on average on every scene (two cycles of queue absorb a longer one); the EE
thread's share at most 2 ms a cycle on average. Step 10 repeats it on levels 24, 6, 8, 9, 12.

**Where the time should be after each batch** (a cycle with K2, milliseconds on the reader's thread; "measured"
columns are section 3's, single process; the rest are estimates from the profile of 3.8 and are what the batches
report against):

| scene (USA `s5` dumps) | today: references + VIF walk + list | after 1 (lean): measured references | after 2 (direct triangles, memos): the list a third | after 3 (the track): measured path, hard triangles 1.5 us each | after 4 (env, skin, TERdr at the track's ratio; water lean) | **after 5 (TOPS views): the VIF walk 0.3 of today's** | the EE thread after 6: 0.12 ms a MB of stream + 0.3 |
|---|---|---|---|---|---|---|---|
| front end | 15.9 + 1.4 + 4.5 = **21.9** | 14.3 | 11.3 | 7.9 | 5.5 | **4.6** | 0.9 |
| level 5 | 18.5 + 1.5 + 5.1 = **25.2** | 16.3 | 12.9 | 10.2 | 7.1 | **6.0** | 0.9 |
| level 17 | 18.7 + 1.6 + 5.3 = **25.6** | 16.7 | 13.1 | 9.9 | 7.4 | **6.2** | 0.9 |
| level 18 | 33.2 + 3.2 + 9.2 = **45.6** | 29.7 | 23.6 | 14.1 | 11.4 | **9.2** | 1.5 |
| level 21 | 24.3 + 2.3 + 6.7 (est.) = **33.3** | 21.7 | 17.2 | 11.5 | 9.2 | **7.6** | 1.2 |
| level 6 | 28.6 + 2.8 + 7.9 (est.) = **39.4** | 25.8 | 20.5 | 12.4 | 10.2 | **8.3** | 1.4 |
| level 24 | 31.9 + 2.9 + 8.9 (est.) = **43.7** | 28.2 | 22.3 | 13.7 | 10.8 | **8.8** | 1.5 |
| Track Editor | 0.5 + 0.1 + 0.3 (est.) = **0.9** | 0.6 | 0.4 | 0.4 | 0.3 | **0.3** | 0.3 |
| level 18, Europe | 31.8 + 3.1 + 9.4 = **44.3** | 29.0 | 22.7 | 13.8 | 11.2 | **9.0** | 1.5 |

Level 18 is the scene to watch: an estimated 9 ms off-line, of which the list is 3.1 (the least certain figure),
the track's path 2.1, water 1.5, hard triangles 1.1, the VIF walk 0.9. P1's live reader ran 1.2 to 1.4 times its
off-line time, so 11 to 13 ms live, against a 16.7 ms cycle on a thread of its own. If it does not hold, in order:
a fast path for water's first pass; rejecting whole runs by the box test turned round (a run whose box lies wholly
behind the near plane or beyond one guard-band edge draws nothing: a quarter of what the track reads is "all
behind"; not measured); 8.3's skip. The Track Editor's time is its 2D (K1), which this step does not touch and
which already holds 60 / 50.

**U3 (K2's GPU time), to be settled by batch 7.** The lists do not change, so 3D design 20.5's figures stand: 0.3 to
1.7 ms a kick with the GPU clocked up, 11.9 ms on level 21 with the driver at 300 MHz. What changes is the load:
at 60 lists a second instead of 20 the driver may raise the clock by itself, or may not. Batch 7 times each scene
twice, K2 per kick and the whole list per frame from the presenter's timestamps with `nvidia-smi` sampled beside:
(i) as `tools/play.sh` runs (the emulated GS's hardware renderer on the same GPU), (ii) with the emulated GS in
software (`PS2X_GS_HW=0 PS2X_GS_GPU=0`), the native renderer the GPU's only work. Pass: K2 at most 8 ms a kick on
average in both, and the frame rate held in both. If (ii) fails on level 21 only by the clock, the answer is U7; if
it fails at full clock, 6.5.

## 11. Build batches (Opus), each with its check

Each is one agent run. The order is by what unblocks what; 6 is independent of 2 to 5 and can run beside them. A
batch that fails its check stops and reports; none of them changes a shader, the RHI or the emulated machine.

**1. The lean references.** `vu_shape.h`: the namespace and the four operations behind one macro; `rc_vu1_lean`
beside `rc_vu1_ref`; `rc::vu1l::run` sets the mode; `rc_vu1_test`; `Vif1Reader::Options::library`; `rc_replay
--vu1 lean`. *Checks:* (s); (a), (b) lean on the 67 and beside the machine on the fourteen; the strict build's
objects unchanged (`RC_VU1_REF=1` on level 17: 0 mismatches; the oracle if the game relinked). *Report:* the
references' time a cycle per scene, strict and lean (expected: 3.6's ratio, about a half).

**2. Triangles straight into the list.** `Gif2dReader::path1Triangle` and the sink that recognises the triangle
and flush packets by their tags, the CLUT and state-packet memos (4.4); `FrameReader::Options::fast` (the lean
library, the direct entry, the memos); `rc_replay --consumer lists`. *Checks:* (e) on the 67 and the live dumps
with its ADC and memo faults; `gif2d` (a) for K1. *Report:* "GIF walk and list" a cycle (expected: a third of
today's).

**3. The track's fast path.** `render/shape/`: `fast_trdr` from `shape_lean.cpp`'s `fastTrdr` (to `GsVertex`
through batch 2's entry), the per-triangle step exported from `trdr_shape.cpp`, the run's `ShapeProgram`, the
registers and poison of 4.2, declines (4.3). *Checks:* (e) with the fast-path faults; the lanes; `shapestat` still
reports 0 differing. *Report:* the reader's time a cycle per scene; hard triangles and declines.

**4. Env, skin, TERdr.** The three paths and their steps (4.2's table), the env-mapped run's last step. *Checks:*
(e) on the 67 (TERdr: `s5/tedit` and the two Track Editor race sessions of 3D design 20.7), the env fault on a
water level; (d) and the chained replay once on the 67 as a regression. *Report:* as 3; water's share, and whether
section 10's budget needs a water path.

**5. TOPS views.** 4.5. *Checks:* (e); (a) for the strict reader untouched; a fault that leaves a view
unmaterialised before a hard triangle. *Report:* "VIF walk" a cycle.

**6. The reader's thread.** `rc::live::KickQueue`, `rc::rec::RamImage`, the reader thread, 8.3's wait, the `DLST`
writer from another thread, `RC_VK_READER_THREAD`. *Checks:* live, both ways; thread sanitizer on `rc_replay
--consumer live2d` driven through the queue (a test harness that feeds a dump's events from one thread to the
reader on another); recording off: `playtest2` unchanged. *Report:* the EE thread's share and the reader's time a
cycle per scene, waits.

**7. Frame rates and U3.** No code but what the measurements need. Section 10's table for both versions, U3's
two configurations, the screenshots of 3D design 20.3 again (exact-mode pairs equal on every pixel). *Outcome:* a
pass makes `RC_VK_PICTURE=native` the default under `RC_RENDERER=vk` (3D design 20.8) and hands step 10 its list; a
fail names the scene and the part (reader, EE share, GPU) and proposes the lever (a water path, 8.3's skip, 6.5).

**8. `SHAPE_BATCH` and units.** 5.1 and 5.2: prepared units by page generations, `CONSTANTS`, `SHAPE_BATCH` with
`expanded`, `UNIT` on request; the fast reader replays prepared units instead of decoding them. *Checks:* (e)
with the new commands left out of the comparison, then (f); the live check. *Report:* the list's size with and
without them; the VIF walk's time. Can be left to P3's first batch if step 10 is wanted sooner: nothing in 7's
outcome depends on it.

(Option B, if chosen instead of leaving it: after 8, a design note for the compute stage, then batches for the
quick path in GLSL with its record-level check against the fast reader's expansion, the box test, and the split
of the record buffer. Option C is 5.4 and is P3's first batch either way.)

## 12. What makes `vk` the default (the end of step 10)

1. Checks: (e) on the 67 and every live dump; the chained replay 67 / 67; the live check on its fourteen sessions
   with the reader on its thread; the oracle and `playtest2` as before. No guard fired in any of them.
2. Frame rates: section 10's table for both versions, and step 10's heavy levels (18, 17, 24, 6, 8, 9, 12), with
   zero waits; a twenty-minute session of each version through the front end, three races and the Track Editor
   without a guard, a validation error or a wait after the loads.
3. U3 answered in both configurations of section 10, and U7 decided.
4. Step 10's own items: pipelines built at start-up (no first-use compile in a race: the longest presenter frame
   reported), the DIVERGENCES file written from 3D design 19.5 and section 13.
5. The scenes no recording reaches (podium, results, more two-player) recorded and passed, or listed as known gaps
   the user accepts.
6. A fall-back that cannot be worse than today: when a guard stops a kick live, or the device lacks the local
   read, the session shows the emulated GS's picture (`RC_VK_PICTURE=gs`, F7) and says so once.

Then `RC_RENDERER=vk` with the native picture is what `tools/play.sh` starts; `gs` stays as the reference and as
the oracle's machine.

## 13. Divergences this design adds or keeps (for step 10's list)

| id | what | where | size |
|---|---|---|---|
| D-P2-1 | none in exact mode: the fast reader's lists are the strict reader's | | check (e) |
| D-P2-2 | VU data memory in the programs' work areas, and the register lanes that reach no packet, are not the machine's after a fast run | reader state only; never drawn | by design (4.2); the strict reader has them |
| D-P2-3 | PATH1's flush packets and the bytes of triangle packets are not made, so a fast reader has no GIF byte hash (check (b)) | | the strict reader has it |
| D-3D-2 | mip levels not sampled, as the reference (U6) | two textures | 0.5 M pixels in all dumps |
| (P3) | option C's vertex stage: device floats, the GPU's clipper | every 3D picture of the enhanced mode | up to 1.11/16 pixel a vertex on the quick path; clipped triangles differ in shape |

## 14. Open

- U4 to U7.
- The estimates of section 10 that are not measurements: the fast paths of env, skin and TERdr, the list after
  batch 2, the EE thread's share. Each batch reports against them.
- Water has no fast path. Levels 5, 6, 9, 11, 12, 17, 18, 21, 23, 24 have water; batch 4 reports its share.
- The page compare of the fixed lists on the EE thread (8.1) is paid every frame; the levers are named, not built.
- Whether the emulated GS keeps rasterising under a default `vk` (8.4, section 10): step 10.
- Parallel expansion (several runs at once on worker threads) is not designed: the lanes of 3D design 3.4 and the
  list's order make it a design of its own, and the budget does not ask for it.
- `shapestat`'s fast path covers the track program only; nothing was run on a GPU for this design.
- Carried from step 8: the scenes not reached; the under-water screenshot of 3D design 20.3; the save-state load
  with the live reader (now also through the queue).

## 15. Batch 1 as built (2026-11-22): the lean references

**What was built.** `decomp/vu1` is compiled twice from the same sources. `vu_shape.h` has the one macro: with
`RC_VU1_LEAN` 0 (the default; `rc_vu1_ref`, the game, the oracle) the namespace is `rc::vu1` and the four operations
are as before; with `RC_VU1_LEAN` 1 (`rc_vu1_lean`, a second CMake library over `RC_VU1_REF_SOURCES` with the same
flags) the namespace is `rc::vu1l`, the strict operations are renamed `faddStrict` ... and `vu_lean.h` supplies
`fadd`, `fsub`, `fmul`, `fmadd` (the host's own under RZ + FTZ + DAZ, the strict operation under the strict mode for
the operand classes of 4.1's third column), `ScopedMode` and `kLeanMxcsr`. `rc::vu1l::run` (`vu1_io.cpp`) sets
`MXCSR | 0xE040` over the caller's and restores it on return or a throw (the reader's runaway guard throws through
it). `vu1_ref.h` declares the lean entries in `rc::vu1l`, sharing `Io`, `Program`, `programName` and `isEntry` with
`rc::vu1` (so `rc_vu1_lean` links `rc_vu1_ref` for the last two). `Vif1Reader::Options::library` (`Vu1Library::Strict
| Lean`, `parseVu1Library`) chooses the run at each MSCAL; `rc_replay --vu1 strict|lean` sets it for the `vif`
consumer, whose timing line names the library. `rc_vu1_test` (`tools/rc_replay/vu1_test.cpp`) is check (s).
`shape_lean.cpp` (shapestat) now takes its operations from `vu_lean.h` (included as text under `rc::vu1lean`, so it
cannot meet `rc_vu1_lean` at link time); its `fastTrdr` is unchanged.

**Changes to the design, and why.**

1. **A third MADD fall-back.** `rc_vu1_test`'s edge triples (every triple of 46 edge values) found 30 MADD results
   the two known fall-backs did not catch, none of which the 1,072 M random results of 3.6 had met: an exact
   cancellation `acc = -p` after a product that lost bits. The strict MADD takes the sign, and the flush to zero,
   from the exact sum with the unrounded product (`finish(single, exact)`), so it gives a signed zero (or +0 with
   the negative flag) where the host gives +0; and near the smallest normal, a sum the host keeps can be one the
   exact sum flushes (a test of cancelling triples aimed at it found 1,455 in 25 M once the first was fixed). By
   argument the exact sum's sign can only differ from the single sum's when that sum is zero, and the flush only
   when the product's unit in the last place is below 2^-102, so the sum is below 2^-126 + 2^-102: **a sum below
   2^-101 from a non-zero accumulator goes to the strict MADD** (`vu_lean.h`). Rare on real data: the 67 dumps were
   equal without it (3.6), and the lean build's time did not move (below). **Batch 3 must carry the same test into
   `fastTrdr`'s `madd`** (its `tiny` mask covers only the zero product); `shapestat`'s fast path still lacks it.
2. **The game and the oracle are unchanged in code.** The macro was written so that the strict build's text is
   token for token what it was: all 84 strict objects of `decomp/vu1` in `build-decomp`, `build-eu`, `build-oracle`
   and `build-oracle-eu` are byte-identical to the ones before the batch, and both `rc_oracle` executables are
   byte-identical. The games relinked only because `rc_live` -> `rc_vif` now references `rc_vu1_lean` (the live
   reader stays strict; nothing selects lean there yet).
3. **`--fault mxcsr` cannot fail the lean build**, by design: `vu1l::run` sets round toward zero itself, so a
   caller that forgets the mode is not a fault there. The planted fault keeps its meaning for the strict reader.
4. Dispatch calls in `vu1_io.cpp` are qualified (`RC_VU1_NAMESPACE::runTrdrShapeIo`): in the lean build `Io` is
   `rc::vu1`'s, so an unqualified call would find both builds' entries by argument-dependent lookup.

**Checks.**

| check | result |
|---|---|
| (s) `rc_vu1_test` (default: edge triples, 10^8 random results, 2.5 x 10^7 cancelling results) | **0 of 125,389,344 differ** (2.5 s); with `--results 1000000000 --seed 12345`: 0 of 1,250,389,344; `vu1l::run` sets `0xffc0` over a caller's `0x3f80` and restores it, a non-entry runs nothing |
| (a), (b) lean, `rc_replay <dump> --consumer vif --verify --vu1 lean`, the 67 | **67 / 67 PASS**, 4 minutes four at a time. USA 34: 23,433 VIF1 kicks, 22,517,443 runs, 559,646,028 packets, (b) 23,433 / 23,433 kicks equal to the recorded GIF hash; (a) keyframes 69 / 69 (VIF1 registers, micro and data memory, vector registers), `GSRG` 103 / 103, register file 82,312,258 values at 839,921 kicks, 0 differ. Europe 33: 20,767 kicks, 20,558,762 runs, 486,125,618 packets, (b) 20,767 / 20,767; keyframes 62 / 62, `GSRG` 95 / 95, 74,883,564 values at 764,118 kicks, 0 differ. Guards: none. (The same counts as the strict reader's, 3D design 16.2.) |
| beside the machine, `--with-emu --vu1 lean`, the fourteen (USA and Europe `s5/l5`, `l17`, `l18`, `l20`, `front`, `tedit`, `boot`) | **14 / 14 PASS**: 8,560,917 runs and 238,010,453 packets compared, 0 differ; VU memory and vector registers after every run equal (poisoned lanes excluded); 0 runs with another packet count, 0 cut short; every picture hash and GIF output kick of the substituted machine equal |
| the strict build | objects byte-identical (above). `RC_BOOT_LEVEL=17 RC_VU1_REF=1 RC_VU1_SYNTH=... PS2X_MTVU=0`, 150 s: 2,323,174 runs, 73,323,714 packets, **0 mismatches** (packets, memory, counts); synth TERdr and Rdr_VUShape 0 |
| the game relinked | `playtest2` (`RC_SCENE_LOG`, `RC_DEBUG_DUMP`, 75 s): USA scene 3020 at frame 658, 60.0 fps; Europe 3020 at frame 1167, 50 fps. Oracle `--all` (its executables byte-identical, run as the rule asks): USA seed 9131 1,632 ok, 0 FAIL, 98 without a setup (468 s); Europe seed 9132 1,563 ok, 0 FAIL, 90 without a setup (853 s) |

**The references' time a cycle** (one K2 kick a cycle; `rc_replay <dump> --consumer vif --quiet --vu1 strict|lean`,
single runs one after the other on an otherwise idle machine, Ryzen 7 3750H; USA `s5` dumps but the last):

| scene | strict, mean / largest (ms) | lean, mean / largest (ms) | lean / strict | 3.8's shapestat estimate (lean) |
|---|---|---|---|---|
| front end | 16.34 / 32.33 | 8.66 / 12.22 | 0.53 | 8.4 |
| level 5 | 18.55 / 30.57 | 9.55 / 13.20 | 0.51 | 9.7 |
| level 17 | 18.60 / 26.52 | 9.42 / 15.63 | 0.51 | 9.8 |
| level 18 | 33.56 / 49.69 | 16.80 / 23.14 | 0.50 | 17.4 |
| level 21 | 23.83 / 33.99 | 12.14 / 17.04 | 0.51 | 12.7 |
| Track Editor | 0.42 / 0.59 | 0.27 / 0.31 | 0.64 | 0.3 |
| level 18, Europe | 32.35 / 47.02 | 16.08 / 22.18 | 0.50 | 16.5 |

As expected (3.6: 0.51), and the third MADD fall-back costs nothing measurable against shapestat's lean figures.
The largest kick fell by more than half on the front end (32.3 to 12.2 ms; not looked into). The VIF walk and the GIF walk and list are
untouched by this batch (lean runs: 1.4-2.9 ms and 6-14 ms a cycle on the race scenes); they are batches 2 and 5.

**Open.** `fastTrdr`'s MADD check (item 1) for batch 3. Nothing in the live reader uses the lean library yet: batch
2's `FrameReader::Options::fast` is the first to select it.

## 16. Batch 2 as built (2026-11-22): triangles straight into the list

**What was built.**

- `Gif2dReader::Options::path1Direct` (`render/gs2d/gif2d_reader.*`): with no GIF tag open, `feedPath1` looks at the
  packet's first tag. A **triangle packet** (160 bytes; NLOOP 3, PACKED, NREG 3, no PRE; descriptors ST RGBAQ XYZ2;
  EOP and, without PRE, the PRIM field are ignored because the walker ignores them) goes to
  **`path1Triangle(const uint32_t *nine)`**, which does what the walker, `reg()` and `vertex()` would: per corner ST
  (and the Q for the RGBAQ), RGBAQ, XYZ2 or XYZ3 (ADC) into the register file with stream provenance, the vertex
  with the reference's Q (0.0 reads 1.0), UV from the register, the counters, then `primitive()` once. It declines
  (returns false, nothing changed; the packet goes through the walker) when the vertex queue is not empty, PRIM is
  not a triangle list, the third corner has ADC, or a planted register fault (`gif2d --fault`) is on in the cycle.
  A **16-byte packet with NLOOP 0 and no PRE** (the flush) is dropped. Everything else is walked as before.
- **The state-packet memo** (`Options::stateMemo`): a packet of register writes only (one PACKED tag whose data are
  PRIM, TEX0, CLAMP descriptors, NOP or A+D to a register below 0x50 other than RGBAQ, ST, UV, XYZF2, XYZ2, XYZF3,
  XYZ3) whose bytes equal the last such packet's is skipped when no register was written (`m_regSerial`: every
  `reg()`, a triangle, a guard, a keyframe, seeding) and no byte of GS memory changed (`GsMemory::writes()`, new:
  every changed pixel, `clear`, `load`) since that packet was walked.
- **The CLUT memo** (`Options::clutMemo`), in three layers, each exact by construction:
  1. as designed: a TEX0 write equal to the last TEX0 write, with TEXCLUT and memory unchanged since, does not touch
     the CLUT buffer (a write is idempotent);
  2. **`Clut::writeMemo`** (`render/tex/tex_decode.*`, with `ClutLoadMemo`): `write()`'s CLD decision unchanged, the
     load split into what it reads (a function of CBP, CPSM, CSM, CSA, 4 or 8 bits, TEXCLUT for CSM2, and memory) and
     where it puts it; the values read are kept per palette until memory changes, so each palette is read from the
     shadow once per change of memory instead of at every TEX0 write (the blocks of a kick alternate palettes, so
     layer 1 alone answered a fifth of the loads);
  3. `TextureCache::get(..., clutVersioned)`: the palette compare of an indexed texture (256 lookups and a memcmp at
     every state rebuild) is skipped while the CLUT buffer's new `version()` (bumped by every `load()` and by every
     `writeMemo` that changed an entry) and TEXA are those the entry's palette was made or found equal with.
- **Direct entries** (`Vif1Reader::Options::directEntries`): an MSCAL to TRdr 0x2050, Env 0x2370 or TERdr 0x2198
  copies the packet at TOP (`rc::vu1::copyGifPacket`, through the same runaway cut) without building a
  `ShapeProgram`; registers untouched, poisoned lanes as before.
- **`FrameReader::Options::fast`** (`render/frame/frame_reader.*`): the lean library, the direct entries,
  `path1Direct`, both memos, and no PATH1 hash (D-P2-3: `path1Hash()` stays 0). Off: P1's strict reader, whose code
  path is unchanged but for a counter increment in `reg()` and the cache's new argument left false.
- **The live reader:** `rc::live::Reader(box, live, fast)`; in the game **`RC_VK_READER=fast`** opts in (the
  start-up line names the reader); default strict, as before.
- **`rc_replay <dump> --consumer lists`** (`tools/rc_replay/lists.{h,cpp}`): check (e). The strict and the fast
  reader on the same events, seeded alike: by default from the first keyframe with the chained replay's options;
  **`--from-reset`** for the live dumps (both from reset, no keyframe but one after a state load, the live
  reader's options, as `dlst`). Per kick the register file (values and provenance, all 0x63 registers); per VU1 run
  the six lanes of 3D design 3.4 (and the other 122, reported, not judged); per cycle the two lists as DLST
  encodes them, byte for byte, the first difference named by `dlstFirstDifference` (now shared with `dlst`, which
  also names the first differing command when the counts differ). Faults: `--fault adc|clut-memo|state-memo[:<cycle>]`
  in the fast reader from that cycle. `rc_replay --consumer vif --reader fast` times the fast reader ((b) is not
  available there: no hash).

**Changes to the design, and why.**

1. **The CLUT memo grew two layers** (2 and 3 above). With the memo as designed the GIF walk and list on level 17
   fell only from 9.5 to 7.8 ms: the profile still had a fifth of the process in `GsMemory::read32`,
   `layoutTables`, `Clut::load` (loads of *other* palettes: a kick's texture blocks alternate them, so 'the same
   TEX0 again' answers 550 K of 2.6 M loads) and in `Clut::lookup` / `palette` (the cache's palette check). Layer 2
   took it to 4.6 ms, the copy as two `memcmp` / `memcpy` runs to 3.8, layer 3 is in both. All three are memos in the
   design's sense: they skip work whose result is provably what is already there; (e) is their proof.
2. **The CLUT memo fault cannot fail on `s5/l17`** (nor `l12`, `tedit`): no palette is rewritten in memory while it
   is loaded there. It fails on the front end and the boot dump, where palettes are re-uploaded (below).
3. **The ADC fault fails on the register file, not the list.** No third corner carries ADC (3D design 3.4), so
   ignoring ADC draws nothing extra; it writes XYZ2 where the walker writes XYZ3, which (e)'s register comparison
   catches at the first kick.
4. **`--from-reset` is a flag**, not guessed: the `s5` dumps start at cycle 0 too (booted into the level), so a
   dump's first cycle does not tell a live session from an exact one.
5. Packets recognised as triangles but declined: none in any dump (the queue is always empty and PRIM a triangle
   list at a triangle packet), so `path1Triangle` is the only way K2's triangles enter the fast reader's list.

**Checks.**

| check | result |
|---|---|
| (e) the 67 exact dumps, `--consumer lists --quiet`, four at a time, 10 minutes | **67 / 67 PASS**. USA 34: 24,174 cycles, every list equal byte for byte (27.5 M commands, 15.0 GB encoded), register file equal at all 839,921 kicks, 22,517,443 runs with the six lanes (and all 128) equal, 0 guards, 0 run-count differences. Europe 33: 21,445 cycles equal (22.9 M commands, 13.6 GB), 764,118 kicks, 20,558,762 runs, all equal. In the fast reader: 1,045,771,646 PATH1 packets, **556,240,491 triangles straight into the list** (0 declined), **209,170,126 flushes dropped**, 18.9 M state packets skipped by the memo, 261.4 M other packets walked; 731,918 direct-entry runs; CLUT loads 60.4 M (strict 127.7 M), 55.9 M of them from the load memo, 52.2 M TEX0 writes answered by layer 1, 68.6 M palette checks skipped. (The counts of 3D design 3.5: 556.2 M triangle packets, 209.2 M flushes, 731,918 direct entries.) |
| (e) the live dumps, `--from-reset`, all 17 in `build-*/frame-dumps/live` (both versions: front, l5, l17, l18, the Track Editor, its race, the memory card, l5long, l5hunt, recheck_l17), 5 minutes | **17 / 17 PASS**: 42,150 cycles equal (30.6 M commands), 41,544 VIF1 kicks, register file equal at 706,070 kicks, 20,900,007 runs equal; 277.6 M triangles direct, 106.4 M flushes dropped |
| (e) faults on USA `s5/l17`, from cycle 300 | `adc`: **FAIL at cycle 300 kick 35**, the first VIF1 kick of the cycle: GS register 0x0d (XYZ3) differs (386 kicks in all); lists equal, as item 3 says. `state-memo`: **FAIL at cycle 300**, command 14 (a SCREEN_PRIMS of 0x31 against 0x34 triangles: a state packet skipped though registers it sets had been written since its twin); 400 of 400 cycles differ. `clut-memo`: PASS on `l17` (item 2); from cycle 0 it **FAILs on `s5/front` at cycle 606** (command 27, a TEXTURE payload: a palette re-uploaded since its values were kept; 290 cycles differ) and **on `s5/boot` at cycle 883** (17 cycles); `l12` and `tedit` pass with it |
| `gif2d` (a), the 67, `--consumer gif2d --verify --quiet --no-tables` (the strict reader: K1 and K2 in one register file) | **67 / 67 PASS**: 157,195,822 register values at 1,604,039 kicks, 0 differ, 6 minutes |
| the live reader unchanged unless opted in | the strict path changed only by a counter (`m_regSerial`) and the cache's new argument (false); (e) is strict against fast, and `gif2d` (a) above is the strict reader against the recorded register files. The game relinked (`rc_live`): see below |
| a live session with the fast reader (`RC_VK_READER=fast RC_BOOT_LEVEL=17 LIVE_SHOTS=600,700 tools/live_check.sh fastl17 800`, USA) | `dlst` (the strict reader off-line): **800 / 800 live lists equal byte for byte**; `vk --verify` PASS; the live reader **16.6 ms a cycle** on the EE thread (largest 26.2), against 35.1 for the strict reader in 3D design 20.4 |
| the game relinked (`rc_live`; the oracle executables did not rebuild) | `playtest2` (`RC_SCENE_LOG`, `RC_DEBUG_DUMP`, 75 s, default renderer): USA scene 3020 at frame 658, 60.0 fps; Europe 3020 at frame 1167, 50 fps. Oracle `--all`: USA seed 9221 1,632 ok, 0 FAIL, 98 without a setup (471 s); Europe seed 9222 1,563 ok, 0 FAIL, 90 without a setup (857 s) |

**"GIF walk and list" a cycle** (one K2 kick a cycle; `rc_replay <dump> --consumer vif --quiet --vu1 lean --reader
strict|fast`, single runs one after the other on an otherwise idle machine, Ryzen 7 3750H; USA `s5` dumps but the
last; the vif consumer decodes no textures, as in 3D design 16.3 and section 15):

| scene | before (strict reader), mean / largest (ms) | after (fast reader), mean / largest (ms) | after / before | the fast reader's whole kick (references lean + VIF walk + GIF walk and list), before / after |
|---|---|---|---|---|
| front end | 6.15 / 17.57 | 2.82 / 11.12 | 0.46 | 16.07 / 12.49 |
| level 5 | 9.22 / 14.10 | 3.89 / 8.09 | 0.42 | 20.01 / 15.20 |
| level 17 | 9.57 / 16.50 | 3.91 / 8.06 | 0.41 | 20.66 / 15.19 |
| level 18 | 14.19 / 24.16 | 5.57 / 14.95 | 0.39 | 34.01 / 25.44 |
| level 21 | 8.23 / 14.60 | 4.46 / 10.21 | 0.54 | 22.20 / 18.85 |
| Track Editor | 0.11 / 0.30 | 0.05 / 0.27 | 0.45 | 0.39 / 0.34 |
| level 18, Europe | 13.54 / 23.89 | 5.44 / 15.03 | 0.40 | 32.73 / 25.13 |

Expected a third; measured 0.39 to 0.54 (0.41 on the race scenes but level 21). Two things to know when reading it:
(i) "before" is larger than 3.8's figures (4.5 to 9.4 ms, from 3D design 16.3): step 8 batch 2's CLUT buffer and
its reload at every TEX0 write came after those were measured, and that is exactly the part the memo removes; (ii)
both columns carry two `rdtsc` a packet for the split between references and sink (about 0.5 ms a kick on level 17),
which the fast reader pays as well: the stamps are taken around every packet the references send, flushes
included, before the sink drops them. What is left in the fast
reader's GIF side (profile, level 17): `primitive()` (state compare, the cull's setup, the list append) about a
third, the walker on the texture and state blocks (a quarter of all packets, 43% on level 17) a quarter, the rest
the triangle entry, the memos and the texture cache.

**Open.**

- The GIF side is 0.4 of before, not a third; level 21 0.54. The levers are in `primitive()` (a flag in place of
  the `GsState` compare per triangle; the walker for the texture blocks: their registers could be written by a
  decoder of that one packet shape) and are batch 3's to weigh against the reader's total, which is now the
  references' (lean): 9.4 to 17 ms of 12.5 to 25.4 on the race scenes.
- The CLUT memo fault is proven on the front end and boot dumps, not on `s5/l17` as the design asked (item 2).
- `rc_replay --consumer vif --reader fast` has no check (b) (no hash); its check is (e).
- Batch 3 (the track's fast path) calls `path1Triangle` with its own quadwords; it must decline to the walker in
  the same cases (the packet bytes then have to exist: build the 160-byte packet from the nine quadwords).

## 17. Batch 3 as built (2026-11-22): the track's fast path

**What was built.**

- **`render/shape/`, library `rc_shape`** (on `rc_vu1_lean`; the references' flags; no runtime header, no GIF reader):
  `fast_run.h` (the interface: `FastSink`, `FastStats`, `FastFault`, `FastOptions`, `kTrdrFastNotReproduced`,
  `kMaxRunInput`) and `fast_trdr.cpp`, **`rc::shape::runTrdr`**: `TRdr_VUShapeProg` entry 0 (track cells, cars and
  scenery without the env map, `RDRPOLY`'s and `RDRALPHA`'s buffers: one program) from `shape_lean.cpp`'s `fastTrdr`.
  The run is the reference's step for step: the header packet (247), the constants, the register blocks and texture
  blocks kicked from VU memory, the packet buffer's position `vi11` with its wrap and flush packets. Per triangle the
  SSE path decides culled / all behind / beyond one edge / quick / hard; a hard triangle, or one with a value of 4.1's
  third column anywhere in it, goes to **`rc::vu1l::trdrStep`** (`trdr_shape.cpp`, exported with `trdrLayout()` in
  the lean build only, so the strict objects and the game's are byte-identical to before) on one `ShapeProgram` kept
  for the run (the reader's VU memory and registers, the constants loaded).
- **The quick triangle's packet.** The slot's GIF tag is read from VU memory (`vi11 - 1`). When it is the triangle
  tag with EOP (the packet is then exactly 160 bytes) the nine quadwords go to `FastSink::triangle` ->
  `Path1Sink::triangle` -> `FrameReader::Bridge::triangle` -> **`Gif2dReader::feedPath1Triangle`** (`feedPath1`'s
  first branch: started, `path1Direct`, no tag open, then `path1Triangle`); when the sink declines, the 160-byte
  packet is built (tag + nine) and sent as a packet; when the tag is anything else, the nine quadwords are stored in
  the slot and the packet kicked from VU memory as the program does. The bridge declines when the reader is not
  fast, collects PATH1 bytes, or has a tap that does not say `fastTriangles()` (the `vif --with-emu` tap gets every
  packet; `lists`' taps take the fast route). The flush after a wrap is sent as the 16-byte tag from VU memory when
  it is a tag with EOP and NLOOP 0 (always), and dropped by the sink as in batch 2.
- **MADD**: the path's `madd` has the three fall-backs of `vu_lean.h` (a zero product of two non-zero factors, tested
  on the bits as `vu_lean.h` does; a product of the largest magnitude; **the third: a sum below 2^-101 from a non-zero
  accumulator**), and the tint's `fmadd(acc, tint, 1.0)` goes through it too (it was a plain add in `fastTrdr`).
  `shape_lean.cpp`'s `fastTrdr` has the same now.
- **Registers and poison** (4.2): the path writes back the `ShapeProgram`'s registers (the constants, and whatever the
  hard steps left: VF25.w, VF26, VF29 are written by hard triangles only, so they are the reference's). The lanes the
  program writes for every triangle it reads and the path does not (VF05-VF07 xyz, VF17.w, VF18-VF20) are poisoned
  after the run with the other unreproduced lanes (`kTrdrFastNotReproduced`, in `Vif1Reader::runProgram`).
- **Declines** (4.3): a run whose count walk leaves `kMaxRunInput` = 0x180 quadwords from TOP is the lean
  reference's (none in the dumps: the largest input is 383 quadwords); every run of a program without a fast path
  is counted as declined by program.
- `Vif1Reader::Options::fastPaths` (with the lean library), `FrameReader::Options::fastPaths` (default on under
  `fast`; `RC_VK_READER=fast` gets it), planted faults `FrameReader::Options::fastFault` / `fastFaultCycle`.
- `rc_replay --consumer lists`: `--fault bias|fused|round-x|edge-two|hard-quick[:<cycle>]` and a "fast paths" line
  (runs, declines by program, the outcomes, odd triangles, the largest input). `rc_replay --consumer vif --reader
  fast-nopaths` is batch 2's fast reader, to time the paths against.

**Changes to the design, and why.**

1. **`rc_shape` does not link `rc_gs2d`** (2 said it would): the path's output goes through an interface
   (`FastSink`), so the VIF1 reader adapts its `Path1Sink` and nothing in `render/shape` knows the GIF reader. `rc_vif`
   links `rc_shape`.
2. **The fast path keeps `vi11` and sends the flushes** where the design said it "never makes one": the slot decides
   which tag the packet carries, and the tags are VU memory the stream writes, so the slot is kept exactly; the
   flush is one virtual call of a 16-byte view into VU memory, which the sink drops.
3. **Odd triangles** (a value of 4.1's third column) are decided before the facing test, as `fastTrdr` decided them;
   the third MADD fall-back makes them occur on real data now (exact cancellations to zero in a corner transform: 3,012
   in the 67 dumps, 50,657 in the live dumps). The step decides them, so nothing is lost but time. `shapestat` no
   longer compares the classification of a run with an odd triangle (the classifier may cull what the path hands to
   the step) and counts those runs separately.
4. **Batch 2's open lever, `primitive()`'s state compare, was tried and left out:** a generation counter in place of
   the 128-byte `GsState` compare per triangle saved 0.03 ms a cycle on level 18 (5.50 to 5.47 ms GIF side, within
   noise), so it is not where the GIF side's time is; the cull's set-up (`gsTriSetup`) decides what the list holds and
   stays. Not pursued further in this batch.

**Checks.**

| check | result |
|---|---|
| (e) the 67 exact dumps, `--consumer lists --quiet`, four at a time, 9.5 minutes | **67 / 67 PASS**: 45,619 cycles equal byte for byte (50.4 M commands, 29.3 GB encoded), register file equal at 1,604,039 kicks, 43,076,205 runs with the six lanes equal (other lanes differ after 33.6 M runs: the poisoned ones, not judged), 0 guards, 0 run-count differences. Fast paths: **32,642,628 runs** (every TRdr entry-0 run), 0 declined by the parse test; 9,701,659 declined as having no path yet (Env 5,685,356, skin 2,978,048, water 1,028,995, TERdr 9,260); 1,141,089,355 triangles read: 474,675,452 culled, 275,871,092 all behind, 49,732,287 beyond one edge, **324,857,240 quick, all straight into the list** (0 rebuilt as packets), **15,953,284 hard** (3,012 of them odd); 62.2 M texture blocks. (3.3's counts but for the odd triangles, which the path hands to the step before the facing test: 2,932 more hard than 3.3's 15,950,352, and 1,045 fewer quick, 1,165 fewer culled, 679 fewer all behind, 43 fewer beyond one edge.) |
| (e) the live dumps, `--from-reset`, all 18 in `build-*/frame-dumps/live` (batch 2's 17 and its `fastl17`), 5 minutes | **18 / 18 PASS**: 42,950 cycles equal (31.9 M commands), 42,336 VIF1 kicks, register file equal at 737,224 kicks, 21,612,005 runs with the six lanes equal; 15,585,579 fast runs, 0 declined by parse; 553.3 M triangles read, 156.1 M quick straight into the list, 10.1 M hard (50,657 odd) |
| (e) fast-path faults on USA `s5/l17`, from cycle 300 | each **FAILs at cycle 300**: `bias` (kick 35: the register file, XYZ2 differs; 400 cycles differ), `fused` (command 875, a SCREEN_PRIMS payload byte; 399 cycles), `round-x` (command 36, another triangle count after the cull; 400 cycles), `edge-two` (command 2582, one triangle fewer; 132 cycles), `hard-quick` (kick 35 run 880: VF25.w differs, the hard path's lane; 384 cycles) |
| the lanes | the six lanes equal after every one of the 64.7 M runs above; the extra poisoned lanes never reach a packet (the lists are equal with them poisoned) |
| `shapestat` with the third MADD fall-back | USA `s5/l17`: 2,875,035 quick triangles compared with the reference's packets, **0 differ**; 0 runs with another count or classification; 6 runs with an odd triangle not classified; self-test 0 of 16 M |
| live, `RC_VK_READER=fast RC_BOOT_LEVEL=18 LIVE_SHOTS=600,700 tools/live_check.sh fastl18 800` (USA, RTX 2060, 100 s) | `dlst` (the strict reader off-line, 51.3 ms a cycle there): **800 / 800 live lists equal byte for byte** (1.9 M commands, 913 MB); `vk --verify` **PASS** (925,765 draws, 8,766 barriers); 0 guarded kicks. The live fast reader **18.2 ms a cycle on the EE thread** on level 18 (14.2 to 21.8 by hundred cycles, largest kick 30.2), against 54.7 for the strict reader in 3D design 20.4; level 17's batch 2 figure was 16.6 |
| the strict build | `trdr_shape.cpp`, `vu_shape.cpp`, `env_shape.cpp` compiled before and after with the game's flags: byte-identical objects; both `rc_oracle` executables byte-identical |
| the game relinked (`rc_live`) | `playtest2` (`RC_SCENE_LOG`, `RC_DEBUG_DUMP`, 75 s, default renderer): USA scene 3020 at frame 658, 60 fps; Europe 3020 at frame 1167, 50 fps. Oracle `--all`: USA seed 9331 1,631 ok, 0 FAIL, 1 skipped (`PathBuilder`'s original timed out on a level 17 input: its setup can cycle, NOTES "AMenuDisplayMIT"; the same with `--only PathBuilder`), 98 without a setup (477 s); Europe seed 9332 1,563 ok, 0 FAIL, 90 without a setup (858 s) |

**The reader's time a cycle** (one K2 kick a cycle; `rc_replay <dump> --consumer vif --quiet --vu1 lean --reader
fast-nopaths|fast`, single runs one after the other on an otherwise idle machine, Ryzen 7 3750H; USA `s5` dumps but
the last; mean / largest, ms; "references" now includes the fast path):

| scene | batch 2 (no paths): references, VIF walk, GIF walk and list, **all** | batch 3: references, VIF walk, GIF walk and list, **all** | all, batch 3 / batch 2 | section 10's estimate after 3 | per kick: TRdr runs fast; runs declined; triangles read; hard (odd in the dump) |
|---|---|---|---|---|---|
| front end | 8.53, 1.35, 2.88, **12.77** / 24.16 | 5.03, 1.31, 2.73, **9.07** / 17.94 | 0.71 | 7.9 | 541; 185; 19,257; 393 (3) |
| level 5 | 9.58, 1.47, 3.77, **14.82** / 22.17 | 6.87, 1.42, 3.66, **11.95** / 18.46 | 0.81 | 10.2 | 497; 301; 17,150; 324 (26) |
| level 17 | 9.74, 1.55, 3.88, **15.17** / 21.39 | 6.61, 1.52, 3.86, **11.99** / 17.66 | 0.79 | 9.9 | 585; 266; 20,472; 303 (24) |
| level 18 | 17.33, 2.97, 5.70, **26.00** / 40.71 | 8.59, 2.89, 5.56, **17.04** / 35.87 | 0.66 | 14.1 | 1,412; 241; 50,597; 618 (34) |
| level 21 | 11.88, 2.04, 4.33, **18.25** / 27.09 | 7.11, 2.06, 4.32, **13.49** / 21.54 | 0.74 | 11.5 | 952; 239; 33,155; 502 (19) |
| Track Editor | 0.22, 0.06, 0.05, **0.33** / 0.59 | 0.17, 0.06, 0.05, **0.29** / 0.71 | 0.88 | 0.4 | 11; 7; 364; 91 (0) |
| level 18, Europe | 17.02, 2.99, 5.56, **25.57** / 51.33 | 8.11, 2.84, 5.15, **16.09** / 29.36 | 0.63 | 13.8 | 1,411; 235; 50,588; 616 (0) |

The track's share of the references fell as measured in 3.7 (level 18: 17.3 to 8.6 ms, of which env, skin and
water are about 4.8 lean): what is left of the track is its path, its 618 hard triangles a kick through the lean
step and the texture and register blocks' packets (a profile on level 18: `runTrdr` 15% of the process,
`setupTriangle` and the other reference routines of the hard steps and of the declined programs about 20%,
`primitive()` 9%). Every scene is 2 to 3 ms over section 10's "after 3" column, because the GIF side after batch 2 is
0.4 of before rather than a third (section 16) and the lean env, skin and water are what they were; batch 4 takes the
next 3 to 5 ms.

**Open.**

- Env, skin and TERdr still run lean (batch 4), water by design; they are now most of the references' time.
- The GIF side (2.7 to 5.6 ms) is batch 2's; `primitive()`'s state compare is not the lever (item 4); its cull set-up
  and the walker on the texture and register blocks remain.
- The `vif --with-emu` tap still receives every triangle as a packet (correct, slower); nothing there was rerun.

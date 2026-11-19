# GS usage survey (renderer plan P0 step 1)

Measured 2026-11-19 on both versions (USA SLUS-20153, Europe SLES-50077) with env-gated instrumentation in the
runtime, to answer what the host reader of option C (`RENDERER_PLAN.md`) has to handle. Every number below comes from
the captures in "Captures"; the tables are cut from `tools/gs_survey.py`'s report over all of them. Section numbers
refer to `ICO_RENDER_STUDY.md`.

## Answers in short

| question (study section 5, plan "Open questions") | answer |
|---|---|
| Frame order: 3D of frame N-1 under 2D of frame N? | **Confirmed.** In 208,305 of 208,355 checkable swap cycles K1 (the 2D packet of frame N) draws into the buffer that K2 (the chain of frame N-1) drew into, the flip then shows that buffer, and K2 of frame N draws into the other one. All 50 exceptions are cycles in which the loading splash flipped the display itself between two game frames. |
| Does all GS traffic pass DMA channels 1 and 2? | **Yes.** No PATH2 (VIF DIRECT) packet, no GS register written by a runtime stub, no GIF FIFO store, no VIF0 kick. The only FIFO stores are 2 quadwords into the VIF1 FIFO at start-up (during `GfxP2_Init`'s resets, before `sceGsResetGraph`), which produce no GS data. Movies, logos, texture uploads, the flip's draw environment and its clear all arrive on channel 2; K2 is the only channel 1 kick. |
| Render to texture, frame-buffer reads | **None.** FRAME is only ever FBP 0x0 or 0x50 (the two display buffers), and is written only by the flip's draw environment. No TEX0 samples below block 0x1e00 (frame and Z memory). No local->host or local->local transfer. |
| Mipmaps | **Two textures.** The track lists' TEX1 is 0xf6000000160 (MXL 0, MMAG LINEAR, MMIN LINEAR_MIPMAP_LINEAR, K -10: bilinear from level 0); polygons and 2D use 0x60 (bilinear). Two CT32 textures, 32x32 with MXL 3 and 64x64 with MXL 4 (MIPTBP1/2 set), use real mip levels, on levels 2 and 14 (about 215,000 of 1.85 billion PATH1 primitives). |
| Fog | **None.** No PRIM with FGE, no FOGCOL write, no XYZF2 vertex. |
| Field or frame, per scene | The same everywhere, movies included: SetGsCrt(interlaced, NTSC/PAL, FFMD 1) once; SMODE2 3; PMODE 0x66 (read circuit 2 only); DISPFB2 alternates FBP 0x0 / 0x50 (640 wide, CT32) once per game frame or movie picture; DISPLAY2 2560x480 (USA game) or 2560x512 (USA movies, Europe throughout). Pictures are 640x240 (USA) or 640x256 (Europe) with one flip per frame. |
| Blend cases fixed function can't express | **One that matters:** ALPHA 0x44 `(Cs-Cd)*As+Cd` with PABE 1, the default 3D state (67% of all primitives), is exactly "blend off" while the source alpha is at most 0x80 and extrapolates past Cs above it; 0.016% of those primitives had a vertex alpha above 0x80 (texture alpha not measured: step 5). Every other state fits Vulkan blending with the source term premultiplied in the shader and a dual-source factor (table below). The 2D path's AFAIL RGB_ONLY with Z write on (3.5 M primitives) needs the colour-only second draw. |
| Does raylib audio run without its window? | **Yes**, with one trap. `InitAudioDevice` and a 48 kHz stereo stream (as `ps2_audio.cpp`) with no `InitWindow` played 48,123 frames/s for 3 s, paced by the device. raylib's timer (`GetTime`, `WaitTime`, `GetFrameTime`) needs the window's GLFW timer: without it `WaitTime` spins for ever. The runtime calls `WaitTime` (`ps2_runtime.cpp`, the loop around `PollInputEvents`) and `GetFrameTime` (`ps2_host_input.cpp`), so step 3 replaces those two. |

No finding changes option C. Nothing for the user to decide (see the end).

## Method

### Instrumentation
`third_party/ps2recomp/ps2xRuntime/src/lib/gs/gs_survey.cpp` (header `include/runtime/gs/gs_survey.h`), switched on
by `RC_GS_SURVEY=<file>`; unset, every hook is one test of a false global. Hooks:

| where | records |
|---|---|
| `PS2Memory::writeIORegister`, a DMA channel start (CHCR.STR) | the kick: channel, mode, MADR/QWC or the chain's first TADR, bytes; for chains every tag (ids, call targets by named range) |
| `PS2Memory::submitGifPacket` (every packet from VU1 XGKICK = PATH1, VIF1 DIRECT = PATH2, GIF DMA = PATH3), the native GIF fast paths | the packet, parsed by the survey's own GIF reader (PACKED, REGLIST, IMAGE, tags spanning packets) into register writes and vertex kicks |
| `GS::writeRegister` (used only by runtime stubs) | register writes that bypass the GIF ("EE direct") |
| GS privileged register stores (`write32`, `write64`, `writeIORegister`) | PMODE, SMODE2, DISPFB, DISPLAY, ... with the guest return address |
| VIF0/VIF1/GIF FIFO stores | EE writes into a FIFO |
| `SetGsCrt` | the display mode call |
| the game's `GfxP2_SwapBuffers` override (`overrides/rc_revenge_pro_overrides.cpp`) | a frame boundary with the scene (`mainVarG` mode, level, split screen), the renderer's counters and buffer address ranges (2D buffers, tag array, state/shape/skin/Track Editor/water packet rings) |
| `sceMpegGetPicture` override | the movie flag of the scene |

The survey keeps its own GS register shadow in submission order and, at every primitive, the state in force: a draw
key (PRIM, ALPHA, TEST, FBA, PABE, TEX0 format bits, TEX1, CLAMP modes, FRAME, ZBUF, COLCLAMP, DTHE, TEXA, FOGCOL,
DIMX), a texture key (TEX0, TEX1, CLAMP, MIPTBP, TEXA, TEXCLUT), the FBP drawn into by kick, and the vertex alpha
range. Output: a stream (one `S` line per segment = one swap cycle, from one frame boundary to the next, or 30 fields
without one; per-kick `I` lines for the first 30 cycles of a scene and every 30th after; `P` display writes; `E` first
sightings) and `<file>.agg` (distinct register values, draw states, textures and uploads by scene and path, rewritten
every 3 s). `tools/gs_survey.py <logs>` turns any set of them into the Markdown report the tables below come from.

### Captures
Raw logs stayed out of the repository (450 MB). Each run: `tools/play.sh`'s environment on the RTX 2060 with
`RC_GS_SURVEY=<file>`, `PS2X_MTVU=0` (the VIF1 chain and VU1 run inside the kick, so every record is in exact kick
order) and `RC_TRACK_SHADER=0` (the VU1 programs run, so PATH1 carries the real packets rather than shader batches).
`tools/play.sh` now passes `RC_TRACK_SHADER` and `RC_VU1_REF` through from the environment for this.

| capture | how | USA | Europe |
|---|---|---|---|
| boot | normal start, no pad, 220 s: legal screen and logos (uploads into the frame buffer), the publisher movie, Dolby screen, title, front end, the attract demo (levels 17/21 USA, 5/14 Europe) | 8,551 cycles | 6,532 |
| front3, front5 | `RC_SKIP_INTRO=1`, `tools/front_walk.py` seeds 3 and 5 (deeper 0.55 and 0.3), 160 s: the menus over the 3D front-end world, option and selection pages, a single race on level 2 (USA), a split-screen set-up (Europe, by the scene flag) | 7,347 / 8,842 | 7,577 / 7,439 |
| tedit1-3 | `tools/tedit_walk.py` scripts 1-3 of NOTES.md (TEDIT part 2): editing, Complete/Random Track, help, 2D map, far view; Play Track on the built track (a race with the Track Editor's pieces); Save Track and Load Track (memory card screens) | 8,247 / 11,756 / 8,277 | 6,738 / 9,772 / 6,749 |
| level1-24 | `RC_BOOT_LEVEL=n`, 50 s each: level load with its splash, the demo race, its restart | 1,017-2,662 each | 1,014-2,347 each |
| playtest3 | USA only: `decomp/recordings/playtest3.pad` replayed for 9 minutes; the replay drifted at the first load, so it shows the front end idling into attract demos (levels 4, 5, 9, 10, 14, 16, 17, 21, some split screen) | 30,623 | - |

Scenes seen (`mainVarG` mode): boot (mode 0), front end (1), Track Editor (2), race (3: the front walk's single race,
Play Track), demo race (4), each with the movie flag where the MPEG player ran (the intro movie, and the loading
splash, which is an MPEG too), split screen in the USA attract demo. **Not reached:** the podium (needs a won cup) and the
results screens after a finished race (no capture finished a race: demo races end by timer, the Track Editor's
race ran to the capture's end, the long recording drifted). The credits roll was not reached either. These screens are drawn by the same front-end paths (2D packet, K2 with shapes and skin), so a
short survey run there when a recording reaches them is worth doing, but no new kind of traffic is expected.

## Frame order

Per swap cycle (all captures, both versions):

| check | cycles |
|---|---|
| cycles with a K2 draw | 208,416 |
| K2 drew into exactly one buffer | 208,416 |
| the flip shows the buffer K2 of the previous frame drew into | 208,305 |
| the flip shows another buffer | 50 (all cycles with a loading-splash chain) |
| K2 draws into the buffer not shown | 208,305 |
| K1 drew (some frames' 2D packets are empty) | 158,841 |
| K1 drew over the previous K2's buffer | 158,841 |

One cycle as the kicks arrive (front end, USA, `I` lines of `front3`):

```
K K1   ch2 normal  1,920 bytes from the 2D buffer       PATH3: 2 primitives into FBP 0x0      (2D of frame N)
P DISPFB2 = FBP 0x0                                       the flip: shows 3D N-1 + 2D N
K      ch2 normal    240 bytes (sceGsSwapDBuff)          PATH3: FRAME_1 = FBP 0x50, ZBUF, XYOFFSET, SCISSOR, TEST,
                                                          PRMODECONT, DTHE, COLCLAMP; one sprite: the clear of 0x50
K      ch2 normal  ~20 pairs (96-byte header, 2-16 KB)    animated texture uploads (GfxTex_Update)
K K2   ch1 chain   ~5 MB, ~1,400 tags                     PATH1: ~10,000-37,000 triangles into FBP 0x50 (3D of N)
K      ch2 normal  (some frames) texture construct uploads during the next frame's game code
```

Consequence for the reader: the GS state is one register file shared by all paths, in kick order. K2's triangles
never set FRAME, XYOFFSET, COLCLAMP, DTHE or PRMODECONT: they inherit them from the flip's draw environment on
PATH3; PATH3's 16-bit textures use the TEXA that PATH1 last wrote. The reader must not keep state per path.

## Kicks, buffers and high-water marks

| kick | per swap cycle | largest seen |
|---|---|---|
| K1 (2D buffer, `sceDmaSendN`) | 1 | 136,720 bytes (8,545 quadwords) of the 327,680-byte buffer, Track Editor; 14 KB in races |
| flip draw environment and clear | 1 | 240 bytes |
| texture uploads (ch2 normal, header + pixels) | 23 on average | 1,373 kicks, 4.2 MB in one cycle (level loads) |
| K2 (VIF1 chain) | 1 | 11.7 MB walked (Europe level 18), 6,427 tags in all; tags used of the 1000-tag half: 354 |
| movie / splash (ch2 chain, one IMAGE per 16x16 tile) | 1 per picture | 652,848 bytes (600 tiles, a 640x240 picture); 1,392,688 bytes (1,280 tiles: the 640x512 background clear) |

| ring (game buffer) | size | most call tags into it in one K2 |
|---|---|---|
| frame tag array (per frame) | 1000 | 354 |
| state packets | 256 | 50 |
| shape packets | 360 | 101 |
| skin packets | 384 | 150 |
| Track Editor packets | 384 | 85 |
| water packets | 32 | 4 |

PATH1: every XGKICK is one 160-byte packet (a GIF tag and three ST/RGBAQ/XYZ2 vertices: one triangle). Up to 37,480
triangles, 7.5 MB of PATH1 data and 401,931 register writes in one cycle. The K2 chain is 5-12 MB because it walks the
level's static triangle lists by reference every frame (most of its tags are `ref`/`cnt` inside the called packets).
PATH3 in a race frame carries about 80 KB of texture data (median 80-82 KB, 99th percentile 127 KB), every frame.

## Traffic outside the planned kicks

| event | where |
|---|---|
| 2 EE stores into the VIF1 FIFO | once per run at start-up (`GfxP2_Init`'s resets, before `sceGsResetGraph`); no GS packet follows |
| PATH2 (VIF DIRECT/DIRECTHL) packets | none |
| runtime stubs writing GS registers (`GS::writeRegister`) | none |
| GIF FIFO stores | none |
| VIF0 kicks | none |
| native GIF fast paths (`tryProcessNativeGif*`) | none (the recompiler never emitted `kickGifDmaChainFromMMIO`) |
| local->host or local->local transfers | none |

So K1, K2, the flip's draw environment and the image uploads, all observable at the D1_CHCR/D2_CHCR stores (study
2.6, the preferred attachment), are the complete set. The flip's display registers are 64-bit stores to DISPFB2 /
DISPLAY2 by `sceGsSwapDBuff` (the game) or `sceGsPutDispEnv` (the movie player, from the vertical blank handler);
the reader can take them at the store or from the runtime's privileged register copy.

## Display

| | USA | Europe |
|---|---|---|
| SetGsCrt | interlaced 1, mode 2 (NTSC), FFMD 1 | interlaced 1, mode 3 (PAL), FFMD 1 |
| SMODE2 / PMODE | 3 / 0x66 (EN2 only, MMOD 1, AMOD 1) | same |
| DISPFB2 | FBP 0x0 or 0x50, FBW 10, CT32, at (0, 0) | same |
| DISPLAY2 | DX 636 DY 50, MAGH 4, 2560x480 (game) / 2560x512 (movies) | DX 653-668 DY 56 or 72 (the screen-adjust option moves it), 2560x512 |
| draw environment | XYOFFSET (1728, 1928), SCISSOR 0..639 x 0..239 | XYOFFSET (1728, 1920), SCISSOR 0..639 x 0..255 |
| 3D scissor (state packets) | 3..637 x 3..237; split screen 3..111 and 112..237 | 3..637 x 3..253 (no split screen captured) |

The half-line offset of alternate fields is already removed by the override, so XYOFFSET is constant. Movies are
640x240 pictures in both versions (600 tiles), uploaded into FBP 0x0 / 0x50 and shown through DISPFB2 like game
frames; the 640x512 background clear covers both buffers.

## State values by path

Distinct values written (both versions unless marked). PATH1 values come from the state packets and the VU1
programs' register packets; PATH3 from the 2D packet, the draw environment and the uploads.

| register | PATH1 (VU1) | PATH3 (2D, draw environment) |
|---|---|---|
| PRIM | 0x5b (tri, Gouraud, textured, blended), 0x1b | 0x5b, 0x53, 0x43, 0x1b, 0x13, 0x0b, 0x03 (triangles), 0x01 (lines), 0x06 (sprite: the clear only) |
| PRMODECONT / PRMODE | - | 1 / never written |
| ALPHA_1 | 0x44, 0x01, 0x08, 0x09; FIX forms 0x29 (FIX 0x80, 0x7f), 0x68 (0x80, 0x15), 0x62 (0x15), 0xa1 (0x80), 0x21 (0x80); 0x8000000044 and 0x8000000001 (FIX ignored) | 0x09, 0x01; 0x29 (FIX 0x80, 0x3f), 0x21 (FIX 0x20, 0x3f, 0x40), 0xa1 (0x80) |
| PABE | 1 (with 0x44 and 0x21 only), 0 | 0 |
| TEST_1 | 0x50000, 0x70000, 0x703f7, 0x702f7, 0x502f7, 0x733f7 | 0x703f7, 0x73017, 0x70000, 0x30000 (clear) |
| FBA, DTHE, COLCLAMP | FBA never written | DTHE 0, COLCLAMP 1 (draw environment) |
| TEXA | 0x80 (TA0 0x80, AEM 0, TA1 0) | never written (inherits PATH1's) |
| TEX1_1 | 0xf6000000160, 0x60, 0xf600000016c (MXL 3), 0xf6000000170 (MXL 4) | 0x60; 0xf6000000160 in the Track Editor |
| MIPTBP1/2 | written with every texture; used only with MXL 3/4 | - |
| CLAMP_1 | 0 (repeat), 5 (clamp) | 5 |
| TEX0_1 | see "Textures" | see "Textures" |
| FRAME_1 | never written | 0xa0000, 0xa0050 |
| ZBUF_1 | 0x10000a0 (Z24), 0x1010000a0 (ZMSK) | 0x10000a0; 0xa0 (Z32) in the draw environment |
| SCISSOR_1 | see "Display" | see "Display"; also menu and Track Editor boxes (e.g. 140..500 x 32..150) |
| XYOFFSET_1 | - | see "Display" |
| context 2 registers, SCANMSK, DIMX, FOGCOL, TEXCLUT, TEX2 | never written | never written |

ALPHA/TEST decode: ALPHA `(A-B)*C+D` with A, B, D in {Cs, Cd, 0} and C in {As, Ad, FIX}; TEST alpha tests are
`LEQUAL` against 0x2f, 0x3f or 0x1 (the game's inverted convention: low alpha passes), with fail KEEP or RGB_ONLY; Z
tests GREATER, GEQUAL (the env pass) or ALWAYS (the clear). No DATE.

## Draw states and Vulkan blending

All captures, primitives by path (PATH1 total 1.85 billion, PATH3 10.5 million). "Vulkan": what fixed-function
blending does with it, the source term premultiplied in the shader (so only the destination coefficient matters)
and As as a dual-source factor (`SRC1_ALPHA` = As*255/128, exact while As <= 0x80).

| path | PRIM | ALPHA | TEST | PABE | share | vertex A | Vulkan |
|---|---|---|---|---|---|---|---|
| 1 | tri TME ABE | 0x44 `(Cs-Cd)*As+Cd` | A<=0x2f KEEP, Z> | 1 | 67.0% | 0x00-0xff (204,744 prims > 0x80) | **blend off while As <= 0x80** (PABE writes Cs below 0x80, and at 0x80 the blend gives Cs); As > 0x80 extrapolates: not fixed function |
| 1 | tri TME | - | A<=0x2f KEEP, Z> | 0 | 19.0% | 0x00-0x80 | no blend |
| 1 | tri TME ABE | 0x29 FIX 0x80 `Cd+Cs` | A<=0x2f KEEP, Z>= | 0 | 4.6% | 0x50-0x80 | add, ONE/ONE |
| 1 | tri TME ABE | 0x68 FIX 0x80 `Cs+Cd` | off, Z> | 0 | 1.4% | | add |
| 1 | tri TME ABE | 0x68 FIX 0x15 `Cs*FIX+Cd` | off, Z>= | 0 | 1.4% | | add, src premultiplied |
| 1 | tri TME ABE | 0x08 `Cs*As+Cs` | A<=0x2f KEEP, Z> | 0 | 1.4% | 0x00-0x80 | blend off, shader colour Cs*(1+As) |
| 1 | tri TME ABE | 0x29 FIX 0x80 | A<=0x3f KEEP, Z> | 0 | 1.2% | | add |
| 1 | tri TME ABE | 0x09 `Cd*As+Cs` | off, Z> | 0 | 0.8% | 0x50-0x80 | add, dst factor As |
| 1 | tri TME ABE | 0x62 FIX 0x15 `Cd-Cs*FIX` | off, Z>= | 0 | 0.8% | | reverse subtract |
| 1 | tri TME ABE | 0x01 `(Cd-Cs)*As+Cs` | A<=0x3f **RGB_ONLY**, Z> (ZMSK on) | 0 | 0.8% | 0x00-0x80 | add, dst factor As; with Z masked, RGB_ONLY only spares the alpha channel |
| 1 | tri TME ABE | 0xa1 FIX 0x80 `Cd-Cs` | A<=0x3f KEEP, Z> | 0 | 0.6% | | reverse subtract |
| 1 | tri TME ABE | 0x29 FIX 0x7f | A<=0x2f KEEP, Z> | 0 | 0.6% | | add, constant factor |
| 1 | tri TME ABE | 0x01 | off, Z> | 0 | 0.5% | | add, dst factor As |
| 1 | tri TME ABE | 0x21 FIX 0x80 `Cd` | A<=0x3f KEEP, Z> | 1 | 0.006% | 0x00-0x80 | per-pixel destination factor 0 or 1: dual-source |
| 3 | tri TME ABE | 0x09 | A<=0x3f KEEP, Z> | 0 | 34.1% | 0x00-0xff (3 prims > 0x80) | add, dst factor As |
| 3 | line | - | A<=0x2f KEEP, Z> | 0 | 23.4% | | no blend |
| 3 | tri TME ABE | 0x01 | A<=0x1 **RGB_ONLY**, Z> (Z write on) | 0 | 16.5% | 0x00-0x80 | add, dst factor As; alpha test needs the colour-only second draw |
| 3 | tri ABE | 0x21 FIX 0x20 | A<=0x1 **RGB_ONLY**, Z> (Z write on) | 0 | 17.1% | | add, constant; second draw |
| 3 | tri ABE | 0x21 FIX 0x40 | A<=0x3f KEEP, Z> | 0 | 5.6% | | add, constant |
| 3 | sprite | - | off, Z always | 0 | 2.0% | 0xff | the clear |
| 3 | others (0x21 FIX 0x3f, 0x29, 0xa1, untextured) | | | 0 | 1.4% | | fixed function |

Cases fixed function cannot express exactly, with where they are used:

1. **PABE + ALPHA 0x44 with As > 0x80** (default 3D state of every scene: track, scenery, cars, front-end world).
   0.016% of its primitives had a vertex alpha above 0x80, in every capture (a few hundred per run, 3,000-3,800 in
   the Track Editor). Texture alpha also feeds As (TCC 1, MODULATE) and was not measured; step 5's decode should
   histogram it. Options for step 7/9 (Fable's blend design): treat the state as blend off and log the divergence,
   or route only draws whose vertex alpha exceeds 0x80 through a destination read (ico-pc `rd_dst_read.c`).
2. **AFAIL RGB_ONLY with Z write on** (2D path: menu panels and boxes, 3.5 M primitives in all). Needs a second draw
   for failing pixels with depth and alpha writes off (ico-pc's alpha-test split). PATH1's RGB_ONLY state runs with
   ZMSK set, so there the split only protects the alpha channel, which nothing reads.
3. **COLCLAMP 0** appears only for the 710 primitives drawn before the first draw environment of a run (the survey's
   register shadow starts at zero). The game writes COLCLAMP 1 in every draw environment: not a real case.

Not used at all: DATE, FBA, dithering, fog, AA1, destination alpha as a factor (C = Ad), FIX > 0x80, context 2.

## Textures and CLUT

| format | where | notes |
|---|---|---|
| PSMT4, PSMT8 with a CT32 palette | PATH1 (most track, scenery, cars), PATH3 (fonts, HUD, menus) | always CSM1, CSA 0, CLD 1 (palette loaded with every TEX0); 16x16 (8-bit) or 8x2 (4-bit) palettes |
| PSMCT32 | PATH1 | 32x16 up to 256x128, a few per level; includes the two mipmapped ones |
| PSMCT16 | PATH3 only (128x128, in races) | alpha from TEXA (TA0 0x80, AEM 0), written by PATH1 |
| CT24, T8H, T4HL/HH, CSM2, CSA != 0 | never | |

Sizes 16x8 up to 256x128; TCC 1 and TFX MODULATE always. Distinct (version, TEX0) values with CLD masked: 22,206. No
texture is sampled from frame or Z memory. CLAMP is REPEAT or CLAMP only (no region modes). Palettes are uploaded with
their textures while a level or the front end loads, never alone during play: no palette animation.

## Uploads

All host->local on channel 2:

| what | destination | format, size | when |
|---|---|---|---|
| level texture memory (`BBLoad`) | texture memory from 0x1e00 | CT32 64x64 tiles | level load |
| single textures and their mip levels (`GfxP2_LoadTexMip`) | texture memory | **in their own format**: T8, T4, CT32 at 8x2 ... 128x128 | loads, and texture construction during play (in three captures alone, 547 cycles had uploads between K2 and the next K1) |
| palettes | texture memory | CT32 16x16 / 8x2 | with their textures |
| animated textures (`GfxTex_Update`) | the same blocks again | T8/T4 64x64 and smaller | **every frame**, about 17-23 uploads, 80 KB, after the flip and before K2 |
| logos (`BackdropDisplayBMP24`) | FBP 0x0 / 0x50 | CT32 640x128 halves | boot |
| movie pictures and the loading splash (`ldimage.cpp`) | frame memory, block 0 | CT32 16x16 tiles, one IMAGE each, one chain per picture | movies, every level load |

The reader's shadow texture memory therefore needs the PSMT8 and PSMT4 write swizzles as well as CT32 (study 4.3
assumed the level image only). Uploads into frame memory are image blits, as planned.

## Surprises

- **Animated textures are re-uploaded every frame** (about 80 KB), not every third frame; dirty tracking will mark
  ~20 small textures dirty per frame.
- **PATH1 is one triangle per XGKICK**: 160-byte packets, ~10,000-37,000 a frame. A host reader running the VU
  references produces the same.
- **The K2 chain re-sends the level's static triangle lists every frame** by reference (5-12 MB walked per frame).
  Uploading those lists once (study 4.2, "shape batches") pays off.
- **Shared state across paths** (above): PATH1 draws depend on PATH3's draw environment and PATH3's 16-bit textures
  on PATH1's TEXA.
- The default 3D state is ALPHA 0x44 + PABE, not the 0x21 FIX 0x80 + PABE form the study expected for polygons
  (0x21 with PABE draws 0.004%).
- The draw environment's clear uses Z32 (ZBUF 0xa0) while every draw uses Z24 (0x10000a0) on the same Z buffer.
- Europe's screen-adjust option changes DISPLAY2's DX/DY: the presenter should ignore DISPLAY's position.
- The intro movie and the loading splash flip the display themselves from the vertical blank handler; the splash
  runs at every level load, which is where the frame-order exceptions sit.

## Decisions for the user

None. Every planned kick is the complete set of GS traffic, there is no render-to-texture or frame-buffer feedback,
and the one blend case outside fixed function is rare enough to handle per draw inside step 7/9's design.

## Reproducing

```
RC_GS_SURVEY=/tmp/survey/level5.log RC_TRACK_SHADER=0 PS2X_MTVU=0 RC_BOOT_LEVEL=5 timeout 50 tools/play.sh
tools/gs_survey.py /tmp/survey/*.log > report.md
```
(the front-end and Track Editor runs used `RC_SKIP_INTRO=1 PS2X_PAD_REPLAY=<pad>` with pads from
`tools/front_walk.py` and `tools/tedit_walk.py`; `RC_VERSION=eu` for Europe).

# The 2D packet on Vulkan: state block, blend and alpha rules, tests (renderer plan P1 step 7: design)

Designed 2026-11-20 for `RENDERER_PLAN.md` step 7 (GIF reader for the 2D packet K1, the `gif2d` consumer, Vulkan
drawing of K1). "Build batches" at the end is the work order for the Opus build agent. Read with
`RENDERER_FRAME_RECORD.md` (sections 9, 10.2, 17, 18), `RENDERER_SURVEY.md` and `ICO_RENDER_STUDY.md` 1.6. Paths are
relative to the repository; as built: sections 14 (batch 1), 15, 16, 17 (the review) and 19 (batch 4b, the exact
blend). The runtime is `third_party/ps2recomp/ps2xRuntime` (`RT/`); "the reference" is the
runtime's software GS (`RT/src/lib/gs/gs_cpu_raster.cpp` and its headers), the machine every `REFI` picture comes
from. Nothing here touches a game function or the runtime.

## Decisions for the user

**U1: built (batch 4b, 2026-11-20, section 19)**; the cost figure is open (19.5). **U1, revised by the batch 4
review (2026-11-20, section 17.2): build the exact blend now, as a local read with
barriers, not as the interlock mode described below.** Recommendation, reversible (an option; fixed function stays
as the fallback and for every integer-exact form), and not a change of architecture, so the orchestrator may take
it as it took the first U1; it is listed here because it reverses that decision. Reasons in 17.2, the build
specification in section 18. What changed since the paragraph below was written: the RTX 2060's blender is 8-bit,
so fixed function is -1..+2 a layer, not 0..+1 (15.3); the check it leaves cannot see a one-unit blend fault
(16.3); and the device has `VK_KHR_dynamic_rendering_local_read`, which the vendored RHI already drives
(`RhiLimits.localRead`), so the destination can be read as an input attachment with the fixed-function depth test
kept: no storage images, no Z test in the shader.

**U1 as first put (2026-11-20, superseded). Exact integer blending by destination read: not in step 7
(recommended), or built now.** Fixed-function
blending is exact for RC's add and subtract forms and, with the source bias of 4.3, for the lerps and `Cd*F+Cs`
wherever the source term is at least half a level; elsewhere it can be one level above the GS per blended layer
(never below, never more: 4.4). Making that zero needs the fragment shader to read the destination. The vendored RHI
has it (`RhiDstRead`), but the RTX 2060 offers only the fragment-shader-interlock form (`rhi_vk: destination read:
interlock yes, rasterization-order attachment read no`), in which the target is a storage image and the depth test
runs before the shader; every blended K1 state tests Z GREATER and writes Z under an alpha test, the
combination ico-pc leaves on fixed function for that reason. An exact mode here would have to keep depth in a
second storage image and do the Z test in the shader too. Estimated one Opus batch plus a Fable review, and it
would be opt-in as in ico-pc. Recommendation: build step 7 on fixed function, list the one-level divergence in
DIVERGENCES (P2 step 10), revisit only if a visible case turns up. The design below assumes this; nothing in it
blocks the exact mode later (it would be one more plan of 4.2 and one more pipeline family).

**U2. Two marked edits to the vendored RHI (recommended), or an integer fallback.** The shader of section 6 needs
64-bit floats (`shaderFloat64`, present on the RTX 2060 and the Vega) to compute interpolation weights exactly as the
reference does. The RHI does not enable that feature. `third_party/ico_rhi/VENDORED.md` allows a marked edit ("mark
the edit with an `RC:` comment and list it here"): enable `shaderFloat64` where the device has it
(`vk/vk_device.c`) and report it (`RhiLimits`, `rhi.h`). Without the edits the weights are computed by a 32-bit
integer long division in the shader (6.2, "fallback"), equal to the reference's except where its double rounding
differs (expected on fewer than one weight in 10^8). Recommendation: make the edits; if there is no answer the
build agent makes them, as VENDORED.md provides.

For information, found while designing (no decision needed):

1. **The reference is not an integer GS.** Its blend, alpha test, texture function and coverage are the GS's
   integer rules, but it interpolates colour, texture coordinates and Z in floating point from barycentric weights
   and filters textures with float weights, rounding to nearest. Two measured consequences: a Gouraud primitive
   whose three vertices have one colour is shaded one level low on 5.8% (USA) and 5.3% (Europe) of its pixels
   (2.5 M of 43.0 M and 1.9 M of 35.9 M pixels: `int(c*w0 + c*w1 + c*w2)` with weights that do not sum to exactly 1);
   and every textured K1 pixel mixes texels (the game maps one texel per pixel horizontally with the sample on a
   texel boundary, weight exactly 1/2 in 96% of the pixels, and 2 texels per pixel vertically in the USA version).
   A renderer using the GPU's own interpolation would differ from `REFI` by a level on a few percent of the
   pixels for reasons that are neither side's error. **So in P1 the K1 shader computes the reference's arithmetic
   per pixel itself** (section 6): the comparison of 10.2 row 7(c) is then exact in alpha and Z and bounded only by
   the blend's rounding in colour. GPU interpolation is P3's (resolution scaling), with its own divergence entries.
2. **Whether the hardware does the same in those two places is not known here** (a real GS filters with 4-bit
   weights and truncates; it would shade a one-colour Gouraud primitive exactly). The software GS is the project's
   reference; the differences are at most one level and are listed in section 12 as inherited.
3. **The Z buffer under K1 matters only in the Track Editor**, whose grid and outline lines (23% of PATH3's
   primitives in the survey; in these dumps all in that scene) carry real, small Z values and are tested against the 3D picture.
   Everywhere else K1's smallest Z is above every Z of the picture under it (measured, 3.3). But **Z among K1's own
   primitives matters everywhere**: 92% of them overlap, by bounding box, an earlier K1 primitive drawn at the same
   or a higher Z, and the test is GREATER with Z write on. So depth has to be exact, and test (c) has to seed the
   depth buffer as well as the colour.

## 1. Summary

- **Reader** (`render/gs2d/`): one GS register file across all paths, `GifWalker` (step 5) for the packets,
  primitive assembly, and a draw list of `STATE`, `TEXTURE`, `SCREEN_PRIMS`, `CLEAR` and scope commands (section 2).
  K1 is 1-16 primitives in a race and 129-1024 in the front end and the Track Editor; one drawing kick besides the
  flip's clear in every cycle.
- **What K1 uses** (section 3): triangles (always pairs forming axis-aligned rectangles but for one rotated flare
  quad), lines (Track Editor only), no sprite but the clear; one frame format, one Z buffer, Z test GREATER with
  write on (a few with ZMSK); alpha test LEQUAL against 0x01, 0x2f or 0x3f with AFAIL KEEP or RGB_ONLY; eight blend
  forms; MODULATE with TCC 1, bilinear, CLAMP; nothing of DATE, FBA, FBMSK, dither, fog, AA1, FST, context 2,
  COLCLAMP 0, region clamps, mip levels.
- **Blend** (section 4): three fixed-function configurations, all `ONE` on the source with the source term
  computed in the shader and a dual-source destination factor. Add, subtract and source-only forms are integer
  exact. Lerps and `Cd*F+Cs` are exact where F is 0 or 0x80 or the source term is at least 127/256 of a level (a
  bias of that size turns the blender's round-to-nearest into the GS's floor); otherwise 0 or +1 against the GS.
- **Alpha test** (section 5): a discard for AFAIL KEEP; for RGB_ONLY (and FB_ONLY, ZB_ONLY) with a different
  write for failing pixels, two draws per group of non-overlapping primitives, in primitive order.
- **Depth** (section 7): `D32F`, depth = Z * 2^-24 written from the shader, exact for every Z below 2^24 (all of
  RC's); GS GREATER is `RHI_CMP_GREATER`.
- **Sampling and rasterisation** (sections 6, 8): texels by `texelFetch` with the reference's bilinear arithmetic in
  the shader; triangles through the GPU rasteriser with GS coordinates moved half a pixel and a power-of-two
  viewport (bit-exact coverage); lines expanded to pixels on the CPU with the reference's Bresenham.
- **Tests** (sections 9, 10): a CPU model of the pixel pipeline (`gs_model`), synthetic per-state scenes drawn
  through Vulkan, the model and the emulated GS, and the three checks of 10.2 row 7 with pass criteria: (a) register
  files, (b) re-encoded K1 through the emulated GS bit for bit, (c) Vulkan over the emulated frame and Z buffers:
  alpha and Z equal, colour `0 <= Vulkan - reference <= L(x, y)` with L the number of inexactly blended K1
  primitives over the pixel.

## 2. The draw list for step 7 (extends `RENDERER_FRAME_RECORD.md` section 9)

Container and command header as section 9 defines (32-byte commands, a payload arena, no pointers). New or now
fixed:

| op | a, b, c, d | payload |
|---|---|---|
| `STATE` | a: flags (bit 0: the state is unsupported, see 2.4) | one `GsState` (128 bytes, below) |
| `TEXTURE` | a: texture id (the `TextureCache` entry's index), b: generation, c: width or height << 16 | RGBA8 texels with raw GS alpha, row by row |
| `SCREEN_PRIMS` | a: kind (1 lines, 2 triangles), b: primitive count, c: flags (bit 0: every primitive has one colour; bit 1: consecutive triangle pairs are rectangles), d: first primitive's index in the cycle | `GsVertex` x 2 or 3 per primitive |
| `CLEAR` | a: RGBA, b: Z, c: unused | 2 `GsVertex` (the sprite, as sent) |
| `SCOPE_BEGIN` / `SCOPE_END` | a: tag kind, b: key | the side tag's payload (`Hud`, `Draw`) |

`STATE` always carries the whole block and is emitted only when a field differs from the list's previous `STATE`
(section 9 said "a delta": a front-end cycle changes state under 200 times, 25 KB; a field delta is not worth its
decoder). `TEXTURE` is emitted before the first `STATE` of a list that names an entry whose generation the reader
has not sent yet, so the presenter never touches the cache (17.5 item 7) and uploads incrementally.

### 2.1 `GsState` (the finite state block)

Raw registers as the GS holds them, so the re-encoder of check (b), the CPU model and the Vulkan replay read the same
thing; everything derived comes from one function (2.3). Little-endian, 128 bytes:

```cpp
struct GsState {
    uint64_t frame;      // FRAME_n     FBP 0-8, FBW 16-21, PSM 24-29, FBMSK 32-63
    uint64_t zbuf;       // ZBUF_n      ZBP 0-8, PSM 24-27, ZMSK 32
    uint64_t scissor;    // SCISSOR_n   SCAX0 0-10, SCAX1 16-26, SCAY0 32-42, SCAY1 48-58
    uint64_t xyoffset;   // XYOFFSET_n  OFX 0-15, OFY 32-47 (12.4)
    uint64_t test;       // TEST_n      ATE 0, ATST 1-3, AREF 4-11, AFAIL 12-13, DATE 14, DATM 15, ZTE 16, ZTST 17-18
    uint64_t alpha;      // ALPHA_n     A 0-1, B 2-3, C 4-5, D 6-7, FIX 32-39
    uint64_t tex0;       // TEX0_n      TBP0 0-13, TBW 14-19, PSM 20-25, TW 26-29, TH 30-33, TCC 34, TFX 35-36, CBP 37-50, CPSM 51-54, CSM 55, CSA 56-60, CLD 61-63
    uint64_t tex1;       // TEX1_n      LCM 0, MXL 2-4, MMAG 5, MMIN 6-8, MTBA 9, L 19-20, K 32-43
    uint64_t clamp;      // CLAMP_n     WMS 0-1, WMT 2-3, MINU 4-13, MAXU 14-23, MINV 24-33, MAXV 34-43
    uint64_t miptbp1, miptbp2;
    uint64_t texa;       // TEXA        TA0 0-7, AEM 15, TA1 32-39
    uint16_t prim;       // PRIM (or PRMODE with PRIM's type when PRMODECONT.AC is 0): type 0-2, IIP 3, TME 4, FGE 5, ABE 6, AA1 7, FST 8, CTXT 9, FIX 10
    uint8_t pabe, fba, colclamp, dthe;   // bit 0 of each register
    uint16_t known;      // bit per group below: the reader had the registers from the stream (0: seeded or default, 2.2)
    uint32_t texture;    // TEXTURE id (0xffffffff when TME is 0)
    uint32_t textureGeneration;
    uint8_t texAlphaMin, texAlphaMax;    // the texture's alpha range as decoded (the cache has it); 0, 0 without one
    uint8_t reserved[10];                // zero
};
```
The context registers are the ones of PRIM.CTXT (always context 1 in RC). `tex0`, `tex1`, `clamp`, `miptbp*`, `texa`
are stored as zero when TME is 0, and `alpha` as zero when ABE is 0, so equal draws compare equal.

`GsVertex`, 32 bytes: `uint16_t x, y` (12.4 as written, before XYOFFSET), `uint32_t z`, `uint8_t r, g, b, a`,
`float s, t, q`, `uint16_t u, v`, `uint32_t reserved`. Triangles from strips and fans are stored as separate
triangles in the GS's vertex order; with IIP 0 the colour is the last vertex's.

### 2.2 The reader

`rc::rd::Gif2dReader`, a `GifSink` behind `TexShadow`'s observer (so a texture's upload is applied before the TEX0
that samples it, 17.5 item 5), fed by channel 2 kicks; from step 8 on the same object takes PATH1's packets.

- **One register file** (0x63 entries) for all paths, with a `known` bit per register. A channel 1 kick clears
  `known` for the registers PATH1 writes (step 8 removes this). A draw that needs an unknown register uses, live, the
  defaults of the table in 3.1 ("default") and counts it; in `rc_replay` on an exact dump the file is seeded from
  the kick's recorded `regs` (and that is counted too). K1 depends on two registers PATH3 never writes: TEXA (only
  for the CT16 flare texture) and, for every draw before the first draw environment of a boot, FRAME and XYOFFSET.
- **Primitive assembly** as the GS: PRIM resets the queue; XYZ2/XYZF2 push and draw, XYZ3/XYZF3 (and a PACKED
  vertex with ADC) push without drawing (K1 sends every triangle as XYZ3, XYZ3, XYZ2: 1.06 M such vertices in the
  USA dumps); point 1, line 2, line strip 2 sliding, triangle 3, strip 3 sliding, fan 3 keeping the first, sprite 2.
  RGBAQ, ST and UV are latched per vertex.
- **What it emits**: a sprite whose state is the clear's (no TME, no ABE, ATE 0, ZTST ALWAYS) as `CLEAR`; other
  sprites and points as unsupported (never seen on PATH3); triangles and lines as `SCREEN_PRIMS`, consecutive
  primitives of one kind under one state in one command. Side-tag spans at their stream offsets become scope
  commands.
- **A draw with FRAME.FBW 0** (USA 324 and Europe 272 primitives before a boot's first draw environment, none
  covering a pixel) is dropped and counted.

### 2.3 The plan: everything derived from a state

`GsPlan planState(const GsState &)` in `render/gs2d/gs_plan.{h,cpp}`, the only place that turns registers into
drawing decisions; the Vulkan replay and the CPU model both call it.

```cpp
struct GsPlan {
    uint8_t target;        // 0: FBP 0x0, 1: FBP 0x50
    uint8_t blend;         // BlendConfig: OFF, MIX, MIXSUB (4.2)
    uint8_t term;          // SourceTerm: which source term and destination factor the shader computes (4.2)
    uint8_t factor;        // F: 0 none, 1 As, 2 FIX;  fix: the FIX value
    uint8_t fix;
    uint8_t exactBlend;    // the blend is integer exact for every pixel (4.3)
    uint8_t atst, aref;    // the alpha test as the shader evaluates it; atst ALWAYS when ATE is 0
    uint8_t passColorMask, failColorMask;   // RGBA bits; failColorMask 0 and failDepthWrite 0: a plain discard
    uint8_t passDepthWrite, failDepthWrite;
    uint8_t split;         // failing pixels write something other than passing ones and other than nothing (5.2)
    uint8_t depthCompare;  // NEVER, ALWAYS, GEQUAL, GREATER
    uint8_t tfx, tcc, linear, wrapU, wrapV;
    uint8_t unsupported;   // a reason code (2.4), 0 when drawable
};
```

### 2.4 Guards (states the renderer does not draw as the GS would; each logs once and counts)

| guard | condition | action |
|---|---|---|
| PABE (step 5's precondition) | PABE 1 and ABE 1 and not (ATE 1, ATST LESS or LEQUAL, AREF < 0x80, AFAIL KEEP) | drawn with the blend as if PABE were 0; reason `PABE_BLEND`. Under the precondition the plan is blend `OFF` (17.3) |
| destination alpha | ALPHA C = Ad, or DATE 1 | reason `DEST_ALPHA`; C = Ad drawn with F = 0x80, DATE ignored |
| frame format | FRAME PSM not CT32, FBW not 10, FBP not 0x0/0x50, FBMSK not 0 | FBMSK 0xff000000 / 0x00ffffff map to write masks; anything else `FRAME` and the draw is dropped |
| Z | ZBP not 0xa0; a vertex Z at or above 2^24 | `ZBUF`: dropped; Z clamped to 0xffffff and counted |
| others | FBA 1, DTHE 1, COLCLAMP 0, FGE, AA1, CTXT 1, a destination coefficient of `1+F` (4.2), TEX1 with MXL > 0 on K1, a texture format the cache does not decode | reason codes; drawn without the feature (COLCLAMP 0: clamped) |

None of these occurs in a K1 draw that covers a pixel in the 55 dumps of section 3. Check (c) fails a cycle in
which a guard fired on a covering primitive.

## 3. Measurements: what K1 uses

Tool: `rc_replay <dump> --consumer gif2dstat [--with-emu]` (`tools/rc_replay/gif2d_survey.{h,cpp}`, added with this
design; measurement only). It starts every channel 2 kick from the kick's recorded register file (so PATH1's
writes are in it), walks the stream with `GifWalker`, assembles every primitive and counts; coverage and weights
come from the reference's own `gs_triangle_rules.h`. `tools/gif2d_agg.py <outputs>` sums the per-dump `H` lines.
Run over step 5's exact dumps (`build-decomp/frame-dumps/s5`, 28 dumps, 22,000 cycles; `build-eu/frame-dumps/s5`,
27 dumps, 19,300 cycles: boot with title and attract demo, the front end, levels 1-24 from their load, the Track
Editor). Not in them (as in 17.6): split screen, podium, results, memory card screens, the Track Editor's race;
the survey's captures, which include some of those, show no state outside the classes below.

PATH3 draws in these dumps: USA 645,858 primitives, of them 21,561 clears; Europe 522,959 and 18,921. Every cycle
with drawing has two drawing kicks on channel 2 (the clear's and K1) or one. K1 has 1-16 primitives in 19,646 USA
cycles (races), 17-128 in 14, 129-1024 in 1,924 (front end, Track Editor); none more.

### 3.1 The state block: values seen at a draw

| field | values at a PATH3 draw (USA / Europe primitives) | varies in K1 | default when unknown |
|---|---|---|---|
| FRAME | 0xa0000, 0xa0050 (FBW 10, CT32, FBMSK 0); 0 before the first draw environment (324 / 272, no pixel) | FBP only, once per cycle | none: dropped |
| ZBUF | 0x10000a0 (Z24) 614,775 / 504,256; 0x1010000a0 (ZMSK) 9,780 / 6; 0xa0 (Z32) for the clear | ZMSK | 0x10000a0 |
| SCISSOR | 3..637 x 3..253 (both versions, `GfxP2_DefRenderState`) 453,912 / 504,310; 3..637 x 3..237 (USA 170,709); 0..639 x 0..239 / 0..255 (the clear) | yes | none |
| XYOFFSET | (1728, 1928) USA, (1728, 1920) Europe, no fraction | no | none |
| TEST | 0x703f7 (ATE, LEQUAL 0x3f, KEEP, Z GREATER); 0x73017 (LEQUAL 0x01, **RGB_ONLY**, Z GREATER); 0x702f7 (LEQUAL 0x2f, KEEP: the lines); 0x30000 (the clear: no alpha test, Z ALWAYS) | yes | none |
| ALPHA | 0x09; 0x01; 0x21 with FIX 0x20, 0x3f, 0x40; 0x29 with FIX 0x80; 0xa1 with FIX 0x80 (no pixel covered) | yes | none |
| PABE, FBA | 0, 0 | no | 0 |
| COLCLAMP, DTHE | 1, 0 (0, 0 only with FRAME 0) | no | 1, 0 |
| PRMODECONT | 1 (0 only with FRAME 0) | no | 1 |
| PRIM | 0x5b (tri, IIP, TME, ABE) 507,570 / 412,708; 0x43 (tri, ABE) 33,258 / 28,230; 0x13 (tri, TME) 19,062 / 8,278; 0x53 (tri, TME, ABE) 2,108 / 0; 0x0b (tri, IIP) 62 / 0; 0x03; 0x01 (line) 62,237 / 54,822; 0x06 (the clear) | yes | - |
| PRIM attributes never set | FGE, AA1, FST, CTXT, FIX; no point, strip, fan or other sprite | | |
| TEX0 | T4 128x128 (fonts: 359,154 / 282,420), T8 32x32 (150,224 / 129,564), T8 128x64, CT32 64x64 (USA 9,784, with ZMSK), CT16 128x128 (56 / 96: the one rotated quad), a few small T4/T8; always TCC 1, TFX MODULATE, CSM1, CSA 0 | yes | none |
| TEX1 | 0x60 (429,128 / 334,746); 0xf6000000160 (99,612 / 86,240: MXL 0, MMIN 5, K -10). Both: bilinear from level 0 | two values, one behaviour | 0x60 |
| CLAMP | 5 (CLAMP, CLAMP); 0 on 8 primitives that cover nothing | no | 5 |
| TEXA | 0x80 (TA0 0x80, AEM 0, TA1 0), written by PATH1; matters for the CT16 texture only | no | 0x80 |
| MIPTBP1/2 | not used (MXL 0) | | |

Draw states (USA; Europe has the same but the 0x21 FIX 0x3f / 0x40 and CT32 rows, which its Track Editor dump did not
reach), with the plan of section 4:

| primitives | pixels | PRIM | ALPHA | TEST | ZMSK | texture | what | blend plan |
|---|---|---|---|---|---|---|---|---|
| 359,378 | 22.3 M | 0x5b | 0x09 `Cd*As+Cs` | LEQUAL 0x3f KEEP | 0 | T4, T8 | text | MIX, +1 possible |
| 148,020 | 37.9 M | 0x5b | 0x01 `(Cd-Cs)*As+Cs` | LEQUAL 0x01 RGB_ONLY | 0 | T8 | panels, icons | MIX, +1 possible; split |
| 62,237 | 3.9 M | 0x01 line | - | LEQUAL 0x2f KEEP | 0 | - | Track Editor lines | OFF |
| 31,460 | 38.5 M | 0x43 | 0x21 FIX 0x20 `(Cd-Cs)*FIX+Cs` | LEQUAL 0x01 RGB_ONLY | 0 | - | dimming boxes | MIX, +1 possible; test decided per primitive |
| 21,561 | 3,312 M | 0x06 sprite | - | off, Z ALWAYS | 0 (Z32) | - | the clear | `CLEAR` |
| 9,776 | 19.5 M | 0x13 | - | LEQUAL 0x3f KEEP | 1 | CT32 64x64 | backdrop | OFF |
| 9,286 | 2.2 M | 0x13 | - | LEQUAL 0x3f KEEP | 0 | T8 | | OFF |
| 2,108 | 0.5 M | 0x53 | 0x21 FIX 0x3f | LEQUAL 0x3f KEEP | 0 | T8 | | MIX, +1 possible |
| 1,798 | 0.5 M | 0x43 | 0x21 FIX 0x40 | LEQUAL 0x3f KEEP / 0x01 RGB_ONLY | 0 | - | | MIX, +1 possible |
| 112 | 0.08 M | 0x5b | 0x29 FIX 0x80 `Cd+Cs` | LEQUAL 0x3f KEEP | 0 | CT16, T4 | flare | MIX, exact |
| 56 | 0 | 0x5b | 0xa1 FIX 0x80 `Cd-Cs` | LEQUAL 0x3f KEEP | 0 | T4 | | MIXSUB, exact |
| 386 | 434 | 0x03, 0x0b | - | both tests | 0 | - | | OFF |

### 3.2 Geometry and sampling

| measured | USA | Europe |
|---|---|---|
| triangles that are half of an axis-aligned rectangle, vertices on whole pixels | all but 252 | all but 430 |
| free (rotated) triangles, subpixel vertices | 28 (the CT16 flare quad) | 48 |
| vertices more than 1024 pixels from the window origin (primitives off screen) | 1,192 primitives | 1,760 |
| textured triangles with one colour at all three vertices (but IIP set) | 461,467 | 364,001 |
| textured triangles with Gouraud RGB (alpha equal at the vertices) | 67,245 | 56,937 |
| any triangle with different vertex alphas; any Q other than 1; any FST | 0; 0; 0 | 0; 0; 0 |
| textured primitives with a vertex R, G or B above 0x80 (MODULATE brightens and clamps) | 35,666 | 30,153 |
| vertex alpha above 0x80 | 0 | 0 |
| triangles with different Z at the vertices | 0 | 0 |
| texel coordinates outside 0..size at a vertex | 0 | 0 |
| texels per pixel: u 1; v 1, 2 or between 1 and 2 (the 480-line art on a 240/256-line field) | all rectangles; u 1/2 on 280 | all; u 1/2 on 480 |
| textured pixels whose bilinear weights are both 0 or 1 (a single texel) | 0 of 82.5 M | 2 of 52.4 M |
| u weight exactly 1/2 in the reference's floats | 96.2% of pixels | 94.7% |
| u weight within 1/1000 of 1/2 but not exactly | 3.7% | 4.7% |
| v weight exactly 1/2, exactly another sixteenth, anything else | 72.5%, 6.4%, 20.5% | 65.9%, 7.6%, 25.6% |
| pixels of one-colour Gouraud primitives the reference shades one level low | 2.52 M of 43.0 M | 1.89 M of 35.9 M |
| lines: horizontal, diagonal, vertical; all one colour, subpixel end points, Z varying along the diagonal half of them; 26% cross the scissor | 31,475 / 30,752 / 10 | 27,722 / 27,098 / 0 |

### 3.3 Depth and layers

| measured | result |
|---|---|
| Z of K1 triangles | 0xfd70a3, 0xfdb22d, 0xfdf3b6, 0xfe353f, 0xfe76c8, 0xffffff (text), 0xe66666, 0xf33333 (flare): one Z per primitive |
| Z of the lines | 0x40a to 0xaf4 |
| the clear's Z | 1, in every cycle |
| largest Z in the buffer before K1 (emulated GS, `--with-emu`; USA front end, boot, levels 5, 17, 18, Track Editor: 4,325 K1 kicks) | below 0x10000 in 4,246, below 0x800000 in the other 79 |
| K1 kicks whose smallest Z is above every Z under them | all 3,625 outside the Track Editor; none of the Track Editor's 700 (its lines) |
| Z-tested K1 primitives that overlap, by bounding box, an earlier K1 primitive that may have written a Z at or above theirs | USA 512,481 of 557,979; Europe 418,194 of 446,390 |
| inexactly blended K1 layers over one pixel in a cycle (bounding the blend error, 4.4) | 1: 28.7%, 2: 51.7%, 3: 16.9%, 4: 2.5%, 5-6: 0.2%, 7 or more: 793 pixels (USA, of 51.5 M blended pixel-cycles); deepest pixel of a cycle: 0 layers in 9,918 cycles, 2 in 9,763, 5-8 in 1,923, never more |
| **as built (batch 3, check (c), 16.2):** L, the inexactly blended primitives of one kick covering a pixel (the reference's coverage, not the box), over all 67 exact dumps | USA: 1: 19.2 M, 2: 37.6 M, 3: 12.2 M, 4: 1.79 M, 5: 109 K, 6: 29 K, 7: 825 pixel-kicks; Europe: 17.5 M, 27.2 M, 10.3 M, 1.89 M, 135 K, 37 K, 617. **Largest L 7** in both versions (Track Editor and a USA front-end key dump); 0 to 3 on 96.9% of the pixel-kicks with L > 0 |
| **as built:** Vulkan - reference per colour channel over the pixels with L > 0, by the state of the last inexact primitive over the pixel (both versions; bias off / bias on) | text `0x09`: +1 4.55 M / -1 5.76 M, +1 0.07 M; panels `0x01` lerp As: -1 0.02 M, +1 21.05 M, +2 0.013 M / -2 129, -1 6.42 M, +1 5.91 M; dimming boxes `0x21` FIX 0x20: +1 66.3 M / -1 13.4 M, +1 50.3 M; `0x21` FIX 0x3f (USA): +1 1.03 M, +2 0.013 M / -1 0.06 M, +1 0.36 M. Every other pixel (L = 0: 14.36 G pixel-kicks) equal in all four channels and Z, with either setting. Section 16.2 |
| **as built (batch 4b, 19.3):** Vulkan - reference with the exact blend (read mode), over all 67 exact dumps | 0 in every channel of every pixel, alpha and Z included (88,421 kicks, 14.49 G pixel-kicks); L no longer bounds anything. Reading primitives 1,169,006 in 555,457 groups, 534,278 barriers; most in one kick 119 |

## 4. Blending

### 4.1 The GS rule

`Cv = clamp(((A - B) * C >> 7) + D, 0, 255)` per colour channel, with A, B, D each Cs, Cd or 0, C one of As, Ad or
FIX, an arithmetic shift (floor), COLCLAMP 1; the stored alpha is As (never blended); with ABE 0, `Cv = Cs`. Cs
and As are the outputs of the texture function (section 6). The largest As or FIX of any RC draw is 0x80 (17.3), so
`F = C/128` lies in 0..1.

### 4.2 Fixed-function configurations

Write the rule as `x = ks*Cs + kd*Cd` with the coefficients the register selects (`ks`, `kd` each one of 0, 1, F,
1-F, -F). The source term `a = |ks|*Cs` is computed in the fragment shader in units of a level (a multiple of 1/128),
the destination coefficient goes out as the second colour output, and the pipeline has `srcColor = ONE`,
`dstColor = SRC1_COLOR` always; alpha is never blended (`srcAlpha = ONE`, `dstAlpha = ZERO`). Three configurations:

| config | RHI blend | used when |
|---|---|---|
| `OFF` | disabled | ABE 0; `kd = 0`; PABE under its precondition |
| `MIX` | ADD: `out0 + Cd * out1` | `ks >= 0`, `kd >= 0` |
| `MIXSUB` | REVERSE_SUBTRACT: `Cd * out1 - out0` | `ks <= 0`, `kd >= 0` |

`ks >= 0` with `kd < 0` (`Cs*F - Cd*F`, ALPHA 0x06 and its kin) would be a third, SUBTRACT; `kd = 1 + F` and C = Ad
are not expressible. No RC draw uses any of them (guards, 2.4). Dual-source blending is required
(`RhiLimits.dualSourceBlend`, a Vulkan 1.0 feature every desktop device has); without it the renderer refuses to
start rather than draw wrongly.

RC's values, both paths (step 9 reuses the table for K2):

| ALPHA | GS rule | config | shader's `a` (levels) | out1 | exact? |
|---|---|---|---|---|---|
| 0x29, 0x68 with FIX 0x80 | `Cd + Cs` | MIX | `Cs` | 1 | yes |
| 0x68 FIX f | `(Cs*f >> 7) + Cd` | MIX | `(Cs*f) >> 7` (integer) | 1 | yes |
| 0xa1 FIX 0x80 | `Cd - Cs` | MIXSUB | `Cs` | 1 | yes |
| 0x62 FIX f | `(-Cs*f >> 7) + Cd` | MIXSUB | `-((-Cs*f) >> 7)` = ceil(Cs*f/128) (integer) | 1 | yes |
| 0x08 | `(Cs*As >> 7) + Cs` | OFF | `min(((Cs*As) >> 7) + Cs, 255)` | - | yes |
| 0x44 with PABE, 0x21 FIX 0x80 with PABE (under their alpha tests) | Cs where the test passes | OFF | `Cs` | - | yes (17.3) |
| 0x09; 0x29 FIX f < 0x80 | `(Cd*F >> 7) + Cs` | MIX | `Cs`, biased (4.3) | F/128 | where F is 0 or 0x80 or `Cs >= 1`; else 0 or +1 |
| 0x01; 0x21 FIX f | `((Cd-Cs)*F >> 7) + Cs` | MIX | `Cs*(128-F)/128`, biased | F/128 | where F is 0 or 0x80 or `Cs*(128-F) >= 64`; else 0 or +1 |
| 0x44 without PABE (not seen) | `((Cs-Cd)*As >> 7) + Cd` | MIX | `Cs*As/128`, biased | (128-As)/128 | as the line above with the roles swapped |
| 0xa1 FIX f < 0x80 (in the sources, not seen) | `(Cd-Cs)*f >> 7` | MIXSUB | `Cs*f/128 + 127/256` | f/128 | yes (the bias is always available on the subtracted side) |

F is As (computed per pixel) or FIX; the shader gets `term`, `factor` and `fix` as uniforms. `out0 = vec4(a / 255,
As / 255)`; `out1 = vec4(vec3(f1), 1)`.

### 4.3 Where fixed function is integer exact, and the source bias

The blender computes `x' = out0*255 + Cd*out1` in floating point and stores `round(x')`, clamped. With an integer
`a` and `out1` of 0 or 1, `x'` is an integer below 2^9: exact (the adds, subtracts and source-only forms above).

Where `out1 = F/128` with 0 < F < 0x80, the exact value `x = a + Cd*F/128` has a fraction that is a multiple of
1/128, the GS stores `floor(x)` and the blender `round(x)`: one level more whenever the fraction is above 1/2
(37% of all (Cs, Cd, F) for `Cd*F+Cs`, 49% for the lerps, counted exhaustively), and either on an exact half
(0.8% to 50% of the cases depending on F; the tie is decided by float noise, so the model may not predict it).

**The bias.** Let `b = 127/256` of a level. If the shader outputs `a - b` in place of `a`, the blender stores
`round(x - b)`, and `x - b = floor(x) + (2k - 127)/256` for a fraction `k/128`: strictly between -1/2 and +1/2 of
`floor(x)`, never a tie, with a margin of 1/256 level (1.5e-5 of full scale, against a float32 blender's 6e-8). So
`round(x - b) = floor(x)`: **the GS's result exactly**, clamp included. Colour outputs are clamped to 0..1 before
blending, so the bias is available only where `a >= b`; where `a < b` (a source term below half a level: Cs 0, or
a small Cs under a large F) the shader outputs `a` unbiased and the result is `floor(x)` or `floor(x) + 1`. For
`MIXSUB` with a fractional factor the bias is added to the subtracted source and is always available. Exhaustive
over all Cs, Cd and F in 0..0x80: with the bias `Cd*F+Cs` is exact on 99.6% of the triples and one above on 0.18%
(it was 37%), the lerps exact on 98.0% and one above on 0.6%. Real pictures have more zero channels than that,
hence the bound of 4.4 rather than a claim of exactness.

The bias assumes a blender with more than 17 bits of precision. Test T1 (9.2) proves it on the device, over every
(Cs, Cd, F); where T1 fails with the bias and passes without it, the bias is switched off (a uniform) and only the
bound of 4.4 holds.

Float against integer, the other points the plan asks about: C = 0x80 against 1.0: `F/128` is exact in binary
floating point for every F, and 0x80 gives exactly 1.0; As above 0x80 would clamp at 1.0 where the GS extrapolates,
which no draw reaches (17.3). COLCLAMP 1 is the blender's own clamp to 0..1; COLCLAMP 0 (wrap) cannot be done and
is guarded. Alpha is written as As (0..0x80 raw, 0xff from the clear) through an `RGBA8_UNORM` target: `As/255`
converts back to As exactly.

### 4.4 The bound for check (c)

For one inexactly blended layer the stored value is the GS's or one above. A later layer multiplies an existing
excess `e` of Cd by its `kd` (0..1, never negative in the forms used) and adds at most one more, so after n such
layers the excess is at most n, and never negative; exact layers pass an excess on unchanged (add, subtract) or
remove it (blend off). Neither the alpha test nor the Z test reads the frame buffer (no DATE, no Ad), so the
excess never changes which pixels are written. Hence, per colour channel and pixel:

**`0 <= Vulkan - reference <= L(x, y)`**, where `L` is the number of K1 primitives with a plan that is not
`exactBlend` whose scissored bounding box contains the pixel (measured: 0 to 3 on 97% of blended pixels, at most 8
in a cycle, 3.3). Alpha and Z have no tolerance. A primitive with a flat source (untextured, one colour) and a
fixed F is exact whenever `a >= b` on all three channels: `planState` can then set `exactBlend` and take it out of
L (the dimming boxes, 0x21 FIX 0x20, are exact unless a channel of their colour is 0).

**As built (batch 3): the bound measured on the device.** The RTX 2060's blender takes the source and the
dual-source factor at 8 bits (15.3), so a layer is -1..+2 against the GS without the bias and -1..+1 with it, and a
flat source is not exact (the dimming boxes differ on 66 M channels): check (c) uses `-L <= Vulkan - reference <= 2L`
(bias off) or `-L <= ... <= L` (bias on), L counting every inexact plan (no flat-source exemption), and holds on all
67 dumps with either setting (16.2). Divergence D-2D-1.

## 5. The alpha test

### 5.1 The GS rule (order as in the reference's `ShadePixel`)

Scissor; alpha test on As (ATE, ATST, AREF) giving a write mask; Z test (ZTE, ZTST) on every pixel, passing or
failing the alpha test alike; then: test passed: RGB, A and Z (unless ZMSK) are written; failed, by AFAIL: KEEP
nothing; FB_ONLY RGB and A; ZB_ONLY Z; RGB_ONLY RGB (on a CT32 frame; on a 16-bit frame RGB and A).

RC uses ATST LEQUAL only, AREF 0x01, 0x2f, 0x3f, AFAIL KEEP and RGB_ONLY; ATE 0 for the clear. FB_ONLY, ZB_ONLY,
the other ATST values and DATE never occur; the shader and the plan handle all eight ATST values and all four AFAIL
values all the same (they cost nothing and the synthetic tests cover them).

### 5.2 Drawing it

- **Failing pixels write nothing** (KEEP; ZB_ONLY with ZMSK set): one draw, `if (!alphaPass(As)) discard;`. A
  discarded fragment writes no colour, alpha or depth, and the depth test of the other fragments is untouched: the
  GS's result.
- **Failing pixels write what passing ones do** (FB_ONLY with ZMSK set): one draw, no test.
- **RGB_ONLY with ZMSK set** (PATH1's state, step 9): one draw, no discard; the pipeline's alpha blend becomes
  `srcAlpha = SRC1_ALPHA`, `dstAlpha = ONE_MINUS_SRC1_ALPHA` with `out1.a` = 1 where the test passes and 0 where it
  fails, so the stored alpha is As or the old alpha: exact, since the factors are 0 and 1.
- **The split** (`plan.split`: RGB_ONLY or FB_ONLY with Z write on, ZB_ONLY with colour written on a pass): the
  same primitives twice,
  1. the passing part: `discard` where the test fails; colour mask RGBA, depth write as ZMSK says;
  2. the failing part: `discard` where the test passes; colour mask `failColorMask` (RGB for RGB_ONLY, RGBA for
     FB_ONLY, none for ZB_ONLY), depth write `failDepthWrite` (on only for ZB_ONLY);
  both with the state's depth test and blend.
- **When As is one value for the whole primitive** (untextured with equal vertex alphas: all of K1's untextured
  blended primitives; or a texture whose alpha range times the vertex alpha falls on one side of AREF,
  `texAlphaMin/Max`), the test is decided on the CPU and the primitive drawn once with the passing or the failing
  masks. This is exact and removes the split for the dimming boxes (31,460 of the 179,480 RGB_ONLY primitives).

**Why the split is equivalent, and its order.** As is a function of the pixel and the primitive only, computed by
the same shader code in both draws from the same inputs (integer and `precise` arithmetic, section 6), so each
pixel of a primitive belongs to exactly one part. Within one primitive the two parts touch disjoint pixels, so
neither sees the other's writes and each pixel gets exactly the writes and the depth test the GS gives it. Across
primitives the GS's order matters (blending is not commutative, and a passing pixel's Z write decides whether a
later primitive draws there), so the two draws are issued **per group of primitives that cannot overlap**, not per
batch: group = a maximal run of consecutive primitives whose pixel sets are pairwise disjoint, taken
conservatively as: the two triangles of a rectangle (they share only the diagonal, which the fill rule gives to
one), plus following rectangles whose scissored boxes are disjoint from every box already in the group. For a
group, passing part then failing part equals the GS's sequence; then the next group. K1 has at most about 90 such
rectangles in a front-end cycle: under 200 draws.

Test T3 (9.2) draws overlapping rectangles in one batch precisely to catch a split issued per batch.

## 6. The fragment shader: the reference's arithmetic per pixel

One fragment shader for K1 (`render/gs2d/gs2d.frag`), in P1 at native resolution only. It gets its primitive's three
vertices as data and computes every interpolated quantity itself, in the reference's operation order, so that
nothing depends on the GPU's interpolators. All float code is declared `precise` (SPIR-V NoContraction); the
project builds the reference with `-ffp-contract=off` and no fast-math, so each C++ expression below is one IEEE
operation per operator, left to right.

### 6.1 Primitive data

Per triangle, written by the replay into a storage buffer the fragment shader indexes with a `flat` integer vertex
attribute (if fragment-stage storage buffers give trouble in the RHI: the same record as `flat` vertex attributes,
repeated on the three vertices):

```
ivec2 p[3];      // window coordinates in 1/16 pixel: X - OFX, Y - OFY
uint  rgba[3];   // vertex colours; with IIP 0 all three hold the last vertex's
vec3  stq[3];    // S, T, Q as sent
uint  z;         // one Z (the reader asserts the three are equal; otherwise the record holds three and 7.1 applies)
uvec2 invArea;   // the bits of the double 1.0 / double(area), computed on the CPU as the reference does
int   a[3], b[3], c[3]; int bias[3];   // the reference's edge functions (gs_triangle_rules.h, gsTriangleSetup), area made positive
```
The vertex shader only places the vertex: `gl_Position.xy = (vec2(p) / 16.0 + 0.5) / 2048.0` (section 8), with y
negated as the RHI's y-up clip space requires (`VENDORED.md`, "Clip space is y-up").

### 6.2 Weights

```glsl
ivec2 px = ivec2(gl_FragCoord.xy);                    // the GS's integer sample point
double e_i = double(a[i]) * double(px.x * 16) + double(b[i]) * double(px.y * 16) + double(c[i]);   // exact: integers below 2^53
if (e_i + bias[i] <= 0) discard;                      // the reference's coverage test (a guard: the rasteriser already agrees, section 8)
float w_i = float(e_i * packDouble2x32(invArea));     // gsTriangleWeight: float(double(e) * invArea)
```
Double multiplication and the conversion to float are correctly rounded in SPIR-V; T7 (9.2) compares the GPU's
weights with the CPU's over whole triangles to prove it on the device.

*Fallback without `shaderFloat64` (decision U2):* the edge values as 32-bit integers relative to a vertex (valid
while every window coordinate is within +-1024 pixels; primitives outside are clipped to that on the CPU), and
`w_i` = the float nearest to `e_i / area` by a restoring division of at most 56 steps with round-to-nearest-even.
This differs from the reference only where its two roundings (to double, then to float) disagree with one.

### 6.3 Colour, texture coordinates, texel, texture function

```glsl
// gs_cpu_raster.cpp, DrawTriangleFast
r = clamp(int(float(r0) * w0 + float(r1) * w1 + float(r2) * w2), 0, 255);   // each of r, g, b, a; truncation
is = s0 * w0 + s1 * w1 + s2 * w2;   it = ...;   iq = q0 * w0 + q1 * w1 + q2 * w2;
invQ = 1.0 / (abs(iq) > 1.0e-8 ? iq : 1.0);
uf = is * invQ * float(texW);   vf = it * invQ * float(texH);              // texW = 1 << TW, texH = 1 << TH
```
With IIP 0 the colour is the stored one, no arithmetic. A one-colour Gouraud primitive is *not* special-cased: the
reference shades it one level low on some pixels and so does this shader (decision note 1). The float division is
within 2.5 ULP by the Vulkan specification, not necessarily exact; T7 checks it, and if a device fails,
`invQ = float(1.0lf / double(...))` is used (a double division's error is far below a float's rounding step).

Texel, MMAG LINEAR (every K1 draw; the rule the reference uses: linear when MMAG is 1 or MMIN is 1 or 4-5, always
from level 0):
```glsl
su = uf - 0.5;  iu = int(floor(su));  fu = su - float(iu);   // likewise sv, iv, fv
c00 = texel(wrapU(iu), wrapV(iv)); c10 = texel(wrapU(iu + 1), wrapV(iv)); c01 = ...(iv + 1); c11 = ...;
// per channel, alpha included (gs_cpu_raster_inline.h, LerpChannel / LerpVector):
top = float(c00) + (float(c10) - float(c00)) * fu;   bot = float(c01) + (float(c11) - float(c01)) * fu;
t = top + (bot - top) * fv;   Ct = clamp(exactLround(t), 0, 255);
```
`exactLround` is the reference's: truncate, then one more when the remaining fraction is 0.5 or above (round half
up for these non-negative values); written out with a truncation, a subtraction and a comparison, not as `round()`.
MMAG NEAREST: `texel(wrapU(int(uf)), wrapV(int(vf)))`. `texel` is `texelFetch` on the `RGBA8_UNORM` texture the
`TEXTURE` command uploaded, as integers (`uint(v * 255.0 + 0.5)`; the texels are step 5's decode: palette applied,
TEXA applied, raw alpha). No sampler object is involved, so no texel-centre convention of the GPU's: **the
half-texel convention is the reference's**, a sample at the integer pixel coordinate and a filter footprint centred
on `uf - 0.5`.

Wrap modes (CLAMP register; `wrapTextureCoordinate`): REPEAT `i & (size - 1)`; CLAMP `clamp(i, 0, size - 1)`;
REGION_CLAMP `clamp(i, MINU, MAXU)`; REGION_REPEAT `(i & MINU) | MAXU`. K1 uses CLAMP on both axes; its texel
coordinates stay inside the texture at the vertices, but the filter reads texel -1 and `size` at the borders, where
the clamp decides.

Texture function (`combineTexture`; integers), giving the Cs and As of sections 4 and 5:

| TFX | RGB | A with TCC 1 | A with TCC 0 |
|---|---|---|---|
| 0 MODULATE | `min((Ct*Cv) >> 7, 255)` | `min((At*Av) >> 7, 255)` | Av |
| 1 DECAL | Ct | At | Av |
| 2 HIGHLIGHT | `min(((Ct*Cv) >> 7) + Av, 255)` | `min(At + Av, 255)` | Av |
| 3 HIGHLIGHT2 | `min(((Ct*Cv) >> 7) + Av, 255)` | At | Av |

Without TME: Cs = Cv, As = Av. RC uses MODULATE with TCC 1 only. Then the alpha test (section 5), the source term
and outputs (section 4), and `gl_FragDepth` (section 7).

## 7. Depth

### 7.1 The rule and the mapping

RC's Z buffer is one buffer at ZBP 0xa0, shared by both frame buffers; draws use it as Z24 (ZBUF 0x10000a0), the
flip's clear as Z32 (0xa0) with Z 1. ZTST is GREATER for every K1 draw (GEQUAL for PATH1's env pass, ALWAYS for the
clear; NEVER unused); the test is `Z > stored` on integers, and a written Z replaces the stored one. Every Z of
every PATH3 draw is below 2^24 (largest 0xffffff).

Vulkan: one `RHI_FMT_D32F` texture, 640x256, used with whichever colour target FRAME selects. The fragment shader
writes `gl_FragDepth = float(Z) * (1.0 / 16777216.0)`: an integer below 2^24 is exact in float32, scaling by a
power of two is exact, a float depth buffer stores it unconverted, and the fixed-function comparison of two such
floats is the integer comparison. So **GS GREATER is `RHI_CMP_GREATER`, GEQUAL `RHI_CMP_GEQUAL`, exactly**, with no
bound to state (a `D24` buffer would not be: `Z / (2^24 - 1)` does not survive the float round trip at the top of
the range). A Z at or above 2^24 is clamped and counted (2.4). The clear writes depth 1 * 2^-24 through the same
path.

K1's triangles have one Z each (3.2), so the shader takes Z from the primitive record. The general case, for step
8 to note: the reference interpolates `z = z0 + (z1 - z0) * double(w1) + (z2 - z0) * double(w2)` in doubles and
stores `uint(clamp(z + 0.5, 0, 4294967295))`; the same three lines work in this shader. Lines: 8.3.

Writing `gl_FragDepth` moves the depth test after the shader, which is what the discards need anyway.

### 7.2 Frame order: K1 over K2

Within a display cycle everything draws into one frame buffer and the one Z buffer, in kick order: the flip's
clear (colour and Z), K2 (3D of frame N), K1 (2D of frame N+1), then the flip shows it (`RENDERER_FRAME_RECORD.md`
section 1; survey "Frame order"). The Vulkan replay therefore draws a cycle's commands in list order into
`target[FBP]` with the shared depth texture and never clears depth between K2 and K1. Consequences:

- K1's triangles (Z 0xe66666 and up) always pass against the 3D picture (Z below 0x800000, mostly below 0x10000),
  and are hidden only by earlier K1 primitives. The Track Editor's lines (Z about 0x400-0xb00) are really sorted
  against the editor's 3D pieces: until step 8 draws K2 on Vulkan, the live renderer shows them unoccluded.
- Until step 8, the live picture is the clear plus K1 (and step 6's uploads): menus over black.
- Check (c) cannot wait for step 8: it seeds the colour target and the depth texture from the emulated GS's memory
  as they are just before the K1 kick (10.3).

## 8. Rasterisation

### 8.1 Triangles

The reference covers pixel (i, j) when the point (i, j), in window coordinates `(X - OFX) / 16`, is inside the
triangle; a point exactly on an edge counts when the edge is a left edge or a horizontal top edge (`bias` in
`gsTriangleSetup`: `a > 0 || (a == 0 && b > 0)` with y down). That is the top-left rule with integer sample points.
Vulkan samples at pixel centres, (i + 0.5, j + 0.5), with the same rule on every desktop driver (the specification
only demands that a shared edge is drawn once; T5 verifies the rule on the device). So:

- **Position:** framebuffer coordinates `(X - OFX) / 16 + 0.5`, likewise y. The triangle is moved half a pixel
  with its sample points; coverage is identical.
- **Exact vertex positions:** the viewport is `x = -2048, y = -2048, width = 4096, height = 4096` (allowed: a
  viewport may exceed the target; `viewportBoundsRange` covers it), so `ndc = framebuffer / 2048` and back is exact
  in float32 for multiples of 1/32 pixel, and the device's 8 subpixel bits hold 1/16 + 1/2 exactly. Every window
  coordinate within +-2048 pixels is inside the viewport, so no primitive is clipped in x or y (clipping would
  re-derive vertices and could move an edge by a subpixel); the handful of K1 primitives that reach further are
  off screen entirely and are dropped on the CPU when their scissored box is empty, clipped on the CPU otherwise
  (never seen). If the RHI's `rhi_cmd_set_viewport` does not pass a negative origin through, the viewport is 0, 0,
  4096, 4096 with `ndc = framebuffer / 2048 - 1` (as exact) and primitives with a negative window coordinate are
  clipped on the CPU. Z is not used for clipping: the vertex shader outputs a constant `gl_Position.z = 0.5`, `w = 1`.
- **Scissor:** the Vulkan scissor rectangle is SCISSOR's inclusive rectangle, intersected with the target.
- **Zero area:** dropped by both.
- **The shader's own coverage test** (6.2) makes the drawn set a subset of the reference's by construction; a
  missing pixel would show in check (c).

Targets: two `RGBA8_UNORM` colour textures of 640x256 (FBP 0x0 and 0x50) in both versions (K1's scissor reaches
row 253 in the USA version too, below the 240 displayed rows), one `D32F`. The picture is the top H rows.

### 8.2 The clear (the only sprite)

A sprite covers the integer points with `min <= p < max` on each axis (`gs_sprite_rules.h`: first = ceil(min), last
= ceil(max) - 1), i.e. the rectangle under the same top-left rule, flat in the second vertex's colour and Z.
`CLEAR` is drawn as two triangles through the `OFF` configuration with depth ALWAYS and write on (or as the pass's
clear when the rectangle and scissor cover the target); colour as sent, alpha included (0xff in RC's clear).

### 8.3 Lines

The reference (`GSCpuBackend::DrawLine`) truncates both end points to whole pixels (`int(X / 16) - (OFX >> 4)`),
walks Bresenham's algorithm between them drawing **both end pixels**, and gives pixel number `step` of `total =
max(|dx|, |dy|)` the colour `int(c0 + (c1 - c0) * t)` (IIP; else the second vertex's) and `Z = uint(z0 + (z1 - z0)
* t)` with `t = float(step) / float(total)`, each through the ordinary pixel pipeline (scissor, alpha test, Z test,
blend). Vulkan's line rules (diamond exit, the last pixel left out, `strictLines` or not by device) cannot give that.

So the replay expands each line on the CPU with a port of that function (the same C++ expressions, hence the same
floats) into one point per pixel: x, y, Z and colour, drawn as a `RHI_TOPO_POINT_LIST` with `gl_PointSize = 1` at
the pixel's centre (a one-pixel point centred on a pixel centre covers that pixel only), through the same fragment
shader with a "flat" record (weights unused, colour and Z from the point). K1 has at most a few hundred lines a
cycle, 63 pixels each on average. The draw list keeps lines as lines (so check (b) re-encodes them and P3 can draw
them at any scale); only the replay expands them. Whether the hardware draws the last pixel is not known here
(section 12).

### 8.4 What is bit-exact

Coverage of triangles, the clear and lines: exact (same integer rule; T5). No bound is needed.

## 9. The CPU model and the per-state pixel tests

### 9.1 `render/gs2d/gs_model.{h,cpp}` (no Vulkan, no runtime)

Pure functions over integers, written from sections 4-8 and not copied from the runtime, so that agreeing with the
emulated GS means something (as step 5's layout was):

```cpp
Rgba  gsTextureFunction(uint tfx, uint tcc, Rgba texel, Rgba vertex);
bool  gsAlphaPass(uint atst, uint aref, uint as);
Write gsAlphaWrite(const GsState &, uint as);             // which of RGB, A, Z this pixel writes
bool  gsDepthPass(uint ztst, uint z, uint stored);
uint  gsBlend(uint64_t alpha, uint cs, uint cd, uint as); // ((A-B)*C >> 7) + D, clamped: the GS
struct Ff { uint lo, hi; };                               // what the fixed-function plan of 4.2 may store
Ff    ffBlend(const GsPlan &, uint cs, uint cd, uint as); // lo == hi where the model is certain; lo..hi on a tie or without the bias
Rgba  gsPixel(const GsState &, Rgba source, Rgba dest, uint z, uint &storedZ);   // the whole per-pixel pipeline, GS rule
```
plus `coverTriangle`, `coverSprite`, `expandLine` (the rules of section 8) and `planState`. `rc_gs2d_test`
(`tools/rc_replay/gs2d_test.cpp`) first checks the model against itself: `ffBlend.lo >= gsBlend`, `hi <= gsBlend +
1`, equality wherever the plan claims `exactBlend` or the bias applies, exhaustively over Cs, Cd in 0..255, As or
FIX in 0..0x80, for every row of 4.2's table; and reproduces the percentages of 4.3.

### 9.2 Synthetic scenes through three machines

Each test builds GIF packets (the register writes and vertices a 2D packet would hold), runs them through the
reader, and draws the result (1) through the emulated GS (`rc_replay`'s machine, as the `tex` consumer's sampling
check does), (2) through `gs_model` on the CPU, (3) through Vulkan (headless RHI, RTX 2060, read-back). Inputs are
chosen so that every tested quantity is an integer by construction: untextured primitives with IIP 0 for colour and
alpha (no interpolation), whole-pixel rectangles.

| test | scene | pass |
|---|---|---|
| T1 blend sweep | destination uploaded with R = x, G = 255 - x, B = (37x) mod 256, x = 0..255, alpha 0x40; 256 rows, row y a flat untextured rectangle with colour (y, y, 255 - y) and alpha As; one draw per As in 0..0x80 (or once per FIX) for each ALPHA of 4.2's table and FIX in {0x15, 0x20, 0x3f, 0x40, 0x7f, 0x80} | emulated = `gsBlend` everywhere; Vulkan within `ffBlend`'s lo..hi everywhere, so equal to the GS for the exact rows and wherever the bias applies; stored alpha = As. Repeated with the bias off: Vulkan - GS in {0, 1} |
| T2 layers | six blended rectangles of different forms over one another | emulated = model; `0 <= Vulkan - emulated <= L` |
| T3 alpha test | every ATST (8) x AFAIL (4) x ZMSK (2), AREF in {0, 1, 0x2f, 0x3f, 0x80}: 129 flat columns with As = 0..0x80 over a patterned destination with a known alpha and Z; then the same with a 129x1 texture carrying the alphas (so the test is per pixel and the split is real), and with two overlapping rectangles in one batch at equal Z and at rising Z | colour, alpha and depth planes: emulated = model = Vulkan (blend off, so exact) |
| T4 depth | ZTST (4) x ZMSK, Z in {0, 1, 0xfffffe, 0xffffff} against stored values around each; the clear with Z 1 | colour and depth planes equal on all three |
| T5 coverage | triangles with every subpixel offset (16 x 16) of each vertex in turn, rectangle halves, slivers, fans of triangles sharing edges through pixel centres, a triangle with vertices 1500 pixels off screen, every scissor edge; lines in all eight octants with subpixel ends, crossing the scissor; sprites | the set of written pixels: emulated = model = Vulkan |
| T6 texture | textured rectangles at 1, 2, 1/2 and 1.07 texels per pixel, mirrored, with Gouraud colours and with one colour under IIP 1, vertex colours above 0x80; T4, T8, CT32, CT16 (TEXA); the four wrap modes with coordinates past the borders; the four TFX with TCC 0 and 1; MMAG nearest and linear; the rotated subpixel quad | emulated = Vulkan in all four channels (blend off). The emulated GS is the model here: `gs_model` supplies only the texture function |
| T7 device arithmetic | a pass that writes the bits of `w_i`, `invQ`, `uf`, `fu` for every pixel of six large triangles to an `RGBA32_UINT` target | equal to the CPU's values computed with the reference's expressions |

Each test has its planted fault (9.3). The depth plane is read back where the RHI allows it for `D32F`
(`RhiLimits`); otherwise it is inferred by a following full-screen draw at each interesting Z.

### 9.3 Planted faults (each must fail the named test, and check (c) on the front-end dump)

`ATST` LEQUAL drawn as LESS (T3); the failing part of the split dropped (T3, (c)); the split issued per batch
instead of per group (T3's overlapping rectangles); ZTST GREATER as GEQUAL (T4, (c): text at equal Z); the half
pixel left out of the position (T5, (c)); the bias applied where `a < b` (T1); FIX off by one (T1, (c)); TEXA
ignored (T6); the texel fetch off by one row (T6, (c)); a vertex colour channel swapped (T6, (c)).

## 10. Test plan for 10.2 row 7, refined

Dumps: step 5's (`s5`, 28 + 27, references as hashes) and step 6's (`s6`, every `REFI`), step 4's exact ones; both
versions. `rc_replay <dump> --consumer gif2d [--with-emu] [--vk] --verify`.

### 10.1 (a) Register files

At every kick with a recorded register file, every register the reader knows (2.2) must equal the recorded value;
after a channel 1 kick the reader's file is replaced by the next kick's recorded one (until step 8). **Pass:** no
mismatch in any dump; the count of compared values is reported (step 6 compared 9 registers and got 3.77 M values on
the USA `s5` dumps; this compares all of them). Also reported: draws that needed a seeded or default register, by
register (expected: TEXA for the CT16 quad, FRAME/XYOFFSET/COLCLAMP/PRMODECONT before a boot's first draw
environment, nothing else).

### 10.2 (b) In front of the emulated GS

For each K1 kick and each clear, the reader's draw list is encoded back into GIF packets (`gs2d_encode`: every
`GsState` field as an A+D register write, each primitive as PRIM, then RGBAQ, ST, XYZ2 per vertex; lines as lines,
the clear as its sprite) and submitted to the `emu` machine in place of the kick. **Pass:** `emu --verify` holds
with the substitution: every cycle's picture equals `REFI` (bytes or hash) and GS memory at keyframes and at the end
equals `GSVR`, Z buffer included, on all dumps. That proves the parse, the primitive assembly and that `GsState`
holds everything a draw depends on. Faults: a dropped primitive, a `GsState` field zeroed in the encoder (each
field in turn must fail somewhere in the front-end or Track Editor dump, or be reported as never mattering).

### 10.3 (c) Vulkan

Per cycle with a K1 kick, `gif2d --with-emu --vk` (the consumer sits in front of `emu` in the consumer list, as
`gif2dstat --with-emu` does, so at the K1 kick it sees the emulated memory before the kick):

1. read the 640x256 frame buffer at FRAME's FBP (CT32) and the Z buffer at ZBP as Z24 from the emulated GS's memory
   (the Z formats' block order is not in `gs_layout` yet: add it and check it against the runtime's tables in
   `rc_tex_test`, as the other formats were);
2. upload the colour (an `IMAGE_BLIT`) and seed the depth texture (a full-rectangle draw, depth ALWAYS, colour mask
   0, `gl_FragDepth` from an `R32_UINT` texture of the Z values);
3. replay K1's draw list through the Vulkan path; read back colour (and depth where possible);
4. let `emu` run the kick; read the emulated frame and Z buffers after it: the reference for this kick. (At the
   cycle's close `emu --verify` already proves the emulated picture is `REFI`.)

**Pass, per cycle, over all 640x256 pixels:**

| plane | criterion |
|---|---|
| alpha | Vulkan = reference |
| depth (where read back) | Vulkan depth * 2^24 = reference Z24 |
| R, G, B | `0 <= Vulkan - reference <= L(x, y)` (4.4); in particular equal wherever no inexactly blended K1 primitive's box covers the pixel. **As built:** `-L <= d <= 2L` (bias off), `-L <= d <= L` (bias on), L by coverage (16.1). **With the exact blend (section 18): Vulkan = reference, no L** |
| guards | none fired on a primitive that covers a pixel; no default or seeded register but those of 10.1 |

and over a dump, reported: the histogram of `Vulkan - reference` by draw state (expected: 0 on the great majority,
+1 on channels whose source term is under half a level), the largest L and the largest difference. A first failure
names the cycle, the pixel, the primitives over it and their states, and writes the three pictures
(`--png`). All `s5` and `s6` dumps and step 4's exact ones must pass on both versions, with the validation layer
on for the four `s6` dumps (0 errors).

The live half (batch 5): with `RC_RENDERER=vk` and a ring dump taken in the same session, a screenshot of a static
menu screen equals `gif2d --vk` of the same cycle drawn over the clear only.

## 11. Build batches (Opus), each with its check

1. **Reader, draw list, encoder.** `render/gs2d/` (`gs_state.h`: `GsState`, `GsVertex`, ops; `gif2d_reader`;
   `gs_plan` with the guards; `gs2d_encode`), library `rc_gs2d` on `rc_tex`; `rc_replay --consumer gif2d` with
   checks (a) and (b) (`--with-emu`), the planted faults of 10.2; `gif2dstat` stays as the measuring tool. *Check:*
   10.1 and 10.2 on every exact dump, both versions; the state and register tables of section 3 reproduced from
   the reader's own draw lists.
2. **The model and the Vulkan path.** `gs_model`, the RHI edits of U2 (or the fallback), `render/gs2d/gs2d_vk`
   (targets, the three blend configurations x {triangles, points} x depth compare/write x colour mask pipelines made
   at start, the primitive buffer, textures from `TEXTURE` commands, the clear, line expansion, the split by
   groups), `gs2d.vert` / `gs2d.frag`; `rc_gs2d_test` with T1-T7 through the emulated GS, the model and Vulkan,
   and the faults of 9.3. *Check:* every test passes on the RTX 2060 with the validation layer on; the bias's T1
   result stated; the faults fail.
3. **Check (c) on the dumps.** The Z layouts in `gs_layout`, the seeding, the comparison and its report in the
   `gif2d` consumer. *Check:* 10.3 on all dumps of both versions; the difference histogram by state and the
   largest L written back into section 3.3's place in this file ("as built"), and the DIVERGENCES entries of
   section 12 drafted.
   *Built 2026-11-20: section 16.*
4. **Review point (Fable):** the report of batch 3 against the bounds here, before the live half; decision U1 is
   revisited with real numbers.
   *Done 2026-11-20: section 17. Outcome: batch 4b below; the bias on for the fixed-function fallback; batch 5
   after step 8's first batch.*

   4b. **The exact blend by local read (Opus).** Section 18 is the specification and the check.
5. **Live.** The reader in the capture's consumer list (EE thread), the draw list published at the cycle close
   (section 8 of the frame record), the presenter replaying clear + uploads + K1 into the targets and presenting
   `target[DISPFB2.FBP]`; `NOTES.md` and the as-built section. *Check:* the front end and menus readable in a
   `RC_RENDERER=vk` session (over black until step 8); the screenshot check of 10.3; recording off and the oracle
   unchanged (the game binary now links `rc_gs2d`: `playtest2` scene log and frame rate, `tools/oracle.sh --all`,
   both versions).

Batches 1 and 2 are independent of each other; 3 needs both. Order from here (17.4): 4b, then step 8's design and
its first batch (the VIF reader and the references off-line: PATH1's register writes), then batch 5.

## 12. Divergences this design accepts or inherits (for P2 step 10's list)

Drafted for `DIVERGENCES` (P2 step 10) after batch 3; numbers from check (c) on the 67 exact dumps (16.2).
**After the batch 4 review (17):** D-2D-1 and D-2D-2 describe the fixed-function blend only, which becomes the
fallback for a device without local read (and an option); with the exact blend of section 18, the default wherever
the device has local read (the RTX 2060 and the Vega do), neither exists. D-2D-2 is decided: the bias is on.
**Batch 4b built it (19):** with local read every plane of every judged kick of the 67 exact dumps equals the
reference, so D-2D-1 applies only to devices without local read, or to `Gs2dVk::kBlendFixed` chosen on purpose.

| | what | size | where |
|---|---|---|---|
| D-2D-1 (devices without local read, or `kBlendFixed`; batch 4b, 19) | fixed-function blend of the forms with a fractional factor (`Cd*F+Cs`, the lerps `(Cd-Cs)*F+Cs`, `(Cs-Cd)*F+Cd`, `(Cd-Cs)*f` with 0 < F < 0x80): the blender rounds where the GS floors, and on the RTX 2060 it takes the shader's source term and the dual-source factor at 8 bits (15.3), so neither the 127/256 bias nor a flat source makes it exact | per inexact layer -1..+2 levels per channel without the bias, -1..+1 with it; per pixel `-L..+2L` / `-L..+L`, L the inexact primitives covering it (largest 7 in the 67 dumps; 0-3 on 97% of the pixels with L > 0). Measured over 14.5 G pixel-kicks: 0.21% (bias off) / 0.19% (on) of all colour channels differ, never alpha or Z; of the pixels under an inexact layer 52% / 56% are equal (16.2) | K1's text (`0x09`), panels (`0x01`), dimming boxes and fades (`0x21` FIX 0x20, 0x3f, 0x40); in the race only the HUD's text and panels. Removed by the exact blend (section 18). The stacking bound is measured, not proved, for an 8-bit blender (16.5) |
| D-2D-2 (batch 3) | the source bias setting changes the sign of D-2D-1's error: off, the error is upward (+1, rarely +2) and the text equal on 80% of its blended pixels (77% with the bias); on, it is two-sided, smaller in total (11% fewer differing channels) and largest on the panels' lerp, but text is darker by one level on 5.8 M channels | see 16.2's table | **decided (17.1): on.** Applies to the fixed-function fallback only |
| I-1 (inherited from the reference, reproduced on purpose) | one-colour Gouraud primitives shaded one level low on about 6% of their pixels | 1 level before the texture function | every flat-coloured K1 quad; a real GS presumably shades them exactly |
| I-2 (inherited) | bilinear filtering with float weights rounded to nearest; a real GS uses 4-bit weights and truncates | up to 1 level | every textured K1 pixel |
| I-3 (inherited) | lines by Bresenham between truncated end points, both end pixels drawn | whole pixels at line ends | the Track Editor's lines |
| note | the game samples its 2D textures on texel boundaries in u (equal mix of two neighbouring texels) | - | what the game sends; the same on any correct GS |

I-1 to I-3 disappear from the picture when P3 moves 2D to GPU interpolation at higher resolutions; they are kept in
P1 because the reference pictures contain them and a renderer that differs from its only reference by design cannot
be checked tightly.

## 13. Open

- **The exact blend's cost live (19.5):** 6x fixed function's draw time on the front end (largest kick 3.6 ms, about
  20 us a barrier); a decision before batch 5 and for step 8.
- Scenes not in the dumps (split screen, podium, results, memory card screens, the Track Editor's race): record them
  when a session reaches them and run `gif2dstat` and check (c); the guards of 2.4 report anything new at once.
- The hardware's behaviour for I-1 to I-3 (no PS2 capture in this project).
- Depth read-back of a `D32F` texture through the RHI (`RhiLimits`): if it is not available the depth plane is
  checked only through its effect on later draws and through check (b)'s GS memory comparison.
- The fixed-function blend on a device with a blender wider than 8 bits (the Vega was not run: graphics tests are
  kept on the RTX 2060): there the bias should give 4.3's 0..+1; `rc_gs2d_test` T1 says which when such a device is
  the target.
- Sampling the K1 fragment shader at more than one sample per pixel (P3): the record and the weights are
  resolution independent (`e_i` at a fractional sample point), the coverage guard is not.

## 14. Batch 1 as built (2026-11-20): reader, draw list, encoder

Section 11 item 1. Built: `render/gs2d/` (library `rc_gs2d` on `rc_tex`, namespace `rc::rd`) and `rc_replay --consumer
gif2d` (`tools/rc_replay/gif2d.{h,cpp}`); the `emu` consumer gained a kick substitution hook. Usage: `NOTES.md`, "The
2D packet: reader, draw list, encoder". Nothing of it is linked into the game or the oracle (`ninja -n rc_revenge_pro
rc_oracle` has nothing to compile or link after the change, both trees), the runtime and every game function are
untouched: normal play and the oracle cannot have changed and were not re-run.

```
render/gs2d/gs_state.h          GsState (128 bytes), GsVertex (32), GsReg / GsGroup / DrawOp / PrimKind, DrawCmd, DrawList
render/gs2d/gs_plan.{h,cpp}     GsPlan planState(const GsState &), the guards, gsSourcePart, flatSourceExact
render/gs2d/gif2d_reader.{h,cpp} Gif2dReader: the consumer (TexShadow + TextureCache inside), register file with
                                provenance, primitive assembly, the draw list per cycle; path1Writes
render/gs2d/gs2d_encode.{h,cpp} encodeDrawList: a list range back into PACKED A+D GIF packets, the register file restored
tools/rc_replay/gif2d.{h,cpp}   Gif2dCheck: checks (a) and (b), the section 3 tables, planted faults
```

### 14.1 Changes to the design, and why

1. **`GsState` is 128 bytes with `reserved[14]`** (the fields of 2.1 add up to 124; the struct is padded to 8, so the
   padding is made explicit and zeroed: the reader compares states with `memcmp`). **`prim`'s type is the primitive
   class** (point 0, line 1 for lines and line strips, triangle 3 for lists, strips and fans, sprite 6): triangles
   from a strip are stored as separate triangles anyway, and the re-encoder writes `prim` as PRIM before each
   primitive. `known` is a bit per register group (`GsGroup`: FRAME ... DTHE, 16 groups), set where every register of
   the group the draw read came from the stream.
2. **`GsVertex`'s `reserved` word holds the fog byte and a flag "sent as XYZF"**, so the re-encoder writes back
   exactly what was sent (`XYZF2` with its F, or `XYZ2` with 32-bit Z). No XYZF vertex occurs in K1 or the clear.
3. **`SCREEN_PRIMS` kinds 0 (points) and 3 (sprites other than the clear)** are emitted rather than dropped (never seen
   on PATH3: 0 in all dumps), so check (b) re-encodes everything the reader saw. **States a guard marks as not
   drawable are emitted with `STATE` flag bit 0** (and the replay drops them) instead of being left out of the list;
   the reader itself drops only a draw with FRAME.FBW 0 (2.2) and a draw that needs an unknown register with no
   default (3.1's "none"). Scope commands: a = tag kind, b = tag sequence number, c = the tag's guest address, payload
   the tag's.
4. **`GsPlan` grew** `ks`, `kd` (the coefficients of Cs and Cd as forms in F, after FIX 0 or 0x80 made them
   integers), `abcd` (the ALPHA selectors the shader uses for the integer source term, 0 = Cs where the blend is off
   by ABE 0 or by PABE under its precondition), `alphaByBlend` (5.2's RGB_ONLY-with-ZMSK case) and `guards` (a bit per
   guard that fired); `unsupported` is the guard that drops the draw. Two guards were added: `ZTE` (ZTE 0: the
   reference ignores it and applies ZTST; drawn the same way) and the 16-bit Z formats under `ZBUF`. A destination
   coefficient that cannot be expressed (1+F, -F, 2, -1) is drawn as its source part with blend off (`BLEND_FORM`).
   The plan's blend classification reproduces 4.2's table row for row (14.3).
5. **`TEXTURE` commands are emitted once per texture generation for the reader's life**, not per list (2's "has not
   sent yet" read as "ever"): `forgetSentTextures()` makes the next lists self-contained again. Over the USA `s5`
   dumps 236 `TEXTURE` commands (8.3 MB) in 22,000 cycles.
6. **The re-encoder restores the register file at the end of each kick.** The design's encoding (state writes,
   PRIM, RGBAQ, ST, XYZ2 per vertex) leaves registers different from the original stream's (PRMODECONT, TEXA and
   TEX0 written as the state holds them, XYZ3 never written, the last RGBAQ/ST...), and `emu --verify` checks the
   register file before every kick. So after the draws the encoder writes back every register whose value its own
   writes left different from the reader's file after the kick (which check (a) proves equal to the recorded one):
   vertex registers under PRIM type 7 (the reference takes their values into its registers and draws nothing for the
   reserved type), then the others, then PRIM and PRMODECONT. TRXDIR, HWREG, SIGNAL, FINISH and LABEL have side
   effects and would only be counted (never needed: 0 in all dumps). UV is written per vertex as well.
7. **The emulated machine takes the substitute through `PS2Memory::submitGifPacket(Path3)` under the kick's MXCSR**
   (the software GS's float arithmetic depends on it), in place of the DMA kick; the kick's recorded GIF hash is not
   checked for a substituted kick (it is counted). Every drawing PATH3 kick of every dump was substituted: no kick
   both uploaded and drew (the reader counts them and would leave them to the DMA).
8. **The registers a VIF1 kick changes were measured** (2.2 left them to the reader): over the 67 exact dumps PRIM,
   RGBAQ, ST, XYZ2, XYZ3, TEX0_1, TEX1_1, TEXA, SCISSOR_1, ALPHA_1, TEST_1, PABE and ZBUF_1 differ before and after a
   VIF1 kick; `path1Writes` is that set plus UV, XYZF2, XYZF3 and FOG. A channel 1 kick makes them unknown; every
   other register is compared by check (a) across VIF1 kicks too (stronger than "replaced by the next kick's file"),
   and none differed.
9. **2.2's "K1 depends on two registers PATH3 never writes" is not the whole story.** Draws that needed a seeded
   register after the dump's first clear (`s5`; "wrong" = the 3.1 default differs or there is none):

   | register | USA primitives (wrong by default) | Europe | where |
   |---|---|---|---|
   | ZBUF_1 | 127,862 (9,780) | 104,322 (6) | every race cycle's HUD (value 0x10000a0, the default); USA front end and boot: the CT32 backdrop with ZMSK |
   | FBA_1 | 645,830 (0) | 522,932 (0) | everything: never written on PATH3, always 0 |
   | SCISSOR_1 | 170,709 (170,709) | 147,372 (147,372) | Track Editor (no default exists) |
   | TEST_1 | 62,237 (62,237) | 54,822 (54,822) | Track Editor lines (no default) |
   | TEX1_1 | 99,612 (99,612) | 86,380 (86,240) | Track Editor (0xf6000000160) |
   | TEXA | 56 (0) | 96 (0) | the CT16 quad, as designed |
   | CLAMP_1 | 10 (8) | 0 | boot, before PATH3 first writes it; cover nothing |
   | PABE | 3 (0) | 3 (0) | boot |

   Before the dump's first clear: 352 / 299 primitives (FRAME, XYOFFSET... at a boot or a dump's start, as designed).
   So K1 inherits from PATH1 (the 3D packet drawn just before it in the cycle): ZBUF_1 on every race cycle's HUD (the
   value is 3.1's default) and on the front end's backdrop (USA: ZMSK set, which the default does not give), and in
   the Track Editor SCISSOR_1, TEST_1 and TEX1_1 (the grid lines' scissor 3..637 x 3..237 in the USA version, their
   alpha test 0x2f, the texture filter K -10). FBA is never written on PATH3 (0, the default) and CLAMP not before 8-10
   primitives covering nothing. **Consequence for batch 5** (live, before step 8 reads PATH1): with only 3.1's defaults
   the front-end backdrop would be drawn with Z writes it does not make and the Track Editor's 2D with the wrong
   scissor, alpha test and filter. Batch 5 needs PATH1's register writes: either step 8 first, or the live reader
   seeds from the runtime's register shadow (`ps2x::capture::g_gsRawRegs`, kept by the GS front end) at each PATH3
   kick, which is exact only where the GS is synchronous with the EE (exact mode) — a decision for batch 5.
10. **The `reg` fault drops every write of one register for a cycle** rather than "write n of the cycle": a single
    lost write of a vertex register is overwritten before the kick ends and only (b) can see it (it did: 14 pixels);
    a register lost for the cycle shows in (a) at the next kick.

### 14.2 Checks and results

`rc_replay <dump> --consumer gif2d --with-emu --verify --quiet` on all 67 exact dumps (this runs (a) and (b) in one
pass; (a) alone, without `--with-emu`, gives the same (a) numbers in about a second a dump). All 67 `PASS`; 16 minutes
for the lot on 7 cores.

| | USA s5 (28) | Europe s5 (27) | USA s6 (2) | Europe s6 (2) | USA b4 (4) | Europe b4 (4) |
|---|---|---|---|---|---|---|
| cycles | 22,000 | 19,300 | 1,300 | 1,300 | 874 | 845 |
| (a) values compared / kicks / differ | 76,654,573 / 785,929 / 0 | 68,996,065 / 707,323 / 0 | 2,363,281 / 24,295 / 0 | 2,769,964 / 28,445 / 0 | 2,896,060 / 29,697 / 0 | 2,764,513 / 28,350 / 0 |
| (b) drawing kicks encoded = substituted | 43,145 | 37,864 | 2,080 | 2,082 | 1,679 | 1,624 |
| (b) primitives re-encoded (STATE writes) | 645,858 (137,884) | 522,959 (104,768) | 176,605 (38,933) | 118,650 (24,315) | 27,751 (6,160) | 32,084 (7,469) |
| registers written back at kick ends; left different | 176,415; 0 | 152,881; 0 | 8,062; 0 | 8,117; 0 | 7,308; 0 | 7,058; 0 |
| emu: register files before a kick | 785,929 of 785,929 | 707,323 / 707,323 | 24,295 / 24,295 | 28,445 / 28,445 | 29,697 / 29,697 | 28,350 / 28,350 |
| emu: picture hashes; stored pictures (REFI bytes) | 22,000 / 22,000; - | 19,300 / 19,300; - | 1,300; 1,300 / 1,300 | 1,300; 1,300 / 1,300 | 874; 130 / 130 | 845; 130 / 130 |
| emu: GS memory at keyframes; final | 62 / 62; 28 / 28 | 54 / 54; 27 / 27 | 3 / 3; 2 / 2 | 3 / 3; 2 / 2 | 4 / 4; 4 / 4 | 5 / 5; 4 / 4 |
| emu: GIF hashes of the kicks not substituted | 742,784 / 742,784 | 669,459 / 669,459 | 22,215 / 22,215 | 26,363 / 26,363 | 28,018 / 28,018 | 26,726 / 26,726 |
| primitives dropped (FRAME.FBW 0) | 324 | 272 | 2 | 2 | 0 | 0 |

Zero everywhere: guards fired, kicks that both uploaded and drew, walker bytes dropped, absent pages, indexed textures
drawn with CLD other than 1, vertices with Z at or above 2^24 under Z24, XYZF vertices, points and non-clear sprites,
draws with SCANMSK or CSM2 set, and writes of any register outside `GsState` a draw could read (SCANMSK, DIMX,
TEXCLUT, FOGCOL, FOG, TEX2). The dropped FBW-0 draws change nothing in the emulated pictures or memory (they are left
out of the substitute, and (b) still holds), which confirms 2.2's "none covering a pixel". The plain dumps of step 4
(14 per version) run without being judged (no recorded register files); (a) compares nothing there.

### 14.3 The tables of section 3 from the reader's own draw lists

The `gif2d` consumer counts, per cycle's list, the keys `gif2dstat` uses where the list holds the information (the
`state`, `blend` and `z` categories, and the `reg` lines PRIM attributes, SCISSOR, XYOFFSET, FRAME, ZBUF, TEST, TEX1,
CLAMP, texture), with the reference's coverage for the pixel column. Summed over the `s5` dumps (`tools/gif2d_agg.py`)
and compared key by key with `gif2dstat`'s: USA 97 keys, Europe 152, equal in primitives and pixels but for the
FRAME.FBW 0 draws (324 / 272 primitives, 0 pixels), which `gif2dstat` counts and the reader drops. So section 3.1's
table and the draw-state table are reproduced from the draw lists. PRMODECONT and TEXA lines are not comparable (the
state folds PRMODECONT into `prim` and keeps TEXA only where the format reads it).

planState over those states (the `plan` lines; USA `s5` without `l17long`) gives 4.2's column: 0x09 MIX linear
(ks 1, kd F=As), 0x01 and 0x21 MIX linear (ks 1-F, kd F) with the RGB_ONLY split, 0x29 FIX 0x80 MIX exact (ks 1, kd 1),
0xa1 FIX 0x80 MIXSUB exact (ks -1, kd 1), untextured and line states OFF, the clear OFF with depth ALWAYS. All 31,460 /
28,230 dimming boxes (0x21 FIX 0x20) are flat-source exact (no zero colour channel), so they leave L.

Draw-list size: `TEXTURE` commands USA `s5` 268 (9.0 MB) in 22,000 cycles, Europe 338 (13.2 MB); the largest list
in any dump 201 commands, the largest payload 362 KB (a front-end cycle whose textures are sent for the first time).

### 14.4 Planted faults (`--fault`, `s5` front end and Track Editor of each version)

| planted | USA | Europe |
|---|---|---|
| `reg:500:66`, `:71`, `:6` (every write of ALPHA_1, TEST_1, TEX0_1 lost in cycle 500), no emu | (a) fails: 6 values at the next kicks | the same |
| `prim:500:7` (primitive 7 of the front end's cycle 500 left out of the substitute) | (b) fails: cycle 500's picture (899 / 900) | the same |
| `prim:400:300` in the Track Editor | passes: that primitive changes no picture or memory image (overdrawn or off the picture; not looked into further) | the same |
| `field:0:k` (GsState field k written as 0 in every STATE), k = frame, zbuf, scissor, xyoffset, test, alpha, tex0, tex1, clamp, prim | (b) fails on both dumps: 4-390 of 900 front-end pictures and 0-1 of 700 Track Editor pictures right; GS memory at the keyframes and the end differs | the same |
| `field:0:k`, k = miptbp1, miptbp2 | never matter: MXL is 0 in every K1 state, so the GS reads neither | the same |
| `field:0:11` (texa), also on `l13` (the CT16 quad) | never mattered in these dumps: TEXA is stored only for the CT16 quad, and zeroing it changed no picture or memory image of `l13` | the same |
| `field:0:k`, k = pabe, fba, colclamp, dthe | never matter: PABE, FBA and DTHE are 0 in every K1 state (zeroing is no change), and with COLCLAMP 0 no K1 blend result leaves 0..255 in these dumps | the same |

Earlier, on step 4's menu dump `b4-key-1`: `field:450:0` and `field:450:4` fail from cycle 450 on, `prim:450:5` fails
cycle 450 (13 pixels); a single lost RGBAQ write (the old `reg` fault) failed (b) by 14 pixels but not (a), hence 14.1
item 10.

### 14.5 Open

- **K1's PATH1-inherited registers (14.1 item 9)** decide batch 5's approach before step 8: read the runtime's register
  shadow at PATH3 kicks, or wait for step 8. Not a batch 1 matter; the replay is exact either way.
- **Why zeroing TEXA changes nothing on `l13`** was not looked into (the quad's vertex alpha or its alpha test may make
  As irrelevant there); check (b) holds with the real value, which is what counts.
- Scenes not recorded (split screen, podium, results, memory card, the Track Editor's race): `path1Writes` and the
  guards are measured on the 67 dumps only; check (a) on a new dump names any register PATH1 changes beyond the set.

## 15. Batch 2 as built (2026-11-20): the model and the Vulkan path

Section 11 item 2. Built: `gs_model` (in `rc_gs2d`), the RHI edits of decision U2, `render/gs2d/gs2d_vk` (library
`rc_gs2d_vk` on `rc_ico_rhi`) with its shaders, and `rc_gs2d_test` (`tools/rc_replay/gs2d_test.cpp`). Usage: `NOTES.md`,
"The 2D packet on Vulkan". Nothing of it is linked into the game or the oracle; the RHI edit is (15.5).

```
render/gs2d/gs_model.{h,cpp}     the GS rules (texture function, alpha test and write mask, Z test, blend), ffBlend, gsPixel,
                                 triangle setup / weights / per-pixel quantities, coverSprite, expandLine, GsModel (a list drawn on the CPU)
render/gs2d/gs2d_vk.{h,cpp}      Gs2dVk: targets, every pipeline, Gs2dPrim records, TEXTURE uploads, the clear, line pixels,
                                 the split by groups, seed / draw / read-back / probe; the planted faults
render/gs2d/gs2d.vert, .frag     the record placed (half pixel, 4096 viewport); the reference's per-pixel arithmetic (GS2D_POINTS,
                                 GS2D_PROBE builds); gs2d_seed.vert/.frag: colour and Z * 2^-24 from CPU planes
tools/rc_replay/gs2d_test.cpp    rc_gs2d_test: the model's self-check, T1-T7 through the emulated GS, gs_model and Vulkan, the faults
third_party/ico_rhi              RC: shaderFloat64 enabled where offered, RhiLimits.shaderFloat64 (VENDORED.md)
```

### 15.1 Changes to the design, and why

1. **One storage buffer of 44-word records, no vertex buffer, no uniforms.** Every primitive (and each flat triangle of
   the clear, each pixel of a line) is a `Gs2dPrim`: window coordinates, colours, S/T/Q, Z, the edge functions with `c`
   and `1 / area` as double bits, and the plan's per-pixel parameters (term, factor, coefficients, FIX, ALPHA selectors,
   alpha test, TFX/TCC/filter/wraps/size, region bounds, which part of the split, bias, faults). The vertex shader reads
   its position from the record by `gl_VertexIndex` (3 a record, or 1 for the points build); the fragment shader reads
   the record by a flat index. So nothing per draw needs a uniform; a draw is a pipeline, a texture group and a record
   range.
2. **The clear is two flat triangles with corners on whole framebuffer coordinates** (no pixel centre on an edge), its
   rectangle from `coverSprite` intersected with the scissor; line pixels are points (`gl_PointSize` 1 at the pixel
   centre) carrying the final colour and Z. A flat record skips the weights.
3. **Pipelines:** blend configuration (3) x alpha-by-blend (2, only without depth write) x {triangles, points} x depth
   compare (4) x depth write (2) x colour mask (none, A, RGB, RGBA): 288, made at `create()` by
   `rhi_create_pipelines` in 0.1-0.7 s; plus the seed and the probe pipelines.
4. **5.2's "As is one value" decision on the CPU** covers untextured primitives with flat alpha and textured ones whose
   texture alpha range is one value; such a primitive goes only into the pass or the fail draw of its group.
   **Groups** are runs of primitives whose scissored boxes are pairwise disjoint, the second half of a rectangle pair
   (same check as the reader's) allowed to touch its first half; for points, runs of distinct pixels.
5. **`1 / Q` in double** (6.3's fallback): T7 found the float division one ULP off on 4% of pixels on the RTX 2060
   (22,063 of 521,450, and with it uf, vf, fu, fv); `float(1.0lf / double(q))` matches the reference on every pixel.
   The weights `float(double(e) * invArea)` were exact from the start.
6. **The source bias is off** (`Gs2dVk::Options::sourceBias`, default false): T1 fails with it (15.3); the design
   switches it off then. It also fails without it, which the design did not foresee (15.3, 15.6).
7. **Seed:** one full-target draw writes colour from an RGBA8 texture and depth from an `R32_UINT` texture (both always).
   Depth read-back of the `D32F` texture works through the RHI (`rhi_readback_texture`, depth aspect); every value read
   back is an integer Z (checked).
8. **Reader test hooks:** `Gif2dReader::startBlank()` / `feedPath3()` (a kick from raw PATH3 bytes) and
   `Options::faultIgnoreTexa` (the TEXA fault: textures decoded as if TEXA were TA0 0, TA1 0x80, AEM 0).
9. **The emulated GS of the tests** is the runtime's `GS` alone (`GS::init` on 4 MB, the bare synchronous
   `GSCpuBackend` as in rc_replay's emu machine), seeded and read with `GS::WriteVram` / `ReadVram` (CT32 at FBP 0x0 and
   0x50, Z24 at ZBP 0xa0). Its fast paths were taken for every triangle; the 129 sprites (the clears) took its general
   sprite path (fallback counter 0).
10. **9.1's figures** are over Cs and Cd in 0..255 (F 0..0x80): without the bias `Cd*F+Cs` is one above on 35.0% and a
    tie on 2.0% (the design's "37%"), the lerps 46.5% and 2.7% (49%); with the bias `Cd*F+Cs` exact 99.81%, one above
    0.18%, the lerps exact 99.39%, one above 0.58% (the design said 98.0% exact: 100 - 0.58 - 0.02 is 99.4). Over Cs, Cd
    in 0..0x80 the bias leaves 0.36% and 1.14% one above.

### 15.2 Results (`rc_gs2d_test --validation`, RTX 2060, driver 620.70.06; 268 s, 1,716 scenes)

| test | what ran | result |
|---|---|---|
| model (9.1) | every row of 4.2 x every As (0..0x80) or FIX (0x15, 0x20, 0x3f, 0x40, 0x7f, 0x80) x Cs, Cd 0..255: 44.2 M triples | pass: `ffBlend.lo >= gsBlend`, `hi <= gsBlend + 1`, equal wherever the plan is exact or the bias applies |
| T1 | 675 scenes (each 256 x 256 pixels) with the bias and 675 without | **fails the design's criterion on the device** (15.3); passes everything else: emulated = `gsBlend` everywhere, stored alpha = As, every integer-exact row (0x68, 0x62, 0x08, 0x29/0xa1 with FIX 0x80, PABE rows) equal to the GS on all 44.2 M pixels |
| T2 | two six-layer stacks x four patterned destinations | pass: emulated = model; `0 <= Vulkan - GS <= L` on every channel (largest L 10; (Vulkan - GS) / L at most 0.5; 97% of pixels have a channel above the GS) |
| T3 | ATST 8 x AFAIL 4 x ZMSK 2 x AREF {0, 1, 0x2f, 0x3f, 0x80} = 320 scenes: flat columns (decided on the CPU), a textured alpha ramp (the split per pixel), two overlapping textured rectangles in one batch at equal and at rising Z; FBP alternating | pass: colour, alpha and Z planes emulated = model = Vulkan (24,120 split primitives in 11,880 groups; 46,440 decisions on the CPU) |
| T4 | ZTST 4 x ZMSK 2; stored Z {0, 1, 2, 0x7fffff, 0xfffffd, 0xfffffe, 0xffffff} by column against drawn Z {0, 1, 2, 0xfffffd, 0xfffffe, 0xffffff}; the clear (Z32, Z 1) and bands over it | pass: all planes equal |
| T5 | 768 triangles (each vertex at each of 16 x 16 subpixel offsets), 64 rectangles with subpixel corners, 8 slivers 1/16 high, 4 fans of 12 through integer centres, a triangle reaching 1,500 px off screen, 6 scissors (each edge), 64 lines in all octants crossing the scissor (Gouraud too), 120 sprites with subpixel corners in either order; additive blend counts coverage | pass: all planes equal (no pixel drawn twice or missed) |
| T6 | CT32 64x64, CT16 64x64 with TEXA (TA0 0x30, TA1 0x70, AEM 1), T8 and T4 32x32 with CT32 palettes: 1, 2, 1/2, 1.07 texels a pixel, mirrored, Gouraud, one colour under IIP, colours above 0x80, nearest and linear; the four wraps past the borders; TFX 0-3 x TCC 0/1; the rotated subpixel quad and one with Q from 1 to 2.11 | pass: emulated = Vulkan (and = model) in all four channels |
| T7 | six large triangles (vertices off screen, subpixel, Q 0.25..3), probe of w0, w1, w2, 1/Q, uf, vf, fu, fv | pass: all 521,450 covered pixels bit-equal to the CPU (after 15.1 item 5), coverage equal |
| validation | Khronos layer on for the whole run | 0 errors, 0 warnings |

### 15.3 T1 and the source bias: the blender is 8-bit on the RTX 2060

Vulkan - GS per channel over T1's 44.2 M pixels (three channels each) for the rows that are not integer exact:

| ALPHA | with the bias | without |
|---|---|---|
| 0x09 `Cd*As+Cs` | -1: 9.0 M, 0: 15.8 M, +1: 0.6 M | 0: 15.9 M, +1: 9.4 M |
| 0x29 FIX f | -1: 0.37 M, 0: 0.79 M, +1: 0.02 M | 0: 0.82 M, +1: 0.36 M |
| 0x01 lerp As (and 0x44) | -1: 4.3 M, 0: 18.1 M, +1: 3.0 M | -1: 0.31 M, 0: 13.7 M, +1: 11.3 M, +2: 0.12 M |
| 0x21 FIX f | -1: 0.24 M, 0: 0.83 M, +1: 0.12 M | -1: 0.05 M, 0: 0.73 M, +1: 0.39 M, +2: 0.008 M |
| 0xa1 FIX f < 0x80 | -1: 0.04 M, 0: 1.04 M, +1: 0.10 M | 0: 0.90 M, +1: 0.27 M, +2: 0.009 M |

The design's premise (4.3: a blender with more than 17 bits) does not hold. The read-back of a small table
(`rc_gs2d_test --only blenddbg`) fits a blender that takes the shader's source colour **and the dual-source factor at
8 bits** before blending: a source of 0.504 level stores 0 and 1.504 stores 1 (the bias then costs a whole level), and
FIX 0x15 (0.1640625) acts as 42/255 (Cd 252 gives 42 where the exact product is 41.34). Integer sources with factor 0
or 1 come through exactly (every exact row above). So on this device fixed function gives, per inexactly blended layer,
-1..+1 with the bias and -1..+2 without it, not 4.4's 0..+1; T2's stacks happen to stay within 0..L. Check (c)'s bound
(4.4) cannot be claimed; exactness needs the destination read (decision U1). **Open for the user (15.6).**

### 15.4 Planted faults (9.3; each run of the named test with the fault, the design criterion of T1/T2 left out)

| fault | test | result |
|---|---|---|
| ATST LEQUAL drawn as LESS | T3 | fails |
| the failing part of the split dropped | T3 | fails |
| the split issued per batch instead of per group | T3 | fails |
| ZTST GREATER as GEQUAL | T4 | fails |
| the half pixel left out of the position | T5 | fails |
| the bias applied where a < b | T1 | not applicable: the bias is off, and the criterion that would catch it fails without a fault |
| FIX off by one | T1 | fails (the integer-exact FIX rows) |
| TEXA ignored | T6 | fails |
| the texel fetch one row off | T6 | fails |
| a vertex colour's R and G swapped | T6 | fails |

Check (c)'s side of each fault is batch 3's.

### 15.5 The game and the oracle

`rc_revenge_pro` and `rc_oracle` link nothing new (their link lines in both trees: no `rc_gs2d*`, `rc_tex`, `rc_blit`;
the oracle has no RHI at all). The RHI edit changes `rc_ico_rhi`, which the game links, so both games were rebuilt
and the regression checks run: `tools/oracle.sh --all` USA (seed 7311) 1632 ok and Europe (seed 7312) 1563 ok, 0 FAIL;
`playtest2` replays reach the race scene 3020 at frame 658 (USA) and 1167 (Europe); an `RC_RENDERER=vk` replay with the
validation layer: frame 658, 0 validation errors. `rc_gs2d_test --no-design-bound` (everything but T1's and T2's bound
for inexact blends) ends PASS; without it the run ends FAIL on T1 alone.

### 15.6 Open

- **The blend on this device (15.3): decision for the user.** Options: (a) decision U1's exact mode now (the shader
  reads the destination through fragment-shader interlock, which the RTX 2060 has; depth then also in a storage image
  with the Z test in the shader, 4.2/U1); (b) accept fixed function with a measured bound (-1..+2 per inexact layer
  without the bias, -1..+1 with it) as D-2D-1 and widen check (c) to it; (c) look for another target format (the RHI has
  no RGBA16_UNORM or RGBA32F target; RGBA16F cannot hold the GS's integer floor). Batch 3's check (c) depends on it.
- Whether to keep the bias off (design rule; larger maximum error) or on (smaller maximum error, fewer equal pixels for
  `Cd*F+Cs`, more for the lerps) if (b) is chosen.
- The model draws textures only from the decoded level, so region wraps that reach past the texture read 0 where the
  reference reads memory (the tests keep region bounds inside the texture; RC uses CLAMP only).

## 16. Batch 3 as built (2026-11-20): check (c) on the dumps

Section 11 item 3, with the orchestrator's decision after 15.3 (option (b), reversible): the fixed-function blend is
accepted with its measured bound as D-2D-1, check (c) is widened to it, and it runs with the source bias off and on so
that the batch 4 review can choose the default and decide U1 on real numbers. U1 was not built. Usage: `NOTES.md`,
"Check (c): the 2D packet on Vulkan against the emulated GS".

```
render/tex/gs_layout.{h,cpp}     Z32/Z24/Z16/Z16S: the colour twin's block order with the top two block bits inverted
tools/gs_tables_export.cpp       also writes CT16S and the Z formats (rc_tex_test compares all eight against the runtime)
tools/rc_replay/emu.{h,cpp}      Emu::setAfterKick: a callback after the machine has run a kick (substituted or not)
tools/rc_replay/gif2d_vk.{h,cpp} Gif2dVkCheck: seeding, the Vulkan replay per kick and bias, the comparison and its report
tools/rc_replay/gif2d.{h,cpp}    --vk wiring (the reader's kick range, the emulated memory before and after), guards fail (c)
tools/rc_replay/rc_replay.cpp    --vk, --vk-bias off|on|both, --vk-fault <names>, --vk-png <dir>; --validation
```

### 16.1 Changes to the design, and why

1. **Per kick, not per cycle.** 10.3 compares at the K1 kick; the consumer judges every drawing PATH3 kick, the
   flip's clear included (half the kicks), so the clear's sprite path, the Z32 write of Z 1 and the seed are checked as
   well. A kick whose only primitives were dropped (FRAME.FBW 0) is not drawn and not judged; kicks that both upload and
   draw would not be judged (none exist, 14.2). The b4 menu dumps `b4-key` / `b4-key-0` hold no drawing kick at all.
2. **L counts coverage, not boxes.** 4.4 defined L by the scissored bounding box; K1 draws every rectangle as two
   triangles with the same box, so that L is twice the layers (the first run gave a largest L of 12 on the front end
   against 6 by coverage). L is the number of inexact primitives whose pixels, by the reference's own rules (edge
   functions with the top-left bias, Bresenham for lines, the sprite rectangle), inside the scissor, include the pixel.
   An alpha-test-failing pixel still counts (an upper bound). No flat-source exemption (4.4's last paragraph): with an
   8-bit blender the dimming boxes are not exact (16.2).
3. **The bound** is the decision's: per channel `-L <= Vulkan - reference <= 2L` with the bias off, `-L <= ... <= L`
   with it on; alpha and Z (low 24 bits) exactly equal everywhere, which also makes every L = 0 pixel exact. Both
   settings must pass for `PASS`; guards firing fail (c) as 10.3 asks (none fired).
4. **Seeding as built in batch 2** (`Gs2dVk::seed`: colour and Z from CPU planes in one full-target draw), from the
   emulated GS memory read with `gs_layout` (CT32 at FBP 0x0 / 0x50 for each target the kick draws into, Z32 layout at
   ZBP, the low 24 bits); the emulated memory after the kick, read the same way, is the reference. No word of the Z
   buffer had a non-zero top byte before any kick. Depth is read back for the whole plane (15.1 item 7).
5. **The kick's sub-list:** the state in force at the kick's first command (STATE is emitted only on a change), every
   TEXTURE command not yet given to the GPU (TEXTURE is sent once per generation, 14.1 item 5), then the kick's own
   commands. One `Gs2dVk` serves both bias settings (`Options::sourceBias` is a per-record flag).
6. **A first failure** prints the cycle, kick, pixel, the Vulkan, reference and seed values, the channel and its
   bound, and every primitive covering the pixel with its state and plan; `--vk-png` writes the kick's before, Vulkan,
   reference and difference pictures.

### 16.2 Results (RTX 2060, driver 620.70.06, validation layer on for every dump; 67 / 67 `PASS`)

`rc_replay <dump> --consumer gif2d --vk --validation --verify --quiet` (checks (a) and (b) in the same pass, both still
holding everywhere). 2 h 36 min of process time, 4 at a time (45 min); about 14 ms of GPU time a kick per setting.

| | USA s5 (28) | USA s6 (2) | USA b4 (4) | Europe s5 (27) | Europe s6 (2) | Europe b4 (4) |
|---|---|---|---|---|---|---|
| drawing kicks judged | 43,119 | 2,079 | 1,679 | 37,839 | 2,081 | 1,624 |
| pixels compared (each setting) | 7.065 G | 0.341 G | 0.275 G | 6.200 G | 0.341 G | 0.266 G |
| out of the bound; alpha, Z differing | 0; 0, 0 | 0; 0, 0 | 0; 0, 0 | 0; 0, 0 | 0; 0, 0 | 0; 0, 0 |
| bias off: pixels not equal (within the bound) | 23,425,183 | 9,554,668 | 1,362,964 | 18,867,273 | 6,766,257 | 1,598,009 |
| bias off: channels differing; range | 36.28 M; -1..+2 | 14.24 M; -1..+2 | 2.04 M; -1..+2 | 28.28 M; -1..+2 | 9.75 M; -1..+2 | 2.36 M; -1..+2 |
| bias on: pixels not equal (within the bound) | 21,204,076 | 8,686,897 | 1,217,209 | 17,110,354 | 6,252,219 | 1,454,099 |
| bias on: channels differing; range | 31.99 M; -1..+1 | 12.85 M; -1..+1 | 1.81 M; -1..+1 | 24.76 M; -2..+1 | 8.81 M; -2..+1 | 2.09 M; -2..+1 |
| largest L | 7 | 6 | 7 | 7 | 6 | 6 |
| validation errors | 0 | 0 | 0 | 0 | 0 | 0 |

In all: 88,421 kicks, 14.49 G pixel-kicks, 43.5 G colour channels. Pixels under no inexact layer (L = 0, 14.36 G):
equal in all four channels and Z with either setting, which covers every integer-exact form (the clear, untextured
and textured blend-off draws, the Track Editor's lines, the flare's `Cd+Cs` and the `Cd-Cs` form). The largest
|d| / L is 2.00 with the bias off (a +2 under one layer: 4,834 channels at L 1) and 1.00 with it on; the bias-on -2
are 129 channels at L 2 (Europe). Largest L 7: USA Track Editor (cycle 1984 kick 197) and `b4-key-1` (cycle 468),
Europe Track Editor (cycle 1903).

**Difference by state** (the state of the last inexact primitive covering the pixel; both versions, channels):

| state | pixels with L > 0 | bias off: equal; -1 / +1 / +2 | bias on: equal; -2 / -1 / +1 |
|---|---|---|---|
| text, `0x09` `Cd*As+Cs`, LEQUAL 0x3f | 12.88 M | 80.3%; 0 / 4.55 M / 0 | 77.1%; 0 / 5.76 M / 0.07 M |
| panels, `0x01` `(Cd-Cs)*As+Cs`, RGB_ONLY split | 15.63 M | 19.9%; 0.02 M / 21.05 M / 0.013 M | 43.6%; 129 / 6.42 M / 5.91 M |
| dimming boxes, `0x21` FIX 0x20 (flat colour) | 98.92 M | 53.4%; 0 / 66.28 M / 0 | 55.6%; 0 / 13.42 M / 50.31 M |
| `0x21` FIX 0x3f, textured (USA) | 0.52 M | 10.2%; 0 / 1.03 M / 0.013 M | 46.5%; 0 / 0.06 M / 0.36 M |
| all inexact | 127.95 M | 51.9%; 92.95 M channels, sum of \|d\| 92.98 M | 56.3%; 82.32 M channels, sum of \|d\| 82.32 M |

`0x21` FIX 0x40 (1,798 primitives) never ends up as the last inexact layer of a pixel in these dumps. **Which bias
gives the smaller real error:** on the dumps the bias on differs on 11% fewer channels (82.3 M against 93.0 M) with a
smaller maximum (|d| <= L against 2L), a lower total |d|, more equal pixels under blending (56.3% against 51.9%) and an
error of both signs; off, the error is one-signed (+1, the blender rounding up) and smaller for the text (4.55 M
channels against 5.83 M). The panels' lerp gains most from the bias (21.1 M -> 12.3 M); the dimming boxes, most of
the blended area, little (66.3 M -> 63.7 M). Either way 0.2% of all colour channels differ, by one level but for
26,000 channels by two. For the review: the default (off as built; on would be the smaller error) and U1.

### 16.3 Planted faults (9.3; `--vk-fault`, USA front end `s5/front`, 900 cycles, both bias settings)

| fault | (c) | out-of-bound pixels (off / on); alpha, Z differing | failing kicks |
|---|---|---|---|
| ATST LEQUAL as LESS | fails | 1,302,500 / 1,302,500; alpha 1,302,499, Z 1,296,600 | 600 / 600 |
| the failing part of the split dropped | fails | 1,673,017 / 1,946,570; 0, 0 | 600 / 600 |
| the split per batch instead of per group | passes | 0 / 0 | 0 (9.3 asks only T3 to catch it; on this dump the order inside a batch changes no pixel) |
| ZTST GREATER as GEQUAL | fails | 22,256 / 27,053; 0, 0 (text at equal Z) | 600 / 600 |
| the half pixel left out of the position | fails | 425,152 / 431,680; alpha 56,256, Z 379,308 | 747 / 747 |
| FIX off by one | fails, narrowly | 265 / 265; 0, 0 (13.97 M pixels differ but stay inside the widened bound, against 9.09 M without the fault) | 49 / 49 |
| a vertex colour's R and G swapped | fails | 940,788 / 954,071; 0, 0 | 600 / 600 |
| the texel fetch one row off | fails | 8,709,019 / 8,985,749; alpha 3,170,802, Z 1,706,291 | 743 / 743 |
| the bias applied where a < b | passes | 0 / 0 | the fault acts only with the bias on, where it stays inside -L..+L (as in 15.4, the bound cannot see it) |
| TEXA ignored (the reader's decode) | passes, also on `s5/l13` (the CT16 quad) | 0 | as 14.4: TEXA never changes a picture in these dumps |

So check (c) fails for the faults 9.3 names for it (the dropped failing part, GEQUAL, the half pixel, FIX, the texel
row, the colour swap). The widened bound makes FIX off by one visible on 265 pixels only, where the design's 0..+L
would have seen far more: a cost of D-2D-1's width the review should weigh.

### 16.4 The game and the oracle

Nothing new is linked into either: the changed and new sources belong to `rc_replay`, `rc_tex` and `rc_tex_test`, and
the game's and the oracle's build commands name none of them (`ninja -t commands`). The CMake re-run regenerated a
shader header (`generated/trdr_shape_glsl.h`) by timestamp, so `ninja` recompiled two unchanged game objects and
relinked `rc_revenge_pro` in both trees; the regression checks were run for that: `tools/oracle.sh --all` USA (seed
7331) 1632 ok and Europe (seed 7332) 1563 ok, 0 FAIL; `playtest2` replays reach the race scene 3020 at frame 658 (USA)
and 1167 (Europe).

### 16.5 Open

- **For the batch 4 review (Fable):** the bias default (16.2) and U1 (exact destination read), with D-2D-1's width
  (the FIX fault nearly hidden) as an argument for U1.
- The design's 4.4 argument that a later layer only scales an earlier excess holds empirically under the widened
  bound (no pixel out of it at L up to 7); it is not proved for the 8-bit blender.
- The live half (batch 5) and scenes not in the dumps (13).

## 17. Batch 4: review (2026-11-20)

Section 11 item 4. Reviewed: sections 14-16 against sections 3-13, the code of `render/gs2d` and
`tools/rc_replay/gif2d_vk.cpp` where a decision depended on it, the vendored RHI's destination-read interface, and
the device (`vulkaninfo`, the RHI's device line). Run for evidence, on the RTX 2060: check (c) on USA `s5/front`,
`s5/boot`, `s5/tedit`, `s5/l17` and `b4-key-1` with a temporary counter in `Gs2dVk::build` (reverted, not
committed; all five `PASS` as before). Nothing was built.

**Outcome.** (1) The bias is on, for the fixed-function blend. (2) U1 is built now, in a cheaper form than the one
batches 2 and 3 weighed: the destination is read as an input attachment under local read, with a barrier before
each group of non-overlapping primitives; depth stays fixed function. Section 18 specifies it as batch 4b. (3) The
order is 4b, step 8's design, step 8's first batch (off-line), then batch 5; the orchestrator's order (step 8
before batch 5) is confirmed with one refinement (17.4).

### 17.1 The bias default: on

| | bias off | bias on |
|---|---|---|
| per layer; per pixel | -1..+2; `-L..+2L` | -1..+1; `-L..+L` |
| channels differing (67 dumps) | 92.95 M | 82.32 M (11% fewer) |
| signed sum of the differences (16.2's table) | about +93 M (everything upward) | about +31 M (text -5.7 M, panels -0.5 M, boxes +36.9 M, `0x21` FIX 0x3f +0.3 M) |
| pixels under an inexact layer that are equal | 51.9% | 56.3% |
| text (`0x09`) | +1 on 4.55 M channels | -1 on 5.76 M, +1 on 0.07 M |

On, because: the worst case is halved, which is also the tighter bound for the check that remains on this path;
the total error and the net brightening are smaller (a stack of layers drifts upward by a third as much); and on a
blender wider than 8 bits the bias is the setting that gives 4.3's near-exact result, where off gives +1 on 35-47%
of the triples (15.1 item 10), so on is right or no worse on every device, and off is right on none. Against it
stands only the text: 28% more differing channels (5.83 M against 4.55 M), each one level on a glyph's blended
edge, of the opposite sign. That is not visible and not a reason to keep the larger bound. The design's rule "T1
fails with the bias, so switch it off" (4.3) was written for a blender that would be exact without ties; it does
not apply to an 8-bit one and is withdrawn.

This decides the fixed-function path only. After batch 4b that path is the fallback for a device without local
read and an option for comparison; the default path does not use the bias at all.

### 17.2 U1: build the exact blend now, by local read

**What is wrong with staying on fixed function.** Not the picture: one level on 0.2% of the channels is invisible.
The check is the cost. This project's renderer steps have each been closed by a bit-exact comparison, and those
comparisons have found real errors that a tolerance would have passed (1/Q one ULP off on 4% of pixels, 15.1 item 5;
a single lost RGBAQ write worth 14 pixels, 14.1 item 10). With D-2D-1 the colour criterion over every blended pixel
is a band of width 3L+1 or 2L+1, and 16.3 shows what that hides: FIX off by one changes 13.97 M pixels on the front
end and 265 of them leave the band. An error of one unit in a blend factor, a selector that is wrong only where it
moves the result by a level, or a wrong layer order between two near-equal layers would pass. And the bound itself
is measured, not proved, for an 8-bit blender (16.5), and is a property of one driver: another device may round
differently, and the check would then fail or pass for reasons unrelated to the renderer.

**What batches 2 and 3 did not consider.** They weighed the interlock form (target and depth as storage images, Z
test in the shader), because the RHI reports "rasterization-order attachment read no". But the RHI has a third
form, used by ico-pc for DATE: under `VK_KHR_dynamic_rendering_local_read` a pass may read its own colour
attachment as an input attachment, and sees every earlier write once a by-region barrier has been issued inside
the pass (`RhiLimits.localRead`, `RhiColorAttachment.readable`, `RHI_BIND_INPUT_ATTACHMENT`,
`rhi_cmd_local_read_barrier`). The RTX 2060 has it (device line: `local read yes`; the Vega too). What it needs
that raster-order access does not is that no pixel is written twice between two barriers, and K1 already has that
structure: `Gs2dVk` cuts split batches into groups of primitives that cannot overlap (15.1 item 4). So:

- the depth test, depth write and the alpha-test split stay exactly as built and tested (T3, T4, T5);
- the integer-exact forms stay on fixed function (equal on 14.36 G pixel-kicks) and need no barrier;
- only the fractional forms change: blend disabled, the shader reads Cd and computes the GS rule in integers.

**Cost, measured.** Groups of inexactly blended primitives per drawing kick, by the grouping rule as built:

| dump (USA) | kicks with an inexact primitive | inexact primitives | groups | largest in a kick |
|---|---|---|---|---|
| `s5/front` | 743 | 146,344 | 69,572 | 115 |
| `s5/boot` | 630 | 150,822 | 71,673 | 115 |
| `s5/tedit` | 700 | 102,624 | 47,843 | 119 |
| `b4-key-1` | 100 | 19,454 | 9,223 | 116 |
| `s5/l17` (race HUD) | 349 | 2,792 | 1,396 | 4 |

2.1 primitives a group: a group is one rectangle, because a batch is nearly always one rectangle. So at most about
120 barriers in a menu frame and 4 in a race frame (512 for the largest K1 the survey saw, 1,024 primitives). On a
desktop GPU a by-region barrier inside a pass costs microseconds; no effect on the frame rate is expected, and
18.5 has the acceptance figure. The build is one Opus batch on the existing structure (one more blend
configuration, 64 pipelines, one shader build, a flag per draw), smaller than the interlock form's estimate.

**Step 8 and after.** The 3D path has the same forms: `0x09` 0.8%, `0x01` 1.3%, `0x29` FIX 0x7f 0.6% of PATH1's
primitives (survey, "Draw states"), about 240 primitives of the 8,900 in an average cycle, and they are effects
that overlap one another, so about one barrier each: a few hundred a frame, the range PCSX2's hardware renderer
works in with the same technique. With the exact blend, step 8's Vulkan check (frame record 10.2 row 8 (d),
"pictures against `REFI` with difference statistics") can be equality, and a whole dump can be replayed without
reseeding and compared picture by picture (17.3 item 6), which fixed function cannot offer. Whether K2's default
at speed (P2 step 9) is the exact blend or fixed function is step 9's decision with step 8's barrier counts; the
interlock form remains the reserve if they are too high, and is not built now. P3 (scaling, GPU interpolation)
gives up equality with the reference anyway and can use either; local read works at any resolution and the device
reports it for multisampled attachments.

**Why now and not at P2 or P3.** Batch 5 and step 8 are both judged by pictures. Built before them, the exact
blend makes those judgements equalities and removes L, the two bias runs and the bound from every later check;
built after, each of those steps is accepted on a band and has to be re-verified later. Nothing in the end-state
goal argues against it: the GS's blend rule is the game's picture, not hardware emulation, and the mode reads no
emulated state.

**Never** is the wrong answer for the reasons above; **later** saves one Opus batch now and costs weaker checks on
two steps.

### 17.3 Other findings in batches 1-3

1. **Sound.** Checks (a) and (b) are exact and complete on the 67 dumps; the model and T2-T7 are bit-exact against
   the emulated GS; the planted faults fail where the design says. The three changes of criterion (per kick, L by
   coverage, the widened bound) are justified in 16.1. No correctness error found in the as-built sections or in
   the code read.
2. **`build-eu/rc_replay` is stale** (06:11 on 2026-11-20, before batch 1: its usage line has no `gif2d`). The
   Europe dumps of 14.2 and 16.2 were run with `build-decomp/rc_replay`, which is correct (the tool does not depend
   on the game version) but is not what "both trees" suggests. Batch 4b: either build `rc_replay` and
   `rc_gs2d_test` in `build-eu` as well or say in `NOTES.md` that the tools are built in `build-decomp` only.
3. **A stale reference in the code:** `gs2d_vk.h`'s comment on `sourceBias` cites "RENDERER_2D_DESIGN.md 14.7";
   it is 15.3. Batch 4b changes that option and corrects the comment.
4. **Drift between `planState` and check (c):** the plan still classifies by 4.3's premise (`flatSourceExact`: the
   dimming boxes "exact", 14.3), which 15.3 disproved; check (c) rightly ignores it (16.1 item 2). After 4b the
   function has no user in the default path; it should be removed or marked as holding only for a blender wider
   than 8 bits, so step 9 does not reuse it.
5. **Faults are planted on one dump** (USA `s5/front`) and TEXA is not exercised by any dump (14.4, 16.3: no
   picture depends on it; T6 covers the decode). Acceptable; 18.4 re-runs the faults with the exact criterion, and
   adds the Track Editor for the two new ones.
6. **Check (c) reseeds before every kick,** so it never tests two kicks in a row through Vulkan: the clear followed
   by K1, textures and state carried across kicks, the two targets alternating. Between the clear and K1 of one
   buffer lies the 3D kick, so a chained check needs step 8. It is the natural final check there, and only
   possible with the exact blend: seed GS memory once at a keyframe, replay every kick through Vulkan, and compare
   every flipped picture with `REFI`. Added to step 8's outline (RENDERER_PLAN.md item 8).
7. **Scenes not in the dumps** (13: split screen, podium, results, memory card screens, the Track Editor's race)
   are still unrecorded. The guards report a new state at once in a live session, but the exact check wants dumps:
   record them when batch 5 gives a session that reaches them, before step 7 is called closed.
8. **4.4's argument is not a proof for an 8-bit blender** (16.5). It no longer carries the default path's check;
   D-2D-1 now says the fallback's bound is measured.

### 17.4 Order of the remaining work, and step 8

**Confirmed: step 8's reader before batch 5.** 14.1 item 9 is right that K1 inherits registers only PATH1 writes
(ZBUF_1 on the race HUD and the USA front-end backdrop; SCISSOR_1, TEST_1 and TEX1_1 in the Track Editor; FBA, PABE
and CLAMP harmlessly), and the alternative, seeding from the runtime's register shadow (`g_gsRawRegs`), is exact
only while the emulated GS is synchronous with the EE and makes the new renderer read emulated hardware state, the
opposite of where the plan is going. PATH1's register values come out of the VU1 programs' packets (the state
packets and each program's GIF tag prefix), so there is no shortcut that reads them from the VIF chain without
running the references: it needs step 8's reader.

**Refinement: batch 5 needs only step 8's first batch,** the off-line reader (VIF chain, VU memory, the C++
references, GIF bytes equal to the emulated VU1's) with check (a) extended across VIF1 kicks. It does not need K2
drawn by Vulkan. Doing batch 5 at that point (2D over black in races, complete menus) exercises the live plumbing
(the reader on the EE thread, the list published at the cycle close, the presenter) that step 8's own live half
then reuses, instead of meeting it for the first time with the 3D on top.

1. **Step 7 batch 4b** (Opus): the exact blend by local read, section 18. Independent re-check as usual.
2. **Step 8 design** (Fable; a short document, `RENDERER_3D_DESIGN.md`): outline in RENDERER_PLAN.md item 8. It is
   needed: the reader's VU side has choices the frame record's test row does not settle (below), and the K2 state
   table of 4.2 has to be turned into plans and guards before anything is drawn.
3. **Step 8 batch 1** (Opus): the VIF reader with the references, off-line; frame record 10.2 row 8 (a), (b) and
   (c), and `gif2d`'s check (a) with no register left unknown after a VIF1 kick.
4. **Step 7 batch 5** (Opus): live 2D as section 11 item 5, PATH1's registers from the live VIF reader.
5. **Step 8 batches 2 and on:** K2 through Vulkan off-line (check (c) per kick, then the chained replay), then live.

What step 8's design must settle is listed in RENDERER_PLAN.md item 8.

## 18. The exact blend by local read (batch 4b: specification)

For the Opus build agent. Read with sections 4, 5.2, 9, 10.3, 15 and 16. Scope: `render/gs2d/gs2d_vk.{h,cpp}`,
`gs2d.frag` (one more build of it), `gs_model` (a function at most), `rc_gs2d_test`, `tools/rc_replay/gif2d_vk.*`
and `rc_replay.cpp`'s options. Not in scope: the reader, the draw list, the encoder, anything live, K2, the game,
the runtime. The fixed-function blend and the bias stay, as the fallback and for comparison.

### 18.1 The mechanism

A draw's fragment shader may read its own pixel of colour attachment 0 as an input attachment when the pass was
begun with that attachment readable; it then sees every write made before the last barrier inside the pass, and
its own write to the same pixel is defined. So if (i) a barrier is issued before a group of reading primitives
whenever anything has been drawn in the pass since the previous barrier, and (ii) no two primitives of the group
write the same pixel, every reading fragment sees exactly the frame buffer the GS would have at that primitive.

RHI, all present (no vendored edit is expected; if one proves necessary, mark it `RC:` and list it in
`VENDORED.md` as U2's was):

- the two colour targets created with `RHI_TEX_INPUT_ATTACHMENT` as well; the pass begun with
  `pass.color[0].readable = true`, the target in `RHI_STATE_RENDER_TARGET_READ`;
- a bind slot `RHI_BIND_INPUT_ATTACHMENT` holding the target (one bind group per target), in the reading
  pipelines' layout (group 0 is empty today; the builder chooses the place);
- `rhi_cmd_local_read_barrier(cl, RHI_LOCAL_READ_COLOR)`; add `RHI_LOCAL_READ_BACK` if the validation layer asks
  for the read-to-write ordering;
- the reading pipelines have `dstRead = RHI_DST_READ_NONE` (`RHI_DST_READ_ATTACHMENT` is the rasterization-order
  form and fails on this device), blend disabled, one colour output.

The condition is `RhiLimits.localRead`. ico-pc's use of the same path is DATE's setup draw (`rhi.h`, the comment
on `rhi_cmd_local_read_barrier`).

### 18.2 What changes in `Gs2dVk`

- **Option.** `Options::blend`: `kBlendAuto` (default: the read where `localRead`, else fixed function), `kBlendFixed`,
  `kBlendRead` (`create()` fails with a reason without `localRead`). `Options::sourceBias` default becomes true and
  applies to fixed function only (17.1); the bias flag is not set on a reading record.
- **Which plans read.** Those with `blend != kBlendOff && !exactBlend` (4.2's rows with a fractional factor: `0x09`,
  `0x29` FIX f, `0x01`, `0x21` FIX f, `0x44` without PABE, `0xa1` FIX f). Everything else is drawn as today,
  through the fixed-function pipelines, in the same pass. A plan whose guard `BLEND_FORM` fired (a destination
  coefficient fixed function cannot express) or that uses C = Ad may be drawn by the read as well, since the
  shader below is the general rule; then the guard does not fire in this mode. Optional, and only with its T1 rows.
- **The shader** (`GS2D_DSTREAD` build of `gs2d.frag`, for triangles and for points): everything up to the alpha
  test unchanged. Then, in integers: `Cd = ivec4(subpassLoad(dst) * 255.0 + 0.5)`; per colour channel
  `Cv = clamp((((A - B) * C) >> 7) + D, 0, 255)` with A, B, D chosen from Cs, Cd, 0 and C from As, Cd.a, FIX by the
  ALPHA selectors already in the record (word 36, `abcd` and `fix`), the shift arithmetic on a signed int;
  `out0 = vec4(vec3(Cv) / 255.0, alpha / 255.0)`. Alpha is As, except for an `alphaByBlend` plan (RGB_ONLY, no
  depth write), where a failing pixel writes `Cd.a` (so this mode has no `SRC1_ALPHA` variant). `gl_FragDepth` as
  now.
- **Groups and barriers.** Every batch of a reading plan is cut into groups by the rule the split uses (scissored
  boxes pairwise disjoint, the second half of a rectangle pair allowed to touch the first; for points, distinct
  pixels), whether or not the plan splits; a split plan keeps its two draws per group (they write disjoint pixels:
  no barrier between them). The first draw of each reading group carries "barrier before", issued unless nothing
  has been drawn in the pass since the last barrier or the pass began. Draws that do not read never need one.
- **Pipelines.** One more configuration, `READ`: {triangles, points} x depth compare (4) x depth write (2) x
  colour mask (4) = 64, made at `create()` with the others when `localRead`.
- **Depth, the seed, the clear, lines, textures, read-back:** unchanged. The depth attachment is not readable.
- **Stats:** reading primitives, reading groups, barriers issued, per list and in total.

### 18.3 The model and `rc_gs2d_test`

`gs_model` already has `gsBlend`; the mode's expected value is `gsBlend`, nothing else. `rc_gs2d_test --blend
read|ff|both` (default both where the device has local read):

- **T1, read:** every row of 4.2 (and of the optional forms, if drawn) x every As or FIX of the present T1 x Cs, Cd
  0..255: Vulkan = `gsBlend` = emulated on every channel of all 44.2 M pixels; stored alpha = As.
- **T1, fixed function:** the criterion becomes D-2D-1's measured one (per channel -1..+1 with the bias, -1..+2
  without; the integer-exact rows equal), so a plain run ends `PASS` and `--no-design-bound` goes.
- **T2-T6, read:** all planes emulated = model = Vulkan, T2's stacks included (no L). T3 gains the panels' state
  (`0x01`, LEQUAL 1, RGB_ONLY, Z write on) on its textured alpha ramp and overlapping rectangles: the split and
  the read together.
- **T8 (new), order:** (i) at least 16 mutually overlapping rectangles of a reading state in one batch, colours and
  alphas chosen so that every order gives a different picture; (ii) the same pixels drawn by an alternation of
  fixed-function and reading states (`0x29` FIX 0x80, `0x01`, `0xa1` FIX 0x80, `0x09`, blend off) in separate and
  in shared batches; (iii) a reading batch of 256 non-overlapping rectangles (one group) over a patterned
  destination; (iv) both targets alternately. Emulated = model = Vulkan in all planes.
- **Planted faults:** the barrier left out, and a reading batch drawn as one group, each must fail T8 (i); FIX off
  by one must fail T1's reading rows as well as the integer-exact ones. The bias fault does not apply to this mode.
- The validation layer on for the run: 0 errors.

### 18.4 Check (c)

`rc_replay ... --consumer gif2d --vk [--vk-blend read|ff|both]`, default `read` where the device has local read,
else `ff`; `--vk-bias` applies to `ff` and defaults to `on`.

**Pass, read:** for every judged kick, every one of the 640 x 256 pixels equal to the reference in R, G, B, A and
Z. No L, no bound. On all 67 exact dumps of both versions, validation layer on, 0 errors. Reported per dump: kicks,
pixels compared, pixels differing (0), reading primitives, groups and barriers (total, largest in a kick), GPU
time per kick beside the fixed-function figure. A first failure prints what 16.1 item 6 prints.

**Fixed function** keeps 16.1's criterion (bias on: `-L..+L`) and is run on eight dumps as the fallback's
regression: `s5/front`, `s5/boot`, `s5/tedit`, `s5/l17` of each version.

**Faults, read mode,** on USA `s5/front` (and `s5/tedit` for the two new ones): atst-less, drop-fail, z-gequal,
no-half-pixel, fix, colour-swap, texel-row must each fail; `fix` on more than a million pixels (16.3: 13.97 M
pixels differ under it), which is the point of the batch; no-barrier and one-group must fail on the front end
(panels over the backdrop, text over panels). split-per-batch and TEXA remain T3's and T6's.

### 18.5 Cost: expectation and acceptance

Measured in the review (17.2): at most 119 reading groups in a kick on the menu and Track Editor dumps, 4 in a
race; 2.1 primitives a group. Expected: GPU time per kick in check (c) within 10% of fixed function's (it is
dominated by the seed and the read-back), and well under 0.5 ms a frame live. **Acceptance:** the figures are
reported; if a kick's GPU time in read mode is more than twice fixed function's, stop and report rather than
optimise (the first lever would be to continue a group across batches whose states both read).

### 18.6 Fallbacks

1. No `localRead`: fixed function with the bias on, one log line naming D-2D-1; `kBlendAuto` selects it.
2. If the device or the validation layer objects to dual-source pipelines drawing in a readable pass: draw every
   blended plan through `READ` (the shader is the general rule), leaving blend-off plans as they are; say so in the
   as-built section. Splitting the pass at each change of kind is the second choice (a pass per run).
3. The interlock form of the first U1 is not built. It is reconsidered only if step 8 measures K2 barrier counts
   that make the local read too slow for the 3D (17.2).

### 18.7 Also in the batch

17.3 items 2, 3 and 4; the as-built section (19) with the results, 3.3's "as built" rows for the exact mode,
D-2D-1 reworded to "devices without local read, or `kBlendFixed`", `NOTES.md` ("The 2D packet on Vulkan", "Check
(c)"), RENDERER_PLAN.md item 7. The game links nothing new (`Gs2dVk` is not in it yet): show it as 16.4 does; run
the oracle and `playtest2` only if a game object is rebuilt.

## 19. Batch 4b as built (2026-11-20): the exact blend by local read

Section 18, built. The fractional blend forms are drawn by reading the target as an input attachment under dynamic
rendering local read, with a by-region barrier before each group of primitives that cannot overlap; everything else
is as batches 2 and 3 built it. No RHI edit was needed. Usage: `NOTES.md`, "The 2D packet on Vulkan" and "Check (c)".

```
render/gs2d/gs2d.frag        GS2D_DSTREAD build (gs2d_read_frag.h): Cd by subpassLoad, the GS rule in integers, one output
render/gs2d/gs2d_vk.{h,cpp}  Options::blend (kBlendAuto / kBlendFixed / kBlendRead), sourceBias default on; the READ
                             pipelines (64), group 0 with the target as an input attachment, readable passes, groups and
                             barriers for reading plans, two faults (no-barrier, one-group), Stats / ListStats (reading
                             primitives, groups, barriers, GPU time of a list by timestamps)
render/gs2d/gs_plan.{h,cpp}  flatSourceExact removed (17.3 item 4)
tools/rc_replay/gs2d_test.cpp --blend read|ff|both; T1 read and ff criteria; T2 read; T3 panels; T8; the new faults; a
                             synchronisation-validation probe
tools/rc_replay/gif2d_vk.*   settings read / ff bias off / ff bias on; --vk-blend, --vk-bias default on; cost figures
CMakeLists.txt               the GS2D_DSTREAD shader build; the track shader header written only when it changes
```

### 19.1 Changes to the specification, and why

1. **Every pass is begun readable in read mode** (the target in `RENDERING_LOCAL_READ`), and the fixed-function
   pipelines (dual-source included) draw in it beside the READ ones. Neither the device nor the validation layer
   objects, so 18.6's second fallback was not needed.
2. **The READ pipelines have their own group 0 layout** (slot 0, `RHI_BIND_INPUT_ATTACHMENT`, binding 16); groups 1
   and 2 are shared with the fixed-function pipelines, and group 0 is rebound at each draw (empty or the target's).
3. **A reading plan without the split is drawn one draw per group**, a split one two draws per group as before; the
   first draw of a group that emits anything carries the barrier flag. The barrier is `RHI_LOCAL_READ_COLOR` only:
   the next group's barrier (source colour output, which implicitly includes the earlier fragment-shader reads;
   destination fragment shader, which implicitly includes the later colour output) also orders the earlier reads
   before the later writes, and a fixed-function draw after a reading one writes in rasterization order after that
   draw's own write. The validation layer never asked for `RHI_LOCAL_READ_BACK` (19.4).
4. **C = Ad and the BLEND_FORM rows (18.2's optional part) were not drawn by the read.** The shader evaluates the
   general rule including C = Ad, but the plan still replaces Ad by FIX 0x80 and fires its guard; no RC draw uses
   either.
5. **GPU time is measured by timestamps** around a list's passes (`Gs2dVk::ListStats::gpuMs`, `rhi_cmd_write_timestamp`
   before the first pass and after the last; read back two frame slots later, the GPU being idle). Batch 3's "about
   14 ms of GPU time a kick" was the wall time of the seed, the draw and the read-back; both are reported now.
6. **The fault runs of `rc_gs2d_test` compared against a model they had not run.** With the model left out (as the
   fault runs do), `threeEqual` still compared the emulated planes with the empty model planes, so T3-T6 failed for
   that reason whatever the fault. Fixed (`Result::modelRun`); every fault is still caught, now by Vulkan alone.
7. **Synchronisation validation is probed**: with `--validation`, `rc_gs2d_test` records two clears of one image with
   no barrier between them (natively; the RHI orders its own copies) and reports whether that hazard was reported
   (`VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true`; the probe's error is not counted).
8. **The track shader's generated header** (`generated/trdr_shape_glsl.h`) is now written only when it changes: every
   CMake re-run used to touch it and recompile `track_draw.cpp`. (The game still relinks after a re-run, since
   `capture.cpp` embeds `git describe`.)

### 19.2 `rc_gs2d_test` (RTX 2060, driver 620.70.06; `--validation` with synchronisation validation; 372 s, 3,530 scenes)

| test | what ran | result |
|---|---|---|
| sync probe | two clears of one image without a barrier | reported (1 error, not counted): synchronisation validation is on |
| model | 44.2 M (Cs, Cd, F) triples | pass, as before |
| T1 read | every row of 4.2 x every As (0..0x80) or FIX x Cs, Cd 0..255 (675 scenes, 44,236,800 pixels) | **pass: Vulkan = `gsBlend` = emulated on every channel of every pixel**, stored alpha As |
| T1 ff, bias on / off | the same, fixed function | pass under D-2D-1's bound (-1..+1 / -1..+2; the integer-exact rows equal): 26.3 M / 22.9 M pixels equal in all channels; per-row histograms as 15.3 |
| T2 read / ff | two six-layer stacks x four destinations | read: all planes emulated = model = Vulkan; ff: within -L..+L ((Vulkan - GS) / L from -0.50 to +0.50, largest L 10) |
| T3 (read) | 320 alpha-test configurations as before, plus the panels' state (`0x01`, LEQUAL AREF 1 / 0x2f / 0x3f, RGB_ONLY, Z GREATER with Z write, bilinear, MODULATE and DECAL) on the ramp and the overlapping rectangles, both targets: 12 scenes | pass: all planes equal |
| T4-T6 (read) | as before | pass |
| T7 | as before | pass: 521,450 pixels bit-equal |
| T8 (read) | (i) 20 mutually overlapping rectangles in one batch for each of `0x01`, `0x09`, `0x21` FIX 0x20, `0x29` FIX 0x3f, `0x44`, `0xa1` FIX 0x55, at Z ALWAYS and at rising Z under GREATER with Z write (12 scenes); (ii) 21 batches alternating `0x29`/0x80, `0x01`, `0xa1`/0x80, `0x09`, blend off, `0x21`/0x20, `0x68`/0x33 over the same pixels, one rectangle a batch and four a batch; (iii) 256 disjoint rectangles in one reading batch (one group), `0x01` and `0x09`, over a patterned destination; (iv) 24 overlapping reading rectangles alternating FBP 0x0 and 0x50 | pass: all planes emulated = model = Vulkan (17 scenes) |
| validation | the whole run | 0 errors |

Planted faults (the fault runs in read mode):

| fault | test | result |
|---|---|---|
| ATST LEQUAL as LESS; the failing part dropped; the split per batch | T3 | fail |
| ZTST GREATER as GEQUAL | T4 | fails |
| the half pixel left out | T5 | fails |
| FIX off by one | T1 | **fails on 15 reading rows and 13 integer-exact rows** (18.3 asks both) |
| TEXA ignored; the texel row; R and G swapped | T6 | fail |
| the barrier left out | T8 | fails (14 of the 17 scenes differ) |
| a reading batch drawn as one group | T8 | fails (13 of 17) |
| the bias where a < b | - | not applicable to the read; on fixed function D-2D-1's bound cannot see it (15.4, 16.3) |

### 19.3 Check (c): equality on the 67 exact dumps

`rc_replay <dump> --consumer gif2d --vk --vk-blend read --verify --quiet --validation` with synchronisation validation,
4 at a time (33 min). Checks (a) and (b) ran in the same pass and still hold everywhere.

| | USA s5 (28) | USA s6 (2) | USA b4 (4) | Europe s5 (27) | Europe s6 (2) | Europe b4 (4) |
|---|---|---|---|---|---|---|
| `PASS` | 28 | 2 | 4 | 27 | 2 | 4 |
| drawing kicks judged | 43,119 | 2,079 | 1,679 | 37,839 | 2,081 | 1,624 |
| pixels compared | 7,064,616,960 | 340,623,360 | 275,087,360 | 6,199,541,760 | 340,951,040 | 266,076,160 |
| pixels differing (R, G, B, A or Z) | **0** | **0** | **0** | **0** | **0** | **0** |
| reading primitives | 476,686 | 154,528 | 22,434 | 383,272 | 105,310 | 26,776 |
| reading groups / barriers | 227,536 / 217,764 | 73,472 / 73,269 | 10,713 / 10,308 | 181,359 / 171,702 | 49,589 / 48,817 | 12,788 / 12,418 |
| most groups (barriers) in one kick | 119 (119) | 115 (115) | 116 (116) | 115 (114) | 95 (94) | 116 (116) |
| validation errors | 0 | 0 | 0 | 0 | 0 | 0 |

In all 88,421 kicks and 14,486,896,640 pixel-kicks, every one equal to the reference; 1,169,006 reading primitives in
555,457 groups, 534,278 barriers (a barrier is skipped where nothing was drawn since the pass began or since the last
one). The group counts are the review's (17.2: front end 69,572 groups for 146,344 primitives, as measured there).

**Fixed function (the fallback's regression, `--vk-blend ff`, bias on, 18.4):** `s5/front`, `s5/boot`, `s5/tedit` and `s5/l17` of each version
all `PASS` (12,134 kicks, 1.92 G pixel-kicks): alpha and Z equal everywhere, colour within `-L..+L`, worst -1..+1
(USA) and -2..+1 (Europe), as in 16.2. The fallback is unchanged.

**Faults, read mode (USA `s5/front`, 900 cycles, 1,790 kicks, 293.3 M pixels; `s5/tedit` for the two new ones and FIX):**

| fault | pixels differing | alpha, Z differing | failing kicks |
|---|---|---|---|
| ATST LEQUAL as LESS | 1,302,500 | 1,302,499, 1,296,600 | 600 |
| the failing part of the split dropped | 2,125,870 | 0, 0 | 600 |
| ZTST GREATER as GEQUAL | 36,753 | 0, 0 | 600 |
| the half pixel left out | 440,621 | 56,256, 379,308 | 747 |
| **FIX off by one** | **6,109,008** (Track Editor 2,508,661) | 0, 0 | 600 (700) |
| a vertex colour's R and G swapped | 955,200 | 0, 0 | 600 |
| the texel fetch one row off | 9,363,135 | 3,170,802, 1,706,291 | 743 |
| **the barrier left out** | **976,578** (Track Editor 1,582,291) | 0, 0 | 600 (700) |
| **a reading batch as one group** | **70,657** (Track Editor 110,951) | 0, 0 | 600 (700) |

So FIX off by one, which the widened bound let through on all but 265 pixels (16.3), now fails on 6.1 M; the
split-per-batch and TEXA faults stay T3's and T6's (they change no pixel of these dumps, 16.3).

### 19.4 Validation

The Khronos layer with synchronisation validation was on for `rc_gs2d_test` and for all 67 dumps: 0 errors, 0
warnings. **The layer does not see a missing local-read barrier:** the no-barrier fault, which changes 14 of T8's 17
scenes and a million pixels of the front end on the device, gives no validation message; synchronisation validation
(1.4.363) does not model input-attachment reads under dynamic rendering local read. So it never asked for
`RHI_LOCAL_READ_BACK` either (19.1 item 3), and the barriers' correctness rests on T8 and check (c), which do see it.

### 19.5 Cost (18.5): the acceptance figure is exceeded on the draw time

Timestamps around a kick's passes, RTX 2060, no validation layer, one process (the 67-dump run's timings are under
the layer and four processes and are not used):

| dump (USA) | fixed function, bias on: draws a kick (avg / largest) | read: draws a kick (avg / largest) | largest kick: draws, barriers | wall time a kick (seed, draw, read-back), ff / read |
|---|---|---|---|---|
| `s5/front` | 0.205 / 0.440 ms | 1.277 / 3.581 ms | 161, 115 | 9.26 / 10.26 ms |
| `s5/l17` (race) | 0.121 / 0.151 ms | 0.186 / 0.365 ms | 4, 3 | 10.62 / 10.56 ms |
| `s5/front`, read with the barrier fault (first 300 cycles) | - | 0.501 / 1.205 ms | 161, 0 | - |

What the specification measured (the whole kick in check (c), "dominated by the seed and the read-back") is within
11% (front end) and equal (race). But the draws themselves are **6.2 times** fixed function's on the front end
(largest kick 3.6 ms against 0.44) and 1.5 times in a race. The barrier fault isolates the cause: about 20 us a
by-region barrier on this device (3.58 - 1.21 ms over 115 barriers), the rest (0.77 ms) one draw per group instead
of one per batch. On 18.5's rule ("more than twice fixed function's: stop and report rather than optimise") nothing
was optimised. For check (c) and off-line work the cost does not matter (the wall time is the seed and read-back).
For live drawing (batch 5) it is 3.6 ms of a 16.7 ms frame in the worst menu kick, against 18.5's "well under 0.5 ms",
and 0.37 ms in a race. **Open for the review / batch 5:** keep `kBlendAuto` = read live, or reduce the barriers
(18.5's first lever: continue a group across consecutive batches whose states both read and do not overlap; or
merge the groups of consecutive batches of one state), or draw live menus by fixed function. Step 8's K2 counts
(17.2: a few hundred effects a frame, about one barrier each) would cost several ms at this rate: the interlock form
(18.6 item 3) may need reconsidering there.

### 19.6 Clean-ups (17.3)

- Item 2: `rc_replay` and `rc_gs2d_test` are built in `build-eu` too (current; `rc_replay` does not depend on the
  version, either tree's binary serves both versions' dumps; NOTES.md says so).
- Item 3: the stale "14.7" comment in `gs2d_vk.h` is gone (the option's comment now cites 15.3 and 17.1).
- Item 4: `flatSourceExact` is removed, with its two users (the gif2d table's "flat source, integer exact" row, and
  T2's exemption from L, which now counts every inexact primitive as check (c) does).

### 19.7 The game and the oracle

Nothing new is linked into either (`Gs2dVk`, `rc_gs2d*` are in `rc_replay` and `rc_gs2d_test` only). The CMake
re-run relinked `rc_revenge_pro` in both trees anyway (`capture.cpp` embeds `git describe`; the track shader header no
longer changes on a re-run, 19.1 item 8), so the regression checks were run: `tools/oracle.sh --all` USA (seed 7451) 1632 ok and Europe (seed 7452) 1563 ok, 0 FAIL; `playtest2`
replays reach the race scene 3020 at frame 658 (USA) and 1167 (Europe). (A first replay of each run under the oracle's
load was slower: USA did not reach the race in 70 s, Europe reached it one frame later, at 1168, its front end entered
at input clock 0:255 instead of 0:256; rerun unloaded, both as above.)

### 19.8 Open

- The cost (19.5): a decision before batch 5 and for step 8.
- 18.2's optional part (C = Ad, BLEND_FORM drawn by the read) was not built; no RC draw uses either.
- The fixed-function path on a device with a blender wider than 8 bits is still unmeasured (13).

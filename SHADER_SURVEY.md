# Native GPU shaders and ray-traced reflections: survey (2026-11-24)

Question (user): which of the game's PS2 rendering programs (VU1 microprograms, and the effects drawn through them or
straight through the GS) should become native GPU shaders, for performance and because ray-traced reflections are
planned on some surfaces, water first.

Read-only study on `main` at 9243ae0 and later. Nothing was built or run except two read-only Python scripts over the
USA disc's level files (section 3.1; they use `tools/bbk_export.py`'s reader). Numbers marked *measured* come from
`PERF_INVESTIGATION.md`, `NOTES.md` ("VU1 programs converted") or `RENDERER_P2_DESIGN.md` section 3 (the
`shapestat` survey over 67 dumps, 1,470 M triangles read); *estimated* means reasoned here, not measured.

## Short answer

- **The VU1 conversion is nearly finished.** Of the six VU1 programs, four (track, shiny pass, skin, water) already
  run as one GLSL vertex shader (`decomp/shaders/trdr_shape.vert`, `shapeMode` 0-6) inside the GS hardware renderer,
  and they carry over 99.9% of all 3D triangles, effects included (smoke, sparks, skid marks, shadows, glows and
  transparent polygons all go through the track program). The other two are Track Editor pieces (`TERdr`, still
  recompiled VU1) and `Rdr_VUShape`, which never runs (no level has version 2.0 shapes).
- **What is left is getting the shader batches out of the emulated GS.** Every batch still travels as a pointer in the
  GS command stream, with its triangles copied twice per frame, its texture looked up in emulated GS memory and a CLUT
  reloaded per texture block. On a 144 Hz race the GS thread is the limit (0.9 of a core); drawing, CLUT loads and
  texture slot set-up are about 60% of it. A **native 3D path** (geometry uploaded once a level, textures decoded at
  load, GS states mapped to native pipeline states, the GS stream used only for 2D) is the top-ranked conversion.
- **It is also the RT prerequisite.** The track shader is the only place in the default build where geometry still
  exists before projection: model-space triangles with face normals, plus each draw's model-to-view matrix. The
  emulated GS, the 2D path and today's native Vulkan path (`RC_RENDERER=vk`, exact mode) only have screen-space
  primitives. A native path that keeps those batches as persistent buffers is exactly what a ray-tracing
  acceleration structure is built from.
- **Reflective surfaces today are all fakes**: water is an emboss bump-map in three passes (no reflection at all),
  car paint and other flagged models get an additive sphere-style environment-map pass, the invincibility shield is
  the same pass pulsing, and road "glints" are sprites. There is no render-to-texture, no mirrored geometry pass and
  no frame-buffer read anywhere in the game (`RENDERER_SURVEY.md`).
- **Water is ideal for reflections**: on 15 of 24 tracks, only 20 to 2,200 triangles a level, and on 9 of those 15
  tracks one horizontal plane (or three) (section 3.1). Planar reflections would be exact there; RT handles the sloped
  rivers (levels 5, 8, 17, 20) too.
- **Recommended path:** (1) screen-space reflections on water inside the current OpenGL track shader (days, no new
  data needed); (2) the native 3D path with per-level geometry and decoded textures (the performance win, and the RT
  data); (3) a Vulkan ray-query reflection pass on water at half resolution, run beside the OpenGL renderer through
  GL/Vulkan interop, falling back to SSR; (4) optionally car paint. Moving the whole renderer to Vulkan first is not
  needed for RT and is the most expensive route (section 5).

## 1. Inventory

### 1.1 How a 3D frame is drawn today (default build: `tools/play.sh`, `PS2X_GS_HW=1`, `RC_TRACK_SHADER=1`)

Game code (native C++) builds the PS2 frame exactly as before: a VIF1 DMA chain (K2) of calls to shape packets and
triangle lists, and a 2D GIF packet (K1). The race loop's order (`decomp/src/main.cpp:520-537`): world
(`LsRenderWorld`), car shadows (`VfxRenderShadows`), objects and cars (`ObjectRenderAll`), vertex-animated scenery,
scenery objects and **water** (`TSORender`), pickups, HUD panel, weapons, then the sorted transparent triangles
(`RdrAlphaAddToDmaChain`) and the effects (`VfxRender`).

The VIF1 chain is replayed by the runtime on the VU thread. When a VU1 program is started (MSCAL),
`decomp/vu1/track_draw.cpp` intercepts the four converted programs (`installTrackDraw`, :352):

1. the program's GS state packets (texture block, blend) are kicked as GIF packets as the program would (`kickPacket`);
2. each texture block's triangles are `malloc`ed into a `GSTrackBatch` (`sendTriangles`, :53; `sendWaterTriangles`,
   :129): the draw constants (matrix, screen scale and offset, clip range, tint, env rows) and **ten quadwords per
   triangle copied from VU memory**, as the game sent them: face normal (w: winding flag), three model-space
   positions, three S T Q, three colours;
3. the batch's pointer goes into the GS stream as a write to an unused register (`kGSTrackBatchRegister`,
   `gs_track_batch.h`), so it stays in order with the state packets;
4. the GS thread's hardware backend (`third_party/ps2recomp/ps2xRuntime/src/lib/gs/gpu/gs_hw_backend.cpp`,
   `drawTrack`, :2739) copies the quadwords into a persistent GL ring (`trackTriangleBuffer`, 64 MB, :2533-2540),
   resolves the block's texture and palette from emulated GS memory (bindless multi-texture slots), and draws
   `3 x triangles` vertices with no attributes. The vertex shader is `trdr_shape.vert`'s `trackMain` (:134), spliced
   into the backend's GS-emulating program (:58, :723-732); the fragment side is the GS emulation shader (GS memory
   layouts, palettes, PS2 blend and alpha test).

Every triangle the program reads is sent, including the 41.6% the VU1 program would have culled by facing and the
24.2% wholly behind the near plane (*measured*, `RENDERER_P2_DESIGN.md` 3.3); the shader discards them.

### 1.2 The VU1 programs

| program (hash) | draws | where | how it runs today | cost (before → now) | C++ / check |
|---|---|---|---|---|---|
| `TRdr_VUShapeProg` (`C3952C0F...`) | the world (level shape, `LsRenderWorld`, `decomp/src/misc.cpp:348`), cars and scenery without an env map (`TRdr_VUShape`, `shape.cpp:356`), **all 3D effect polygons** (`RdrPoly`, `rdr.cpp:1031`, camera packet `RdrPolyInitCamera` :988) and the sorted transparent triangles (`RdrAlphaAddToDmaChain`, `shape.cpp:814`) | every 3D scene | vertex shader, `shapeMode` 0 | *measured* 14.81 s of VU1 in a 30 s level-18 demo before (about half a core); now GPU vertex work. 1,141 M of 1,470 M triangles read in the dumps (78%); 33,000 a frame on average, 65,600 at most (level 18) | `trdr_shape.cpp`, 0 differences; shader vs VU1 worst 1.11/16 pixel on the quick path |
| `EnvRdr_VUShapeProg` (`AB0E7743...`) | models with an environment map (head flag 0x20 in `RendObjRender`, `misc.cpp:557`, :631-660): car bodies (`object3.cpp:159`, doTint 5), wheels, pickups and scenery objects that carry the flag. Two passes: the model as usual, then an additive env-map pass (`shape.cpp:356-401`) | races, front end, podium | vertex shader, `shapeMode` 1 (plain) and 2 (env map) | *measured* 2.24 s / 30 s before; 203 M triangles read (14%) | `env_shape.cpp`, 0 differences |
| `Rdr_VUSkinProg` (`EC08B222...`) | skinned models: drivers, the swaying trees and plants (AKLM Studios), the jungle natives, podium animations (`Rdr_VUSkin`, `shape.cpp:530`) | races (some levels), front end | vertex shader, `shapeMode` 3; the whole 16 KB of VU memory (bones) carried per run | *measured* 0.96 s / 30 s before; 88 M triangles read (6%) | `skin_shape.cpp`, 0 differences |
| `Rdr_VUWaterProg` (`E96770E4...`) | water surfaces: scenery objects with flag 0x200 (`tso.cpp:622-629` → `Rdr_Water`, `shape.cpp:618`) | 15 of 24 tracks (section 3.1) | vertex shader, `shapeMode` 4, 5, 6 (one batch per pass, pass-major within a block, since d4c7115); `RC_WATER_SHADER=0` runs the C++ program | *measured* level 9, 4x, 144 Hz: 105 → 131 pictures a second, VU thread 0.79 → 0.54, GS 0.58 → 0.94, GIF 0.30 → 0.16. 37 M triangles read (2.5%), 40% of its runs have a clipped triangle | `water_shape.cpp`, 0 differences; `tools/shader_check --water` 60,033 passes, worst 1/16 pixel |
| `TERdr_VUShapeProg` | Track Editor pieces (`TERdr_VUShape`, `shape.cpp:436`, from `tile.cpp:103`): the track program plus four directional lights per corner | Track Editor and its races | **recompiled VU1** on the VU thread | *measured* Track Editor 60 fps, VU thread 0.14-0.25; 0.3 M triangles in the dumps | `terdr_shape.cpp`, 0 differences on 284,151 real runs |
| `Rdr_VUShapeProg` | version 2.0 shapes (`Rdr_VUShape`, `shape.cpp:297`) | **never**: all 2,079 shapes in the 53 level files are version 3.0 | recompiled VU1 (unused) | 0 | `rdr_shape.cpp`, synthetic runs only |

### 1.3 Effects (all through `RdrPoly` → the track program, so already in the track shader)

The effects are world-space quads built on the EE (`VfxPutGT4*`, `VfxPS2SpriteRotated3D` billboards turned to the
camera on the CPU, `visfx.cpp:860`), batched per channel by `RDRPOLY` and drawn by `TRdr` with a camera packet; they
reach the GPU as `shapeMode` 0 batches from rewritten buffers (3.3% of triangles, *measured* 3.2 of the P2 design).

| effect | code | how it looks | notes for a shader / RT |
|---|---|---|---|
| car shadows | `VfxRenderShadows`, `visfx9.cpp:594` | a quad of the car's shadow texture per car, corners at the wheel ground heights + 3, subtractive (style 4), depth writes off | world-space quad; a modern shadow would replace it (not a reflection topic) |
| skid marks | `VfxRenderer_CarSkidMark`, `visfx4.cpp:1162` | ring of quads, additive or subtractive by ground material | decal; fine as is |
| smoke, dust, sparks, fire, explosions, spray, splashes, boat wake, foam | `visfx.cpp:1170`, `visfx4.cpp`, `visfx9.cpp:433, 508`, `visfx3/5/6/7.cpp` | camera-facing sprites and ribbons, mostly additive (0x29/0x68) or the 0x01 cut-out blend | soft particles need the depth buffer, which a native path has; exclude from any RT scene |
| light glows, flares, candles | `Vfx2Renderer_LightGlow` `visfx8.cpp:361`, `LightFlare` `visfx7.cpp:152` | additive flare sprites, distance-faded | bloom candidates; no occlusion test |
| road glints | `Vfx3Glint`, `visfx7.cpp:476` | sparkle sprites dropped on road of material kind 2, fading | the only "shiny floor" hint in the game; a wet-road material flag for SSR/RT could reuse the same material test |
| sky terror sky | `VfxSkyBackdropMerge`, `visfx12.cpp:1277` | three scrolling dome layers around the camera | the normal sky was not traced here (likely part of the world shape); RT misses need a sky colour or captured cube |
| lightning, quake ring, heat ray, ribbons, aerials | `visfx4/6/9/11.cpp` | additive quads | |

### 1.4 What only the GS draws (2D, briefly)

K1, the GIF 2D packet on PATH3, goes to the emulated GS and its hardware renderer as ordinary GS primitives (XY in
12.4 fixed point): HUD and fonts (`sprite.cpp`, `hud.cpp`, `font.cpp`; `GfxP2_Tex*`, `rdr.cpp:471-541`), menus and
boxes (`RectRender*`), debug lines, the logos, movie pictures and the loading splash (uploads into frame memory), the
flip's clear. The widescreen HUD anchoring of e7202e8 (`decomp/rc_widescreen.cpp`) moves these primitives in the
packet. *Measured* cost: the front end is about 450 draws a frame at 60 fps with the GS thread at 0.47 of a core
(3x); on the jungle track select 93% of draw breaks were CLUT loads.

### 1.5 Where the time goes now (*measured*, `PERF_INVESTIGATION.md`)

- 3x, 60 fps: every track at 60 (worst second 56-60), GS thread 0.4-0.75, GPU 35-40% busy.
- 4x, 144 Hz (`PS2X_REFRESH_HZ`): 131-135 new pictures a second on levels 9, 18, 21; **the GS thread is the limit at
  ~0.9 of a core**: by inclusive time drawing 33%, CLUT loads 16%, multi-texture slot set-up 10%, the rest the
  command queue, VRAM uploads and present. VU thread 0.54 on level 9 (VIF processing, state packets, the per-batch
  copies), GIF 0.16.
- Draws a frame: level 9 ~440, level 18 280-460 (PCSX2: 170).
- GPU: ~6 ms a frame at 60 fps on level 18 (38% busy at 1,900 MHz, 3x; utilisation times frame time) for a game PCSX2 draws in ~1 ms: the GS
  emulation fragment shader (GS memory addressing, palette fetch, PS2 blend) is not cheap. *Estimated*: that is the
  budget RT would compete with at 144 Hz.

## 2. Each candidate: benefit, effort, risk, needs

"Native path" below means: the batch goes from `track_draw.cpp` (or a successor on the EE side) to a native renderer
queue instead of through the GS stream; its triangles live in a per-level GPU buffer; its texture is a pre-decoded
RGBA texture (array layer or bindless handle) rather than emulated GS memory plus a CLUT; its GS blend/test state is
mapped once to a native pipeline (`RENDERER_SURVEY.md` "Draw states": every 3D state fits fixed-function blending with
a premultiplied source and a dual-source factor). It renders into the same GL colour and depth targets the HW
backend uses for the display buffer, in stream order, so the 2D still lands on top.

### Rank 1. Native 3D path for the shader batches (track, env, skin, water)

- **Benefit.** *Estimated* GS thread -50 to -60% on races (drawing 33% + CLUT 16% + slot set-up 10% are almost all
  3D blocks: 2,012 texture blocks a kick on average, each a TEX0 with CLD 1, i.e. a palette load); VU thread loses the
  per-frame copy of every triangle read (33,000-58,000 triangles x 160 bytes, 5-9 MB a frame, twice: VU memory →
  `GSTrackBatch` → GL ring, `track_draw.cpp:58-110`, `gs_hw_backend.cpp:2796`) and the `malloc`/`free` per block;
  GIF thread loses the marker and state packets; GPU loses the GS-emulating fragment work on 3D (plain textured,
  vertex-coloured fragments). Draws: one per pipeline state and texture array, *estimated* well under 200 a frame.
  That moves the 144 Hz limit and leaves GPU time for reflections.
- **Precision and resolution.** Already float positions before the GS's 1/16-pixel rounding (the track shader writes
  `gl_Position` from float GS coordinates); a native path keeps that and adds real mip levels and anisotropic
  filtering (textures decoded at load), MSAA, and depth that is not tied to the GS Z layout.
- **Needs.**
  1. Geometry once a level: the fixed units (shape and cell triangle lists, 5.1-11.5 MB a level, 92% of runs and 97%
     of triangles, *measured* 3.2 of the P2 design) uploaded at first use, keyed by guest address and the level's load
     generation; rewritten units (`RDRPOLY`, `RDRALPHA` buffers, the shape packet ring) stay inline per frame (about
     1,090 triangles a kick). The P2 design's `SHAPE_BATCH`/`UNIT` scheme (5.1-5.4) is the worked-out form for the vk
     path; for the GL path a simpler key (address + level load counter + a hash on first use) is enough, since the
     level lists do not change while a level lives.
  2. Textures decoded at level load from the level's GS memory image (palettes are static: no palette animation,
     `RENDERER_SURVEY.md` "Textures and CLUT"), plus the ~20 animated textures re-uploaded each frame (80 KB) and
     textures constructed during play (547 cycles in three captures): watch `GfxTex_Update` / `GfxP2_LoadTexMip` (both
     native) or the upload transfers.
  3. Per-batch constants (already in `GSTrackBatch::draw`), and for skin only the bone matrices instead of all 16 KB.
  4. State mapping from the block's GS registers (the 13 3D blend/test cases in the survey; the default 0x44+PABE is
     blend off with the alpha test as a discard).
  5. Order: batches interleave with state packets and, in the alpha and effect passes, depend on draw order; keep a
     single ordered queue per frame (the GS stream position is the order today).
- **Effort.** Large: *estimated* 2-4 weeks of Opus batches (geometry cache, texture decode, state map, ordering with
  the GS stream's targets, split screen, then the remaining state-packet traffic). The pieces exist in the archived
  exact renderer (`render/tex` decode, `render/shape`, the P2 design's units), but that renderer is Vulkan and exact,
  this path is GL and enhanced.
- **Risk.** Medium: render-target identity (which GL target is the display buffer), draw order against the 2D and the
  flip (3D of frame N-1 under 2D of frame N), textures uploaded mid-frame, alpha-sorted triangles. Each step can be
  switched off (as `RC_TRACK_SHADER`, `PS2X_GS_HW_TRACK_MULTITEX` are) and compared by screenshot.

### Rank 2. Water: one pass, then reflections (already a shader)

What the water is: `Rdr_Water` (`shape.cpp:618`) puts a light 1,000 units above and 2,000 ahead of the camera
(`gRdr_WaterLightPos`) and the bump texture's step (`gRdr_WaterBumpUVs`) into the packet. Pass 1 draws the water
texture (CT32) with ALPHA 0x09 (`Cs + Cd*As`, As 127: the riverbed shows through); pass 2 subtracts the T4 bump
texture (0x62, FIX 0x15); pass 3 adds the same bump with texture coordinates pushed away from the light point
(`trdr_shape.vert:101-108`, `water_shape.cpp` `rippleTexture`). That is **emboss bump mapping**: a fake lit ripple,
with no reflection of anything. (The shader names VU memory 11 "ripple centre"; from the packet layout it is the
light position.)

- **One pass.** A water fragment shader that samples both textures and writes `Cs1 + f*(bump(st+d) - bump(st))` over
  `Cd*As` does the three passes in one draw (per-pass clamping differs slightly: enhanced mode). Removes the
  multipass lanes (`gs_hw_backend.cpp:2020-2200`) and two thirds of the water's draws and state changes. Effort
  small on the native path; medium in today's GL path (two textures per draw on the multi-texture slots).
- **Reflections.** Section 4: SSR first (no new data), then RT or planar.
- **Cost.** Water is 2.5% of triangles read; the win is draws and state churn on water levels (level 9 drew 3,400-5,400
  GL draws a frame before the multipass lanes, 78% of them single water triangles).

### Rank 3. Car paint and other env-mapped models (already a shader)

What it is: `RendObjRender` (`misc.cpp:631-660`) sets `gEnvMap` from the model head's env-map texture when flag 0x20
is on, with a distance fade (`maxEnvMapDistG`, `fadeEnvMapDistG`; car bodies use doTint 5, which scales both), or a
fixed or pulsing strength for the shield (flags 0x8000, 0x2000). `TRdr_VUShape` then draws the model with `EnvRdr`
twice: plain, then additive (ALPHA 1,2,2,0 FIX 0x80: `Cs + Cd`) with the block texture replaced by the env map and
coordinates from a per-component "reflection" of a per-vertex normal stored in the colours' fractions
(`trdr_shape.vert:87-99`): the original's per-component maths, not a textbook reflection (`NOTES.md`, "VU1 programs
converted").

- **Benefit.** Visual: a real reflection vector in world space, a dynamic cube map (six low-cost views from the native
  path) or RT, and folding both passes into one draw. Performance: small (14% of triangles drawn twice).
- **Needs.** The per-vertex normals (already in the data), the model's world orientation (side tag `SpanChain` in
  `TRdr_VUShape` has it, or derive from the batch matrix and the frame camera).
- **Effort.** Small for the single pass and a correct reflection vector into the existing env texture; medium for a
  dynamic cube map; RT as in section 4.
- **Risk.** Look changes (the original look is stylised); keep a switch.

### Rank 4. TERdr (Track Editor) as `shapeMode` 7

- **Benefit.** Performance none worth having (the editor holds 60 with the VU thread at 0.14-0.25). Needed so that K2
  is entirely native (the native path can then stop the emulated VU1 for render programs), and so Track Editor tracks
  have world-space geometry for reflections.
- **Needs.** Four directional lights and a colour matrix per corner (`terdr_shape.cpp` `lightCorners`), constants four
  quadwords earlier than the track's, the depth bias only on the quick path.
- **Effort.** Small (a day): the same ten-quadword format, `tools/shader_check` can compare it as it does env and
  water.
- **Risk.** Low.

### Rank 5. Effects and transparent triangles (already a shader)

- **Benefit.** Soft particles (depth fade) and higher-precision billboards once on the native path; RT must exclude
  them (additive sprites in a BVH are wrong and costly). Sorting stays on the CPU (`RdrAlphaAddToDmaChain`).
- **Effort.** Comes with rank 1 (they are `shapeMode` 0 batches from rewritten buffers); soft particles small after.

### Rank 6. The 2D packet (HUD, fonts, menus)

- **Benefit.** Front end and HUD already at 60; a native 2D renderer would remove CLUT-load draw breaks (fonts and HUD
  are paletted) and draw at full precision. Low priority for performance; useful later for crisp text and the HUD at
  any resolution.
- **Effort.** Medium to large (all the 2D blend cases, including the AFAIL RGB_ONLY split, movies, uploads into frame
  memory). The archived `Gs2dVk` (Vulkan) shows the scope.
- **Risk.** Medium (menus, fades, the frame-order offset).

### Not worth converting

- `Rdr_VUShapeProg`: never runs. Keep the recompiled fallback.
- Movies, logos, the loading splash, the flip: uploads and one clear; nothing to gain.

### Ranking table

| rank | path | perf benefit | visual / precision | needed for RT | effort | risk |
|---|---|---|---|---|---|---|
| 1 | native 3D path for shader batches (persistent geometry, decoded textures, native states) | high: GS thread -50-60% (est.), VU copies gone, GPU fragment work cheaper | mips, aniso, MSAA, float depth | **yes** (geometry and materials on the GPU in model space) | large | medium |
| 2 | water: single pass, then SSR/RT | medium on water levels (draws, states) | reflections | target surface | small-medium | low |
| 3 | car paint / env pass: one pass, real reflection vector, cube map or RT | low | yes | optional | small-medium | low (look) |
| 4 | TERdr as `shapeMode` 7 | none | none | Track Editor tracks | small | low |
| 5 | effects / alpha: soft particles | low | yes | exclude from RT | small after 1 | low |
| 6 | native 2D | low (front end already 60) | crisp HUD/text | no | medium-large | medium |
| - | `Rdr_VUShape`, movies, flip | none | none | no | - | - |

## 3. Reflective surfaces and reflection tricks

### 3.1 Water, from the level files

Scenery objects with flag 0x200 in the level's TSO list are water (`tso.cpp:622`). Counted from the USA disc's
`BB/*/*.BBK` with `tools/bbk_export.py`'s reader (level number = file number; levels 6/12/18/24 share a world, as
`WorldTrackToLevel` = track*6 + 1 + world says; levels 25-48 repeat 1-24):

| level | water shapes: triangles | shape | world triangles |
|---|---|---|---|
| 1 | 60 | gentle slope (10 heights over 26 units) | 18,375 |
| 5, 17 | 1,617 + 182 (5) / 393 (17) | **a river**: 92 heights over ~500 units, 89% of triangles flat; the second shape one plane | 16,305 / 19,838 |
| 6, 12, 18, 24 | 1,138 | **one plane** at y 0, 413,000 units across (a sea) | 43,540 / 43,540 / 48,995 / 48,995 |
| 8, 20 | 1,245 + 186 (8) / 450 (20) | **a river** falling ~290 units in 87 steps, 99% flat triangles | 18,326 / 23,889 |
| 9, 21 | 1,481 + 605 (9) / 395 (21) | **one plane** at y -525 | 37,712 / 31,881 |
| 11, 23 | 1,810 | **one plane** at y -525 | 14,221 / 15,019 |
| 13 | 116 + 60 + 75 | three small planes | 28,486 |
| 19 | 20 | small slope | 29,441 |
| 2, 3, 4, 7, 10, 14, 15, 16, 22 | none | | |

So: water on 15 of 24 tracks, at most ~2,200 triangles of it a level, flat planes on 9 tracks (6, 9, 11, 12, 18,
21, 23, 24 one plane each; 13 three small planes), terraced rivers on 4 (5, 8, 17, 20), small slopes on 2 (1, 19). Water is drawn after the world, the
shadows and the cars, before the transparent triangles and effects (`main.cpp:520-534`), so whatever SSR reads at
that point already holds the cars.

### 3.2 The rest

| surface | today's trick | how (code) | data available |
|---|---|---|---|
| water | emboss bump mapping, three passes (2.2 above) | `Rdr_Water` `shape.cpp:618`, `trdr_shape.vert` modes 4-6 | face normals (flat: the planes above), bump texture, light point |
| car paint, other flagged models (wheels, pickups, scenery with head flag 0x20) | additive env-map pass, sphere-style coordinates from per-vertex normals and the eye vector, faded with distance | `misc.cpp:631-660`, `shape.cpp:321-401`, `trdr_shape.vert:87-99` | per-vertex normals in the colour fractions, the env texture per model head. Which car parts and objects carry the flag was not counted (they are in the car and object resources, not the BBKs); `RC_VU1_REF=1` runs saw the env map on in 19,710 runs |
| invincibility shield | the env pass at a fixed or frame-pulsing strength | `gEnvShield`, flags 0x8000/0x2000, `object3.cpp:147-152` | as car paint |
| shiny / wet road | sparkle sprites on material kind 2 | `Vfx3Glint`, `visfx7.cpp:476` | the collision material under a point (`AColDownCast`), not the render triangles |
| glass, chrome, mirrors | nothing separate found: no render-to-texture, no frame-buffer read, no mirrored geometry pass (`RENDERER_SURVEY.md`: FRAME is only ever the two display buffers, no texture is sampled from frame or Z memory); chrome is presumably env-flagged model parts | | |
| "Mirror Mode" | the tracks mirrored, not a reflection | | |

### 3.3 Where world-space data still exists in the pipeline

| stage | what it holds | 3D (pre-projection)? |
|---|---|---|
| level and model data in guest RAM (`sRdrVUShape`, `pTri` DMA lists; `tools/bbk_export.py` reads the same) | model-space triangles: face normal + winding, positions, S T Q, colours (env: normals in fractions; skin: bone indices and weights) | yes, persistent per level |
| the CPU draw calls (`TRdr_VUShape`, `Rdr_Water`, `Rdr_VUSkin`, `RdrPoly`) | the model's world matrix and position (`shape.mat`, `shape.pos`), the camera (`rdrCameraG`, built by `LsCameraToRdr`, `misc.cpp:318`), effect quads in world space | yes; `writeShapeCamera` (`shape.cpp:56`) folds model and camera into one model-to-view transform for VU1 |
| VU1 inputs (`GSTrackBatch` today) | the triangles as above + the model-to-view rows (VU 18-21), screen scale/offset, near plane | yes, in model space; view space after one matrix |
| the track shader (`trdr_shape.vert`) | `view = matrix * position` (camera space), the face normal, the GS-space position | **yes**: the last place it exists in the default build |
| the GS hardware renderer's draw records (everything not a track batch: TERdr, 2D) | GS window XY (12.4 fixed), Z, colour, S T Q | no: screen space only |
| the GS targets | colour and a D32F depth texture per target (`attachDepth`, `gs_hw_backend.cpp:943`) | depth only: enough for SSR |
| the archived exact Vulkan path (`RC_RENDERER=vk`, `render/`) | `SCREEN_PRIMS` from the fast exact reader: post-projection, 1/16 pixel | no; `SHAPE_BATCH` (P2 design 5.2, batch 8) was never built |

Model-space positions plus a model-to-view matrix are enough for RT: an acceleration structure can be built in view
space each frame, each instance's transform being its batch's model-to-view matrix, with no camera extraction. The
world-space alternative needs the frame camera passed alongside (a marker in the stream from `LsFrameInit`, or the
level shape's own matrix, which is the view matrix if the level shape sits at the origin: not checked).

Shading a ray hit is unusually cheap here: the game's lighting is baked into vertex colours (128 = full) and a tint,
so a hit's colour is `texture(uv) * colour * tint`, no light evaluation. A miss needs a sky value (3.2: the normal sky
was not traced).

## 4. Reflections: options and the API situation

### 4.1 The API

- The default renderer is the GS hardware renderer on **OpenGL** (GLX context sharing the window's, since 80ad41a).
  OpenGL has no ray-tracing extension on NVIDIA; RT needs Vulkan (`VK_KHR_acceleration_structure` +
  `VK_KHR_ray_query`, or `VK_KHR_ray_tracing_pipeline`). The RTX 2060 (Turing, 30 RT cores, 6 GB) supports them on
  current drivers (not queried here; check `vulkaninfo` for both extensions and for `GL_EXT_memory_object_fd` /
  `GL_EXT_semaphore_fd` in `glxinfo`).
- The repository has Vulkan pieces: `third_party/ico_rhi` (ico-pc's RHI, Vulkan 1.3 backend; **no acceleration
  structure or ray-query support**, grep finds none), volk and Vulkan-Headers fetched at configure time, the SDL3
  window for `RC_RENDERER=vk`, and the archived exact renderer (`render/`, `Gs2dVk`). That path is not a base to build
  on soon: its races ran at 40-45 fps because the emulated GS still renders beside it, the reader takes 12-17 ms a
  cycle, and with the GPU otherwise idle the driver stays at 300 MHz and K2 takes 10-16 ms a kick
  (`RENDERER_P2_DESIGN.md` 21); and it only has screen-space primitives.

### 4.2 Options

| option | where | quality | cost on the RTX 2060 | effort | prerequisite |
|---|---|---|---|---|---|
| **A. SSR on water** | GL: the water's fragment shader ray-marches the current target's depth (D32F) and colour (already bound for feedback, `glTextureBarrier`) | good for planar water at grazing angles; misses what is off screen or behind objects, and the transparent and effect passes drawn after water | *est.* under 0.5 ms at 4x on water pixels at half rate | small (days) | none: view-space position and face normal are in the track shader (3.3) |
| **B. Planar reflection** | re-draw the opaque batches mirrored about the water plane into an off-screen target (clip plane at the water) | exact on the 9 flat-water tracks; wrong on terraced rivers (per-plane cost) | one extra opaque pass (14-49k world triangles: cheap on this GPU) | small-medium **after rank 1**; impractical through the GS stream | native path (redraw batches with a mirrored matrix) |
| **C. RT water reflections, half resolution** | Vulkan ray query (compute, or the fragment shader in a Vulkan renderer), one ray per water pixel at half res, normal perturbed by the bump texture, bilateral upsample; temporal accumulation only if rough | correct off-screen and occluded content, sloped rivers, split screen | *est.* well under 1 ms: at most ~2,200 water triangles to shade, 14-49k world triangles + cars in the BVH, coherent near-mirror rays, ≤0.5 M rays a frame at 4x half res | medium-large | native path geometry and decoded textures (rank 1); a Vulkan device beside GL (interop) or a Vulkan renderer |
| **D. Hybrid C + A** | trace only where SSR fails (off screen, occluded), SSR elsewhere | best quality per ray | lower than C | C + a little | as C |
| **E. RT on car paint / env models** | rays from env-flagged model pixels, rougher (needs denoise), or a dynamic cube map from the native path (6 views a frame, or 1 a frame round-robin) | cube map: good and cheap at these triangle counts; RT: accurate but noisy at low ray counts | cube map *est.* 0.5-1 ms; RT more | medium | rank 1 |

Denoising: flat water with a light bump is a near-mirror; one ray a pixel at half resolution plus a depth- and
normal-aware upsample is enough. Car paint (rougher, curved, small on screen) is where a temporal denoiser would be
needed, which is one more reason to give cars a cube map rather than rays.

### 4.3 How RT would sit beside the OpenGL renderer (recommended over porting the renderer first)

- One Vulkan device on the same GPU as the GL context (match `GL_DEVICE_UUID_EXT` with
  `VkPhysicalDeviceIDProperties::deviceUUID`; `tools/play.sh` already offloads both APIs to the NVIDIA GPU).
- The per-level geometry buffer (rank 1) is allocated in Vulkan with exportable memory and imported into GL
  (`GL_EXT_memory_object_fd`), so the raster path and the BLAS read the same bytes. One BLAS per fixed unit (built once
  a level; the track lists' ten-quadword stride works as a vertex stride with a position offset, or a compute pass
  writes a compact position buffer); skinned models get a per-frame BLAS from a compute skinning pass (or are left out
  of the first version); effects and alpha triangles are left out. Alpha-tested cut-outs (foliage) need non-opaque
  geometry and an alpha check per candidate hit; start with them opaque or excluded.
- A TLAS per frame (and per split-screen view): one instance per drawn batch, transform = the batch's model-to-view
  matrix, custom index = the batch's material/geometry offset. A few hundred instances: trivial to build.
- The water pass writes its pixels' view-space position and perturbed normal into a shared image (an extra render
  target of the water draw); at the end of the 3D (before the 2D packet) GL signals a shared semaphore, Vulkan traces
  and writes the reflection colour into a shared image, signals back, and GL composites it over the water with a
  Fresnel weight (the original's `Cd*As` keeps the riverbed: reflection = mix(underwater, reflected, fresnel)). Hit
  shading: barycentrics → S T and colour from the geometry buffer, texture from the decoded texture array.
- SSR (option A) stays as the fallback when Vulkan, the extensions or interop are missing.

The alternative, moving the native 3D path to Vulkan (or finishing `RC_RENDERER=vk` with an enhanced mode), puts RT in
one API but needs the 2D path, presentation and every state case on Vulkan first: weeks more before the first ray.

## 5. Recommended order

1. **SSR on water in the current GL track shader** (`shapeMode` 4, the base pass): view-space position and normal
   from the batch, the target's depth and colour textures. No new data path. Gives reflections on all 15 water tracks
   within days and becomes the RT fallback. Optionally fold the three passes into one at the same time.
2. **TERdr as `shapeMode` 7** (a day): the last VU1 program that draws, so K2 is entirely shader-drawn and the
   emulated VU1 has no render work left.
3. **The native 3D path** (rank 1), in steps that each keep a switch:
   a. persistent per-level geometry for fixed units (stop copying every triangle twice a frame);
   b. textures decoded at load plus the per-frame animated ones (CLUT loads and slot set-up leave the GS thread);
   c. native pipeline states and draw submission outside the GS stream, ordered by a frame queue, into the GS
      targets; 2D stays on the emulated GS;
   d. a frame camera marker and per-batch object keys (for planar reflections, RT and later effects).
   This is the performance step (the 144 Hz limit) and the RT data step: converting a path to a native shader that
   keeps its model-space geometry and materials on the GPU is what makes it RT-able. Paths that stay as GS primitives
   (2D, anything left on recompiled VU1) can never be ray traced.
4. **Water: single pass, then planar reflections on the single-plane tracks** if RT is far off (cheap once 3 exists).
5. **RT water reflections through Vulkan interop** (section 4.3): half resolution, opaque world + cars, sky colour on
   a miss, SSR fallback. Add skinned models and cut-outs after.
6. **Car paint**: one pass with a world-space reflection vector into a dynamic cube map from the native path; RT on
   cars only if the cube map is not enough.
7. Native 2D and the rest of the enhanced-mode items (soft particles, bloom on flares) when wanted.

## 6. Open points

- Which car parts and objects carry the env-map flag (a survey of the car and object resources' model heads, flag
  0x20, as `bbk_export.py` reads level shapes).
- How the normal sky is drawn (part of the world shape or a scenery object) and what an RT miss should return.
- Whether the level shape's matrix is the camera's view matrix (shape at the origin, unrotated), which would give the
  frame camera without a marker.
- `vulkaninfo` / `glxinfo` on the RTX 2060 for the RT and interop extensions; a first GL/Vulkan interop test belongs
  in a stand-alone tool like `tools/rhi_triangle`.

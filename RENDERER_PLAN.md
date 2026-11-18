# Native Vulkan renderer: plan

Decided 2026-11-18: once the game code is native, the priority is a full native Vulkan renderer. The game's own drawing
code calls Vulkan directly; there is no PS2 graphics emulation in that path. This is not the ps2-port-engine backend
(`PS2X_GS_VK=1`), which still emulates the GS from raw GIF packets.

## Goals
- The game draws through a renderer interface (`rc::gfx`) at the level of its own concepts (a shape with a matrix, a
  sorted alpha triangle list, a sprite, a font string, a water strip, an effect quad, a texture upload, a movie frame),
  not GS packets.
- Two backends behind one switch: the **GS backend** emits exactly what the game emits today (the reference, and what
  the oracle and live comparisons check), and the **Vulkan backend** draws with Vulkan. `RC_RENDERER=gs|vk`.
- Visual parity with the GS path at 1x first (screenshot comparison), then the reasons for doing it: resolution
  scaling, widescreen (with HUD anchoring), stable 60 fps on the heavy levels (18, 17, 24, 6, 8, 9, 12), no VU1 in the
  frame.
- Both versions (USA NTSC 640x448-ish, Europe PAL 512 lines): the backend takes the display mode from the game.

## Why the order matters
The oracle and the live shadow check render code by comparing what it writes: GS/VIF packets in RAM. A renderer that
no longer writes packets can't be checked that way. So:
1. the render-side units must be native and checked first (all are, except `TSORender` and the GFX_P2 set-up functions,
   in the leftovers batch running now);
2. `rc::gfx` is introduced as a refactor whose GS backend produces byte-identical packets (checked by the existing
   oracle and shadow, so the refactor itself is verified);
3. only then the Vulkan backend, checked by screenshots against the GS backend, path by path.

## How a frame is drawn today (to be confirmed by the P0 survey)
- `Rdr_StartFrame` and the RDR DMA chain (`rdr.cpp`): VIF packets that upload a VU1 program and data; the program
  writes GIF packets to the GS over PATH1.
  - Track and scenery: `TRdr_VUShape` -> `TRdr_VUShapeProg` (C++ `decomp/vu1/trdr_shape.cpp`, GLSL
    `decomp/shaders/trdr_shape.vert`, both checked against VU1; `RC_TRACK_SHADER=1` already draws it with the shader
    in the GL backend through `decomp/vu1/track_draw.cpp`).
  - Shiny pass: `EnvRdr` (C++ + shader mode 1/2). Skinned drivers: `Rdr_VUSkin` (C++ + shader mode 3). Water:
    `Rdr_Water` (C++, three passes per triangle with state changes). Track Editor pieces: `TERdr_VUShape` (C++,
    checked, still VU1 in the game). Version-2 shapes: `Rdr_VUShape` (never runs in game data).
  - RDRPOLY (`RdrPoly`, `RdrPolyTexture`, `RdrPolyCamera`): HUD, menu and effect polygons batched into VIF packets.
  - RDRALPHA: transparent triangles, sorted back to front once a frame (`qsort` through guest_call).
- GFX_P2's packet buffer, sent straight to the GS (PATH3, double-buffered `gpGfx_DmaBuff0/1`,
  `GfxP2_RenderDmaSendActual`): SPRITE (HUD, menu, font sprites), FONT, the GfxP2 texture setters.
- VISFX (237 functions, all native): effects drawn through RDRPOLY/sprites/alpha triangles.
- Texture memory: each level's BBK holds a GS memory image (64x64 32-bit tiles) uploaded whole; textures are
  addressed by TBP/PSM/CBP (palettised 4/8-bit with CLUTs). `GfxTex_*` manages uploads; `sceGsExecLoadImage` also from
  `backdrop.cpp` (24-bit backdrops), `tedit_level.cpp` (the Track Editor's HUD map to VRAM), the MPEG decoder
  (movies, `mpeg.cpp`). `tools/track_viewer` already decodes BBK texture memory (swizzle tables in
  `decomp/generated/gs_tables.bin`).
- Display: `sceGsSwapDBuff`, `sceGsPutDispEnv`, `sceGsSetHalfOffset` (field offset dropped by the overrides),
  `GsSetCrt` (NTSC/PAL), vsync through `sceGsSyncV`.

## GS behaviour the Vulkan backend must reproduce (to be narrowed by the survey)
Blending `((A-B)*C >> 7) + D` with alpha 0x80 = 1.0 and the fixed alpha; PABE (the game's zero-alpha textures rely
on it to draw opaque, found by the God of War renderer); alpha test with its fail modes (keep framebuffer only / Z only);
24-bit (or 16-bit) depth with the GS test directions and ZMSK; texture functions (modulate with x2/128 scaling, decal,
highlight), TCC; PSMCT32/24/16 and PSMT8/4 with CSM1 CLUTs; CLAMP including region repeat; mipmaps (LOD K/L, MIPTBP)
if the game uses them; fog (FOG/FOGCOL); primitive types (triangles, strips, fans, sprites, lines); scissor; FBMSK;
DATE; any render-to-texture or framebuffer feedback (none seen yet; the survey must prove it either way). Colour
precision: the GS's 8-bit integer maths; Vulkan in UNORM8 targets with blend constants matching the GS where
possible, shader blending where not (the port_engine `gs.frag -DSHADER_BLEND` is a reference for the hard cases).

## Reference: ico-pc's renderer (study first)
`../ico-pc` (MIT, already on disk, the project this decompilation follows) has a working native renderer for a
decompiled PS2 game on Vulkan, Direct3D 12 and OpenGL, with about 12,000 lines of design docs in `docs/port/`
(`RENDER.md`, `RHI.md`, `GS.md`, `PIPELINES.md`, `TEXTURES.md`, `VU1_PROGRAMS.md`, `DISPLAY.md`,
`TESTING_PROGRAMS_RENDER.md`). Its shape, from `GS.md` and `RENDER.md`:
- The game's render layer still runs as on the PS2 and builds its packets in its own buffers. A headless build sends
  them on unchanged; the window build (`ICO_RD`) sends the same register writes to a host decoder that turns them into
  `rd` state, and reads the VU1 chains with a host VIF reader that turns each model packet into an `rd` mesh drawn by
  a shader version of the VU program. libgraph/libdma are stubs; a DMA kick does nothing.
- `rd` records a frame as command lists with the GS's finite state (blend, test, TEXA, wrap, filters as a pipeline
  key), decodes GS texture formats into a texture cache, and replays the frame on an RHI (Vulkan first), with the
  exact GS blend read from the destination in the shader where fixed-function blending can't match.
- Render-to-texture, post effects, upscaling, culling and testing programs are all documented.

This is a better route than the one sketched in P1 below: intercepting at the game's own packet buffers keeps the
game's render code byte-identical (so the oracle and shadow keep checking it) and needs no rewrite of the draw paths
to a new API. The plan therefore starts with a study of ico-pc (P0 step 0) and P1 is decided after it: either adopt
its decoder-at-the-buffers model (likely), or the `rc::gfx` interface. Code can be reused under MIT (the RHI, the
texture decoding, the pipeline cache), credited in NOTES.

Other projects, for narrower questions: OpenGOAL (Jak and Daxter, OpenGL; per-VU-program renderers rewritten for the
GPU, documented at opengoal.dev); PCSX2's Vulkan GS (GPL-3; the reference for hard GS blending/PABE/DATE cases);
BT3-Recomp (PS2Recomp + paraLLEl-GS: still GS emulation, so not this design).

## Phases

### P0: preparation (no change to the game; can run beside other work)
0. **Study ico-pc's renderer** (`docs/port/*.md`, `port/render`, `port/rhi`, `ico2/seki/src/GsBase.c`,
   `GifPacket.c`, `RegistPacket.c`, `Texture.c`): write `fork/ICO_RENDER_STUDY.md` mapping each part to RC's
   equivalents (RC's packet buffers: the RDR DMA chain, GFX_P2's packet buffer, RDRALPHA; its VU programs: track, env,
   skin, water, TERdr; its uploads), what can be reused as is, what must change, and a recommendation for P1.
1. **GS usage survey.** A runtime-side counter (env-gated, in the GS front end) of every distinct GS state the game
   uses: PRIM types, ALPHA, TEST, FBA/PABE, ZBUF, TEX0 PSM/TFX/TCC, TEX1 (LOD/mip), CLAMP, FOG use, FRAME/ZBUF
   formats, scissor, texture uploads (size/format/source), any draw whose TBP is a framebuffer. Tagged by the draw
   path that issued it (the native function on the call stack: shape, poly, alpha, sprite, font, water, effect,
   backdrop, movie). Run over boot, front end, Track Editor, levels 1-24 (demo races), a podium. Output: a table
   that becomes the backend's feature list. Nothing outside the survey's list gets implemented.
2. **Reference captures.** A fixed set of screenshots from the GS path (front end screens, each level at fixed fields
   with `RC_BOOT_LEVEL`, Track Editor, movies), both versions, deterministic enough to compare (same pad script,
   same fields). Plus per-path masks (`RC_GFX_MASK`, implemented in both backends) so one path can be compared alone.
3. **Window and input decision.** raylib owns the window (GL context), keyboard, gamepad and audio. A Vulkan surface
   needs a window without a GL context. Recommended: in `vk` mode a GLFW window with `GLFW_NO_API` (GLFW is already
   linked inside raylib; 3.4 runs natively on Wayland), input read through GLFW, raylib kept for audio only. Alternative:
   SDL3 for window, input and gamepads. Decide in P0 with a small spike (open window, Vulkan surface on the RTX 2060,
   pad input, audio still playing).
4. **Scaffolding.** Vulkan device/swapchain/frame pacing (volk + headers already fetched by
   `third_party/port_engine/CMakeLists.txt`; `glslangValidator` installed; port_engine's `vk_device.cpp` as a
   reference), validation layers (not installed: `vulkan-validation-layers` package or built from source), RenderDoc
   for captures.

### P1 (to be decided after the ico-pc study): the `rc::gfx` interface, GS backend only (byte-identical refactor)
Route every draw path through `rc::gfx` calls whose GS backend writes exactly the packets written today. Checked by
the oracle (`--all` 0 FAIL both versions) and live shadow on several levels: no visible or measurable change.
Design points: the frame structure (begin/end frame, passes, the alpha sort), texture handles in place of raw TBP
(with a lookup from GS addresses while the game still thinks in TBP), state blocks in place of GS registers, and
where the VU1 programs sit (in the Vulkan backend their work moves into vertex shaders: track, env and skin shaders
exist; water and TERdr have C++ references).

### P2: Vulkan backend, path by path
Order (each compared alone with masks, then together): clear/display and movies (full-screen images) -> sprites and
fonts (front end becomes usable) -> polygons and alpha triangles -> track shapes (shader exists) -> env, skin, water,
Track Editor pieces -> VISFX -> backdrops, HUD map. Textures: decode BBK GS memory at level load into host textures
(palettes applied in the shader or pre-expanded; decided by the survey), invalidated by later uploads.

### P3: enhancements
Resolution scaling, widescreen (projection and HUD anchors: ps2-port-engine `docs/GPU.md`), frame pacing at 60, MSAA
or SMAA, and dropping the VU1 path from the frame. The GS backend stays as the reference and the oracle's target.

## Delegation
P0 ico-pc study: Fable (it decides the architecture). P0 survey and captures: Opus. P0 window spike and Vulkan scaffolding: Opus. P1 interface design: Fable (the
architecture decisions are the hardest part and shape everything after), implementation in batches by Opus with the
oracle as the check. P2 hard paths (blending/PABE/alpha-test semantics, water, textures and palettes): Fable; the
mechanical paths: Opus. Every batch re-checked independently and logged in `AGENT_USAGE.md`, as for the decompilation.

## Open questions
- Does the game ever render to a texture or read the framebuffer back (mirrors, menus showing a 3D car, screen
  effects)? The survey answers it; it decides whether the backend needs render targets beyond the swapchain.
- Interlaced field rendering: does the game draw full frames or fields? (`sceGsSetHalfOffset` suggests frame mode with
  a field offset.) Decides the native output size.
- Mipmaps: used or not.
- How much of the SDK GS/DMA/VIF library code is still reached once `rc::gfx` exists (the plan is to skip decompiling
  those libraries and let the Vulkan backend replace them).

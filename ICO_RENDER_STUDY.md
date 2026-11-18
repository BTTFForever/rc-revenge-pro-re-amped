# ico-pc's renderer, mapped onto RC Revenge Pro

Study for P0 step 0 of `RENDERER_PLAN.md`, written 2026-11-18 from a read of both trees. Nothing was built or run.
ico-pc is `../ico-pc` at commit 4e6224495 (MIT). Paths starting `port/`, `ico2/` or `docs/` are in ico-pc; paths
starting `decomp/`, `overrides/`, `third_party/` or `tools/` are in this fork; the runtime is
`~/PS2Recomp-sotc/ps2xRuntime`. Anything marked **guess** was not confirmed in code. GS register values were decoded
by hand from the constants in the sources; the decodings are mine and the survey should confirm them.

How much of ico-pc was read: `docs/port/GS.md`, `TEXTURES.md` and the head of `ARCHITECTURE.md` in full; the model,
state, target, texture, replay, blend and dump sections of `RENDER.md`; the first section of `PIPELINES.md`; the file
table, interface and Vulkan head of `RHI.md`; the run, memory, mesh and oracle sections of `VU1_PROGRAMS.md`; the
frame, window, pacing, presentation, scale and interpolation sections of `DISPLAY.md`; the build section of
`SHADERS.md`; the row list of `DIVERGENCES.md`; headings only for `RENDER_POST.md`, `WORK_BUFFERS.md`, `MSAA.md`,
`CONVENTIONS.md`. Code was read for sizes, dependencies and function names, not line by line. Section 8 (beyond
graphics) rests on a lighter read still: the summaries of `AUDIO.md`, `FMV.md`, `INPUT.md`, `SAVES.md`, `CONFIG.md`,
`UI.md` and `docs/OPTIONS.md`.

## Summary

- **Recommendation: decoder at the buffers, not `rc::gfx`.** RC's game code keeps writing exactly the bytes it writes
  today. A host decoder reads them at the two DMA kicks every GS byte already passes through (VIF1 on channel 1, GIF
  on channel 2) and turns them into a recorded frame that a Vulkan replay draws. The oracle and the shadow check stay
  valid without any refactor, because no native function changes. P1 as written in the plan (a byte-identical
  `rc::gfx` refactor of every draw path) is not needed.
- **RC's seam is narrower than ico-pc's.** ico-pc had to edit its packet helpers (`setGsReg` feeds `gsWrite`). RC
  needs no edit in game code at all: three kick points and the image uploads cover everything (section 2.6).
- **Reuse from ico-pc:** the RHI (`port/rhi/rhi.h` and `port/rhi/vk/`, about 16,700 lines of C) nearly as is; the GS
  integer maths for shaders (`port/shaders/gs_math.hlsli`); the texture format decoders; the design of rd's state
  block, pipeline key, blend paths, alpha-test split, frame dump and replay tool. **Do not take rd itself**
  (93,000 lines, built around ICO's 13 lists, its post effects, its VU programs and its frame interpolation).
- **What RC needs that ico-pc never had:** a shadow copy of GS texture memory (RC uploads a raw GS memory image per
  level and samples it in other formats, so textures cannot be decoded from files as ICO's are); image uploads into
  the frame buffer (backdrops, movies); a picture made of two frames' packets (the 2D packet of frame N is drawn over
  the 3D chain of frame N-1); split screen; and coexistence with an emulated machine.
- **Biggest risks:** the frame order above; palette textures inside the level memory image; the exact blend with
  PABE, which RC uses by default on its 3D paths; the runtime's raylib window; and the movie player, which runs from
  the vertical blank interrupt.

## 1. ico-pc's architecture

### 1.1 Where it intercepts

The game's own render layer (`ico2/seki/src`) is compiled in every build and builds its packets and display lists in
its own buffers as on the PS2. Sony's libgraph and libdma are stubs: a DMA kick does nothing, the GS is always idle,
`sceGsSyncV` waits for the next simulated vsync (`port/null/gfx_null.c`, header comment and `sceGsSyncV`,
`sceGsSyncPath`, `sceDmaSend`). There are two taps, both below the game's drawing API and above the hardware:

| tap | where | what it does |
| --- | --- | --- |
| GS register writes | `ico2/seki/src/GifPacket.c` (`setGsReg`, `gsWrite`, `gsApplyPrim`, `gsVertex`, `gif_HostWriteRegs`) | in the window build `setGsReg` calls `gsWrite` instead of writing the packet; every `gif_*` helper keeps its body. `gsWrite` turns PRIM, RGBAQ, ST, UV, XYZ2, TEX0, TEX1, CLAMP, ALPHA, TEST, ZBUF, FBA, PABE, TEXA, FRAME, XYOFFSET, SCISSOR into rd state calls and batched primitives (table in `docs/port/GS.md`, "The register decoder") |
| VU1 chains | `ico2/seki/src/MicroCode.c` (`mc_HostDma`, `mcHostVif`, `mcHostGif`) | a host VIF reader walks the chains the game built: UNPACK goes to a staging buffer, `MSCAL 0` (`SET_GSREGISTER`) sends the GIF packet to the register decoder, other MSCALs become `rd_vu_call`, MSCNT draws a batch, DIRECT sends GIF data to the decoder |

Model geometry skips the decoder: each model packet becomes one rd mesh holding the VIF UNPACK payload unchanged
(`ico2/seki/src/Packet.c` (`pac_hostBuild`), `rd_create_vu_mesh`), drawn by a shader version of the VU program
(`ico2/seki/src/RegistPacket.c` (`regHostMesh`), `rd_draw_vu_mesh`). The principle, from `port/render/rd_mesh.h`: the
GPU gets what the VU got (the same vertex stream, the same VU data memory as a constant block, the same MSCAL code
choosing the shader entry), so nothing is derived again and no rounding differs before the shader runs.

What the decoder does not model is logged once and handled by a stated fallback (`GS.md`, "What is reported once":
unknown FBP, ALPHA outside the known modes, HIGHLIGHT, context 2, fog, region clamp, dithering). Effects whose
generic mapping would be wrong (fog from the Z buffer, blur chains, shadow volumes, water reflections) record on rd
directly from their own files (`GS.md`, "Effects recorded on rd directly").

### 1.2 Headless and window builds

`ICO_HEADLESS=ON` has no window, renderer or audio device and runs vsyncs as fast as it can; it serves traces and
tests. The window build defines `ICO_RD`. Both compile the whole render layer, so everything the draw phase changes in
game state (fades, view matrices, heap allocations by packet builders, random number use) is identical with or
without a renderer (`docs/port/ARCHITECTURE.md`, "What runs"; `GS.md`, "The layer in both builds"). The frame latch
and the culling changes are in both builds because they are game logic. This is ico-pc's equivalent of RC's oracle
requirement, and it is why the tap is below the game's API.

### 1.3 rd: the recorder

- **Lists.** A frame is 13 ordered command lists, one per display-list priority of the game, recorded by the game
  thread in any order and replayed 0..12 against one persistent state block. Draws are never sorted by material; GS
  state leaks from list to list and frame to frame as on the GS (`port/render/rd.h` header; `RENDER.md`, "The
  model").
- **Commands.** Fixed 40-byte `RdCmd` records with no pointers; variable data in a per-frame payload arena addressed
  by offset. State calls record deltas (`RDC_TEST`, `RDC_BLEND`, `RDC_TEXTURE`, `RDC_TARGET`, `RDC_SCISSOR`...);
  actions are `RDC_CLEAR`, `RDC_SCREEN` (GS-space primitives), `RDC_COPY`, the VU draws (`RDC_MESH`, `RDC_SKINNED`,
  `RDC_GRID`, `RDC_PARTICLES`) and post stubs (`port/render/rd_internal.h`, `rd_core.c`; `RENDER.md`, "Recording").
  Because a frame has no pointers it can be written to a file as it is.
- **Coordinates** stay in GS units (12.4 fixed point about 2048,2048; GS Z; UV in 12.4 texels or STQ) until the replay
  converts them under the GS sampling rules and the scale in force (`port/render/rd_replay.c`, header comment).
- **State.** `port/render/rd_state.h` enumerates only the GS state the game reaches: twelve ALPHA modes (`RdBlend`),
  fourteen TEST values, three TEXA modes, clamp or repeat, nearest or linear, MODULATE or DECAL. `RdPipelineKey`
  (16 bytes: program, blend path, Z test and write, colour mask, stencil mode, target format, topology, sample count)
  selects a GPU pipeline; everything else (AREF, FIX, sampler, PABE, FBA, DATE) is a uniform. The reachable set of
  keys is enumerated and created at start-up so no frame stalls on a pipeline compile (`port/render/rd_pipeline.c`;
  `PIPELINES.md`).
- **Frame boundary.** `dl_Clear` opens a frame (`rd_begin_frame`) and drops one never kicked (`rd_discard_frame`);
  `dl_Swap`, the PS2's kick, ends it (`rd_end_frame`) and the replay follows (`ico2/seki/src/DisplayList.c`).
- **Texture cache.** `port/render/rd_tex.c`, `rd_tex_decode.c`: the TIM2 bytes the game keeps in RAM are decoded once
  into an RGBA8 texture keyed by (texture id, content generation, TEXA mode). Formats: PSMCT32/24/16/16S, PSMT8,
  PSMT4, the H variants, CSM1 CLUT order; raw GS alpha kept (0x80 = 1.0); padded to 2^TW x 2^TH so REPEAT wraps as on
  the GS. **No VRAM exists**: the game's VRAM allocator still runs as bookkeeping, and a TEX0 is resolved to a cache
  entry through a resolver the texture layer registers (`ico2/seki/src/Texture.c` (`texHostTexture`,
  `texHostResolve`, `texHostBind`)). A CLUT animation bumps the generation.
- **Replay.** `port/render/rd_replay.c` (`rd__replay_frame`) walks the lists into one RHI command list. Render passes
  stay open across draws to the same targets. A draw that samples its own target samples a copy taken just before.
  Sprites, fans and strips are expanded to triangle lists; consecutive screen draws with the same bindings are merged.

### 1.4 The RHI

`port/rhi/rhi.h` (1,898 lines) is a small C interface: opaque 32-bit handles with a generation, buffers, textures,
samplers, shaders, bind groups, pipelines, explicit texture state transitions (`rhi_cmd_barrier`), render passes,
draws, copies, readback, two frames in flight (`rhi_wait_frame`). Backends: Vulkan (`port/rhi/vk/`, 11 files, about
12,800 lines), D3D12, D3D11, OpenGL, selected at run time through a table (`port/rhi/rhi_backend.c`). The Vulkan
backend uses volk, accepts Vulkan 1.0 to 1.3, falls back from dynamic rendering to render pass objects and from
timeline semaphores to a fence ring, keeps a pipeline cache file, and has dynamic depth and blend state where the
device offers it (`port/rhi/vk/vk_device.c`, `vk_pipeline.c`, `vk_cmd.c`, `vk_swapchain.c`; `RHI.md`, "Vulkan").
Its only outside dependency besides volk is SDL3: surface creation and instance extensions
(`SDL_Vulkan_CreateSurface`, `SDL_Vulkan_GetInstanceExtensions` in `vk_device.c`), window size, and mutexes
(`vk_swapchain.c`, `vk_pipeline.c`). `rhi.h`, `rd.h` and `rd_tex.h` all have `extern "C"` guards.

### 1.5 VU programs as shaders, and their checks

Five microprograms became vertex shader families (`port/shaders/vu_prelit.hlsl`, `vu_lit.hlsl`, `vu_skin.hlsl`,
`vu_grid.hlsl`, `vu_particle.hlsl`, shared code in `vu_common.hlsli`). Each draw carries VU data memory 0..35 as the
program reads it (`VuCB.vu_mem`), bones in a second block, and the vertex stream is the UNPACK payload. Each
invocation has all three vertices of its triangle, so per-triangle decisions (the region test, scissor) agree across
the triangle (`VU1_PROGRAMS.md`, "The mesh path").

Checks: `port/render/vu1_ref/` holds a CPU reference per program that does what the instructions do to one batch and
returns what the program writes to the GS (ST, RGBAQ, XYZ2 with the ADC bit), in plain C with the VU's rounding and
accumulator order; the shader tests compare against these (`VU1_PROGRAMS.md`, "The CPU oracles"). RC already has the
same pair: `decomp/vu1/*.cpp` are the references, checked against the real VU1 programs with 0 differences
(`NOTES.md`, "VU1 programs converted"), and `tools/shader_check.cpp` checks `decomp/shaders/trdr_shape.vert` against
captures.

### 1.6 Exact GS blending, PABE, alpha test, depth

- **Blend.** The GS computes `((A - B) * C >> 7) + D` in integers with 0x80 = 1.0. Default path: fixed-function
  blending chosen per mode (`PIPELINES.md`, first table): additive and subtractive forms premultiply in the shader
  (`min((Cs*F) >> 7, 255)`) and blend ONE/ONE; the lerps use dual-source blending with factor F/128; `Cd*FIX + Cs`
  uses the second source as the destination factor; the Ad forms use DST_ALPHA. The float blender rounds to nearest,
  one level above the GS on about half the channels of a lerp (`DIVERGENCES.md` VD-7).
- **Exact blend (opt-in).** The fragment shader reads the destination pixel and computes the GS result itself
  (`gs_dst_channel` in `port/shaders/gs_math.hlsli`), through an input attachment read in rasterization order or a
  storage image under fragment shader interlock (`port/render/rd_dst_read.c`; `RENDER.md`, "Destination reads and
  blend paths").
- **PABE** (blend only where the source alpha's top bit is set): the unblended colour goes out through the second
  output on the dual-source paths; the premultiplied and Ad forms cannot do it and blend every pixel (logged once).
- **Alpha test.** ATST and AREF are uniforms (`gs_alpha_pass`). A draw whose failing pixels write something
  different (AFAIL FB_ONLY, ZB_ONLY, RGB_ONLY) is split into two draws; where failing and passing pixels write the
  same thing (Z write off with FB_ONLY) the test is dropped and it is one draw (`PIPELINES.md`, "The AFAIL split").
  An alpha planner drops the test or the blend when the texture's alpha range and the vertex alpha range decide the
  outcome for every pixel (`port/render/rd_alpha_plan.c`).
- **Depth.** GS Z maps to a depth that grows with it (`gs_z_to_depth`), so GS GEQUAL is GPU GEQUAL; the scale depends
  on the Z format (`port/render/rd_frame.c` (`rd__target_z_scale`)).
- **Sampling.** The GS samples at integer pixel coordinates; the frame constants move GS coordinates half a pixel so
  the GPU's centre rule selects the same pixels (`rd_replay.c`, header comment). TEXA is applied per texel in the
  shader when it matters. Bilinear filtering on the GS's 4-bit grid is reproduced for the Original preset.

### 1.7 Render targets and post effects

FRAME.FBP selects a named rd target (SCENE, DISPLAY, work buffers, shadow levels); any other FBP becomes a temporary
render-to-texture block, and a TEX0 with that block's address samples it (`GifPacket.c` (`gsTargetOfFbp`,
`gsAliasTarget`, `gsResolveTex0`)). Post passes (reduction to the displayed field buffer, fades, letterbox,
softening, glow, motion blur, fog from depth, shadow stencil count) are recorded as `rd_post` kinds that issue the
same register writes the game's packets do, so state leaks as on the PS2 (`port/render/rd_post.c`, `rd_blur.c`,
`rd_shadow.c`, `rd_water.c`, `rd_fog_path.c`; `RENDER_POST.md`). **RC appears to have none of this** (section 5).

### 1.8 Display, presentation, pacing

- The game steps once per simulated vsync; the window paces vsyncs to real time (20 ms PAL, 16.683 ms NTSC) and
  drops lag beyond 100 ms (`port/platform/window_host.c` (`ico_window_pace`); pure rules in
  `port/platform/pace_policy.c`, tested with a fake clock).
- Presentation blits the finished picture into a centred box of the swapchain with the right pixel shape, black
  outside; then overlays (`port/render/rd_present.c`). The interlace half-line offset is never applied
  (`ico2/seki/src/GsBase.c` (`gsbHostField`); `DIVERGENCES.md` VD-4).
- With a frame rate above Original, `rd_end_frame` only closes the frame and the window calls `rd_present(alpha)`
  repeatedly: the last closed frame is replayed with every keyed draw blended from its match in the frame before
  (`port/render/rd_interp.c`). Section 8.1.

### 1.9 Upscaling and widescreen

- **Scale.** Scene-class targets are their GS size times the scene scale; work buffers keep PS2 sizes
  (`rd_core.c` (`rd__target_scale_of`)). Sprites on a scaled target snap corners to whole GS pixels. Optional MSAA,
  supersampling, FSR 1.0 and 3.1 (`DISPLAY.md`, `MSAA.md`).
- **Widescreen** is done in the game, not by stretching: the "native frame" is the GS window made wider (512 at 4:3,
  688 at 16:9) with the PS2's height and vertical field of view; the projection is unchanged and only the window's
  extent grows (`port/include/ico_frame.h`; `GsBase.c` (`gsbHostFrameLatch`)). Culling is widened to match. 2D helpers
  map the 640 grid to the centred 4:3 box unless inside a full-frame scope (`GifPacket.c` (`gifUiW`,
  `gif_FullFrameBegin`)). The VU programs drop triangles with a vertex outside the GS region; the port's "region
  rule" draws them clipped instead (`VU1_PROGRAMS.md`, "The region rule").

### 1.10 Window, input, threads

- One SDL3 window; backend picked from config; device made through `rd_init`; the whole reachable pipeline set
  created before the first frame behind a progress screen (`port/platform/window_host.c` (`ico_window_open`)).
- Input is SDL3 keyboard, mouse, up to 8 gamepads and touch, through a binding table into a virtual pad that a host
  libpad serves (`port/input/input_sdl.c`, `bindings.c`, `vpad.c`, `pad_host.c`).
- Every `rd_*` recording call comes from the game thread. The replay, uploads and present run on a render thread;
  ending, submitting and presenting the command list can run on a third submit thread. The game thread waits only
  when it wants to change state a picture is still reading (`port/render/rd_thread.c`, `rd_submit_thread.c`;
  `RENDER.md`, "Threads").

### 1.11 Testing tools

- **Frame dumps and the replay tool.** F12 or `dump_every` writes an `.rddump`: one recorded frame with its lists,
  payload, textures and meshes (`port/render/rd_dump.c`). `rd_replay_tool` renders a dump to a PNG with no game
  (`port/render/tools/rd_replay_tool.c`), with options for stats and A/B switches. Dumps hold game assets and are
  never committed.
- **Tests.** `port/render/test/` (listed in `TESTING_PROGRAMS_RENDER.md`): state and pipeline enumeration, pixel
  tests against the GS's integer arithmetic, mesh tests, golden pictures from dumps; variants rerun the same tests
  without dual-source blending, with minimum device limits, and with each blend mode.
- **Start-up self-tests** on the real device: blend (`rd_blend_selftest.c`), DATE, the fog path, the vertex
  pre-pass.
- **`DIVERGENCES.md`** lists every place the port's picture or logic knowingly differs from the PS2, with the code
  that does it. RC should keep the same list from the first draw.

## 2. RC's equivalents

### 2.1 A frame today

From `decomp/src/main.cpp` (`MainGameProcess`'s loop, lines 449-546), `decomp/src/rdr.cpp` and
`decomp/src/gfx_p2.cpp`:

1. `Rdr_StartFrame` picks this frame's half of the tag array (`gRdr.pDmaTag`, two chains of 1000 tags;
   `currDmaTag` = 0 or 1000) and rotates the camera's side planes. `GfxP2_BeginScene` clears `zWriteEnabled`.
2. Game code draws. 3D paths append "call" tags to the chain (`Rdr_AddDmaCall`). 2D paths append GIF data to
   `gGfxP2.pDmaBuff` at `buffIdx`. Per view: world (`LsRenderWorld` or `TEditRenderGameTrack`), shadows, objects,
   vertex animation, `TSORender`, pickups, `HudPanel`, weapons, `RdrAlphaAddToDmaChain`, `VfxRender`. Split screen
   repeats this for the second view after `SetFrameWindow(1)`, which changes the scissor
   (`RdrState_SetScissor(0, 3, 0x70, 0x27d, 0xed)`) and the camera's vertical draw offset (plus or minus 60.0).
3. `Rdr_EndFrame` (`rdr.cpp:222`):
   - `GfxP2_EndScene` sets EOP on the last GIF tag.
   - `GfxP2_SwapBuffers` (`gfx_p2.cpp:19`): `sceGsSyncPath`; `GfxP2_RenderDmaSendActual` (0x18ff88, still
     recompiled) sends the 2D buffer with `sceDmaSendN` on the GIF channel and switches to the other buffer
     (`gpGfx_DmaBuff0/1`, 0x50000 bytes each); `sceGsSyncPath`; `sceGsSyncV`; `sceGsSetHalfOffset`;
     `sceGsSwapDBuff(&gGfxP2_DBuff, frameNumber)`.
   - an end tag (id 0x70) closes the chain; `GfxTex_Update` re-uploads animated textures; `sceGsSyncPath`;
     `startVif1Chain` (`rdr.cpp:95`) writes D1_TADR and D1_CHCR = 0x145 and VIF1 starts walking the chain.

**The picture is made of two frames' packets.** The 2D buffer of frame N is sent before the flip, into the buffer
the 3D chain of frame N-1 was drawn into; the flip then shows that buffer and clears the other; the 3D chain of frame
N starts only after the flip and draws while the game runs frame N+1. So a displayed picture is 3D of frame N-1 with
the 2D of frame N on top. This follows from the call order above and from what `sceGsSwapDBuff` does in Sony's
library (shows the buffer just drawn, makes the other the draw target with its clear); RC's copy of that library is
recompiled and was not read, so the SDK half is a **guess** to confirm in the survey. `NOTES.md` already suspects
this double buffering for the speedometer flash.

`GfxP2_RenderDmaSend` is empty (`rdr.cpp:384`), so the calls to it in the frame loop send nothing.

### 2.2 Packet buffers and who writes them

| buffer | where | written by | format |
| --- | --- | --- | --- |
| frame chain | `gRdr.pDmaTag` (32,000 bytes: 2 x 1000 tags) | `Rdr_AddDmaCall`, `Rdr_EndFrame` (`rdr.cpp`) | DMA "call" tags (id 0x50) to packets that end with "ret" (0x60); one "end" (0x70) |
| state packet ring | `g_RdrStateDmaBlocks` (256 packets of 0x90 bytes) | `RdrState_ForceApplyStateBlock` (`rdr.cpp`) | VIF UNPACK of a GIF A+D packet with six registers (ALPHA_1, CLAMP_1, SCISSOR_1, TEST_1, ZBUF_1, PABE), then MSCAL to the loaded program's "kick what is at TOP" entry (0x2050 for the shape program, 0x2370 for the env program), FLUSHA |
| shape packet ring | `gRdr.pShapeData` (360 packets; 384 in `pTrackEdData`), `pSkinData`, `pWaterShapeData` | `TRdr_VUShape`, `TRdr_VUShapeCell`, `TERdr_VUShape`, `Rdr_VUSkin`, `Rdr_Water`, `RdrPolyInitCamera`, `RdrAlphaAddToDmaChain` (`shape.cpp`, `rdr.cpp`) | a copy of a template packet with the camera in the shape's space, projection scale, tint, env rows written in (`writeShapeCamera`, `writeTint`, `writeCameraRows`) |
| shape triangle lists | level data (`sRdrVUShape.pTri`), fixed up at load by `Rdr_FixVUShape`, `Rdr_FixVUShapeDma`, `Rdr_FixVUGridDma`, `Rdr_FixupShapeTex` | level loader | VIF packets of texture blocks: register data, then triangles of ten quadwords (normal with a winding flag, three positions, three STQ, three colours; `decomp/shaders/trdr_shape.vert` header) |
| polygon channels | `gRdrPolyClass.channels[4].pDmaBuffer[2]` (sizes in `channelSizes`) | `RdrPoly`, `RdrTri`, `RdrPolyTexture`, `RdrPolyFlush` through `sceVif1Pk*` (`rdr.cpp`, `vifpk.cpp`) | per texture run: the six state registers, TEX0, TEX1_1 = 0x60, MIPTBP1 = 0, MIPTBP2 = 0, a triangle count, then ten-quadword triangles (`beginPolygon`, `emitTriangle`); MSCAL 0, FLUSHA |
| alpha triangles | `RDRALPHA`'s two buffers (`alpha.pDmaTags[buffer]`) | `RdrAlphaAddTriangles`, `RdrAlphaAddToDmaChain` (`shape.cpp:746-930`) | triangles collected from shapes, sorted far to near with the game's `qsort`, drawn by the shape program |
| 2D buffer | `gpGfx_DmaBuff0/1` | SPRITE (`sprite.cpp`, `SpriteStrip`), FONT through sprites (`font.cpp`), RECT (`rect.cpp`), LINE (`line.cpp`), `Rdr_DrawLine`, HUD (`hud.cpp`), the `GfxP2_*` register helpers and texture setters (`rdr.cpp`) | plain GIF: PACKED tags (ST, RGBAQ, XYZ2; triangles, every third vertex kicks) and A+D register writes |

Which game units draw through what (from a grep of `decomp/src`): `RdrPoly`/`RdrPolyTexture` from VISFX
(`visfx*.cpp`), `collision.cpp`, `object3.cpp`; sprites from `font.cpp`, `amenu*.cpp`, `front_menu.cpp`,
`loadsave_process.cpp`, `tedit_*.cpp`; `TRdr_VUShape` from `misc.cpp`, `tedit_cursor.cpp`, `tedit_level.cpp` and
three VISFX files; `TERdr_VUShape` from `tile.cpp`; `Rdr_VUSkin` from `skinanim.cpp`; `RdrState_*` directly from
`front.cpp`, `main.cpp`, `pickup.cpp`, `weapon.cpp`, `tedit_autocomp.cpp` and VISFX.

### 2.3 VU1 programs

Six programs, each a VIF packet with MPG codes called from the chain by `Rdr_UploadProg` when it is not the one
loaded (`rdrShapeCurrProg`). The address of the program packet identifies the program, so the host needs no hashing.

| program (symbol) | used by | C++ reference | shader | check so far |
| --- | --- | --- | --- | --- |
| `TRdr_VUShapeProg` | track and scenery (`TRdr_VUShape`, `TRdr_VUShapeCell`), **RDRPOLY, RDRALPHA**, state packets | `decomp/vu1/trdr_shape.cpp` | `trdr_shape.vert` mode 0 | 0 differences on real runs; drawn by shader with `RC_TRACK_SHADER=1` |
| `EnvRdr_VUShapeProg` | shiny pass | `env_shape.cpp` | modes 1, 2 | 0 differences, 33M packets |
| `Rdr_VUSkinProg` | skinned drivers (`Rdr_VUSkin`) | `skin_shape.cpp` | mode 3 | 0 differences, 18.6M packets |
| water (`gRdr_WaterEndShapeData`) | `Rdr_Water` | `water_shape.cpp` | none | 0 differences; C++ replaces VU1 in game |
| `TERdr_VUShapeProg` | Track Editor pieces (`TERdr_VUShape`) | `terdr_shape.cpp` | none | 0 differences on 284,151 real runs; still VU1 in game |
| `Rdr_VUShapeProg` | version-2 shapes (`Rdr_VUShape`, `Rdr_VUShapeSetup`) | `rdr_shape.cpp` | none | synthetic only; never runs on shipped data |

The shared clipping and packet-writing code is `decomp/vu1/vu_shape.{h,cpp}`. The references return a `KickList`: the
GIF packets the program would send. `decomp/vu1/track_draw.cpp` already does inside the emulator what ico-pc does on
the host: at the MSCAL it sends the program's register packets to the GS and turns each texture block's triangles
into one batch drawn by the shader with the GS state in force at that point.

Water draws each triangle three times with GS state changes between passes and rewrites the triangle in VU memory
as it goes (`water_shape.cpp`, header), so it cannot be one batch without changing how overlapping water blends
(`NOTES.md`).

### 2.4 Texture and image uploads

Every upload goes through `sceGsExecLoadImage` (SDK, recompiled) or through the game's own GIF chain builder. All of
them:

| path | function | destination | format | notes |
| --- | --- | --- | --- | --- |
| level texture memory | `BBLoad` (0x112038, recompiled; pseudo-C in `decomp/generated/pseudo_all.c`) | GS memory from byte address 0x1e0000 (block 0x1e00) upward to 0x400000 | 64x64 PSMCT32 tiles, 0x4000 bytes each | **a raw image of GS memory**: the textures inside it are PSMT8, PSMT4, 16 and 32 bit, addressed later by TBP. `nextTexAddr` is left after it |
| single textures | `GfxP2_LoadTexMip` (recompiled), called by `GfxTex_ManageTexture` (`gfxtex.cpp`) from `TextureConstructDaemon` | next free block from `nextTexAddr` | the texture's own `fmt` (0, 1: 32/24 bit; 2 and 10: 16 bit; 0x13: PSMT8; 0x14: PSMT4), each mipmap level its own block | writes `idxList[level]` (the TBP) and `miptbp1/2` into the `_sGfxTexture` |
| palettes | `GfxTex_ManageTexture` (`gfxtex.cpp:177`) | block after the texture; `clutVramAddr` | PSMCT32, 16x16 (8-bit) or 8x2 (4-bit) | |
| animated textures | `GfxTex_Update` (`gfxtex.cpp`), every frame from `Rdr_EndFrame` | each level's block again | as above | a new frame of the animation every 3 game frames |
| `GfxP2_LoadTex` | recompiled | as `LoadTexMip` level 0 | | callers not checked |
| backdrops | `BackdropDisplayBMP24` (`backdrop.cpp:116-122`) | **the frame buffer**: block 0 or 0xa00, width 640, two 640x128 halves | PSMCT32 converted from a 24-bit BMP | which buffer depends on `gRdr.frameNumber & 1` |
| movie frames | `setLoadImageTags`, `loadImage` (`ldimage.cpp`), from `mpeg.cpp` | **the frame buffer**: block 0, width 640, at y = 0 or y = 0x100 | PSMCT32 in 16x16 tiles, one IMAGE transfer per tile | the game's own DMA chain on the GIF channel (D2_CHCR = 0x105), started from the vertical blank handler `vblankPlayer` |
| movie background | `clearBackground`, `setLoadImageTagsTile` (`mpeg.cpp:272-281`) | block 0, 640x512 | one 16x16 tile repeated | |
| Track Editor HUD map | `TEditCopyHudMapForTrackLevelToVRAM` (`tedit_level.cpp:580`) | the map sprite's texture block | 64x64 PSMT4 | |

After `TextureConstructDaemon` places a single-frame texture it moves the 0x50-byte header and lets the loader free
the pixel data (`gfxtex.cpp`, comment above `TextureConstructDaemon`). So the pixels are not in RAM later and **must
be captured at upload time**.

TEX0 is built by `gsTex0Of` (`rdr.cpp`): TBP0 = `idxList[0]`, TBW = width / 64 (at least 1), PSM = `fmt`, TW and TH
= log2 of the sides, TCC = 1, TFX = 0 (MODULATE), and for the palette formats CBP = `clutVramAddr`, CPSM = PSMCT32,
CSM1, CLD = 1.

No `sceGsExecStoreImage` call exists anywhere in `pseudo_all.c` (one unused `sceGsStoreImage` local in a Track Editor
function), so the game never reads GS memory back.

### 2.5 Display, flip, PAL and NTSC

- `GfxP2_Init` (0x18e770, recompiled; called through `Gfx_Init(0x280, 0xf0)` from `DisplaySetMode`,
  `decomp/src/display.cpp:53`): `sceGsResetGraph(0, 1, 2, 1)` (interlaced, NTSC, frame mode),
  `sceGsSetDefDBuff(dbuff, PSMCT32, 640, 240, ztest 3, PSMZ32?, clear 1)` and two clear packets. The Z format the
  game then writes in ZBUF_1 is 0x10000a0 (`GfxP2_ZWriteEnable`, `RdrState_SetStateBlockToDefaults`): Z buffer at
  page 0xa0, PSM field 1, which is PSMZ24. Layout that follows: frame buffers at blocks 0 and 0xa00, Z at 0x1400,
  textures from 0x1e00.
- The game draws full 640x240 frames at the field rate and shifts alternate ones by half a line
  (`sceGsSetHalfOffset(draw, 0x800, 0x800, field ^ 1)`). The override already drops the offset
  (`overrides/rc_revenge_pro_overrides.cpp`; `NOTES.md`, "Game overrides").
- **Europe.** `decomp/generated/eu/changed_functions.txt`: `GfxP2_Init` differs by one constant (2 to 3: PAL in
  `sceGsResetGraph`); `DisplaySetMode` is resized (256 lines, per `NOTES.md` "Constants, PAL screen (240 -> 256
  lines)"); the default scissor's bottom line is 0xfd instead of 0xed (`RdrState_Init`,
  `RdrState_SetStateBlockToDefaults`, `VfxRenderer_ReadySteadyGo`); `RdrAlphaCameraToRdrCamera` and
  `RdrPolyInitCamera` have other vertical constants; `GfxP2_Tex`, `GfxP2_TexExt`, `GfxP2_TexFX`,
  `GfxP2_RenderDmaSendActual`, `mpegOnInterruptPLAY`, `mpegOnInterruptLOAD` and `BackdropDisplayBMP24` differ. These
  run recompiled in the Europe build (`RC_DIVERT_IF_CHANGED`). A decoder that reads packets sees both versions the
  same way; a function-level API would need both variants of each.
- Movies use their own display: `sceGsSetDefDispEnv(disp, 0, 640, 256, 0, 0)` and `sceGsPutDispEnv`
  (`mpeg.cpp:84-86`), two 640x256 pictures in one 640x512 area at block 0, flipped by the display registers.
- The timer is EE timer 0 (`decomp/src/timer.cpp`); vsync waits go through `sceGsSyncV`, replaced by the runtime's
  stub in the overrides.

### 2.6 Where a host decoder sits

Everything the GS receives arrives by one of three routes. A decoder needs exactly these:

| # | kick | function | what the host reads |
| --- | --- | --- | --- |
| K1 | 2D packet | `GfxP2_RenderDmaSendActual`, called from `GfxP2_SwapBuffers` (`gfx_p2.cpp:25`) | `gGfxP2.buffIdx` quadwords of GIF data at the buffer being sent |
| K2 | 3D chain | `startVif1Chain` in `Rdr_EndFrame` (`rdr.cpp:95`, `:238`); also `Rdr_Vif1Send` (never called) | the tag chain at `gRdr.pDmaTag` (+1000 tags on odd frames), each call tag's packet until its ret |
| K3 | flip | `sceGsSwapDBuff` and `sceGsSetHalfOffset` in `GfxP2_SwapBuffers`; `sceGsPutDispEnv` in `mpeg.cpp` | which buffer is shown; the draw environment and its clear |
| U | uploads | `sceGsExecLoadImage` (callers in 2.4) and `loadImage` (`ldimage.cpp:20`) | destination block, width, format, rectangle, source pixels |

Two ways to attach, both leaving every native function's RAM writes as they are:

- **At the hardware registers (preferred).** All four routes end in a store to D1_CHCR (VIF1), D2_CHCR (GIF) or a GS
  privileged register. The runtime already has `g_ps2HardwareStoreHook` (`ps2_runtime.cpp:2300`), which sees each
  32-bit store to a hardware register and can drop it, and `GS::s_rawGifSink` (`gs_frontend.cpp:810`), which
  `third_party/port_engine/rc_backend.cpp` uses today to receive raw GIF data. DMA channel starts are handled in
  `ps2_memory.cpp` around lines 1447-1700. In `vk` mode the hook takes the channel 1 start for the host VIF reader
  and drops it (VIF1 and VU1 are never emulated), and channel 2's data goes to the host GIF reader instead of the GS
  front end. This covers SDK code, recompiled functions, both game versions and the interrupt-driven movie player
  with no per-function work. **Guess:** that `sceGsExecLoadImage`, `sceDmaSendN`, `sceGsSwapDBuff`'s draw
  environment and the clear all go through channel 2, and that the display registers (64-bit stores) can be read
  from the runtime's GS register copy at vsync as `latchHostPresentationFrame` does; first job of the first step.
- **At the functions.** A call into the decoder placed after the RAM writes in `Rdr_EndFrame` and
  `GfxP2_SwapBuffers`, and runtime overrides for the SDK functions (as `sceGsSyncV` is overridden today). More hook
  points and two game versions to cover; keep it as the fallback if the hardware route hides something.

Game code can still tell the decoder things it cannot see in the bytes (which draw path a run of packets came from,
for masks, widescreen anchors and interpolation keys) through calls that write nothing to guest RAM. Those are
invisible to the oracle and the shadow check, and compile to nothing in the oracle build.

### 2.7 GS state RC reaches (from the sources; the survey confirms)

| register | values seen | source |
| --- | --- | --- |
| ALPHA_1 | 0x44 `(Cs-Cd)*As+Cd` (3D default); 0x21 FIX 0x80 `(Cd-Cs)*FIX+Cs` (polygons, normal); 0x29 FIX 0x80 `Cd*FIX+Cs` (additive; env pass); 0xa1 FIX 0x80 `(Cd-Cs)*FIX` (subtractive); 2D: 0x09 `Cd*As+Cs`, 0x01 `(Cd-Cs)*As+Cs`, 0x29 with FIX 0x3f, 0x7f, 0x80; 0x21 with FIX 0x3f; 0xa1 with FIX 0x7f, 0x80 | `RdrState_SetStateBlockToDefaults`, `RdrPolyTexture`, `envState` (`shape.cpp`), `GfxP2_Tex`, `GfxP2_TexExt`, `GfxP2_TexFX` |
| PABE | 1 by default on the 3D state block; 0 for additive, subtractive, alpha-test styles and all 2D texture setters | same |
| TEST_1 | 0x702f7, 0x703f7 (alpha test LEQUAL 0x2f or 0x3f, fail KEEP, Z GREATER); 0x73017 (LEQUAL 1, fail RGB_ONLY); 0x70000 (no alpha test); Z GEQUAL for the env pass; Z ALWAYS when the Z test is "off" | `RdrState_SetAlphaTestEnable`, `RdrState_SetZTestEnable`, the `GfxP2_*` helpers |
| ZBUF_1 | 0x10000a0, with ZMSK for no write | `GfxP2_ZWriteEnable/Disable`, `RdrState_SetZBufferWriteEnable` |
| CLAMP_1 | 5 (clamp both) or 0 (repeat) | `RdrState_SetTextureClamp`, `GfxP2_TexFX` |
| SCISSOR_1 | 3..0x27d by 3..0xed (0xfd in Europe and in `GfxP2_DefRenderState`); split screen halves | `RdrState_SetScissor`, `GfxP2_Scissor`, `SetFrameWindow` |
| TEX1_1 | 0x60 (linear, no mipmaps) in polygons and `GfxP2_DefRenderState`; what the track lists write is not known | `beginPolygon`; the source comment in `GfxP2_DefRenderState` calls register 0x14 PRMODECONT, which it is not |
| PRIM (2D) | 0x5b (triangle, Gouraud, textured, blended, STQ) and variants from `GfxP2_TexExt` | `GfxP2_Tex*` |

The transparency convention is inverted against most games: with ALPHA 0x21 and PABE on, a pixel whose alpha has the
top bit clear is written opaque and one with it set takes the destination; the alpha test passes low alpha. This is
why `NOTES.md` records that zero-alpha textures rely on PABE to draw opaque. Only two of these ALPHA values (0x44,
0x29) are in ico-pc's `RdBlend` table, so rd's state enumeration cannot be reused as it is.

## 3. Mapping table

Effort: S = one batch (a day or less), M = several batches, L = many batches or a design of its own.

| ico-pc component | RC counterpart | verdict | effort |
| --- | --- | --- | --- |
| `port/null/gfx_null.c` (libgraph/libdma floor) | the runtime's DMA and GS layers; in `vk` mode the hardware store hook drops the kicks | write new (small runtime patch) | S |
| register decoder `GifPacket.c` (`gsWrite`, `gsVertex`, `gsApplyPrim`) | a GIF reader for K1 and for kicks from the VU references: PACKED and A+D, triangles, sprites (the clear), lines | write new, following `gsWrite`'s table; simpler (one context, no lists, few prim types) | M |
| host VIF reader `MicroCode.c` (`mc_HostDma`, `mcHostVif`, `mcHostGif`) | a chain walker for K2: call/ret/end tags, MPG (program identity by packet address), UNPACK into a VU memory image, STCYCL, MSCAL, FLUSHA | write new, same shape; RC's chains are call tags, ICO's are cnt/ret | M |
| display lists `DisplayList.c` (13 lists, `dl_Swap`) | none: RC has one chain and one 2D buffer | not needed; RC's frame is K1, flip, K2 in kick order | - |
| frame latch and flip `GsBase.c` (`gsbHostFrameHead`, `gsbHostFlip`) | K3; display mode from `GfxP2_Init` and `mpeg.cpp` | write new | S |
| `Texture.c` resolver, `rd_tex.c` cache keyed by texture id | shadow GS texture memory plus a cache keyed by (TBP, TBW, PSM, TW, TH, CBP) with dirty pages | write new; reuse the format decode kernels of `port/render/rd_tex.c` (`rdtex_decode`) and the swizzle tables already in `decomp/generated/gs_tables.bin`, `tools/track_viewer`, the runtime's `gs_swizzle.cpp` | M-L |
| model packets as meshes `Packet.c`, `rd_mesh.c` | shape triangle lists (`sRdrVUShape.pTri`) are static level data: upload once, keyed by address, draw by texture block | adapt the idea; `track_draw.cpp` has the per-block batching | M |
| VU shaders `port/shaders/vu_*.hlsl`, `VuCB` | `decomp/shaders/trdr_shape.vert` (track, env, skin; also covers RDRPOLY and RDRALPHA, which use the track program and the same ten-quadword triangle) | reuse RC's own; port from the GL backend's bindings to the RHI | M |
| VU CPU oracles `port/render/vu1_ref/` | `decomp/vu1/*.cpp`, `tools/shader_check.cpp` | have it | - |
| rd recorder `rd_core.c`, `rd_internal.h` (`RdCmd`, payload arena) | a much smaller recorder: state deltas, screen prims, shape batches, image blits, clear, flip | write new on the same model (fixed records, offsets, no pointers) | M |
| `rd_state.h`, `rd_pipeline.c` (finite state, 16-byte key, blend paths, AFAIL split, start-up set) | the same for RC's values (2.7) | adapt the design, rewrite the tables | M |
| exact blend `rd_dst_read.c`, `gs_dst_channel` | PABE with inverted lerps on most 3D draws | port `gs_math.hlsli` to GLSL (or compile HLSL with glslang, **guess** that it accepts it); decide fixed-function against destination read after the survey | M |
| replay `rd_replay.c` (6,292 lines) | a replay of the small command set | write new; borrow the pass rules, the half-pixel rule, sprite snapping | M |
| post effects, named targets, shadows, water blocks, fog (`rd_post.c`, `rd_blur*.c`, `rd_shadow.c`, `rd_water.c`, `rd_fog_path.c`) | none seen in RC | not needed unless the survey finds a use | - |
| RHI `port/rhi/rhi.h`, `rhi_backend.c`, `port/rhi/vk/` | none (`third_party/port_engine/gpu/src/vk_device.cpp` is a 232-line device set-up for another design) | reuse as is, Vulkan only, built as a C library | M |
| shader build (DXC, HLSL, embedded tables) | `glslangValidator` rules in `third_party/port_engine/CMakeLists.txt` (`rc_shader`) | keep RC's GLSL and glslang; write RC's shaders in GLSL | S |
| presenter `rd_present.c` | box blit of the 640x240 (or 256) picture at 4:3 | write new, small | S |
| window `port/platform/window_host.c`, pacing `pace_policy.c` | raylib window in the runtime (`ps2_runtime.cpp:877`), pacing in `EeScheduler` | write a small SDL3 window; keep the runtime's pacing first | M |
| render and submit threads `rd_thread.c`, `rd_submit_thread.c` | the runtime's GS thread | later; first replay on the thread that kicks | M (later) |
| interpolation `rd_interp.c`, draw keys | nothing | P3; section 8.1 | L |
| dumps and replay tool `rd_dump.c`, `rd_replay_tool.c`, `rd_png.c` | the runtime's `gs_recording.cpp` (GS-level, another format) | write new for RC's frame format; take `rd_png.c` (171 lines) | S-M |
| tests and self-tests `port/render/test/`, `rd_blend_selftest.c` | oracle, shadow, `shader_check` | write RC's pixel tests against a CPU model of the GS blend | M |
| `DIVERGENCES.md` | none | start one with the renderer | S |

## 4. Recommendation for P1 and P2

### 4.1 Decoder at the buffers

**Adopt it, in a stricter form than ico-pc's: a pure reader.** Reasons:

1. **The checks stay valid for free.** The oracle and the shadow comparison compare what functions write to RAM and
   which hardware stores they make. A reader changes neither. The `rc::gfx` route needs every draw path rewritten
   against an interface whose GS backend must reproduce today's bytes, checked function by function, twice where the
   Europe version differs (2.5), before the first Vulkan pixel. That is weeks of refactor whose only product is the
   status quo.
2. **The render code is small and regular at the packet level.** Section 2.2: one triangle format shared by track,
   polygons and alpha triangles; one six-register state packet; plain triangles in the 2D buffer. At the function
   level there are 237 VISFX functions, the HUD, menus, fonts, the Track Editor, each calling the same few packet
   builders.
3. **It keeps what the game does without understanding it.** Stale stack words in quads, the alpha sort's order, the
   state that leaks between paths, the 2D packet one frame ahead of the 3D: a decoder reproduces them by
   construction. An API would have to model each.
4. **RC already works this way.** `decomp/vu1/track_draw.cpp` replaces a VU program at its MSCAL with a shader batch
   while the register packets flow on; `rc_backend.cpp` taps raw GIF data. The new renderer moves the same idea out
   of the emulator.
5. **Enhancements do not need the API either.** ico-pc gets widescreen, scaling and interpolation from the decoder
   plus tags and keys the game hands over beside the packets (`gif_HostDrawKey`, the full-frame scope). RC can add
   the same kind of side calls where a later phase needs them (HUD anchors, object keys).

What it costs: the decoder is a GS front end for the subset RC uses, so the plan's wording "no PS2 graphics
emulation in that path" should be read as ico-pc reads it: no VU1 execution, no GS rasteriser, no frame-buffer VRAM,
and nothing outside the survey's list of states. The shadow texture memory (4.3) is the one piece of emulated
hardware state, kept because the level data is a GS memory image.

The oracle build and `RC_RENDERER=gs` keep the emulated path. `RC_RENDERER=vk` installs the decoder. One binary can
hold both.

### 4.2 Shape of the renderer

```
game code (unchanged) -> packets in guest RAM
   K1 / K2 / K3 / uploads  (hardware store hook or function hooks)
        |
   rc::rd front (C++, fork/render/):  GIF reader, VIF chain reader, VU program table, shadow texture memory
        |
   frame record (state deltas, screen prims, shape batches, image blits, clear, flip)   <- dump / replay tool
        |
   replay -> rhi (ico-pc's, C, Vulkan) -> SDL3 window
```

- **VU programs.** At an MSCAL the chain reader looks up the loaded program. First version, exact and slow: run the
  C++ reference (`decomp/vu1`), feed its `KickList` to the GIF reader. Every path then draws correctly through one
  code path (screen prims), which is the parity reference inside the Vulkan renderer. Second version, per program:
  record a shape batch (triangle range, a copy of the VU memory words the shader needs, the GS state in force) and
  draw it with `trdr_shape.vert`. Water stays on the reference path until someone designs its three passes.
- **Static geometry.** Triangle lists inside level data do not change after `Rdr_FixVUShape`; cache them on the GPU
  keyed by guest address and a hash of the list, and drop the cache at level load. Polygon and alpha packets are
  rebuilt every frame and go through a ring buffer. (`NOTES.md` "Device-local geometry test" found that keeping
  geometry in VRAM did not help the GL renderer; measure again here rather than assume.)
- **Frame order.** Process kicks as they arrive, in order: K1 draws 2D onto the current target; K3 presents it and
  clears the other; K2 draws the chain into that one. This reproduces 2.1 exactly, including the one-frame offset.
  Removing the offset is an enhancement for P3, not part of parity.
- **Size.** Parity at 640x240 (640x256 PAL), presented at 4:3. Then scale both axes; a vertical scale of 2 at
  horizontal 1 gives square-ish pixels and is the natural "1x" for players. The half-line offset stays off.

### 4.3 Textures

Keep a 4 MB shadow of GS memory for uploads only. An upload writes it through the block layout of the upload's
format. A TEX0 is resolved by (TBP0, TBW, PSM, TW, TH, CBP): decode from the shadow through the layout of the
sampling format into an RGBA8 host texture with raw GS alpha, cache it, and mark it dirty when an upload touches one
of its pages or its palette (animated textures, the Track Editor's map). Draws never write the shadow. Uploads that
land on a frame buffer block (backdrops, movie frames, the movie background) are recorded as image blits into the
frame target at that point in kick order, and are not textures.

Palettes can be applied at decode time (simple; one host texture per texture and palette pair) since nothing in the
sources animates a palette alone; the survey confirms. Mipmap levels are separate blocks named by MIPTBP; decode the
levels the game uploaded rather than generating them, as ico-pc does for authored levels.

This is new work with no ico-pc equivalent. `tools/track_viewer` already decodes level texture memory offline and is
the reference for correctness; `tools/bbk_export.py` and `gs_tables.bin` give test data without running the game.

### 4.4 What to take from ico-pc, and the cost

| take | how | cost of adapting |
| --- | --- | --- |
| `port/rhi/rhi.h`, `rhi_backend.c`, `rhi_backend.h`, `rhi_backend_names.h`, `rhi_pipeline_pool.h`, `rhi_bind_cache.*`, `rhi_adapter.*`, `rhi_driver_version.*`, `port/rhi/vk/*` except the Android surface | copy to `third_party/ico_rhi/` beside the existing `third_party/ico_platform/` (same pattern: README with the commit, LICENSE, local edits marked "RC:") | build as a C static library (add C to the project's languages if it is not there); `rhi.h` has `extern "C"`; volk and the Vulkan headers come from the existing `FetchContent` in `third_party/port_engine/CMakeLists.txt` (move those two declarations to the top level so both use one copy); needs SDL3 (`find_package(SDL3)` or FetchContent); strip the D3D/GL backend entries from the table; check what `vk_internal.h` expects for logging |
| `port/shaders/gs_math.hlsli` (336 lines) | port to a GLSL include | mechanical; the functions are integer maths |
| `port/render/rd_tex.c` decode kernels, `rd_png.c` | copy the functions | small |
| `port/render/rd_state.h`, `rd_pipeline.c`, `rd_dst_read.c`, `rd_alpha_plan.c`, `rd_dump.c`, `rd_replay_tool.c` | read as designs; write RC's in C++ | none as code |
| `port/platform/pace_policy.c` | later, if the runtime's pacing is not enough | - |

Credit ico-pc in `NOTES.md` "Credits" (MIT). Shaders stay GLSL through `glslangValidator`; do not bring DXC in.

**Window.** The RHI's surface code is SDL3, so the P0 window decision becomes SDL3 for window and input in `vk` mode.
The runtime calls `InitWindow` and `InitAudioDevice` (`ps2_runtime.cpp:877-892`) and reads keys and pads through
raylib (`ps2_host_input.cpp`, `Kernel/Stubs/Pad.cpp`). Smallest change: in `vk` mode skip `InitWindow`, keep
`InitAudioDevice` (**guess**: raylib's audio works without its window; the spike checks it), and feed the pad from
SDL3. Section 8.3 argues for moving audio too, later.

### 4.5 P2 order (replaces the plan's)

1. Window, device, present a test pattern (SDL3 + RHI).
2. Uploads and movies: shadow memory, frame-buffer image blits, display registers. Boot logos, the movie and
   backdrops appear. This also proves the kick hooks.
3. The 2D packet (K1) with textures: front end and menus usable. Blend and alpha test for the 2D values of 2.7.
4. The 3D chain (K2) with every program on its C++ reference: races draw correctly, slowly. Full-picture parity
   comparisons start here.
5. Shader batches: track, polygons and alpha triangles (one shader), then env, skin; TERdr and water last.
6. Scale, then P3.

## 5. Questions for the GS usage survey

Run it at the kicks (the same place the decoder will sit), so its counts are of what the decoder must handle.

| question | why it matters | what the sources say so far |
| --- | --- | --- |
| Is FRAME_1 ever written outside the flip's draw environment? Is any TEX0 TBP below 0x1e00? | render to texture, frame-buffer feedback: decides whether targets beyond two frame buffers exist | none seen; no store-image call exists |
| Which TEX1 values do the track lists carry (MXL, MMIN, K, L)? Which MIPTBP? | mipmaps: `GfxTex_ManageTexture` builds levels when a texture's flags ask; polygons write none | unknown |
| PRIM.FGE, FOG, FOGCOL ever used? | fog | 2D PRIM values have FGE clear; DEPTHCUE sets a draw distance (`misc.cpp` `DepthCueSet`), not GS fog (**guess**) |
| Every distinct ALPHA, PABE, TEST, FBA, COLCLAMP, DTHE, TEXA, CLAMP value, by path | the pipeline and uniform tables | 2.7 |
| Does any draw use AFAIL RGB_ONLY (0x73017) with Z write on? DATE? | whether the alpha-test split is needed | `GfxP2_TexFX` attrib 0x20 and polygon style 0x20 |
| PRIM types reaching the GS, including from the VU programs' clip fans, the clear sprite, lines | topology list | triangles, fans, sprite, lines expected |
| Texture formats and sizes actually sampled; CLUT formats; any CSM2, any CSA other than 0; any palette uploaded without its texture | texture cache key and palette handling | 2.4 |
| Uploads: destination, size, format, count per frame, by caller | shadow memory and dirty tracking cost | 2.4 |
| Per frame: number of kicks, order of K1/K3/K2, and whether any upload overlaps a chain still drawing | confirms the frame order of 2.1 | derived from code |
| Field and frame: the display registers at each flip, in game, front end, movies, Track Editor; both versions | output size, movie presentation | 2.5 |
| Do `sceGsExecLoadImage`, the draw environment and the clear all arrive on channel 2? Are there any PATH2 (VIF DIRECT) packets? | whether three hook points are really all | **guess** yes; none seen |
| Tags per frame against the 1000-tag limit; state packets per frame against the 256 ring; shape packets against 360 | a wider view or more draw distance (P3) will overflow them; ico-pc padded its buffers on the host (`DL_HOST_PAD`, `DPK_HOST_PAD`) | limits in `rdr.cpp`, `shape.cpp` |
| Chain contents during the front end's turntable, the podium, the credits, the Track Editor (other `Rdr_EndFrame` callers: `front.cpp`, `credits.cpp`, `tedit_autocomp.cpp`) | paths not in a race | not examined |

**What RC does that ico-pc never needed**

- A GS memory image as level texture data, sampled in formats other than the one it was uploaded in (4.3).
- Images uploaded straight into the frame buffer, with drawing possibly on top (loading screens: to check).
- A picture built from two frames' packets, and a 2D packet sent on PATH3 beside a VIF1 chain.
- Movies decoded by the SDK's MPEG library and pushed as GS image tiles from an interrupt handler, concurrently with
  a level load on the main thread.
- Split screen (two scissored views with a vertical draw offset in one frame).
- Two game versions with different vertical sizes in one renderer.
- Living inside an emulated machine: guest RAM, a scheduler with fibers, recompiled SDK code that still expects a
  DMA controller that finishes its transfers (the hook must leave the channel looking idle, as `sceGsSyncPath` and
  `sceGsExecLoadImage` wait on it; `Kernel/Stubs/GS.cpp:1226` polls D1_CHCR).
- PABE on by default with an inverted alpha convention.

**What ico-pc needed that RC does not seem to:** named work buffers, post-effect chains, stencil shadow counts,
render-to-texture water, depth fog from the Z buffer, 13 priority lists, TIM2 files, morph meshes.

## 6. First steps

Each is one batch with its check. Steps 1-3 change nothing in the game and can start now.

1. **Kick survey (Opus).** Env-gated logging at the three kicks and the uploads: prove or refute the guesses of 2.6
   (which stores happen, in what order, per frame; whether all GS traffic passes channels 1 and 2). Then the state
   survey of section 5 at the same place. *Check:* a table per scene (boot, front end, levels 1-24, Track Editor,
   podium, movies), both versions; every row of section 5 answered.
2. **Vendor the RHI (Opus).** `third_party/ico_rhi/` with its own CMake target, Vulkan only, SDL3 found or fetched;
   a stand-alone test program opens a window on the RTX 2060 and draws a textured triangle through `rhi.h`. *Check:*
   the program runs under validation layers with no errors; `ctest`-style exit code.
3. **Window and input spike (Opus).** In the runtime, `vk` mode: no raylib window, SDL3 window, pad from SDL3,
   raylib audio still playing, the game running blind. *Check:* music plays, pad script and live pad work, frame
   pacing unchanged (`tools/fps.py`).
4. **Frame record, dump and replay tool (Fable designs, Opus builds).** The command set of 4.2 as fixed records; a
   writer; `rc_replay` renders a dump to PNG through the RHI with no game. *Check:* a hand-made dump of a few
   triangles and a sprite renders to an expected PNG.
5. **Shadow texture memory and decode (Fable).** Upload capture, block layouts, decode for the survey's formats,
   cache and dirty pages. *Check:* off-line against `tools/track_viewer`'s decode of the same BBK files (every
   texture of several levels identical); in game, textures dumped at a fixed frame equal the emulated GS memory's
   decode.
6. **Uploads to the frame buffer and the flip (Opus).** Backdrops, movie frames, display registers. *Check:*
   screenshots of logos, the intro movie at fixed fields and a backdrop equal the GS path's.
7. **GIF reader and the 2D packet (Opus, blend rules by Fable).** K1 into screen prims; the 2D states of 2.7; a CPU
   model of the GS blend as the pixel test's reference. *Check:* front-end screens against GS-path captures with the
   3D masked off; pixel tests for each ALPHA/PABE/TEST combination against the CPU model.
8. **VIF chain reader with the C++ references (Opus).** K2: tags, program table, UNPACK image, MSCAL to
   `decomp/vu1`, kicks to the GIF reader. *Check:* for a recorded frame, the GIF bytes produced equal the bytes the
   emulated VU1 sends to the GS (compare at `s_rawGifSink`); then full-picture screenshots on levels 5, 17, 18, 20.
9. **Shader batches (Fable for the state and blend design, Opus for the plumbing).** Track, polygons and alpha
   through `trdr_shape.vert` on the RHI; then env and skin. *Check:* each batch type against step 8's picture of the
   same dump, in the replay tool; `tools/shader_check` still passes.
10. **Pipelines at start-up, performance pass, DIVERGENCES list.** *Check:* no pipeline created after the first
    frame on a level sweep; `tools/level_sweep.sh` frame rates on the heavy levels (18, 17, 24, 6, 8, 9, 12).

Update `RENDERER_PLAN.md` after step 1: P1 becomes "decoder and frame record", and the open questions there are the
rows of section 5.

## 7. Risks

| risk | why | mitigation |
| --- | --- | --- |
| The frame order is not what 2.1 derives | the SDK half was not read | step 1 logs it before any design depends on it |
| Not all GS traffic passes the two channels | recompiled SDK code was not read | step 1; fall back to function hooks |
| PABE and the inverted lerps do not fit fixed-function blending on some draws | PABE needs a per-pixel choice; ico-pc could not do it on its premultiplied forms | dual-source for the lerps (the RTX 2060 has it); destination read as in `rd_dst_read.c` if not exact enough; the survey says how common each case is |
| Texture decode cost at level load or with animated textures | a level image is about 2 MB sampled as many textures | decode lazily on first use; dirty pages only |
| The recompiled SDK waits on hardware the hook now bypasses | `sceGsSyncPath`, `sceGsExecLoadImage` poll channel state | the hook completes the transfer state as the runtime does today before returning |
| The movie player kicks from an interrupt while the main thread uploads | two sources of GIF kicks | the decoder takes kicks under one lock, in arrival order; record, do not draw, on the kicking thread |
| raylib without a window | not verified | step 3 is a spike for exactly this |
| Fixed-size game buffers overflow with widescreen or longer draw distance | 1000 tags, 256 state packets, 360 shape packets | P3; pad on the host as ico-pc did, with a logged high-water mark |

## 8. Beyond graphics

Scope added during the study. Read more lightly than the graphics parts (see the top).

### 8.1 Frame rate above the original

**ico-pc.** The simulation is untouched: the game steps once per simulated vsync and a Main tick is normally every
second vsync. Frame rate "Original" presents each tick's frame once. Any other setting turns interpolation on
(`port/game/video_options.c` (`ico_video_interpolate`): simply "frame rate is not Original"). Then `rd_end_frame`
only closes the frame, and the window calls `rd_present(alpha)` as often as the display allows: the last closed frame
is replayed with every keyed draw blended from its match in the frame before, so the picture runs one tick behind and
never feeds back into the game (`port/render/rd_interp.c`, header; `DISPLAY.md`, "Interpolation between ticks").
What makes this possible:

- every draw carries a key naming the same logical draw in consecutive frames (`RD_KEY(obj, part, ordinal)`, set by
  the game layer: `regKeyPart` in `RegistPacket.c`, `gif_HostDrawKey` for 2D); unmatched draws snap;
- VU draws are blended at the level of their constant blocks (matrices as a slerped rotation and a lerped stretch,
  bones, UV scroll), the camera as one rigid blend, screen prims by position and colour;
- the whole frame snaps on a camera cut, a fade edge or a large camera move (`ico_video_camera_cut`);
- culling in the game is widened to cover the cameras between ticks (`GsBase.c` (`gsbHostCullViews`));
- pacing: `port/platform/pace_policy.h` (the vsync lock steps the game on a grid of display refreshes and presents
  each step n times).

It is the largest single part of rd (`rd_interp.c` is 6,576 lines) and much of `WORK_BUFFERS.md` and
`DIVERGENCES.md` (VD-20, 25, 26) is about its side effects.

**RC.** The situation differs in two ways. First, RC already steps and draws at the field rate (60 or 50 a second),
where ICO ticks at 25 or 30; the gain from interpolation is only for displays above 60 Hz. Second, `NOTES.md` says
game speed follows the frame rate, so the first need is the opposite one: a steady 60 on heavy levels, which the
Vulkan renderer addresses by removing VU1 and the GS thread from the frame. What RC would need for more than 60:

1. the decoder model (interpolation is a property of the recorded frame; an API-level renderer could do it too, but
   nothing else in this study does);
2. keys: side calls from the native draw functions naming the object and part (`TRdr_VUShape` has the shape
   pointer; sprites and polygons need an ordinal per caller); effect particles and anything driven by `rand` snap;
3. blending shape packets: RC's packets hold the camera in the shape's space (`writeShapeCamera`), so two frames'
   packets for one shape can be blended as ico-pc blends matrices;
4. a cut signal from `CameraSetMode` and the level transitions.

Effort L, after everything else; only worth it once parity and 60 fps hold. Halving the tick instead (running the
game at 30 with interpolation to 60 on the heavy levels) is a possible use, but the game's own speed coupling would
have to be understood first; not examined here.

### 8.2 Settings menu and options

**ico-pc.** The game's menus are two tables loaded at run time (`texLayout`, `texProperty`). The port extends both
past their ends (`port/ui/layout_ext.c` (`lt_ext_add_layout`, `lt_ext_add_property`)); the game's own menu code reads
rows through `LT_LAYOUT(i)` and `LT_PROP(i)`, which return the game's row below the original count and the port's
above it, so the port's pages are drawn and navigated by the game's menu engine with its own look. Port rows are
text, a button glyph, a rectangle or an image (`UI.md`, "The layout extension"). A left or right press calls the
option's setter at once; values are written when a page is left (`port/ui/settings.c` (`ui_settings_step`,
`ui_settings_save`)). Storage: `config.toml` in the user folder, read and written by the menu, and `ico-pc.ini`
beside the executable as an override layer and for start-up switches (`port/config/config.c`,
`port/platform/host_config.c`; `CONFIG.md`).

**RC.** AMENU is native (`decomp/src/amenu.cpp`, `amenu_display.cpp`, `amenu_process.cpp`, `amenu_prog.cpp`): a menu
is an `sAMenuHeader` with a linked list of `sMenuItemStd` items (plain, choice, number, slider, tabbed), each with an
optional `sMenuItemVar` and text in groups of four languages (`amenu.cpp`, header). That is easier to extend than
ICO's fixed tables: a port page is a header and items allocated on the host side of guest RAM (or in a reserved
block), linked from the options menu and the pause menu, with variables whose feedback writes host settings. The
existing choice and slider items cover every option type below. Constraints: items and strings must live in guest
memory because the menu code follows guest pointers; adding a link changes RAM the oracle compares, so the extension
must be absent in the oracle build and off under `RC_SHADOW`; the Europe build's menu functions that differ run
recompiled, which is fine since they read the same structures. Config file: one TOML or INI beside the executable,
replacing the growing set of environment variables (`NOTES.md`, "Run-time switches"). Effort M for the mechanism,
S per page.

ico-pc's options (`docs/OPTIONS.md`) and which make sense for RC:

| page | ico-pc rows | for RC |
| --- | --- | --- |
| Display | preset, resolution (1x-16x), aspect ratio, window mode, vertical sync, frame rate, frame rate counter, video mode (PAL/60 Hz) | all except video mode (RC's version decides it); frame rate above 60 only after 8.1 |
| Picture | anti-aliasing, downsampling, FSR 1.0/3.1 and sharpness, full-height picture, texture filtering, full pixel, brightness, CRT filter, gamma/contrast/saturation, texture and model packs | anti-aliasing (MSAA), texture filtering, gamma; the rest later or never |
| Effects | glow, depth of field, softening, motion blur, fog, water reflections, cinematic bars | none: RC has no post effects; a draw-distance row (DEPTHCUE) would be RC's own |
| Graphics | graphics API, graphics card, apply | graphics card only (the RTX 2060 against the Vega; `NOTES.md` "GPU"); the API row could switch `gs`/`vk` |
| Audio | volume, music, effects, output (stereo/mono), device | the game has its own volumes; add output device and a master volume |
| Controls | remap (keyboard and gamepad columns), vibration, mouse rows, touch | remap and vibration |
| Gameplay, Extras, save states, photo mode, achievements | ICO-specific | not applicable |

### 8.3 Window, input and audio platform

**ico-pc.** SDL3 throughout: the window (`port/platform/window_host.c`), devices (`port/input/input_sdl.c`: keyboard
by scancode, mouse, up to 8 gamepads with hotplug, touch), a binding table read from `config.toml`
(`port/input/bindings.c`, `input_config.c`), a virtual pad (`vpad.c`) served by a host libpad
(`port/input/pad_host.c`), pad scripts and recordings (`pad_script.c`, `input_record.c`), and the audio device
(SDL's audio thread renders the SPU2, or the game's clock does once per vsync; `AUDIO.md`, "The path of a sound").

**RC.** raylib owns all three in the runtime: `InitWindow` and `InitAudioDevice` (`ps2_runtime.cpp:877-892`), a
48 kHz stereo stream (`ps2_audio.cpp:217` `LoadAudioStream`), keys and gamepads (`ps2_host_input.cpp`,
`Kernel/Stubs/Pad.cpp`), plus pad scripts and replays of its own (`PS2X_PAD_SCRIPT`, `PS2X_PAD_REPLAY`).

Should SDL3 take input and audio when it takes the window? Input: yes, at the same time. raylib reads input from
its own window's event queue, so with no raylib window there is no keyboard or pad to read; the bindings in
`ps2_host_input.cpp` can stay and only their device queries change. Audio: not required at first (raylib's audio is
a separate module; **guess** that it runs without the window, checked in step 3), but move it in a later batch so
the `vk` build has one platform library, one device-selection story and SDL's hotplug. Effort: input S-M, audio S.
The `gs` mode keeps raylib untouched so the reference path does not move.

### 8.4 Sound

**ico-pc.** The game's sound sequencer still runs as game code and sends its RPC pages; a host server stands in for
the IOP sound driver module, written from the driver's code command by command
(`port/audio/sndn2_host.c` (`ico_sndn2_host_serve`)); each command becomes the libsd calls the driver made, through
a libsd front end (`port/audio/spu2_sd.c`) into a software SPU2 (`port/audio/spu2.c`: 48 voices, ADPCM, envelopes,
reverb, transfers, IRQ), rendered at 48 kHz. There is no IOP.

**RC.** The runtime emulates the IOP side and the SPU2 (`iop_spu2.*`, `iop_emulator.cpp`, `ps2_iop_host.cpp`; local
patches for the SPU2 IRQ address and the IOP heap in `NOTES.md`). The game's side is native:

- SFX (`decomp/src/sfx.cpp`): VAG samples in SPU2 memory, 48 voices, driven through the SDK's remote sound library
  (`sceSdRemote`, an RPC to the IOP's `sdrdrv`), one batch of volume and pitch changes per frame (`SfxDoSfx`).
- Music (`decomp/src/rcmusic.cpp`): XA2 files streamed by the IOP's music module, RPC server 0x12345, commands
  listed in the file header (play, stop, select file, position, switch, pause, volume).
- REVSFX (`decomp/src/revsfx.cpp`) sits on SFX; `BackdropLoadVAG`/`BackdropPlayVAG` play a VAG during backdrops.

The ico-pc route would be: a host server for the `sceSdRemote` command set (generic SDK, so it maps almost directly
onto libsd calls) and one for the music module's dozen commands (needs the XA2 stream format and the module's
buffering read from the IRX), over a software SPU2. ico-pc's `spu2.c` and `spu2_sd.c` are reusable under MIT if the
runtime's SPU2 is not kept. This is only worth doing as part of removing the emulated IOP altogether; sound works
today and nothing in the renderer depends on it. Effort L; order: last. How the runtime currently services those two
RPC servers (emulated IRX modules or high-level stubs) was not checked.

### 8.5 Movies

**ico-pc.** `port/fmv/movie.c` keeps the game's `movie_init`/`movie_proc` signatures and replaces the PS2 player
underneath: `pss.c` demuxes the PSS program stream, Ittiam libmpeg2 decodes (`m2v.c`), pacing is driven once per
vsync (`movie_pace.c`), audio goes through the same stream calls as on the PS2, and the picture is drawn in its own
box (`FMV.md`).

**RC.** `decomp/src/mpeg.cpp` is the game's player, native: whole `.M2V` files (video elementary streams, no demux
needed) read into memory, decoded picture by picture with the SDK's `sceMpeg*` calls into an RGBA buffer (`rgb32`),
uploaded as GS image tiles, flipped with the display registers, all from the vertical blank handler `vblankPlayer`.
The decoding itself is the runtime's (`Kernel/Stubs/MPEG.cpp`, `IPU.cpp`; `NOTES.md` mentions MPEG streaming and end
detection in the overrides and a field-rate fix). For the renderer nothing has to change: movie frames arrive as
uploads to the frame buffer (2.4) and section 4.5 step 2 presents them. A later improvement in ico-pc's direction:
hand the decoded `rgb32` picture straight to the presenter as one texture (skipping 1,280 tile transfers a frame)
and present movies in their own box at full height. Effort S once the upload path exists. Replacing the decoder is
not needed.

### 8.6 Saves

ico-pc serves libmc from a folder in PCSX2's folder-card layout, always a formatted 8 MB card in port 0, with
completion at the next vsync (`port/save/mc_host.c`; `SAVES.md`). RC's card already works through the runtime's
`sceMc*` stubs with files under `mc0` (`NOTES.md`, "Known limits"), and CARD and LOADSAVE are native. Nothing to do;
the folder-card layout is worth copying only if importing PCSX2 saves is wanted (S).

### 8.7 Order for these

After the renderer reaches parity: (1) SDL3 input with the window (part of step 3); (2) a config file and the menu
extension mechanism with a Display page; (3) audio device on SDL3; (4) movie frames as one texture; (5) more option
pages as features land; (6) interpolation; (7) host sound, only together with dropping the emulated IOP.

## 9. Could not determine

- Whether Sony's library code behaves as assumed in 2.1 and 2.6 (the recompiled SDK functions were not read).
- What the track triangle lists write to the GS between triangles (TEX1, MIPTBP, any other register): the level data
  format was not examined, only the code that fixes it up.
- Whether loading screens draw over an uploaded backdrop.
- How the runtime handles the two sound RPC servers and whether raylib's audio runs without a window.
- Whether glslang's HLSL front end accepts ico-pc's shaders (moot if the GS maths is ported to GLSL).
- `TSORender`, `BBLoad`, `BBTransferSRAM`, `GfxP2_Init`, `GfxP2_LoadTex`, `GfxP2_LoadTexMip` and
  `GfxP2_RenderDmaSendActual` are still recompiled; they were read only as Ghidra pseudo-C.
- Which parts of the game are stepped per frame rather than by the timer ("game speed follows the frame rate" is
  taken from `NOTES.md`).

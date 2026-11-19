# Native Vulkan renderer: plan

Decided 2026-11-18 (user): once the game code is native, the priority is a native Vulkan renderer, and in the long
run a PC port with no emulation of PS2 hardware at all (end state below). Architecture chosen the same day:
**option C: get Vulkan drawing first by reading the game's own packets on the host, then move draw paths to a native
API over time, where features need it.** Background and the evidence for every step: `ICO_RENDER_STUDY.md` (the
study of ico-pc's renderer mapped onto RC, section numbers below refer to it).

## The decision: option C

| Option | What | Chosen? |
|---|---|---|
| A: read the packets | Game code unchanged; a host reader takes its GS/VIF packets at the kicks, records the frame, Vulkan draws it; VU1 programs become shaders (ico-pc's design) | the first stage |
| B: native API | The game's render functions call `rc::gfx` (shape, sprite, font, effect) instead of building packets | the direction, path by path, where a feature needs it |
| C: A first, then B where needed | | **yes** |

Why: A gets a native Vulkan picture soonest, keeps the oracle and shadow checks valid for every native function (the
game still writes the same bytes), and covers recompiled SDK code, the movie player and both versions with no
per-function work. B is the cleaner end state, so paths move to it later, starting with the ones the PC features need
(cars and track for interpolation, the HUD for anchors and widescreen).

What A costs: the reader is a front end for the subset of GS state RC uses (no GS rasteriser, no VU1 execution, no
frame-buffer VRAM). The one piece of emulated hardware state kept is a shadow of GS texture memory, because each
level's texture data is a GS memory image (study 4.3). Both go away as paths move to B and the level textures are
decoded at load time instead.

**Designed in from the start: side tags.** Packets don't say what a draw is. Native code hands the renderer small
tags beside the packets, never changing what it writes to RAM (so the oracle is unaffected): an object/draw key (for
interpolation between frames), the HUD element (for anchoring), the camera and view (for widescreen and effects), the
pass (opaque, alpha, 2D). ico-pc does the same (`gif_HostDrawKey`, its full-frame scope). The tags are also the first
step of each B migration.

## Shape of the renderer (study 4.2)

```
game code (unchanged) -> packets in guest RAM
   kicks: K1 2D GIF buffer (GfxP2_RenderDmaSendActual), K2 VIF1 chain (Rdr_EndFrame), K3 flip, image uploads
   (on the runtime's hardware-store hook at D1/D2_CHCR: the survey found no GS traffic anywhere else)
        |                                    + side tags from native code
   rc::rd front (C++, fork/render/): GIF reader, VIF chain reader, VU program table, shadow texture memory
        |
   frame record (state deltas, screen prims, shape batches, image blits, clear, flip)  <- dump / replay tool
        |
   replay -> RHI (ico-pc's, C, Vulkan) -> SDL3 window
```

- Switch: `RC_RENDERER=gs` (today's emulated path, the reference; the oracle build always uses it) or `vk`.
- VU programs: first the existing C++ references (`decomp/vu1`) feed the GIF reader (exact, slow, the parity
  reference inside the new renderer); then shader batches per program (`trdr_shape.vert` covers track, RDRPOLY and
  RDRALPHA; env and skin shaders exist; water and TERdr last).
- Frame order as the game does it today (3D of frame N-1 under the 2D of frame N, confirmed by the survey in every
  swap cycle but the loading splash's); removing the one-frame offset is an enhancement, not parity.
- One GS register file across all paths, in kick order: K2's triangles take FRAME, XYOFFSET, COLCLAMP and PRMODECONT
  from the flip's draw environment (PATH3), and PATH3's 16-bit textures take PATH1's TEXA (survey, "Frame order").
- Parity at 640x240 (640x256 PAL) presented at 4:3, then scaling.
- Reuse from ico-pc (MIT): its RHI (`port/rhi/rhi.h`, `port/rhi/vk/`) nearly as is in `third_party/ico_rhi/`; its GS
  maths (`gs_math.hlsli`) ported to GLSL; its texture decode kernels; its renderer (`rd`) as a design only. Secondary
  GLSL reference for GS blending: ps2-port-engine's `gs.frag` (already in `third_party/port_engine`). Credit both.

## Phases and steps

Each step is one batch with its check (details: study section 6). Every batch is re-checked independently and logged
in `AGENT_USAGE.md`, as for the decompilation.

### P0: groundwork (changes nothing in the game; can start as soon as the builds are free)
1. **Kick and GS usage survey** (Opus). **Done 2026-11-19**: `RENDERER_SURVEY.md` (findings), `RC_GS_SURVEY=<file>`
   in the runtime (`gs_survey.cpp`), `tools/gs_survey.py` (report). Boot, movies, front end, levels 1-24, Track
   Editor (editing, its race, card screens) on both versions; the podium and post-race results were not reached.
   Answers are folded into "Open questions" below; nothing changes option C.
2. **Vendor the RHI** (Opus). **Done 2026-11-19.** `third_party/ico_rhi/` (ico-pc `port/rhi` at 4e62244, Vulkan
   backend only, 25 files unchanged, `VENDORED.md`); SDL3 from the system or fetched, Vulkan-Headers and volk fetched;
   `tools/rhi_triangle` draws a checkerboard-textured triangle on the RTX 2060 with 0 validation errors or warnings
   and a pixel-exact read-back. How to build and run it: below, "The RHI test".
3. **Window and input spike** (Opus). In `vk` mode: SDL3 window and pad, no raylib window, raylib audio still
   playing, the game running blind; pad scripts and frame pacing unchanged. The survey checked raylib audio without a
   window (it plays); raylib's timer does not run without one, so the runtime's `WaitTime` (`ps2_runtime.cpp`) and
   `GetFrameTime` (`ps2_host_input.cpp`) need host replacements in `vk` mode.
   **Done 2026-11-19** (NOTES.md, "RC_RENDERER=vk"): runtime host platform hooks (`ps2_host_platform.h`, null by
   default), `platform/vk_host.cpp` (SDL3 window, keyboard, mouse, gamepads; RHI device and swapchain; installed only
   for `RC_RENDERER=vk`), a std::chrono host loop at 60/s. The game is not blind: the emulated GS's latched picture is
   uploaded and drawn letterboxed. playtest2 replays on both versions follow the default run's scene log exactly,
   60/50 fps, audio as default, 0 validation messages over 140 s; keyboard and pad input checked with uinput devices.
   For later steps:
   - The present path is `present(pixels, w, h, aspect)` once per host frame on the main thread, after the runtime's
     `UploadFrame` latch (which also syncs the VU1 thread). P1's frame record should replace what `present` draws, not
     the hook shape: the RHI device, swapchain, bind-group layouts and resize handling are in `vk_host.cpp` to grow
     into `rc::rd`'s presenter. The GS emulation still runs in `vk` mode (its own EGL context); turning it off is a
     P1/P2 decision once the reader draws.
   - Input stays in raylib's codes behind the hooks; a binding layer/virtual DualShock 2 (Beyond graphics, item 1) can
     replace `ps2_host_input.cpp`'s tables later without touching the runtime loop.
   - Audio is still raylib/miniaudio (works without its window, no conflict with SDL3 in one process); moving it to
     SDL3 is a separate later batch, as planned.
   - One volk in the program: the RHI links port_engine's `volk::volk`, so `PS2X_GS_VK=1` and `RC_RENDERER=vk` must
     not run together (both would load the global device table).
   - Missing in `vk` mode: debug panel, perf overlay drawing, save-state message, `PS2X_FRAME_TIMES`.
After step 1: fold its answers into this plan's open questions.

### P1: the reader and the frame record (option A)
4. Frame record, dump and replay tool (Fable designs, Opus builds); the side-tag channel defined here.
5. Shadow texture memory and decode (Fable); checked against `tools/track_viewer` off-line and the emulated GS
   memory in game. Uploads arrive as CT32 (level image, palettes) **and** PSMT8/PSMT4 (single textures, animated
   frames), so the shadow needs those write swizzles too; formats sampled are T4/T8 with CT32 palettes (CSM1, CSA 0,
   CLD 1), CT32 and CT16; about 20 animated textures are re-uploaded every frame (~80 KB). Also histogram the texture
   alpha: whether any texel has alpha above 0x80 decides the PABE question of step 7/9.
6. Uploads into the frame buffer and the flip (Opus): logos, movies, backdrops match the GS path. Movies and the
   loading splash are one IMAGE per 16x16 tile (640x240 pictures in both versions) into FBP 0x0/0x50, flipped through
   DISPFB2 from the vertical blank handler; ignore DISPLAY2's position (Europe's screen-adjust option moves it).
7. GIF reader and the 2D packet (Opus, blend/alpha rules by Fable): front end and menus usable; per-state pixel tests
   against a CPU model of the GS blend. The 2D path needs the alpha-test split (AFAIL RGB_ONLY with Z write on, 3.5 M
   primitives in the survey); its other states are plain fixed function (survey, "Draw states").
8. VIF chain reader with the C++ VU references (Opus): races draw correctly (slowly); GIF bytes equal the emulated
   VU1's for recorded frames; full screenshots on levels 5, 17, 18, 20.

### P2: speed and parity
9. Shader batches (Fable: state and blend design; Opus: plumbing): track, polygons and alpha in one shader, then env,
   skin, TERdr, water. The default 3D state (ALPHA 0x44 with PABE, two thirds of all primitives) is "blend off" while
   As <= 0x80; the rare draws above it (0.016% by vertex alpha) need a destination read or a logged divergence. No
   fog; real mip levels on two textures only. K2 walks the level's static triangle lists by reference every frame
   (5-12 MB), so uploading them once is where the speed is.
10. Pipelines built at start-up, performance pass on the heavy levels (18, 17, 24, 6, 8, 9, 12), a DIVERGENCES list.
Then `vk` becomes the default renderer; `gs` stays as the reference.

### P3: PC features on the native renderer (see "PC features")
Resolution scaling, widescreen with HUD anchors, anti-aliasing, texture filtering, frame pacing, higher frame rates,
modern effects.

### P4: move paths to the native API (option B, ongoing)
Path by path, where a feature or the end state needs it: the side tags grow into calls, the path stops building
packets in the `vk` build, and the GS build keeps the original code for the oracle. Likely order: HUD and fonts (anchors,
crisp text at any resolution), cars and track (interpolation, modern effects), effects, the rest. Level textures
decoded at load time replace the shadow texture memory once no path samples it raw.

### The RHI test (P0 step 2)
Its own CMake project; the game's build and build folders are not involved.
```
cmake -S tools/rhi_triangle -B build-rhi -DCMAKE_BUILD_TYPE=Debug && cmake --build build-rhi
tools/rhi_triangle/run.sh build-rhi --frames 120 --screenshot build-rhi/triangle.png
```
`run.sh` sets the PRIME offload variables, asks for the validation layer with synchronisation validation, and
fails unless the program passes (image check, exit code), the device is the NVIDIA one and stderr holds no
validation error or warning. With no validation layer installed, `RC_VVL_LIB=<path to
libVkLayer_khronos_validation.so>` points it at a copy (it writes the manifest). Dependencies: system SDL3 and
`glslangValidator` when present (pacman `sdl3`, `glslang`), else built from source (`RC_RHI_FETCH_SDL3`,
`RC_RHI_FETCH_GLSLANG` force that). Notes for the renderer: the RHI's clip space is y-up; GLSL bindings are
set = bind group, binding = slot + 16 (textures) / + 32 (samplers) / + 0 (uniforms) (`third_party/ico_rhi/VENDORED.md`).

## PC features (references: ico-pc `docs/OPTIONS.md`, burnout3-pc)
burnout3-pc (MIT, github.com/danielgomesvieira2000/burnout3-pc; a racing game on ps2-port-engine, still GS-emulated
but with VU1 as shaders) is the closest reference for what a racing port can offer: `games/burnout3/docs/
HIGH_FPS_PLAN.md`, `MODERN_GRAPHICS_PLAN.md`, `GRAPHICS_PLAN.md`, `NATIVE_RENDERER.md`.
- **Display:** resolution 1x up to 4x or the screen's, 4:3 / 16:9 / window shape, fullscreen/borderless/windowed,
  vsync and VRR, frame-rate counter.
- **Widescreen** with a HUD editor that anchors each HUD element (burnout3-pc F10); needs the HUD tags, later the HUD on
  the native API; the game's fixed buffers (1000 tags, 256 state, 360 shape packets) padded on the host.
- **Higher frame rates** (60 / 120 / screen rate) with the game logic at its own tick and frames between ticks drawn
  from interpolated transforms (burnout3-pc: matrices that moved, halfway back; ico-pc: keyed draws). RC's native code
  knows every car, wheel and camera matrix, so the keys come from the side tags rather than memory scans.
- **Picture:** FXAA/SMAA/SSAA or MSAA, texture filtering up to 16x anisotropic, LOD and particle options, upscaling.
- **Modern preset** (burnout3-pc's MODERN, ~1 ms a frame): ambient occlusion and sun shadows from the depth buffer,
  reprojected motion blur, bloom, colour grade, soft particles, reflections; ORIGINAL stays the default for checks.
- **Settings menu** inside the game's own menus (AMENU is native; ico-pc extends the game's layout tables), saved to a
  config file; pages for video, graphics, controls, audio, mods.
- **Controls:** full remapping, button prompts that follow the bindings, mouse in menus, rumble, DualSense triggers
  (SDL3).
- **Mods / extras** (optional): speedometer, mirror mode, free or orbit camera, more AI cars, split-screen layouts.

## Beyond graphics (added 2026-11-18)
ico-pc also replaces the PS2's other hardware layers and has a full settings menu (`../ico-pc/docs/OPTIONS.md`,
`docs/port/AUDIO.md`, `FMV.md`, `INPUT.md`, `UI.md`, `CONFIG.md`). The same direction for RC, in rough order:
1. **Platform: SDL3 window, input and audio** (window and input in P0 step 3; audio later): a window without a GL context,
   SDL3 gamepads and keyboard behind a binding layer that presents a virtual DualShock 2 to the game's pad code,
   remapping, and the audio device. Replaces raylib in the Vulkan build.
2. **Settings menu** built on the game's own menus (AMENU is native; ico-pc extends the game's layout tables so the
   port's screens are drawn, navigated and faded by the game's code), saved to a `config.toml`. Early rows: window
   mode, resolution scale, aspect ratio, vsync, frame-rate cap, anti-aliasing, volumes, controls; later rows as the
   features below land.
3. **Frame rate above the original** (see PC features). RC's game speed follows the frame rate today, so higher rates need the game to
   keep its own tick while the presenter draws extra frames by interpolating between the last two ticks (ico-pc:
   `docs/port/ARCHITECTURE.md` "interpolation", `DISPLAY.md`). Needs the native renderer (P3); the hardest feature on
   this list.
4. **Picture options from the native renderer** (P3): resolution 1x and up, widescreen with HUD anchors, MSAA/SMAA,
   upscaling (FSR), texture filtering, per-effect switches (fog, glow, motion trails) where the game has them.
5. **Sound**: a host replacement for the IOP sound driver the game's music and effect RPCs talk to, driving a
   software SPU2 (ico-pc `AUDIO.md`), in place of the IOP/SPU2 emulation. Gives volume/output options and removes
   the IOP from the frame.
6. **Movies**: a host movie player with the game's entry points (PSS demux + libmpeg2, ico-pc `FMV.md`) in place of
   the MPEG/IPU emulation; movies drawn by the presenter at any resolution.
7. **Saves**: the card already works; later, PCSX2-layout folders and an importer for real PS2 saves.
The Fable study (`ICO_RENDER_STUDY.md`) covers how ico-pc did each of these and what RC would need.

## End state: no PS2 hardware emulation (user, 2026-11-18)
The long-term goal is a PC port with no emulation of PS2 hardware anywhere, reached gradually after the game code is
native. What is still emulated, and what replaces it:

| Emulated today | Where | Replaced by |
|---|---|---|
| GS (graphics chip) | runtime GS front end + GL backends | the native Vulkan renderer (P1-P2), then native API paths (P4) |
| VU1 microprograms | runtime VU1 recompiler/threads | shaders and C++ in the renderer (track, env, skin done as references; water, TERdr, Rdr_VUShape) |
| VIF/DMA/GIF transfers | runtime DMA and GIF paths | read on the host in P1-P3; gone as paths move to the native API (P4) |
| IOP + SPU2 (sound, music streaming) | `ps2xIOP` emulator, SPU2 | a host sound driver behind the game's RPCs and a software mixer (Beyond graphics, item 5) |
| IPU/MPEG (movies) | runtime MPEG/IPU stubs | a host movie player (item 6) |
| CD/DVD streaming | runtime CD stubs, IOP side | host file reads behind the game's disc calls (keeping the disc's timing where the game depends on it) |
| SIF RPC, IOP modules | runtime SIF/IOP | host servers for each RPC the game uses (sound, music, card, pad) |
| EE kernel: threads, semaphores, interrupts, vblank, timers | runtime EeScheduler (fibers) | a small host kernel layer as ico-pc's `port/platform` (likely the last to change; it is HLE already, not hardware emulation) |
| Memory card, pad | runtime HLE stubs | host libmc/libpad over folders and SDL3 (items 1 and 7) |
| SDK libraries (libgraph, libdma, libvu0, libc internals) | recompiled code | native where reached (SDK plan), stubs where the renderer/host layers make them dead |
| EE recompiled code | PS2Recomp output | native decompiled code (1,730 of 2,486: all game code since 2026-11-19; SDK and libc remain) |

When every row is done the recompiled output and the runtime's emulation can be dropped from the build; the
oracle/shadow tooling stays available against the recompiled reference as a separate test build.

## Delegation
- Fable (approved by the user for the harder parts): the frame record and side-tag design (step 4), texture memory
  and decode (5), GS blend/alpha rules (7, 9), the B-migration designs (P4), and the high-frame-rate design.
- Opus: the survey, vendoring, spikes, readers, plumbing, performance passes, and the remaining decompilation.
- Independent re-check of every batch and a row in `AGENT_USAGE.md`. One agent on the builds and the game at a time.

## Open questions (answered by P0 step 1, 2026-11-19; evidence in `RENDERER_SURVEY.md`)
- **Render to texture, frame-buffer reads:** none. FRAME is only FBP 0x0/0x50 (the flip's draw environment); no
  texture sampled from frame or Z memory; no local->host or local->local transfer.
- **Mipmaps:** two CT32 textures (MXL 3 and 4) on levels 2 and 14; everything else bilinear from level 0.
- **Fog:** none (no FGE, no FOGCOL).
- **Field or frame:** the same in every scene and both versions: SMODE2 3 (interlaced, FFMD 1), circuit 2, one
  640x240 (USA) or 640x256 (Europe) picture per frame alternating FBP 0x0/0x50; movies use the same two buffers.
- **All GS traffic on DMA channels 1 and 2:** yes. No PATH2, no stub register writes, no GIF FIFO stores; only 2
  start-up quadwords into the VIF1 FIFO (no GS data).
- **Frame order:** confirmed (3D of N-1 under 2D of N) in 208,305 of 208,355 swap cycles; the 50 others are cycles in
  which the loading splash flipped the display itself.
- **Blend cases fixed function can't match:** ALPHA 0x44 + PABE when As > 0x80 (rare; texture alpha to be checked in
  step 5); AFAIL RGB_ONLY with Z write on in the 2D path needs a split draw. Nothing uses DATE, FBA, dithering, fog,
  Ad as a factor, FIX > 0x80, COLCLAMP 0 or context 2.
- **raylib audio without its window:** plays; its timer functions do not (step 3).
- **Buffer high-water marks:** 2D buffer 136,720 of 327,680 bytes; frame tags 354 of 1000; state packets 50 of 256;
  shape 101 of 360; skin 150 of 384; Track Editor 85 of 384; water 4 of 32 (most in one frame).
- Still open: the podium and post-race results screens were not reached by any capture (the same paths draw them;
  survey them when a recording gets there); texture alpha above 0x80 (step 5).

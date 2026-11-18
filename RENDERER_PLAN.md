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
   (preferably on the runtime's hardware-store hook at D1/D2_CHCR; function hooks as the fallback)
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
- Frame order as the game does it today (3D of frame N-1 under the 2D of frame N, if the survey confirms); removing
  the one-frame offset is an enhancement, not parity.
- Parity at 640x240 (640x256 PAL) presented at 4:3, then scaling.
- Reuse from ico-pc (MIT): its RHI (`port/rhi/rhi.h`, `port/rhi/vk/`) nearly as is in `third_party/ico_rhi/`; its GS
  maths (`gs_math.hlsli`) ported to GLSL; its texture decode kernels; its renderer (`rd`) as a design only. Secondary
  GLSL reference for GS blending: ps2-port-engine's `gs.frag` (already in `third_party/port_engine`). Credit both.

## Phases and steps

Each step is one batch with its check (details: study section 6). Every batch is re-checked independently and logged
in `AGENT_USAGE.md`, as for the decompilation.

### P0: groundwork (changes nothing in the game; can start as soon as the builds are free)
1. **Kick and GS usage survey** (Opus). Env-gated logging at the kicks and uploads: confirm the frame order and that
   all GS traffic passes DMA channels 1 and 2; then every question of study section 5 (render to texture, mipmaps,
   fog, every ALPHA/PABE/TEST/TEXA/CLAMP value by path, texture formats, uploads, field/frame, buffer high-water
   marks), per scene (boot, front end, levels 1-24, Track Editor, podium, movies), both versions.
2. **Vendor the RHI** (Opus). `third_party/ico_rhi/`, Vulkan only, SDL3 found or fetched; a test program draws a
   textured triangle on the RTX 2060 with validation layers clean.
3. **Window and input spike** (Opus). In `vk` mode: SDL3 window and pad, no raylib window, raylib audio still
   playing, the game running blind; pad scripts and frame pacing unchanged.
After step 1: fold its answers into this plan's open questions.

### P1: the reader and the frame record (option A)
4. Frame record, dump and replay tool (Fable designs, Opus builds); the side-tag channel defined here.
5. Shadow texture memory and decode (Fable); checked against `tools/track_viewer` off-line and the emulated GS
   memory in game.
6. Uploads into the frame buffer and the flip (Opus): logos, movies, backdrops match the GS path.
7. GIF reader and the 2D packet (Opus, blend/alpha rules by Fable): front end and menus usable; per-state pixel tests
   against a CPU model of the GS blend.
8. VIF chain reader with the C++ VU references (Opus): races draw correctly (slowly); GIF bytes equal the emulated
   VU1's for recorded frames; full screenshots on levels 5, 17, 18, 20.

### P2: speed and parity
9. Shader batches (Fable: state and blend design; Opus: plumbing): track, polygons and alpha in one shader, then env,
   skin, TERdr, water.
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
| EE recompiled code | PS2Recomp output | native decompiled code (1,710 of 2,486 now; game code nearly complete) |

When every row is done the recompiled output and the runtime's emulation can be dropped from the build; the
oracle/shadow tooling stays available against the recompiled reference as a separate test build.

## Delegation
- Fable (approved by the user for the harder parts): the frame record and side-tag design (step 4), texture memory
  and decode (5), GS blend/alpha rules (7, 9), the B-migration designs (P4), and the high-frame-rate design.
- Opus: the survey, vendoring, spikes, readers, plumbing, performance passes, and the remaining decompilation.
- Independent re-check of every batch and a row in `AGENT_USAGE.md`. One agent on the builds and the game at a time.

## Open questions (answered by P0 step 1)
Render to texture or framebuffer reads; mipmaps; fog; field or frame rendering per scene; whether all GS traffic
passes DMA channels 1 and 2; the frame order; PABE/alpha cases that fixed-function blending can't match; whether
raylib audio runs without its window. Study sections 5, 7 and 9 list the rest.

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
   (step 4's design records one level lower as well: the kicks themselves, RENDERER_FRAME_RECORD.md)
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
   **Designed 2026-11-19: `RENDERER_FRAME_RECORD.md`.** The record has two tiers: the *kick record* (per display
   cycle, i.e. per displayed picture: the DMA kicks as their registers, the hardware stores, the side tags, and a
   sparse 4 KB page shadow of guest RAM so by-reference level data is stored once), which steps 5-8 are developed
   from off-line, and the *draw list* the readers produce for the RHI (the "frame record" of the diagram above;
   its container and first commands are in that file's section 9). Live and off-line consumers share one interface
   (`rc::rec::Consumer`: kick registers + a RAM view + tags), so the live renderer reads guest RAM in place. Dumps
   are `.rcfr` files (`RC_FRAME_RECORD=<file>`, `RC_FRAME_RECORD_AT=<trigger>`); in exact mode (software GS,
   synchronous: `tools/frame_record.sh`) they carry the emulated GS's picture per cycle, its register file before
   each kick and its memory at keyframes. `rc_replay` feeds a dump to consumers: `stats`, and `emu`, which
   re-submits the kicks to the runtime's VIF1, VU1 and software GS and must reproduce every reference bit for bit.
   Side tags are `rc::gfx` begin/end marks anchored to the 2D buffer position, the chain slot or the next kicks.
   Build batches, each with its check (details: that file, sections 10 and 12):
   1. Capture hooks in the runtime, the container, the page shadow, plain keyframes, `rc_replay --consumer stats`,
      `tools/frame_dump.py`. Check: every recorded stream hash reproduced by the host's own chain walker; nothing
      changes with recording off (scene logs, oracle).
   2. Exact mode and the `emu` consumer. Check: levels 5, 17, 18, 20, the front end, boot with the movie, a level
      load and the Track Editor, both versions: every picture and all of GS memory bit-identical on replay; planted
      faults fail.
   3. The side-tag channel and its first sites. Check: tags resolve to the right stream offsets; a sampled shadow
      run and the oracle are unaffected.
   4. Ring mode with the F10 hotkey, save-state handling, measured size and time budget, the NOTES.md section.
   Batches 1 and 2 are what steps 5-8 depend on.
   **Batch 1 done 2026-11-19** (usage: NOTES.md, "Frame record"; what it changed in the design and why:
   `RENDERER_FRAME_RECORD.md` section 13; sizes and cost: its sections 2 and 6.3). The runtime's capture hook table
   (`ps2_capture_hooks.h`) and sites, `render/record/` (format, page shadow, chain walker, writer, reader, consumer
   interface, capture with the boot/cycle/game/level/scene triggers, plain keyframes), `rc_replay` with `stats`,
   `rc_record_test`, `tools/frame_dump.py`. Check:
   - Synthetic round trip (`rc_record_test`): 8 cycles, 32 kicks over chains with every tag kind, the scratchpad and
     the wrap at the end of RAM, two segments: all 32 reproduced by the walker after the read-back, the second segment
     alone too; the file cut at 5 points reads 3, 3, 5, 4 and 8 whole cycles (each its last whole one) and says so; a
     flipped byte and a dropped page are caught.
   - Dumps (`tools/play.sh`, default renderer, RTX 2060), both versions: levels 17 and 18 (`level:300+120`), the front
     end (`RC_SKIP_INTRO=1`, `game:600+120`), boot (`boot+900`: logos, the publisher movie, title, front end). `rc_replay
     --verify`: every stream hash reproduced, USA 4,440 + 3,720 + 1,320 + 7,619 kicks, Europe the same counts (the
     streams differ: 648 / 1,149 / 527 / 3,013 MB walked USA, 660 / 1,138 / 581 / 3,284 MB Europe); no page read that
     the dump lacks; no `ODD` event but the survey's two start-up VIF1 FIFO stores in each boot dump (counted apart);
     no hook call off the EE thread; `frame_dump.py verify` clean on all eight.
   - Recording off changes nothing: `tools/play.sh replay playtest2` (150 s, `RC_SCENE_LOG`) gives the same frames,
     input clocks and scenes as the run before the change on both versions (USA frames 1, 5, 657, race at 658;
     Europe 1, 5, 1166, race at 1167); 60.0-60.2 fps (USA) and 50.0-50.2 (Europe, one 46.2 sample at boot as before
     the change: 45.8). `tools/oracle.sh --all` with new seeds (the oracle links the runtime and the overrides): USA
     seed 6160 1632 ok, 0 FAIL, 98 without a setup; Europe seed 6161 1563 ok, 0 FAIL, 90 without a setup (8 dumps).
   - Sizes (plain mode): a race cycle 457-544 KB raw, 86-102 KB stored; front end 292-340 / 63-75 KB; keyframe
     4.1 MB raw, 1.2-1.4 MB stored. Outside the game's rings a race cycle changes about 290-320 KB (two alternately
     rewritten buffers at 0x617000/0x696000 read by K2, and the animated-texture header page), as much as inside them.
     The recorder costs 2.6-6.8 ms a cycle on the EE thread (estimate was 1-2 ms; the stream hash was left out).
   **Batch 2 done 2026-11-19** (usage: NOTES.md, "Frame record"; changes to the design and why:
   `RENDERER_FRAME_RECORD.md` section 14). `ps2x::savestate::saveGraphics`/`loadGraphics` (the graphics machine's
   state but GS memory, `EMUS`) and `exactCapable` in the runtime; exact mode in the capture (GS register file and GIF
   output hash per kick, the picture at DISPFB2 per cycle, exact keyframes, `RC_FRAME_RECORD_REF`/`_STRICT`);
   `tools/frame_record.sh`; `rc_replay --consumer emu` (headless runtime as the oracle builds it, forced to the
   synchronous software GS) with `--verify`, `--png`, `--picture-before`, `--path1`, `--kick-hashes`, `--fault`.
   Check, exact-mode dumps (RTX 2060), both versions:
   - Levels 5, 17, 18, 20 (`level:300+120`), the front end (`game:600+120`), boot with the logos and the publisher
     movie (`boot+900`, `REF=10`), a level load with its loading splash (`RC_BOOT_LEVEL=17`, `boot+400`) and the Track
     Editor (`game:2400+120`, `tedit_walk.py` pad): `rc_replay emu --verify` PASS on all 16. USA: 4,440 + 4,440 +
     3,720 + 4,680 + 5,890 + 9,535 + 15,530 + 360 kicks, every GIF hash and register file equal; picture hashes
     120 x 5 + 900 + 400 + 120 of the same counts, all equal; stored pictures 120 x 5 + 90 + 400 + 120, all bit for
     bit; GS memory and registers at keyframes 2/2 (boot) and 1/1 (load), final 8/8. Europe: 4,440 + 4,440 + 3,720 +
     4,680 + 4,680 + 12,275 + 15,602 + 360 kicks, the same results. `stats --verify` and `frame_dump.py verify` pass on
     all 16 too (0 `ODD` but the two known start-up FIFO stores in boot and load). The emulated replay was bit-exact
     from the first try: no hidden state had to be found.
   - Determinism: every dump replayed with `PS2X_GS_BANDS=1` gives the same replay digest (all picture hashes) as the
     default 4 bands; level 18 also with 8 bands and a second default run, levels 5 and 17 (USA) twice.
   - Planted faults (`--fault`), USA and Europe, level 17 and boot: a flipped byte in a page the first kick reads fails
     that kick's GIF hash in that cycle; a dropped kick fails the next kick's register file (race) or the cycle's
     picture (movie) in that cycle; DISPFB2 stored with bit 0 flipped fails that cycle's FBP and picture. A flipped
     byte that no kick reads replays clean, as designed. A dump cut at 60% replays its 77 whole cycles and says so.
   - Static screens: `RC_RENDERER=vk` screenshots (`PS2X_SCREENSHOT_FIELDS=650,950`, the studio logo and the Dolby
     screen, each shown 300 fields) equal the `REFI` of the cycle shown, 0 of 153,600 (USA) / 163,840 (Europe)
     pixels different in RGB; the presentation transform is the stored R, G, B with alpha set to 255.
   - Recording off changes nothing: `tools/play.sh replay playtest2` (150 s) gives the scene log of batch 1 (USA frames
     1, 5, 657, race at 658; Europe 1, 5, 1166, race at 1167) at 60.0-60.2 fps (one 59.8 sample) and 50.0-50.2 (46.4 at
     boot, as before). `tools/oracle.sh --all` with new seeds: USA seed 7170 1631 ok, 1 FAIL,
     98 without a setup; Europe seed 7171 1562 ok, 0 FAIL, 1 skipped (`PathBuilder`: the recompiled original timed
     out on one level 17 input, also when run alone), 90 without a setup (8 dumps). The USA FAIL is `FrontEndProcess`
     (1 of 20 inputs on each of the four dumps: a byte at 0x1f02d7f 0x20 vs 0x28, scratch word 0 vs 1.0f, v0 0xcb vs
     0x2a0000): a decompilation difference the new seed reaches, not this batch's: it fails the same way with the
     runtime of batch 1 (`--only FrontEndProcess --seed 7170`, 1 of 40), and the oracle calls none of the new code.
     With batch 1's seeds: USA 6160 1632 ok, 0 FAIL; Europe 6161 1563 ok, 0 FAIL (8 dumps): the counts of batch 1.
   - Sizes and cost (exact mode): a 120-cycle race dump with every picture is 57-74 MB (a stored picture 280-400 KB);
     register files add 32 KB raw a race cycle; `EMUS` 112 KB raw, 11 KB stored. The recorder costs 3.0-6.1 ms a race
     cycle on the EE thread; the game ran near real time in exact mode (a race dump with its level load: 42-64 s).
   **Batch 3 done 2026-11-19** (usage: NOTES.md, "Frame record"; changes to the design and why:
   `RENDERER_FRAME_RECORD.md` section 15). The side-tag channel `render/gfx/tags` (`rc::gfx`; compiled to nothing
   unless `RC_GFX_TAGS`, which only the game target's build of the native and override code sets), the comparison
   setters in `rc_decomp.cpp` (both halves of a live comparison and random-input runs make no tag), resolution at the
   kick in the capture (spans by stream offset, labels, expiry after two cycle closes), `TAG` / `TAGP` / `TNAM` in the
   dump, the first sites (`Scene` in the swap override; `View` in `MainGameProcess`; `Kick` labels for K1, K2, the
   flip, texture uploads, animated textures, backdrops and movie pictures; `Draw` on the chain in `TRdr_VUShape` and
   `Rdr_VUSkin`; `Hud` in the HUD panel functions and `Draw` in `FontPrint` in the 2D packet), `rc_replay` tag
   statistics and checks, `frame_dump.py tags`. Check (RTX 2060 for the game runs):
   - A8, exact-mode dumps of levels 17 and 18, the front end and boot (`boot+900`, `REF=10`), both versions: every
     anchored tag made in a replayed cycle was resolved to a kick (USA 5,296 + 9,536 + 4,526 + 34,360, Europe 3,908 +
     5,822 + 7,692 + 42,074; the last two cycles of each dump not judged), 0 unresolved, 0 dropped, 0 repeats; every
     begin's end in the same kick (USA 2,675 + 4,817 + 2,282 + 17,180 pairs, Europe 1,971 + 2,924 + 3,889 + 21,037),
     every span's stream offset delivers the guest address its tag names (host walker). Where they land: shape and
     skin spans only in K2 (USA level 17: 1,278 shapes and 1,097 skins in 120 cycles; level 18 4,054 and 463), the
     HUD panel and font spans only in K1 (level 17 cycle 350: the panel from stream +0xc0 to +0x270 of a 0x270-byte
     K1; level 18: +0xc0 to +0x780 of 0x780, its end the end of K1); every chain span's anchor holds a `call` tag (26
     + 26 in USA level 17 cycle 350, 17 + 17 Europe, 36 + 36 USA level 18). Race cycles: one `Scene`, one `View`
     (USA), labels on every kick (K1, K2, flip, texture uploads, animated textures USA); level loads leave the palette
     and level-image tile uploads unlabelled (not first sites). Europe has no `View`, `Hud`, animated-texture or
     backdrop tags: those functions run recompiled there.
   - A1 and A2 with tags in the dumps, the same eight: `rc_replay --verify` PASS (every stream hash reproduced, 0
     `ODD` but the two known start-up FIFO stores in boot), `frame_dump.py verify` clean, `rc_replay --consumer emu
     --verify` PASS on all eight (USA 4,440 + 3,720 + 4,474 + 9,151 kicks, Europe 4,440 + 3,720 + 4,440 + 12,275, every
     GIF hash, register file, picture hash and stored picture, GS memory at keyframes and the end).
   - A4: `tools/oracle.sh --all` with new seeds: USA seed 8180 1632 ok, 0 FAIL, 98 without a setup (4 dumps); Europe
     seed 8181 1563 ok, 0 FAIL, 90 without a setup (8 dumps). Recording off: `tools/play.sh replay playtest2` (150 s)
     gives USA frames 1, 5, 657, race at 658 (twice) at 60.0-60.2 fps; Europe 1, 5, 1166, race at 1167 at 50.0-50.4
     (46.0 at boot), the same as HEAD's binary in the same session. A first Europe run gave 1167 / 1168 with the front
     end reached at input clock 0:255 instead of 0:256: a boot-timing variance, not reproduced in the next run.
   - A5's shadow half: `RC_SHADOW_RATE=20 RC_SHADOW_FIRST=1 tools/shadow.sh 17 115 0` with `RC_FRAME_RECORD` set (the
     channel live all run): USA 2,574 compares, only the known `TaskProcess` (52 of 101), `InputSnapshot` (2 of 100)
     and `AIControl` `svGlob` pad-byte (5 of 5) artefacts; without the recorder 2,572 compares, the same three (50, 3,
     1). Europe could not be checked: the shadow run stops at the level-load movie after 139 compares, with and
     without the recorder and with HEAD's binary too (it ran on 2026-11-18): open, not this batch's.
   **Batch 4 done 2026-11-20; step 4 complete** (usage: NOTES.md, "Frame record"; changes to the design and why:
   `RENDERER_FRAME_RECORD.md` section 16; A9's table: its 6.3). `RC_FRAME_RECORD_AT=key` (now the default: F10 starts,
   F10 or the count stops, again on the next F10) and `ring:<n>` (the last n cycles kept compressed in memory, whole
   segments of 120 cycles; F10 writes them with a final keyframe to the next numbered file and recording goes on); F10
   in the raylib loop and in `RC_RENDERER=vk`'s loop; the segment break at a save-state load (the cycle closes before
   the load, a keyframe flagged `afterStateLoad` follows it); `RC_FRAME_RECORD_F10` for scripted presses; the GS
   save-state lock order fixed in the runtime (timed saves deadlocked against the presentation latch). Check (RTX 2060):
   - A ring dump by F10 in a default `play.sh` session, `rc_replay --verify` PASS: the real key through a uinput
     keyboard in both host loops (USA, raylib 417 cycles, vk 411 cycles, `frame_dump.py verify` ok), simulated presses
     on both versions (USA 300 and 360 cycles, Europe 300 through the level load and 340).
   - Exact-mode ring dumps by F10 (level 17 attract demo, a save state loaded mid-dump), `rc_replay --consumer emu
     --verify` PASS on both versions: USA 340 cycles with the load and 404 after it, Europe 340 with the load and 375;
     every GIF hash, register file and picture hash, GS memory at every keyframe but the one after the load (loaded,
     not compared) and the final one.
   - A1/A2 on normal dumps (exact mode, `key+100`, one stopped by F10 after 30 cycles, one by the count), both
     versions: `stats --verify`, `emu --verify`, `frame_dump.py verify` pass.
   - A5's ring half: `tools/play.sh replay playtest2` (180 s) with `RC_FRAME_RECORD_AT=ring:300`: USA frames 1, 5
     (clock 0:255), 657 (0:907), race scene 3020 at 658 (clock 1:0), 60.0 fps (59.8-60.2 samples); Europe 1, 5
     (0:256), 1166 (0:1417), race at 1167 (1:0), 50.0 fps (49.8-50.2): the same as without the recorder.
   - A4: without `RC_FRAME_RECORD` the same replays give the same scene logs and frame rates (USA 60.0, Europe 50.0).
     `tools/oracle.sh --all`: Europe seed 9495 1563 ok, 0 FAIL, 90 without a setup (8 dumps); USA seed 9496 1631 ok,
     0 FAIL, 1 skipped (`PathBuilder`: the original timed out on a level 17 input, as with seed 7171 in Europe), 98
     without a setup; USA seed 9494 1631 ok, 1 FAIL: `AMenuDisplayMIT`, 1 of 20 inputs on each dump, also with the
     runtime of batch 3 (the oracle calls none of the new code): a decompilation difference the seed reaches, left
     open (NOTES.md, "AMenuDisplayMIT, seed 9494").
   - A9: `RENDERER_FRAME_RECORD.md` 6.3 (a race cycle 63 KB stored, 4.1-4.2 ms of recorder time; `ring:300` 22-37 MB).
   - Open: in exact mode with the ring recording, `playtest2` with a timed save state loaded in the front end stopped
     at game frame 404 in 2 of 6 runs (none of 9 without recording or with the capture idle); section 16 item 10.
5. Shadow texture memory and decode (Fable); checked against `tools/track_viewer` off-line and the emulated GS
   memory in game. Uploads arrive as CT32 (level image, palettes) **and** PSMT8/PSMT4 (single textures, animated
   frames), so the shadow needs those write swizzles too; formats sampled are T4/T8 with CT32 palettes (CSM1, CSA 0,
   CLD 1), CT32 and CT16; about 20 animated textures are re-uploaded every frame (~80 KB). Also histogram the texture
   alpha: whether any texel has alpha above 0x80 decides the PABE question of step 7/9.
   **Done 2026-11-20** (design, every check and the cost: `RENDERER_FRAME_RECORD.md` section 17; usage: NOTES.md,
   "Shadow texture memory and decode"). `render/tex/` (library `rc_tex`, not yet linked into the game or the oracle):
   the GS memory layout from its own formulas, `GsMemory` (host-to-local transfers in CT32/24/16/16S, T8, T4; a write
   serial per 256-byte block, bumped only when a transfer changes the block), `GifWalker`, TEX0/CLUT/TEXA decode to
   RGBA8 with raw GS alpha and mip levels, `TextureCache` (decodes an entry again only when a block of its texels or
   palette changed; a `generation` per entry for the incremental Vulkan upload), `TexShadow` (the consumer that
   applies the PATH3 kicks). `rc_replay --consumer tex [--with-emu]`, `rc_tex_test`, `tools/tex_check.py`. Check:
   - New exact-mode dumps, 27 per version (boot with logos, movie, title and an attract demo; the front end from its
     load; levels 1-24 from their load through 11 seconds of the demo race; the Track Editor), 19,300 cycles each
     version: `tex --verify` PASS on all 54 alone and behind `emu`. Texture memory (blocks 0x1e00 up) equals `GSVR`
     at 54 of 54 later keyframes and 27 of 27 final ones per version; the transfer registers before every kick and
     texture memory against the emulated GS after every kick, 681,563 of 681,563 kicks (USA) and 707,323 of 707,323
     (Europe); every decoded texture against the emulated GS's own sampling (a point-sampled DECAL sprite read back),
     11,433 of 11,433 textures / 32.1 M texels (USA) and 13,182 of 13,182 / 36.0 M (Europe): T4, T8, CT32, the mip
     levels of the two mipmapped textures, the CT16 texture. Step 4's 22 dumps pass too (the plain ones' GPU-backend
     `GSVR` included).
   - Decode against the track viewer on the level files: 8,980 (USA) and 9,004 (Europe) textures of 49 files each,
     byte for byte. `rc_tex_test` PASS (layout against the runtime's tables, uploads, the walker, CLD, the cache).
   - Planted faults fail where they should: a swapped column-table entry (32, 8, 4 bit) at the next keyframe, a
     dropped load-time transfer at keyframe 300, a skipped kick at the next kick's registers and the final keyframe,
     a dropped animated upload at that kick behind `emu` (it heals at the next cycle), a decode-only layout error in
     the comparison with the emulated sampling (61-209 of 213 textures).
   - **The alpha histogram** (all 54 dumps, 477 M primitives; the texture's alpha as decoded when drawn with): **no
     texture has a texel alpha above 0x80**, on either path, in any level, the front end, the title or the Track
     Editor (0 of 11,419 textures USA, 0 of 13,168 Europe, summed over the dumps; nine in ten have alpha 0 in every
     texel); the level files agree (0 of 8,980 / 9,004). No vertex alpha above 0x80 either, outside the flip's clear:
     the survey's 204,744 such primitives were its counter's artefact (one per swap cycle, the clear sprite's alpha
     carried into the next primitive). The largest source alpha of any draw is 0x80.
   - **PABE is settled, and not by the histogram alone:** both states that set PABE (ALPHA 0x44, and 0x21 FIX 0x80)
     also set an alpha test that discards every pixel with As above 0x2f / 0x3f (ATE, LEQUAL, AFAIL KEEP), and PABE
     blends only at As 0x80 or more: no pixel it would blend is ever written. Those states are **blend off with the
     alpha test as a discard**. Steps 7 and 9 need no destination read and no logged divergence for PABE; the reader
     checks the precondition when it records a state (PABE 1 implies ATE on, LESS or LEQUAL, AREF below 0x80, AFAIL
     KEEP) and logs if a state ever breaks it.
   - Cost (one core): walking and applying a race cycle's uploads (13-23 transfers, 52-124 KB) 0.32-0.43 ms; the
     animated textures change memory only every third cycle, and then 2-7 of the drawn textures are decoded again
     (16-31 thousand texels, about 0.1 ms); a level's first frame decodes 196-345 textures in 2.5-5.4 ms; a full
     decode of everything cached (324-517 textures, 3.6-5.6 MB of RGBA, on the three levels timed) 4.3-6.4 ms. Over
     all levels the cache holds 281-703 textures, 3.2-6.7 MB.
   - Not recorded for this step: split screen, the podium and results screens, the Track Editor's race (the shadow
     and decode do not depend on the scene; their alpha histogram is missing).
6. Uploads into the frame buffer and the flip (Opus): logos, movies, backdrops match the GS path. Movies and the
   loading splash are one IMAGE per 16x16 tile (640x240 pictures in both versions) into FBP 0x0/0x50, flipped through
   DISPFB2 from the vertical blank handler; ignore DISPLAY2's position (Europe's screen-adjust option moves it).
   **Done 2026-11-20** (`RENDERER_FRAME_RECORD.md` section 18; usage: NOTES.md, "Uploads into the frame buffer and the
   flip"). `render/blit/`: `FrameBlit` (a consumer on step 5's `TexShadow`/`GsMemory`: DISPFB2 from the stores, the
   displayed 640 x H rectangle read at the flip, and per word of GS memory whether a draw may have written it, so a
   picture is classed upload-only or not applicable without a rasteriser) and `BlitPresenter` (the picture through the
   RHI: upload, a texelFetch draw with the presentation transform R, G, B as stored and alpha 255, read back; headless
   Vulkan). `rc_replay --consumer blit`. Nothing is linked into the game or the oracle.
   - Check, RTX 2060, both versions: every upload-only picture equals the reference bit for bit on the CPU and through
     the RHI. New full-reference dumps (`boot`, a level-17 load): USA 261 and Europe 260 pictures against `REFI`'s
     bytes; step 5's 55 dumps (hash only): USA 416 and Europe 357; step 4's key dumps 30 + 30. DISPFB2's FBP equals
     the recorded one in every cycle (43,960 cycles of those dumps). 0 validation errors. Not applicable (a draw may be in the
     picture): every cycle with K1, K2 or another draw over the shown buffer; every cycle without any drawing was
     upload-only.
   - Upload-only pictures: legal screen and logos, the publisher movie, the loading splash (every level load and the
     demo race's restart), the front end's load.
   - Planted faults fail where they should (a dropped movie tile, DISPFB2's FBP xor 0x50, a byte flipped in memory, a
     byte flipped in the GPU upload only, which fails only the GPU check); a dropped tile that a later upload of the
     same cycle rewrites does not, as designed.
   - For steps 7-9: upload-only is a property of the picture, which the live presenter can use to show an uploaded
     picture directly; the RHI runs headless in `rc_replay`, so the Vulkan pictures of steps 7-9 can be checked there.
7. GIF reader and the 2D packet (Opus, blend/alpha rules by Fable): front end and menus usable; per-state pixel tests
   against a CPU model of the GS blend. The 2D path needs the alpha-test split (AFAIL RGB_ONLY with Z write on, 3.5 M
   primitives in the survey); its other states are plain fixed function (survey, "Draw states").
   **Designed 2026-11-20: `RENDERER_2D_DESIGN.md`** (the state block and draw-list commands, the blend, alpha-test,
   depth, sampling and rasterisation rules, the CPU model and tests, five build batches; measured with `rc_replay
   --consumer gif2dstat` on step 5's 55 dumps). What it fixes: K1 is 1-16 primitives in a race, 129-1024 in the front
   end; triangles as axis-aligned rectangle pairs, lines only in the Track Editor, no sprite but the clear. Blending
   is three fixed-function configurations with the source term computed in the shader and a dual-source destination
   factor: adds and subtracts integer exact, the lerps and `Cd*F+Cs` exact wherever the source term is at least half
   a level (a bias of 127/256 level turns the blender's rounding into the GS's floor), otherwise 0 or +1 per layer.
   The alpha-test split is two draws per group of non-overlapping primitives, in primitive order. Depth is `D32F`
   with Z * 2^-24 written by the shader, exact. The software GS, the only reference, interpolates in floats (a
   one-colour Gouraud quad is one level low on 6% of its pixels; every textured 2D pixel mixes texels), so the K1
   shader computes the reference's own per-pixel arithmetic from the primitive's vertices instead of using the
   GPU's interpolators; check (c) is then: alpha and Z equal, colour `0 <= Vulkan - reference <= L` with L the
   inexactly blended layers over the pixel. Check (c) seeds the depth buffer as well as the colour (2D primitives
   hide one another by Z everywhere; the Track Editor's lines are sorted against the 3D). Two decisions are put to
   the user at the top of that file: exact integer blending by destination read (recommended: not now), and two
   marked edits to the vendored RHI to enable `shaderFloat64` (recommended).
   - **Batch 1 built 2026-11-20** (`RENDERER_2D_DESIGN.md` section 14): the reader, draw list, plan and GIF re-encoder
     (`render/gs2d`, library `rc_gs2d` on `rc_tex`) and `rc_replay --consumer gif2d`. On all 67 exact dumps of both
     versions (steps 4-6), (a) every register the reader knows equals every kick's recorded register file, and (b)
     every drawing PATH3 kick replaced by its draw list encoded back into GIF in front of the emulated GS gives every
     picture, register file and GS memory image bit for bit. Section 3's tables come out of the draw lists exactly
     (but the 324 / 272 FRAME.FBW 0 primitives the reader drops, which cover no pixel). Found: K1 inherits ZBUF_1
     (front-end backdrop, race HUD) and in the Track Editor SCISSOR_1, TEST_1 and TEX1_1 from PATH1, so the live half
     (batch 5) needs PATH1's register writes (step 8, or the runtime's register shadow). Nothing is linked into the
     game or the oracle.
8. VIF chain reader with the C++ VU references (Opus): races draw correctly (slowly); GIF bytes equal the emulated
   VU1's for recorded frames; full screenshots on levels 5, 17, 18, 20.

### P2: speed and parity
9. Shader batches (Fable: state and blend design; Opus: plumbing): track, polygons and alpha in one shader, then env,
   skin, TERdr, water. The default 3D state (ALPHA 0x44 with PABE, two thirds of all primitives) is blend off with
   its alpha test (A<=0x2f, fail KEEP) as a discard: the test removes every pixel PABE would blend, and no draw has a
   source alpha above 0x80 anyway (step 5). The cache's alpha range per texture lets the alpha planner drop the test
   for the nine textures in ten whose alpha is 0 throughout. No
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
8. **Multichannel surround** (user, 2026-11-19; on top of item 5's host sound driver). The game's Dolby Surround
   option (sound mode 2) is already native (`decomp/src/sfx.cpp`, `revsfx.cpp`): `RevSfxPlay3D` gives every
   positioned sound a left/right pan (0-30) and a front/back pan (0-31) from its position relative to the camera; in
   Dolby mode a sound with effect flag 0x40 gets a second, rear voice, the front/back pan splits the volume between the
   front and rear voices, and `SetDolbyVolume` sends the rear voice as the average of its left and right volumes at
   +v left and -v right (out of phase: the Pro Logic matrix steers it to the surround channels). With the host sound
   driver, a 5.1/7.1 output routes the front voice to FL/FR and the rear voice to SL/SR as a discrete pair, panned
   by the rear lookup the game already has, instead of the out-of-phase mono fold; optional centre and LFE, and the
   front/back pan applied to sounds that lack the flag. A settings row: stereo / Dolby Surround (the game's own
   matrix, for Pro Logic receivers) / 5.1 / 7.1. No game logic changes: the positions already exist per sound.
   Audio stack (2026-11-19, user asked about OpenAL): the core is our own software SPU2 mixer (ADPCM decode, ADSR,
   pitch, the SPU2 reverb; checkable against the emulated sound path, as ico-pc's AUDIO.md), output through SDL3
   audio (already a dependency; 2/6/8-channel layouts; replaces raylib). OpenAL Soft (LGPL) or Steam Audio (Apache)
   only as an optional HRTF headphone mode fed with RevSfxPlay3D's world positions, not as the engine: the game
   already computes per-voice pans, and OpenAL's mixing and reverb would change the sound.
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
- **Blend cases fixed function can't match:** not ALPHA 0x44 + PABE after all (step 5: its alpha test discards
  every pixel PABE would blend, no texture has a texel alpha above 0x80, and the survey's vertex alphas above 0x80
  were a counting artefact); AFAIL RGB_ONLY with Z write on in the 2D path needs a split draw. Nothing uses DATE, FBA, dithering, fog,
  Ad as a factor, FIX > 0x80, COLCLAMP 0 or context 2.
- **raylib audio without its window:** plays; its timer functions do not (step 3).
- **Buffer high-water marks:** 2D buffer 136,720 of 327,680 bytes; frame tags 354 of 1000; state packets 50 of 256;
  shape 101 of 360; skin 150 of 384; Track Editor 85 of 384; water 4 of 32 (most in one frame).
- Still open: the podium and post-race results screens were not reached by any capture (the same paths draw them;
  survey them when a recording gets there). Texture alpha above 0x80: none (step 5); those screens' and split
  screen's alpha histograms are still to take when a dump reaches them (`rc_replay --consumer tex --with-emu`).

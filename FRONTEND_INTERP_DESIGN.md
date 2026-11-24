# The front end at high refresh rates: design (2026-11-24)

**Built 2026-11-24** (batches 1-3; batch 4 not done): see PERF_INVESTIGATION.md, "As built (2026-11-24): the menus
at high refresh rates". One correction: `VfxUpdate` reschedules every 25 ms, not 1 ms (the effect handlers were
already clock-timed).

Read-only study (nothing built, the game not run). Question from the user: the menus (front end) still run at the
video mode's rate (60 USA, 50 Europe) while races go up to 144 Hz with smooth motion; make the menus present at the
high rate smoothly too. Background: `PERF_INVESTIGATION.md`, "As built (2026-11-23): high refresh rates and drawing
between physics steps" (and its "Fixes after the user's first 144 Hz run"), commits `9243ae0` (V-Sync, variable
refresh, present log, live pad) and `dc26ba2` (Europe race rate). Line numbers are as of `7a25890`.

## Summary and recommendation

The front end's logic is stepped once per loop pass (once per vblank, since `Rdr_EndFrame` waits for one): the fly-by
camera, the animated scenery, the banner, the slider repeat, the music fades, the effects task, animated textures, the
podium's countdown, the credits. Only a few things there are timed by the clock (the carousel, the scenery's
vertex/skin animation in `TSORender`, the demo timeout, the physics task). Making every per-frame step time-scaled
(option B) means touching a dozen functions with their own counters plus every effect handler, and each would play
slightly differently from the original. Not recommended.

**Recommended: option A, "logic ticks at 60/50, extra pictures drawn between them".** While the front end runs, the
emulated vblank goes to the race frame rate (as `RcRefreshRace` does for races). Each pass of `FrontEndProcess` asks a
host clock whether a logic tick is due (every video-mode period: 16.68 ms USA, 20 ms Europe):

- **Tick pass**: the original body, unchanged in what it does and in what order, so every timing (input, slider repeat,
  fades, music, demo timeout, camera path, banner, holdoffs) stays exactly the original's. Around its 3D drawing the
  camera, the animated scenery (TSOs) and the turntable's cars are drawn blended between the previous tick and this
  one, and put back before the menu logic runs. The 2D packet (K1) it builds is kept.
- **Render-only pass** (no tick due): no logic at all. The 3D part is drawn again with the state blended by how far
  the clock is into the tick; the 2D is the last tick's K1 packet sent again (held at 60/50 Hz). Nothing that steps
  per frame is called, or it is called inside a rollback.

This is the race implementation's pattern (snapshot, blend, draw, restore) applied with a logic tick instead of a
physics step, and it reuses its pieces: `RcRefreshRace`'s vblank switching, the blend helpers in `gametask.cpp`, and
`RcInterpBeginDraw`/`RcInterpEndDraw` as they are for the podium's cars (driven by the same 20 ms physics task in the
front end). Smooth: camera fly-bys, the turntable, scenery animation and the podium cars move at the presented rate;
the 2D (menus, banner slide, fades) stays at 60/50 Hz steps, which is how it looks on the original anyway.

**Effort:** three Opus batches, about 5-7 hours of agent time and roughly 0.6-0.9 M tokens; moderate risk, all of it in
new code paths that are off when Smooth motion is off or the race frame rate is not above the video rate. A fourth,
optional batch blends the remaining 60 Hz movers (butterflies and other effect particles, the banner).

Track Editor, loading screens, the boot screens and movies stay at the video rate (simplest; see section 4).

## 1. What advances per frame in the front end

### 1.1 The loop

`MainLoop` (recompiled) calls `MainFrontInit`, `MainFrontProcess` (`decomp/src/main.cpp:275`, a call to
`FrontEndProcess`), `MainFrontTerm`. `FrontEndProcess` is native (`decomp/src/front.cpp:218-527`, one body for both
versions via `RC_VERSION_VARIANT`). One pass of its `do ... while (m.status == 0)` loop is one frame; the pace comes
from `Rdr_EndFrame` (`rdr.cpp:230`) -> `GfxP2_SwapBuffers` (`gfx_p2.cpp:29`) -> `sceGsSyncV`. There is no other
frame limiter: at a 144 Hz vblank the whole loop simply runs 144 times a second (the 2.4x the user saw).

In loop order (front.cpp line numbers), with how each one keeps time:

| line | call | per frame or timed | notes |
|---|---|---|---|
| 233 | `Rdr_StartFrame` | draw | chain ring select by `gRdr.frameNumber` parity |
| 236 | `InputSnapshot` | per frame | hit edges are between snapshots: menu navigation is per-frame edge based; no timed key repeat |
| 238-239 | `InputTestAnyKeyHeld` -> `uiDemoLoopWait = 0` | logic | |
| 240-242 | `TimerGet(gameTimer)` -> `CameraProcess(time, 0)` | **per frame** | camera mode 1 -> `CameraFollowPath` (`camera.cpp:827`): `path.interp += node->speed` each call; node and end callbacks (`PathPilotNodeCallback`, `PathPilotEndCallback`) drive menu arrival. The fly-by ran 2.4x fast |
| 243 | `TaskProcess(time)` | timed | runs due tasks with catch-up: `GameTaskProcess` every 20 ms (physics, **and another `AnimUpdateAnims`**, camera skipped in mode 1, `gametask.cpp:583`), `FrontEndTaskProcess` every 30 ms (the demo idle counter, `front.cpp:547`), `VfxUpdate` with period 1 ms (`visfx.cpp:1028`): **effectively once per call**, so per frame (effect handlers: butterflies, car-viewer sparks/skids, fireworks) |
| 244 | `enhance::poll()` | logic | the enhanced recomp's Enhancements page |
| 245 | `AnimUpdateAnims` (`anim.cpp:336`) | **per frame** | linear moves, spins, swings, bezier paths: frame counters with loop/bounce; writes TSO position/angles/matrix and collision. Also called from each physics step, so the original steps scenery 60 + 50 times a second (2,2,2,2,2,1 steps per frame: a mild judder already in the original) |
| 246 | `GfxTex_Update` (`gfxtex.cpp:192`) | **per frame** | `framecount++`, animated texture frame = framecount / 3; **also called from `Rdr_EndFrame` (`rdr.cpp:243`)**, so twice per frame |
| 247-252 | `CheckCheatKeys(.., time)` | timed | |
| 254-257 | `ObjectSfxFrontEnd`, `RevSfxUpdateAmbient`, `SfxDoSfx`, `RCMusicFadeControl` | **per frame** | sound updates and the music fade step |
| 259-271 | `Init3dFrame(.., m.time)`, `MeshInitFrame`, `LsFrameInit(m.time)`, `CellRenderInit`, `Init3dChangeViewcone`, `LsRenderWorld`, `TSORender(m.time)` | draw | `TSORender` (`tso.cpp:379`) rebuilds a TSO matrix when flag 2 is set; its vertex-animated and boned objects advance **by elapsed time** (`now - last`), so they are already rate independent |
| 273-278 | podium (`f.active` 3/4): `VfxRenderShadows`, `ObjectRenderAll` | draw | objects drawn from their physics blocks (20 ms steps); `ObjectRenderAll` steps each object's tint toward its target per call (`ObjectInterpolateTint`) |
| 282 | `CarouselRender(m.time)` (`carousel.cpp:118`) | **mixed** | turntable angle and the highlighted car's spin are **dt based** (`time - lastUpdated`); but it also decides modes from the camera's distance, enables/disables scenery groups, and gives the highlighted car a hop with `RandomGetRange(0,10) == 0` **per call**; writes each car's render object and physics copies |
| 283-284 | `VfxRenderShadows`, `CarouselDraw(m.time)` (`carousel.cpp:272`) | draw | sets `c.lastUpdated = time` (so it must get the tick's time or the next tick's dt shrinks); the locked-car question mark bobs on `mainVarG.time` |
| 286-288 | `RdrAlphaAddToDmaChain`, `BuildLockedTracksList` (logic), `HudDrawLockedTracks` (`hud.cpp:725`, 3D sprites through `RdrPoly`, i.e. the VIF1 chain, not 2D) | draw | |
| 289-319 | first frame: memory card auto-load (`TimerPause`, `LoadSaveCheckIfCardInSlot1` with up to 7 `sceGsSyncV` waits) | once | |
| 328-433 | 2D and menu logic: `DisplayRCRevengeBanner` (`front_menu.cpp:906`: x -= 5, y += 3 **per frame**), `AMenuDisplayOptions`, `AMenuProcessAll` (`amenu_process.cpp:868`: draws the menu then runs its input; slider items **repeat once per frame** while left/right is held, line 219; `specialStatsHoldoff` counts 20 **frames**, line 173, set in `DisplayVehicleStats`), `ScreenAdjust`, `ProcessRewardMessage`/`ShowMessage`, `LoadingPlayFlyingLogo` (`loading.cpp:105`, a **frame counter**), `DisplayVehicleStats` (reads the camera position, moves the carousel), `BuildTimeTrials`/`BuildProgress` (scrolling pages, `PTTScroll` steps **per frame**), `ParseSelectionTags` (camera paths, cuts, music, mode changes), `LoadSaveProcess(frameCnt)` | **per frame** | all of it writes 2D into the GfxP2 packet (K1) |
| 435-444 | fps counter, `VersionDisplay`, `GfxP2_RenderDmaSend` (a no-op, `rdr.cpp:397`) | | |
| 445-488 | attract demo: `uiDemoLoopWait > 1000` (the 30 ms task: ~30 s idle) -> status 0x10, `rand()` picks the level | timed | |
| 490 | `VfxRender` (`visfx.cpp:997`) | **mixed** | `Vfx2PodiumPlay` (podium countdown `iResultTrigger` per call from ~4000, car releases, fireworks with `rand()` and `SfxPlay`), `VfxSkyChangeStuff` (sky backdrop; podium texts), `Vfx2RenderTSFX`, `VfxCallRenderers` (draw), `Vfx3Rain`, `Vfx3Glint` (both spawn and age per call with `rand()`), `RdrPolyFlush` |
| 491 | `ObjectResetAllTint` | per frame | clears the target tints logic set this frame |
| 492 | `Rdr_EndFrame` | | K1 sent and flipped in `GfxP2_SwapBuffers`; `GfxTex_Update` again; `gRdr.frameNumber++` |
| 493-497 | `CreditsDisplay` (`credits.cpp:58`) | **its own loop** | lines scroll by `displayVarG.uiVSyncInc` (counted per `SwapBuffers` and by the vblank callback), `VfxCallHandlers` per pass, its own `Rdr_EndFrame` |
| 499-507 | after the loop: music fade, **one step per `sceGsSyncV`** | per vblank | |

Other per-frame counters noticed: `gEnvRamp` from `gRdr.frameNumber % 13` (`misc.cpp:652`, shield shimmer, races),
the HUD's flash phase from `uiVSyncInc` (`hud.cpp:375`, races).

### 1.2 Frame counts against timers, at a glance

- **Clock-timed already** (fine at any pass rate): the physics task and what it moves, the demo idle counter, the
  turntable turn and car spin (dt), the locked-car bob, vertex and skinned scenery animation, cheat keys, the timer.
- **Per frame** (2.4x fast at 144 if run every pass): fly-by camera, `AnimUpdateAnims` (scenery moves), effect
  handlers through the 1 ms `VfxUpdate` task (butterflies, sparks, fireworks), animated textures, banner slide, slider
  repeat, stats holdoff, flying logo, page scrolling, sound and music fade steps, podium countdown, rain/glints, tint
  stepping, the carousel hop chance, credits scroll, the exit fade.

### 1.3 Other non-race screens

- **Podium** (after a race, `status == 0x40`): inside `FrontEndProcess` (`FrontEndInit`, front.cpp:126-143). Covered by
  the same design; its cars are physics objects, blended by the race's existing code.
- **Results, cup ranking, pause menu**: inside the race loop `MainGameProcess` (`main2.cpp`: `BuildResults`,
  `BuildRanking`, `PauseMenuProcess`), so they already run at the race rate. See section 6, side findings.
- **Loading** (`LoadingInit` between modes), **boot screens** (`BackdropDoInitialScreens`, `backdrop.cpp`),
  **movies** (`mpeg.cpp`): their own `sceGsSyncV`-paced loops at the video rate; nothing to gain.
- **Track Editor** (`MainTrackEdProcess`, `main.cpp:309`): its own loop with `TEditDisplay(time, ++frameCount)`
  (frame counted), the cursor and menus stepped per frame, and the TERdr VU program still on the old path
  (`SHADER_SURVEY.md`). Out of scope: it stays at the video rate (no change needed, `RcRefreshRace` never touches it).

## 2. Options

### A. Logic at a fixed 60/50 Hz tick, extra pictures drawn blended (recommended)

As in the summary. What makes it cheap here: `FrontEndProcess` is our own C++, so the tick/render-only split is a
branch in one function; the 2D is all in one packet (K1, the GfxP2 buffer, `RENDERER_FRAME_RECORD.md` section 2) that
can be kept and sent again, so none of the 2D code needs a "draw only" variant; the 3D draw functions are the same ones
the race loop already calls at 144 Hz.

- Effort: 3 batches (section 3). Risk: moderate, contained (new code, off by default when the feature is off).
- Timing: identical to the original by construction (all logic on the tick; at most one tick per pass; if the machine
  cannot keep 60 passes a second, logic slows as the original would).
- Smoothness: camera, scenery, turntable, podium cars at the presented rate; 2D, butterflies and particles at the tick
  rate (batch 4 can blend the particles).
- VRR: with Variable refresh on, the front end runs at `raceRefreshHz()` (screen - 3) like races and each pass is
  presented; with it off, the window's fixed cap. Europe: 50 Hz ticks, the same code.

### B. Time-scaled steps, logic every presented frame

Every per-frame item of 1.2 scaled by `dt / tick`: camera path (`interp += speed * dt/P` with a remainder),
`AnimUpdateAnims` (frame counters, bounce, loop and collision: practically a rewrite), banner, holdoff, slider repeat
(would need a repeat timer), flying logo, page scrolls, the 1 ms effect task and every handler it runs, animated
textures, podium countdown, rain and glints, credits, music fade steps, sound updates. Each changes how something
plays (rounding, random draws per call, sound triggers), the oracle can no longer vouch for those functions, and the
list is open-ended (effect handlers). Effort several times A, risk high. Not recommended.

### C. Hybrid

A as the structure, with the parts that are already clock-timed drawn at the presented time on render-only passes:
`TSORender` with the current game time (vertex/skin animation advances smoothly by itself), the locked-car bob from
`mainVarG.time`, `LsFrameInit`'s time. Recommended A includes these (they cost nothing); "C" beyond that (time-scaling
individual per-frame steps, e.g. the camera path) would only replace blending with something riskier.

## 3. Build plan

New file `decomp/rc_front_interp.cpp` (+ a small header), next to `rc_enhance_menu.cpp`, holding the clock, the
snapshots and the blend; `FrontEndProcess` gets the branch. Helpers worth moving out of `gametask.cpp`'s anonymous
`interp` namespace into a shared header: `blendInts`, `blendFloats`, `blendShortMatrix`, `blendFloatMatrix`,
`distanceInts`, `jumpLimit`.

### Batch 1: the clock, rate switching, render-only passes with the camera blended and K1 replayed

1. **Rate switching** (`gametask.cpp:362-409`). Generalise `RcRefreshRace(bool)` to a shared helper with two users:
   `RcRefreshRace(bool)` unchanged in behaviour and a new `RcRefreshFront(bool)`. The front end goes fast only when
   `settings::smoothMotion()` is on and `raceRefreshHz()` is above the video rate (without blending the extra pictures
   would be repeats). `RcRefreshRaceUpdate` (called from `rc_enhance_menu.cpp:623` when settings change) must also
   re-apply while the front end runs (`g_racing || g_front`), so changing the frame rate or Smooth motion on the
   Enhancements page takes effect in the menus at once.
2. **Where the rate is fast** inside `FrontEndProcess`: raise after the first pass (the memory card first entry,
   front.cpp:289-319, uses `sceGsSyncV` waits); drop to the video rate for: passes with `f.loadSaveFlag != 0` (load/save
   screens, card waits), around `CreditsDisplay` (front.cpp:496; its scroll counts vblanks/swaps), before the exit
   fade loop (front.cpp:500), and on the early `return` (front.cpp:260). An RAII guard over the whole function plus
   the explicit drops. Rate changes are a `setVBlankPeriodMicroseconds` call; cheap.
3. **The tick clock**: `std::chrono::steady_clock`, period P = the video mode's vblank period (`g_normalPeriod`, so
   16,683 us or 20,000 us). Pass start (top of the loop): if `now >= nextTick`: tick; `lastTick = nextTick; nextTick +=
   P`; if `now - nextTick > P`, resync (`nextTick = now + P`: never two ticks in one pass). The first pass, any pass at
   the video rate, and any pass with the feature off are ticks. Blend factor `t = clamp((now - lastTick) / P, 0, 1)`,
   one tick behind like races (prev = what tick k-1 drew, cur = tick k).
4. **Tick pass** = the existing body with hooks:
   - before line 259 (`Init3dFrame`): `FeInterp::tick()` (rotate snapshots: prev = cur, cur = capture), then
     `FeInterp::apply(t)`.
   - after line 288 (`HudDrawLockedTracks`): `FeInterp::restore()` so the menu logic (`DisplayVehicleStats` reads the
     camera position, `ParseSelectionTags` moves it) sees the real state.
   - just before line 492 (`Rdr_EndFrame`): keep K1: copy `gGfxP2.pDmaBuff[0 .. buffIdx*16)` and `gifTagIdx` to a host
     buffer (FE frames reach ~137 KB; the buffer is 0x50000).
5. **Render-only pass** (new function in `front.cpp`, called instead of the body):
   `Rdr_StartFrame`, `RdrState_ApplyDefaultStateBlock`; `FeInterp::apply(t)`; save `m.time`, set it to
   `TimerGet(gameTimer)` (reading the timer only brings it up to date; calling it more often is the same arithmetic,
   `timer.cpp:241`); `Init3dFrame` .. `LsRenderWorld`, `TSORender(m.time)`; `VfxRenderShadows` and `CarouselDraw`
   with `carousel().lastUpdated` saved and restored (podium: `VfxRenderShadows`, `ObjectRenderAll`);
   `RdrAlphaAddToDmaChain`, `HudDrawLockedTracks`; the draw part of `VfxRender` (batch 2; batch 1 may call
   `VfxRender` minus `Vfx2PodiumPlay` and accept per-pass glints/rain if there are none in the front end, see 2.4);
   then **replace K1**: `buffIdx = 0`, copy the kept bytes into the current `pDmaBuff`, set `buffIdx` and `gifTagIdx`
   (anything the redrawn 3D functions wrote to K1 is discarded, so it does not matter which of them write 2D);
   `ObjectResetAllTint` is not needed; `Rdr_EndFrame`; `FeInterp::restore()`; `m.time` back. No `InputSnapshot`,
   `CameraProcess`, `TaskProcess`, `poll`, `AnimUpdateAnims`, `GfxTex_Update`, sound, music, menu, demo or credits
   code, no `frameCnt++`, no `m.frameCount++`.
6. **`Rdr_EndFrame`'s `GfxTex_Update`** (`rdr.cpp:243`): on a render-only pass it must not advance the animation:
   save `G(framecount)` before and restore after (the upload of the same frame is kept, in case the texture area was
   reused). A `rc::frontinterp::renderOnly()` flag read there.
7. **Input clock** (`overrides/rc_revenge_pro_overrides.cpp:546`, `g_inputClock.fetch_add` in the `GfxP2_SwapBuffers`
   override): skip the increment on render-only passes, so `PS2X_PAD_RECORD`/`PS2X_PAD_REPLAY` files keep counting
   logic frames in the menus (existing recordings such as `decomp/recordings/playtest2.pad` and `tools/front_walk.py`
   walks replay the same at 144 as at 60). `g_gameFrames` and the frame record keep counting every picture.
8. **Camera blend**: camera 0 only (no split screen in the front end); the same fields as `RcInterpBeginCamera`
   (`gametask.cpp:449`): `pos` (+0xc, 3 ints), `matrix` (+0x24, 9 shorts), its translation (+0x38, 3 ints); maybe the
   zoom at +0x574 if it changes. A cut (`SetCameraToThisTag`, a new path from another place) is a jump between two
   ticks: not blended above `RC_FE_INTERP_JUMP` (default from the largest normal step logged; fly-bys move far more per
   tick than a race camera, so do not reuse the race's 256 units blindly). Restore exactly after drawing.
9. **Off switches and diagnostics**: off when Smooth motion is off, the race rate is not above the video rate,
   `RC_FE_INTERP=0`, or the frame record / GS survey is active (`RC_FRAME_RECORD`, `ps2x::gssurvey::enabled()`: their
   cycles assume every picture has new K1 tags). `RC_FE_INTERP_LOG=1`: once a second passes, ticks, render-only passes,
   largest camera and TSO steps, blends skipped as jumps, K1 bytes; `=2`: the drawn camera per pass (as
   `RC_INTERP_LOG=2`) to check even motion; `RC_FE_TRACE=<file>`: one line per tick with the input clock, active menu
   tag, selection, camera path node/interp, carousel segment/angle, `uiDemoLoopWait`, music volume (for the
   equivalence test below).

### Batch 2: the scenery, the turntable, the podium and effects drawn blended

1. **TSOs** (the level's scenery objects: `tsoVarG` header, count at +0, entries from +4, stride 0x3c; `_sTSO` = tag,
   two vis words, then `sTOBInst`: `pos` at TSO+0x10 (vec32, 1/256 units), `angles` +0x1c, `flags` +0x22, `matrix`
   +0x24 (`mat3n`, 18 bytes), `animData` +0x36).
   - On the tick, before capturing: build the matrices `TSORender` would build (its first block, `tso.cpp:388-406`:
     flag 2 set and not vertex/boned -> `AngleAxisToMatrix` with flag 4 or `Mat3nBuild` with flag 8, then clear flag
     2). Doing it before the draw instead of inside it changes nothing visible, and gives a "cur" matrix to blend to.
   - Capture pos + matrix of each TSO; blend those that changed (pos with `blendInts`, matrix with
     `blendShortMatrix`), skip jumps (a linear move restarting at its start, `anim.cpp` flag 0x80 path), restore pos and
     matrix after drawing (never the flags: the draw sets 0x10).
   - The scenery's 2-or-1 steps per tick (60 + 50 a second, 1.1) remain as uneven speed; the original has it too.
2. **The turntable's cars**: on the tick, restore the real camera just before `CarouselRender` (it decides modes from
   the camera's distance: logic must see the real camera), call it, capture each car's drawn fields, then re-apply the
   camera and the car blend before `VfxRenderShadows`/`CarouselDraw`. Fields: the render object's `pos` (+0x4, 3
   ints) and `matrix` (+0x18, 9 shorts); the physics block's render position and position (+0x8, +0x14 floats), float
   matrix (+0x4c); the integer copy's position (+4, +0x10) and matrix (+0x28) as in `kCarFields` (`gametask.cpp:196`).
   Blending positions on the chord of the turntable is enough (about 1.5 degrees a tick). Restore after drawing: the
   physics task reads these blocks at the next tick. The hop's height comes from the 20 ms physics through
   `CarouselRender` (renderY), so it moves at tick steps (as the original).
3. **Podium cars**: around `VfxRenderShadows` + `ObjectRenderAll` (front.cpp:275-277) call the race's
   `RcInterpBeginDraw(drawTime)` / `RcInterpEndDraw()` (`gametask.cpp:477/537`): `GameTaskProcess` already runs
   `interp::stepBegin/stepEnd` in the front end, and the blend is by physics-step time, so it works at any pass rate.
   Do not call `RcInterpBeginCamera` (the front-end camera is not stepped by the physics). `Vfx2PodiumPlay`'s release
   of a car writes its physics block: it must run outside any blend (see 4).
4. **`VfxRender` split**: a native "draw part" in `rc_front_interp.cpp` that does what `VfxRender` (`visfx.cpp:997`)
   does minus `Vfx2PodiumPlay`. Tick pass: `VfxSetupCameraMatrix`, `RdrZedsWriteOff`, scratch sprite reset,
   `Vfx2PodiumPlay` unblended, then apply the blend, the rest, restore. Render-only pass: the draw part only.
   `Vfx3Glint` and `Vfx3Rain` age and spawn per call with `rand()`: on render-only passes either skip them when the
   front end has none (log the live glint count; rain returns at once without rain areas, `visfx12.cpp:1313`), or call
   them inside a rollback of their state (glint table `visFxVarG+0xeb0`, 128 x 0x30; last spot `+0x26f0`; the rain
   parts at `+0x1c0`; and newlib's `rand` state behind `_impure_ptr`, 0x3213f4 USA). Check `VfxSkyChangeStuff`'s
   podium branch for state it steps before relying on it as draw-only.
5. **Tints**: in podium mode `ObjectRenderAll` steps tints per call (`object.cpp:555`): on render-only passes save and
   restore each object's tint words (+0x4..+0xc) around it.

### Batch 3: tests, Europe, VRR, documentation

How to test (the game is driven with the existing aids; `RC_SKIP_INTRO=1` starts in the front end):

- **Equivalence of logic**: `tools/front_walk.py <seed> 300 6000 > walk.pad`, replayed with `PS2X_PAD_REPLAY` and
  `RC_FE_TRACE` twice: race rate 60 (feature off) and 144 (on). The menu sequence (active menu tag, selection,
  `ParseSelectionTags` results) per input-clock tick must be identical; time-based fields (camera interp, carousel
  angle) agree within a tick. Same with `decomp/recordings/playtest2.pad` (the run must reach the same race).
- **Timing** at 60 against 144 (and 50 against 144 in Europe), live input through `PS2X_PAD_LIVE`: a fly-by's length
  (from `CameraSetNewPath` to the end callback), the carousel turning from car 1 to car 10, the banner slide, a music
  volume slider held right for 2 s (same end value), the exit music fade, credits scroll speed, the attract demo
  starting ~30 s after the last key, the podium countdown to the menus (~4000 ticks).
- **Smoothness**: `RC_FE_INTERP_LOG=2` during a fly-by at 144: the drawn camera moves an even amount per pass, no
  repeats (the race check measured 11.3 units +- 1.1); window captures of consecutive pictures during a fly-by and the
  turntable; `PS2X_PRESENT_LOG`: in the menus ~140 game fields/s at 144 (now 59-60), ~141 with VRR on a 144 Hz screen.
- **Settings**: Smooth motion off or race rate 60 -> the front end exactly as now (video rate, no render-only passes);
  changing either on the Enhancements page applies at once; V-Sync off/on; VRR on (KDE "Always").
- **Europe**: the language menu first (`activeLanguage == -1`), 50 Hz ticks blended to 60/144.
- **Screens inside the front end**: memory card messages and load/save menus (dropped to the video rate), Progress and
  Time Trials pages (scrolling), credits, the stats box under the car menu, cheats, the podium after a cup race and
  after a single race, the enhancements and controls pages (`RC_MENU_DUMP=1` to list them), split-screen car choice.
- `tools/front_live.sh` takes screenshots at fixed fields; with render-only passes, key them on the input clock.
- Documentation: README's Race frame rate row ("menus keep the original rate" becomes "menus too, with Smooth motion"),
  PERF_INVESTIGATION "As built" section with the numbers, AGENT_USAGE row.

### Batch 4 (optional): the remaining 60 Hz movers

Effect particles (butterflies `VfxAddNewPlace(.., 0x29)`, car-viewer sparks and skids, fireworks) move in the 1 ms
`VfxUpdate` task, i.e. once per tick: blend their positions like the TSOs (prev/cur per effect object, keyed by its
address, dropped when freed). The banner (x/y per tick) could be drawn blended by leaving it out of the kept K1 and
drawing it on each pass (it is native, `front_menu.cpp:906`), but it slides for a quarter second only.

## 4. What could break, and how the design avoids it

| risk | handling |
|---|---|
| Logic sees a blended state (the carousel's camera test, `DisplayVehicleStats`, the podium release, the physics at the next tick) | blend windows only around draw calls; restore before `CarouselRender`, before the menu section, around `Vfx2PodiumPlay`, and at the end of every pass |
| A per-frame step called on a render-only pass speeds something up | render-only passes call only draw functions; the two hidden steppers in them are rolled back (`GfxTex_Update` in `Rdr_EndFrame`, tint stepping) or skipped (`Vfx2PodiumPlay`, glints/rain) |
| `CarouselDraw` resets `lastUpdated`, slowing the turntable | saved and restored on render-only passes |
| vblank-counted loops inside the front end run fast (credits, exit fade, card waits) | the rate drops to the video mode's for them |
| K1 replay shows the wrong thing | K1 is self-contained (`gLastTex` reset at each send, GIF tags closed, contiguous normal-mode DMA); 2D is the tick's for up to one tick. Check no 2D effect reads the frame buffer by parity (none found) |
| Pad recordings drift | the input clock counts ticks only |
| `gRdr.frameNumber` parity effects (`gEnvRamp`) cycle faster | cosmetic; the race at 144 has the same |
| Fly-by camera cuts drawn as a sweep | jump limit from logged step sizes, plus the cut happening between ticks |
| Front end too heavy for 144 | passes simply come less often; ticks stay at 60 (logic never slower unless passes drop below 60, as on the original) |
| Oracle / fuzz | no registered function's behaviour changes: the new code is in `FrontEndProcess` (blocking, whole-game checked), `Rdr_EndFrame`'s flag test and the override; `CarouselRender`, `TSORender`, `VfxRender` are called as they are |

## 5. Why not simpler variants

- **Just raise the vblank in the front end and gate only the camera and `AnimUpdateAnims`**: the menus, slider
  repeat, banner, animated textures, effect task and fades would still run 2.4x fast (1.2).
- **Present the old picture more often** (the current behaviour at 144): smooth pacing is impossible without new
  pictures; this is what the user sees as "original frame rate".
- **Re-run the whole body with a RAM snapshot rollback**: sound, DMA and GS side effects cannot be rolled back.

## 6. Side findings (races at 144 Hz, for a later look)

Read from the code, not measured. The race loop (`MainGameProcess`, `main.cpp:474-597`) runs every pass at the race
rate, so its own per-frame steps run faster than on the original there too:

- `Rdr_EndFrame` calls `GfxTex_Update` each pass (`rdr.cpp:243`): **animated textures in races animate at 2.4x at
  144 Hz** (framecount / 3).
- The pause menu and results/ranking menus run `AMenuProcessAll` each pass: sliders in the pause menu (music volume)
  repeat 2.4x as fast.
- `RCMusicFadeControl`, `SfxDoSfx`, `RevSfxPauseFade`, `Vfx3Rain`/`Vfx3Glint`, object tint stepping, `uiVSyncInc`
  (HUD flash phase) per pass.

The same tick clock could gate these in the race loop (logic on 60 Hz ticks, the physics already on its 20 ms task);
`GfxTex_Update` alone is a one-line fix (advance `framecount` by elapsed video-rate ticks).

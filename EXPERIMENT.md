# exp/quick-dirty: what the port can do with accuracy set aside

A throw-away experiment branch (`exp/quick-dirty`, worktree `fork-exp`, from main 8b6d6dd). It tries widescreen,
interpolated high frame rates and raw speed on the game as it runs today: recompiled and native code on the PS2Recomp
runtime, drawn by the emulated GS's hardware renderer (`PS2X_GS_HW=1`, what `tools/play.sh` uses). It does not touch
the native Vulkan renderer work.

**None of this is behaviour-matching.** Every switch is off by default. With none set, the branch behaves as main (each
hook is one test of a false flag). With a switch on, the native functions it changes no longer match their recompiled
originals, so `RC_SHADOW` and the oracle will report differences. Never merge this into main as it is.

## Building and running

The generated inputs come from the main checkout through links (read only): `game`, `output`, `output_eu`, `vu1`,
`vu1_images` and the files of `decomp/generated` (the links are in `.gitignore`). The build folder is `build-exp`:

    cmake -S . -B build-exp -G Ninja -DRC_VERSION=usa -DRC_FETCH_CACHE=/home/chazable/PS2Recomp/out/build/_deps \
        -DFETCHCONTENT_SOURCE_DIR_VOLK=../fork/build-decomp/_deps/volk-src \
        -DFETCHCONTENT_SOURCE_DIR_VULKAN_HEADERS=../fork/build-decomp/_deps/vulkan_headers-src
    nice -n 19 ninja -C build-exp -j4 rc_revenge_pro

`tools/exp_play.sh` is `tools/play.sh` for `build-exp`: the same defaults, every switch passed through from the
environment (`EXP_BINARY=<path>` runs another build, such as main's, in the same sandbox). With `EXP_TAG=<name>` it also writes `build-exp/runs/<name>.frames.csv` (`PS2X_FRAME_TIMES`, with image
hashes), `<name>.dump` (`RC_DEBUG_DUMP` each second: game ticks) and `<name>.log`; `tools/exp_fps.py
build-exp/runs/<name>` prints game ticks per second, new pictures per second and the spread of the intervals between
pictures.

## Switches

| Switch | Effect |
|---|---|
| `RC_WIDESCREEN=1` | Hor+ widescreen: the projection's x scale times (4/3)/aspect, the culling planes widened to match, the picture presented at the aspect, the window opened at 1280x720 |
| `RC_ASPECT=<w:h>` | the aspect for `RC_WIDESCREEN` (default 16:9; 21:9 and the like work the same way) |
| `RC_HUD_ANCHOR=1` | with `RC_WIDESCREEN`: the race HUD drawn at the right aspect, its corners pinned to the screen's corners and its middle to the centre (otherwise the HUD is stretched) |
| `RC_INTERP=1` | interpolated in-between pictures in races: camera and cars blended between the last two game ticks |
| `RC_FPS=<n>` | pictures per second for `RC_INTERP` (default 120): n / field rate, rounded, 2 to 8 pictures per tick |
| `RC_INTERP_PACE=0` | do not space the in-between pictures over the tick (each is shown as soon as it is drawn) |
| `RC_INTERP_FORCE=1` | keep drawing in-between pictures when a tick misses its vertical blank (normally they stop, see below) |
| `RC_INTERP_OBJECTS=0` | blend the camera only |
| `RC_EXP_LOG=1` | a line of interpolation counters each second in the log (`[exp] ticks=... inbetween=... missed=... skipped=...`) |
| `PS2X_HOST_VSYNC=1` | the window loop waits for the monitor's vertical blank instead of a 60 Hz timer (uncapped unless `PS2X_HOST_FPS`) |
| `PS2X_HOST_FPS=<n>` | the window loop's rate (default 60; 0 uncapped). `RC_INTERP` needs 120 or more, or `PS2X_HOST_VSYNC=1` on a fast monitor |
| `PS2X_WINDOW_SIZE=<w>x<h>` | the window's first size |
| `PS2X_GS_FORCE_LINEAR=1` | bilinear filtering of every textured triangle (the game asks for nearest on some); `2` also sprites |

Existing switches worth trying for speed (all already in the runtime): `RC_VU1_REF=0` (play.sh's default 2 checks each
VU1 program not yet compared in the game), `PS2X_GS_HW_SCALE=<n>` (internal resolution, default 2),
`PS2X_GS_FRAMES_IN_FLIGHT=<n>` (default 2), `RC_TRACK_SHADER` (already 1), `PS2X_PERF_OVERLAY=1` (F3: frame times on
screen).

## 1. Widescreen (`RC_WIDESCREEN`, `RC_HUD_ANCHOR`)

Hor+: the vertical view stays as on the PS2 and the horizontal view widens, as PC widescreen patches do. The game's
3D renderers all take their x projection scale from one of three places, each made from the field of view
(`lsVarG.fov`, 60) as `cot(half angle) * width / 2`:

- `Rdr_SetupCamera` (`decomp/src/rdr.cpp`): `dValX`, which the VU1 shape programs (world, track, cars, skins, water,
  environment map) receive in their packets (`shape.cpp` writes `camera.dValX` and `-camera.dValY`), and which
  `Rdr_DrawLine` and `line.cpp` use. `dValX *= hscale`; `dValY`, `dVal` and `normDVal` (distance culling) are kept.
  The left and right culling planes (`normRight`, `normLeft`) are turned through the widened half angle
  `atan(tan(half) / hscale)`. `misc.cpp`'s object culling (the one that uses `normLeft`) follows from that.
- `shape.cpp`'s second camera set-up (the one that also sets `vertDrawOffset`): `dValX *= hscale`, `dValY` from the
  unscaled value.
- `VfxSetupCameraMatrix` (`decomp/src/visfx6.cpp`): the effects' depth scale at `visFxVarG + 0x1b0` (x; `0x1b4`, y, kept),
  which `RdrPolyCamera` hands the polygon builder (shadows, glows, particles drawn as polygons).
- The PSX-style view cone (`View3dInitViewconeReject`, `decomp/src/display.cpp`): its x normal is made from the window
  width; the half width is divided by hscale so whatever culls with `view3dVarG.xNorm` widens too.

Presentation (`third_party/ps2recomp/ps2xRuntime`): `ps2_host_display::setAspect(aspect)` (already in the runtime,
never called before) letterboxes the 640-wide picture to the aspect, which stretches it horizontally;
`setInitialWindowSize` opens the window at 1280x720. The game still draws into its 640x448 frame, so at 16:9 the
horizontal resolution per screen pixel is lower than the vertical one: `PS2X_GS_HW_SCALE=3` makes up for it.

HUD (`RC_HUD_ANCHOR`): `rc::exp::HudAnchorScope` (`decomp/rc_exp.cpp`) wraps each HUD element that already carries a
side tag with an anchor (`hud.cpp`: `HudTopRight`, `HudBottomRight`, `HudBottomLeft`, `HudMiddle`; `HudPanel` is
"stretch", so untouched). When the element returns, it walks the GIF tags it appended to the 2D packet
(`gGfxP2.pDmaBuff`, from the write position at entry, including an open tag it continued) and scales every vertex's x
(PACKED XYZ2/XYZF2/XYZ3/XYZF3, and the same through A+D) by hscale about the left edge, the right edge or the centre
of the screen. Off in split screen.

Known or expected artefacts (to check in the test runs):
- 2D drawn outside those HUD elements stays stretched: the front end, menus, the pause menu, results, "DEMO",
  loading screens, the videos (the videos are 4:3 pictures, so they are stretched: pillarboxing them would need the
  presentation to know when a video plays).
- Anything else that projects with its own constant (not one of the three scales above) will sit at the wrong x:
  look for sprites, lens flares, light glows, skid marks or shadows that slide sideways against the world when the
  camera turns. Each is a one-line fix once found (multiply its x scale by `rc::exp::hscale()`).
- The VU1 programs clip against the guard band on projected coordinates, so nothing there should change; the
  scissor is the whole 640 frame.
- Objects or track sections popping in at the screen edges would mean a culling path not covered above. How the
  world itself is culled was not traced (the VU1 track program and the level's own visibility data were not read for
  it): watch the edges on every track.

## 2. Interpolated frame rate (`RC_INTERP`, `RC_FPS`)

### The seam

The race loop is `MainGameProcess` (`decomp/src/main.cpp`), native. One iteration is one game tick and ends in
`Rdr_EndFrame`, whose `GfxP2_SwapBuffers` waits for the vertical blank (`sceGsSyncV`), so game speed is tied to the
field rate (59.94 or 50 Hz). In order:

1. `Rdr_StartFrame`, the default render state, `InputSnapshot`, `TaskProcess(time)` (all game logic: physics, AI,
   cameras), `SetFrameWindow(0)`, `TBoxProcess`, `RdrParticleProcess`, `MeshInitFrame`, `Init3dChangeViewcone`,
   `LsFrameInit` (the renderer's camera from the view), `AIDoRaceOrder`;
2. the drawing: `SetFrameWindow(0)` again (`Init3dFrame`: the view matrices from `camera.matrix`, the 3D heap reset),
   `LsRenderWorld`, shadows, `ObjectRenderAll`, `VertAnimRender`, `TSORender`, pickups, `HudPanel`, weapons, the alpha
   list, `VfxRender`;
3. sound, the pause menu, the frame counter, `Rdr_EndFrame` (the 2D packet sent, the vertical blank, the flip, the 3D
   chain kicked).

The seam is between 1 and 2: after `AIDoRaceOrder` the tick's state is final and nothing in 2 is logic (with the
exceptions below). The renderer reads the state it draws from a few places, all copied out of the logic's results:

- the camera: `sCamera.matrix` (a `matPsx`: 4.12 rotation and translation, `camera + 0x24`) and `sCamera.pos`
  (`+0xc`), which `Init3dFrame`, `LsFrameInit` and `VfxSetupCameraMatrix` turn into every view the renderers use;
- each car (every object has physics): the integer state `pPhysics->pInteger` (`renderPos`, `mat`, `frontWheelMat`),
  which `ObjectRender` copies into the render object, and the float physics `renderPos`, `mat` (headlights) and
  `wheel[i].pos` (the wheels' positions).

### What the prototype does (`decomp/rc_exp.cpp`, `rc::exp::interp`; `main.cpp`)

At the seam, `capture()` keeps the last tick's and this tick's copies of those fields. Then, for k = 1 .. n-1
(n = pictures per tick): `apply(k/n)` writes the blend into the game's own fields (integer lerp of the 4.12 matrices
and positions, float lerp of the floats; no blend across a cut, a jump of more than 1024 units or a turn of more than
about 45 degrees), the frame is set up again and part 2 runs as is, `restore()` writes this tick's values back,
`paceWait(k)` sleeps until k/n of the field has passed since the last vertical blank, and `endFrameNoVSync()` ends
the frame as `Rdr_EndFrame` does but without `sceGsSyncV`, without counting a field (`uiVSyncInc`) and without
`GfxTex_Update` (animated textures stay at the tick rate). Then the frame starts again (`Rdr_StartFrame`) and the
tick's own picture is drawn as before. What the tick wrote into the 2D packet and the DMA chain before the seam
(logic-time drawing) is saved at the seam and written again at the start of each later frame (`save2D`,
`replay2D`), so it appears in every picture.

The runtime presented a new picture only at a new vertical blank. `ps2_host_display::setPresentOnFlip` (on with
`RC_INTERP`) and `notifyFlip()` (called after each flip, in-between or real, `gfx_p2.cpp` and `endFrameNoVSync`) make
`UploadFrame` latch the displayed buffer at each flip as well. The window loop must run at least as fast as the
pictures: `PS2X_HOST_FPS=120` (or 240) or `PS2X_HOST_VSYNC=1` on a 120 Hz+ monitor.

Guard: if a tick misses its vertical blank while in-between pictures are on (the work no longer fits in a field), the
game would slow down by half, so they stop for 2 s, then 4, 8 ... (up to ~64 s) before trying again
(`RC_INTERP_FORCE=1` turns this off). Not in split screen, not while the pause menu is up.

### What it costs and what will break

- Part 2 runs n times per tick: the CPU, VU1 and GS work of drawing doubles at 120 fps. Heavy levels (18, 17, 21)
  already run under 60 at 2x, so they will not get in-between pictures at all; the light ones and the menus' 3D are
  where this can work. Raw speed (section 3) decides how far this goes.
- Not interpolated, so moving at the tick rate inside smooth camera motion: pickups, weapons and missiles, particles
  and sparks, skid marks, water, the boats' wakes, animated scenery (`VertAnimRender`, `TSORender`), AI-only objects
  that are not in the object list, the wheels' spin, and the skinned characters (`skinanim`).
- Drawing that changes state runs n times per tick: `ObjectInterpolateTint` (tint fades go n times faster),
  `FPhysLibUpdateCarSpheres` and `GameTaskShadowCalc` in `ObjectRender` (recomputed from the blended car, then again
  from the real one, so the end state is right), and any effect that spawns or ages particles in its render function
  (`VfxRender`: expect denser smoke, sparks or exhaust).
- The HUD is drawn n times with the same values (fine); its flashing uses `uiVSyncInc`, which the in-between frames
  do not count.
- The 3D chain lags the 2D packet by one flip (the game kicks a frame's 3D after its flip), as on main, so now by
  half a tick.
- The in-between flips are paced by sleeping on the game thread (up to half a field) in 0.5 ms steps with a
  scheduling point (`guest_checkpoint`) between them, so interrupts and other EE threads still run, a little late.
- The window loop latches what is displayed when it runs: without vsync on the window it tears, and at 120 Hz on a
  60 Hz display every other picture is never seen.

## 3. Raw speed

Accuracy-only work in a normal `tools/play.sh` run: `RC_VU1_REF=2` (each VU1 program not yet compared is checked as it
runs), `PS2X_GS_SHADER_PRELOAD=0` (shaders compile when first needed: stutters), the frame-record and survey hooks
(null tests only), `RC_SHADOW` (off). `PS2X_VU1_TIMING=0` already drops the exact VU1 timing. Things to measure:

- `RC_VU1_REF=0`.
- `PS2X_GS_HW_SCALE=1, 2, 3, 4` (2 is the default): where the GS thread stops being the limit.
- `PS2X_GS_FRAMES_IN_FLIGHT=3` or 4.
- `PS2X_GS_FORCE_LINEAR=1` (looks, not speed; check it costs nothing).
- Build: `-DRC_GAME_OPT=-O2` with `-march=native` (main found no gain from -O2 alone), and a profile-guided build
  trained on the hardware renderer (`-DRC_PGO=generate`, a scripted race, `-DRC_PGO=use`; main's profile was trained on
  the software renderer). Each is a second build folder (`build-exp-pgo`), about 15+ minutes at -j4.

## Files touched

- `decomp/rc_exp.h`, `decomp/rc_exp.cpp` (new): the switches, HUD anchoring, the interpolation.
- `decomp/src/rdr.cpp` (`Rdr_SetupCamera`), `shape.cpp` (the second camera set-up), `visfx6.cpp`
  (`VfxSetupCameraMatrix`), `display.cpp` (`View3dInitViewconeReject`): Hor+.
- `decomp/src/hud.cpp`: one `HudAnchorScope` beside each HUD side tag.
- `decomp/src/main.cpp` (`MainGameProcess`): the in-between pictures at the seam.
- `decomp/src/gfx_p2.cpp` (`GfxP2_SwapBuffers`): `notifyFlip()` with `RC_INTERP`.
- `third_party/ps2recomp/ps2xRuntime/include/runtime/ps2_display_options.h`, `src/lib/ps2_runtime.cpp`: initial window
  size, present on flip, `PS2X_HOST_VSYNC`, `PS2X_HOST_FPS`, `PS2X_WINDOW_SIZE`.
- `third_party/ps2recomp/ps2xRuntime/src/lib/gs/gs_frontend.cpp`: `PS2X_GS_FORCE_LINEAR`.
- `tools/exp_play.sh`, `tools/exp_fps.py` (new); `.gitignore` (the links).

## Test plan (when the GPU is free)

Every run: the attract-mode demo of one level, from a clean card, about 35 s, on the RTX 2060 (the scripts use PRIME
offload). `RC_BOOT_LEVEL=<n> RC_SKIP_INTRO=1`; the first 8 s (load and start) are left out of the numbers. Shader
cache: run each level once untimed first so compiles do not count (`build-exp/shader-cache`).

    run() { tag=$1; shift; env "$@" EXP_TAG=$tag RC_SKIP_INTRO=1 timeout 40 tools/exp_play.sh; \
            tools/exp_fps.py build-exp/runs/$tag --skip 8 --until 36; }

(The game is killed at 40 s, so the last buffered lines of the CSV may be lost: hence `--until 36`. The demo ends
by itself after 30 s of race; check in the log or a screenshot that the 8-36 s window is still racing, and shorten
it if not.)

1. **Baseline** (the branch with nothing set must match main): `run base18 RC_BOOT_LEVEL=18`, and 17, 21, plus a light
   level (1). Then the same with main's binary in this sandbox (read only, nothing written under `fork/`):
   `run main18 RC_BOOT_LEVEL=18 EXP_BINARY=../fork/build-decomp/rc_revenge_pro`. Also NOTES.md's numbers (level 18 at
   2x: 36-42).
2. **Speed**, per heavy level, one variable at a time from the baseline: `RC_VU1_REF=0`; `PS2X_GS_HW_SCALE=1`, `3`, `4`;
   `PS2X_GS_FRAMES_IN_FLIGHT=3`; all the winners together. Look at ticks/s mean and min. `PS2X_PERF_OVERLAY=1` for a
   look at where the frame goes.
3. **Widescreen**: `RC_WIDESCREEN=1` on levels 1, 3, 18, then `RC_WIDESCREEN=1 RC_HUD_ANCHOR=1`, then
   `RC_ASPECT=21:9`. Screenshots with `PS2X_SCREENSHOT_DIR=<dir> PS2X_SCREENSHOT_FIELDS=900,1500` (and the same fields
   without widescreen to compare). Look for: correct proportions (round wheels), objects popping at the left and right
   edges, sprites or glows sliding against the world, HUD corners at the screen corners and not stretched, the front
   end's 3D. Also a split-screen race by hand (`tools/exp_play.sh`, two pads or keyboard).
4. **Interpolation**, light level first (1, then 3), USA: `RC_INTERP=1 PS2X_HOST_FPS=240 RC_EXP_LOG=1`, then with
   `PS2X_HOST_VSYNC=1` instead of `PS2X_HOST_FPS` if the display runs above 60 Hz. Look at: ticks/s must stay ~60
   (if it halves, the guard failed); pictures/s ~120 and interval p50 ~8.3 ms with a tight p90; the `[exp]` counters
   (`missed` should not climb; `inbetween` ~= ticks). Then `RC_INTERP_PACE=0` (expect uneven intervals),
   `RC_INTERP_OBJECTS=0` (the player's car should judder against the camera: proves the car blending works),
   `RC_FPS=180`/`240` on the lightest level, and `RC_VERSION=eu` (50 Hz: 100 pictures per second; needs a
   `build-exp-eu`). Watch by eye for: the car steady in the chase camera, wheels attached to the car, headlights on
   the car, shadows under it, particles and pickups at 60 Hz, any flashing HUD or 2D (a sign the save/replay of the
   pre-seam 2D is wrong), black or torn pictures, the race start countdown, a respawn (should cut, not swoop), the
   pause menu (no in-between pictures, no flicker), the end of the race.
5. **Everything on**: the best speed settings + `RC_WIDESCREEN=1 RC_HUD_ANCHOR=1 RC_INTERP=1` on a light level and on
   level 18 (expect the guard to turn interpolation off on 18; the `[exp]` line says so).
6. A short driven race by hand with everything on (`tools/exp_play.sh` alone), to see the feel.

Results go in the "Results" section below.

## Results

(Not run yet: the GPU was busy with main's measurements when this was written.)

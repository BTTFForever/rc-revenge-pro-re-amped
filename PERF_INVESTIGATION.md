# Why the recomp is heavy: performance investigation (2026-11-22)

Machine: Ryzen 7 3750H (4 cores / 8 threads, acpi-cpufreq + schedutil, boost to ~3.8 GHz), RTX 2060 by PRIME offload.
Build: `build-decomp` as of 49126d0 (USA). Game code `-O1`, runtime `-O3 -flto`, `RC_PGO=off`. All runs use
`tools/play.sh` settings (`PS2X_GS_HW=1`, GS/VU1/GIF threads, `RC_TRACK_SHADER=1`) unless stated. Logs:
`build-decomp/perf-logs/` (not tracked).

Project goal as of this write-up: an enhanced recomp (high frame rate and resolution, new features). Exactness and
the oracle no longer limit fixes; gameplay must still play like the original.

## Tools added

- `tools/thread_cpu.py <pid> [s]`: each thread's share of a core, from /proc.
- `tools/perf_session.sh <out> <warm s> <measure s> [play.sh args]`: one live run. Writes per-thread CPU, a flat
  `perf` profile (`task-clock:u`; hardware counters are blocked, `perf_event_paranoid=2`), nvidia-smi samples, the
  RC_DEBUG_DUMP for `tools/fps.py` and session.log.
- `tools/gl_count_shim.c`: an LD_PRELOAD library that wraps the GS hardware renderer's GL calls through `dlsym` /
  `eglGetProcAddress`. It counts draws, vertices, barriers, read-backs, fence waits, texture parameter and uniform
  calls per second. It can also turn single calls into no-ops, to measure what each one costs (the picture breaks):
  `GLSHIM_NO_TEXPARAM`, `GLSHIM_NO_BARRIER`, `GLSHIM_NO_WAIT`, `GLSHIM_NO_READPIXELS`, `GLSHIM_SKIP_DRAWS_OVER/UNDER`,
  `GLSHIM_UNBIND_FEEDBACK`.
- Call graphs need `perf record --call-graph dwarf,8192 --user-callchains` (plain `-g` is refused here).

## Where the time goes today

Level 18 (`RC_BOOT_LEVEL=18`), `gs` renderer, 3x: **38-55 fps, median about 43**. Thread shares of one core:
GSThread 95-97%, VUThread 47%, GameThread (EE) 22%, GIFThread 15%, main 8%. GPU at 99-100% at 1,900 MHz.
At **1x it is the same picture**: GPU 95-99%, 44-59 fps. The GPU load does not scale with resolution.

GSThread at 3x (dwarf call graph, 1,454 samples):

| part | share of GSThread |
|---|---|
| issuing draws (`drawOne` from `flushPending`, mostly inside the NVIDIA driver) | ~55% |
| `Present`: frame read-back, its copies, the frames-in-flight fence wait | ~22% (copies ~6.5%, spin in the fence wait ~8%) |
| `PersistentRing::allocate` waiting for GPU fences (spin in `clock_gettime`) | ~11% |
| `Submit`, CLUT loads, texture lookups | ~12% |

About 20% of the GS thread is the NVIDIA driver busy-waiting for the GPU, so it is GPU-bound. The GL counts
(level 18, 3x) per second:

- **119,000 `glDrawArrays`** (≈2,700 a frame; 8 M vertices, ≈50 vertices a draw);
- **357,000 `glTexParameteri`** that pass the state cache (3 a draw);
- 356,000 uniforms;
- ~900 `glTextureBarrier`;
- 45 frame read-backs through pixel buffers (**250-440 MB/s**);
- 120-150 fence waits totalling 200-250 ms per second.

91% of the draws render to a target that is also bound to a texture unit (unit 4; a feedback loop).

## Ranked causes, with the measured gain of removing each

### 1. Changing texture parameters on every draw stalls the GPU (by far the largest)
`gs/gpu/gs_hw_backend.cpp:2453-2470` (drawOne) sets `GL_TEXTURE_MIN/MAG_FILTER` and `WRAP_S/T` with
`glTexParameteri` on the bound texture for every draw. It re-sets them to nearest for indexed textures, and sets them on
render targets sampled as textures.

Experiment: the same runs with `glTexParameteri` turned into a no-op (`GLSHIM_NO_TEXPARAM=1`):

| level 18 | fps (5 s samples) | GPU util | GSThread |
|---|---|---|---|
| 1x, as is | 44-59 | 95-99% | 94% |
| 1x, no tex params | **55-60, capped at 60** | **33%** | 67% |
| 3x, as is | 38-54 | 99.8% | 97% |
| 3x, no tex params | **53-60** | **41.5%** | 86% |

Other experiments ruled out the rest:
- skipping every draw brings the GPU to 21%;
- skipping only the large track draws changes nothing;
- skipping only the small ones (<3,001 vertices) brings it to 32%;
- unbinding the feedback-loop texture changes nothing.

So the per-draw texture state change costs about 5 µs of GPU time per draw (NVIDIA re-validates the texture header
while in-flight draws use it). The fix: **sampler objects** (one per filter × wrap combination, `glBindSampler` per
unit) and no `glTexParameteri` in the draw path. PCSX2 does the same.

- Expected gain: races to 60 at 3x with the GPU at ~40%.
- Effort: small (a few hours).
- What could break: filtering or wrapping on some surface. Test with a race at 1x and 3x, the front end and the Track
  Editor, with screenshots before and after (`PS2X_SCREENSHOT_DIR`); they should be identical.

### 2. Too many draws: 2,700 a frame on a race, against PCSX2's 170
Our `[gs-hw]` stats show draws ≈ batches. Batches break on:
- any byte change in the 184-byte `GSDrawState` (`sameState`, memcmp, `gs_hw_backend.cpp:1361`);
- every new CLUT (`LoadClut`, which flushes, :2867);
- every TEXFLUSH (`TextureFlush` flushes although the comment says textures don't need it, :2938);
- transfers.

In the profile, flushes come from `submit` (state change) about 28% and from `LoadClut` 26% of GSThread samples.
After cause 1 is fixed, the CPU cost of these draws in the driver is the GS thread's limit: 86% of a core at 3x
with the GPU idle half the time.

Fixes, in order:
1. Do not flush on TEXFLUSH.
2. Compare only the fields that change the draw (not the whole struct: per-primitive fields, texture size, unused
   context fields).
3. Keep palettes in an atlas or array indexed per vertex or draw, so a CLUT change does not need a new batch.
4. Collapse uniforms into one UBO per batch.
- Diagnostic step first: log, per flush, which fields of `GSDrawState` differed (a counter per field). That shows which
  of 2-3 matters.
- Expected gain: GS thread from ~86% to ~30-40% of a core.
- Effort: medium.
- What could break: draw order or blending if two "same" states aren't really the same. Test: the race, the front end
  (transparencies, fonts), and water and shadows on levels 3, 18 and 21.

### 3. Every frame is read back from the GPU and uploaded again
`gs_hw_backend.cpp:3256` `readBackFrame` (PBO, then `readbackLast.assign` and `pixels = readbackLast`: two copies).
Then `copyLatchedHostPresentationFrame` copies again, and raylib `UpdateTexture` (`ps2_runtime.cpp:626`) or vk_host
(`platform/vk_host.cpp:486`) uploads it back. At 3x that is 250-440 MB/s down and as much up, through PRIME.

- The direct-present path exists only for WGL (`gs_gl.cpp:1137`, `Active()` is false on Linux).
- `UploadFrame` (`ps2_runtime.cpp:596`) first waits for the VU1 and GIF threads to drain (`asyncvif::sync`). The
  latch (`gs_frontend.cpp:677`) then runs Flush + Sync + Present on the GS thread, up to 3 times.
- Under `RC_VK_PICTURE=native` all this still runs, although the picture shown is the native one.

The fix:
- present from a texture shared with the window's context: an EGL shared context for raylib, or a GL/Vulkan
  interop image for vk_host;
- drop the extra copies;
- under vk native, skip the latch and read-back unless F7 or a screenshot asks for it.

- Expected gain: ~1-2 ms a frame of GS thread, the PCIe traffic, and the frame overlap the per-frame drain prevents.
- Effort: small for the copies and the vk skip; medium for sharing.
- What could break: black or stale frames (the latch retry exists for a black-frame bug). Test: videos, the front
  end, and scene changes.

### 4. The track-batch leak: why software-GS mode grows to 12 GB
RSS grows about 500 MB/s from boot (VmRSS 1.0 GB at 2 s, 11.6 GB at 24 s). It is root-caused:
- With `RC_TRACK_SHADER=1`, `decomp/vu1/track_draw.cpp:53` `malloc`s a `GSTrackBatch` for every track, env, skin and
  water batch.
- It sends the pointer through a GS register write (`gs_frontend.cpp:1258`, `kGSTrackBatchRegister`). Only
  `GSHwBackend::Submit` (`gs_hw_backend.cpp:2843`) recognises the marker and frees it.
- The software backend (`PS2X_GS_HW=0`), `RC_GS_NO_RASTER` and `PS2X_GS_DISCARD_DRAWS=1` (the threaded backend drops
  the Submit, `gs_threaded_backend.cpp:241`) never free it, and the software backend does not draw the track at all.

Proof: the same software-GS run with `RC_TRACK_SHADER=0` stays at 490-520 MB for 60 s.

- Fix: free the batch wherever the marker is dropped (the threaded backend's discard, `GSCpuBackend::Submit`, the
  frontend when no backend accepts it), or don't emit batches unless the HW backend is active.
- Effort: trivial.
- Visible risk: none. This unblocks the "GS off" route.

### 5. The VU1 thread: 47-63% of a core on races
Folded stacks:
- `processVIF1Data` memcpy 16%;
- `copyGifPacket` (GIF packets copied out of VU memory) about 20%;
- the native water program (`runWaterShape`, `setupTriangle`) 15%;
- the recompiled programs (cycle-accurate per instruction: budget check, 5-8 ready-time reads and 4 writes per FMAC;
  `ps2_vu1_impl.inl:3189`) the rest.

Without the track shader it reaches 85-90%.

Fixes:
- (a) the timing-free "lean" generator for the programs still on VU1;
- (b) on route (b) below, do not run the render programs on the emulated VU1 at all: the reader already re-derives
  their output.

- Gain: up to the whole thread.
- What could break: anything that reads VU1 memory or flags back (it is believed nothing does). Test: races with
  water, skinned cars and the Track Editor.

### 6. The EE thread is not the bottleneck
GameThread is 22% of a core on level 18. Of that:
- 35% is `writeIORegister` copying the whole VIF1 DMA chain into a buffer for the VU thread
  (`ps2_memory.cpp:1599`, chain flattening, about 1.5 ms a frame);
- 20% is SPU2 mixing in the IOP (`IopSpu2::advanceTo`);
- 2% is the IOP interpreter;
- the recompiled and decompiled game code is the rest (about 8% of a core).

Per-instruction overheads in the generated code are real but cost little in total. They are listed for later:
- `dispatchGuestBranch` per call (`ps2_runtime.cpp:1591`);
- `eeCheckpointDue` per back edge;
- GPRs in memory, made worse by `uint8_t* rdram` aliasing;
- the RAM fast path of READ/WRITE laid out as the cold branch (no PGO).

PGO, `-O2`, `__restrict` and `__builtin_expect` would give maybe 10-20% of this thread, which is not where frames are
lost now.

### 7. Synchronisation that serialises threads
- `asyncvif::sync` (wait for VU1 and GIF drained) runs on every GS privileged register access and on every VIF/GIF
  register access (`ps2_memory.cpp:845, 895, 1055, 1125, 1266, 2596`) and on every present (`ps2_runtime.cpp:596`).
- `GSThreadedBackend::Enqueue` takes a mutex, `getcsr`, a 184-byte memcmp and two vector inserts per primitive. It
  measured 25-30 ns uncontended in a microbenchmark, so ~3 ms a frame at 100k primitives, on the GIF thread.
- Fix: answer status reads from published state without draining, and enqueue per packet.
- What could break: the game waiting on VIF/GS status at the wrong time (hangs, or flicker if a buffer is shown
  before it is drawn). Test: the front end, a race, the Track Editor, and videos.

### Smaller items
- `RC_VU1_REF=2` (the play.sh default) still checks TERdr on every run in the Track Editor (`vu1_check.cpp:440`),
  although it has passed 284k real runs.
- `PS2_RUNTIME_LOGS=1` and the RPC trace are compiled in (cheap).
- The texture cache re-decodes ~22 textures a frame on the front end (1,320 per second).

## As built (2026-11-23): causes 1, 3 and 4 fixed

Commits on `main`: 4529302 (sampler objects), 38af110 (the jungle smoke's subtractive blend, from exp/quick-dirty
928bf9a, blend part only), 63a6a24 (the track-batch leak), b514f1e (the present path), fcc190b (a batch-break
diagnostic). Measured with `tools/perf_scene.sh` (`PERF_RENDERER=gs` unless noted, `PERF_SECS=60`, the window after
15 s) and `tools/gl_count_shim.c` preloaded; "before" is 78f957e or the old path by its switch.

| run | fps median (worst 5 s) | GPU | GS thread | process | draws a frame | other |
|---|---|---|---|---|---|---|
| level 18, 3x, before | 44.6 (39.8) | 99% at 1905 MHz | 0.96 | 1.86 cores | ~2,700 | fence waits 207 ms/s, 360k glTexParameteri/s |
| level 18, 3x, samplers | 60.0 (53.2) | 39% | 0.81 | 1.94 | ~2,450 | fence waits 2 ms/s, ~250 glTexParameteri/s |
| level 18, 3x, all | 59.8 (54.8) | 38% | 0.75 | 1.85 | ~2,400 | RSS 724 MB |
| level 18, 1x, before (`PS2X_GS_HW_TEXPARAM=1`) | 53.8 (43.4) | 97% at 1905 MHz | 0.91 | 1.85 | ~2,600 | |
| level 18, 1x, all | 59.8 (55.6) | 34% at 870 MHz | 0.72 | 1.76 | ~2,400 | |
| front (`playtest2`), 3x, before | 60.0 (55.0) | 70% at 1905 MHz | 0.59 | 1.42 | ~450 | |
| front, 3x, all | 59.8 (56.0) | 35% at 975 MHz | 0.47 | 1.32 | ~450 | |
| Track Editor (`tedit`), 3x, all | 60.4 (58.8) | 32% at 780 MHz | 0.20 | 0.61 | ~140 | |
| vk native, level 18, 3x, read-back every frame (`RC_VK_GS_PICTURE=always`) | 58.8 (51.4) | 38% | 0.87 | 3.33 | | 64 read-backs/s, 355 MB/s |
| vk native, level 18, 3x, all | 58.2 (52.6) | 37% | 0.83 | 3.26 | | no read-back; the reader thread (15.4 ms a cycle) limits |

Draws a frame are the shim's draws a second over the frame rate (they include the presents' few draws).

**Cause 1, sampler objects** (`gs_hw_backend.cpp`, `drawSamplers`): the draw texture is bound on unit 6 with one of 8
samplers (filter x wrap S x wrap T), rebound only when the combination changes; the reinterpret / CLUT source on unit
1 has a fixed point sampler. The choice is the old one: linear only for non-indexed, non-depth sources (indexed
textures use `texelFetch`; depth is point-sampled), repeat for CLAMP mode 0, clamp-to-edge otherwise; region
clamp/repeat stay in the shader (`vUVClamp`, `indexedTexel`). `PS2X_GS_HW_TEXPARAM=1` is the old path.

**Smoke** (`PS2X_GS_HW_SUBBLEND=0` is the old shader blend): the USA jungle track select (a scripted walk: START, Race,
Single Race, the default car, right twice) shows smooth dark clouds where the blocks were.

**Cause 4, the leak**: `gsFreeTrackBatch` (`gs_track_batch.h`) in the software GS, the compute GS, the discard path and
the frontend without a backend. Level 18 with `PS2X_GS_HW=0 PS2X_GS_GPU=0`: VmRSS 293 MB at 5 s, 328 MB at 100 s
(steps at level streaming, flat between); `PS2X_GS_DISCARD_DRAWS=1`: 343 -> 351 MB over 40 s.

**Cause 3, the present path**:
- gs path: the mapped pixel buffer is copied once, straight into the frame (no zero fill, no kept copy), and the
  frontend swaps the latched buffer with the caller's instead of copying. Old: `PS2X_GS_HW_READBACK_COPY=1
  PS2X_PRESENT_COPY=1`. Level 18 at 3x: GS 0.86 -> 0.75, main 0.15 -> 0.11, process 2.12 -> 1.85 cores.
- Read-back slots older than 100 ms are dropped and that present reads synchronously, so a picture after skipped
  presents is never an old one.
- vk native: the optional host hook `wantsGsPicture` is false while the native picture is shown with no F7 press
  pending and no `RC_VK_SHOT` pair waiting; the host loop then skips the drain, the latch and the read-back (also taken
  for `PS2X_SCREENSHOT_FIELDS` and `PS2X_FRAME_HASH`). `RC_VK_GS_PICTURE=always` is the old path.
- The `asyncvif::sync` drain before the latch stays. `PS2X_PRESENT_DRAIN=0` (an experiment switch) gave nothing
  measurable on level 18 at 3x and the drain is the guard against black frames.
- Not done: presenting from a shared texture (EGL shared context or GL/Vulkan interop). The upload to the window
  (raylib `UpdateTexture`, 250-350 MB/s at 3x) and the PBO read-back remain on the gs path.

**Cause 2, draws**: `PS2X_GS_HW_BREAKS=1` prints why batches end. Level 18 at 3x: TEX0 to another texture 46-49%,
ALPHA with another blend 23-25%, CLUT load 22-26%, track batches that cannot join 4-7%; TEXFLUSH, transfers and syncs
0%, and no breaks on unused bytes. Jungle track select: CLUT load 93% of ~1,000 draws, all batches of indexed
textures that read the new palette (a flush that skipped batches not reading the palette was tried: no change, not
kept). So the TEXFLUSH and "irrelevant bytes" items gain nothing; fewer draws needs a palette atlas (indexed
textures look up a palette index per draw) and texture arrays or bindless textures. The per-primitive mutex
(`GSThreadedBackend::Enqueue`) was left: it is on the GIF thread (0.17-0.19 of a core), which limits nothing at 60 fps.

**Looked at for correctness** (screenshots, `PS2X_SCREENSHOT_DIR`/`PS2X_SCREENSHOT_FIELDS`): level 18's demo race at
1x and 3x (town, cave, sand, water), the intro (Acclaim video, Dolby) and front end (mean brightness of every 50th
field from 100 to 2900 equal old and new: no black or stale frames), the Track Editor walk (`tedit`), the jungle track
select before and after the blend, the software GS race (water only: the track is not drawn there, as before), and
GS pictures taken under vk native after long skipped stretches (current, not old). Not tested: F7 by hand (no input
injection here), Europe, two-player.

**Next**: route (b) below. On route (a) the GS thread is now 0.75 of a core at 60 fps with the GPU at ~38%; the next
gains are the palette atlas (CLUT breaks), a shared-texture present, and an uncapped frame rate to see the headroom.

## As built (2026-11-23): the AKLM Studios stutter (skinned trees, one triangle per texture block)

**The symptom.** On AKLM Studios' first track (level 4: world AKLM Studios = index 3, track 0, `WorldTrackToLevel`
= track*6 + 1 + world) the frame rate fell to 13-16 fps for 5-8 s at the same two spots every lap, all of it in
submit, with the GS thread at 96-99%. The attract demo on `RC_BOOT_LEVEL=4` passes the same spots (24-30, 55-61,
86-92 s from boot, then every 93 s).

**The cause.** `PS2X_GS_HW_BREAKS=1` there: 2,600 batches a present, 82% ended by a CLUT load and 17% by "track
batch not joinable". A draw dump showed one-triangle track batches, each with its own T4/T8 texture and palette, and
the batches were skin (track mode 3, `Rdr_VUSkinProg`). Dropping the skin batches for one run showed what they are:
the swaying cartoon trees and plants beside the track. Their models come as one triangle per texture block, so each
triangle was a CLUT load, a flush, a full draw (~9,500 a frame), and in `track_draw.cpp` a malloc and a 16 KB copy of
all of VU memory per block (about 150 MB a frame through the VU thread and the GL ring).

**The fix** (`gs_hw_backend.cpp`, `track_draw.cpp`, `trdr_shape.vert`, `gs_track_batch.h`):
- Multi-texture track batches. Consecutive track blocks (mode 0, and skin blocks of one VU1 run) whose GS state
  differs only in the paletted texture (TEX0, CLUT, TW/TH, wrap) join one draw. Each block's index texture and
  palette are resolved as it arrives and kept as a slot of `GL_ARB_bindless_texture` handles. Each triangle carries
  its slot, and for skin its first quadword, in a table the vertex shader reads. A CLUT load no longer flushes such a
  batch, unless the CLUT comes from a render target. The draw order is unchanged. Textures with handles are deleted
  4 presents later and are never re-specified.
- A skin run's first batch carries VU memory and the rest carry none (the loop changes only quadword 4, which the
  shader does not read). The backend keeps the run's copy.
- `PS2X_GS_HW_TRACK_MULTITEX=0` restores one texture per batch, as does the absence of the bindless entry points.
  `[gs-hw] track batches:` in session.log says which is in use.

| run (3x, `RC_VU1_REF=0`) | fps in the slow windows (worst second) | GS thread | VU thread | process | draws |
|---|---|---|---|---|---|
| level 4, before (e48d38e) | 24.2 (13) | 0.92 | 0.46 | 1.76 | up to 716k per 5 s, ~9,500 a frame |
| level 4, after | 60.0 (59) | 0.49 | 0.28 | 1.20 | up to 176k per 5 s, ~590 a frame |
| level 4 elsewhere, before / after | 60 / 60 | 0.38 / 0.34 | 0.18 / 0.17 | | |
| level 18, `MULTITEX=0` / after (5 s samples, 15-70 s) | 59.8 median (58.4) / 59.8 (58.6) | 0.71 / 0.63 | | 1.82 / 1.71 | 794k / 654k per 5 s |

All windows are the six of a 200 s run (two laps). The pictures at the spots (fields 1500-1700) and on level 18 look
the same as before.
Front end (`replay playtest2`, 3x): 60.0 fps median, worst 5 s 59.2; menus, cup confirmation and race HUD look right.
The jungle track select (scratchpad walk `jungle.pad`, field 1600) matches the subtractive-blend reference: smooth
smoke, and the dancing natives (skinned, so on the new path) are drawn correctly. Not tested: Europe, two-player, the
Track Editor.

## As built (2026-11-23): whole-lap track sweep, the water drawn three passes per triangle

**Finding the slow spots.** The attract demo ends after 30 s (`MainDemoEndTask`, `decomp/src/main.cpp`), so a
`RC_BOOT_LEVEL` run repeated each track's opening. `RC_DEMO_SECS=<n>` lengthens it; `tools/track_sweep.sh` runs every
level's demo for 130 s (a lap) at 3x with frame times, `[gs-hw]` stats and per-thread CPU every second, and
`tools/track_sweep_table.py` makes the table. The demo is all AI (no human player); it follows the racing line, so
shortcuts, crashes and split screen are not covered, and it is not deterministic between runs.

**First sweep** (USA, after the AKLM fix): levels 9 (32 s under 55 fps, worst 40), 21 (16 s, worst 47), 17 (9 s,
worst 48) and 23 (4 s, worst 53), each with the GS thread at 96-99% of a core in the dips. No frames over 50 ms.

**The cause.** `PS2X_GS_HW_BREAKS=2` (new: each reason split by what the ended draw was, with triangles a draw) on
level 9: 78% of draws were single triangles of plain primitives, in a repeating cycle of three states at the same
position: a CT32 texture with ALPHA 0x09 (Cs + Cd*As, As = 127), a T4 bump texture subtracted (0x62, FIX 0x15) and
added (0x68), Z test on and Z writes off. That is the water VU1 program (`Rdr_Water`, `gRdr_WaterEndShapeData`)
drawing every triangle in three passes: up to ~5,400 GL draws a frame.

**The fix** (`gs_hw_backend.cpp`, multipass lanes): when a one-triangle batch is followed by the same triangle (same
host positions) under another state, with no depth writes, the same FRAME/ZBUF and no texture read from a render
target, each state of the cycle (2-4) gets a lane; the following triangles fill the lanes in cycle order while the
positions repeat, and anything else (or any flush) draws the lanes in order, one draw per pass. This reorders the
blend only where two triangles of one group overlap: `PS2X_GS_HW_MULTIPASS=3` measures it, 0.04-0.05% of the pixels
the groups cover on level 9 (shared edges included, the test counts them twice), and there the base pass keeps
127/128 of what is below, so the bump term changes by under 1%. `PS2X_GS_HW_MULTIPASS=0` restores the old path.
The front end (`replay playtest2`) never forms a group.

| level (3x, 130 s demo, 15-140 s) | <55 fps seconds before / after | worst second before / after | GS thread mean / peak before | after |
|---|---|---|---|---|
| 9 | 32 / 0 | 40 / 56 | 0.75 / 0.99 | 0.50 / 0.75 |
| 21 | 16 / 0 | 47 / 59 | 0.70 / 0.98 | 0.48 / 0.70 |
| 17 | 9 / 0 | 48 / 59 | 0.63 / 0.98 | 0.39 / 0.52 |
| 23 | 4 / 0 | 53 / 57 | 0.68 / 0.98 | 0.43 / 0.55 |

All 24 levels after: median 60, no second under 55, worst seconds 56-60. Level 9's GL draws: 3,400-5,400 a frame to
660-800 (the table's "maxPrim/f" counts GS primitives, which grouping does not change). What still splits level 9's
draws: skinned blocks with direct-colour textures (track mode 3, 4 triangles a draw) and blocks of track modes 1 and
2, not yet on the multi-texture path. Not tested: Europe (built only), two-player, the Track Editor.

## As built (2026-11-23): 4x judder, shared-texture presentation on Linux

**Symptom.** At 4x (2560x2048) the user saw smooth play but "around 45-50 fps", with loud fans. A logged session:
races average 57-59 fps but ~4% of frames take over 20 ms (each shows the previous picture for an extra refresh);
GPU 40-50% busy at 83-87 C. A 4x sweep (`tools/track_sweep.sh`, `PS2X_GS_HW_SCALE=4`): levels 9, 20, 21 had 12-21%
late frames, 5, 6, 8, 11, 12, 17, 18 had 1.5-5%; at 3x the same levels had 0.0-0.3%. Every late frame spent ~23 ms in
the present's submit step, against ~7 ms normally.

**Cause.** On Linux the GS context (EGL, no surface) shared nothing with the window's (GLFW on X11: GLX), so each
picture was read back to RAM (`readBackFrame`, a PBO) and uploaded again (raylib `UpdateTexture`): 21 MB a frame at
4x, ~1.3 GB/s each way. When the previous asynchronous read-back's fence had not signalled, `readBackFrame` fell back
to a synchronous `glReadPixels` of the whole picture. The shared path (`GSSharedPresent`, slot textures behind fences)
existed for Windows only.

**Fix** (`gs_gl.cpp`, `gs_gl.h`, `gs_gl_api_load.inl`, `gs_hw_backend.cpp`): the slot machinery is common to both
platforms; on Linux `CaptureHostContext` takes the window's GLX context and `Context::Create` makes the GS context with
GLX sharing it (its own X connection and pbuffer). The HW renderer's `Present` composes as before, then copies the
picture into a free slot with `glCopyImageSubData` (`GSGL::PublishSharedTexture`); pictures written straight into memory
(movies) go through a texture first. When the window holds every slot, the picture is dropped. On by default on Linux;
`PS2X_GS_DIRECT_PRESENT=0` restores the read-back. The session log says `[gs-present] shared GPU textures active`.

| level, 4x (130 s demo, 15-140 s) | late frames (>20 ms) before / after | 99th percentile frame | worst second |
|---|---|---|---|
| 9 | 21.2% / 0.0% | 29.7 / 16.7 ms | 44 / 59 fps |
| 20 | 12.5% / 0.0% | 27.4 / 16.7 ms | 42 / 59 fps |
| 21 | 12.5% / 0.0% | 28.6 / 16.7 ms | 41 / 59 fps |
| 5, 6, 8, 11, 12, 17, 18 | 1.5-5.2% / 0.0% | 20.9-23.9 / 16.7 ms | 47-54 / 59 fps |

GPU over those runs: 44% busy, 67 W, 83 C before (all 24 levels); 35%, 31 W, 59 C after (the ten levels). Front end and
a race (`replay playtest2`, 4x): 0.02% late. Checked in window captures (`spectacle -b -n -a`): the intro movie, a 4x
race and the HUD, upright and with the right colours; the native Vulkan route (`RC_RENDERER=vk`) has its own window
and is unchanged. Not tested: Europe (built), two-player, the Track Editor, resizing or fullscreen.

## As built (2026-11-23): high refresh rates and drawing between physics steps

**How the game keeps time.** The race clock is EE timer 0 (`TimerGet`, milliseconds), not a vblank count. The physics
is a task that reschedules itself every `gameTaskTimeStep` = 20 ms (50 Hz) and integrates with a fixed step
(`kTimestep` 0.59375); `TaskProcess` catches up with as many steps as are due. The camera is updated inside the step
(`CameraProcess` in `GameTaskProcess`). The race loop draws once a loop with the latest step and waits for a vblank,
so at 60 fps one picture in five repeats the cars and camera (a judder of the original), and changing the step would
change how the game plays.

**`PS2X_REFRESH_HZ=<n>`** (opt-in): the emulated vblank runs at n Hz (`Syscalls/System.cpp`) and the window presents up
to n frames a second. Game speed is unchanged (the demo still ends after 30 s of real time). At 144 Hz, 3x: level 1
draws ~140 new pictures a second; level 9 (heavy water) ~100, the VU1 thread at 0.8 of a core being the limit.

**`RC_INTERP`** (on by default, `RC_INTERP=0` off; `gametask.cpp`, hooks in `MainGameProcess`): before each physics
step the cars' physics blocks (0x23c bytes) and both cameras' position and matrix are kept as the previous state.
After the frame's game logic, just before the render camera is built, each car's drawn fields (float position and
render position, body and front-wheel matrices, the four wheel positions, and the integer copy the renderer reads)
and each camera are blended between the previous and current state by how far the frame's time is into the step
(rows of the matrices rescaled to the current lengths); after drawing, the exact current state (and the collision
spheres, which a draw may recompute) is put back, so the simulation never sees a blend. A step longer than
`RC_INTERP_JUMP` (65536 physics units for cars, /256 for cameras: normal driving is up to ~14,000 and ~45) is drawn
without a blend (respawns, camera cuts). `RC_INTERP_LOG=1` logs the largest steps; `=2` logs the drawn camera per
frame. Level 1 at 144 Hz: the drawn camera moves an even 11.3 units a frame (standard deviation 1.1, no still frames)
where unblended it moves ~35 every third frame. Window captures of levels 18 and 9 at 144 Hz look right.
Not yet covered: projectiles and other objects outside the car list, split screen checked only by construction.

## The two renderer routes

**(a) Keep the emulated GS's GL renderer and speed it up PCSX2-style.**
- Causes 1-3 are cheap to fix, and cause 1 alone brings level 18 to ~60 fps at 3x with the GPU at ~40%.
- What remains is the GS thread at 86% of a core (draw count) and the VU1, GIF and GS emulation threads, three cores
  busy for a 2000 game.
- Every enhancement (resolution, widescreen, effects) stays bound to emulating GS memory, render-target aliasing and
  palette reads from render targets.

**(b) The native Vulkan renderer from the VU1 output, with the emulated GS stopped.** Measured today with
`RC_TRACK_SHADER=0 RC_GS_NO_RASTER=1 PS2X_GS_HW=0 PS2X_GS_GPU=0`, needed because of the leak:
- 28-37 fps on level 18, limited by the VU thread at 85% (the track back on VU1), plus the GS frontend thread 50%,
  GIF 29% and the reader 17.2 ms a cycle (53%);
- the GPU at 375 MHz and 23%, with the list's GPU time 4.8 ms a frame at that clock (1-2.5 ms at full clock, per
  batch 7).

With the leak fixed and `RC_TRACK_SHADER=1` the VU1 load drops back to ~50%. Then the emulated VU1 (render programs),
the GIF thread and the GS frontend have no consumer and can be stopped: about 1.5-2 cores freed. The reader (16-17 ms
a cycle on level 18, over the 12 ms target) then becomes the one heavy thread and the place to optimise.

**Recommendation.**
1. Do (a)'s fixes 1, 3 (the copies and the vk skip) and 4 now: one to two days for a playable 60 fps default at 3x.
2. Make (b) the target. It is the only route to new features and resolution without emulation cost, and its GPU cost
   is already about a tenth of (a)'s.
3. Don't invest in (a)'s draw batching (cause 2) beyond the TEXFLUSH change unless (b) stalls.

## PCSX2 comparison (2.8.2 AppImage, Vulkan, 3x, RTX 2060)

The PCSX2 log names the device: `[3.3711] NVIDIA GeForce RTX 2060` (under "Vulkan Graphics Driver Info"), and
`nvidia-smi` lists the PCSX2 process as C+G, about 257 MiB. Its OSD stats:

| scene | fps | draw calls a frame (DRWC) | primitives | CPU (threads, share of a core) | GPU |
|---|---|---|---|---|---|
| front end (main menu) | 59.9 | 780 | 6,922 | CPU Thread (EE+IOP) 50%, GS 32% | 0.87 ms a frame (OSD); nvidia-smi 27% at 572 MHz |
| attract demo race (a cave level) | 59.9 | 170 | 7,489 | CPU Thread 57%, GS 21% | 0.97 ms a frame; 28% at 716 MHz |
| ours, front end, `gs` 3x | 60.0 | ~470 | - | 137% (GS 49, VU 31, EE 27, GIF 16, main 13) | 55% at 1,900 MHz |
| ours, demo race (rain jungle, single screen), `gs` 3x | 53-54 | ~1,200 | - | 210% (GS 95, VU 63, GIF 22, EE 22) | 98.5% at 1,890 MHz |
| ours, level 18, `gs` 3x | 43 | ~2,700 | - | 191% | 99.8% at 1,905 MHz |

The two demos are different levels: the game picks one at random, so these rows don't compare one scene.

What explains the gap:
- PCSX2's GS issues 5-15 times fewer draws, with no per-draw texture state changes (samplers);
- its GPU idles at 600-700 MHz;
- microVU makes VU1 nearly free (its MTVU thread is under 0.5% of a core);
- its EE+IOP thread is about 50-57%, against our EE 22% + VU 47-63% + GIF 15-22% + GS 95%.

The emulated machine is not what is slow. Ours issues the GPU work badly (cause 1) and runs three threads of GS/VU
plumbing that PCSX2 folds into one recompiled CPU thread and one batched GS thread.

### Test control in PCSX2
What works and what doesn't:
- **Fast boot (`EnableFastBoot`) hangs** on the first "loading" screen after the logos (the game stops flipping;
  seen 3 times, with and without patches). A full BIOS boot works. The first BIOS boot stopped at the BIOS language
  screen with a new NVM, so `~/tools/pcsx2/home/PCSX2/bios/scph39001.nvm` is now a link to the user's
  `scph39001.NVM`. Its checksum was unchanged after every run.
- **Boot to level by pnach** (`~/tools/pcsx2/pnach-off/A8083AE6.pnach`: MainFrontProcess replaced with the same
  writes `RC_BOOT_LEVEL` makes, BackdropDoInitialScreens skipped) reaches the loading screen. Then PCSX2 aborts:
  `FiFo.cpp:32: assertion failed ... ReadFIFO_VIF1: FQC = 0 on VIF FIFO READ` (with fast boot it hung instead).
  - The cause is not settled. PCSX2 applies patches only once the game's ELF runs, so it is not "writing during the
    BIOS".
  - Our override does more than the patch (heap and stack layout, CD positions), so the level boot works in the
    recomp's machine and not in a real one.
  - Cheats are off again (`EnableCheats = false`).
- **The attract demo** plays after idling at the title, about 110-130 s after boot, on a random level.
- **Save states** were not tried: no input injection is available here (no xdotool/ydotool on Wayland), so a state
  can't be saved by hand. Once the user saves one, `-statefile` would give repeatable scenes.
- Measurement that works: the OSD (read from window screenshots, `spectacle -b -n -a`), nvidia-smi, and per-thread
  CPU from /proc. The threads are named `CPU Thread`, `GS`, `MTVU`. MangoHud logging to CSV did not write a file.
- Compared with the recomp: no frame-exact input replay, no level boot, and no per-scene counters beyond the OSD.

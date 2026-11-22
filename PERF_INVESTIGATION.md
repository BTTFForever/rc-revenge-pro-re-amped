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

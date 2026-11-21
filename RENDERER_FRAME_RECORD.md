# Frame record, dump format, replay tool and side tags (renderer plan P1 step 4: design)

Designed 2026-11-19 for `RENDERER_PLAN.md` step 4; "Build batches" at the end is the work order. **Batch 1 (capture
and container) built 2026-11-19**: what it changed in this design is marked "(batch 1)" where it applies and listed in
section 13 with the reasons; its measurements are in sections 2 and 6.3. **Batch 2 (exact mode and the emulated
replay) built 2026-11-19**: its changes are marked "(batch 2)" and listed in section 14. **Batch 3 (side tags) built
2026-11-19**: its changes are marked "(batch 3)" and listed in section 15. **Batch 4 (ring, F10, save-state loads,
write-up) built 2026-11-20**: its changes are marked "(batch 4)" and listed in section 16; the measured budget (A9) is in
6.3. **Step 5 (shadow texture memory and decode, the `tex` consumer) built 2026-11-20**: section 17. Read with `RENDERER_SURVEY.md` (what the kicks carry) and `ICO_RENDER_STUDY.md` section 2 (where the
game builds its packets). Paths are relative to the repository; the runtime is `third_party/ps2recomp/ps2xRuntime`
(`RT/` below).

## Decisions for the user

None. Nothing found conflicts with option C. Three things the design fixes that the plan left open, for information:

1. **The record has two tiers.** The plan's diagram puts one "frame record" after the readers. Steps 5-8 have to be
   developed off-line *before* those readers exist, so step 4 records one level lower: the **kick record** (what the
   game sent, byte for byte). The decoded **draw list** the RHI replays is the readers' output; its container and
   first commands are defined here (section 9) and it grows in steps 6-9.
2. **Reference pictures need the software GS.** A bit-exact reference is only possible from the runtime's CPU
   rasteriser run synchronously (`PS2X_GS_GPU=0 PS2X_GS_HW=0 PS2X_GS_THREAD=0 PS2X_MTVU=0 RC_TRACK_SHADER=0`).
   `tools/frame_record.sh` sets that; the game runs slower while such a dump is taken, which changes nothing in the
   record. Any other configuration can still record (no reference pictures).
3. **Dumps hold game data** (packets, textures, pictures) and are never committed (`*.rcfr` goes in `.gitignore`).

## 1. Summary

- **Unit: the display cycle.** Everything between two stores to DISPFB2, the closing store included. By the
  survey's frame order one cycle is: the flip's draw environment and clear, the animated texture uploads, K2 (3D of
  game frame N), any texture construction uploads, K1 (2D of frame N+1), then the flip that shows the buffer all of
  it drew into. So a cycle is exactly one displayed picture, and the picture read from GS memory at the closing
  store is its reference. Movies and the loading splash fit the same unit (one cycle per picture).
- **Kick record.** Per cycle, in arrival order: each DMA kick on channels 1 and 2 as its *registers* (CHCR, MADR,
  QWC, TADR, ASR0/1), every hardware store that matters to the graphics machine (GS privileged, VIF1, GIF), and the
  side tags. No packet bytes are stored in the kick itself.
- **By-reference data: a sparse page shadow of guest RAM.** The recorder keeps a host copy of every 4 KB page a
  kick's stream touches. At a kick it compares each touched page with the copy and emits only the pages that are new
  or changed, before the kick. A replay rebuilds a RAM image from those pages and walks the kick from the same
  addresses. Static level lists (3.8-9.0 MB a frame, measured) are stored once; a race frame adds roughly 0.3-0.5 MB.
- **One consumer interface, live and off-line.** A consumer gets `(kick registers, RAM view, tags)`. Live, the RAM
  view is guest RAM itself (no copy at all, so the P1 renderer costs nothing extra); off-line it is the rebuilt
  image. The readers of steps 5-8 are written once against it.
- **Keyframes** make a dump self-contained from any segment start: GS memory (4 MB), privileged and general GS
  registers, VIF1 and VU1 state. Each keyframe also resets the page shadow.
- **Exact mode** (the configuration in decision 2) adds what off-line development needs from the emulated GS: the
  reference picture per cycle, the general register file before every kick, a hash of the GIF bytes each kick
  produced, exact GS memory at keyframes.
- **Replay tool `rc_replay`**: reads a dump and feeds consumers. `stats` (walks everything, checks every stream
  hash), `emu` (re-submits the kicks to the runtime's own VIF1, VU1 and software GS and must reproduce every
  reference picture and keyframe bit for bit: the proof the record is complete), later `tex`, `blit`, `gif2d`, `vif`
  and `vk` (steps 5-8).
- **Side tags** (`rc::gfx`): native code logs begin/end marks anchored to the guest address it is about to write
  (2D buffer position, chain slot) or to the kicks it is about to make. The recorder resolves them to stream offsets
  at the kick. They write nothing to guest RAM, are one test of a global when off, are compiled out of the oracle
  and suspended during shadow comparisons.

## 2. What the code does today (the facts the design rests on)

| fact | where |
|---|---|
| Every DMA start goes through one place: a store to D*n*_CHCR with STR set | `RT/src/lib/ps2_memory.cpp`, `PS2Memory::writeIORegister` (the `address >= 0x10008000` block) |
| The runtime already flattens a chain into one buffer at the kick (so the emulated path copies the 5-12 MB each frame today) and hands it to VIF1 or the GIF | same function (`chainBuf`, `appendData`, `appendVifTagData`), then `PS2Memory::processPendingTransfers` |
| With `PS2X_MTVU=1` the copy is queued to a VU thread, which feeds a GIF thread; with `PS2X_GS_THREAD=1` the rasteriser is a fourth thread. With all three off everything runs inside the store, on the EE thread | `RT/src/lib/ps2_async_vif.cpp`, `gs/gs_threaded_backend.cpp` (`GSThreadedBackend::MakeDefault`) |
| All guest code (every EE thread, every interrupt handler, the movie player) runs on one host thread, the EE executor | `RT/src/lib/Kernel/EeScheduler.cpp` (`m_executorThread`) |
| Every GIF packet, from any path, passes `PS2Memory::submitGifPacket` | `ps2_memory.cpp` |
| GS privileged stores land in three places (32-bit, 64-bit, I/O) | `PS2Memory::write32`, `write64`, `writeIORegister`: the three `gssurvey::privWrite` sites |
| The save-state code can already serialise the GS front end, the backend's state and memory, the VU1 and the VIF1 | `RT/src/lib/ps2_save_state.cpp` (`GSStateAccess::serialize`, `saveVuState`, `EeScheduler::serializeMachine`) |
| A headless graphics machine exists: the oracle makes one with `runtime.memory().initialize() && runtime.syncCoreSubsystems()` (no window, no ELF needed for graphics) | `decomp/oracle/rc_oracle.cpp`, `PS2Runtime::syncCoreSubsystems` |
| The VU1 programs build as a library that needs only the runtime; the C++ references need only two headers | `rc_vu1_code` in `CMakeLists.txt`; `decomp/vu1/*_shape.cpp`, `vu_shape.cpp` |
| K1: `GfxP2_RenderDmaSendActual` sends `gGfxP2.buffIdx` quadwords from the buffer being swapped out; the write position while drawing is `gGfxP2.pDmaBuff + buffIdx * 16` | `decomp/src/gfx_p2.cpp` |
| K2: `Rdr_EndFrame` -> `startVif1Chain` (D1_CHCR = 0x145: chain, TTE); 3D draws enter the chain only through `Rdr_AddDmaCall`, slot `&gRdr.pDmaTag[gRdr.currDmaTag]` | `decomp/src/rdr.cpp` |
| The frame boundary the game sees: the `GfxP2_SwapBuffers` override | `overrides/rc_revenge_pro_overrides.cpp` (`countGameFrame`, with the survey's scene id) |
| Native code knows when it runs inside a comparison | `decomp/rc_decomp.cpp` (`g_comparing`) |

Measured for this design from the oracle's RAM dumps (one frame each, the chain as built at `GfxP2_SwapBuffers`;
script logic: walk call/ret/cnt/ref tags from `gRdr.pDmaTag`, merge the ranges, classify by the game's rings):

| dump | DMA tags | stream | distinct ranges | 4 KB pages | of which the game's rings (tags, state, shape, skin, Track Editor, water) |
|---|---|---|---|---|---|
| level 5 | 982 | 4.89 MB | 41 | 1,188 | 49 pages (171 KB) |
| level 17 | 1,380 | 5.64 MB | 74 | 1,356 | 48 pages (170 KB) |
| level 18 | 1,893 | 9.16 MB | 31 | 2,228 | 35 pages (120 KB) |
| level 20 | 2,320 | 5.63 MB | 75 | 1,386 | 82 pages (309 KB) |
| front end | 733 | 3.97 MB | 23 | 971 | 21 pages (75 KB) |
| Track Editor | 73 | 0.24 MB | 13 | 59 | 17 pages (45 KB) |

One contiguous range of level data is 2.6-8.0 MB of each race stream; the rest is 20-70 ranges of 0.2-270 KB. zlib
level 1 shrinks the touched ranges to about 30%. What changes per frame outside the rings (polygon channels, alpha
triangles, at the top of the heap) could not be measured from single-frame dumps: batch 1 measures it.

**Measured by batch 1** (plain-mode dumps of 120 cycles, the segment's first cycle left out; a page counts once per kick
that finds it changed; the rings are the survey's ranges, read from the oracle RAM dump of the same level and version:
`gRdr`'s tag array and packet rings, `g_RdrStateDmaBlocks`, `gpGfx_DmaBuff0/1`. The heap layout is the same in both runs:
every K2 TADR in the frame dumps is one of that dump's two tag-array halves):

| dump | changed pages a cycle | in the rings | outside the rings | outside: where |
|---|---|---|---|---|
| level 17 USA | 113 (452 KB) | 41 (163 KB) | 72 (290 KB) | two alternately rewritten buffers at 0x617000 and 0x696000 (30-36 pages each, about 15 pages of each a cycle), read by K2; one page at 0xff000 re-captured whole 14-17 times a cycle (channel 2 uploads read it, and it changes between them: the animated-texture upload headers); a few scattered pages (two 7-page runs at 0xee2000 / 0xfc2000, about 2 a cycle each) |
| level 18 USA | 135 (540 KB) | 63 (253 KB) | 72 (287 KB) | the same buffers (35-36 pages, 11-12 a cycle each), 0xff000 14 times |
| level 17 Europe | 122 (489 KB) | 41 (166 KB) | 81 (324 KB) | the same buffers (43-44 pages, 18 a cycle each), 0xff000 17 times |
| level 18 Europe | 132 (526 KB) | 51 (206 KB) | 80 (321 KB) | the same buffers (42-43 pages, 17 a cycle each), 0xff000 14 times |
| front end USA | 73 (290 KB) | 39 (155 KB) | 34 (136 KB) | a 30-page run at 0xd4f000 and 9 pages at 0xde4000 (4-5 a cycle each), 0xff000 4 times, smaller runs |
| front end Europe | 84 (338 KB) | 48 (191 KB) | 37 (146 KB) | the same kind |

So per-frame data outside the rings is as large as inside them (about 0.3 MB a race cycle), but it is a small fixed
set of pages; the level's static lists never change between keyframes. The page at 0xff000 is the one case where
whole-page capture is wasteful: each re-capture is made for the 96-byte header kick of an animated-texture upload
(14-17 x 4 KB raw a race cycle); a finer unit for re-captures is a possible later saving, not needed for the budget.

## 3. The kick record

### 3.1 Events

A cycle is an ordered list of these events, exactly as they happened on the EE thread:

| event | when | contents |
|---|---|---|
| `PAGE` | before the kick that needs it | a run of 4 KB pages of guest RAM (or the scratchpad), valid from here on |
| `KICK` | a D1_CHCR or D2_CHCR store with STR | the channel's registers at the store, the caller, counts and hashes, resolved tags; in exact mode the GS register file before it |
| `HW` | a store to a GS privileged register (0x12000000-0x12001fff), a VIF1 register (0x10003c00-0x10003cff) or a GIF register (0x10003000-0x100030ff) | address, width, the register's resulting 64-bit value, caller |
| `CRT` | `SetGsCrt` | interlace, mode, field/frame |
| `TAG` | native code called `rc::gfx` | section 5 |
| `ODD` | traffic the survey said never happens: a VIF0 kick, a FIFO store, `GS::writeRegister` from a stub, a native GIF fast path, PATH2 data, a local-to-host transfer | kind, address, value. A dump with an `ODD` event is reported by every tool: the record may be incomplete there |
| `NOTE` | recorder messages (state loaded, trigger fired) | text |
| `END` | the DISPFB2 store that closes the cycle (recorded as `HW` first) | counters, timing, the display registers, the reference picture's hash |

The cycle closes at a DISPFB2 store when at least one `KICK` came since the last close or the FBP differs from the
last close's; other DISPFB2 stores are plain `HW` events. Stores to DISPLAY2, PMODE and BGCOLOR that Sony's library
makes after DISPFB2 simply open the next cycle's list.

### 3.2 By-reference data: the page shadow

`rc::rec::PageTracker`: 8,192 RAM pages plus 4 scratchpad pages (page index 8192-8195), each "unknown" or holding a
4,096-byte host copy.

- The runtime's own chain walk reports every byte range the kick's stream is made of, in stream order
  (`kickTouch`): each DMA tag (16 bytes) and each payload. A normal-mode kick is one range (MADR, QWC * 16).
- For each page of each range, once per kick (a bitmap): unknown or `memcmp` differs -> copy it, emit it. Consecutive
  pages go out as one `PAGE` event. All of a kick's `PAGE` events precede its `KICK`.
- Cost per K2: one `memcmp` pass over what the stream touches (at most 12 MB, about 1-2 ms) and a copy of what
  changed. Only while a recorder is attached.

What a replay may rely on: **at a `KICK`, the rebuilt image equals live guest RAM on every byte of that kick's
stream.** Other bytes of the same pages are whatever they were when the page was last captured, and pages never
touched are absent (`RamView::has(page)` is false; reading one is a consumer bug that the reader reports). Consumers
therefore read only the stream; anything else they need from the game comes in tag payloads, never by following a
guest pointer.

Rejected:

| alternative | why not |
|---|---|
| Store each kick's flattened stream | 5-12 MB a frame (0.3-0.7 GB/s); loses guest addresses, which identify the VU program (study 2.3), anchor the tags and key step 9's static geometry |
| Content-hashed blobs per DMA payload | Same saving as pages with more machinery (a store, hashing 12 MB a frame, collisions to rule out); deduplicates equal data at different addresses, which RC doesn't have |
| A whole-RAM snapshot at level load plus whole-RAM deltas | 32 MB compared every kick, and every unrelated game variable that changes lands in the dump |
| Record below the VU1 (the GIF stream, as PCSX2's GS dumps or the runtime's `PS2X_GS_RECORD`) | It is the *output* of the VIF/VU stage the reader of step 8 must replace, 7.5 MB a frame of PATH1 triangles, no addresses, no tags. Kept only as a hash per kick |
| Record only the decoded draw list (ico-pc's `.rddump`) | Its producers are steps 6-8; they could not be developed from it |

### 3.3 Hashes

`rc::rec::Hash64`, a streaming 64-bit hash defined in `render/record/format.h` (normative there): four 64-bit
lanes over 32-byte blocks as `GSRecording::HashVram` does, the last block zero-padded, the byte length mixed in at
the end. Chunking never changes the result. Uses:

- `streamHash`: the kick's stream as the DMA controller delivers it (for a chain: per tag the upper 8 bytes when
  CHCR.TTE is set on a VIF channel, then its payload; for normal mode the range). Computed live from the runtime's
  `chainBuf` or the RAM range. A host walker that reproduces it has read the same bytes in the same order.
- `gifHash` (exact mode): every byte `submitGifPacket` received because of this kick, in order (PATH1 for K2).
- `refHash`: the reference picture's bytes.

### 3.4 Keyframes and segments

A dump is a sequence of **segments**; each starts with a keyframe and is replayable alone. At a keyframe the page
shadow is emptied, so the segment re-emits the static pages on first touch (about 2-4 MB compressed per keyframe).

Keyframe contents, taken on the EE thread at a cycle close after `ps2x::asyncvif::sync` and the backend's `Flush`
(batch 1: no `Sync(GSSyncReason::DebugReadback)`; GS memory is taken as a save state takes it, see 13):

| chunk | contents | used by |
|---|---|---|
| `GSVR` | GS local memory, 4 MB as the runtime stores it (`SnapshotVram`) | texture memory at segment start (steps 5, 7, 8), the `emu` consumer |
| `GSPR` | `GSRegisters` (the privileged registers), 160 bytes | flip and display state |
| `GSRG` | the general register file: `u64[0x63]` indexed by GS register address (PRIM, RGBAQ, ST, UV, TEX0/1, CLAMP, MIPTBP, XYOFFSET, PRMODECONT, PRMODE, TEXCLUT, SCANMSK, TEXA, FOGCOL, SCISSOR, ALPHA, DIMX, DTHE, COLCLAMP, TEST, PABE, FBA, FRAME, ZBUF, BITBLTBUF, TRXPOS, TRXREG, TRXDIR, both contexts). (batch 1) Not re-encoded from `GS::getDebugSnapshot()`: the GS front end keeps the values as written (`ps2x::capture::g_gsRawRegs`, PACKED vertex data in register form), see 13 | the readers' register file at segment start |
| `VU1M` | VU1 micro memory then data memory (16 KB each) | the VIF reader and the C++ references (data memory persists between runs) |
| `VU1R` | VF[32][4], VI[16], ACC, Q, P, I as plain values | the C++ references ("a program can store registers it left set in an earlier run") |
| `VIF1` | the runtime's `VIFRegisters` for VIF1 plus ROW and COL | the VIF reader (CYCLE, MODE, MASK, BASE, OFST, TOPS persist across kicks) |
| `EMUS` | opaque: the runtime's own serialisation of the GS front end, backend state, VU1 and VIF1 (`ps2x::savestate::saveGraphics`, new, cut from `serializeMachine`), with `emuStateVersion` in the file header. (batch 2) Without GS memory (`loadGraphics` takes `GSVR`), so a keyframe does not carry 4 MB twice: 112 KB raw, 11 KB stored. Written in both modes | only the `emu` consumer, so no hidden emulator state (CLUT cache, vertex queue, a GIF tag in progress, path 3 mask) can break bit-exactness. A dump whose version differs from the tool's is refused by `emu` only |
| `TAGP` | tags emitted but not yet consumed by a kick (section 5.3) | tags of the segment's first K1/K2 |

A dump ends with a **final keyframe** (flag `final`, `GSVR` + `GSPR` + `GSRG` only): the state after the last cycle,
so a replay can compare all of GS memory at the end. In exact mode `GSVR` is exact; otherwise the keyframe carries
`vramExact = 0` (a GPU backend's memory image is whatever its `SnapshotVram` gives) and tools say so.

A save-state load (F9) while recording closes the segment; the next cycle close writes a keyframe flagged
`afterStateLoad`. (batch 4) As built the cycle in progress closes just before the machine is replaced (its `END`
carries the picture the display shows then, with a `NOTE`), and the keyframe is taken right after the load, before any
guest code runs: the cycles before the load replay and verify as recorded, the segment after it starts from the loaded
machine. `emu` loads such a keyframe without comparing GS memory against it (section 16).

### 3.5 Exact mode extras

On when the run is synchronous and software-rendered (the recorder checks `ps2x::asyncvif::active` is false and the
backend is the bare `GSCpuBackend`; (batch 2) one runtime function, `ps2x::capture::exactCapable`, asks the GS itself
instead of a `GSThreadedBackend::DefaultName()`) and `RC_TRACK_SHADER` is not 1 (batch 2: the track shader replaces
four VU1 programs, so the GIF output would not be the VU1's). Then, and only then:

- `KICK.regs`: the 0x63-entry register file read from the emulated GS just before the kick. Lets the 2D reader
  (step 7) be developed and tested on K1 before a VIF reader exists, although K1 depends on what K2 left behind.
- `KICK.gifHash`, `gifBytes`. (batch 2) So the `KICK` record is emitted at `kickDone`, after the runtime processed the
  stream, in both modes; a kick's `PAGE` events still precede it.
- `END.refHash` and, every `ref`-th cycle, a `REFI` chunk: the 640 x H rectangle (H = 240 USA, 256 Europe, from the
  file header) at DISPFB2's new FBP, FBW 10, read through the PSMCT32 layout: 4 bytes a pixel R, G, B, A exactly as
  stored (alpha raw, 0x80 = opaque). DISPLAY2 is not applied (the survey: ignore its position and its 512-line
  height in movies).
- Exact `GSVR`.

With `RC_FRAME_RECORD_STRICT=1` a run that cannot be exact stops with a message instead of recording without
references.

### 3.6 Timing

`KICK.field` and `END.field`: the vertical blank counter (`gs_regs.vsyncTick`). `END.hostNs`: steady-clock
nanoseconds since the segment's keyframe. `END.gameFrame`: from the last `Scene` tag (all ones if none). No wall
clock, user or path strings are written.

## 4. Code layout and interfaces

```
RT/include/ps2_capture_hooks.h        the runtime's hook table (null by default, as ps2_host_platform.h)
render/record/format.h                on-disk structs, ids, Hash64 (no dependencies; C++17)
render/record/consumer.h              Consumer, KickView, RamView, CycleInfo, Keyframe, TagView
render/record/page_tracker.{h,cpp}    3.2
render/record/chain_walk.{h,cpp}      the host's DMA chain walker over a RamView (step 8 builds on it)
render/record/writer.{h,cpp}          chunks, zlib, index
render/record/reader.{h,cpp}          chunks -> RAM image -> Consumer calls; random access by segment
render/record/capture.{h,cpp}         live: implements the hooks, cycle logic, triggers, ring, fan-out to consumers
render/record/emu_state.{h,cpp}       keyframe <-> runtime (needs ps2_runtime)
render/gfx/tags.{h,cpp}               the side-tag channel (section 5)
tools/rc_replay/                      the replay tool (section 7)
tools/frame_dump.py                   inspector (section 6.4)
tools/frame_record.sh                 play.sh with the exact-mode environment and RC_FRAME_RECORD passed through
```

CMake (top level): `rc_record` = static library of `format`, `consumer`, `page_tracker`, `chain_walk`, `writer`,
`reader`, `gfx/tags` (needs only zlib: `find_package(ZLIB)`); `rc_record_emu` = `emu_state` (+ `ps2_runtime`). The
game links both plus `capture.cpp`; the oracle links neither and compiles the tag header to nothing
(`RC_GFX_TAGS=0`). Namespaces: `rc::rec` (record), `rc::gfx` (tags), `rc::rd` later (readers, draw list).

### 4.1 Runtime hooks (`ps2x::capture`)

```cpp
namespace ps2x::capture {
struct Hooks {
    // PS2Memory::writeIORegister, a CHCR store with STR on channel 0/1/2, after the DMAC-enable test.
    void (*kickBegin)(uint32_t channelBase, uint32_t chcr, uint32_t madr, uint32_t qwc,
                      uint32_t tadr, uint32_t asr0, uint32_t asr1, uint32_t callerRa);
    // Each range of the stream in order. physAddr: offset into RDRAM or the scratchpad. isTag: a DMA tag.
    void (*kickTouch)(uint32_t physAddr, uint32_t bytes, bool scratchpad, bool isTag);
    // Before processPendingTransfers. flat: the chain as flattened (null for normal mode).
    void (*kickEnd)(const uint8_t *flat, uint32_t flatBytes, uint32_t dmaTags);
    // After processPendingTransfers returned.
    void (*kickDone)();
    void (*gifPacket)(int path, const uint8_t *data, uint32_t bytes);   // submitGifPacket; any thread
    void (*hwStore)(uint32_t address, uint32_t width, uint64_t value, uint32_t callerRa);
    void (*setCrt)(int interlaced, int mode, int ffmd);
    void (*odd)(int kind, uint32_t address, uint64_t value);
    void (*stateLoaded)();
    void (*hotkey)();                                                    // F10, from either host loop
    // (batch 1) PS2Runtime::initialize finished: the runtime the capture reads keyframes from.
    void (*attach)(PS2Runtime *runtime);                                 // (first in the struct as built)
    // (batch 1) The game's GfxP2_SwapBuffers override: game frame, frames since the level started (all ones before
    // the first level start), the survey's scene id. Feeds the game:/level:/scene: triggers until Scene tags exist.
    void (*gameFrame)(uint64_t frame, uint64_t levelFrame, uint32_t scene);
};
inline const Hooks *g_hooks = nullptr;
inline uint32_t g_storeCallerRa = 0;                                     // (batch 1) set by Store32/Store64
}
```
(batch 1) The header also declares the GS register shadow (`g_gsRawRegs`) and `snapshotGs` (keyframes; defined in
`ps2_save_state.cpp`, where the GS's friend struct lives). It is not included by `ps2_runtime.h`: a change to a header
that file includes recompiles all the recompiled game code (15 minutes per version), so nothing was added to
`GS`, `PS2Memory` or `PS2Runtime` themselves.

Sites (all beside an existing `gssurvey` hook, which stays): `writeIORegister` (begin; `kickTouch` in the chain loop
after each tag is read and in `appendData`, and for the two `enqueueTransfer` calls; end; done), `submitGifPacket`,
the three privileged store sites plus the VIF1 and GIF register blocks of `writeIORegister`, `System.cpp`
(`SetGsCrt`), the FIFO, `GS::writeRegister` and native-fast-path sites (`odd`), `ps2_save_state.cpp` (after a load),
`ps2_runtime.cpp` (F10 in `runHostPlatformLoop` and the raylib loop). `callerRa` is the survey's `noteStoreCaller`
value (`PS2Runtime::Store32/Store64`). Unset, each site is one test of a null pointer. The runtime never drops or
changes anything because of a hook in step 4.

### 4.2 The consumer interface (`render/record/consumer.h`)

```cpp
namespace rc::rec {
struct RamView {                       // live: guest RAM itself; off-line: the rebuilt image
    const uint8_t *ram;                // 32 MB
    const uint8_t *scratchpad;         // 16 KB
    bool has(uint32_t page) const;     // always true live
    uint32_t generation(uint32_t page) const; // bumped at each PAGE event (0 live unless a tracker runs)
};
struct TagView { uint32_t seq; uint16_t kind; uint8_t op; uint8_t depth; uint32_t anchor, parent;
                 const void *payload; uint32_t payloadBytes; };
struct Span { uint32_t streamOffset, guestAddr; const TagView *tag; uint8_t op; };   // begin or end
struct KickView {
    uint32_t cycle, index;             // position
    uint8_t channel, mode; bool tte;   // 1 = VIF1, 2 = GIF; 0 normal, 1 chain
    uint32_t chcr, madr, qwc, tadr, asr0, asr1, callerRa, mxcsr, fbrst;
    uint32_t dmaTags, streamBytes, gifBytes; uint64_t streamHash, gifHash, field;
    const Span *spans; uint32_t spanCount;          // address-anchored tags, by stream offset
    const TagView *const *labels; uint32_t labelCount; // time spans open at the kick, outermost first
    const uint64_t *regs;              // GS register file before the kick (0x63 entries) or null
};
struct Consumer {
    virtual void open(const FileInfo &) {}
    virtual void keyframe(const Keyframe &) {}
    virtual void cycleBegin(uint64_t cycle) {}
    virtual void pages(uint32_t firstPage, uint32_t count, const uint8_t *data) {} // (batch 1) PAGE events
    virtual void hw(uint32_t address, uint32_t width, uint64_t value, uint32_t callerRa) {} // (batch 1) + callerRa
    virtual void crt(int interlaced, int mode, int ffmd) {}
    virtual void tag(const TagView &) {}             // every tag, in emission order
    virtual void kick(const KickView &, const RamView &) {}
    virtual void odd(int kind, uint32_t address, uint64_t value) {}
    virtual void note(const std::string &text) {}    // (batch 1) NOTE events
    virtual void cycleEnd(const CycleInfo &, const uint8_t *refRgba) {} // refRgba null without a picture
    virtual void close() {}
    virtual ~Consumer() = default;
};
}
```

All calls are made on one thread, in event order: the EE thread live (inside the hook), the tool's thread off-line.
A live consumer must not block on the GPU or on another thread's work. The file writer and the ring are consumers
too; `capture.cpp` owns the page tracker and the list of consumers. (batch 1) Being a consumer, the writer needs the
`PAGE` and `NOTE` events and each store's caller, which the interface above lacked: `pages`, `note` and `hw`'s
`callerRa` were added. Off-line, `pages` is called after the reader has copied the pages into the RAM view. As built,
`KickView` also carries `cycle` (64-bit) and `hasGifHash`, `RamView` a `pageOf(offset, scratch)` helper, and
`render/record/consumer.h` is the exact interface.

`rc::rec::walkChain(const KickView &, const RamView &, Visitor &)` in `chain_walk.cpp` mirrors the runtime's walk
exactly (tag ids 0-7, the two-deep ASR stack, IRQ with TIE, the TTE bytes on VIF channels, wrap at the end of RAM,
the 2^20 tag cap) and reports `(streamOffset, guestAddr, bytes, isTag)` ranges; `streamHash` proves it equal on
every recorded kick.

## 5. Side tags (`rc::gfx`, `render/gfx/tags.h`)

### 5.1 Model

A tag is a small fixed-layout record native code appends to a host-side log. Three shapes:

- **set**: frame state that holds until the next one (scene, view and camera, pass).
- **address span**: `begin` / `end`, each anchored to the guest address the game will write next in a per-frame
  buffer. Everything the kick's stream delivers from the begin address up to the end address belongs to the tag.
  Two anchors cover the game: the 2D buffer (`gGfxP2.pDmaBuff + buffIdx * 16`) and the chain slot
  (`&gRdr.pDmaTag[currDmaTag]`); `beginAt(address, ...)` takes any other buffer position (polygon channels).
- **kick span**: `begin` / `end` with no address: labels every kick made while it is open (uploads, the flip, K1,
  K2, movie pictures), for kicks made inside Sony's library. (batch 3) Spans may close out of order (a guest thread can
  switch inside a blocking call): `end(token)` closes that span wherever it is in the open list. Kick spans never
  expire; the flip's span stays open across the cycle close its DISPFB2 store makes.

Resolution happens at the kick, in the recorder: an address-anchored tag whose anchor lies in one of the kick's
ranges gets that range's stream offset plus the distance into it, and goes into the kick's `spans` (sorted by stream
offset, then sequence). (batch 3) A DMA tag's range matches only its own address (the tag's stream offset); a payload
range matches `[start, end]`, its end included (an end mark after the last packet, an empty span at the end of K1).
Resolution also runs while the recorder waits for its trigger, so the first keyframe's `TAGP` holds only tags no kick
has consumed. Anchors must be in buffers a kick visits once (the tag array, the rings, the 2D buffer,
polygon and alpha buffers), never in static level data, which one chain can reference twice; an anchor is resolved
at its first visit only and repeats are counted. Tags never consumed are dropped two cycle closes after they were
made, and counted.

Tags are advisory. A consumer treats a missing tag as "unknown" (no key: don't interpolate; no HUD id: default
anchor). They are missing by design in three cases: sampled calls under `RC_SHADOW`, functions that run recompiled
(the Europe build's changed functions until they get variants), and SDK code.

### 5.2 API

```cpp
namespace rc::gfx {
namespace detail { extern bool g_live; }                    // recorder or renderer attached, and not comparing
inline bool live() { return RC_GFX_TAGS && detail::g_live; } // RC_GFX_TAGS is 0 in the oracle build

struct DrawKey { uint32_t kind, object, part, ordinal; };   // object: a guest address used as an identity only

void scene(const SceneTag &);                 // once per game frame, from the GfxP2_SwapBuffers override
void view(const ViewTag &);                   // per view: index, scissor, vertical offset, camera
void pass(uint32_t pass);                     // world, shadows, objects, pickups, HUD, weapons, alpha, effects, menu...
uint32_t begin2D(const DrawKey &, const HudTag *hud = nullptr);      // anchor: the 2D buffer's write position
uint32_t beginChain(const DrawKey &, const float *matrix3x4 = nullptr); // anchor: the next chain slot
uint32_t beginAt(uint32_t guestAddress, const DrawKey &);
void end(uint32_t token);                     // anchor of the same kind, read now
uint32_t beginKicks(const KickLabel &);       // K1, K2, texture upload, animated texture, backdrop, movie, flip
void endKicks(uint32_t token);
void label(const char *text);                 // debugging: shown by the inspector

struct Span2D    { uint32_t t; explicit Span2D(const DrawKey &k, const HudTag *h = nullptr) : t(live() ? begin2D(k, h) : 0) {} ~Span2D() { if (t) end(t); } };
struct SpanChain { /* the same with beginChain */ };
struct SpanKicks { /* the same with beginKicks */ };
}
```

Rules for every call site (they are what keeps game behaviour and the oracle untouched):

1. Only in native functions, after any `RC_DIVERT_IF_CHANGED`. Arguments are built inside `if (rc::gfx::live())` or
   by a `Span*` constructor, from values the function already has; no `guest_call`, no `guest_checkpoint`, no
   hardware access, no allocation from the game's heap.
2. The channel reads guest RAM (the two anchors) and never writes it; it charges no EE cycles and cannot yield.
3. `detail::g_live` is `enabled && !comparing`. Every assignment to `g_comparing` in `decomp/rc_decomp.cpp` goes
   through one inline setter that also updates `detail::g_live`. Random-input runs and both halves of a
   live comparison therefore log nothing. (batch 3) The native half of a live comparison runs with `g_comparing`
   false and `g_inComparedNative` true, so both flags go through setters (`setComparing`, `setInComparedNative`) and
   "comparing" for the channel is either of them.
4. Nothing reads a tag back into game logic. The channel has no query functions.

Cost when off: one load and test of a global byte per site; in the oracle build nothing.

(batch 3) As built (`render/gfx/tags.h` is normative): `RC_GFX_TAGS` defaults to 0 in the header and is set to 1 only
for the game target's build of the native and override sources (a generator expression on those sources'
`COMPILE_DEFINITIONS`, since the same files compile into `rc_oracle`), for `capture.cpp` and for `rc_record`. With 0
every function is an inline no-op and `live()` is `constexpr false`: `rc_oracle` holds no `rc::gfx` symbol. The
channel itself (`render/gfx/tags.cpp`, in `rc_record`) knows no game layout: the game registers the two anchor readers
(`decomp/rc_gfx_tags.cpp`), and a consumer attaches through `rc::gfx::detail::attach(Sink *)`. The capture attaches
when `RC_FRAME_RECORD` installs it (`RC_FRAME_RECORD_TAGS=0` leaves the channel detached) and detaches when the dump is
closed. `begin2D` with a `HudTag` makes one `Hud` begin (payload: the key and the element), without one a `Draw`
begin. `pass`, `beginAt`, `label` and `Cut` are built but have no call site yet.

### 5.3 In the record

`TAG` events are written in emission order with a dump-wide sequence number. A kick refers to tags by sequence
(`spans`, `labels`); K2's tags were emitted in the cycle before, so a reader keeps the tags of the current and the
two previous cycles, and a keyframe re-emits the unconsumed ones as `TAGP`. A `TNAM` chunk after the file header
lists `kind`, pass, draw-kind, HUD and label enumerators as text (`table value name`), so tools print names without
the game's headers.

Tag kinds, version 1 (payloads little-endian, no pointers to follow, sizes multiples of 8):

| kind | payload |
|---|---|
| 1 `Scene` | game frame (the override's counter), `gRdr.frameNumber`, scene id (the survey's: mode << 12, level << 4, split << 1, movie; (batch 3) as the override and the `scene:` trigger compute it, without the movie bit), flags |
| 2 `View` | index (0, 1 in split screen), scissor x0 y0 x1 y1, vertical draw offset, camera position, camera rotation (3 x 3, from `_sRdrCamera`), `dVal` `dValX` `dValY` (projection scales), `minZ` `maxZ`, `viewAngle` |
| 3 `Pass` | pass id |
| 4 `Draw` | `DrawKey`, flags, optional object matrix (3 x 4 floats) |
| 5 `Hud` | element id, its box in the game's 2D units, anchor hint (left/right/top/bottom/centre/stretch) |
| 6 `Kick` | what (1 K1, 2 K2, 3 texture upload, 4 animated texture, 5 backdrop, 6 movie picture, 7 flip), object (the texture's `_sGfxTexture` address as identity), two free words |

(batch 3) Payload sizes as built: `Scene` 24, `View` 104 (also the camera's guest address), `Pass` 8, `Draw` 24 (key,
flags, has-matrix) plus 48 with the matrix (3 x 4 floats: the object's rotation rows then its position), `Hud` 40 (key,
element, anchor hint, box; the box is 0 where the function does not know it), `Kick` 16, `Label` the text padded with
zeros, `Cut` 8. `TNAM` is text, one `table value name` line each for the tables `kind`, `op`, `draw`, `hud`, `anchor`,
`kick`. `TAGP` is the waiting tags as `TAG` records (header and body as in `CYCL`); a reader adds them to its tag store
without delivering them again as tag events.
| 7 `Label` | text |
| 8 `Cut` | reason (camera mode change, level transition): reserved for interpolation |

Step 4 builds the channel and a first set of sites that exercises every shape: `Scene` (the override), `View`
(`SetFrameWindow` / where the frame loop sets up each view's camera), `Kick` labels (`GfxP2_RenderDmaSendActual`,
`startVif1Chain`, `GfxTex_Update`, `BackdropDisplayBMP24`, the texture upload helpers in `gfx_p2.cpp`,
`ldimage.cpp`'s `loadImage`), `Draw` on the chain (`TRdr_VUShape`, `Rdr_VUSkin`), `Draw` + `Hud` in the 2D buffer
(the `Hud*` panel functions of `hud.cpp`, one font string function). Full coverage is added when a feature needs it
(P3: keys for interpolation, HUD anchors). (batch 3) As built: `View` in `MainGameProcess` right after the second
`SetFrameWindow` of each view (one tag per view and frame); a `flip` label in `GfxP2_SwapBuffers` around
`sceGsSwapDBuff` (otherwise the draw-environment kick that opens each cycle was the one unlabelled kick); `Hud` in
`HudPanel`, `HudTopRight`, `HudBottomRight`, `HudBottomLeft`, `HudMiddle` (`HudTopLeft` draws nothing); the font
function is `FontPrint` (so also `FontCentre` and `FontPrintRight`). In the Europe build the changed functions run
recompiled, so `View` (`MainGameProcess`), `Hud` (the panel functions), the animated-texture label (`GfxTex_Update`)
and the backdrop label (`BackdropDisplayBMP24`) are missing there by design (5.1, second case).

### 5.4 How P4's native API grows from it

The key and descriptor structs are the future draw calls' arguments. Today a path does
`rc::gfx::SpanChain span(key, matrix); /* builds its packets */`; when the path moves to the native API the same
function, in the `vk` build, calls `rc::gfx::drawShape(key, matrix, shape)` instead of building packets, and the
renderer receives the same key by another route. The reader passes tags through into the draw list as scope commands
(section 9), so the presenter sees one kind of key whichever way a draw arrived.

## 6. Recording

### 6.1 Modes

| mode | what runs | cost | for |
|---|---|---|---|
| off (default) | nothing | a null test per hook site, a byte test per tag site | play, oracle, shadow |
| live consumer (the P1 renderer, from step 6) | hooks -> `rc::rd` reader directly, RAM view = guest RAM, no page tracker, no writer | the reader's own | `RC_RENDERER=vk` |
| file | page tracker + writer | 1-2 ms a frame + disc | off-line work, tests |
| ring | page tracker + the last *n* cycles kept encoded in memory, whole segments at a time; F10 writes them | 1-2 ms a frame, memory below | catching a glitch after it happened (the speedometer flash) |

The renderer needs no ring of raw records: it consumes each kick where it happens. Its own in-memory ring is of
closed draw lists (section 9).

### 6.2 Switches

```
RC_FRAME_RECORD=<file>          write a dump (".rcfr"; "%d" numbers successive dumps of one run)
RC_FRAME_RECORD_AT=<trigger>    when; default "key" (batch 1-3: "boot+120"; batch 4: "key")
    boot[+count]                from the first kick
    cycle:<n>[+count]           display cycles since boot
    game:<n>[+count]            game frames since boot (the override's counter)
    level:<n>[+count]           game frames since the level started (as RC_RAMDUMP_FRAME counts; batch 1: only once
                                a level has started, so a long boot or front end cannot fire it early)
    scene:<hex>[+count]         the first cycle of a scene id
    key[+count]                 F10 starts, F10 or count stops (default count 120)
    ring:<n>                    keep the last n cycles; F10 writes them and goes on
RC_FRAME_RECORD_F10=<c>[,<c>]   (batch 4, a test aid) act as if F10 was pressed at the close of these display cycles
RC_FRAME_RECORD_REF=<n>|hash|off  reference picture every n-th cycle (default 1; ring: hash), hashes always (exact mode)
RC_FRAME_RECORD_KEY=<n>         cycles per segment (default 300; ring 120)
RC_FRAME_RECORD_STRICT=1        stop the run if exact mode is not possible
```

A recording starts and stops at cycle closes. `count` is in cycles. Both versions use the same switches;
`tools/frame_record.sh [play.sh arguments]` is `play.sh` with the exact-mode environment and honours `RC_VERSION`.
`platform/vk_host.cpp` needs no change in step 4 (F10 arrives through the runtime's loop). (batch 4) Successive dumps of
one run (`key` after each stop, every F10 of `ring`) take the next number for `%d`; a name without `%d` gets `-1`,
`-2`... before its extension for the second and later dumps.

### 6.3 Size budget (estimates from section 2's measurements; batch 1 replaces them with measured numbers)

| part | raw | stored (zlib level 1) |
|---|---|---|
| segment start: `GSVR` + state | 4.1 MB | 1-2 MB |
| segment start: static pages re-emitted on first touch | 4-9 MB (race), 4 MB (front end) | 1.5-3 MB |
| a race cycle: changed pages (rings 0.1-0.3 MB, polygon and alpha buffers, K1 14 KB, texture frames 80 KB until the animations have cycled once) | 0.3-0.5 MB | 0.1-0.2 MB |
| a front-end cycle (K1 up to 137 KB) | 0.2-0.4 MB | 0.1 MB |
| a movie cycle (652 KB of tiles) | 0.7 MB | 0.3-0.5 MB |
| exact mode: register files (25 kicks x 792 bytes) | 20 KB | under 1 KB |
| reference picture | 614 KB (USA), 655 KB (Europe) | 0.1-0.2 MB |

So 120 race cycles with pictures are roughly 30-50 MB, 600 cycles 150-250 MB (with `REF=10`: 60-120 MB). Ring of
300 cycles, hashes only: about 3 segments x (4 + 9 + 120 x 0.5 MB) = 220 MB of memory, uncompressed until written.

**Measured (batch 1, plain mode, 2026-11-19).** Dumps taken with `tools/play.sh` (default renderer: God of War
backend, GS and VU1 threads on) on the RTX 2060: levels 17 and 18 (`RC_BOOT_LEVEL`, `level:300+120`), the front end
(`RC_SKIP_INTRO=1`, `game:600+120`) and boot (`boot+900`: legal screen, logos, the publisher movie, title and front
end), both versions. Raw is the records as written, stored is after zlib level 1. USA / Europe:

| part | raw | stored |
|---|---|---|
| keyframe: `GSVR` + state | 4.1 MB | 1.2-1.4 MB (39 KB at power-on, GS memory still empty) |
| segment's first cycle (static pages re-emitted on first touch) | race 4.9 / 5.6 MB (level 17), 10.0 / 9.1 MB (level 18); front end 4.5 / 5.0 MB | 1.5-2.9 MB; front end 1.3-1.4 MB |
| race cycle | 457 / 494 KB (level 17), 544 / 530 KB (level 18); largest 812 KB | 86 / 93, 95 / 102 KB |
| of which outside the game's rings | about 290 / 320 KB (section 2) | |
| front-end cycle | 292 / 340 KB | 63 / 75 KB |
| movie or splash picture (254 a boot) | 350 / 367 KB, largest 2.6 MB | 93 / 94 KB |
| other boot cycles (title, front end, attract) | 301 / 347 KB | 66 / 78 KB |
| `KICK` records (no register files) | 3.5 KB a race cycle (37 kicks), 1 KB front end | |
| exact-mode extras (batch 2): register files | 32 KB a race cycle | (in `CYCL`) |
| exact-mode extras (batch 2): reference picture | 614 / 655 KB | 280-400 KB (section 14) |

Files: 120 race cycles 13.9 / 15.0 MB (level 17), 17.1 / 17.6 MB (level 18), front end 11.3 / 12.8 MB, `boot+900`
71.8 / 79.5 MB (three segments of 300 cycles). The estimates above held: a race cycle is 0.46-0.54 MB raw,
0.09-0.10 MB stored.

**Recorder time** (EE thread, inside the kick hooks: page compare and copy, the stream hash, building the records;
compression and disc writes run on the writer's own thread): 3.3 / 4.1 ms a cycle on average on level 17, 5.4 / 6.8 ms
on level 18, 3.5 / 4.0 ms front end, 2.6 / 3.0 ms over boot; largest single cycle 10-28 ms (a segment's first, which
copies every static page). Keyframes 7-32 ms (GS memory read back from the God of War backend and the VU1/VIF1 state).
This is above the 1-2 ms estimated in 3.2, which left out the stream hash: measured hot-cache, `Hash64` costs about
1.5 ms and the page compare about 1.2 ms per 10 MB of stream. The ring's memory is measured with batch 4.

**A9, the final table (batch 4, 2026-11-20).** Measured with the ring (`RC_FRAME_RECORD_AT=ring:300`, segments of
120 cycles) during `tools/play.sh replay playtest2` (default renderer, MTVU on, RTX 2060, 180 s; the same runs as A5),
from the game's per-segment `ring:` lines, each scene's first segment after a change (the transition) left out, plus the
exact-mode ring of the same replay (`tools/frame_record.sh`, USA). Bytes are a cycle's records raw (as the writer
builds them) and its `CYCL` chunk stored (zlib level 1); recorder time is the EE thread's time in the kick hooks a
cycle (page compare and copy, stream hash, records; compression runs on the writer's thread). USA / Europe; the
race of `playtest2` is the player on the grid with the computer cars racing (its driving inputs are skipped, see
NOTES.md) and the idle camera:

| scene | segments | raw a cycle (PAGE, KICK, TAG, HW + END) | stored a cycle | recorder a cycle (largest cycle) |
|---|---|---|---|---|
| boot, legal screen (0x30) | 1 / 1 | 364 / 379 KB (362 / 378, 0.1, 0.1, 0.3) | 97 / 97 KB | 0.43 / 0.39 ms (0.8) |
| front end (0x1620) | 4 / 8 | 324 / 324 KB avg, 288-379 (318, 1.9, 3.8, 0.3) | 75 / 75 KB avg, 70-87 | 2.9 / 2.9 ms avg, 2.6-3.3 (6.9 / 5.9) |
| race (0x3020) | 76 / 57 | 331 / 340 KB avg, 279-608 (321 / 331, 5.0 / 4.8, 5.1 / 4.4, 0.3) | 63 / 64 KB avg, 51-105 | 4.1 / 4.2 ms avg, 3.2-5.4 (13.3 / 14.0) |
| exact mode, front end (USA) | 9 | 341 KB (326, 10.5 with register files, 4.1, 0.3) | 77 KB | 2.75 ms (5.9) |
| exact mode, race (USA, hashes only) | 3 | 471 KB (427, 37.0, 6.5, 0.3) | 79 KB | 4.0 ms (8.2) |

Keyframes: 4.1 MB raw, 1.2 MB stored in plain mode (GS memory from the God of War backend), 2.3-2.5 MB in exact mode
(the software GS's own memory compresses less); `EMUS` 112 KB raw, 11-14 KB stored. **Ring memory** (`ring:300`, the
compressed chunks the worker keeps, plus the last final keyframe): 21.6-36.8 MB once full (USA), 21.7-36.6 MB
(Europe), peak 43.6 / 43.4 MB just before the oldest segment is dropped; exact mode with picture hashes only 31.6-42.1
MB, peak 47.8 MB. A ring dump is that memory written out: 25.7-43.2 MB for 300-417 cycles. The estimate of 220 MB in
6.3 assumed raw records; the ring keeps them compressed (section 16). Recording costs no frame rate in these runs:
60.0 / 50.0 fps with the ring on, as without (A5).

### 6.4 The file

Little-endian. A file is a sequence of chunks; every chunk and record is padded to 8 bytes.

```
struct ChunkHeader {          // 32 bytes
    char     id[4];
    uint32_t flags;           // bit 0: payload is a zlib stream
    uint32_t crc32;           // of the raw payload (zlib's crc32)
    uint32_t reserved;
    uint64_t storedBytes;     // bytes that follow (before padding)
    uint64_t rawBytes;
};
```

| chunk | payload |
|---|---|
| `RCFR` (first; never compressed) | `FileHeader`: magic `"RCFREC1\0"`, `formatVersion` (1), `headerBytes`, `gameVersion` (0 USA, 1 Europe), `flags` (exact, has pictures, has tags, complete), width 640, height, field rate in mHz, RAM / scratchpad / page / GS memory sizes, `emuStateVersion`, `elfName[16]`, `gsBackend[16]`, `build[40]` (the fork's `git describe`), `indexOffset` (patched at close; 0 = not closed) |
| `TNAM` | tag name table (5.3) |
| `KEYF` | `{cycle, gameFrame, field, flags (vramExact, final, afterStateLoad), stateChunkCount}`, followed by that many state chunks (3.4) |
| `CYCL` | one cycle's events as records (below), compressed as one stream |
| `REFI` | the preceding cycle's reference picture, `width * height * 4` bytes |
| `INDX` | per segment `{offset, firstCycle, cycleCount}`, per cycle `{cyclOffset, refiOffset}` |
| `ENDF` | empty; a file without it was cut short and is still readable up to its last whole chunk (the reader scans when `indexOffset` is 0) |

Records inside `CYCL`: `struct RecHeader { uint16_t type; uint16_t flags; uint32_t bytes; }` (bytes includes the
header), then:

| type | body |
|---|---|
| 1 `PAGE` | `u32 firstPage, count`; `count * 4096` bytes |
| 2 `HW` | `u32 address, width; u64 value; u32 callerRa, pad` |
| 3 `CRT` | `i32 interlaced, mode, ffmd; u32 pad` |
| 4 `KICK` | `u32 index, chcr, madr, qwc, tadr, asr0, asr1, callerRa; u8 channel, mode; u16 kflags (hasGifHash, hasRegs, tte); u32 dmaTags, streamBytes, gifBytes, mxcsr, fbrst, spanCount, labelCount; u64 field, streamHash, gifHash;` then `spanCount` x `{u32 streamOffset, guestAddr, tagSeq; u8 op; u8 pad[3]}`, `labelCount` x `u32 tagSeq` (padded), then `u64 regs[0x63]` if `hasRegs` |
| 5 `TAG` | `u32 seq; u16 kind; u8 op (0 set, 1 begin, 2 end); u8 depth; u32 anchor (physical guest address, 0 = none); u32 parent (enclosing begin; for an end, its begin); u32 payloadBytes, pad;` payload |
| 6 `ODD` | `u32 kind, address; u64 value` |
| 7 `END` | `u64 cycle, gameFrame, field, hostNs, dispfb2, display2, pmode, refHash; u32 refWidth, refHeight, refFbp, kickCount, hasImage, pad` |
| 8 `NOTE` | text |

Versioning: a reader refuses a higher `formatVersion`; unknown chunk ids and record types are skipped by their
sizes, so additions (new tag kinds, new state chunks, the draw list's `DLST`) don't need a version bump. Changing an
existing layout does.

Inspector, `tools/frame_dump.py` (Python standard library only; `zlib` and `struct`):
`info <dump>` (header, segments, cycles, sizes by chunk and record type), `list <dump> [cycle]` (the events, one
line each, tags by name), `tags <dump> <cycle>`, `kick <dump> <cycle> <index>` (registers, spans, the first
stream bytes from the rebuilt pages), `png <dump> <cycle> <out.png>` (the reference picture; alpha forced opaque
with `--opaque`), `verify <dump>` (chunk CRCs, record sizes, index against a scan).

## 7. The replay tool (`tools/rc_replay/`, target `rc_replay`)

A target of the top-level CMake project, `EXCLUDE_FROM_ALL`, as `vu1_recomp` is: it links `rc_record`,
`rc_record_emu`, `ps2_runtime` and `rc_vu1_code` (whole archive, when `vu1/` exists; otherwise the runtime's VU1
interpreter runs the microcode from the dump's `VU1M`, slower, same results), plus the six reference files of
`decomp/vu1` from step 8 on and `rc_ico_rhi` from the `vk` consumer on. It compiles none of the recompiled game
code, needs no disc and no ELF, and one build replays both versions' dumps (the version is in the header; the
microcode is identical). `ninja -C build-decomp rc_replay` in any configured build folder.

```
rc_replay <dump> [--consumer stats|emu|...] [--cycles a-b] [--verify] [--png <dir>] [--quiet]
          [--picture-before <cycle>:<kick> <out>]    emu: the frame buffer just before that kick
          [--path1 <cycle>:<kick> <out>]             emu: the GIF bytes that kick produced
```

It starts at the last segment whose first cycle is not after `a`. Exit 0 pass, 1 a check failed, 2 unusable dump.

**`stats`** (no emulation): per cycle and total: kicks by channel, mode and label, stream bytes, DMA tags by id,
pages and bytes by event type, tags by kind, `ODD` events. With `--verify`: `walkChain` over the rebuilt image
reproduces every `streamHash` and `streamBytes`, never reads an absent page, and every resolved tag's stream offset
lies on a DMA tag or a quadword boundary of its range.

**`emu`** (the completeness proof): a headless `PS2Runtime` (`memory().initialize()`, `syncCoreSubsystems()`),
forced to the bare `GSCpuBackend` with VU1 and GIF synchronous. Per event:

- keyframe: `EMUS` loaded with `ps2x::savestate::loadGraphics` (GS memory included); the RAM image is the reader's;
- `PAGE`: copied into the runtime's RDRAM / scratchpad;
- `HW`: `memory.write32` / `write64` to the address (so a GS reset through CSR, the display registers and VIF1's
  ERR and FBRST go the way they went live); `CRT`: recorded mode applied;
- `KICK`: MXCSR and `cpu().vu0_fbrst` set from the record, QWC / MADR / TADR / ASR0 / ASR1 written, then CHCR:
  the runtime walks, unpacks, runs VU1 and rasterises exactly as it did live;
- `END`: the 640 x H rectangle at DISPFB2's FBP is read as in 3.5.

With `--verify`, on an exact dump: every `gifHash` and `gifBytes` equal (the first difference names the cycle and
kick); every `refHash` equal, and every stored picture equal byte for byte; at every later keyframe and the final
one, all 4 MB of GS memory equal `GSVR`. `--png` writes its pictures (and, on a mismatch, the reference and a
difference image). On a dump that is not exact it replays and writes pictures without verifying.

(step 5) **`tex`** (`--consumer tex [--with-emu]`): the host's shadow of GS texture memory and the texture decode,
alone or behind `emu`; section 17.

Later consumers plug into the same loop (section 8). Two of them reuse `emu` as a back end, which is what lets each
reader be proved without a Vulkan rasteriser in the comparison:

- `gif2d` (step 7) and `vif` (step 8) can run **in front of the emulated GS**: the reader's output, re-encoded as
  GIF packets (for `vif`: the C++ references' kicks), is submitted to the `emu` machine in place of the kick.

## 8. Determinism and threads

- **Where hooks run.** Every `KICK`, `HW`, `CRT` and tag call happens on the EE executor thread, inside the guest
  store or the native function, so the event order is the order the game made them, including kicks from the
  vertical blank handler (the movie player, the splash) between a level load's kicks. No lock is needed between
  hooks. `gifPacket` is the exception: with `PS2X_MTVU=1` it runs on the VU thread, so its hash is kept only in
  exact mode, where it too is on the EE thread.
- **Guest RAM at the hook is what the emulated machine used**: the runtime reads or copies the stream inside the
  same store. The page capture sits before that read in the same call.
- **`PS2X_MTVU`, `PS2X_GS_THREAD`, GPU backends.** They change when the emulated GS *draws*, not what the game
  sends: the kick record is identical in every configuration. They only decide whether the extras of 3.5 exist.
  Keyframes outside exact mode call `ps2x::asyncvif::sync` and the backend's `Flush`/`Sync` first, as a save state
  does.
- **Replay is deterministic**: one thread feeding it, synchronous VU1, the software rasteriser, MXCSR restored
  per kick. The rasteriser's band pool (`gs_band_pool.cpp`) uses worker threads; its result is expected not to
  depend on their number, and the acceptance test checks it (`PS2X_GS_BANDS=1` against the default). Two replays of a dump must give identical hashes.
- **Runs are not reproducible against each other** (interrupt timing is host timing), and don't need to be: a dump is
  compared with its own references.
- **The `vk` host loop.** In P1 the live reader builds a draw list on the EE thread and publishes it at the cycle
  close (a mutex-guarded slot holding the last two closed lists); the main thread's `present` hook replays the newest
  through the RHI. Publishing never waits for the GPU. A draw list is immutable once closed; textures and static
  geometry it names are owned by caches with their own generations (steps 5 and 9). While the emulated GS still
  runs beside it (until P2 turns it off), neither side waits for the other.
- **Shadow, oracle, save states.** Capture hooks and tags are absent from the oracle. Under `RC_SHADOW` a compared
  function's kicks happen once as today (blocking functions are not compared live) and its tags are suppressed. A
  state load forces a new segment (3.4).

## 9. The draw list (tier 2; container and first commands defined here, built by steps 6-9)

The readers' output and the RHI replay's input, on ico-pc's model (study 1.3): one list per cycle; fixed 32-byte
commands `{u16 op; u16 flags; u32 a, b, c; u32 payloadOffset, payloadBytes; u64 d}` with no pointers and a payload
arena addressed by offset, so a closed list is one block that can be published to the presenter, kept in a ring
(the last two or three, for P3's interpolation) or written to a dump as a `DLST` chunk after its cycle. Coordinates
stay in GS units (12.4 fixed point, GS Z, raw 0x80 alpha) until the replay converts them.

| op | first producer | meaning |
|---|---|---|
| `IMAGE_BLIT` | step 6 | CT32 rectangle into a frame buffer (FBP, x, y, w, h; pixels in the payload) |
| `TEX_WRITE` | step 5/6 | shadow texture memory changed (block range): cache invalidation for the replay |
| `FLIP` | step 6 | the display registers at the cycle close |
| `STATE` | step 7 | the finite state block (blend mode and FIX, TEST, ZBUF, scissor, texture key and sampler, PABE, FRAME target), emitted when it changes; layout, the `TEXTURE` command and the vertex format: `RENDERER_2D_DESIGN.md` section 2 |
| `CLEAR` | step 7 | the draw environment's sprite |
| `SCREEN_PRIMS` | step 7 (2D), step 8 (everything through the references) | GS-space triangles or lines: XY, Z, RGBA, STQ |
| `SHAPE_BATCH` | step 9 | a static triangle list by guest address and generation, the VU memory constants, the program id |
| `SCOPE_BEGIN` / `SCOPE_END` | step 7 | a side tag passed through (key, HUD element, view) |

`rc_replay --consumer vk` is then reader + replay through the RHI off-screen with a read-back; `--save-drawlists`
writes them into a copy of the dump for golden-picture tests of the replay alone.

*As built (renderer P1 step 8 batch 4, `RENDERER_3D_DESIGN.md` 20):* the `DLST` chunk is written live, not by a copy:
with `RC_FRAME_RECORD_DRAWLISTS=1` the live frame reader's list of each recorded cycle follows that cycle's `CYCL` (and
`REFI`), zlib-compressed and not indexed; payload `render/frame/dlst.h` (`DlstHeader` "DLS1": command count, cycle, arena
bytes, the cycle's VIF1 kicks; the 32-byte commands; the arena as the list holds it). `rc_replay --consumer dlst`
compares it byte for byte with the list the same reader makes from the recorded kicks (a dump recorded from boot).
`--save-drawlists` was not built.

## 10. Test plan

### 10.1 Step 4's acceptance (both versions unless said)

Dumps, exact mode, by `tools/frame_record.sh`: levels 5, 17, 18, 20 (`RC_BOOT_LEVEL`, `level:300+120`); the front
end (`RC_SKIP_INTRO=1`, `game:600+120`); boot through the logos and the publisher movie (`boot+900`,
`RC_FRAME_RECORD_REF=10`); one level load with its splash (`RC_BOOT_LEVEL=17`, `boot+400`); the Track Editor with
a `tedit_walk.py` pad (120 cycles); USA only, a split-screen attract demo if a `scene:` trigger reaches one.

| # | check | pass |
|---|---|---|
| A1 | `rc_replay --consumer stats --verify` | every stream hash reproduced by `walkChain`; no absent page read; no `ODD` event (batch 1: but `GfxP2_Init`'s two start-up stores into the VIF1 FIFO, which the survey found and which carry no GS data; `rc_replay` counts them apart while no VIF1 kick has been seen, see 13); `frame_dump.py verify` clean |
| A2 | `rc_replay --consumer emu --verify` | every `gifHash`, every reference picture (100% of cycles, bit for bit) and all of GS memory at every keyframe and at the end |
| A3 | the reference is what the game showed | on one static screen per version (legal screen or a paused menu) a `PS2X_SCREENSHOT_DIR` screenshot of a field inside the cycle equals that cycle's `REFI` in RGB after the runtime's presentation transform (the builder states it) |
| A4 | off costs nothing | `RC_FRAME_RECORD` unset: `playtest2` replays follow the default scene log (frames, clocks, scenes) at 60.0 / 50.0 fps; `tools/oracle.sh --all` counts unchanged on both versions |
| A5 | recording changes no behaviour | the same replay with `RC_FRAME_RECORD_AT=ring:300` (default renderer, MTVU on) follows the same scene log; a sampled `RC_SHADOW` run on level 17 with the tag channel on shows only the known artefacts |
| A6 | determinism | two `emu` replays, and one with `PS2X_GS_BANDS=1`, give identical picture hashes |
| A7 | the checks can fail | a flipped byte in one `PAGE`, a dropped `HW` event and a dropped `PAGE` each fail A1 or A2 at the right cycle; a truncated file replays up to its last whole cycle and is reported incomplete |
| A8 | tags | on a race cycle the inspector shows `Scene`, `View` (two in split screen), `Kick` labels on K1, K2 and the texture uploads, `Draw` spans whose anchors are chain slots holding call tags, `Hud` spans inside K1's byte range; begins and ends pair up; 0 unresolved outside the documented cases |
| A9 | budget | a measured table replaces 6.3: bytes per cycle by event type and scene, recorder time per cycle, ring memory |

### 10.2 Steps 5-8, off-line from the same dumps

| step | consumer | checks |
|---|---|---|
| 5 shadow texture memory (built: section 17) | `tex`: starts from a keyframe's `GSVR`, applies every host-to-local transfer of the PATH3 kicks through its own write swizzles | at every later keyframe and the final one, the shadow equals `GSVR` on all texture blocks (0x1e00 up) bit for bit, on dumps that include a level load (CT32 tiles), a race (PSMT8/PSMT4 animated frames) and the front end; decode against `tools/track_viewer` on the same level's BBK as planned; the alpha histogram (texels above 0x80) per level |
| 6 uploads into the frame buffer, flip (built: section 18) | `blit` | on cycles made only of uploads and a flip (logos, movie pictures, the splash) the picture it builds equals `REFI` bit for bit, on the CPU and through the RHI's read-back |
| 7 GIF reader and 2D | `gif2d` | (a) its register file after each PATH3 kick equals the next kick's recorded `regs`; (b) in front of the emulated GS (section 7): the picture equals `REFI` bit for bit, proving the parse; (c) Vulkan: K1 drawn over `--picture-before <cycle>:<K1>` (the emulated frame buffer just before K1, so the 3D needs no mask) against `REFI`, exact where the blend is integer-exact, otherwise within the bounds step 7's blend design states; the per-state pixel tests against the CPU blend model stay as planned. **Refined with pass criteria in `RENDERER_2D_DESIGN.md` section 10** (the depth buffer before K1 is seeded too; alpha and Z equal, colour within the blended-layer bound) |
| 8 VIF chain reader with the references | `vif` | (a) `walkChain` hashes (already A1); (b) per K2 kick the GIF bytes from the reader and references equal the emulated VU1's (`gifHash`, and byte for byte against `--path1`); (c) in front of the emulated GS: `REFI` bit for bit on levels 5, 17, 18, 20; (d) Vulkan pictures of the same cycles against `REFI` with difference statistics, feeding DIVERGENCES |
| 9 shader batches | `vk` with batches | against step 8's own pictures of the same dumps; `RamView::generation` shows which static pages ever change |

## 11. Risks

| risk | handling |
|---|---|
| The emulated replay is not bit-exact for a reason outside the record (hidden emulator state, thread-dependent rasterisation, MXCSR) | `EMUS` carries the emulator's whole graphics state; `gifHash` per kick separates "VU1 differs" from "GS differs"; a debugging switch `RC_FRAME_RECORD_VRAMHASH=1` adds a GS-memory hash after every kick to bisect; A6 tests thread independence first |
| Something reaches the GS outside channels 1 and 2 after all | `ODD` events at every such site make it visible in the dump and in every tool |
| A page changes between its capture and the runtime's read | impossible on one thread within one store; stated in section 8 so nobody moves the capture to another thread |
| Per-frame data is larger than estimated (polygon and alpha buffers unmeasured) | batch 1 measures before the writer's defaults are fixed; `REF` and `KEY` are switches |
| The software GS is too slow to reach a scene under exact mode | triggers start anywhere (`level:`, `scene:`), pad replays are keyed by game frame so slow runs stay aligned; scenes that need live play are caught with the ring in plain mode and replayed without references |
| Tags perturb the game | the four rules of 5.2, A4/A5, and the oracle build compiling them out |
| Tags attach to the wrong data when a buffer is reused before its kick | anchors resolve at the first visit only and expire after two cycles; the inspector reports unresolved and repeated anchors (A8) |
| The opaque `EMUS` layout changes with the runtime | `emuStateVersion` in the header; only `emu` needs it, every other consumer reads the plain chunks; old dumps are re-recorded |
| Recording through a level load in exact mode produces large dumps | `REF=hash` or `REF=10`; nothing else scales with it |

## 12. Build batches (Opus), each with its check

1. **Capture and container.** (Built 2026-11-19; section 13.) `ps2_capture_hooks.h` and the hook sites; `render/record` (format, page tracker,
   writer, reader, consumer interface, `walkChain`, capture with cycles and the `boot` / `cycle` / `game` / `level`
   / `scene` triggers, plain mode, keyframes with the plain state chunks); `rc_replay` with `stats`;
   `tools/frame_dump.py`; `.gitignore`. *Check:* a unit test that writes a synthetic dump and reads it back (no
   game); A1 and A4 on levels 17 and 18, the front end and boot, both versions; the measurements for A9's first
   table.
2. **Exact mode and the emulated replay.** (Built 2026-11-19; section 14.) `saveGraphics` / `loadGraphics` in the runtime and `EMUS`; register
   files per kick, `gifHash`, reference pictures, exact and final keyframes, `STRICT`; `tools/frame_record.sh`; the
   `emu` consumer with `--verify`, `--png`, `--picture-before`, `--path1`. *Check:* A2, A3, A6, A7 on the full dump
   list of 10.1, both versions.
3. **Side tags.** (Built 2026-11-19; section 15.) `render/gfx/tags`, the comparing switch in `rc_decomp.cpp`, resolution at the kick, `TAG` /
   `TAGP` / `TNAM`, the first sites of 5.3, the inspector's `tags`. *Check:* A8, A5's shadow half, A4 again (oracle
   counts on both versions), A1 and A2 still pass with tags in the dumps.
4. **Ring, hotkey, save states, write-up.** (Built 2026-11-20; section 16.) `ring:` and `key` triggers, F10 in both host loops, the state-load
   segment break, A5's ring half, A9's final table, `NOTES.md` ("Frame record": switches, tools, how to take and
   check a dump) and the measured numbers back into this file. *Check:* a ring dump taken by F10 during a default
   `play.sh` session replays through `stats --verify` and, taken in exact mode, through `emu --verify`.

Batches 1 and 2 are the dependency of steps 5-8; 3 and 4 can follow step 5 if the builds are busy.

## 13. Batch 1 as built (2026-11-19): changes to this design and why

Built: `third_party/ps2recomp/ps2xRuntime/include/ps2_capture_hooks.h` and its sites (`ps2_memory.cpp`: the kick with
its ranges, the GS privileged, VIF1 and GIF register stores, FIFO stores, PATH2, `submitGifPacket`; `ps2_runtime.cpp`:
`attach`, the store caller, the native GIF paths; `System.cpp`: `SetGsCrt`; `gs_frontend.cpp`: the register shadow,
`GS::writeRegister`, local->host; `ps2_save_state.cpp`: `snapshotGs`), `render/record/` (format, consumer, page
tracker, chain walker, writer, reader, capture, plain keyframe state in `emu_state`), `tools/rc_replay/` (`rc_replay`
with `stats`; `rc_record_test`, the synthetic round trip), `tools/frame_dump.py`, the `*.rcfr` rule in `.gitignore`.
How to use them: `NOTES.md`, "Frame record". Results of batch 1's check: `RENDERER_PLAN.md` step 4.

Changes, each the smallest that worked:

1. **Consumer interface** (4.2): `pages()`, `note()` and `hw(..., callerRa)` added. The writer is a consumer (6.1) and
   could not write `PAGE`, `NOTE` or a store's caller without them.
2. **Two more hooks** (4.1): `attach(PS2Runtime *)`, called at the end of `PS2Runtime::initialize`, because the capture
   needs the runtime for keyframes and is installed before it exists; `gameFrame(frame, levelFrame, scene)`, called by
   the `GfxP2_SwapBuffers` override, because the `game:`, `level:` and `scene:` triggers need the game's counters and
   the `Scene` tag that will carry them comes with batch 3. The override now counts frames since the level start in
   every run (the RAM dump's semantics are unchanged) and passes all ones until a level has started, so `level:n`
   cannot fire during boot or the front end.
3. **Nothing added to `GS`, `PS2Memory` or `PS2Runtime`** (4.1): their headers are included by every recompiled file
   (via `ps2_runtime.h`), so adding a member or a method rebuilds the whole game twice over. The hook table, the
   store caller (`g_storeCallerRa`) and the GS register shadow are globals declared in `ps2_capture_hooks.h`, and the
   keyframe's GS copy is a static member of the save-state code's existing friend struct (`GSStateAccess`).
4. **`GSRG` from a register shadow** (3.4): `GS::getDebugSnapshot()` holds the registers decoded and not all of them (no RGBAQ,
   ST, UV, PRMODECONT, FOG or TEX2), so a re-encoding could not give the values written. The GS
   front end now stores every register write in `ps2x::capture::g_gsRawRegs[0x63]` (one store per write, always on;
   PACKED RGBAQ/ST/UV/XYZ/FOG re-encoded in their register form, Q of RGBAQ from the last PACKED ST). Batch 2's
   per-kick register files read the same array.
5. **Keyframe GS memory as a save state takes it** (3.4): `Flush` then `SnapshotVram`, no `Sync(DebugReadback)`, and
   the backend-lifetime mutex taken before the state mutex. The first version took them the other way round (as
   upstream's `GSStateAccess::serialize` does) and deadlocked within one frame against the presentation latch
   (`GS::latchHostPresentationFrame`, main thread), which takes lifetime then state. Upstream's save-state code keeps
   its order and can in principle deadlock the same way (not changed here; F5 has not been seen to hang).
6. **Cycle close** (3.1): a 64-bit store to DISPFB2 or a 32-bit store to its low word; a store to the high word alone
   does not close a cycle. All recorded closes were 64-bit stores.
7. **Tags in `kickTouch`** (4.1): every tag is reported as its 16 bytes, also when only its upper 8 enter the stream or
   none do: the replay's walker reads the whole tag.
8. **A1's "no ODD event"** (10.1): both boot dumps hold exactly two `ODD` events, the survey's two start-up stores
   into the VIF1 FIFO (`GfxP2_Init`, before `sceGsResetGraph`, whose VIF1 reset is recorded as `HW`). They are counted
   apart while the dump has shown no VIF1 kick; any other `ODD` fails `--verify`. Their 16 data bytes are not in the
   record (`ODD` keeps an address and 64 bits); since VIF1 is reset after them, a replay from boot does not need them.
9. **Default trigger** (6.2): `boot+120` until batch 4 brings `key`. `RC_FRAME_RECORD_REF` and `RC_FRAME_RECORD_STRICT`
   are not read yet (batch 2).
10. **Format details fixed by the code** (6.4, normative in `render/record/format.h`): `FileHeader` is 144 bytes (a
    4-byte pad before `indexOffset`); `KEYF` is `KeyframeHeader` (32 bytes) then each state chunk with its own chunk
    header (compressed one by one); `GSPR` is the runtime's 20-register order (160 bytes), `VU1R` 624 bytes, `VIF1` the
    runtime's `VIFRegisters` plus a pad (96 bytes); `INDX` is `{segmentCount, cycleCount}` then the segments and cycles.
    A dump that was not closed has no index; the reader rebuilds it from a scan and reads each segment's first cycle
    number from its first `END`, so a cut dump still starts at the right segment.
11. **Recorder time** (3.2, 6.3): 2.6-6.8 ms a cycle, not 1-2 ms; the estimate left out the stream hash. Acceptable for
    a development tool; if the ring of batch 4 is to stay on during play, hashing only on demand or a cheaper hash for
    the flattened chain are the first savings.
12. **`callerRa` of native stores**: kicks and stores that native code makes through `hw_write32` carry the running
    guest context's `$ra`, which says nothing about the native caller (most K1 and upload kicks show 0xfffffff0). Batch
    3's `Kick` labels are what identifies those kicks.

## 14. Batch 2 as built (2026-11-19): changes to this design and why

Built: `ps2x::savestate::saveGraphics` / `loadGraphics` / `graphicsStateVersion` and `ps2x::capture::exactCapable`
(`ps2_save_state.cpp`, declared in `ps2_capture_hooks.h`); `EMUS` in every keyframe and `loadKeyframeState`
(`render/record/emu_state.cpp`); exact mode in `render/record/capture.cpp` (register file and GIF hash per kick, the
reference picture per cycle through `render/record/picture.h`, exact keyframes, `RC_FRAME_RECORD_REF`,
`RC_FRAME_RECORD_STRICT`); `tools/frame_record.sh`; the `emu` consumer (`tools/rc_replay/emu.cpp`) with `--verify`,
`--png`, `--picture-before`, `--path1`, `--kick-hashes` and `--fault`. Usage: `NOTES.md`, "Frame record". Check
results: `RENDERER_PLAN.md` step 4. The emulated replay was bit-exact at the first try on every dump: no hidden state had
to be hunted, so the bisection aid `RC_FRAME_RECORD_VRAMHASH` (a recorder-side GS memory hash per kick) was not built;
`rc_replay --kick-hashes` prints the replay's side if it is ever needed.

Changes, each the smallest that worked:

1. **`EMUS` without GS memory** (3.4): `loadGraphics(runtime, blob, vram)` takes GS memory from `GSVR`, so a keyframe
   does not store 4 MB twice. `EMUS` is written in plain mode too (cheap); `emu` replays any dump with it and verifies
   only exact ones.
2. **A GIF tag in progress is in `EMUS`** (3.4): upstream's save state (`GSStateAccess::serialize`) does not know the
   GS front end's `m_gifProgress` (a local change: a GIF tag continuing across DMA transfers). `saveGraphics` adds it;
   the save-state format is unchanged. `EMUS` also holds the privileged registers, the register shadow, VU1 memory and
   registers, VIF1 and DMA registers, the PATH3 mask and its FIFO, pending transfers and the I/O register map. Locks
   are taken in the presentation latch's order (13.5).
3. **`KICK` at `kickDone`** (3.5): the GIF hash is only known after the runtime processed the stream, so the record is
   emitted then (both modes; plain mode simply has no hash). Events from the processing itself (a `PATH2` `ODD`) come
   between the kick's `PAGE` events and its `KICK`.
4. **`KICK.fbrst` from the running guest thread's context** (4.2): batch 1 recorded `runtime.cpu().vu0_fbrst`, but
   the runtime's VU1 start (MSCAL) reads the current guest thread's context; the recorder now reads the same.
5. **Exact mode also needs `RC_TRACK_SHADER` off** (3.5): with it, four VU1 programs are replaced by the track
   shader's paths, so the kicks' GIF output is not what a replay's VU1 makes. `tools/frame_record.sh` sets it to 0
   and `RC_VU1_REF=0` (the reference checks only observe, but cost time), plus `RC_FRAME_RECORD_STRICT=1`.
   `tools/play.sh` now honours `PS2X_GS_GPU`, `PS2X_GS_HW` and `PS2X_GS_THREAD` from the environment (defaults
   unchanged) and takes a pad path for `replay`.
6. **How `emu` applies events** (7): a GS privileged store sets the recorded register value (the value after the
   store; CSR directly, since its write-one-to-clear bits would change a written value); VIF1 and GIF stores go
   through `write32` as the game's did; `CRT` is not applied (it changes no GS memory). A kick sets MADR, QWC, TADR,
   ASR0/1 in the register map, forces the DMAC enable bit (the recorder sees only kicks that passed that test), sets
   MXCSR and the VU0 FBRST, then stores CHCR. The picture is read at the replayed machine's own DISPFB2 (and its FBP
   checked against `END.refFbp`), so a wrong DISPFB2 store is caught. `emu` always replays from the segment's keyframe;
   `--cycles a-b` only limits the pictures it writes.
7. **More checks than planned** (7): the GS register file before every kick is compared with `KICK.regs` (it names the
   first kick after a divergence even when that kick's own output is still equal), and the register file at keyframes
   with `GSRG`. `--fault page|kick|hw:<cycle>[:<n>]` plants a fault inside the tool (a flipped byte in the cycle's first
   page, a dropped kick, a store with bit 0 flipped) instead of editing a dump. A flipped byte is only caught when it is
   part of a stream (3.2): bytes 16 and 2048 of the Europe movie's first page are not read by any kick and replay
   clean, byte 3000 fails.
8. **The headless machine** (7): `rc_replay` sets `PS2X_GS_THREAD=0 PS2X_GS_GPU=0 PS2X_GS_HW=0 PS2X_MTVU=0` before the
   runtime exists, registers the generated VU1 programs itself (`registerGeneratedVu1Programs`, as the game's
   overrides do) and defines the runtime's recompiled-function table as empty (no game code is linked).
9. **Reference pictures are larger than estimated** (6.3): a stored `REFI` is 280-400 KB with zlib level 1 (race and
   front-end pictures 300-400 KB), not 0.1-0.2 MB. A 120-cycle race dump with every picture is 57-74 MB, the 900-cycle
   boot with `REF=10` 104-114 MB, the 400-cycle level load with every picture 188-204 MB.

**Measured (exact mode, 2026-11-19, both versions, USA / Europe).** `KICK` records with register files: 32 KB raw a
race cycle (37 kicks), 9-12 KB a boot cycle. `CYCL` stored: race 86-191 KB a cycle, front end 110-126 KB, boot 82-93 KB,
Track Editor 7 KB. Recorder time on the EE thread (kick hooks, the picture read included): 3.0-6.1 ms a race cycle,
3.1 / 3.6 ms front end, 2.1 / 2.7 ms boot, 0.6 ms Track Editor; largest cycle 8-16 ms. The game ran close to real time
in exact mode: a 120-cycle race dump with the level load took 42-64 s. `rc_replay emu` replays a 120-cycle race dump in
13-18 s, boot (900 cycles) in 38 s.

## 15. Batch 3 as built (2026-11-19): changes to this design and why

Built: `render/gfx/tags.{h,cpp}` (the `rc::gfx` channel; payloads, enumerators and the name table in the header,
normative), `decomp/rc_gfx_tags.cpp` (the game's two anchor readers), the comparison setters in `decomp/rc_decomp.cpp`,
the capture as the channel's consumer (`render/record/capture.cpp`: resolution at the kick, labels, expiry, `TAGP`,
`TNAM`, the `kFileTags` flag), `TNAM` in the writer and `TNAM` / `TAGP` in the reader, the first sites (5.3), the
statistics and checks of `rc_replay` and `frame_dump.py tags`. Usage: `NOTES.md`, "Frame record". Check results:
`RENDERER_PLAN.md` step 4. Nothing found conflicts with option C or the plan's architecture.

Changes, each the smallest that worked:

1. **`RC_GFX_TAGS` per target** (4, 5.2): the native and override sources compile into both `rc_revenge_pro` and
   `rc_oracle`, so the switch is a generator expression on those sources (`RC_GFX_TAGS=1` only when the consuming
   target is the game) and the header defaults to 0. It changes only the native and override compile lines; the
   recompiled code is untouched. `rc_oracle` (both versions) holds no `rc::gfx` symbol.
2. **The channel knows no game layout** (4, 5.2): `tags.cpp` sits in `rc_record`, which cannot include the
   per-version globals, so the game registers how to read the 2D buffer and chain-slot anchors
   (`setAnchorReader`), and the capture attaches as a `Sink`. The channel is attached while `RC_FRAME_RECORD` has
   installed the capture (from start-up, so tags are resolved and expired while the trigger waits) and detached when
   the dump closes; `RC_FRAME_RECORD_TAGS=0` keeps it off.
3. **Both comparison flags** (5.2 rule 3): the native half of a live comparison runs with `g_comparing` false and
   `g_inComparedNative` true; both go through setters and the channel's "comparing" is either.
4. **Resolution rules** (5.1): a DMA tag's range matches only its own address; a payload range matches up to and
   including its end, so an end mark after the last packet (every HUD panel's end is the end of K1) and an empty span
   at the end resolve; first visit in stream order wins; a second visit inside another range counts as a repeat.
5. **Out-of-order ends** (5.1): guest threads can switch inside a blocking call made within a span, so `end(token)`
   closes that span wherever it is in the open list; `depth` is the number of open spans, `parent` the latest open.
6. **Payload layouts fixed** (5.3, values in the header): `View` also carries the camera's guest address; `Draw`
   carries a has-matrix word and the 3 x 4 matrix as the object's rotation rows then its position; a 2D begin with a
   HUD descriptor is one `Hud` begin (key and element), not a `Draw` and a `Hud` tag. `Scene`'s id is the override's
   (no movie bit), as the `scene:` trigger uses.
7. **Sites** (5.3): `View` in `MainGameProcess` right after the second `SetFrameWindow` of each view (one per view
   and frame; `SetFrameWindow` itself runs twice per view and once for the full-screen reset). A `flip` label in
   `GfxP2_SwapBuffers` around `sceGsSwapDBuff`: without it the draw-environment kick that opens every cycle was the
   one unlabelled kick of a race; its span stays open across the cycle close the DISPFB2 store inside it makes, so its
   end tag is in the next cycle. `HudTopLeft` draws nothing and has no span. The font function is `FontPrint`.
   `pass`, `beginAt`, `label` and `Cut` have no site yet.
8. **Europe** (5.1, case 2): `MainGameProcess`, the HUD panel functions, `GfxTex_Update` and `BackdropDisplayBMP24`
   run recompiled there, so its dumps have no `View`, no `Hud`, no animated-texture label and no backdrop label (its
   two backdrop uploads per screen and the recompiled `GfxTex_ManageTexture`'s uploads are unlabelled). `Scene`, K1
   (`GfxP2_RenderDmaSendActual` has a variant), K2, the flip, the texture uploads, movie pictures and the chain and
   font spans are there.
9. **Unlabelled kicks left** (10.1 A8 asks labels on K1, K2 and the texture uploads): in races none. During level
   loads (front end and boot dumps) the palette uploads of `GfxTex_ManageTexture` and the 64 x 64 level-image tiles of
   `BBLoad` (`bb.cpp`, `uploadTile`): 334 of 4,474 kicks in the USA front-end dump, 572 of 9,151 in USA boot. They
   are texture construction, not on the first-site list; labelling them is two more `SpanKicks` when a consumer needs
   it.
10. **Demo races draw only the panel's "DEMO"** (10.1 A8): in the attract demo `HudPanel` returns after the flashing
    `DEMO` (and the off-screen "." that sets the font's packet state up), so the dumps' `Hud` spans are the panel's;
    the corner panels need a real race, which no check dump has.
11. **`TAGP` is not replayed as events** (5.3): a reader adds its tags to the tag store so a segment's first kicks
    find them, but does not deliver them again through `Consumer::tag`.
12. **More checks than planned** (7, 10.1): `rc_replay --verify` also checks that every span's stream offset delivers
    the guest address its tag names (by the host's walker) and pairs every begin with its end in the same kick;
    `stats` prints tags by kind, kicks by innermost label, spans by tag class and kick, and anchored tags left
    unresolved (those of the last two cycles of a dump are not judged).

**Measured (exact mode, 2026-11-19).** `TAG` records, raw, a cycle (USA / Europe): level 17 4.7 / 3.7 KB (88 / 74
tags), level 18 6.8 / 4.6 KB, front end 4.1 / 5.8 KB; they compress inside `CYCL` with the rest. Recorder time with the channel on: 3.5 / 3.2 ms a cycle level
17 (USA / Europe), 5.4 / 5.3 level 18, 3.0 / 4.3 front end, 2.2 / 2.6 boot: within batch 2's 3.0-6.1 ms. Resolution
found every anchored tag in every dump: 0 dropped unconsumed, 0 repeats, 0 unanchorable.


## 16. Batch 4 as built (2026-11-20): changes to this design and why

Built: the `key` and `ring:<n>` triggers and the save-state segment break in `render/record/capture.cpp`; the ring in
the writer (`render/record/writer.{h,cpp}`, `Writer::Ring`); F10 in both host loops and the `stateLoading` /
`stateLoaded` calls (`third_party/ps2recomp/ps2xRuntime`: `ps2_runtime.cpp`, `ps2_save_state.cpp`,
`include/ps2_capture_hooks.h`); `rc_replay`'s handling of `afterStateLoad` keyframes; the ring round trip in
`rc_record_test`. Usage: `NOTES.md`, "Frame record". Nothing found conflicts with option C or the plan.

Changes, each the smallest that worked:

1. **`key` is the default trigger** (6.2, 13 item 9): as designed; batch 1 used `boot+120` only until `key` existed.
   A run with `RC_FRAME_RECORD` set and no `RC_FRAME_RECORD_AT` now records nothing until F10; every script and
   example names its trigger.
2. **The ring is kept compressed** (6.1, 6.3): the writer's worker thread encodes each cycle and keyframe as it does for
   a file (zlib level 1) and keeps the finished chunks in memory instead of writing them, so the EE thread does exactly
   what it does in file mode and the ring of 300 cycles holds 26-45 MB, not the 220 MB estimated for raw records. A
   segment (a keyframe chunk and the chunks after it) is dropped from the front while the others still hold n cycles,
   so a dump holds n to n + `KEY` cycles. Its default segment length is 120 (6.2), so at most 120 cycles are kept beyond
   n.
3. **F10 in ring mode** (6.1): at the cycle close after the press the EE thread takes a final keyframe (the only work
   it does) and queues a dump request behind the cycles already queued; the worker then writes the header, the name
   table, the ring's chunks unchanged, the final keyframe, the index and `ENDF`, and the ring goes on. The final
   keyframe is kept apart from the ring, so a later dump is still made of whole segments.
4. **F10 reaches the capture as a counter** (4.1): the `hotkey` hook (main thread) only increments an atomic; the EE
   thread reads and clears it at each cycle close. Starting, stopping and dumping therefore happen at cycle boundaries
   and the game thread pays one atomic exchange per displayed picture. `platform/vk_host.cpp` needed no change: F10 is
   read in `runHostPlatformLoop` through the platform's `keyPressed`, as F5/F9 are.
5. **The state-load break closes the cycle before the load and keyframes right after it** (3.4): one hook after the load
   could no longer read the picture the cycle in progress ended on (GS memory is already replaced), and a keyframe at
   the next cycle close would leave that cycle's kicks drawing on GS memory the replay does not have. So the hook table
   gained `stateLoading` (last in the table, so earlier positional initialisers keep their meaning), called in
   `EeScheduler::serviceStateRequests` once the file is read and checked, just before `serializeMachine`: the capture
   closes the cycle there (an `END` with the picture the display shows, a `NOTE` saying why). `stateLoaded` follows the
   load (also a failed one), still on the EE thread before any guest code runs, and takes a keyframe flagged
   `afterStateLoad`. `rc_replay emu` loads that keyframe without comparing GS memory against its replay of the cycles
   before (they drew the old machine), `stats` and `frame_dump.py` name it.
6. **Numbered dumps** (6.2): `%d` takes the dump's number in the run; a name without `%d` keeps the first dump's name
   and gets `-1`, `-2`... before its extension for the next ones, so a second F10 never overwrites the first dump.
7. **A test trigger**: `RC_FRAME_RECORD_F10=<cycle>[,<cycle>...]` acts as a press at the close of those display cycles,
   for scripted checks; the real key was proved once per host loop with a uinput keyboard.
8. **Ring status lines**: at each new segment the game logs the last segment's bytes a cycle by record type, raw and
   stored, the recorder's time and the ring's memory; A9's table below is read from them.
9. **Save-state lock order (runtime)**: upstream's `GSStateAccess::serialize` took the GS state mutex before the backend
   lifetime mutex, the opposite of the presentation latch (13 item 5 noted the risk). With the recorder in exact mode a
   timed save (`PS2X_STATE_SAVE_AT`) deadlocked against the latch in 2 runs of 3; it now takes them in the latch's
   order, and 3 runs of 3 saved.
10. **Open: a stall after a save-state load with the recorder in exact mode.** `playtest2` in exact mode with a timed
    save at field 1700 and load at 2000 (both in the front end) stopped advancing emulated time at game frame 404-405
    after the load in 2 of 6 runs with `ring:300` recording (the EE thread busy at 100%, the debug dump still writing);
    0 of 6 runs without `RC_FRAME_RECORD` and 0 of 3 with the capture installed but idle (`key`, never pressed) stopped.
    Not found: the recorder's state-load work only reads (it closes a cycle, then takes a keyframe as at any segment
    start), and the same load in the level 17 attract demo, recorded, replayed bit for bit on both versions (A2 below).
    Upstream's save state also leaves the GS front end's GIF tag in progress out (14 item 2) and cannot restore native
    code's host frames, so a load is timing-sensitive in this port anyway; left for a later look.

**Checks (2026-11-20, RTX 2060).** Real F10 through a uinput keyboard, `ring:300`, `tools/play.sh replay playtest2`
(USA): raylib loop, the press at 44 s, a dump of 417 cycles (1440-1856, 4 segments, 42.5 MB); `RC_RENDERER=vk`, 411
cycles (1440-1850, 42.3 MB; `[vk-host]` logged keyboard focus, 0 validation errors). Both: `rc_replay --verify` PASS,
`frame_dump.py verify` ok. Simulated F10 (`RC_FRAME_RECORD_F10`) in default play, `rc_replay --verify` PASS: USA
cycles 1200-1499 (300, 12,300 kicks) and 2040-2399 (360, 14,760 kicks); Europe 1200-1499 (300 cycles through the
level load, 6,382 kicks) and 2160-2499 (340, 13,940 kicks). Exact-mode ring dumps by F10, `rc_replay --consumer emu
--verify` PASS: level 17 attract demo with a save at field 1200 and a load at 1500, USA cycles 360-699 (the load at
cycle 596; 12,543 kicks, 340 picture hashes, keyframes 1/1 compared and the one after the load loaded) and 596-999
(16,024 kicks, 404 pictures, keyframes 3/3, final 1/1); Europe 360-699 (load at 505; 13,345 kicks, keyframes 2/2) and
625-999 (13,875 kicks, 3/3). A1/A2 on normal (`key+100`) dumps, exact mode, both versions: a dump stopped by the
second F10 after 30 cycles and one ended by the count (100 cycles, 1,100 kicks), `stats --verify`, `emu --verify` and
`frame_dump.py verify` pass on all four. `rc_record_test` PASS (with the ring round trip). A5's ring half and A4:
`tools/play.sh replay playtest2`, 180 s, with `ring:300` and without the recorder give the same scene logs (USA frames
1, 5, 657, race at 658, clock 1:0; Europe 1, 5, 1166, race at 1167) at 60.0 / 50.0 fps; `tools/oracle.sh --all`
Europe seed 9495 1563 ok, 0 FAIL; USA seed 9496 1631 ok, 0 FAIL, 1 skipped (`PathBuilder` timed out); USA seed 9494
one FAIL (`AMenuDisplayMIT`) that the runtime before this batch gives too (NOTES.md).

## 17. Step 5 as built (2026-11-20): shadow texture memory and decode

Renderer plan P1 step 5, test plan 10.2's `tex` row. Built: `render/tex/` (library `rc_tex`, namespace `rc::rd`),
`rc_replay --consumer tex` (`tools/rc_replay/tex.{h,cpp}`), `rc_tex_test` (`tools/rc_replay/tex_test.cpp`),
`tools/tex_check.py`. Usage: `NOTES.md`, "Shadow texture memory and decode". Nothing of it is linked into the game or
the oracle yet (the live reader of steps 6-9 will link `rc_tex`), the runtime is untouched, and so is every game
function: recording off and normal play cannot have changed. Nothing found conflicts with option C.

### 17.1 Design

```
render/tex/gs_layout.{h,cpp}    where a pixel of a buffer lives in GS memory: block and column tables, CT32/24, CT16,
                                CT16S, T8, T4 (no dependencies)
render/tex/gs_memory.{h,cpp}    GsMemory: the 4 MB, host-to-local transfers, per-block write serials
render/tex/gif_walk.{h,cpp}     GifWalker: GIF tags (PACKED, REGLIST, IMAGE; a tag may span packets) -> register
                                writes in register form and image data, through a GifSink
render/tex/tex_decode.{h,cpp}   Tex0, Texa, Clut (the GS's CLUT buffer and CLD), mipLevel (MIPTBP1/2), decodeTexels
render/tex/tex_cache.{h,cpp}    TextureCache: RGBA8 per texture, decoded again only when its blocks changed
render/tex/tex_shadow.{h,cpp}   TexShadow: the rc::rec::Consumer that walks channel 2 kicks into GsMemory
```

- **Layout.** Written from the GS's arrangement (32 blocks of 256 bytes a page, four 64-byte columns a block), the
  column order as four small formulas, not copied from the runtime's `GSMem` tables: the shadow is checked against the
  emulated GS's memory, which only means something if the two were made independently. `rc_tex_test` compares the two
  address by address as well.
- **Transfers.** `GsMemory::writeRegister` takes BITBLTBUF, TRXPOS, TRXREG and TRXDIR (a TRXDIR of 0 opens a transfer:
  DBP, DBW (0 read as 1), DPSM, DSAX/DSAY, RRW x RRH pixels row by row), `imageData` the pixels: CT32 four bytes, CT24
  three (the fourth byte of memory kept; a pixel may straddle packets), CT16/16S two, T8 one, T4 two pixels a byte, low
  nibble first, an odd rectangle dropping the last high nibble. Data after the last pixel is ignored; a TRXDIR of 1 or
  2 (local to host, local to local: never seen, survey) is counted and not performed. Uploads into frame memory are
  applied like any other (cheap, and step 6's blits read them back from there if it wants); draws never write the
  shadow, so only blocks 0x1e00 up are comparable with the GS.
- **Dirty tracking.** Every transfer has a serial number and every block the serial of the last transfer that
  *changed* it: a pixel written with the value it already holds changes nothing. That matters: the game uploads its
  animated textures again every frame (17-19 transfers, 100-124 KB, in the level 17 and 2 dumps), but they hold a new
  animation frame only every third cycle, so two cycles in three dirty no block at all. A cache entry keeps the serial
  it was decoded at and the blocks of its texels and palette; a lookup when memory's serial has not moved is one
  comparison, otherwise a scan of the entry's blocks (at most 512), and a decode only if one is newer. Each decode
  bumps the entry's `generation`; the Vulkan uploader of step 9 re-uploads the entries whose generation it has not
  seen, which makes the upload incremental with no further bookkeeping.
- **The cache key** is what decides the texels: TBP, TBW, format, the level's width and height, and for T8/T4 CBP,
  CPSM, CSM and CSA; TEXA is part of it only where a format takes alpha from it (CT16, CT24, 16-bit palettes). TCC,
  TFX and CLD are not. Entries are made at the first draw that names them and live until `clear()`: memory under an
  entry can come to hold something else (the next level's image), which costs nothing while no draw names it; when to
  drop entries is step 9's.
- **CLUT.** `Clut` is the GS's 512-entry buffer: a TEX0 write loads it as CLD says (1 always, 2/3 load and remember
  CBP0/CBP1, 4/5 load when CBP differs from the remembered one, 0 keeps it), CSM1 entries at their swizzled places in
  the 16x16 or 8x2 image, CSM2 from TEXCLUT's row; lookups add CSA. The cache takes a `Clut` when the reader has one
  that may differ from memory, and otherwise reads the palette from memory as it is at the draw, which is exactly what
  a CLD 1 write does and is all RC uses. Two things follow the emulated GS rather than a hardware reference, because
  only it can be checked against and RC reaches neither: the buffer is loaded only when PSM is T8/T4, and an 8-bit
  texture's palette is loaded in place from entry CSA * 16 up.
- **Decode** gives 4 bytes a texel, R, G, B, A with the GS's raw alpha (0x80 = 1.0): what the texture function takes.
  CT16 and CT24 take alpha from TEXA (bit 15 clear: TA0, or 0 when AEM and the colour is black; set: TA1), 5-bit
  channels go to the top of 8 bits. Mip levels are textures of their own at MIPTBP's place and half the size.
- **`GifWalker`** delivers every register write in register form whatever the packet's form (a PACKED RGBAQ with the
  Q of the last ST, a PACKED XYZ2 with its ADC bit as XYZ3), so the transfers, the usage scan of the `tex` consumer
  and step 7's reader can share it. PRE writes PRIM only in PACKED form, as on the GS (the emulated GS writes it in
  any form; no dump had a tag where that differs, see the counter in 17.2).
- **`TexShadow`** is a `Consumer`: on a channel 2 kick it walks the stream with `walkChain` (normal mode or chain, as
  the movie pictures are) and feeds the payload ranges to the walker. Live it will sit in the capture's consumer list
  on the EE thread and read guest RAM in place; off-line `rc_replay` drives it. It follows PATH3 only: every transfer
  of RC is on channel 2 (survey), and the check below confirms it (the transfer registers before every kick, and
  texture memory after the VIF1 kicks).

### 17.2 Checks and results

`rc_replay --consumer tex --verify` does 10.2's check: the shadow starts from the first keyframe's `GSVR` (and
the transfer registers of `GSRG`), applies every transfer of the channel 2 kicks, and at every later keyframe and the
final one must equal `GSVR` on all texture blocks (0x1e00 up: 2,228,224 bytes) and hold the keyframe's BITBLTBUF,
TRXPOS, TRXREG and TRXDIR; a keyframe after a save-state load is loaded, not compared. On exact dumps it also needs
the recorded four transfer registers before *every* kick (the shadow follows PATH3 only, so a transfer register
written on another path would show there). Two things were added to what 10.2 asked, both through `--with-emu`, which
runs the consumer behind `emu` (the `emu` consumer gained a tap that hands every GIF packet of a replayed kick, with
its path, to the consumer behind it):

- **texture memory after every kick** against the emulated machine's GS memory, the VIF1 kicks included (draws must
  not write texture memory); the first difference is named by cycle and kick;
- **the decode against the emulated GS's own sampling.** Every texture a draw of any path names is decoded through
  the cache (PATH3's from the shadow's own walk, in order with its transfers; PATH1's from the emulated VU1's
  packets), mip levels too. After the last cycle each cache entry is decoded again and drawn by the emulated GS: a
  point-sampled DECAL sprite (TCC 1, CLD 1, TEX1 nearest, level 0, UV at the texel centres) of the texture, one texel
  to a pixel, into the emulated frame buffer at FBP 0, read back through the CT32 layout. Decoded texel and drawn
  pixel must be equal in all four bytes. That covers the layout on the read side, the CSM1 palette order, CSA, the
  CLD 1 load, TEXA and the 16-bit widening with nothing of the emulated GS's internals touched.

**Dumps.** Step 4's dumps hold at most two segments and little loading, so new ones were taken in exact mode
(`RC_FRAME_RECORD_REF=hash`, RTX 2060), 27 per version, in `build-decomp/frame-dumps/s5/` and
`build-eu/frame-dumps/s5/` (2.5 and 2.3 GB): `boot` (`boot+900`: logos and the publisher movie as uploads into
frame memory, the title, the front end, the first attract demo), `front` (`RC_SKIP_INTRO=1`, `boot+900`: the front
end from its load, then an attract demo), `l1`-`l24` (`RC_BOOT_LEVEL=n`, `boot+700`: the level load with its splash
(CT32 tiles, palettes, single textures), then the demo race with its animated textures), `tedit` (the Track Editor,
`tools/tedit_walk.py 1 7000` pad, `game:1900+700`). Each has keyframes every 300 cycles, so two later keyframes and
the final one. One more, USA only, outside the table: `l17long` (`RC_BOOT_LEVEL=17`, `boot+2700`: 45 seconds of the
demo race, 284 MB): PASS both ways, 104,366 kicks, later keyframes 8 of 8 and the final one, every kick's registers
and texture memory, 619 of 619 textures (1,527,040 texels) equal to the emulated sampling.

| check | USA (27 dumps) | Europe (27 dumps) |
|---|---|---|
| `tex --verify` alone and `tex --verify --with-emu` | PASS on all 27, both ways | PASS on all 27, both ways |
| cycles / kicks replayed / transfers applied | 19,300 / 681,563 / 792,705 | 19,300 / 707,323 / 865,993 |
| texture blocks equal `GSVR` at later keyframes; at the final one | 54 of 54; 27 of 27 | 54 of 54; 27 of 27 |
| transfer registers before a kick equal the recorded ones | 681,563 of 681,563 | 707,323 of 707,323 |
| texture blocks equal the emulated GS's after a kick | 681,563 of 681,563 | 707,323 of 707,323 |
| decoded textures equal the emulated GS's sampling | 11,433 of 11,433 (32,117,216 texels) | 13,182 of 13,182 (35,962,336 texels) |
| the `emu` consumer's own verification in the same runs | every GIF hash, register file, picture hash, GS memory | the same |

The textures compared with the emulated sampling are T4 and T8 through CT32 palettes, CT32, the mip levels of the two
mipmapped textures (levels 2 and 14: 32x32 to 4x4 and 32x16 to 8x4) and the one CT16 texture (128x128, the HUD's
dial, alpha from TEXA 0x80/0/0). Counters that stayed at zero in all 54 dumps: transfers in a format outside the
list, local-to-host and local-to-local transfers, transfers cut off by the next TRXDIR, image data with no transfer
open, packet bytes too short for a tag or an item, GIF tags with PRE outside PACKED form, TEX2 writes, textures in a
format the decoder lacks, pages the dump lacks. What the transfers were (level 17, USA, 700 cycles): 11,980 CT32
(44.2 MB, 8,880 of them the splash's tiles into frame memory), 9,096 T8 (35.5 MB), 72 T4; `boot`: 161,789 CT32
(165.7 MB; 161,456 into frame memory: logos and movie pictures), 364 T8, 2,664 T4; levels 4, 10, 16 and 22 (USA) upload
animated T4 textures every frame (4,240-4,968 T4 transfers, 9.5-12.3 MB). No dump has a CT16 or CT24 transfer: the
CT16 texture's memory arrives in another format.

**Step 4's own dumps** (`build-decomp/frame-dumps/`, `build-eu/frame-dumps/`, 22 dumps): `tex --verify` PASS on all.
The exact ones give later keyframes 4 of 4 and final 4 of 4 (USA: `b4-key`, `b4-key-1`, `b4-x2-0`, `b4-x2-1`), 5 of
5 and 4 of 4 (Europe). The 14 plain dumps (recorded with the hardware GS backend, `vramExact` 0) are compared too and
reported apart: their `GSVR` equals the shadow on the texture blocks at all 32 keyframes (USA 20, Europe 12), so a
GPU backend's memory image is exact there as well.

**Decode against the track viewer** (`tools/tex_check.py`): for every `BB/<n>/<n>.BBK` with a world, every texture
the world's and the placed objects' texture lists name is decoded by `tools/bbk_export.py` (the track viewer's
decoder; numpy, the runtime's tables) and by `render/tex` (the file's GS memory image uploaded as the game uploads it,
64x64 CT32 tiles from block 0x1e00, then `decodeTexels`): USA 8,980 textures of 49 level files (the 48 levels and the
front-end world), Europe 9,004 of 49, all equal byte for byte (CT32, T8 and T4).

**`rc_tex_test`**: PASS, 635 checks: each format's page is a permutation; 1,783,040 addresses (every pixel of a page
at each of the 32 block offsets, and 200,000 random buffers a format) equal the runtime's tables
(`decomp/generated/gs_tables.bin`: CT32, CT16, T8, T4); 600 random uploads in six formats (4.0 M pixels) through the
walker in two pieces read back and leave memory equal to a model placed by the runtime's tables; the walker's PACKED,
REGLIST and IMAGE forms across packet boundaries; CLD 0, 1, 2, 4, 5 and a 4-bit palette at CSA 2; decode of T8, T4,
CT16 with TEXA and AEM, mip levels from MIPTBP1; the cache (a write elsewhere keeps an entry, a write into its texels
or its palette decodes it again, the reader's CLUT buffer wins over memory, the alpha summary).

**Pictures.** `--png` on `b4-key-1` (the race menu) beside the cycle's reference picture: the font sheets, the button
icons, the arrows and the panel pieces of the menu, with the glyphs in the alpha channel (0 where the glyph is: the
game's inverted alpha); on level 2 the CT32 textures with their mip chains and the CT16 dial. Looked at once; the
bit-exact checks above are the proof.

**The checks can fail** (`--fault`, level 17 of each version, 700 cycles, keyframes at 300 and 600):

| planted | USA | Europe |
|---|---|---|
| 32-bit column table, two entries swapped, from the start | keyframe 300: 28,976 bytes in 5,235 blocks; 600 and final fail too | 29,020 bytes in 5,236 blocks; the same |
| 8-bit table, from the start | 300 (760 bytes, 380 blocks), 600 and final fail | 300 (744 bytes, 372 blocks), 600 and final |
| 4-bit table, from the start | 300 fails (196 bytes, 98 blocks: T4 is uploaded only at the load here), then clean | 300 and final fail |
| 8-bit table from cycle 350 | 300 passes, 600 (134 bytes, 67 blocks) and final fail | the same |
| one load-time transfer loses its data (`transfer:4:100` USA, `transfer:2:100` Europe) | 300 fails: 1,000 bytes in 4 blocks at block 0x2e4c | 997 bytes in 4 blocks |
| the shadow skips a pixel kick of the last cycle (`kick:699:2`) | the next kicks' transfer registers differ (16 kicks), final fails | the same |
| one animated upload loses its data, behind `emu` (`transfer:301:1` USA, `302:1` Europe) | the per-kick comparison fails at that kick (12,898 bytes, 64 blocks) for 37 kicks, until the next cycle uploads it again | 12,897 bytes, 37 kicks |
| table swapped only before the final decode, behind `emu` (`decode:0:8`, `:4`, `:32`) | 63, 95 and 209 of 213 textures differ from the emulated sampling | 61, 96 and 208 of 212 |
| as designed, no failure: the 16-bit table swapped (nothing is uploaded in CT16); an animated upload dropped in a cycle where it carries the frame memory already holds | PASS | PASS |

A dropped animated upload heals at the next cycle, because the game sends every animated texture again each frame;
alone, the keyframe check sees only what is still wrong at a keyframe. That is the reason for the per-kick
comparison behind `emu`.

### 17.3 The alpha histogram and PABE

`tex --with-emu` keeps, per texture and per draw state (path, PRIM, ALPHA, PABE, the alpha test), the primitives
drawn, the largest vertex alpha of each and the texture's alpha range *as decoded when it was drawn with*. (Taking
the range of whatever memory later holds under a cache entry gives nonsense: a front-end entry decoded from the next
level's image.) All 54 dumps: the 24 levels' first 11 seconds from their load, the front end, boot with the title and
an attract demo, the Track Editor; 477,476,267 primitives.

- **No texture has a texel alpha above 0x80 when drawn with**, on either path: 0 of 11,419 textures (USA, summed
  over the dumps; 281 to 696 a level) and 0 of 13,168 (Europe). 10,513 and 12,134 of them have alpha 0 in every
  texel; the others lie between 0 and 0x80. The level files agree: 0 of the 8,980 (USA) and 9,004 (Europe) textures
  `tex_check.py` decodes has a texel above 0x80.
- **No vertex alpha above 0x80** in any textured or blended primitive of these dumps; the only primitives with one
  are the flip's clears (sprites with A 0xff, not blended, no alpha test).
- So the largest source alpha any draw can produce here is 0x80 (TFX is MODULATE with TCC 1 everywhere).

Draw states over the 54 dumps ("As >= 0x80 out": primitives whose largest possible source alpha is at or above 0x80
at a pixel the alpha test lets reach the colour output; none can exceed 0x80):

| path | PRIM | ALPHA / FIX | PABE | alpha test | primitives | largest As | As >= 0x80 out |
|---|---|---|---|---|---|---|---|
| 1 | 0x5b | 0x44 `(Cs-Cd)*As+Cd` | **1** | A<=0x2f KEEP | 314,276,470 | 0x7f | 0 |
| 1 | 0x5b | 0x21 / 0x80 | **1** | A<=0x3f KEEP | 18,840 | 0x80 | 0 |
| 1 | 0x1b | (no blend) | 0 | A<=0x2f KEEP | 63,187,871 | 0x7f | 0 |
| 1 | 0x5b | 0x29 / 0x80 | 0 | A<=0x2f KEEP | 29,271,991 | 0 | 0 |
| 1 | 0x5b | 0x29 / 0x80 | 0 | A<=0x3f KEEP | 8,314,022 | 0 | 0 |
| 1 | 0x5b | 0x09 | 0 | off | 11,851,828 | 0x7d | 0 |
| 1 | 0x5b | 0x62 / 0x15 | 0 | off | 11,851,596 | 0x54 | 0 |
| 1 | 0x5b | 0x68 / 0x15 | 0 | off | 11,851,320 | 0x54 | 0 |
| 1 | 0x5b | 0x68 / 0x80 | 0 | off | 6,609,987 | 0x7f | 0 |
| 1 | 0x5b | 0x08 | 0 | A<=0x2f KEEP | 6,586,453 | 0x7f | 0 |
| 1 | 0x5b | 0xa1 / 0x80 | 0 | A<=0x3f KEEP; off | 3,551,088; 17,640 | 0 | 0 |
| 1 | 0x5b | 0x01 | 0 | off | 3,202,327 | 0x7f | 0 |
| 1 | 0x5b | 0x29 / 0x7f | 0 | A<=0x2f KEEP | 2,930,917 | 0 | 0 |
| 1 | 0x5b | 0x01 / 0x80 | 0 | A<=0x3f RGB_ONLY | 2,803,457 | 0x80 | 719,849 (at 0x80) |
| 3 | 0x5b | 0x09 | 0 | A<=0x3f KEEP | 625,966 | 0x80 | 0 |
| 3 | 0x5b | 0x01 | 0 | A<=0x01 RGB_ONLY | 277,626 | 0x80 | 277,626 (at 0x80) |
| 3 | 0x01 lines, 0x13, 0x03, 0x0b | (no blend) | 0 | A<=0x2f / 0x3f KEEP, A<=0x01 RGB_ONLY | 145,055 | 0x80 | 156 (at 0x80) |
| 3 | 0x43, 0x53 | 0x21 / 0x20, 0x40, 0x3f | 0 | A<=0x01 RGB_ONLY, A<=0x3f KEEP | 63,596 | 0 | 0 |
| 3 | 0x5b | 0x29 / 0x80, 0xa1 / 0x80 | 0 | A<=0x3f KEEP | 420 | 0x80 | 0 |
| 3 | 0x06 sprite (the clear) | (no blend) | 0 | off | 37,797 | 0xff | - |

**Consequence for PABE (steps 7 and 9).** PABE blends a pixel only when its source alpha is 0x80 or more. Both
states that set it also set an alpha test that keeps only pixels with As at most 0x2f (or 0x3f) and discards the rest
(AFAIL KEEP): no pixel that PABE would blend is ever written. Under such a test the state is exactly **blending off
with the alpha test as a discard**, for any texture and any vertex alpha, so the answer does not rest on the
histogram: it holds for the primitives with vertex alpha above 0x80 the survey counted in this state too (they are
discarded where As exceeds 0x2f and written unblended where the texture's alpha brings As below it). The survey's
state tables, over all its captures, show PABE 1 with these two tests only. The renderer therefore needs no
destination read and no logged divergence for PABE; the reader should check the precondition where it records a
state (PABE 1 implies ATE on, ATST LESS or LEQUAL with AREF below 0x80, AFAIL KEEP) and log a divergence if a state
ever breaks it. The histogram adds: (1) with no source alpha above 0x80 anywhere, every blend factor is within fixed
function's range (`SRC1_ALPHA` = As * 255 / 128 is exact up to 0x80), on PATH3 as well; (2) nine textures in ten
have alpha 0 throughout, so with the cache's alpha range per texture the renderer can drop the alpha test (and the
discard in the shader) for most draws: step 9's alpha planner has its input.

**The survey's vertex alpha above 0x80 was a counting artefact.** It reported 204,744 primitives of the 0x44 state
with a vertex alpha above 0x80, "a few hundred per run"; these dumps have none, nor has a 2,700-cycle dump of level
17 made to look for them (45 seconds of the demo race, 17.3 M primitives in that state). The number is one per swap
cycle (the survey counted 208,416 cycles with a K2 draw): `gs_survey.cpp` starts each primitive's alpha range from
the RGBAQ in force when the previous primitive was drawn, and the primitive before every K2 is the flip's clear
sprite with A 0xff. The `tex` consumer keeps the same count for comparison ("counted the GS survey's way"): 700, 695
and 695 primitives in PABE states in the 700 cycles of the Track Editor, level 5 (USA) and level 18 (Europe) dumps,
against 0 counted per primitive. So no draw of the game has a vertex alpha above 0x80 in the scenes recorded, and the
"0.016%" of the survey and of the plan's step 9 is nothing to handle.

### 17.4 Cost

Measured in `tex --with-emu` (the consumer's own time only; one core of the development machine, the machine
otherwise idle), and `rc_tex_test bench`.

| | level 17 (USA) | level 18 (USA) | level 2 (USA) |
|---|---|---|---|
| a race cycle's uploads | 17 transfers, 100 KB | 14, 68 KB | 19, 124 KB |
| ... that change memory | all 17 every third cycle (about 320 blocks), none in between | 14 every third | 19 every third |
| walk the kicks and apply them, a race cycle: median / 99% | 0.35 ms / 0.69 ms | 0.32 / 0.45 ms | 0.43 / 0.84 ms |
| the level load's largest cycle (3,965 transfers, 5.8 MB with the splash's tiles) | 7.7 ms | 9.7 ms | 7.7 ms |
| textures decoded again in a race cycle: median / 99% | 0 / 5 (30,720 texels, 0.12 ms) | 0 / 2 (16,384 texels, 0.07 ms) | 0 / 7 (28,928 texels, 0.13 ms) |
| first decode of what the race's first frame draws | 196 textures, 569,856 texels: 2.5 ms | 345 textures, 1.24 M texels: 5.4 ms | 262 textures, 698,112 texels: 3.3 ms |
| a full decode of every cached texture | 324 textures, 942,848 texels (3.6 MB of RGBA): 4.3 ms | 419, 1.47 M texels (5.6 MB): 6.4 ms | 517, 1.19 M texels (4.5 MB): 6.1 ms |

Over the 24 levels (both versions) a race cycle carries 13 to 23 transfers and 52 to 124 KB; the cache holds 281 to
703 textures, 3.2 to 6.7 MB of RGBA. Per pixel: an upload costs 3.2-4.7 ns (it compares before it writes), a decode
1.2-1.9 ns a texel (CT16 9.2 ns; one texture a level). Applying the animated textures is 2% of a 16.7 ms frame and
needs no work; of the 17 animated textures of level 17 five are among the textures drawn, so a third of the cycles
decode and would upload about 30,000 texels (120 KB), the others nothing. Without the change test every cycle would.

**For the Vulkan upload (step 9):** one host texture per cache entry, RGBA8 with raw GS alpha; after a cycle's
lookups, upload the entries whose `generation` differs from the one uploaded. A level's first frame uploads 2-5 MB
once; a race cycle 0 or about 120 KB. Mip levels are entries of their own (two textures a level at most). If block
granularity ever matters (it does not at these sizes), `GsMemory::blockSerial` is there to upload sub-rectangles.

### 17.5 What it changed in the plan, and notes for steps 6-9

1. **PABE needs nothing** (17.3): steps 7 and 9 treat PABE 1 states as blend off under their alpha test.
   `RENDERER_PLAN.md` step 5, step 9 and its open questions are updated.
2. **Dirty tracking is by changed content, per 256-byte block**, not by upload: the plan's "about 20 animated
   textures re-uploaded every frame" stays true of the uploads, but only every third cycle changes memory.
3. **Uploads and sampling formats differ more than the survey listed**: animated textures arrive as CT32 64x64 or
   64x32 images of a T8/T4 texture's storage, as T8, or as T4; the CT16 texture is never uploaded as CT16. The shadow
   in the GS's own layout makes that free, as planned.
4. **The `tex` consumer can run behind `emu`**, and `emu` has a GIF tap for consumers behind it: step 7's `gif2d` and
   step 8's `vif` can use the same arrangement for their checks against the emulated machine.
5. **`GifWalker` is step 7's starting point** (register writes in register form from every packet form). For the
   reader: vertices without a drawing kick (XYZ3, or a PACKED XYZ2 with its ADC bit) occur on both paths (768,124 in
   the level 17 dump's 700 cycles); no tag has PRE outside PACKED form; TEX2 is never written. The shadow's observer
   (`TexShadow::setObserver`) delivers PATH3's other register writes in order with the transfers, so a reader built
   on it sees a texture's upload before the TEX0 that samples it whatever the packets' layout.
6. **Mipmaps**: by TEX0 the two mipmapped textures are CT32 64x64 with MXL 4 (TEX1 0xf6000000170, MIPTBP1 and 2) and
   CT32 64x32 with MXL 3 (TEX1 0xf600000016c; the survey's table says 32x32), on levels 2 and 14 in both versions.
   The cache holds each level as an entry at MIPTBP's place.
7. **Live use.** `TexShadow` is a `Consumer`; the live reader adds it to the capture's consumer list (EE thread) and
   the draw-list builder asks the cache as it records draws, so texture contents are those at the draw's place in
   kick order. The cache is not thread-safe: the presenter takes decoded texels through the published draw list
   (section 8), not from the cache.

### 17.6 Open

- **Split screen, the podium, results screens, the Track Editor's race and its loaded tracks** were not recorded for
  this step. The shadow and decode do not depend on the scene; the alpha histogram of those scenes is missing.
- **CSA other than 0, CSM2, 16-bit palettes, CT24 and CT16S** are implemented from the GS's rules and the emulated
  GS's code and covered by `rc_tex_test` only: RC never uses them, so nothing in a dump can check them.
- **Cache eviction and the texture count on the GPU** (entries of a level that is gone) are step 9's.

## 18. Step 6 as built (2026-11-20): uploads into the frame buffer and the flip

Renderer plan P1 step 6, test plan 10.2's `blit` row. Built: `render/blit/` (libraries `rc_blit`, CPU, on `rc_tex`;
`rc_blit_vk`, the GPU half on the vendored RHI; namespace `rc::rd`), word origins in `render/tex/gs_memory`
(`GsMemory::trackOrigins`), `rc_replay --consumer blit` (`tools/rc_replay/blit.{h,cpp}`). Usage: `NOTES.md`,
"Uploads into the frame buffer and the flip". Nothing of it is linked into the game or the oracle (`ninja -n
rc_revenge_pro rc_oracle` has nothing to do after the change, both versions), the runtime and every game function are
untouched, so normal play and the oracle cannot have changed and were not re-run. Nothing found conflicts with
option C.

### 18.1 Design

```
render/blit/frame_blit.{h,cpp}      FrameBlit: the rc::rec::Consumer that keeps GS memory from uploads and DISPFB2, builds
                                    the displayed picture at each cycle's close and says whether a draw may be in it
render/blit/blit_presenter.{h,cpp}  BlitPresenter: the picture through the RHI (upload, draw, read back)
render/blit/blit.vert, blit.frag    a covering triangle; texelFetch at the target pixel, R G B as stored, alpha 1
tools/rc_replay/blit.{h,cpp}        BlitCheck: FrameBlit + a headless Vulkan device + BlitPresenter, the checks, faults
```

- **Memory and transfers are step 5's.** `FrameBlit` owns a `TexShadow` (so `GsMemory` and `GifWalker`), which already
  applied uploads into frame memory; nothing about the transfers is duplicated. The shadow starts from the first
  keyframe's `GSVR` (and one after a save-state load); later keyframes are ignored (they hold the draws too).
- **The flip.** DISPFB2 is taken from the `HW` events (64-bit stores, or either 32-bit half), starting from the
  keyframe's `GSPR`. The cycle closes at the DISPFB2 store (section 3.5), whose `HW` event comes first, so at
  `cycleEnd` the consumer reads what the store shows: the 640 x H rectangle at FBP, FBW, DBX/DBY, read through
  `gs_layout`'s CT32 addressing (made independently of `record/picture.h`, which the recorder used for the reference).
  DISPLAY2 is ignored (its position moves with Europe's screen adjust; movies set 512 lines); PMODE is always circuit 2
  only. A DISPFB2 in another format is "unsupported display" (never seen).
- **Upload-only, without a rasteriser.** `GsMemory` keeps one origin byte per 32-bit word when asked: loaded (keyframe
  or clear), uploaded (a CT32 transfer wrote all of it; the 16-, 8- and 4-bit formats write parts of words and leave
  the mark), drawn. `FrameBlit` marks drawn, conservatively: on a PATH3 vertex kick (XYZ2/XYZF2) the pages of the
  context's frame buffer inside its scissor (unless FBMSK masks every bit) and of its Z buffer (unless ZMSK), the
  context from PRIM or PRMODE as PRMODECONT says; on a PATH3 vertex kick when any of those registers is unknown, and
  on every VIF1 kick (PATH1 output needs step 8), all of GS memory. Registers are known from the keyframe's `GSRG` and
  PATH3's own writes (the shadow's observer), and become unknown at a VIF1 kick (PATH1 could write them). A picture
  with no drawn word is **upload-only**; otherwise **not applicable** (steps 7-9 draw it), never a failure.
- **The GPU half.** `BlitPresenter` needs a device from `rhi_init` (the tool makes it headless: `sdlWindow` null, no
  swapchain, which the RHI supports), uploads the RGBA8 picture through an upload buffer into an `RGBA8_UNORM`
  texture, draws one triangle into an `RGBA8_UNORM` target of the picture's size with `texelFetch` at
  `gl_FragCoord` (no filtering or coordinate arithmetic, so exactness does not depend on the GPU's interpolation), and
  reads the target back (`rhi_readback_texture`); it can also read the uploaded texture back. It is the live
  presenter's path minus the letterbox: the live renderer draws the same texture into its swapchain image.
- **The presentation transform** is the runtime's (step 4's A3): R, G, B as stored, alpha 255 (the GS alpha, 0x80 for
  opaque, is not shown). The CPU picture keeps the stored alpha and is compared with `REFI` in all four bytes; the GPU
  target is compared with `REFI`'s R, G, B and alpha 255; the GPU's uploaded texture with the CPU picture in all four.

### 18.2 Checks and results

`rc_replay <dump> --consumer blit --verify` on every exact dump: for every cycle, DISPFB2's FBP must be `END.refFbp`;
for every upload-only cycle with a reference, the CPU picture must equal `REFI` byte for byte where the dump stores it
and otherwise hash to `END.refHash` (the recorder's Hash64 of the same 640 x H x 4 bytes), and through the RHI the
displayed read-back must equal the reference with alpha 255 (where the dump keeps only the hash, the CPU picture that
hashed equal stands for it) and the uploaded texture the CPU picture. RTX 2060 by PRIME offload, headless; the
validation layer (`--validation`, synchronisation validation on) on the four full-reference dumps.

New dumps with every reference picture stored (`RC_FRAME_RECORD_REF=1`, exact mode), in
`build-decomp/frame-dumps/s6/` and `build-eu/frame-dumps/s6/`: `boot` (`boot+900`: legal screen and logos, the
publisher movie, title, front end; 332 / 329 MB) and `load17` (`RC_BOOT_LEVEL=17`, `boot+400`: the level load with
its splash; 192 / 199 MB).

| dumps | cycles | upload-only (no drawing in the cycle / drawing elsewhere) | not applicable (equal anyway) | CPU = reference | GPU displayed = reference; GPU upload = CPU | DISPFB2 FBP |
|---|---|---|---|---|---|---|
| USA s6 `boot` + `load17` (REFI every cycle) | 1,300 | 261 (256 / 5) | 1,039 (2) | 261 of 261, REFI bytes | 261 / 261; 261 / 261 | 1,300 / 1,300 |
| Europe s6 `boot` + `load17` | 1,300 | 260 (255 / 5) | 1,040 (2) | 260 of 260, REFI bytes | 260 / 260; 260 / 260 | 1,300 / 1,300 |
| USA s5, 28 dumps (hash only) | 22,000 | 416 (396 / 20) | 21,584 (44) | 416 of 416, hash | 416 / 416; 416 / 416 | 22,000 / 22,000 |
| Europe s5, 27 dumps | 19,300 | 357 (326 / 31) | 18,943 (51) | 357 of 357, hash | 357 / 357; 357 / 357 | 19,300 / 19,300 |
| step 4's `b4-key` (USA, Europe; REFI) | 30 + 30 | 30 + 30 (all no drawing) | 0 | 30 + 30, REFI bytes | 30 + 30 | 60 / 60 |
| step 4's exact `b4-x2-*` (CPU only) | 744 + 715 | 3 + 2 | the rest | 3 + 2, hash | not run | all |

0 validation errors or warnings. Every cycle with no drawing at all is upload-only (USA s5 396 of 396, Europe 326 of
326, s6 all). The tracked PATH3 registers equal the recorded register files at every kick where they were known
(USA s5 3,772,556 values, Europe 3,344,935, s6 all). Step 4's 14 plain dumps have no references; they classify the
same way (their boot dumps: 256 upload-only cycles each).

Where the upload-only cycles are: boot cycles 0-256 (legal screen and logos as 640x128 halves, 4 transfers a
picture; the publisher movie, 600 16x16 tiles a picture, with the 1,280-tile 640x512 clears at its start), the
loading splash (2,560 tiles of two clears and the 600-tile splash, then a 600-tile picture: cycles 0-2 or 0-1 of each
level dump; in the boot dumps the attract demo's level load, USA cycles 880-881 (s5) or 888-889 (s6), Europe 768) and the splash of the demo race's restart (four or two cycles near the end of
most level dumps; four times in `l17long`), and the front end's load. The "drawing elsewhere" cycles are splash cycles
with the clear sprite of the flip's draw environment or a 2D packet drawn before the tiles replaced every word. The
"equal anyway" not-applicable cycles are pictures a draw could have touched but did not change (the conservative
marking: a K1 packet drawn while the context was unknown marks everything).

**The checks can fail** (`--fault`, the s6 `boot` dumps, cycle 100 is a movie picture):

| planted | USA | Europe |
|---|---|---|
| `tile:100:37` (one 16x16 movie tile dropped) | cycle 100 fails, CPU and GPU: 256 pixels from (32,112) | 256 pixels from (32,120) |
| `dispfb:100` (DISPFB2's FBP xor 0x50) | FBP check fails (0x0, recorded 0x50), picture 78,569 pixels, CPU and GPU | the same |
| `byte:100:1000` (a byte of displayed pixel 1000 flipped in the shadow) | cycle 100 fails, 1 pixel, CPU and GPU | fails at cycle 100 and at every later picture of that buffer up to the title (76 pictures): Europe's 640x240 movie sits 8 lines down in the 640x256 buffer, so row 1 is never uploaded again |
| `gpubyte:100:1000` (the byte flipped in the GPU upload only) | CPU passes; GPU displayed and GPU upload fail, 1 pixel | the same |
| as designed, no failure: `tile:888:100`, `tile:888:2561` (USA) | a tile of the first of two 640x512 clears that the second rewrites, and a transfer outside frame memory | |
| `tile:889:599` (the splash's last tile) | cycle 889 fails, 256 pixels from (624,224) | |

**Cost** (one core; the GPU path synchronous with its read-back, so not a frame-time figure): building the picture
with its origin count 0.6-0.75 ms a cycle; upload, draw and two read-backs 2.4-6.8 ms a picture. Live, the presenter
would only upload and draw (no read-back), and the CPU picture is needed only when the picture is upload-only.

### 18.3 What it changed in the plan, and notes for steps 7-9

1. **Upload-only is a property of the picture, not of the cycle.** The test plan's "cycles made only of uploads and a
   flip" is answered per word of the displayed rectangle: a splash cycle that draws the flip's clear sprite before its
   tiles is still upload-only, a cycle that draws nothing can show a buffer drawn earlier (none did in these dumps).
   The live renderer can ask the same question to present an uploaded picture directly and the render target
   otherwise; steps 7-9 replace the conservative "drawn" marks with their own draws.
2. **Uploads into frame memory are applied by the shadow**, as step 5 left them; the live renderer will copy the
   uploaded rectangle into its render target (or present the texture) rather than keep frame memory, once draws are
   on the GPU. In RC the uploads cover whole buffers (logos as halves, movie tiles, clears), so the copy is per picture.
3. **K1 runs before the flip's draw environment**, so with PATH1 not read, PATH3's context is unknown for every 2D
   packet (195,400 of 197,002 PATH3 vertex kicks in USA `boot`). Step 7's reader keeps one register file across paths
   and makes this exact; it does not change any result here.
4. **The RHI runs headless** for checks: `rhi_init` with no window, `rhi_wait_frame` per picture, off-screen targets and
   `rhi_readback_texture`. Steps 7-9 can check their Vulkan pictures the same way in `rc_replay`.

### 18.4 Open

- The intro movie's audio and the host movie player (plan item 6 of "Beyond graphics") are not this step's: the
  pictures arrive as uploads, so the renderer shows whatever the runtime's MPEG/IPU stubs decode.
- The swapchain present itself (letterbox, aspect) is the live presenter's (`platform/vk_host.cpp`); only the
  off-screen equivalent is checked bit for bit.

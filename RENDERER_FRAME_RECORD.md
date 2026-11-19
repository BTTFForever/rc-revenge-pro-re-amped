# Frame record, dump format, replay tool and side tags (renderer plan P1 step 4: design)

Designed 2026-11-19 for `RENDERER_PLAN.md` step 4. Nothing here is built yet; "Build batches" at the end is the
work order. Read with `RENDERER_SURVEY.md` (what the kicks carry) and `ICO_RENDER_STUDY.md` section 2 (where the
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

Keyframe contents, taken on the EE thread at a cycle close after `ps2x::asyncvif::sync`, the backend's `Flush` and
`Sync(GSSyncReason::DebugReadback)`:

| chunk | contents | used by |
|---|---|---|
| `GSVR` | GS local memory, 4 MB as the runtime stores it (`SnapshotVram`) | texture memory at segment start (steps 5, 7, 8), the `emu` consumer |
| `GSPR` | `GSRegisters` (the privileged registers), 160 bytes | flip and display state |
| `GSRG` | the general register file: `u64[0x63]` indexed by GS register address (PRIM, RGBAQ, ST, UV, TEX0/1, CLAMP, MIPTBP, XYOFFSET, PRMODECONT, PRMODE, TEXCLUT, SCANMSK, TEXA, FOGCOL, SCISSOR, ALPHA, DIMX, DTHE, COLCLAMP, TEST, PABE, FBA, FRAME, ZBUF, BITBLTBUF, TRXPOS, TRXREG, TRXDIR, both contexts), re-encoded from `GS::getDebugSnapshot()` | the readers' register file at segment start |
| `VU1M` | VU1 micro memory then data memory (16 KB each) | the VIF reader and the C++ references (data memory persists between runs) |
| `VU1R` | VF[32][4], VI[16], ACC, Q, P, I as plain values | the C++ references ("a program can store registers it left set in an earlier run") |
| `VIF1` | the runtime's `VIFRegisters` for VIF1 plus ROW and COL | the VIF reader (CYCLE, MODE, MASK, BASE, OFST, TOPS persist across kicks) |
| `EMUS` | opaque: the runtime's own serialisation of the GS front end, backend state, VU1 and VIF1 (`ps2x::savestate::saveGraphics`, new, cut from `serializeMachine`), with `emuStateVersion` in the file header | only the `emu` consumer, so no hidden emulator state (CLUT cache, vertex queue, a GIF tag in progress, path 3 mask) can break bit-exactness. A dump whose version differs from the tool's is refused by `emu` only |
| `TAGP` | tags emitted but not yet consumed by a kick (section 5.3) | tags of the segment's first K1/K2 |

A dump ends with a **final keyframe** (flag `final`, `GSVR` + `GSPR` + `GSRG` only): the state after the last cycle,
so a replay can compare all of GS memory at the end. In exact mode `GSVR` is exact; otherwise the keyframe carries
`vramExact = 0` (a GPU backend's memory image is whatever its `SnapshotVram` gives) and tools say so.

A save-state load (F9) while recording closes the segment; the next cycle close writes a keyframe flagged
`afterStateLoad`.

### 3.5 Exact mode extras

On when the run is synchronous and software-rendered (the recorder checks `ps2x::asyncvif::active` is false and the
backend is the bare `GSCpuBackend`; a new `GSThreadedBackend::DefaultName()` reports which backend `MakeDefault`
made). Then, and only then:

- `KICK.regs`: the 0x63-entry register file read from the emulated GS just before the kick. Lets the 2D reader
  (step 7) be developed and tested on K1 before a VIF reader exists, although K1 depends on what K2 left behind.
- `KICK.gifHash`, `gifBytes`.
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
};
inline const Hooks *g_hooks = nullptr;
}
```

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
    virtual void hw(uint32_t address, uint32_t width, uint64_t value) {}
    virtual void crt(int interlaced, int mode, int ffmd) {}
    virtual void tag(const TagView &) {}             // every tag, in emission order
    virtual void kick(const KickView &, const RamView &) {}
    virtual void odd(int kind, uint32_t address, uint64_t value) {}
    virtual void cycleEnd(const CycleInfo &, const uint8_t *refRgba) {} // refRgba null without a picture
    virtual void close() {}
    virtual ~Consumer() = default;
};
}
```

All calls are made on one thread, in event order: the EE thread live (inside the hook), the tool's thread off-line.
A live consumer must not block on the GPU or on another thread's work. The file writer and the ring are consumers
too; `capture.cpp` owns the page tracker and the list of consumers.

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
  K2, movie pictures), for kicks made inside Sony's library.

Resolution happens at the kick, in the recorder: an address-anchored tag whose anchor lies in one of the kick's
ranges gets that range's stream offset plus the distance into it, and goes into the kick's `spans` (sorted by stream
offset, then sequence). Anchors must be in buffers a kick visits once (the tag array, the rings, the 2D buffer,
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
   live comparison therefore log nothing.
4. Nothing reads a tag back into game logic. The channel has no query functions.

Cost when off: one load and test of a global byte per site; in the oracle build nothing.

### 5.3 In the record

`TAG` events are written in emission order with a dump-wide sequence number. A kick refers to tags by sequence
(`spans`, `labels`); K2's tags were emitted in the cycle before, so a reader keeps the tags of the current and the
two previous cycles, and a keyframe re-emits the unconsumed ones as `TAGP`. A `TNAM` chunk after the file header
lists `kind`, pass, draw-kind, HUD and label enumerators as text (`table value name`), so tools print names without
the game's headers.

Tag kinds, version 1 (payloads little-endian, no pointers to follow, sizes multiples of 8):

| kind | payload |
|---|---|
| 1 `Scene` | game frame (the override's counter), `gRdr.frameNumber`, scene id (the survey's: mode << 12, level << 4, split << 1, movie), flags |
| 2 `View` | index (0, 1 in split screen), scissor x0 y0 x1 y1, vertical draw offset, camera position, camera rotation (3 x 3, from `_sRdrCamera`), `dVal` `dValX` `dValY` (projection scales), `minZ` `maxZ`, `viewAngle` |
| 3 `Pass` | pass id |
| 4 `Draw` | `DrawKey`, flags, optional object matrix (3 x 4 floats) |
| 5 `Hud` | element id, its box in the game's 2D units, anchor hint (left/right/top/bottom/centre/stretch) |
| 6 `Kick` | what (1 K1, 2 K2, 3 texture upload, 4 animated texture, 5 backdrop, 6 movie picture, 7 flip), object (the texture's `_sGfxTexture` address as identity), two free words |
| 7 `Label` | text |
| 8 `Cut` | reason (camera mode change, level transition): reserved for interpolation |

Step 4 builds the channel and a first set of sites that exercises every shape: `Scene` (the override), `View`
(`SetFrameWindow` / where the frame loop sets up each view's camera), `Kick` labels (`GfxP2_RenderDmaSendActual`,
`startVif1Chain`, `GfxTex_Update`, `BackdropDisplayBMP24`, the texture upload helpers in `gfx_p2.cpp`,
`ldimage.cpp`'s `loadImage`), `Draw` on the chain (`TRdr_VUShape`, `Rdr_VUSkin`), `Draw` + `Hud` in the 2D buffer
(the `Hud*` panel functions of `hud.cpp`, one font string function). Full coverage is added when a feature needs it
(P3: keys for interpolation, HUD anchors).

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
RC_FRAME_RECORD_AT=<trigger>    when; default "key"
    boot[+count]                from the first kick
    cycle:<n>[+count]           display cycles since boot
    game:<n>[+count]            game frames since boot (the override's counter)
    level:<n>[+count]           game frames since the level started (as RC_RAMDUMP_FRAME counts)
    scene:<hex>[+count]         the first cycle of a scene id
    key[+count]                 F10 starts, F10 or count stops (default count 120)
    ring:<n>                    keep the last n cycles; F10 writes them and goes on
RC_FRAME_RECORD_REF=<n>|hash|off  reference picture every n-th cycle (default 1; ring: hash), hashes always (exact mode)
RC_FRAME_RECORD_KEY=<n>         cycles per segment (default 300; ring 120)
RC_FRAME_RECORD_STRICT=1        stop the run if exact mode is not possible
```

A recording starts and stops at cycle closes. `count` is in cycles. Both versions use the same switches;
`tools/frame_record.sh [play.sh arguments]` is `play.sh` with the exact-mode environment and honours `RC_VERSION`.
`platform/vk_host.cpp` needs no change in step 4 (F10 arrives through the runtime's loop).

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
| `STATE` | step 7 | a delta of the finite state block (blend mode and FIX, TEST, ZBUF, scissor, texture key and sampler, PABE, FRAME target); the block's layout comes with step 7's blend design |
| `CLEAR` | step 7 | the draw environment's sprite |
| `SCREEN_PRIMS` | step 7 (2D), step 8 (everything through the references) | GS-space triangles or lines: XY, Z, RGBA, STQ |
| `SHAPE_BATCH` | step 9 | a static triangle list by guest address and generation, the VU memory constants, the program id |
| `SCOPE_BEGIN` / `SCOPE_END` | step 7 | a side tag passed through (key, HUD element, view) |

`rc_replay --consumer vk` is then reader + replay through the RHI off-screen with a read-back; `--save-drawlists`
writes them into a copy of the dump for golden-picture tests of the replay alone.

## 10. Test plan

### 10.1 Step 4's acceptance (both versions unless said)

Dumps, exact mode, by `tools/frame_record.sh`: levels 5, 17, 18, 20 (`RC_BOOT_LEVEL`, `level:300+120`); the front
end (`RC_SKIP_INTRO=1`, `game:600+120`); boot through the logos and the publisher movie (`boot+900`,
`RC_FRAME_RECORD_REF=10`); one level load with its splash (`RC_BOOT_LEVEL=17`, `boot+400`); the Track Editor with
a `tedit_walk.py` pad (120 cycles); USA only, a split-screen attract demo if a `scene:` trigger reaches one.

| # | check | pass |
|---|---|---|
| A1 | `rc_replay --consumer stats --verify` | every stream hash reproduced by `walkChain`; no absent page read; no `ODD` event; `frame_dump.py verify` clean |
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
| 5 shadow texture memory | `tex`: starts from a keyframe's `GSVR`, applies every host-to-local transfer of the PATH3 kicks through its own write swizzles | at every later keyframe and the final one, the shadow equals `GSVR` on all texture blocks (0x1e00 up) bit for bit, on dumps that include a level load (CT32 tiles), a race (PSMT8/PSMT4 animated frames) and the front end; decode against `tools/track_viewer` on the same level's BBK as planned; the alpha histogram (texels above 0x80) per level |
| 6 uploads into the frame buffer, flip | `blit` | on cycles made only of uploads and a flip (logos, movie pictures, the splash) the picture it builds equals `REFI` bit for bit, on the CPU and through the RHI's read-back |
| 7 GIF reader and 2D | `gif2d` | (a) its register file after each PATH3 kick equals the next kick's recorded `regs`; (b) in front of the emulated GS (section 7): the picture equals `REFI` bit for bit, proving the parse; (c) Vulkan: K1 drawn over `--picture-before <cycle>:<K1>` (the emulated frame buffer just before K1, so the 3D needs no mask) against `REFI`, exact where the blend is integer-exact, otherwise within the bounds step 7's blend design states; the per-state pixel tests against the CPU blend model stay as planned |
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

1. **Capture and container.** `ps2_capture_hooks.h` and the hook sites; `render/record` (format, page tracker,
   writer, reader, consumer interface, `walkChain`, capture with cycles and the `boot` / `cycle` / `game` / `level`
   / `scene` triggers, plain mode, keyframes with the plain state chunks); `rc_replay` with `stats`;
   `tools/frame_dump.py`; `.gitignore`. *Check:* a unit test that writes a synthetic dump and reads it back (no
   game); A1 and A4 on levels 17 and 18, the front end and boot, both versions; the measurements for A9's first
   table.
2. **Exact mode and the emulated replay.** `saveGraphics` / `loadGraphics` in the runtime and `EMUS`; register
   files per kick, `gifHash`, reference pictures, exact and final keyframes, `STRICT`; `tools/frame_record.sh`; the
   `emu` consumer with `--verify`, `--png`, `--picture-before`, `--path1`. *Check:* A2, A3, A6, A7 on the full dump
   list of 10.1, both versions.
3. **Side tags.** `render/gfx/tags`, the comparing switch in `rc_decomp.cpp`, resolution at the kick, `TAG` /
   `TAGP` / `TNAM`, the first sites of 5.3, the inspector's `tags`. *Check:* A8, A5's shadow half, A4 again (oracle
   counts on both versions), A1 and A2 still pass with tags in the dumps.
4. **Ring, hotkey, save states, write-up.** `ring:` and `key` triggers, F10 in both host loops, the state-load
   segment break, A5's ring half, A9's final table, `NOTES.md` ("Frame record": switches, tools, how to take and
   check a dump) and the measured numbers back into this file. *Check:* a ring dump taken by F10 during a default
   `play.sh` session replays through `stats --verify` and, taken in exact mode, through `emu --verify`.

Batches 1 and 2 are the dependency of steps 5-8; 3 and 4 can follow step 5 if the builds are busy.

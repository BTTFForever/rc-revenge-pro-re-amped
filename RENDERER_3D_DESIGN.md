# K2 on the host: the VIF1 chain reader, the VU1 references, PATH1 on Vulkan (renderer plan P1 step 8: design)

Designed 2026-11-20 for `RENDERER_PLAN.md` step 8 (the VIF chain reader with the C++ VU1 references, K2 through
Vulkan off-line, then live) and for the cost decision left open by step 7's batch 4b (`RENDERER_2D_DESIGN.md` 19.5).
"Build batches" (section 13) is the work order for the Opus build agent. Read with `RENDERER_2D_DESIGN.md`
(sections 2, 6-8, 15, 18, 19), `RENDERER_FRAME_RECORD.md` (3, 4.2, 7-10) and `NOTES.md` ("VU1 programs converted").
Paths are relative to the repository. The runtime is `third_party/ps2recomp/ps2xRuntime` (`RT/`); "the reference"
is its software GS, the machine every `REFI` picture comes from; "the references" (plural) are the six C++ VU1
programs of `decomp/vu1`. Everything measured here comes from `rc_replay --consumer vifstat` (section 3), added
with this design; nothing in it is the reader, and no game function or runtime file was touched.

## Decisions for the user

**U1. The reference rasterises PATH1 rounding toward zero. Recommended: correct that in the runtime (one site) and
regenerate the dumps' references by replay, before K2 is drawn through Vulkan (batch 2).** Needs an answer before
batch 2; batch 1 and step 7's batch 5 do not depend on it.

**Decided by the user (2026-11-20): A.** Batch R (section 13) runs next, before batch 2. **Built 2026-11-20 (section 17).**

- *Found.* The runtime runs a VU1 program with the host FPU rounding toward zero (`ps2_vu1_core.cpp`: MXCSR
  `| 0x6000`, which the VU's arithmetic needs) and hands every XGKICK packet to the GS from inside the run
  (`VU1Interpreter::finishXgkick`). The software GS interpolates in floats on the calling thread (and, as its code
  reads, carries the caller's MXCSR to its band threads and to the GS thread), so **every PATH1 triangle is set up
  and shaded with truncating float arithmetic, every PATH3 triangle with round to nearest.** Measured at the
  packets: PATH1 under MXCSR 0x7fb2 / 0x7fa2, PATH3 under 0x1fb2 / 0x1fa0 / 0x1fa2, in every kick of the three
  dumps looked at (3.11). It is an accident of where the call sits, not a rule of the GS (the hardware does not
  interpolate in host floats at all; a GPU back end rounds as the GPU does), and it is not small: replaying dumps with PATH1 handed to the same GS under the kick's own rounding
  mode changes **32-33% of all picture pixels in a race** (levels 5 and 17, 400 cycles each: +1 on 13-16% of a
  channel's values, +2 on 1.5-3%, -1 on 3.5-8%) and 6% in the front end.
- *Why it matters now.* Step 7's shader computes the reference's per-pixel arithmetic and is bit-exact against it
  (14.49 G pixel-kicks) because K1 is rasterised under round to nearest, which is what a GPU does. The same shader
  on K2 would differ from `REFI` on about a third of the pixels, for a reason that is neither renderer's error;
  check (c) for K2 and the chained replay (section 11) could then only be statistical.
- **A (recommended): fix the reference machine.** In `finishXgkick`, hand the packet to the GS under the rounding
  mode the VU1 run was entered with, and restore; in the same batch lift the 65,536-cycle limit that cuts 1,095
  water runs short in these dumps (3.3; the hardware finishes them, and the references do). Listed under "Local
  changes to the runtime". The 67 exact dumps are then *re-referenced*, not re-recorded: a new `rc_replay` mode
  replays each dump on the corrected machine and writes a copy whose pictures, picture hashes, register files, GIF
  hashes and keyframe state are the corrected machine's; the kick record (what the game sent) is untouched, so
  every reader check of steps 4-7 keeps its inputs. Cost: one small Opus batch (section 13, batch R) and about two
  hours of machine time to repeat the checks of steps 4-7 on the new dumps. Effect on the game: the `gs` renderer's
  software path draws 3D about one level brighter on a third of the pixels; the default GPU back ends are
  unchanged.
- **B: reproduce the truncation in the K2 shader.** A second build of `gs2d.frag` in which every float operation of
  `RENDERER_2D_DESIGN.md` section 6 (weights, Z, colours, S/T/Q, 1/Q, the bilinear filter: about eighty operations)
  is replaced by a directed-rounding version made from double precision and error-free transformations, proved by
  repeating T5-T7 under MXCSR 0x7f80. No runtime change and no re-baselining; an estimated two Opus batches and a
  Fable review, two to three times the fragment cost on every 3D pixel, to match an artefact P3 discards.
- **C: leave both.** K2 is judged by difference statistics (the frame record's 10.2 row 8 (d) as first written);
  about a third of the pixels differ by a level, a one-unit fault in a 3D blend state cannot be seen, and the
  chained replay of 17.3 item 6 cannot be exact. Not recommended.
- *Default if there is no answer:* batches 1 and (step 7) 5 proceed; batch 2 waits.

**U2. Live `vk` at step 8 will not hold 60 / 50 fps in 3D scenes; the default taken is to accept that until P2.**
Say if it is not acceptable. The exact references cost **22 ms of one CPU thread per K2 kick in the front end
(largest 33 ms), 26 ms on level 17 (42 ms) and 47 ms on level 18 (69 ms)** on the development machine (Ryzen 7
3750H; 3.10), against a 16.7 ms frame; game speed follows the frame rate, so a `vk` session runs at roughly 60% of
full speed in the menus and a third to a half in races, wherever a 3D scene is drawn, from step 7's batch 5 on (the live 2D needs PATH1's registers, and they come out of the references,
section 7). This is what the plan's step 8 says ("races draw correctly (slowly)") and it is not the blend's cost:
the GPU side of the decision below is within a frame. `gs` stays the default renderer; 60 / 50 fps becomes an
acceptance criterion of P2 (step 9 replaces the references by shader batches, program by program, and step 10 is
the performance pass). Alternatives, neither recommended now: (i) run the references of a kick on several threads
(they depend on one another only through VU memory and three register lanes, 3.4; a P2 batch pulled forward); (ii)
feed the live GIF reader from the emulated VU1's packets (the runtime's `gifPacket` hook) instead of the
references: full speed at once, but the new renderer would then draw from emulated hardware output, on the
emulator's threads, which is the opposite of where the plan goes and is thrown away at step 9.

**U3 (raised by batch 2, 2026-11-21; not answered). K2's GPU time against 10.4's limit: how it is to be measured.**
Batch 2 passed every correctness check (section 18) but 10.4's acceptance is open: level 21 measured 9.05 ms a K2
kick against the limit of 8, the time is fragments through the exact shader (not draws or barriers, as 10.4 had
assumed), and the measurement itself swings between 6.0 and 21.9 ms for the same kicks with the GPU's clock, which
the driver holds at 300-1,005 MHz of 2,100 during off-line replays. To choose: (A) time it again with the driver's
performance mode set for the run (a setting of the desktop, `GPUPowerMizerMode`), (B) leave it to batch 4's live
timestamps, (C) accept and move the question to P2 with U2. Batch 3 does not depend on it. Details: 18.5, 18.7.

**Decided here (not the user's), for the record:**

1. **The exact blend's cost (19.5): read mode is the live default for K1 and for K2; the barrier rule changes from
   box groups to covered pixels; the interlock form is not built; fixed function stays as the fallback and a
   switch.** Section 10. Measured on all 67 exact dumps: K2 as built would issue 173 barriers a kick on average
   and up to 879 (3.6 ms and 18 ms of GPU time at the measured 20.6 us a barrier); issuing a barrier only when a
   reading primitive covers a pixel written since the last barrier gives **16 on average and 85 at most (0.33 ms,
   1.75 ms)**, because 3D reading primitives are meshes of triangles that touch but do not overlap. K1 goes from
   119 to 96 at most. A whole frame (K2 and K1) then needs 26 barriers on average and 126 at most: 0.5 ms and 2.6 ms.
2. **No VU interpreter and no new chunk:** every program and entry point the dumps call has a reference that gives
   the emulated VU1's bytes (1,045.7 M packets compared, 0 differing); the keyframes already hold the VIF1
   registers, both VU memories and the VU registers as plain chunks (3.2, 5).
3. **The references return their registers.** Three lanes of an earlier run's registers reach the GS in a later
   run's packets (3.4), so a reference must leave the registers as the program does; batch 1 adds that output.
4. **The reader drops what covers no pixel.** 56% of PATH1's triangles lie wholly outside the scissor or have no
   area (3.7); the K2 draw list carries the rest.

For information, found while measuring:

1. **The VIF1 streams are plain.** Eight codes (NOP, STCYCL, OFFSET, BASE, MPG, MSCAL, FLUSHA and UNPACK V4-32),
   one write cycle (CL 4, WL 4), no mode, no mask, no ITOP, no MSCALF or MSCNT, no FLUSH or FLUSHE, no DIRECT, no
   MARK, no interrupt bit; nothing crosses a DMA packet boundary except a code in a DMA tag whose data is that
   tag's payload (3.2).
2. **Every PATH1 draw takes the reference's one triangle routine** (`DrawTriangleFast`; its fallback counters stay
   at zero over all 67 replays), the routine K1 takes. PATH1 is triangle lists only: no strip, fan, sprite, line or
   point; no FST, no fog, IIP always 1.
3. **The reference never samples a mip level** (it filters level 0 whenever MMAG or MMIN asks for linear), so the
   two mipmapped textures (177,492 primitives) need no mip chain to match it. Inherited divergence (section 14).
4. **K2's GPU time will be draws, not barriers:** 544 state runs a kick on average and up to 2,619, a third of them
   (races) or nearly all (front end) a change of texture only. At the 7 us a small draw cost in 19.5 that is about
   4 ms on average. Batch 2 measures it; reducing it is P2's (section 10.4). *Measured (18.5): wrong. A draw costs about
   0.4 us; K2's GPU time is its fragments, about 4 ns a covered pixel.*

## 1. Summary

- **Reader** (`render/vif/`, section 4): `Vif1Reader` walks a channel 1 kick with the host's chain walker, decodes
  the eight VIF codes into its own VIF1 registers and a 16 KB image of VU data memory (micro memory too, to know
  the program), and at each MSCAL runs the C++ reference of the loaded program; the packets go to the GIF reader
  of step 7 as PATH1. Anything else stops the kick with a named guard.
- **VU1 state** (section 5): data memory, micro memory, the 32 vector registers and the VIF1 registers persist
  across calls, kicks and frames; a replay seeds them from a keyframe's `VU1M`, `VU1R` and `VIF1`, the live reader
  starts from reset with the game. Programs are recognised by the hash of the uploaded image's prefix, as the
  runtime does it.
- **Floating point** (section 6): the references run under the kick's MXCSR with rounding toward zero, as the
  runtime runs the VU1. Check (b) is the recorded GIF hash and byte count per kick; beside the emulated machine it
  compares packet by packet and names the first differing run, packet and word.
- **One register file** (section 7): after a VIF1 kick the reader's file equals the recorded one on every register
  (1,603,839 kicks checked with the emulated VU1's packets); step 7's `path1Writes` and its "unknown after a
  channel 1 kick" go.
- **Draw list** (section 8): `STATE`, `TEXTURE` and `SCREEN_PRIMS` as K1; a K2 list is 5,500 triangles on average
  (17,768 at most) under 544 states (2,619): 0.6 MB (2.1 MB).
- **Drawing K2** (section 9): `Gs2dVk` and its shader as built, with nothing new in the per-pixel arithmetic
  (perspective Q, per-vertex Z in doubles, REPEAT, GEQUAL, alpha by blend are all present); new are the capacity,
  the tests for those paths, and the barrier rule.
- **The exact blend** (section 10): read mode by default, barriers by covered pixels.
- **Checks** (section 11): (a) VIF1, VU memory and registers at every later keyframe; (b) GIF bytes; (c) the
  reader's bytes and its draw list in front of the emulated GS; (d) Vulkan per K2 kick, seeded, equal in every
  plane; then the chained replay: seeded once, every kick through Vulkan, every flipped picture equal to the
  reference.
- **Live** (section 12): the reader on the EE thread in the capture's consumer list, the emulated machine still
  running beside it until P2.

## 2. Code layout

```
render/vif/vif1_reader.{h,cpp}  rc::rd::Vif1Reader: VIF1 registers, VU memory, VU registers; kick() walks and decodes,
                                runs programs, hands PATH1 packets to a sink; guards; stats
render/vif/vu1_programs.{h,cpp} the program table (section 5.3): image prefix -> reference, entry points
decomp/vu1/*_shape.cpp, vu_shape.{h,cpp}, vu1_ref.h
                                the references: one more entry point each (registers out, a packet callback);
                                the present functions stay as wrappers (RC_VU1_REF and tools/shader_check use them)
render/gs2d/gif2d_reader.*      feedPath1(); no path1Writes; cullOffscreen
render/gs2d/gs_cover.{h,cpp}    GsCoverMap: which pixels were written since the last barrier (section 10.2)
render/gs2d/gs2d_vk.*           barriers and split groups from GsCoverMap; capacity for a K2 list
render/frame/frame_reader.{h,cpp}  rc::rd::FrameReader, a rec::Consumer: Vif1Reader + Gif2dReader + step 6's FrameBlit;
                                one draw list per cycle (the live reader and rc_replay both use it)
tools/rc_replay/vif.{h,cpp}     the `vif` consumer: checks (a), (b), (c); faults
tools/rc_replay/frame_vk.*      the `vk` consumer: check (d) and the chained replay
tools/rc_replay/vif_survey.*    `vifstat` (this design's measuring tool; stays)
```
Libraries: `rc_vu1_ref` (the seven reference files, already a target for `vifstat`; `-O2 -fwrapv
-fno-strict-aliasing`, the project's `-ffp-contract=off`), `rc_vif` (on `rc_vu1_ref` and `rc_record`), `rc_frame`.
None includes a runtime header: the reader reads guest RAM through `RamView` and nothing else.

```cpp
namespace rc::rd {
struct Vif1Regs { uint32_t cycle, mode, mask, base, ofst, tops, itops, top, itop; bool dbf; uint32_t row[4], col[4]; };
struct Vu1Run   { int program; uint32_t pc, top, itop, index; uint32_t streamOffset; };   // index: MSCAL number in the kick
struct Path1Sink {
    virtual void runBegin(const Vu1Run &) {}
    virtual void packet(const uint8_t *data, uint32_t bytes) = 0;                         // one XGKICK
    virtual void runEnd(const Vu1Run &, const uint8_t *vuData, const float (*vf)[4]) {}  // state after the run
};
class Vif1Reader {
  public:
    void reset();                                         // the machine's reset state
    void seed(const rc::rec::Keyframe &);                 // VIF1, VU1M, VU1R
    void hw(uint32_t address, uint64_t value);            // VIF1 register stores (FBRST)
    bool kick(const rc::rec::KickView &, const rc::rec::RamView &, Path1Sink &);   // false: a guard stopped it
    const Vif1Regs &regs() const; const uint8_t *vuData() const; const uint8_t *vuMicro() const; const float (*vf() const)[4];
};
}
```

## 3. Measurements

### 3.1 Tool and dumps

`rc_replay <dump> --consumer vifstat [--with-emu]` (`tools/rc_replay/vif_survey.{h,cpp}`; `tools/vif_agg.py
<outputs>` sums the `V` lines and takes the maximum of the `X` lines). Without the machine it walks every channel 1
kick with `walkChain` and decodes the VIF codes with a model of the VIF1 registers seeded from the keyframe. With
`--with-emu` it sits in front of the `emu` consumer: every run of a generated VU1 program is wrapped, the C++
reference is run on a copy of VU memory with the same registers and its packets and final memory compared with the
emulated VU1's; a second run of the reference with every register lane marked shows which lanes reach the output;
every GIF packet of both paths is walked from the kick's recorded register file, primitives are assembled as the
GS does, planned with step 7's `planState`, and the reading primitives of each kick are grouped by the rule as
built and by five alternatives. `--vif-round-nearest` is the experiment of U1 (3.11).

Run over **the 67 exact dumps of both versions** (`build-decomp/frame-dumps/{s5,s6,b4-*}`, 34 files, 24,144 cycles;
`build-eu/frame-dumps`, 33 files, 21,415 cycles; boot with the movie, the front end, levels 1-24, a level load, the
Track Editor, two-player races): 1,603,979 kicks, of them **44,200 VIF1 kicks**, 43,076,205 VU1 runs, 556.2 M PATH1
triangles. Four at a time, about 45 minutes. Not in them (as in step 7): podium, results, memory card screens, the Track
Editor's race. `Rdr_VUShapeProg` never runs (as `NOTES.md` says: no shipped shape is version 2.0). To repeat:
`for d in <dumps>; do rc_replay $d --consumer vifstat --with-emu > out/$name.txt; done; tools/vif_agg.py out/*.txt`
(either tree's `rc_replay` reads both versions' dumps).

### 3.2 The VIF1 stream

Every channel 1 kick is a chain with TTE (the two words of each DMA tag are VIF codes).

| DMA tag | count | payload bytes |
|---|---|---|
| `cnt` (1) | 38,372,032 | 236.9 G |
| `ref` (3) | 22,569,536 | 4.7 G |
| `call` (5) / `ret` (6) | 4,877,996 each | 21.4 G after `ret` |
| `end` (7) | 44,200 | |

| VIF code | count | notes |
|---|---|---|
| NOP | 243,568,487 | |
| UNPACK V4-32 (0x6c) | 104,955,087 | 257.5 G of data; unmasked; 97.15 M relative to TOPS (FLG), 7.80 M absolute |
| STCYCL | 66,071,403 | always CL 4, WL 4 |
| MSCAL | 43,076,205 | |
| FLUSHA | 6,021,858 | no effect on the reader (PATH3 is never in flight inside a VIF1 kick) |
| BASE, OFFSET | 4,381,009 each | BASE 0x100 with OFFSET 0x180 (shapes), BASE 0 with OFFSET 0 (skin) |
| MPG | 2,376,250 | 4.1 G; five codes a program: 2,048 bytes at 0, 0x800, 0x1000, 0x1800, the rest at 0x2000 |
| every other code | 0 | STMOD, STMASK, STROW, STCOL, ITOP, MARK, MSKPATH3, FLUSH, FLUSHE, MSCALF, MSCNT, DIRECT, DIRECTHL, the other UNPACK formats, the mask bit, the interrupt bit |

- **TOPS:** TOP at an MSCAL is 0x100 (20.37 M), 0x280 (19.71 M) or 0 (2.99 M): the double buffer of 0x180
  quadwords at 0x100, and the skin program's single buffer.
- **Where UNPACK writes:** TOPS + 0..0x17f; absolute 0..0x3ff (the constants and, for skin, the bone matrices over
  all of VU memory). Nothing wraps past 0x3ff.
- **Packet boundaries:** 64,502,045 UNPACK codes and 475,250 MPG codes are the second word of a DMA tag with their
  data in that tag's payload. No code's data crosses any other boundary, no code is cut by the end of the stream,
  and no stream leaves bytes after its last code.
- **What a kick inherits:** MODE is never written (0 throughout); ITOPS never; 122,624 MSCALs run with BASE and
  OFFSET set in an earlier kick; 28.6 M MSCALs run a program an earlier kick uploaded. So the VIF1 registers and
  micro memory must persist across kicks, as data memory does.
- **VIF1 register stores** (HW events): ERR = 2 once a frame (44,257) and = 6 (57): no effect on decoding; FBRST =
  1 (RST) 114 times, at start-up.
- **The model against the dumps:** started from a keyframe's `VIF1` and `VU1M`, the decoded registers (CYCLE, MODE,
  MASK, BASE, OFST, TOPS, ITOPS, DBF) and micro memory equal the next keyframe's on all 131 later keyframes. Every
  one of the 200 keyframes has `VU1R`.
- **Size:** a kick is 5,800 KB of stream on average (11,450 KB at most), 1,600 DMA tags (6,363), 10,743 VIF codes
  (21,056), 2,375 UNPACKs (7,017), **975 MSCALs (1,882)**.

### 3.3 Programs and runs

| program (runtime image hash) | image prefix, bytes | entry: runs | packets compared | differing | cut short |
|---|---|---|---|---|---|
| `TRdr_VUShapeProg` (c3952c0faeb52ead) | 8,328 | 0: 32,642,628; 0x2050: 490,969 | 583,351,801 | 0 | 0 |
| `EnvRdr_VUShapeProg` (ab0e7743a9ba7393) | 9,128 | 0: 5,685,356; 0x2370: 240,949; 0x78: 0 | 111,484,295 | 0 | 0 |
| `Rdr_VUSkinProg` (ec08b22243d4dc8c) | 8,904 | 0: 2,978,048 | 92,393,893 | 0 | 0 |
| `Rdr_VUWaterProg` (e96770e4143d2da7) | 9,184 | 0: 1,028,995 | 258,235,856 | 0 | **1,095** |
| `TERdr_VUShapeProg` (bac581270f262987) | 8,656 | 0: 9,260; 0x2198: 0 | 270,823 | 0 | 0 |
| `Rdr_VUShapeProg` (6ec8b9ff8fa44221) | - | never | | | |

- Every MSCAL of every dump starts one of these at one of these addresses; no run is ever re-entered at another
  address and no PATH1 packet comes from anything else. The microcode is the same in both versions.
- **0 packets differ, 0 runs differ in packet count, 0 runs leave different VU memory** (16 KB compared after every
  run), over 1,045,736,668 packets. TERdr is now covered on recorded Track Editor frames of both versions.
- **Cut short:** the runtime stops a VU1 run after 65,536 cycles and never resumes it (`NOTES.md`). 1,095 water
  runs in 23 dumps (levels 5, 6, 9, 11, 12, 17, 18, 21, 23, 24, the front end's water, the two-player dumps) end
  that way; the reference goes on, as the hardware would, so on those kicks its bytes are a superset of the
  recorded ones. Section 6.3.
- **A program's image:** micro memory after an upload is the program followed by whatever a longer program left
  (353,000 of the 475,000 uploads) or by zeros; the whole-image hash is therefore not the key (5.3).
- Direct entries (0x2050, 0x2370, 0x2198) send the packet at TOP unchanged: state packets and ready-made polygons.

### 3.4 Registers between runs

The references take the 32 vector registers a run starts with. Running each reference a second time with every
lane of VF01-VF31 replaced by a marker shows what of them reaches the output:

| program | lanes of the incoming registers found in the packets (occurrences) | values | last changed by |
|---|---|---|---|
| TRdr, Env, TERdr | VF05.w, VF06.w, VF07.w (324.9 M, 101.5 M, 0.16 M each) | always 0 | nothing since the keyframe, in any dump |
| TRdr, Env | VF29.w (6,005; 8) | 0x80000000, 0, 1.0 | TRdr, water, skin, Env |
| water | VF25.w (38.3 M), VF26.w (39.8 M), VF29.w (0.82 M) | integers left by a colour or position conversion (0x80, 0x7f, 0x100, ...) | water itself (85%), TRdr, skin, Env |
| skin | none | | |

- No run's packets depend on an incoming register in any other way (no value computed from one, no change in the
  number of packets); a run always starts with the registers the previous run left.
- These lanes are the fourth word of PACKED vertex data, whose bit 15 is the GS's ADC flag (21.3 M PATH1 vertices
  arrive as XYZ3, without the drawing kick; none of them is the third vertex of a triangle). So they are part of
  the bytes check (b) compares and could, in principle, hide a triangle: they have to be right.
- What the programs change: most lanes of VF01-VF31, VI01-VI15, ACC and Q (water: P too). The env program's 0x78
  entry ("the constants still in registers") would read whole registers; it never runs.

### 3.5 PATH1's packets and registers

1,045,736,668 packets, all one PACKED tag with EOP:

| first tag | packets | what |
|---|---|---|
| NLOOP 3, NREG 3, REGS ST RGBAQ XYZ2, 160 bytes | 556,235,415 | one triangle |
| NLOOP 0, 16 bytes | 209,169,750 | the flush when the packet buffer wraps |
| NLOOP 1, REGS PRIM A+D A+D ... TEX0 ... (48, 80, 112, 144 bytes) | 276,919,572 | texture and state blocks |
| NLOOP 6, NREG 1, REGS A+D, 112 bytes | 3,411,931 | the game's state packets (SCISSOR, ZBUF, ...) |

Registers PATH1 writes: PRIM (0x5b, 0x1b), RGBAQ, ST, XYZ2 (XYZ3 by ADC), TEX0_1, CLAMP_1 (0, 5), TEX1_1
(0xf6000000160, 0x60, 0xf600000016c, 0xf6000000170), MIPTBP1_1, MIPTBP2_1, TEXA (0x80), SCISSOR_1 (3..637 x 3..237
USA, 3..253 Europe, and the two halves of a split screen), ALPHA_1, TEST_1, PABE, ZBUF_1 (0x10000a0, 0x1010000a0).
Never: FRAME, XYOFFSET, PRMODECONT, COLCLAMP, DTHE, FBA, context 2, fog. **After every one of the 1,603,839 kicks
that have a successor, the register file made by walking the kick's packets from its recorded file equals the next
kick's recorded file** (every register but the vertex ones).

### 3.6 Draw states (PATH1, both versions; pixels by the reference's coverage inside the scissor)

| primitives | pixels | PRIM | ALPHA | TEST | PABE | ZMSK | TEX1, CLAMP | plan |
|---|---|---|---|---|---|---|---|---|
| 358,569,448 | 14.40 G | 0x5b | 0x44 `(Cs-Cd)*As+Cd` | LEQUAL 0x2f KEEP, Z GREATER | 1 | 0 | 0xf6..160, clamp | OFF (PABE under its precondition) |
| 76,564,072 | 0.75 G | 0x1b | - | LEQUAL 0x2f KEEP, GREATER | 0 | 0 | 0xf6..160, clamp | OFF |
| 34,070,315 | 0.51 G | 0x5b | 0x29 FIX 0x80 `Cd+Cs` | LEQUAL 0x2f KEEP, **GEQUAL** | 0 | 0 | 0xf6..160, clamp | MIX, exact (the env pass) |
| 15,686,563 | 2.04 G | 0x5b | 0x09 `Cd*As+Cs` | off, GREATER | 0 | 1 | 0xf6..160, **repeat** | MIX, **reads** (water, pass 1) |
| 15,686,327 | 2.04 G | 0x5b | 0x62 FIX 0x15 `Cd-Cs*F` | off, GEQUAL | 0 | 1 | repeat | MIXSUB, exact (water, pass 2) |
| 15,686,046 | 2.04 G | 0x5b | 0x68 FIX 0x15 `Cs*F+Cd` | off, GEQUAL | 0 | 1 | repeat | MIX, exact (water, pass 3) |
| 9,767,107 | 2.06 G | 0x5b | 0x29 FIX 0x80 | LEQUAL 0x3f KEEP, GREATER | 0 | 1 | 0x60, clamp | MIX, exact |
| 7,885,477 | 2.69 G | 0x5b | 0x08 `Cs*As+Cs` | LEQUAL 0x2f KEEP, GREATER | 0 | 0 | 0xf6..160, clamp | OFF |
| 7,013,793 | 2.91 G | 0x5b | 0x68 FIX 0x80 `Cs+Cd` | off, GREATER | 0 | 1 | 0xf6..160, clamp | MIX, exact |
| 4,348,472 | 1.09 G | 0x5b | 0xa1 FIX 0x80 `Cd-Cs` | LEQUAL 0x3f KEEP, GREATER | 0 | 1 | 0x60, clamp | MIXSUB, exact |
| 3,824,272 | 0.18 G | 0x5b | 0x01 `(Cd-Cs)*As+Cs` | off, GREATER | 0 | 1 | 0xf6..160, clamp | MIX, **reads** |
| 3,326,592 | 0.05 G | 0x5b | 0x29 FIX 0x7f | LEQUAL 0x2f KEEP, GREATER | 0 | 1 | 0xf6..160, clamp | MIX, **reads** |
| 3,307,477 | 1.07 G | 0x5b | 0x01 | LEQUAL 0x3f **RGB_ONLY**, GREATER | 0 | 1 | 0x60, clamp | MIX, **reads**, alpha by blend |
| 177,492 | 0.5 M | 0x5b | 0x44 | LEQUAL 0x2f KEEP, GREATER | 1 | 0 | MXL 3 / MXL 4 | OFF; guard `MIPMAP` (level 0, as the reference) |
| 321,962 | 0.16 G | | 0x29 / 0xa1 FIX 0x80 with repeat or no test; 0x1b with 0x60; 0x21 FIX 0x80 PABE 1 | | | | | exact or OFF |

- Plans: OFF with Z write 79.7% of the primitives; exact fixed-function blends 15.6% (34.1 M of them writing Z, with
  GEQUAL); **reading 4.7% (26,144,904 primitives, 3.35 G pixels)**, all with ZMSK set. No split plan (the one
  RGB_ONLY state does not write Z). Guards: `MIPMAP` only.
- Environment at a draw: FRAME 0xa0000 / 0xa0050, XYOFFSET (1728, 1928) USA / (1728, 1920) Europe with no
  fraction, COLCLAMP 1, DTHE 0, PRMODECONT 1, TEXA 0x80: all from PATH3.
- Textures: T4 (0x14) and T8 (0x13) with CT32 palettes (CSM1, CSA 0), CT32 (0x00); 16x8 to 256x128; always TCC 1,
  MODULATE.

### 3.7 Geometry (PATH1, 556,235,415 triangles)

| measured | count |
|---|---|
| Q other than 1 at a vertex; Q differing between the vertices | all; all but 135,360 |
| Z differing between the vertices | 541,209,996 |
| a subpixel vertex | all but 913 |
| Gouraud RGB (IIP is always 1); alpha differing between the vertices | 374,035,488; 296 |
| **wholly outside the scissor** | **305,804,973 (55.0%)** |
| zero area | 6,373,567 |
| cut by the scissor | 257,437,719 |
| a vertex outside the picture by up to 64 / 512 / 1024 / 2048 pixels; further | 41.5 M / 121.0 M / 55.6 M / 39.2 M; **0** |
| S/Q, T/Q within 0..1; up to 2 or negative; up to 8; up to 64; up to 1024; beyond | 509.2 M; 46.2 M; 578,389; 151,302; 14,928; 32,890 |
| scissored box of the drawn ones: 1 pixel; 2-16; 17-256; 257-4,096; larger | 17.6 M; 93.3 M; 103.9 M; 25.6 M; 3.7 M |
| vertex Z | 0 to 0x165acc4; at or above 2^24: **one vertex** (Europe, level 22) |

The programs clip to a guard band, not to the screen: no vertex is more than 2,048 pixels outside the picture, the
range `Gs2dVk`'s viewport holds exactly (`RENDERER_2D_DESIGN.md` 8.1).

### 3.8 A K2 kick (44,200 kicks; mean, largest)

| | mean | largest |
|---|---|---|
| PATH1 packets; bytes | 23,659; 2.6 MB | 63,013; 7.0 MB |
| triangles; drawn (some area inside the scissor) | 12,585; **5,522** | 38,306; **17,768** |
| state runs (consecutive drawn triangles under one `GsState`) | **544** | **2,619** (13% of the kicks have more than 1,024) |
| ... that differ from the run before only in the texture (measured on 3 dumps) | 36-37% in races, 98% in the front end | |
| reading primitives; reading state runs | 228; 88 | 1,426; 568 |
| pixels covered; by reading primitives | 724 K; 75 K | 5.47 M; 1.09 M |

K1 for comparison (44,258 kicks): 30 primitives drawn on average, 340 at most; 5 state runs (81); 26 reading
primitives (256). The Track Editor's K2 is 160 triangles; the front end's is a full 3D scene (5,465 drawn, 669
state runs on average).

### 3.9 Reading primitives and barriers

Barriers a kick would issue for the exact blend (`RENDERER_2D_DESIGN.md` 18), by rule. "As built": a group is a
run of primitives of one batch whose scissored boxes are pairwise disjoint, a barrier before each group. The
others issue a barrier before a reading primitive only if it touches something drawn since the last barrier, with
"touches" decided by boxes, by 8x8 or 4x4 tiles the triangle can cover, or by the pixels it covers (the
reference's coverage rule, `gsTriCovers`).

| rule | K2 mean | K2 largest | K1 mean | K1 largest |
|---|---|---|---|---|
| as built (box groups inside a batch) | 173.4 | 879 | 12.1 | 119 |
| a group continues across consecutive reading batches | 169.8 | 872 | 11.2 | 102 |
| dirty boxes | 135.8 | 778 | 10.5 | 98 |
| dirty 8x8 tiles by coverage | 149.6 | 1,029 | 10.9 | 103 |
| dirty 4x4 tiles by coverage | 134.6 | 898 | 10.6 | 100 |
| **dirty pixels** | **16.2** | **85** | **10.3** | **96** |

- K2 kicks by barriers as built: 0-16: 2,084; 17-64: 9,222; 65-128: 8,424; 129-256: 14,752; 257-512: 8,885;
  513-879: 833. By dirty pixels: 0-4: 10,938; 5-16: 13,153; 17-32: 14,861; 33-64: 5,196; 65-85: 52.
- Why only the pixel rule helps K2: the reading primitives are meshes (water above all: every triangle is drawn
  three times, the first pass reading). Neighbours share an edge, so their boxes and their tiles overlap, but the
  fill rule gives every pixel to one triangle; the second and third passes dirty exactly the pixels the same
  triangle just read.
- Why nothing helps K1 much: its layers really do overlap (text on a panel on a backdrop), two barriers a panel.
- A whole cycle (K2 then K1): 185 barriers on average and 882 at most as built; **26.4 and 126 by dirty pixels**.
- Draws: as built K2 needs 630 on average (2,810 at most); by dirty pixels 557 (2,665), i.e. the state runs plus
  the barriers that fall inside one. K1: 17.7 (171) against 12.8 (123).
- What the pixel rule has to look at: only what is drawn from a kick's first reading primitive on (one barrier
  covers everything before it): 626 primitives and 303 K pixels a K2 kick on average, 5,181 and 5.04 M at most;
  K1 27 primitives and 5 K pixels (262, 68 K).

### 3.10 The references' time

Wall time of the reference calls alone (one run per MSCAL, `-O2`), Ryzen 7 3750H:

| dump | per K2 kick, mean | largest | conditions |
|---|---|---|---|
| USA `s5/l17` | 26.4 ms | 42.1 ms | one process |
| USA `s5/front` | 22.3 ms | 32.7 ms | two processes |
| USA `s5/l18` | 46.6 ms | 69.3 ms | one process |
| USA `s5/l6` | 42.2 ms | 68.6 ms | one process |
| all 67 | 35.6 ms | 222 ms | four processes on four cores (inflated; for the ranking only: levels 6, 24, 18, 9, 21, 12 are the heaviest) |

About 1 us a packet: the references reproduce the VU's arithmetic operation by operation (each result checked for
overflow and underflow in double precision). By program on level 17: TRdr 48%, water 22%, Env 18%, skin 12%; on
level 18 TRdr 75%.

### 3.11 The rounding experiment (U1)

`--vif-round-nearest`: while a VU1 program runs, its packets are held back (the runtime's own `Vu1PacketSink`), and
handed to the GS after the run, in the same order, with the rounding bits of MXCSR cleared, i.e. under the mode
the kick was made in. Nothing else changes. A plain replay of the same cycles gives the pictures to compare with
(`RC_VIFSTAT_PICTURES=<file>` on both runs).

| dump (USA) | cycles | pixels differing | R: +1, +2, +3 or more, -1 | G: +1, +2, +3 or more, -1 | alpha |
|---|---|---|---|---|---|
| `s5/l5` | 401 | 19,617,124 of 61,593,600 (31.8%) | 12.6%, 1.5%, 0.14%, 3.5% | 15.5%, 2.5%, 0.35%, 8.4% | +1 on 0.08% |
| `s5/l17` | 401 | 20,470,200 of 61,593,600 (33.2%) | 13.8%, 1.6%, 0.19%, 4.5% | 16.1%, 3.1%, 0.55%, 8.4% | +1 on 0.08% |
| `s5/front` | 501 | 4,690,549 of 76,953,600 (6.1%) | 2.6%, 0.8%, 0.005%, 0.14% | 2.3%, 0.9%, 0.005%, 0.09% | +1 on 1.1% |

MXCSR at the packets, the same dumps: PATH1 0x7fb2 (and 0x7fa2 in 25,108 front-end packets), PATH3 0x1fb2, 0x1fa0,
0x1fa2; the kicks themselves are all made under 0x1fb2, 0x1fa0 or 0x1fa2 (round to nearest; the values differ in
sticky exception flags only).

## 4. The VIF1 walk

**Input.** `walkChain(kick, ram, visitor)` (`render/record/chain_walk`) delivers the stream as ranges: for each
DMA tag its upper 8 bytes (TTE), then its payload. The reader consumes them in order with a small state machine,
copying nothing but what a code writes: `{ code in progress, data bytes still to come, destination }`. A code whose
data starts in the next range (the tag case of 3.2) simply continues there. A normal-mode channel 1 kick (never
seen) is one range and decodes the same way.

**Codes** (`cmd`: IMMEDIATE bits 0-15, NUM 16-23, CMD 24-30, bit 31 the interrupt):

| code | effect on the reader |
|---|---|
| NOP 0x00 | none |
| STCYCL 0x01 | `cycle = IMMEDIATE` |
| OFFSET 0x02 | `ofst = IMMEDIATE & 0x3ff; tops = base; dbf = false` |
| BASE 0x03 | `base = IMMEDIATE & 0x3ff` |
| ITOP 0x04, STMOD 0x05, STMASK 0x20, STROW 0x30, STCOL 0x31 | the register is set (4, 4, 16 and 16 data bytes for the last three); see UNPACK |
| FLUSHE 0x10, FLUSH 0x11, FLUSHA 0x13, MARK 0x07 | none (every run is complete before the next code, as in the runtime) |
| MPG 0x4a | `(NUM ? NUM : 256) * 8` bytes copied to micro memory at `IMMEDIATE * 8` (clipped at 16 KB); the program is looked up again at the next MSCAL |
| UNPACK 0x60-0x7f | below |
| MSCAL 0x14 | `top = tops; itop = itops; tops = dbf ? base : base + ofst; dbf = !dbf`; run the program at `IMMEDIATE * 8` (section 5) |
| MSCALF 0x15, MSCNT 0x17, DIRECT 0x50, DIRECTHL 0x51, MSKPATH3 0x06, any other | guard `VIF_CODE` |

These are the runtime's rules (`RT/src/lib/ps2_vif1_interpreter.cpp`), which check (a) holds the reader to.

**UNPACK.** Implemented: V4-32 with CL equal to WL, MODE 0, no mask: `(NUM ? NUM : 256)` quadwords copied to VU
data memory at `(IMMEDIATE & 0x3ff) + (bit 15 ? tops : 0)`, modulo 0x400 (wrapping is implemented though never
seen). Bit 14 (unsigned) does not matter for 32-bit data. Any other format, the mask bit (0x10), CL different from
WL, or MODE other than 0 at an UNPACK: guard `VIF_UNPACK` (the general case is forty lines in the runtime; it is
left out because nothing would test it).

**Guards.** `VIF_CODE`, `VIF_UNPACK`, `VIF_TRUNCATED` (a code or its data cut by the end of the stream, bytes left
over), `VIF_PAGE` (a range on a page the RAM view does not hold: off-line only), `VU_PROGRAM` and `VU_ENTRY`
(section 5). A guard stops the kick there: the packets made so far stand, the registers PATH1 can write (3.5) are
marked unknown as step 7 marked them after every channel 1 kick, the guard is counted and logged once with the
kick, the stream offset and the code. Off-line every guard fails the check; live the frame is drawn without the
rest of its 3D. None fires in the 67 dumps.

**VIF1 register stores** (`hw()`): a store to FBRST (0x10003c10) with bit 0 resets the reader's VIF1 registers as
`RT/src/lib/ps2_memory.cpp` resets them; ERR, MARK and the rest are ignored.

**Interrupt bit, PATH3 masking, VIF1 stalls:** not modelled (never used); the interrupt bit on a code is counted.

## 5. VU1 state and the program table

### 5.1 What persists

| state | size | written by | read by |
|---|---|---|---|
| VU data memory | 16 KB | UNPACK; the programs (packets, clip lists, return addresses) | the programs |
| VU micro memory | 16 KB | MPG | the reader only, to know the program (the references do not read it) |
| vector registers VF00-VF31 | 32 x 4 floats | the programs | the programs (3.4) |
| VIF1 registers | `Vif1Regs` | the codes | the codes |

Integer registers, ACC, Q, P, I, the flags and the pipeline are not kept: no reference takes them and 1,045.7 M
packets and 43 M memory images agree without them. MSCNT, which would continue from the last run's program
counter, is a guard.

All four live in `Vif1Reader` for the life of the session. `reset()` is the machine's reset: memories and
registers zero, VF00 = (0, 0, 0, 1). `seed(keyframe)` copies `VIF1` (CYCLE, MODE, MASK, BASE, OFST, TOPS, ITOPS,
TOP, ITOP, STAT bit 7 as DBF, ROW, COL), `VU1M` (micro, then data) and `VU1R` (`vf`). All three chunks exist in
every keyframe of every dump (3.2): **no change to the dump format is needed.** The live reader calls `reset()`
when the capture attaches at boot and sees every kick from there; after a save-state load it seeds from the same
three chunks, which the capture already builds for the keyframe it writes then (`render/record/emu_state.cpp`).

### 5.2 Running a program

At an MSCAL the reader looks up the program (5.3) and the entry, sets MXCSR (section 6), and calls the reference
with VU data memory in place, the registers in place, the start address and `top`. The reference appends its
packets through the sink, changes memory as the program does, and leaves the registers as the program does.

**The references' new entry point** (batch 1; `decomp/vu1/vu1_ref.h`):
```cpp
namespace rc::vu1 {
struct Io {
    uint8_t *memory;                 // 16 KB of VU data memory, changed in place
    float (*vf)[4];                  // 32 registers in, and out as the program leaves them
    uint32_t startPc, top;
    void (*packet)(void *user, const uint8_t *data, uint32_t bytes);   // one XGKICK
    void *user;
};
enum Program { kTrdr, kEnv, kWater, kSkin, kTerdr, kRdr };
bool run(Program, Io &);             // false: the start address is not an entry of that program
}
```
The present `run*Shape(memory, registers, startPc, top, kicks)` functions stay as wrappers (they copy the
registers and collect a `KickList`), so `RC_VU1_REF`, `RC_VU1_SYNTH` and `tools/shader_check` are unchanged.
`ShapeProgram` already holds the registers in `r[32]`; what batch 1 has to establish is that they are the VU's
when the program ends, at least on the lanes of 3.4 and then on every lane it can make equal. Check (a) compares
them with the emulated VU1's after every run (section 11.1); a lane a reference does not reproduce is listed in
the as-built section with the proof that no program lets it reach a packet, and the reader writes a signalling
NaN pattern into it after each run of that program, so a later dependence fails check (b) at once instead of
passing by luck.

### 5.3 Which program is which

The runtime's rule, kept: a program is recognised by the FNV-1a 64 hash of micro memory from 0 to the program's
length (what follows it is another program's tail or zeros, 3.3). The table:

| program | length, bytes | entries (byte addresses) | reference |
|---|---|---|---|
| `TRdr_VUShapeProg` | 8,328 | 0 (a shape); 0x2050 (the packet at TOP) | `trdr_shape.cpp` |
| `EnvRdr_VUShapeProg` | 9,128 | 0; 0x78 (second pass, constants kept: not seen); 0x2370 (packet at TOP) | `env_shape.cpp` |
| `Rdr_VUSkinProg` | 8,904 | 0 | `skin_shape.cpp` |
| `Rdr_VUWaterProg` | 9,184 | 0 | `water_shape.cpp` |
| `TERdr_VUShapeProg` | 8,656 | 0; 0x2198 (packet at TOP: not seen) | `terdr_shape.cpp` |
| `Rdr_VUShapeProg` | from its image | 0 | `rdr_shape.cpp` (synthetic checks only; never runs) |

`vu1_programs.cpp` holds `{length, prefix hash, Program, entries}`; batch 1 takes the lengths and hashes from the
images `tools/vu1dis.py` / `tools/make_vu1_images.py` write and confirms them against the dumps (`vifstat` prints
both where another program's tail follows: TRdr 8,328 bytes, 5666277de85aa04d; Env 9,128, f60f50c9db326773; skin
8,904, 9f7cf923ab686a0c; water's length is its five MPG codes', 8,192 + 992). The
lookup runs once after an upload, at the next MSCAL: one FNV pass over the first 9,184 bytes gives the hash at all
six lengths (eleven program changes a kick: 0.1 MB hashed).

**No reference:** an image that matches no entry is guard `VU_PROGRAM`, a start address that is not an entry
`VU_ENTRY` (section 4: the kick stops, PATH1's registers become unknown, logged with the image hash and address).
There is no interpreter in the renderer; a new program means a new reference, checked by `RC_VU1_REF` first.

## 6. Floating point

### 6.1 The mode

The runtime executes a VU1 program with `MXCSR = (the EE thread's MXCSR at the kick) | 0x6000` and restores the
rounding bits afterwards. The reader does the same around each reference: `KickView::mxcsr | 0x6000` off-line
(every recorded value is 0x1fb2, 0x1fa0 or 0x1fa2), `_mm_getcsr() | 0x6000` live (it is on the EE thread inside
the kick's store), restored after the run. Everything else the kick produces (the GIF walk, the draw list) is
integer work or copies.

### 6.2 What the references reproduce (`decomp/vu1/vu_shape.h`)

- operands normalised as the VU sees them: denormals are zero, infinities and NaNs the largest finite value;
- results: overflow clamps to the largest finite value with the exact result's sign, underflow gives a signed
  zero (decided on the exact result in double precision; the value is the single-precision one, truncated);
- MADD and MSUB: the product is rounded to single precision before the sum;
- division: by zero gives the largest value with the sign of the operands' signs; ERLENG of a zero vector is zero;
- float to integer: truncation, saturating at 0x7fffffff; integer to float exact;
- every step in the microcode's order (the shared clipping text in `vu_shape.cpp`, a main loop per program).

Evidence: 3.3. The six files are compiled as the game compiles them (`-O2 -fwrapv -fno-strict-aliasing`,
`-ffp-contract=off`, no fast-math); `rc_vu1_ref` fixes those flags for every user.

### 6.3 Check (b) and how it reports

- **Per kick, no machine:** the reader's PATH1 bytes of a channel 1 kick, hashed with `Hash64` in order, equal
  `KickView::gifHash`, and their count `gifBytes`. This is the whole of check (b) on a dump whose runs were not cut
  short, and it is fast (the references' time: under ten minutes for the 67 dumps, four at a time).
- **Beside the machine (`--with-emu`):** the `vif` consumer runs in front of `emu` and wraps the six generated
  programs as `vifstat` does. Per run it compares, packet by packet, the reader's packets with the emulated VU1's
  as they reach the machine's GIF tap, then VU data memory and the vector registers after the run. The first
  difference stops the dump and prints: cycle, kick, MSCAL number and stream offset, program, entry, TOP, the
  packet's number in the run, the byte offset, the two words, the GIF register that word belongs to; and writes
  `<out>/vu1-<cycle>-<kick>-<run>.bin` (VU data memory before the run, the registers before it, `top`, both packet
  lists), the format `tools/shader_check` reads, so the run can be repeated alone.
- **Runs cut short.** Until the runtime's limit is lifted (U1's batch R), a run the emulated VU1 abandoned at its
  cycle budget is compared up to the emulated packets' end and counted; its kick is excused from the hash
  comparison only beside the machine, where the cut is seen, and fails it otherwise (23 dumps have such kicks:
  the list is in the tool's output). After batch R no run is cut and the excuse is removed.

## 7. PATH1's registers: one register file

`Gif2dReader` gets `feedPath1(const uint8_t *, uint32_t)`: one XGKICK packet through a second `GifWalker` (a GIF
tag never spans PATH1 packets: every packet is one tag with EOP, 3.5; the walker reports one that does) into the
same register file, vertex queue and draw list as PATH3. `path1Writes()`, the clearing of `known` at a channel 1
kick and the provenance value `kProvUnknown` after one are removed; a register is unknown only before its first
write in a session or a segment (seeded off-line from `GSRG`), or after a guard of section 4.

**Check (a) for the registers** is step 7's check (a) with nothing excused: after every kick of either channel the
reader's file equals the next kick's recorded `regs` on every register it has written or been seeded with, and
after the first draw environment of a dump no draw needs a seeded or defaulted register. This is all step 7's
batch 5 needs from step 8, and it needs the references in full: the state a kick leaves depends on which packets
the programs sent (water sends state between its passes, per triangle that survives clipping), so there is no
shorter walk that gives the registers exactly.

FRAME, XYOFFSET, PRMODECONT, COLCLAMP and DTHE reach PATH1's draws from the flip's draw environment (PATH3), TEXA
reaches PATH3's CT16 texture from PATH1: both fall out of the one file.

## 8. The draw list for K2

Step 8 emits what step 7 emits: `STATE` (128 bytes, on change), `TEXTURE` (first use of a cache entry's
generation), `SCREEN_PRIMS` (triangles, 3 x 32-byte `GsVertex`), scope commands. `SHAPE_BATCH` is step 9's.

- **Dropped by the reader** (`Gif2dReader::Options::cullOffscreen`, on live and in the `vk` consumer, off for
  check (c)'s re-encoding): a triangle of zero area, and one whose bounding box (the reference's
  `gsTriangleSetup` bounds) misses the scissor. Both tests are the reference's own early exits, so no pixel and no
  later state depends on them. 56% of PATH1.
- **Size of a K2 list:** 5,522 triangles on average, 17,768 at most: 0.53 MB and 1.7 MB of vertices; 544 states
  (2,619): 87 KB (420 KB) with their command headers; scope commands as tagged. A cycle's list (K2, K1, the clear)
  is under 2.2 MB. Textures: `TEXTURE` payloads only for entries whose generation the presenter has not been sent
  (step 5's cache; a race frame uploads about 80 KB of texture data, survey, so a few textures are re-sent
  every frame).
- **Side tags.** Tags anchored to a chain slot (`KickView::spans`) are passed through as `SCOPE_BEGIN` / `SCOPE_END`
  around the packets of the MSCALs between their begin and end offsets. Nothing in P1 reads them.
- **Cost on the reader's thread, a K2 kick:** the references 22-47 ms on average by scene (3.10; the cost that
  matters); the VIF walk copies 5.8 MB of UNPACK data into VU memory in 2,400 pieces; the GIF walk reads 2.6 MB of
  packets; the
  off-screen test and the list about 1 us a triangle. Batch 1 reports the three figures separately.

## 9. Drawing K2 through step 7's path

### 9.1 Plans and guards

`planState` already gives every PATH1 state of 3.6 a plan, and no guard but `MIPMAP` fires:

| state | plan as built | note |
|---|---|---|
| 0x44 with PABE 1, LEQUAL 0x2f KEEP | blend OFF, alpha test as a discard | the precondition of `RENDERER_FRAME_RECORD.md` 17.3; its guard `PABE_BLEND` would fire on any other test |
| no ABE (PRIM 0x1b), 0x08 | OFF; 0x08's `Cs*As+Cs` computed in the shader | |
| 0x29 / 0x68 FIX 0x80, 0x68 FIX 0x15, 0xa1 FIX 0x80, 0x62 FIX 0x15 | MIX / MIXSUB, integer exact, on the blender | no read, no barrier, in either mode |
| 0x09, 0x01, 0x29 FIX 0x7f | reading (`READ` pipelines) | section 10 |
| 0x01 with LEQUAL 0x3f RGB_ONLY, ZMSK | reading, alpha by blend: a failing pixel keeps the stored alpha | 18.2 of the 2D design; no split, because Z is not written |
| ZTST GEQUAL (the env pass, water's passes 2 and 3) | `RHI_CMP_GEQUAL` on the `D32F` buffer: exact (2D design 7.1) | pipelines exist |
| TEX1 with MXL 3 / 4 | level 0, bilinear; guard `MIPMAP` counted but not a failure | what the reference does (information item 3) |
| 0x21 FIX 0x80 with PABE 1, LEQUAL 0x3f KEEP | OFF | 17.3 |

New guards for step 8: none in the plan. In `Gs2dVk`: `beyondViewport` (a vertex more than 2,048 pixels from the
window origin: counted today, never seen on PATH1) becomes a failure of check (d); the one vertex with Z at or
above 2^24 (3.7) takes the existing clamp-and-count path, and batch 2 looks at what the reference writes for it
(a Z24 buffer keeps 24 bits) and makes the model and the shader do the same if a pixel depends on it.

### 9.2 The arithmetic

Every PATH1 triangle takes the reference's `DrawTriangleFast` (information item 2), the routine section 6 of the
2D design transcribes. What K2 exercises that K1 did not, and where it already is:

| K2 uses | in the shader and the model | tested by |
|---|---|---|
| Q per vertex (perspective) | `iq`, `1/Q` by a double division (15.1 item 5) | T6 (one quad), T7 (six triangles, Q 0.25..3) |
| Z per vertex | `z0 + (z1-z0)*double(w1) + (z2-z0)*double(w2)`, `+0.5`, truncated (`gs2d.frag`, `gsTriPixel`) | **nothing yet** (K1's Z is flat): T9 |
| Gouraud colour and alpha, then MODULATE, then the alpha test | as K1 | T3, T6; T9 adds a ramp across LEQUAL 0x2f |
| REPEAT, coordinates far outside 0..1 | `i & (size - 1)` on the integer texel coordinate | T6 (wraps at the borders); T9 adds +-1,000 texels |
| CT32 textures, T8 / T4 with CT32 palettes, up to 256x128 | step 5's decode | T6 |
| subpixel vertices, slivers, large triangles, the scissor | section 8 of the 2D design | T5 (to 1,500 pixels off screen); T9 to 2,047 |
| vertices with ADC (XYZ3) before the kick | the reader's assembly | batch 1's check (c) |

So no new shader arithmetic is expected. **With U1 answered A the K1 arithmetic is the reference's for PATH1 too.**
With B, a `kPrimRoundZero` record flag selects the directed-rounding build for PATH1's primitives (not designed
here: it would be its own design note); with C nothing changes and check (d) is statistical.

### 9.3 Rasterisation, capacity, textures

- Viewport +-2,048 pixels, half-pixel shift, scissor from SCISSOR_1 as K1: PATH1's vertices stay inside (3.7).
- `Gs2dVk` sizes: a list of up to 18,000 triangle records (44 words each: 3.2 MB), 3,000 draws, 300 textures
  bound in a kick; the record buffer and the draw array grow instead of being fixed. Records are written once per
  list, as now.
- State is rebound per draw today (three bind groups, the scissor). Batch 2 binds only what changed between
  consecutive draws; that is the one optimisation in scope (section 10.4).
- Textures come from `TEXTURE` commands as in step 7; the level textures are uploaded by PATH3 kicks during the
  load and by a few kicks a frame afterwards, all already through `TexShadow`.

## 10. The exact blend: the cost decision

### 10.1 Decision

| question (19.5) | decision |
|---|---|
| K1 live (step 7 batch 5) | **read mode** (`kBlendAuto`: the local read where the device has it), as built, box groups: 1.28 ms of GPU time a front-end kick on average, 3.58 ms at most, 0.19 / 0.37 ms in a race, on the RTX 2060. That is a fifth of a frame in the worst menu, with nothing else of weight in the frame until K2 is drawn. |
| K2 | **read mode**, with barriers by covered pixels (10.2). The local read serves K2; **the interlock form is not built.** |
| K1 after step 8's batch 2 | the same rule as K2 (one mechanism in `Gs2dVk`): 96 barriers at most instead of 119, 123 draws instead of 171 |
| fixed function | stays as built: the fallback where `localRead` is missing, and a switch (`RC_VK_BLEND=ff`, `--vk-blend ff`) for comparison and for the eight-dump regression. Not a live default on any device that can read. |
| carrying groups across reading batches (18.5's first lever) | measured, not worth building: 2% fewer barriers on K2, 7% on K1 (3.9) |

Reasons. (i) The project's order is behaviour-matching and exact verification now, speed in P2. With read mode
live, the picture on the screen is the picture the checks prove, by one code path; with fixed function live, the
live path would be checked only by the eight-dump regression and within a band (the band hid a one-unit FIX
fault on all but 265 pixels, 16.3). (ii) The cost left after 10.2 is small against a frame: 126 barriers at most
in a cycle is 2.6 ms on the RTX 2060 at the measured 20.6 us, 0.54 ms on average. (iii) The interlock form (storage
images, Z test in the shader) was the reserve for barrier counts in the high hundreds; the pixel rule removes
those, and the interlock form would cost a batch and the fixed-function depth test. (iv) What does stand between
step 8 and 60 fps is not the blend: the references on the CPU (U2), and the number of draws (10.4).

60 / 50 fps live: the blend's share is inside the budget on the development GPU; the frame as a whole is P2's
(U2). If P2's performance pass finds the barriers matter on a weaker device, the switch exists.

### 10.2 The rule: barriers by covered pixels

`render/gs2d/gs_cover.{h,cpp}`, `rc::rd::GsCoverMap`: a 640 x 256 map of two 16-bit stamps a pixel (`read`,
`split`), each compared with a current epoch; `covers(tri, box)` yields the pixels of a primitive inside its
scissored box by `gsTriSetup` / `gsTriCovers`, the functions the model uses and T5 proved equal to the device's
coverage (a point is its pixel, a flat rectangle its box).

Per pass (one target between two flips), in draw-list order, for each primitive P that survives the cull:

1. *Before the first reading primitive of the pass* nothing is marked; a flag records that something was drawn.
2. *P reads* (its plan is `blend != OFF && !exactBlend`, read mode): if this is the pass's first reading
   primitive and the flag is set, or any pixel of P carries the current `read` epoch, **a barrier is issued
   before P's draw** and the `read` epoch advances (the map is not cleared). A barrier ends the draw in progress.
3. *P's plan splits* (K1 only: RGB_ONLY with a depth write): if any pixel of P carries the current `split` epoch,
   the group ends before P (the pass parts and then the fail parts of the group so far are drawn) and the `split`
   epoch advances. A change of state also ends a group and advances it.
4. Once a reading primitive has been seen in the pass, every primitive stamps its pixels with the current `read`
   epoch after step 2; a splitting one stamps `split` as well.

Why it is right: a reading fragment must see every earlier write to its pixel and no later one. By 2 and 4, any
earlier write to a pixel P covers that is not yet visible (made since the last barrier) forces a barrier before P;
within a draw no two primitives share a pixel unless the later one does not read (it then blends in
rasterisation order on the fixed-function path, in another draw, since the pipeline differs). Coverage is taken
before the alpha, depth and scissor-independent tests, so the stamped set contains every pixel actually written.
The rectangle-pair exception of the box rule is not needed: the halves share no pixel.

Cost of the rule: the coverage of 303 K pixels a K2 kick on average and 5.0 M at most (3.9), where the presenter
builds the draws (not on the EE thread); about a nanosecond a pixel with incremental edge functions. Batch 2
reports the time; if the largest kick costs more than 3 ms, the first lever is to stop stamping after the kick's
last reading primitive (known when the list is closed).

Replaces in `RENDERER_2D_DESIGN.md` 18.2 "Groups and barriers" and 5.2's box groups; the box rule stays behind
`Gs2dVk::Options::grouping = kGroupBoxes` for the fault runs and for comparison.

### 10.3 Tests and faults for the rule

- **T10 (order under the pixel rule):** (i) a fan and a strip-like mesh of 64 triangles sharing edges, each drawn
  three times as water does (0x09, 0x62 FIX 0x15, 0x68 FIX 0x15, ZMSK, GEQUAL on the last two) over a patterned
  destination: emulated = model = Vulkan in every plane, **and exactly one barrier**; (ii) the same mesh with every
  fourth triangle moved to overlap its neighbour: equality, and the barrier count the model predicts; (iii) reading
  triangles interleaved with exact additive ones on the same pixels; (iv) T8 of step 7 unchanged (K1's layers,
  both targets, the split with the read).
- **Faults (each must fail T10 and check (d)):** no barrier at the first reading primitive; coverage taken from
  the box of a reading primitive's *own batch* only (the old rule's blind spot is the other direction: it must
  still pass, with more barriers); stamps left out for non-reading primitives; the `read` epoch advanced without
  a barrier.
- T1-T8 of `rc_gs2d_test` and check (c) of step 7 on the 67 dumps are repeated with the new rule: equality as
  before.

### 10.4 What K2's GPU time will be, and the acceptance

Estimates from 19.5's figures (20.6 us a barrier, about 7 us a small draw with its state), RTX 2060:

| a K2 kick | mean | largest |
|---|---|---|
| draws (state runs and barriers inside them) | 557: about 3.9 ms | 2,665: about 19 ms |
| barriers | 16: 0.3 ms | 85: 1.8 ms |

The draw figure is an extrapolation from 161 tiny draws and may be far off for larger ones; batch 2 measures it
with timestamps on USA `s5/l17`, `s5/l18`, `s5/l21`, `s5/l23`, `s5/front`. **Acceptance:** reported per dump for
read mode with the pixel rule, read mode with box groups, and fixed function. If a K2 kick's draws take more than
8 ms on average on any of those dumps, stop and report with the split between barriers, draws and fill; do not
optimise beyond 9.3's "bind what changed". The levers after that belong to P2: a texture array or atlas (a third
of the state runs in a race and nearly all in the front end change only the texture), and step 9's batches.

## 11. Checks

All off-line, on the 67 exact dumps of both versions unless said; `rc_replay <dump> --consumer vif|vk ...`. With U1
answered A, batch R comes first and the dumps are the re-referenced ones.

### 11.1 (a) State

- `stats --verify` (the chain walker's hashes) is unchanged and assumed.
- Every channel 1 kick decodes to its end with no guard.
- At every later keyframe and at the end of every segment: the reader's VIF1 registers equal `VIF1` (the fields of
  5.1), micro and data memory equal `VU1M` byte for byte, `vf` equals `VU1R`'s (bit for bit; lanes the as-built
  section lists as not reproduced are excluded and printed).
- Beside the machine: the same three after every run (memory and registers) and every kick (VIF1), which is
  where a difference is found; the keyframe form is what runs without the machine.
- The register file: section 7.

### 11.2 (b) GIF bytes

Section 6.3: per kick by hash and count on every dump; per packet beside the machine on USA and Europe `s5/l5`,
`l17`, `l18`, `l20`, `front`, `tedit` and `boot`.

### 11.3 (c) In front of the emulated GS

`emu`'s substitute hook gains a path: the reader's PATH1 bytes of a VIF1 kick are handed to the machine's GS as
PATH1 packets, under the MXCSR the machine uses for PATH1 (the kick's with 0x6000 set before batch R, the kick's
after), in place of the DMA transfer, for every VIF1 kick of the dump.

- (c1) the reader's bytes: every picture, register file and GS memory image as recorded, bit for bit.
- (c2) the draw list: each K2 list (with `cullOffscreen` off) encoded back into GIF by `gs2d_encode` and
  submitted the same way: every picture and GS memory image bit for bit; then with `cullOffscreen` on: pictures
  and GS memory still equal (the register file is compared without the vertex registers).

### 11.4 (d) Vulkan, per K2 kick

Step 7's check (c) for channel 1: before each VIF1 kick the colour target and the depth buffer are seeded from the
emulated machine's memory, the kick's list is drawn by `Gs2dVk` in read mode, both are read back and compared with
the machine's memory after the kick: **every pixel of 640 x 256 equal in R, G, B, A and Z**, 44,200 kicks.
Validation layer with synchronisation validation on, 0 errors. Reported per dump as 18.4 does, plus the pixel
rule's barriers beside the box rule's and the GPU times of 10.4. Faults: 10.3's four, GEQUAL drawn as GREATER, the
Z of one vertex off by one, REPEAT drawn as CLAMP, Q left out, a culled triangle drawn and a drawn one culled; each
must fail on USA `s5/l17` (and `s5/l12` for the water ones).

### 11.5 The chained replay

`rc_replay <dump> --consumer vk --verify`: `FrameReader` in front of nothing (no emulated machine).

- At a segment's keyframe: both colour targets from `GSVR` (CT32 at FBP 0x0 and 0x50), the depth buffer from the
  Z buffer at ZBP 0xa0 (`gs_layout`'s Z24), texture memory into `TexShadow`, the register file from `GSRG`, the VU
  and VIF state as 5.1. Then nothing is seeded again in that segment.
- Every kick through the readers and the cycle's list through Vulkan: uploads into the frame buffer (step 6's
  blit), the clear, K2, K1; one readable pass per target between flips.
- At every cycle close: the top H rows of `target[DISPFB2.FBP]` are read back; their `Hash64` must equal
  `END.refHash` (every cycle has it; a `REFI` picture, where present, is compared pixel by pixel and gives the
  difference picture). On a hash mismatch without a `REFI` the tool prints the `emu --png` command that produces
  the reference picture of that cycle.
- At every later keyframe: both colour targets and the depth buffer equal the frame buffers and the Z buffer of
  that keyframe's `GSVR`, over 640 x 256.
- **Pass:** every cycle of every dump. First on levels 5, 17, 18, 20, the front end and the Track Editor of both
  versions, as the plan lists; then all 67 (boot and `s6/boot` bring the movie and the logos through the same
  chain).
- Faults: the depth buffer not carried from K2 to K1; the clear left out; a `TEXTURE` generation not re-sent
  after an upload; the flip showing the other target.

### 11.6 Faults for batch 1 (each must fail the named check on USA `s5/l17` and `s5/tedit`)

An UNPACK's destination off by one (b); the TOPS swap left out (b); OFFSET not resetting DBF (a, b); one MPG
dropped (`VU_PROGRAM` or b); one MSCAL skipped (b, and the register file where it was a state packet); MXCSR not
set for the references (b); a register lane of 3.4 not carried between runs (b on a water level: `s5/l12`); one
PATH1 register packet lost (the register file); the VU memory not carried across kicks (b).

## 12. Live

- **Where.** `FrameReader` is a consumer in the capture's list (`render/record/capture.cpp`), called on the EE
  thread inside each kick's store, as step 7's batch 5 places the 2D reader; it reads guest RAM in place. The list
  is published at the cycle close and the presenter replays it (frame record section 8). No thread is added in P1.
- **The emulated machine keeps running** in `vk` mode (VIF1, VU1 and the GS on their own threads, as today): it
  costs the EE thread nothing new, it is the picture `RC_VK_PICTURE=gs` shows for comparison (default `vk` once
  batch 4 passes; a key toggles them), and turning it off means deciding what the game reads back from the
  emulated hardware, which is P2's work. The oracle build is untouched.
- **Start and save states:** 5.1.
- **Cost on the EE thread:** U2 and 3.10. The live batch reports frame rate and the EE thread's time in the
  reader by scene (front end, levels 5, 17, 18, the Track Editor) and does not optimise.
- **The live check.** The live reader's draw lists can be written into a recording as `DLST` chunks
  (`RC_FRAME_RECORD_DRAWLISTS=1`; frame record section 9). A short exact-mode session recorded with `vk` live is
  then replayed: the lists the off-line `FrameReader` makes from the recorded kicks must equal the recorded live
  lists byte for byte, and the chained replay of that dump must pass. That proves the live reader without
  comparing screenshots across renderers.
- **Recording new scenes.** With `vk` usable, the scenes no dump has (podium, results, memory card, the Track
  Editor's race, more two-player) are recorded in exact mode and run through all checks before step 8 is closed.

## 13. Build batches (Opus), each with its check

**R. The reference machine (only if U1 is answered A; any time before batch 2).** In the runtime: `finishXgkick`
hands the packet to the GS under the rounding mode the VU1 run was entered with (a thread-local saved where
`previousCsr` is read in `ps2_vu1_core.cpp`), restored afterwards; the VU1 cycle limit of the MSCAL and MSCNT
callbacks in `ps2_runtime.cpp` raised so that no RC run is cut (the largest run's cycle count is printed once).
Both listed in `NOTES.md`, "Local changes to the runtime". `rc_replay <dump> --consumer emu --rereference <out>`:
replays on the machine and writes a copy with every `KICK.regs`, `gifHash`, `gifBytes`, `END.refHash`, `REFI` and
every keyframe's state chunks taken from the replaying machine, everything else (pages, kicks, stores, tags)
copied, and a `NOTE` naming the build. *Checks:* `emu --verify` passes on every re-referenced dump, twice with
equal digests; a re-referenced dump of a session without 3D (the movie part of `boot`) is byte-identical to its
source in every picture; `vifstat --with-emu` shows PATH1 packets under the kick's MXCSR and no run cut short; the
pictures change as 3.11 measured (the histogram is recorded in the as-built section); steps 5, 6 and 7's checks
(`tex`, `blit`, `gif2d` (a), (b), (c) in read mode) pass on all re-referenced dumps; `tools/oracle.sh --all` and
the `playtest2` replays of both versions are unchanged (frame rate, scene logs). The originals are kept beside
the new dumps until step 8 is closed.

**1. The reader and the references, off-line.** `render/vif`, the references' `Io` entry with registers out,
`Gif2dReader::feedPath1`, `FrameReader`, `rc_replay --consumer vif`. *Checks:* 11.1, 11.2, 11.3, the faults of
11.6; `gif2d`'s checks (a) and (b) for K1 still pass with the register file now fed by PATH1 and nothing excused;
`RC_VU1_REF=1` and `RC_VU1_SYNTH` runs of the game still report 0 mismatches and `tools/shader_check` passes (the
references were edited); the oracle and `playtest2` as usual if a game object was rebuilt. Reported: the reader's
time a kick, split as section 8 says; the lanes of `vf` not reproduced, if any.

*Then step 7's batch 5 (live 2D), as `RENDERER_2D_DESIGN.md` section 11 item 5 with `FrameReader` as the consumer
and read mode as the default blend (10.1).*

**2. K2 on Vulkan, per kick.** `GsCoverMap` and the pixel rule in `Gs2dVk` (box rule kept as an option),
`cullOffscreen`, the capacities and "bind what changed" of 9.3, `rc_gs2d_test` T9 (9.2: per-vertex Z under GREATER
and GEQUAL against a stored ramp, the alpha ramp across LEQUAL 0x2f and 0x3f with every PATH1 blend state of 3.6,
REPEAT to +-1,000 texels, 256x128 CT32, a triangle with vertices 2,047 pixels out, ADC vertices) and T10 (10.3),
`tools/rc_replay/frame_vk` with check (d). *Checks:* `rc_gs2d_test` complete (T1-T10) in read mode with the pixel
rule, validation on; step 7's check (c) on the 67 dumps with the pixel rule; 11.4 on all 67; the faults; 10.4's
report and acceptance. Needs U1 (with C: the criterion becomes a difference histogram per state, stated in the
as-built section, and the batch says what it could not prove).

**3. The chained replay.** `rc_replay --consumer vk` without the machine: seeding at keyframes, the pass per
target, hash comparison at every flip, the keyframe comparisons, PNGs on a mismatch. *Checks:* 11.5 and its
faults; run time a dump; the DIVERGENCES entries of section 14 drafted.

**4. Live.** `FrameReader` in the capture's consumer list, the presenter drawing the cycle's list (clear, uploads,
K2, K1) and presenting `target[DISPFB2.FBP]`, `RC_VK_PICTURE`, the `DLST` writer. *Checks:* the live check of
section 12 on USA and Europe sessions through the front end into races on levels 5, 17 and 18 and into the Track
Editor; screenshots of the plan's four levels beside the `gs` renderer's; frame rates reported; recording off,
the oracle (`--all`, both versions) and `playtest2` unchanged; the new-scene dumps recorded and passed.

Batch R is independent of 1; 2 needs 1 and the answer to U1; 3 needs 2; 4 needs 3 and step 7's batch 5.

## 14. Divergences this design accepts or inherits (for P2 step 10's list)

**Drafted with measured sizes after batch 3: section 19.5** (these rows and the ones batches R-3 added).

| id | what | where | size |
|---|---|---|---|
| D-3D-1 | With U1 = A: the reference machine itself changes (PATH1 rasterised rounding to nearest, water runs finished) | every 3D picture of the software GS | a level on a third of the pixels; the hardware's own rule is unknown here |
| D-3D-2 | Mip levels are not sampled (MXL 3 / 4 drawn from level 0), as in the reference | two textures, levels 2 and 14 | inherited; real mip levels are P2's (plan step 9) |
| D-3D-3 | The software GS's float interpolation (2D design, decision notes 1 and 2) applies to PATH1 as to PATH3 | all | at most a level |
| D-3D-4 | A VIF code, UNPACK form, program or entry outside sections 4 and 5 stops the kick's 3D | none seen | guarded, logged |
| D-2D-1 | unchanged: fixed-function blending is -1..+1 a layer, on devices without the local read or with `ff` chosen | | |

## 15. Open

- U1 and U2.
- The references' time is the frame time (3.10); nothing in step 8 reduces it.
- K2's GPU time is estimated, not measured (10.4).
- The one Z at or above 2^24 (9.1).
- Scenes not recorded yet (section 12).
- Whether every lane of the vector registers can be made equal by the references, or only those that matter (5.2).

## 16. Batch 1 as built (2026-11-20): the reader and the references off-line

Section 13 item 1. Built: `render/vif` (library `rc_vif`: `Vif1Reader`, the program table), `render/frame` (`rc_frame`:
`FrameReader`), the references' `Io` entry in `decomp/vu1`, `Gif2dReader::feedPath1` and `cullOffscreen`,
`Emu::setPath1Substitute`, and `rc_replay --consumer vif` (`tools/rc_replay/vif.{h,cpp}`). The `gif2d` consumer now
drives a `FrameReader`. Usage: `NOTES.md`, "The VIF1 reader and the frame reader". Batch R and everything that waits
for U1 was not built.

```
render/vif/vif1_reader.{h,cpp}   Vif1Reader: VIF1 registers, VU data and micro memory, VF; kick() walks, decodes, runs; guards; faults
render/vif/vu1_programs.{h,cpp}  the program table (prefix length and hash, image hash), the lanes not reproduced, poison pattern
render/frame/frame_reader.{h,cpp} FrameReader (rc::rec::Consumer): Vif1Reader -> Gif2dReader as PATH1; Path1Tap; side tags as scopes
decomp/vu1/vu1_ref.h, vu1_io.cpp rc::vu1::Io, Program, run(), isEntry(); run*ShapeIo(Io&) in each *_shape.cpp; run*Shape are wrappers
render/gs2d/gif2d_reader.*       beginPath1Kick / feedPath1 / endPath1Kick (a second GifWalker), cullOffscreen, scope()
render/tex/gif_walk.*            GifSink::vertexQ (below, 16.1 item 6)
render/gs2d/gs2d_encode.cpp      a tag ends only before a PRIM (16.1 item 6)
tools/rc_replay/emu.*            setPath1Substitute: PATH1 packets in place of the machine's, after its run
tools/rc_replay/vif.{h,cpp}      VifCheck: checks (a), (b), (c), the wrapped VU1 programs, faults
```

Nothing of it is linked into the game or the oracle except the references themselves (`decomp/vu1` is compiled into
both, as before): their interface changed (16.1 items 1-3) and the game and the oracle were rebuilt and checked
(16.5). The runtime and every decompiled game function are untouched.

### 16.1 Changes to the design, and why

1. **The registers out: the lanes of 3.4 exact, the rest poisoned.** Beside the emulated VU1 (vif `--with-emu
   --vif-no-poison`, twelve dumps of both versions) the references left 15-57 lanes per program different from the
   machine after a run: VF08.xyz, VF11-VF16, VF17.xyz, VF24-VF28 (not VF25.w or VF26.w), VF29.z, VF30, VF31. They are
   the microcode's temporaries the C++ keeps in locals (the near-plane test in VF08, the facing products in VF24,
   the tint in VF30, the block header in VF31...) and work the microcode does on a triangle it then rejects. One of
   them mattered: **EnvRdr computes the environment-mapped corners (normals in VF26-VF28) before its facing test**, so
   a rejected last triangle leaves VF26.w, which the water program's next run sends to the GS; the reference skipped
   it. `env_shape.cpp` now keeps the normals in p[26..28] and runs the corner pass for rejected triangles too (no
   packet or memory changes; RC_VU1_REF unchanged, 16.5). With that, VF05.w-VF07.w, VF25.w, VF26.w and VF29.w are
   never different after any run of any program. The union of the measured lanes is written after every run as a
   signalling NaN naming the lane (`vu1_programs.cpp`, `RC_VU1_NOT_REPRODUCED`, 72 lanes): over all 67 dumps no
   packet and no byte of VU memory then differs, so nothing reads them. Check (a) excludes exactly those lanes (it
   recognises the pattern) and prints how many it excluded. Making every lane equal (5.2's "and then on every lane it
   can make equal") would mean auditing about a thousand instructions a program for state no output reads; not done.
2. **Runs the machine cuts short.** Without the machine the reader cannot know where the runtime's 65,536-cycle limit
   stopped a water run, so the kicks with such a run fail (b) there (as 6.3 says) and the state they leave can pull
   the register file and later kicks with them. Beside the machine the run is compared up to the machine's last
   packet, the reader's later packets are left out (not passed to the GIF reader) and the reader takes the machine's
   VU memory and registers after the run; everything after it is compared normally and the list of kicks is
   printed. This disappears with batch R.
3. **The reader runs a kick after the machine, not before it.** In `--with-emu` mode the vif consumer sits behind the
   emu consumer and runs each channel 1 kick from `Emu::setPath1Substitute`, called once the machine has run the
   kick's DMA (its six generated programs wrapped: each run's packets, VU memory and registers kept, packets diverted
   from the GS when the reader's are to be drawn). The reader then knows the cuts (item 2) and its bytes or encoded
   list go to the GS after the machine's run, in the same kick: (c) needs no second pass. The `gif2d` consumer drives
   its own `FrameReader`; with `--with-emu` a vif consumer attached to that reader runs the channel 1 kicks the same
   way (`deferPath1`).
4. **New guard VU_RUNAWAY.** The planted UNPACK fault made a reference loop on garbage, sending packets without end
   (and `copyGifPacket` grow one packet without bound, which exhausted memory). A run that sends more than 2^20
   packets, or a packet of 64 K quadwords, is now abandoned with that guard; `copyGifPacket` (shared with the game's
   wrappers) cuts a packet there instead of growing, which no real packet comes near (the largest PATH1 packet in the dumps is 160 bytes).
5. **`FrameBlit` is not in `FrameReader` yet.** Nothing in batch 1 reads uploads into the frame buffer; it joins with
   the chained replay (batch 3), its first user. Side tags of a VIF1 kick are placed as scope commands before the run
   at or after their stream offset (8).
6. **Two findings in step 7's code.** (i) The reference GS gives a vertex the Q of the last PACKED ST or of an RGBAQ
   written as a register, and sets it back to 1.0 at every GIF tag; a PACKED RGBAQ leaves it. The GIF walker reported
   RGBAQ with the frame record's convention (the last PACKED ST's Q carried into it, which is what the recorded
   register file holds); vertices now take the reference's Q (`GifSink::vertexQ`). On every dump the two agree on
   every vertex (counted: 0 differ), so no list changed. (ii) `gs2d_encode` split its A+D stream into tags of 32,767
   writes wherever the count fell; a tag boundary between a vertex's RGBAQ and its XYZ2 resets Q to 1.0 in the
   reference. K1 never reached the limit; K2's lists do, and (c2) failed until a tag ends only before a PRIM.
7. **Faults.** `--fault <name>:<cycle>[:<n>]` as 11.6; the "lane" fault zeroes VF25.w before every run of the cycle,
   the "packet" fault drops the n-th PATH1 packet of the cycle made only of A+D writes (the game's state packets),
   "vumem" zeroes VU data memory at each kick's start in the cycle.

### 16.2 Checks (the 67 exact dumps; `build-decomp/frame-dumps/{s5,s6,b4-key,b4-key-1,b4-x2-0,b4-x2-1}`, `build-eu`'s
`s5`, `s6`, `b4-key-0`, `b4-key-1`, `b4-x2-0`, `b4-x2-1`; `build-decomp`'s `rc_replay`, which reads both versions)

Four `--with-emu --verify` passes (`--vif-substitute bytes`, `list`, `list-cull`, and `gif2d --with-emu --verify`) and
one without the machine. **Every `--with-emu` pass: 67 / 67 `PASS`.**

| | USA (34 dumps) | Europe (33) |
|---|---|---|
| cycles; VIF1 kicks; VU1 runs; PATH1 packets | 24,174; 23,433; 22,517,443; 559.6 M | 21,445; 20,767; 20,558,762; 486.1 M |
| guards fired | 0 | 0 |
| (a) keyframes: VIF1 registers, micro memory, data memory, vector registers (poisoned lanes excluded) equal | 69 / 69 each | 62 / 62 each |
| (a) GS register file equal to the keyframe's `GSRG` (final keyframes included) | 103 / 103 | 95 / 95 |
| (a) register file at every kick, every register, nothing excused (beside the machine) | 82,312,258 values at 839,921 kicks, 0 differ | 74,883,564 at 764,118, 0 differ |
| (a) beside the machine: VU memory and registers after every run; VIF1 registers and micro memory after every kick | 0 differ (22.5 M runs, 23,433 kicks) | 0 differ (20.6 M, 20,767) |
| (b) beside the machine, packet by packet | 22,517,443 runs, 559,628,234 packets, 0 differ; 0 runs or kicks with another count | 20,558,762, 486,108,434, 0 differ |
| runs the machine cut short (compared to the cut, reader's later packets left out, machine's state taken) | 575 runs in 541 kicks, 17,794 packets | 520 in 513, 17,184 |
| (b) without the machine, hash and count per VIF1 kick | 22,892 / 23,433: **the 541 failing kicks are exactly the kicks with a cut run** | 20,254 / 20,767: the 513 cut kicks |
| (a) without the machine, register values that differ | 10 (on the dumps with cuts, after them) | 8 |
| (c1) the reader's bytes in place of the machine's PATH1: recorded GIF hash per kick; register files before every kick; picture hashes; `REFI` pictures; GS memory at keyframes, final | 23,433 / 23,433; 839,921 / 839,921; 24,174 / 24,174; 1,430 / 1,430; 69 / 69, 34 / 34 | 20,767; 764,118; 21,445; 1,430; 62, 33 (all equal) |
| (c2) the K2 draw list encoded back (`cullOffscreen` off): pictures, `REFI`, register files, GS memory | all equal; 292,106,072 primitives re-encoded | all equal; 264,129,343 |
| (c2) with `cullOffscreen`: the same, all equal; triangles left out (no area + outside the scissor) | 3,443,271 + 161,151,224 of 292.6 M (56.2%); 127,594,995 drawn | 2,930,296 + 144,808,389 of 264.6 M (55.9%); 116,461,880 |
| `gif2d --with-emu` (step 7's (a) and (b), K1 now on the frame reader's register file) | (a) 82,312,258 values, 0 differ; (b) 46,904 drawing PATH3 kicks substituted, every picture and GS memory equal | 74,883,564, 0; 41,570 |

- The plain `gif2d` without the machine (`front`, `tedit`, `l13`, `s6/boot`) also passes: on a dump without cuts the
  register file needs nothing from the recording after the first keyframe.
- Draws needing a seeded or default register after a dump's first clear: FBA_1 only (written by neither path in any
  dump; the keyframe's value is the default 0), so 7's "a register is unknown only before its first write" holds.
- Run time (one process each, four at a time on the four cores of the Ryzen 7 3750H): 10-80 s a dump without the
  machine, 70-200 s with it, about 200 MB.

### 16.3 The reader's time a K2 kick (section 8's split; without the machine, four processes at a time)

| dump | references | VIF walk | GIF walk and list | all, mean / largest |
|---|---|---|---|---|
| USA `s5/front` | 17.7 ms | 1.4 ms | 4.5 ms | 23.6 / 43.2 ms |
| USA `s5/l17` | 20.6 ms | 1.6 ms | 5.3 ms | 27.5 / 40.9 ms |
| USA `s5/l18` | 36.3 ms | 3.1 ms | 9.2 ms | 48.6 / 71.3 ms |
| USA `s5/l5` | 20.3 ms | 1.6 ms | 5.1 ms | 27.0 / 40.2 ms |
| Europe `s5/l18` (the largest kick of all) | 37.6 ms | 3.2 ms | 9.4 ms | 50.2 / 84.8 ms |

The references' share is 3.10's figure; the VIF walk (5.8 MB of UNPACK copies a kick) is 6% and the GIF walk with the
draw list about a fifth. Time stamps are TSC reads calibrated against `steady_clock` (the clock here is an HPET
clocksource, a system call each read: timing every packet with it tripled the run time).

### 16.4 Planted faults (11.6; `--with-emu --verify`, four cycles from the fault; the clean runs of the same ranges pass)

| fault | USA `s5/l17` (cycle 120) | USA `s5/tedit` (cycle 2020) |
|---|---|---|
| `unpack` (every UNPACK one quadword on) | (b) first packet differs (A+D ALPHA_1), 2,003 packets; (a) 1,122 register values; (c1) a GIF hash, 112 register files, a picture | guard VU_RUNAWAY (16.1 item 4); (b) run counts; (c1) |
| `tops` (no TOPS swap at MSCAL) | (b) the run's TOP differs from the machine's (515 runs); packets equal | the same (10 runs) |
| `dbf` (OFFSET leaves DBF) | (b) run TOP differs (255 runs) | the same (2 runs) |
| `mpg` (the cycle's first MPG not copied) | guard VU_PROGRAM; (b) 4 kicks with another run count; (c1) 4 GIF hashes, 6 register files, 4 pictures | the same |
| `mscal:…:5` (the sixth MSCAL skipped) | (b) 1 kick with another run count, 1,200 packets; (c1) a GIF hash | (b); (c1) a GIF hash and a picture |
| `mxcsr` (references not rounding toward zero) | (b) 12,767 packets (first: a PACKED ST word one unit apart); (c1) a picture | (b) 309 packets; (a) 2 register values; (c1) |
| `lane` (VF25.w zeroed before every run) | (a) registers after the run; (b) 2,511 packets; (c1) a GIF hash | (a) 6 runs |
| `lane` on USA `s5/l12` (water) | (a); (b) 699 packets; (c1) a GIF hash | |
| `packet` (a state packet lost) | (c1) the kick's GIF hash | (c1) GIF hash and a picture |
| `vumem` (VU memory zeroed at each kick) | (a) VU memory after 838 runs | (a) 84 runs |

Two do not show in the bytes (b) compares: a lost TOPS swap or DBF clear moves where the next UNPACK lands and
where the program reads, consistently, so the packets stay right and only TOP (b's run identity) and the state (a)
show it; a lost state packet changes the GIF stream (c1's hash) but the next kick rewrites those registers, so the
register file at the next kick is right again. Every fault fails a named check on both dumps.

### 16.5 The references in the game and the oracle

The references' interface changed (the `Io` entry, `ShapeProgram`'s packet callback, the env corner pass for rejected
triangles, `copyGifPacket`'s cut), so the game and the oracle relinked in all four trees.
Checked on the final build, both versions where it applies:

- `tools/oracle.sh --all`: USA seed 9131, 1,632 ok, 0 FAIL; Europe seed 9132, 1,563 ok, 0 FAIL (and seeds 8817 / 8818
  on the build before the last rename, the same).
- `RC_SCENE_LOG=... timeout 70 tools/play.sh replay playtest2`: USA race scene 3020 at frame 658, Europe at 1167, as
  before.
- Level 17 (`RC_BOOT_LEVEL=17 RC_VU1_REF=1 RC_VU1_SYNTH=decomp/generated/vusrc RC_TRACK_SHADER=0 PS2X_MTVU=0`, 120 s):
  1,828,480 runs (track 1,228,437, env 348,925 of them 110,549 env-mapped, water 111,595, skin 139,523), 56.9 M packets,
  0 packet, memory or count mismatches; the synthetic TERdr and Rdr_VUShape runs 6,093 each, 0 mismatches. (With the
  default `PS2X_MTVU=1` the checks reported no run at all in 240 s; not looked into, the session above turns the VU1
  thread off.)
- `tools/shader_check decomp/shaders/trdr_shape.vert` on 1,500 TRdr runs captured in that session (`RC_VU1_CAPTURE`),
  built by the line at the top of the file: 52,634 triangles, no facing disagreement, worst error 1.11/16 pixel, z 2,
  colour 0, nothing over tolerance (as `NOTES.md` recorded on 2026-11-10). The `Io` entries are `run*ShapeIo` and the
  dispatch lives in `vu1_io.cpp` so that the old names stay unoverloaded and shader_check still links from its five
  files.

### 16.6 Open

- **U1 and batch R** (built later the same day: section 17): until the runtime's VU1 cycle limit goes, the 1,054 kicks with a cut
  water run fail (b) without the machine, and beside it the reader is given the machine's state after the cut.
- The 72 poisoned lanes: the references could be made to keep them (5.2's second half); nothing needs them now.
- `FrameBlit` joins `FrameReader` with batch 3.
- Step 7's batch 5 (live 2D) can now take its PATH1 registers from `FrameReader` (FBA_1 is the only register no
  path writes; the keyframe gives 0, the default).
- The reader's time (16.3) is the references' as 3.10 predicted; nothing in batch 1 reduces it (U2).

## 17. Batch R as built (2026-11-20): the reference machine, the dumps re-referenced

Section 13 item R, with U1 = A. Two runtime changes (`NOTES.md`, "Local changes to the runtime"), a new `rc_replay`
mode, and the 67 exact dumps re-referenced. The kick records (what the game sent) are untouched.

```
third_party/ps2recomp/ps2xRuntime/src/lib/vu/ps2_vu1_core.cpp   runLoopWith: keeps the entering thread's MXCSR rounding bits (m_runRounding), lastRunCycles
third_party/ps2recomp/ps2xRuntime/src/lib/vu/ps2_vu1_impl.inl   finishXgkick: submitGifPacket under those bits, then back to the run's
third_party/ps2recomp/ps2xRuntime/src/lib/ps2_runtime.cpp       MSCAL / MSCNT budget kRunGuardCycles (2^22) instead of 65,536; ps2x::vu1run statistics
third_party/ps2recomp/ps2xRuntime/include/runtime/ps2_vu1.h     the members, lastRunCycles(), ps2x::vu1run
tools/rc_replay/emu.{h,cpp}                                    Rereference (consumer behind Emu); Emu keeps what it made; PATH1 substitute under the kick's MXCSR
tools/rc_replay/rc_replay.cpp                                  --consumer emu --rereference <out.rcfr>
render/record/format.h                                         kFileRereferenced (header flag 1 << 4)
```

### 17.1 The runtime

1. **PATH1 under the kick's rounding.** The rounding bits are kept in the interpreter, not a thread-local: the
   `VU1Interpreter` is the one object the run and `finishXgkick` share, and a nested run (`runVerified`) restores the
   outer value. Only the rounding bits change around the submission; exception flags the GS raises stay set, as
   before. Every PATH1 packet reaches the GS through `finishXgkick` (the generated programs start an XGKICK through
   `startXgkick` and finish it through the interpreter's pipeline), so this one site covers the machine `rc_replay`
   and exact mode run. The game's own native VU1 wrappers (`decomp/vu1/track_draw.cpp`, `RC_TRACK_SHADER`) submit
   their packets themselves and still do so inside the run; they draw through the GL back end and are never on in
   exact mode.
2. **No cut runs.** The budget of an MSCAL / MSCNT run is `ps2x::vu1run::kRunGuardCycles` = 2^22 cycles (about a
   frame of VU1 time), a guard for a runaway program; the runtime counts runs, the largest, the runs over 65,536 and
   the guard stops, logs `[vu1] a run took N cycles ...` at the first run over 65,536 and each of the first eight
   guard stops, and `rc_replay emu` prints the counts. **Largest run in the dumps: 99,261 cycles (USA), 90,201
   (Europe); 575 + 520 runs over 65,536, exactly the runs batch 1 found cut; 0 guard stops.**

### 17.2 Re-referencing (`rc_replay <dump> --consumer emu --rereference <out.rcfr>`)

The `Rereference` consumer sits behind `Emu` and writes, through the frame record's own `Writer`, a copy in which:

- every `KICK`'s register file (before the kick) and GIF hash and length are the replaying machine's;
- every `END.refHash` and every `REFI` picture are the machine's picture of that cycle (its DISPFB2 rectangle);
- every keyframe after the first (and not after a save-state load) holds the machine's `GSVR`, `GSRG`, `VU1M`,
  `VU1R`, `VIF1` and `EMUS` there. In this mode `Emu` does not load those keyframes: the machine runs on from the
  first one, so the copy's references are one continuous replay. **`GSPR` is kept as recorded**: the privileged
  registers are made by stores (replayed, so equal) and by the vertical blank, which the replay machine does not run;
  the machine's copy differs only in CSR, IMR and vsyncTick, none of them the GS's output. `TAGP` is copied.
- the first keyframe and keyframes after a save-state load are copied: they are where a replay starts, so their GS
  memory is still the old machine's drawing (it is redrawn within a cycle or two in every dump);
- pages, kicks (registers and streams), stores, CRT, tags, odd events and `TNAM` are copied; the header gets
  `kFileRereferenced` (`rc_replay` prints "exact, re-referenced") and the first cycle a `NOTE` naming the build
  (`git describe`) and the source.

It counts what changed and, where the source stored pictures, a histogram of the picture differences. **Where the
dumps live:** the re-referenced dumps are at the old paths (`build-decomp/frame-dumps/{s5,s6,b4-key*,b4-x2-*}`,
`build-eu/frame-dumps/...`), so every command of steps 4-8 runs on them unchanged; **the originals are kept under
`<tree>/frame-dumps/pre-r/` with the same relative paths** until step 8 is closed (3.4 GB + 3.1 GB). Run time: 20
minutes for the 64 large dumps, four at a time.

| what changed (re-referenced against the original) | USA (34 dumps) | Europe (33) |
|---|---|---|
| cycles; kicks | 24,174; 839,921 | 21,445; 764,118 |
| GIF output (hash and length) | 541 kicks, all longer: exactly batch 1's kicks with a cut run | 513 kicks |
| register files before a kick | 10 kicks, one value each (after a cut) | 8 kicks |
| picture hashes | 23,415 of 24,174 | 20,738 of 21,445 |
| stored pictures (`REFI`) | 1,136 of 1,430 | 1,137 of 1,430 |
| keyframes from the machine; their `GSVR` changed | 103; 102 (20.2 MB in all) | 95; 94 (19.4 MB) |
| `GSRG`, `VU1M`, `VU1R`, `VIF1` changed at a keyframe | 0, 0, 0, 0 | 0, 0, 0, 0 |
| `EMUS` changed | 69 (the VU1 cycle counter, the GS's state) | 62 |

The pictures without 3D are unchanged: `s6/boot`'s logos and movie, cycles 0-256 (USA) and 0-257 (Europe), have the
same hash in both copies (as do a few black cycles later), and `b4-key` (USA) and `b4-key-0` (Europe), menu screens
drawn from uploads, change in no picture.

**The picture difference** (re-referenced minus original, share of all pixels of the stored pictures, by channel):

| dump | pixels differing | R: -1, +1, +2, +3 or more | G: -1, +1, +2, +3 or more | alpha |
|---|---|---|---|---|
| USA `s6/load17` (level load, 380 race cycles) | 31.75% | 3.4%, 12.4%, 1.6%, 0.20% | 8.4%, 15.3%, 2.5%, 0.38% | +1 on 0.08% |
| Europe `s6/load17` | 36.85% | 5.1%, 14.8%, 1.8%, 0.25% | 9.2%, 17.1%, 3.5%, 0.63% | +1 on 0.06% |
| USA `s6/boot` (front end after the movie) | 4.40% | 0.10%, 1.8%, 0.59%, 0.01% | 0.07%, 1.6%, 0.62%, 0.01% | +1 on 0.77% |
| Europe `s6/boot` | 11.52% | 0.06%, 7.4%, 2.3%, 0.20% | 0.05%, 6.7%, 0.42%, 0.20% | +1 on 0.02% |
| USA `b4-key-1`, Europe `b4-key-1` (front end) | 6.95%, 7.32% | 0.17%, 2.9%, 1.2%, 0.01% (USA) | 0.11%, 2.3%, 1.1%, 0.01% (USA) | +1 on 1.0%, 0.9% |

As 3.11 measured with PATH1 alone moved to round to nearest (31.8-33.2% of a race's pixels, mostly +1); Europe's
level 17 differs a little more (-2 or less on 1-2% of the channel values) because its water runs were also cut
before and are now finished. One race picture, USA `s6/load17` cycle 380 (the boat by the octopus): 50,659 of
153,600 pixels (33.0%) differ, R -1 8.2% / +1 15.7% / +2 1.5% / +3 or more 0.19%, G and B alike, alpha none,
largest -1..+5; the picture is otherwise the same (`build-decomp/batchR-shots/usa_load17_c380_{before,after,diff}.png`,
not committed): the difference is a level of dither over the 3D (water, buildings, the tentacles, the boat), none
on the sky or the HUD.

### 17.3 Checks (all on the re-referenced dumps, both versions)

| check | USA (34) | Europe (33) |
|---|---|---|
| `emu --verify`, twice: every GIF hash, register file, picture hash, `REFI`, keyframe and final GS memory | 34 / 34 `PASS` both runs, equal digests | 33 / 33, equal digests |
| totals of those (both versions) | 1,604,039 kicks' GIF output, 1,604,039 register files, 45,619 picture hashes, 2,860 stored pictures, 131 keyframes, 67 finals: all equal | |
| VU1 runs; over 65,536 cycles; stopped by the guard | 22,517,443; 575; 0 | 20,558,762; 520; 0 |
| `vifstat --with-emu` (`s5/l5`, `l17`, `l12`, `front`): MXCSR at the PATH1 packets | 0x1fa2, 0x1fb2, 0x1fb3, 0x1fba, 0x1fbb: the kicks' own (round to nearest; batch 1 saw 0x7fb2 / 0x7fa2); runs cut short 0 | |
| `vif --verify` without the machine: (b) hash and count per VIF1 kick | **23,433 / 23,433** (was 22,892: the 541 cut kicks) | **20,767 / 20,767** (was 20,254) |
| the same: register values at every kick | 82,312,258 at 839,921 kicks, 0 differ (was 10) | 74,883,564 at 764,118, 0 differ (was 8) |
| the same: keyframes (VIF1 registers, micro and data memory, vector registers; `GSRG`) | 69 / 69 each; 103 / 103 | 62 / 62; 95 / 95 |
| `tex --verify` | 34 / 34 `PASS` | 33 / 33 |
| `blit --verify --validation` | 34 / 34 `PASS` | 33 / 33 |
| `gif2d --verify` without the machine (a) | 34 / 34 `PASS` (before batch R only the dumps without cut runs passed) | 33 / 33 |
| `gif2d --vk --verify --vk-blend read --validation` ((a), (b) with the machine, (c)) | (a), (b), (c) hold on all 34; `PASS` on 32 (below) | on all 33; `PASS` on 31 |
| (c) totals, both versions | 88,421 kicks, 14,486,896,640 pixel-kicks, **every pixel equal** in R, G, B, A and Z (as batch 4b); 0 validation errors | |
| `live2d --verify --validation` (`s6/boot`, `s6/load17`, `b4-key*`) | 4 / 4 `PASS` | 4 / 4; 0 validation errors |

- **The four `gif2d --vk` FAILs are not batch R's:** `s5/l2` and `s5/l14` of both versions end `FAIL` only because
  `--vk` fails on any fired guard, and since batch 1 the gif2d reader also reads PATH1 (`FrameReader`), whose
  primitives with the two mipmapped textures fire `MIPMAP` (27,399 / 59,687 USA, 31,071 / 59,335 Europe). Every
  comparison in those runs is equal; the original USA `s5/l2` gives the same `MIPMAP 27399` on the machine before
  this batch. The check was last run on all dumps in step 7's batch 4b, before PATH1 reached the reader. Batch 2
  decides K2's mip guard (D-3D-2) and with it what `--vk` counts.
- `rc_gs2d_test` and the game: 17.4.

### 17.4 `rc_gs2d_test`, the game and the oracle

The runtime changed, so the game and the oracle were rebuilt in all four trees (`build-decomp`, `build-eu` with its
`rc_replay`, `build-oracle`, `build-oracle-eu`; 6-7 minutes each).

- `rc_gs2d_test --validation` (synchronisation validation on): `PASS`, 0 validation errors (266 s).
- `tools/oracle.sh --all`: USA seed 5521, **1,632 ok, 0 FAIL** (472 s); Europe seed 5522, **1,563 ok, 0 FAIL** (861 s).
- `RC_SCENE_LOG=... RC_DEBUG_DUMP=... timeout 70 tools/play.sh replay playtest2`, idle machine: USA default race
  scene 3020 at frame 658, `RC_RENDERER=vk` 658, **60.0 fps** in both (`tools/fps.py`, every 5 s sample after the
  boot); Europe `RC_RENDERER=vk` 1167, default 1167 on a second run (the first default run, right after the USA
  runs and a GPU test, reached it at 1168: the drift `NOTES.md` records under load), **50.0-50.4 fps** in both.
- Level 17 (`RC_BOOT_LEVEL=17 RC_VU1_REF=1 RC_VU1_SYNTH=decomp/generated/vusrc RC_TRACK_SHADER=0 PS2X_MTVU=0`,
  120 s): 1,862,204 runs (track 1,262,098, env 344,140 of them 113,443 env-mapped, water 114,462, skin 141,504),
  58.7 M packets, **cut short 0**, 0 packet, memory or count mismatches; the synthetic TERdr and Rdr_VUShape runs
  6,261 each, 0 mismatches.
- The emulated picture: 17.2's race frame (before, after, difference) and the checks above; the GPU back ends the
  game uses by default (`play.sh`: `PS2X_GS_GPU=1`, `PS2X_GS_HW=1`) do not rasterise in host floats and are
  unchanged.

### 17.5 Open

- `gif2d --vk` counts PATH1's `MIPMAP` guards as failures on `s5/l2` and `s5/l14` (17.3); batch 2 settles K2's mip
  levels and what the check counts.
- The game's native VU1 wrappers (`track_draw.cpp`) still submit PATH1 inside the run's rounding (17.1); they never
  feed the software GS in exact mode.
- The originals under `frame-dumps/pre-r/` go when step 8 is closed (6.5 GB).
- The vif consumer's handling of runs the machine cut short (16.1 item 2) stays in the code and now reports 0.

## 18. Batch 2 as built (2026-11-20/21): K2 on Vulkan, per kick

Section 13 item 2. Built: the pixel rule (`render/gs2d/gs_cover`, `GsCoverMap`) in `Gs2dVk` with the box rule kept as
an option, K2 drawn by `Gs2dVk` (viewport, bindings, counters, faults), check (d) in `rc_replay`'s `gif2d` consumer,
`rc_gs2d_test` T9 and T10. Check (d) found three differences between the reader, the shader, the model and the
reference, all corrected (18.1 items 5-7); the comparison runs found an error in the box rule (item 8). Usage:
`NOTES.md`, "K2 on Vulkan per kick, barriers by covered pixels". The chained replay and everything live is batch 3 and 4.

```
render/gs2d/gs_cover.{h,cpp}     GsCoverMap: two 16-bit stamps a pixel (read, split) with epochs; a triangle's coverage as spans a row
render/gs2d/gs2d_vk.{h,cpp}      Options::grouping (kGroupPixels default, kGroupBoxes), viewport 8192 at -4096, only changed bindings
                                 recorded, stamping stops after the last reading primitive, ListStats, ten new faults, measureNoFill
render/gs2d/gs2d.vert, gs2d.frag ndc = framebuffer / 4096; cvtt and exactLroundInt (18.1 item 7)
render/gs2d/gs_model.cpp         the same two conversions in the model
render/gs2d/gif2d_reader.{h,cpp} the GS's CLUT buffer, loaded at TEX0 writes (18.1 item 6)
render/tex/gif_walk.cpp          a vertex's Q 0.0 read as 1.0 (18.1 item 5)
tools/rc_replay/gif2d.cpp        channel 1 kicks handed to the Vulkan check; MIPMAP counted, not a failure
tools/rc_replay/gif2d_vk.{h,cpp} check (d); K1 and K2 reported apart; --vk-boxes, --vk-nofill; the Z >= 2^24 report
tools/rc_replay/gs2d_test.cpp    T9 (i)-(vii), T10 (o)-(v), their faults
```

`rc_tex` (the GIF walker) is linked into the game through `rc_live`, so the game was relinked in both trees and
checked (18.6). The oracle links none of it (`rc_oracle` was not rebuilt). The runtime and every decompiled function
are untouched.

### 18.1 Changes to the design, and why

1. **Check (d) is part of `gif2d --vk`, not a new `frame_vk` tool.** The `gif2d` consumer already drives a
   `FrameReader` (batch 1) and already seeds, draws and compares K1 kicks; a channel 1 kick is judged by the same code:
   the emulated GS memory is kept before the machine runs the kick, the vif consumer behind the machine makes the
   kick's K2 commands, they are drawn over the kept memory and compared with the machine's memory after the kick. One
   run of a dump gives (a), (b), (c) and (d). `--vk-boxes` adds a second read-mode pass with the box rule.
2. **The viewport is 8,192 x 8,192 at -4,096** (9.3 said +-2,048). 3.7's "no vertex more than 2,048 pixels outside
   the picture" measured from the picture's edges; from the window origin PATH1's vertices reach 2,368 pixels, and
   the first run of check (d) counted them beyond the viewport. With 4,096 every window coordinate a 16-bit GS vertex
   can have (X / 16 - OFX) lies inside, the GPU never clips, and `ndc = framebuffer / 4096` is still exact. A vertex
   beyond it fails check (d) (`beyondViewport`); it cannot happen and the count is 0 on all dumps.
3. **Coverage a row at a time, and stamping stops early.** `GsCoverMap::triangle` gives a triangle's pixels as one
   span a row (the reference's edge functions with its bias; T10 (o): 13,763 random triangles, 327 M covered pixels,
   0 different from `gsTriCovers`). The lever 10.2 kept in reserve is built: nothing is stamped after the list's last
   reading primitive. The split's groups (K1) use the second stamp of the same map.
4. **Bindings.** Between consecutive draws only the pipeline, the bind groups and the scissor that changed are
   recorded: 1,294 bind commands a K2 kick for 559 draws (2.3 a draw; up to 5 before). Record and draw arrays grow as
   needed; the largest list had 17,768 triangles in 2,666 draws.
5. **A vertex's Q of 0.0 is 1.0** (commit 2f8978b). The first run of check (d) differed where PATH1's clipping sends
   vertices with S, T and Q all zero: the reference's GS front end reads Q 0.0 as 1.0 at every RGBAQ and PACKED ST
   write. `GifWalker` (`referenceQ`) now gives the vertex that Q. T9 (vi).
6. **The CLUT buffer** (same commit). The GS loads its CLUT buffer when TEX0 is written (as CLD says) and draws
   indexed textures through it; PATH3 re-uploads palettes between kicks while PATH1 goes on drawing with the TEX0 it
   wrote earlier, so reading the palette from memory at the draw was wrong for K2. The reader keeps step 5's `Clut`
   model, loads it at TEX0 writes and hands it to the texture cache (memory is used only before the first load).
7. **Float to int as the reference's x86 build does it** (commit 854540f). Clipped PATH1 triangles reach texture
   coordinates near 1e10. The reference converts with `cvttss2si`, which gives 0x80000000 for anything outside the
   int range, where the GPU's `int()` saturates; and its `ExactLround` of a bilinear value beyond 2^23 keeps the low
   32 bits of `lround`'s result. The shader (`cvtt`, `exactLroundInt`) and `gs_model` do the same. T9 (vii).
8. **The box rule's rectangle-pair test was wrong** (found 2026-11-21 by the `--vk-boxes` pass). `rectangleHalves`
   took any two triangles on three corners each of one axis-aligned rectangle with two corners in common for the
   halves of a quad. Two such triangles that share a *side* are cut along different diagonals and overlap; PATH1
   sends them (USA level 17 cycle 279, Europe level 24 cycle 132: 3 pixels wrong in 2 of 44,200 kicks with the box
   rule; K1's pairs are always halves, so step 7's results stand). The shared corners must now be opposite ones. The
   pixel rule never used the test (10.2). T10 (v) draws such pairs: with the old test the box rule differs on 357
   pixels and issues 1 barrier where 9 are needed.
9. **`MIPMAP` is counted, not a failure** (9.1's table, D-3D-2): 18.4.
10. **One fault of 11.4 cannot fail.** "A culled triangle drawn": the cull leaves out exactly what the reference's own
    early exit leaves out (no area, or a box outside the scissor); drawn, such a triangle covers no pixel. It is kept
    as a run that must pass (18.3).

### 18.2 Checks

**`rc_gs2d_test --validation --blend read`** (RTX 2060, driver 620.70.06, synchronisation validation on: the planted
hazard is reported), pixel rule: `PASS`, T1-T10, 2,241 scenes, 0 validation errors, 191 s (with item 8's change and
T10 (v); before it 2,239 scenes, `PASS`).

| test | result |
|---|---|
| model, T1 (44,236,800 pixels, read) | every pixel equal to the emulated GS |
| T2-T8 | pass (T8: 17 scenes, K1's layers under the pixel rule) |
| T9 (i) per-vertex Z under GREATER and GEQUAL, with and without the Z write | 381,528 values, model = emulated = Vulkan |
| T9 (ii) the 18 PATH1 states on an alpha-ramped 256 x 128 CT32 texture | 184,357 values equal |
| T9 (iii) REPEAT to +-1,000 texels, bilinear and nearest | 287,996 values equal |
| T9 (iv) vertices to 2,047 pixels out; (v) ADC vertices; (vi) Q 0; (vii) S/Q to 1e10 | 29,416; 21,724; 37,396; 168,904 values equal |
| T10 (o) spans against `gsTriCovers` | 327,060,069 covered pixels, 0 differ |
| T10 (i) water's three passes over a fan and a strip | equal; **1 barrier** (the box rule: 64) |
| T10 (ii) every fourth triangle moved; (iii) reading and exact interleaved; (iv) a pass per target | equal; 15, 32, 24 barriers, each the number an independent dense-map computation predicts (box rule: 64, 60, 696) |
| T10 (v) pairs sharing a rectangle's side | equal; 9 barriers under both rules |

**Checks (c) and (d) on all 67 dumps** (`rc_replay <dump> --consumer gif2d --vk --vk-blend read --vk-boxes --verify
--quiet --no-tables --validation`, synchronisation validation on, four at a time, 61 minutes; the re-referenced dumps
of batch R, nothing from `pre-r/`):

| | USA (34 dumps) | Europe (33) | both |
|---|---|---|---|
| (c) K1 and clear kicks judged | 46,877 | 41,544 | 88,421 |
| (c) pixels compared; differing in R, G, B, A or Z | 7,680,327,680; **0** | 6,806,568,960; **0** | 14,486,896,640; 0 |
| (d) K2 kicks judged | 23,433 | 20,767 | **44,200** |
| (d) pixels compared; differing in R, G, B, A or Z | 3,839,262,720; **0** | 3,402,465,280; **0** | **7,241,728,000; 0** |
| kicks the machine did not run; kicks without a state | 0; 0 | 0; 0 | |
| validation errors | 0 | 0 | |
| vertices beyond the viewport | 0 | 0 | |
| triangles with a vertex Z at or above 2^24 | 0 | 1 | (18.4) |
| `MIPMAP` primitives (levels 2 and 14) | 27,399; 59,687 | 31,071; 59,335 | (18.4) |
| (a) register files, (b) the encoded K1 in front of the machine | 0 differ on every dump | 0 differ | |
| result with the pixel rule | 34 / 34 | 33 / 33 | |

The box-rule pass of the same runs was equal everywhere but for item 8's three pixels (USA `s5/l17` 2, Europe `s5/l24`
1); with the corrected test both dumps pass in all planes under both rules.

**Barriers and draws** (all 67 dumps; the pixel rule beside the box rule):

| | pixel rule | box rule | 3.9 predicted |
|---|---|---|---|
| K2 barriers, mean a kick; largest | **16.15; 85** | 173.4; 879 | 16.2; 85 |
| K2 kicks with 0-4 / 5-16 / 17-32 / 33-64 / 65+ barriers | 10,933 / 13,158 / 14,861 / 5,196 / 52 | 1,349 / 735 / 2,439 / 6,783 / 32,894 | 10,938 / 13,153 / 14,861 / 5,196 / 52 |
| K2 reading primitives; groups; barriers | 10,085,550; 4,441,096; 713,894 | 10,085,550; 7,663,691; 7,663,691 | |
| K2 draws, mean a kick; largest | 559.0; 2,666 | 631.9; 2,812 | 557; 2,665 (boxes 630; 2,810) |
| K2 primitives drawn, mean; largest | 5,522; 17,768 | | 5,500; 17,768 |
| K1 barriers in all; largest in a kick | 457,720; 97 | 534,278; 119 | largest 96 (pixels), 119 (boxes) |
| K1 draws, largest in a kick | 165 | 167 | 123; 171 |

K1's largest kick has 97 barriers where 3.9 counted 96, and 165 draws where it counted 123: the survey counted
neither the barrier at a pass's first reading primitive (10.2 step 2) nor the draws the split adds.

### 18.3 Faults

`--vk-fault <name>` in read mode with the pixel rule; a fault is caught when the run ends `FAIL` on pixels.

| fault | USA `s5/l17`: K2 pixels differing (of 113,868,800); kicks failing (of 695) | USA `s5/l12` (water) |
|---|---|---|
| no barrier at a pass's first reading primitive (`no-first-barrier`) | fails: 277; 5 | fails: 1; 1 |
| a reading batch sees only its own batch's stamps (`cover-own-batch`) | fails: 5,528; 399 | fails: 3,547; 328 |
| no stamps for non-reading primitives (`no-stamp-other`) | fails: 337; 103 | fails: 403; 30 |
| the read epoch advanced without a barrier (`epoch-no-barrier`) | fails: 523,911; 512 | fails: 153,457; 520 |
| GEQUAL drawn as GREATER (`gequal-greater`) | fails: 3,949,763; 695 | fails: 3,656,047; 695 |
| the Z of a triangle's first vertex one more (`z-plus-one`) | fails: 6,470 colour, 34,637,808 depth; 695 | |
| REPEAT drawn as CLAMP (`repeat-clamp`) | fails: 4,271,248; 695 | |
| Q left out (`no-q`) | fails: 99,827,441; 695 | |
| a drawn triangle culled, every 16th (`cull-drawn`) | fails: 21,414,600; 695 | |
| a culled triangle drawn (`draw-culled`) | passes, as it must (18.1 item 10) | |

K1 is equal in every one of these runs (the faults touch only what K2 uses, or K1's lists need no barrier they
remove). `no-first-barrier` is caught by few pixels: in a race the first reading primitive of a kick usually lies
where nothing was drawn before it in the same pass; T10 (iv) is the test built for it. `cover-own-batch` is 10.3's
second fault read as a planted error (stamps of other batches ignored), not the box rule, which passes with more
barriers (18.2).

Step 7's faults again, on USA `s5/front` with the pixel rule (K1 293,273,600 pixels in 1,790 kicks, K2 146,636,800 in
895):

| fault | K1 pixels differing | K2 pixels differing |
|---|---|---|
| ATST LEQUAL as LESS (`atst-less`) | fails: 1,298,271 | fails: 7,780 |
| the failing part of the split dropped (`drop-fail`) | fails: 2,127,471 | 0 (K2 has no split) |
| ZTST GREATER as GEQUAL (`z-gequal`) | fails: 36,753 | fails: 422,323 |
| FIX off by one (`fix`) | fails: 6,350,468 | fails: 18,896,418 |
| a vertex colour's R and G swapped (`colour-swap`) | fails: 955,200 | fails: 113,134,126 |
| the half pixel left out (`no-half-pixel`) | fails: 440,619 | fails: 14,199,433 |
| the texel fetch one row down (`texel-row`) | fails: 9,364,035 | fails: 104,287,147 |
| the barrier left out (`no-barrier`) | fails: 783,666 | fails: 98,462 |
| a reading batch as one group (`one-group`) | fails: 68,534 | fails: 97,825 |
| the split per batch (`split-per-batch`) | passes, as in step 7 (`RENDERER_2D_DESIGN.md` 16.3: no pixel of these dumps depends on it; T3 catches it) | 0 |
| TEXA ignored (`texa`) | passes, as in step 7 (TEXA changes no picture of the dumps; T6 catches it) | 0 |
| the bias where a < b (`bias-always`) | not applicable in read mode (no bias) | |

In `rc_gs2d_test` every fault of both lists fails its test (T1, T3-T6, T8, T9, T10), except `bias-always` (not
applicable in read mode) and `draw-culled` (must pass).

### 18.4 The mip guard and the large Z

- **D-3D-2, `MIPMAP` (TEX1 with MXL 3 / 4): accounted for, no change to the plan.** The reference never samples a mip
  level (information item 3); `Gs2dVk` draws level 0 with the reference's filter. The primitives with the two
  mipmapped textures are counted (the table above) and are no longer a failure of `gif2d --vk`: on `s5/l2` and
  `s5/l14` of both versions every pixel of every K2 kick is equal, 177,492 such primitives among them. The four dumps
  that ended `FAIL` in batch R (17.3) now end `PASS`. Real mip levels stay P2's (plan step 9), as D-3D-2 says.
- **The vertex with Z at or above 2^24 (9.1): no pixel depends on it.** One triangle, Europe `s5/l22` cycle 283 kick
  47 (no blend, TEST 0x702f7, Z written): vertex Z 0x33a7cb, 0x118c2c and 0x165acc4. It covers 638 pixels inside the
  scissor; the largest Z the reference's interpolation gives a covered pixel is 0x27304a, none reaches 2^24, and the
  kick is equal in every plane. The shader's clamp to 2^24 - 1 (where the reference's Z24 store would keep the low 24
  bits) is therefore never reached; `gif2d --vk` prints the triangle and these counts whenever one appears, so a dump
  where a pixel does depend on it will show it.

### 18.5 Section 10.4's report: K2's GPU time

RTX 2060, 2026-11-21, one run at a time on an otherwise idle machine, no validation layer; timestamps around the
kick's passes; `--vk-blend both --vk-boxes`. Milliseconds a kick, mean / largest.

| USA dump | K2 fixed function (bias on) | **K2 read, pixel rule** | K2 read, box rule | draws a kick (pixel rule), mean / largest | barriers a kick, pixel / box | covered pixels a kick, mean / largest |
|---|---|---|---|---|---|---|
| `s5/l17` | 3.38 / 5.29 | **3.86 / 6.45** | 8.24 / 14.55 | 740 / 1,791 | 14.5 / 230.5 | 861 K / 1.33 M |
| `s5/l18` | 3.45 / 4.27 | **4.00 / 5.64** | 11.06 / 15.17 | 1,202 / 1,912 | 10.6 / 335.6 | 788 K / 0.93 M |
| `s5/l21` | 7.82 / 14.21 | **9.05 / 15.68** | 14.82 / 21.59 | 823 / 1,643 | 34.0 / 287.2 | 2.18 M / 4.21 M |
| `s5/l23` | 2.64 / 3.91 | **3.24 / 5.54** | 7.61 / 19.10 | 757 / 2,666 | 19.1 / 220.6 | 655 K / 0.85 M |
| `s5/front` | 2.21 / 4.69 | **2.69 / 6.02** | 4.64 / 15.10 | 682 / 1,782 | 14.6 / 110.0 | 519 K / 1.14 M |

K1 on the same runs: races 0.11 / 0.22 ms in read mode under either rule (0.08 / 0.12 fixed function); the front end
1.16 / 3.22 ms with the pixel rule (31.4 barriers a kick, 93 at most), 1.35 / 3.82 with the box rule (38.8; 115), 0.20
/ 0.44 fixed function.

CPU, where the list is built (read, pixel rule): the list 1.5-2.7 ms a K2 kick on average, of it the cover map 0.13-1.16
ms; the cover map's largest kick 2.44 ms (`s5/l21`, 2.1 M pixels looked at on average), under 10.2's 3 ms. (On the
67-dump run, four replays at a time with the validation layer, the largest was 5.5 ms; that machine was loaded.)

**Acceptance: over the limit on one of the five dumps as 10.4 measures it, and not judgeable by that measurement
(below).** In this run `s5/l21` takes **9.05 ms a K2 kick on average** in read mode with the pixel rule, over 10.4's
8 ms; the other four take 2.7-4.0 ms. As 10.4 says, nothing was optimised beyond "bind what changed"; the split
follows. `--vk-nofill` (a measurement: every draw's scissor cut to one pixel, so the draws, binds and barriers are
issued and almost no fragment is shaded):

| `s5/l21`, ms a K2 kick, mean | fixed function | read, pixel rule | read, box rule |
|---|---|---|---|
| as drawn | 7.82 | 9.05 | 14.82 |
| without fragments (`--vk-nofill`) | 0.32 | 0.59 | 0.83 |
| the fragments' share | 7.50 | 8.46 | 13.99 |

| the 9.05 ms of `s5/l21` (read, pixel rule) | ms | how measured |
|---|---|---|
| draws, binds, vertex work (823 draws, 2,196 binds) | 0.3 | the fixed-function list without fragments |
| the read path without fragments (34 barrier commands, the readable pass) | 0.3 | read minus fixed function, without fragments |
| the barriers with fragments in flight | 0.75 | 34.0 a kick at 22.8 us (box minus pixel rule: 5.77 ms for 253.2 barriers) |
| fill: 2.18 M covered pixels through the exact shader | 7.7 | the rest; 3.5 ns a covered pixel |

The same on `s5/l17` and `s5/l18`: without fragments 0.25 / 0.29 / 0.49 ms and 0.42 / 0.46 / 0.68 ms (fixed function /
pixel rule / box rule) of 3.38 / 3.86 / 8.24 and 3.45 / 4.00 / 11.06.

**The GPU's clock decides these numbers.** A replay submits one kick's lists every 16-25 ms of wall time, so the GPU
is mostly idle and the driver keeps it in a low power state: `nvidia-smi` during the runs shows P8 with the graphics
clock between 300 and 525 MHz of the device's 2,100 MHz. The same level 21, three ways:

| USA `s5/l21`, K2 in read mode with the pixel rule | ms a kick, mean / largest | the GPU meanwhile (sampled every 4-5 s) |
|---|---|---|
| alone, read only (one list a kick) | 21.86 / 38.10 | P8 on 48 of 49 samples, clock 300-315 MHz, 15% busy |
| alone, three settings a kick (the table above) | 9.05 / 15.68 | not sampled |
| four replays at a time, three settings a kick | 5.97-6.07 / 23.4-25.5 | P8 to P0, 405-1,005 MHz, 13% busy |
| the 67-dump run (four at a time, validation on, two settings) | 5.55 / 15.71 (Europe 5.27 / 17.03) | not sampled |

So the absolute times are those of a GPU clocked at a seventh to a half of its speed, they vary by a factor of
four with what else the GPU is given, and **10.4's acceptance cannot be decided by them**: if the time goes with
the clock as these runs suggest, level 21 at 2,100 MHz is well under 8 ms, but that was not measured, and the live
game's load (a list every 16.7 ms and a presentation) will set a clock of its own. In the 67-dump run every dump but the two level 21s stays at 3.4 ms a K2 kick or less. What does
hold is what was measured within one run: the order and rough ratios of the three settings, and the split below
(the runs without fragments load the GPU least, so their share is if anything overstated). 19.5 of the 2D design
and 10.4's 20.6 us a barrier were measured the same way and carry the same caveat. The clock was not forced
(`nvidia-settings` offers `GPUPowerMizerMode`; it is a setting of the user's desktop, left alone).

What this corrects in 10.4 and in information item 4:

- **K2's GPU time is fill, not draws.** At these clocks a draw with its state costs about 0.4 us (10.4 extrapolated 7 us from K1's 161
  tiny draws); the largest list timed without fragments took 1.1 ms (1,522 draws). A covered pixel costs about 4 ns
  (3.5-4.2 on the three dumps timed without fragments): the fragment shader computes the reference's arithmetic in doubles and writes `gl_FragDepth`, so no
  fragment is rejected before it is shaded, and level 21 covers 2.2 M pixels a kick (14 times the picture) where the
  others cover 0.5-0.9 M.
- **A barrier costs 20-23 us only because fragments are in flight** (0.9 us without them): l17 20.3, l18 21.7, l21
  22.8, l23 21.7, front 20.5 us. With the pixel rule they are 0.2-0.8 ms of a K2 kick; the box rule would have cost
  4.4-7.1 ms more a race kick.
- **10.4's levers for P2 (a texture array or atlas, fewer state runs) would not help here;** what would is fewer
  shaded fragments or a cheaper fragment (depth tested before shading where the alpha test allows it, front-to-back
  order of opaque states, single precision where it is provably exact). None of that is in step 8, and whether P2
  takes it is not decided here.

### 18.6 The game, the oracle, the replays

`rc_tex` (the GIF walker) and `rc_gs2d_vk` reach the game through `rc_live`, so `rc_revenge_pro` relinked in
`build-decomp` and `build-eu`; `rc_oracle` links nothing of `render/` and was not rebuilt. All on 2026-11-21 with the
final code:

- `rc_gs2d_test --validation --blend read` (synchronisation validation on), with item 8's change and T10 (v): `PASS`,
  2,241 scenes, 0 validation errors, 191 s; 20 faults fail their tests, `bias-always` not applicable, `draw-culled`
  passes as it must. `build-eu`'s `rc_gs2d_test` T8-T10: pass.
- The two dumps of item 8 and Europe `s5/l22` again with `--vk-boxes --validation`: `PASS`, every pixel equal under
  both rules.
- `live2d --verify --validation` (the live 2D path, now with the pixel rule) on `s6/boot`, `s6/load17` and `b4-key*` of
  both versions: 8 / 8 `PASS`, 0 validation errors (USA `s6/boot` 42,288,326 pixels compared, Europe 45,130,830, all
  equal).
- `tools/oracle.sh --all`: USA seed 2111, **1,632 ok, 0 FAIL** (528 s); Europe seed 2112, **1,563 ok, 0 FAIL** (938 s).
- `RC_SCENE_LOG=... RC_DEBUG_DUMP=... timeout 70 tools/play.sh replay playtest2`, idle machine: USA race scene 3020
  at frame 658 with the default renderer and with `RC_RENDERER=vk`, **60.0 fps** in both; Europe at frame 1167 in
  both, **50.0-50.2 fps** (`tools/fps.py`, the 5 s samples after the load).
- `RC_RENDERER=vk RC_VK_PICTURE=native RC_VK_VALIDATION=1`, USA `playtest2`, 70 s: scene 3020 at frame 658, 1,840
  lists drawn, 0 failed, 71,444 barriers, 0 validation errors; 30 fps as in step 7's batch 5 (the reader on the EE
  thread, U2).

### 18.7 Open

- **10.4's acceptance is not decided** (18.5). Level 21 takes 9.05 ms a K2 kick against the limit of 8 in the run
  10.4 asks for, 6.0 ms with four replays sharing the GPU and 21.9 ms alone with one setting, all with the GPU held
  at 300-1,005 MHz of 2,100 by the driver's power management; reported, not optimised. The time is the exact fragment
  shader on 2.2 M covered pixels a kick, not draws or barriers. Needed, and for the user or P2 to choose: a timing
  that holds the clock (the driver's performance mode for the measurement, or the live game's own timestamps in
  batch 4), and then, if level 21 is still over, what may change in the shader without losing equality. With U2 (the
  references on the CPU, 24-49 ms a kick) this is not what limits step 8's frame rate.
- The Z24 store of a pixel Z at or above 2^24 is still a clamp in the shader and a mask in the model (18.4): unreached.
- The fixed-function fallback draws K2 within D-2D-1's bound only; it was timed here (18.5) and compared on five dumps,
  not on all 67.
- Batch 3 (the chained replay) can start: nothing in it waits for a decision except 18.5's, which does not block it.

## 19. Batch 3 as built (2026-11-21): the chained replay

Section 13 item 3. Built: `rc_replay --consumer vk` (`tools/rc_replay/frame_vk.{h,cpp}`, `FrameVkCheck`): the frame
reader and `Gs2dVk` on an exact dump with no emulated machine beside them, seeded once, every cycle's list drawn, every
flipped picture and every later keyframe compared. **It passed on all 67 dumps at the first run; no check found a
difference, so nothing in the reader, the references, the shader or `Gs2dVk` was changed.** Usage: `NOTES.md`, "The
chained replay". Live is batch 4.

```
tools/rc_replay/frame_vk.{h,cpp}  FrameVkCheck (rc::rec::Consumer): FrameReader + Gs2dVk, seeding, the comparisons, PNGs, faults, the report
tools/rc_replay/rc_replay.cpp     --consumer vk, runFrameVk (no runtime machine is made), --fault depth|clear|texture|flip
CMakeLists.txt                    frame_vk.cpp in rc_replay
```

Nothing outside `tools/rc_replay` changed but a comment in `render/frame/frame_reader.h` (19.1 item 4): no library's code,
no shader, no game source, not the runtime. The game relinked for that comment and its build string and was checked
(19.6).

### 19.1 Changes to the design, and why

1. **One chain a dump, not one a segment.** 11.5 seeds at each segment's keyframe. As built the targets, the depth
   buffer, texture memory, the register file and the VU1 / VIF1 state are seeded at the dump's first keyframe only (and
   at a keyframe after a save-state load, where the recording's own machine was replaced: one in each version's
   `b4-x2-0`). Every later keyframe is compared and then *not* loaded, so a dump is one chain from its first cycle to
   its final keyframe (up to 2,700 cycles, `s5/l17long`) and an error cannot be hidden by a re-seed 300 cycles later.
   `--cycles a-b` starts at the keyframe of the segment that holds `a`, as the other consumers do.
2. **The final keyframe is compared too** (`GSVR` only, as it holds): targets and Z after the last cycle.
3. **`Gs2dVk` directly, not the live presenter.** `rc::live::Presenter` (step 7 batch 5) merges lists, shows upload-only
   pictures from `FrameBlit`'s CPU picture and never waits; the check wants each cycle's list drawn, waited for and read
   back. The live path's own check is batch 4's (section 12).
4. **Uploads into the frame buffers are the GIF reader's `IMAGE_BLIT` commands** (`Gif2dReader::Options::frameUploads`,
   built by step 7's batch 5), in list order with the draws. `FrameBlit` therefore does not join `FrameReader` (16.1
   item 5 and 16.6 expected it here): the logos, the movie's tiles, the splashes and the front end's backdrop reach the
   targets through the list, and a picture that is all upload is read back from the target like any other (727 + 666
   cycles whose list is uploads only, all equal). An upload the reader cannot place (not CT32, not FBW 10, not on a page row) fails the run:
   0 of 1,445,504 transfers.
5. **The register file: `GSRG` once, through `Gif2dReader::Options::seed`.** After a `GSRG` seed no register is unknown,
   so the option's other half (unknown registers taken from a kick's recorded file) never runs; no `KICK.regs` is read
   in the chain. Draws that still used a register as the keyframe left it (never written by the stream since): FBA_1 on
   every draw (written by neither path in any dump, value 0, the default; 16.2); in the four boot dumps PABE (3 draws
   each) and, USA only, CLAMP (8 draws, a value other than the default); in each version's `b4-x2-0` (the dump with the
   save-state load) 10 draws take FRAME, ZBUF, XYOFFSET, PRIM, COLCLAMP and DTHE from `GSRG`. Nothing
   else in 557.8 M primitives: the reader's register file is the stream's.
6. **A pass per target as `Gs2dVk` already makes them:** one render pass from a change of target (or an upload, which
   ends the pass) to the next, readable in read mode; a cycle's list is one submitted frame, waited for.
7. **Also a failure:** a guard of the VIF1 reader, an unsupported state, a skipped sprite, a vertex beyond the viewport,
   DISPFB2 showing neither target or not `END.refFbp`, a refused list, a failed read-back, a validation error. None
   occurred. `MIPMAP` and a vertex Z at or above 2^24 are counted (18.4).
8. **Z is compared in its 24 bits** (the Z buffer is read through the Z32 layout as in check (d); the depth read-back
   must be a whole Z).
9. **The faults** (11.5), `--fault <name>:<cycle>[:<cycles>]` (default 1 cycle, 0 to the end): `depth` zeroes the depth
   buffer after each channel 1 kick's commands (the list is drawn in parts); `clear` leaves out the cycle's `CLEAR`;
   `texture` leaves out every `TEXTURE` command for an id already sent (the new generation after an upload); `flip`
   reads the other target. 19.3.
10. **The largest list is 4.7 MB**, not "under 2.2 MB" (section 8): a level's first cycles carry their textures
    (3.75 MB in most level dumps; `s5/l18` 4.47 MB, `s5/l7` 4.66 / 4.70 MB). The mean is 0.4-1.2 MB a cycle.

### 19.2 Checks

`rc_replay <dump> --consumer vk --verify --quiet --validation` with `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true`, RTX 2060
(driver 620.70.06) by PRIME offload, read mode (the exact blend, the pixel rule), four at a time, all 67 exact dumps
(the re-referenced ones; `build-decomp/frame-dumps/{s5,s6,b4-key*,b4-x2-*}` and `build-eu`'s; nothing from `pre-r/`):
**67 / 67 `PASS`**, 661 s. Again without the validation layer: 67 / 67 `PASS`, 545 s, the same counts.

| | USA (34 dumps) | Europe (33) | both |
|---|---|---|---|
| keyframes seeded (first; after a save-state load) | 35 (34; 1) | 34 (33; 1) | 69 |
| cycles drawn | 24,174 | 21,445 | 45,619 |
| ... with K2 / PATH3 draws only / uploads only / nothing | 23,433 / 14 / 727 / 0 | 20,767 / 12 / 666 / 0 | |
| VIF1 kicks; PATH3 kicks through the readers | 23,433; 816,488 | 20,767; 743,351 | 44,200; 1,559,839 |
| **flipped pictures whose hash equals `END.refHash`** | **24,174 / 24,174** | **21,445 / 21,445** | **45,619 / 45,619** |
| stored pictures (`REFI`) equal pixel by pixel | 1,430 / 1,430 (219,648,000 pixels, 0 differ) | 1,430 / 1,430 (234,291,200, 0) | 2,860; 453,939,200 pixels |
| **later keyframes: targets 0x0 and 0x50 (R, G, B, A) and Z equal over 640 x 256** | **103 / 103** (69 inside, 34 final) | **95 / 95** (62, 33) | **198**; 97,320,960 values, 0 differ |
| DISPFB2 on neither target; FBP not the reference's | 0; 0 | 0; 0 | |
| guarded VIF1 kicks; unsupported states; sprites skipped; vertices beyond the viewport | 0; 0; 0; 0 | 0; 0; 0; 0 | |
| draw failures; read-back failures; validation errors (layer on, synchronisation validation) | 0; 0; 0 | 0; 0; 0 | |
| primitives read; dropped (all FRAME.FBW 0, before a boot's first draw environment: 0 pixels) | 292,959,100; 326 | 264,805,898; 274 | |
| culled: no area; outside the scissor | 3,443,291; 161,153,394 | 2,930,323; 144,810,767 | |
| `MIPMAP` primitives; triangles with a vertex Z >= 2^24 | 87,086; 0 | 90,406; 1 | (18.4's) |
| uploads into the frame buffers; as `IMAGE_BLIT`s; not placed | 710,272; 1,212; 0 | 735,232; 1,262; 0 | |
| `TEXTURE` commands sent | 32,646 (408.5 MB) | 33,947 (419.0 MB) | |
| draws; a cycle; largest cycle | 13,561,483; 561; 2,609 | 11,241,803; 524; 2,218 | |
| barriers; a cycle; largest cycle | 636,373; 26.3; 123 | 543,950; 25.4; 126 | 10.1 expected 26 and 126 |

- The plan's first list (levels 5, 17, 18, 20, the front end and the Track Editor of both versions) is in it: `s5/l5`,
  `l17`, `l17long`, `l18`, `l20`, `front`, `tedit`, 700-2,700 cycles each, every picture and keyframe equal. `s5/boot`
  and `s6/boot` bring the legal screen, the logos, the movie (600 tiles a picture), the title and the attract demo's
  level load through the same chain; `s6/load17` the loading splash into a race; `b4-x2-0` a save-state load.
- The longest chains: USA `s5/l17long`, 2,700 cycles from one seed (2,685 VIF1 kicks, 101,681 PATH3 kicks, nine later keyframes); `s5/boot`
  and `s5/front`, 900.
- **What it proves beyond check (d):** per kick, (d) seeded the targets from the machine before every kick; here
  nothing comes from the machine after the first keyframe, so the depth buffer carried between kicks and cycles, the
  targets under the uploads and the clear, texture memory and the CLUT buffer across 2,700 cycles, the VU1 memory and
  registers across kicks, and the flip all have to be right for the last picture to hash equal.
- Fixed function (`--vk-blend ff`, the fallback of D-2D-1) is not expected to pass and does not; its size over whole
  frames is in 19.5.

### 19.3 Faults

`--fault <name>:<cycle>:4` (four cycles from the fault), `--cycles 0-<cycle + 40>`, no validation. The clean runs of the
same dumps pass (19.2). A fault is caught when the run ends `FAIL`.

| fault | USA `s5/l17` (cycle 120) | USA `s5/front` (200) | USA `s5/tedit` (2020) | Europe `s5/l17` (120) |
|---|---|---|---|---|
| `clear` (the flip's clear left out) | fails: 4 pictures | fails: 4 | fails: 4 | fails: 4 |
| `flip` (the picture read from the other target) | fails: 4 pictures; cycle 121's wrong picture hashes as cycle 120's reference | fails: 4 | fails: 4 | fails: 4 |
| `texture` (a generation not re-sent after an upload) | fails: 3 `TEXTURE` commands left out, 3 pictures (121-123) | fails: 6 left out, 5 pictures | passes, as it must: nothing to leave out (no texture is sent twice in the whole dump, checked with `texture:1903:0`) | fails: 3 left out, 12 pictures |
| `depth` (the depth buffer zeroed after K2's draws) | no picture changes; with the fault at cycle 299 the keyframe at 300 fails: Z differs on 153,423 pixels | no picture changes; at 299: keyframe Z, 128,640 pixels | fails: 4 pictures | no picture changes; at 299: keyframe Z, 163,663 pixels |

- **The depth fault and what the pictures depend on.** A cycle's kicks run clear, K2, K1 (K1 has primitives in every
  Track Editor cycle, in 743 of 900 front-end cycles and in every second cycle of a race: USA `s5/l17` 349 of 700).
  With the depth buffer zeroed between K2 and K1 no picture of a race or of the front end changes, also with the
  fault held for long: USA `s5/l17` and Europe `s5/l5` 150 cycles each, USA `s5/front` 180, USA `b4-x2-1` all 404
  (there the four later keyframes fail, Z on 153,423-153,600 pixels each). K1's Z tests are GREATER and zero is the
  smallest Z, so the fault can only let through K1 pixels that K2's depth would have rejected: in those scenes K2's
  depth rejects none, in the Track Editor it does. Everywhere else the fault is caught by the Z plane at the next
  keyframe, which is why the keyframe comparison includes Z. So 11.5's "the depth buffer not carried from K2 to K1"
  fails on pictures in the Track Editor and on keyframes elsewhere; in a cycle not followed by a keyframe before
  the next clear nothing that is drawn depends on it.
- `flip`'s hashes show the chain's order: the other target holds the previous cycle's picture.
- `--png`: a failing cycle writes `fail_<cycle>_vk.png`, and with a stored picture `_ref.png` and `_diff.png` (red: colour
  differs, blue: alpha only); a failing keyframe `keyframe_<cycle>_{fb00,fb50,z}_{vk,ref,diff}.png` for each plane that
  differs (Z as its three bytes); at most 12 sets a run. Without a stored picture the `FAIL` line gives the
  `rc_replay <dump> --consumer emu --cycles n-n --png <dir>` command that makes the reference. Tried with `clear` on
  USA `s6/load17` (72,160 and 72,431 of 153,600 pixels differ in the two cycles) and `depth` before a keyframe.

### 19.4 Run time

Ryzen 7 3750H, RTX 2060. The chain is one process and one thread plus the driver's.

| a dump (cycles) | alone, no validation: s; ms a cycle | of it the reader (a VIF1 kick, largest) | the draw, waited for (the list's build) | read-back and hash | GPU a list, mean / largest | GPU clock meanwhile |
|---|---|---|---|---|---|---|
| USA `s5/l17` (700) | 32.9; 46.9 | 30.8 (30.7, 53.7) | 11.4 (1.2) | 2.3 | 7.27 / 16.59 ms | P8, 300-375 MHz |
| USA `s5/l18` (700) | 49.9; 71.3 | 52.1 (52.1, 81.1) | 15.1 (1.8) | 2.4 | 9.07 / 12.96 | P8, 300-375 |
| USA `s5/l21` (700) | 37.6; 53.7 | 36.0 (35.8, 54.3) | 13.7 (2.2) | 2.2 | 8.74 / 15.67 | P8, 300-375 |
| USA `s5/l5` (700) | 29.3; 41.9 | 30.0 (29.9, 46.5) | 8.1 (1.3) | 2.2 | 4.24 / 11.27 | P8, 300-375 |
| USA `s5/l20` (700) | 32.3; 46.1 | 30.2 (30.0, 46.9) | 11.5 (1.4) | 2.3 | 7.05 / 10.89 | P8, 300-375 |
| USA `s5/front` (900) | 32.4; 36.0 | 24.3 (24.2, 45.9) | 8.4 (1.4) | 2.1 | 4.34 / 6.13 | P8, 300-375 (one sample 1,005) |
| USA `s5/boot` (900) | 23.4; 26.0 | 16.2 (21.9, 32.1) | 6.7 (1.0) | 1.8 | 3.45 / 5.55 | P8, 375-600 |
| USA `s5/tedit` (700) | 3.6; 5.1 | 0.8 (0.7, 1.6) | 2.3 (0.6) | 1.1 | 0.95 / 1.15 | P8, 300 |
| Europe `s5/l17` (700) | 34.7; 49.5 | 33.1 (32.9, 54.7) | 12.6 (1.4) | 2.3 | 8.14 / 23.14 | P8, 300-375 (one sample 1,005) |
| Europe `s5/l18` (700) | 48.5; 69.3 | 49.9 (49.9, 79.8) | 15.2 (1.8) | 2.4 | 9.30 / 12.53 | P8, 300-375 |

- **All 67, four at a time:** 661 s with the validation layer (2,609 s of process time: 0.8-64 s a dump, `s5/l17long`
  173 s; 26-92 ms a cycle, the Track Editor 10-13), 545 s without it (2,149 s of process time).
- Three quarters of a race dump's time is the reader, and that is the C++ VU1 references (16.3, U2). The GPU's part
  is the draw waited for; the read-back with its hash is 2-3 ms a cycle.
- **The GPU times are off-line times at a low clock (U3):** every sample of `nvidia-smi` during the runs alone shows
  P8 at 300-375 MHz of 2,100 (two samples of 1,005 and one of 600 among 87), 12-20% busy. A cycle's list (K2, the clear, K1,
  uploads) then takes 4.2-9.3 ms on a level and 0.95 ms in the Track Editor; with four replays sharing the GPU and the
  validation layer on the same lists took 1.4-6.1 ms (level 21: 6.1). As in 18.5 these numbers order the dumps and
  nothing more; no driver setting was changed. Batch 4's live timestamps decide 10.4 (U3 = B).

### 19.5 Divergences, drafted for `DIVERGENCES` (P2 step 10)

Section 14's rows with what batches R, 1, 2 and 3 measured, and the rows those batches added. "Inherited" means the
reference (the runtime's software GS, the machine the dumps' pictures come from) does it and the renderer reproduces
it on purpose, so that the checks can be equalities; a real GS differs. With the exact blend (read mode: the default
where the device has the local read) **the renderer's picture differs from the reference's in no pixel of any flipped
picture of the 67 dumps (19.2)**, so every row below is either inherited, a change to the reference itself, a
fallback, or a guard that never fired.

| id | what | size | where |
|---|---|---|---|
| D-3D-1 (batch R, U1 = A; a change to the reference) | PATH1 is rasterised under the kick's rounding mode (round to nearest) instead of the VU1 run's (toward zero), and VU1 runs are no longer cut at 65,536 cycles. The `gs` renderer's software path changed with it; the GPU back ends did not | a level on a third of a race's pixels (USA level 17 31.8%, Europe 36.9%; mostly +1), 4-12% in the front end, none without 3D; 575 + 520 water runs finished (17.2) | every 3D picture of the software GS; the hardware's own rule is not known here |
| D-3D-2 (inherited) | mip levels are not sampled: TEX1 with MXL 3 / 4 is drawn from level 0 with the reference's filter | 177,492 primitives: USA 27,399 (level 2) and 59,687 (level 14), Europe 31,071 and 59,335; every pixel equal to the reference (18.4) | two textures; real mip levels are P2's (plan step 9) |
| D-3D-3 (inherited) | the reference's float interpolation applies to PATH1 as to PATH3: weights, colours, S/T/Q and Z in host floats and doubles, the bilinear filter with float weights (I-1 and I-2 of `RENDERER_2D_DESIGN.md` 12) | at most a level | every 3D pixel |
| D-3D-4 (guard) | a VIF code, UNPACK form, program or entry outside sections 4 and 5, or a run that sends more than 2^20 packets (`VU_RUNAWAY`), stops the kick's 3D | none fired in 44,200 VIF1 kicks | logged, counted; the chained replay fails on one |
| D-3D-5 (inherited; batch 2) | a vertex Q of 0.0 is read as 1.0 (the reference's front end, at every RGBAQ and PACKED ST write) | the vertices PATH1's clipping sends with S, T and Q all zero; pixels not counted | 18.1 item 5, T9 (vi) |
| D-3D-6 (inherited; batch 2) | float to int as the reference's x86 build: out of range gives 0x80000000 (`cvttss2si`), and the bilinear rounding beyond 2^23 keeps the low 32 bits | texels of clipped triangles whose texture coordinates reach 1e10; pixels not counted | 18.1 item 7, T9 (vii) |
| D-3D-7 (unreached) | a pixel Z at or above 2^24: the shader clamps where the reference's Z24 store keeps the low 24 bits | one triangle has such a vertex (Europe `s5/l22` cycle 283); no pixel of it reaches 2^24 (18.4) | reported by `gif2d --vk` and counted by `vk` whenever one appears |
| D-3D-8 (state, no picture) | 72 lanes of the VU1 vector registers are not reproduced by the references (the microcode's temporaries); they are overwritten with a signalling NaN after every run | no packet and no byte of VU memory depends on them in 43.1 M runs (16.1 item 1) | `vu1_programs.cpp` |
| D-3D-9 (guard) | an upload into a frame buffer that is not CT32 at FBW 10 from a page row is not drawn | none in 1,445,504 uploads (19.1 item 4) | counted; the chained replay fails on one |
| D-3D-10 (the reader, no picture) | triangles of no area or outside the scissor are left out of the list, and a draw with FRAME.FBW 0 is dropped | 56% of PATH1's triangles; 326 + 274 primitives; the reference draws no pixel for any of them (16.2 (c2), 18.3 `draw-culled`) | |
| D-2D-1 (fallback; unchanged) | fixed-function blending on a device without the local read, or with `ff` chosen: per inexact layer -1..+1. Over whole frames, chained (`vk --vk-blend ff`, the four `s6` dumps, 2,600 stored pictures): a race differs on 41% of its pixels (USA `s6/load17` 25.2 M of 61.4 M; Europe 27.2 M of 65.5 M), 22% of the colour values by one level, 0.1% by more, largest 8; the front end and the attract demo on 4-7% (largest 4); alpha and Z never; nothing accumulates across cycles (the clear) | as left | K2's and K1's layers with a fractional blend factor |

Not divergences of the renderer, for the same list: the first keyframe of a re-referenced dump (and one after a
save-state load) still holds the old machine's drawing in `GSVR` (17.2; both chains start from it); DISPLAY2 is
ignored and the stored alpha is not shown (step 6).

### 19.6 The game, the oracle

The batch added two files to `rc_replay` and changed `rc_replay.cpp` and `CMakeLists.txt`; a comment in
`render/frame/frame_reader.h` was corrected (19.1 item 4). The game relinked in `build-decomp` and `build-eu` for two
reasons that change no behaviour: `render/record/capture.cpp` carries the build's `git describe` (recompiled at the
first configure after the change), and `rc_frame` / `rc_live` recompiled for the comment. `rc_oracle` had nothing to
rebuild in either tree (its binaries date from batch R). Checked all the same, 2026-11-21, on the final build:

- `tools/oracle.sh --all --seed 3371` (USA): **1,632 ok, 0 FAIL** (511 s).
- `RC_VERSION=eu tools/oracle.sh --all --seed 3372`: **1,562 ok, 1 FAIL** (909 s): `WeaponProcess` on `level17_cc`, 1 of
  20 inputs (iteration 1: the original stores 0xdb at address 0x8, the native does not; `v0` 5 against 8). It repeats
  with `--only WeaponProcess --seed 3372 --cases 20` and passes with seed 2112. **Not this batch's:** the oracle
  binary was not rebuilt and links nothing of `render/`; it is a difference between the native `WeaponProcess` and the
  original, or in the harness's setup for it, that this seed reaches (as seeds 7170 and 9494 found theirs). Not
  looked into here; left for its own fix. USA passes with its seed.
- `RC_SCENE_LOG=... RC_DEBUG_DUMP=... timeout 70 tools/play.sh replay playtest2`: USA race scene 3020 at frame 658,
  60.0 fps; Europe at frame 1167, 50.0-50.2 fps (`tools/fps.py`, the 5 s samples after the load): as before.
  `RC_RENDERER=vk RC_VK_PICTURE=native` (USA): scene 3020 at frame 658, about 30 fps as in 18.6 (U2).
- `build-eu`'s `rc_replay`: Europe `s5/tedit`, `s5/front` and `s6/load17` `PASS` (`s6/load17` with validation, 0 errors,
  400 / 400 stored pictures equal).

### 19.7 Open

- Nothing from the batch's own checks. The chained replay is equal on every cycle and keyframe of the 67 dumps.
- **Europe `WeaponProcess`, oracle seed 3372** (19.6): one input of 20 on `level17_cc` differs (a store to address 0x8
  and the return value). In the native or the harness, unrelated to the renderer; to be looked into on its own.
- The depth fault shows that outside the Track Editor no picture of the dumps depends on the depth K2 leaves for K1
  (19.3); the scenes not recorded yet (podium, results, the Track Editor's race, more two-player; section 12) may.
- U3 (K2's GPU time) is batch 4's to measure; 19.4's times carry 18.5's caveat.
- `FrameBlit` stays outside `FrameReader` (19.1 item 4); the live presenter still uses it for upload-only pictures
  (step 7 batch 5). Batch 4 decides whether the live path keeps that or shows the target, as the chain does.
- The originals under `frame-dumps/pre-r/` (6.5 GB) can go when step 8 is closed.
- Batch 4 (live) can start.

## 20. Batch 4 as built (2026-11-22): live 3D

Section 13 item 4. `RC_RENDERER=vk RC_VK_PICTURE=native` now draws the whole picture live: the frame reader on the EE
thread (step 7 batch 5's `render/live` consumer) puts K2's triangles in the cycle's list, and the presenter draws the
list (uploads, the clear, K2, K1) through `Gs2dVk` with the exact blend and the pixel rule and shows
`target[DISPFB2.FBP]`. The live reader's lists go into a recording as `DLST` chunks, and `rc_replay --consumer dlst`
proves them equal to the lists made off-line from the recorded kicks. **Every correctness check passed at the first run;
nothing in the reader, the references, the shader or `Gs2dVk`'s drawing had to change. The frame-rate check does not
pass (U2: the references on the EE thread), so `gs` stays the default.** Usage: `NOTES.md`, "Live 3D".

```
render/live/live2d.{h,cpp}     Reader: K2's primitives in the list (Gif2dReader::Options::path1Prims), FrameBlit no longer
                               run (20.1 item 1), the cycle's VIF1 kicks published as command ranges, a list sink;
                               Mailbox: a stripped list keeps TEXTURE and IMAGE_BLIT; Presenter: the target for every
                               picture, K2's GPU time a kick and a frame (U3), a window of its stats for the periodic line
render/live/live_host.{h,cpp}  the DLST sink, the presenter's periodic line, RC_VK_SHOT_* also the emulated GS picture
render/gs2d/gs2d_vk.{h,cpp}    record(list, cl, stamps, ranges): timestamps 2 + 2k and 3 + 2k around each range's draws
render/frame/frame_reader.*    path1Ranges(): each channel 1 kick's commands in the list
render/frame/dlst.h            the DLST payload (header, commands, arena) and its encoder
render/record/                 format.h kChunkDrawList; writer Writer::drawList (a job after the cycle's CYCL and REFI);
                               capture rc::rec::drawListWanted / recordDrawList (RC_FRAME_RECORD_DRAWLISTS=1); reader
                               hands a DLST after a delivered cycle to the new Consumer::chunk
platform/vk_host.cpp           the window title; the GS picture of the present handed to the screenshots
tools/rc_replay/dlst.{h,cpp}   rc_replay --consumer dlst (the live check)
tools/rc_replay/live2d.*       every pixel judged (K2 is drawn)
tools/live_check.sh            a native session in exact mode from boot with DLST, then dlst and vk --verify
tools/shot_compare.py          native_<c> against gs_<c> screenshots
```

No shader, no VU1 reference, no runtime file and no oracle source changed; `rc_oracle` had nothing to rebuild in either
tree. The game relinked (it links `rc_live` and the capture).

### 20.1 Changes to the design, and why

1. **Upload-only pictures are shown from the target, as the chained replay shows them (19.7's question).** The live
   reader no longer runs `FrameBlit`; the uploads reach the targets as the GIF reader's `IMAGE_BLIT` commands in list
   order (batch 5's `frameUploads`) and the presenter samples `target[DISPFB2.FBP]` for every picture. It is equal: the
   chained replay already showed all 1,393 upload-only cycles of the 67 dumps from the target (19.1 item 4); the live
   classes on USA `s6/boot` (`rc_replay --consumer live2d`, now judging every pixel) give 900 / 900 pictures equal to
   `REFI` on all 138,240,000 pixels (258 of them upload-only); and the live sessions below include the boot's logos and
   movie (255 USA, 254 Europe upload-only cycles in the `front` sessions), every one hash-equal. It saves `FrameBlit`'s
   work on the EE thread and a picture upload a frame on the main thread. `FrameBlit` stays for `rc_replay --consumer
   blit`.
2. **The live check needs a dump recorded from boot.** Section 12 asked for "a short exact-mode session". The live
   reader starts at the machine's reset with the game and numbers its textures in the order it first meets them
   (`Gif2dReader::textureId`, generations per cache entry), so a reader started at a mid-session keyframe sends
   other ids and other `TEXTURE` commands and no list could be byte-equal. `rc_replay --consumer dlst` therefore runs
   the live `rc::live::Reader` itself, from reset, on a dump whose first cycle is cycle 0 (`RC_FRAME_RECORD_AT=boot+n`),
   with no keyframe but one after a save-state load (as the capture gives the live consumer), and refuses any other
   dump (exit 2). The sessions are 800-2,800 cycles, 100-290 s each in exact mode with the reader.
3. **"Through the front end into races on levels 5, 17 and 18":** no pad recording drives the menus into those levels
   (`playtest2` goes through the menus into one race, and no recording chooses levels 5, 17 or 18). As built: a
   `front` session (`replay playtest2`: logos, the movie, the title, the menus, the level load and its race) and one
   session per level booted into it (`RC_BOOT_LEVEL`: the loading screen and the
   attract race), plus the Track Editor (`RC_SKIP_INTRO=1`, `tools/tedit_walk.py 1 7000`: the menus into the editor and a
   random edit). Each in both versions.
4. **`DLST` is written live by the capture** (`RENDERER_FRAME_RECORD.md` 9 had planned a `--save-drawlists` copy): the
   live consumer's cycle close comes after the recording's in the capture, so `rc::rec::recordDrawList` queues the chunk
   right behind its cycle's `CYCL` and `REFI` and before the next keyframe; it is written only for a cycle that was
   recorded and only from the EE thread. Ring dumps carry it too (the ring keeps whole chunks), though only a dump from
   boot can be checked.
5. **U3's measurement:** timestamps around each VIF1 kick's draws inside the frame's own command list
   (`Gs2dVk::record`'s `ranges`: the first draw made from a kick's first command, the first draw after its last;
   `RHI_TIMESTAMP_KEEP_BARRIERS`, so the image barriers recorded are those of the list without them; at most 126 kicks a
   frame, none exceeded). K2's time a frame is the sum over its kicks. The game's GPU is clocked as the game loads it,
   which is what 18.7 asked for (20.5).
6. **The mailbox keeps uploads when it strips a list** (the presenter 16 lists behind): `TEXTURE` and now `IMAGE_BLIT`,
   because the targets persist and a later picture can show an upload without repeating it. Never triggered in the
   runs below (0 lists reduced).
7. **Screenshots of both pictures at once:** `RC_VK_SHOT_*` writes the emulated GS picture of the same present beside the
   native one (`gs_<cycle>`), so check 2 compares pictures of one moment; `tools/shot_compare.py` summarises the pairs.

### 20.2 The live check (section 12; check 1)

`tools/live_check.sh` per session: `RC_RENDERER=vk RC_VK_PICTURE=native` in exact mode (`tools/frame_record.sh`) recorded
from boot with `RC_FRAME_RECORD_DRAWLISTS=1` (every 10th reference picture stored), then `rc_replay <dump> --consumer
dlst` and `rc_replay <dump> --consumer vk --verify` (RTX 2060 by PRIME offload, read mode). **20 / 20 `PASS`.**

| session (cycles from boot) | version | DLST lists equal byte for byte | commands; VIF1 kicks | chained replay: flips hash-equal; `REFI` equal (pixels); later keyframes equal | cycles with K2 / PATH3 only / uploads only |
|---|---|---|---|---|---|
| `front`: `replay playtest2` (1,500 / 1,700) | USA | **1,500 / 1,500** | 1,007,004; 1,240 | 1,500 / 1,500; 150 / 150 (23.0 M, 0 differ); 5 / 5 | 1,240 / 5 / 255 |
| | Europe | **1,700 / 1,700** | 1,522,179; 1,441 | 1,700 / 1,700; 170 / 170 (27.9 M, 0); 6 / 6 | 1,441 / 5 / 254 |
| `l5`: `RC_BOOT_LEVEL=5` (800) | USA | **800 / 800** | 1,197,588; 795 | 800 / 800; 80 / 80 (12.3 M, 0); 3 / 3 | 795 / 0 / 5 |
| | Europe | **800 / 800** | 1,166,756; 795 | 800 / 800; 80 / 80 (13.1 M, 0); 3 / 3 | 795 / 0 / 5 |
| `l17` (800) | USA | **800 / 800** | 1,331,090; 793 | 800 / 800; 80 / 80 (12.3 M, 0); 3 / 3 | 793 / 0 / 7 |
| | Europe | **800 / 800** | 1,411,315; 795 | 800 / 800; 80 / 80 (13.1 M, 0); 3 / 3 | 795 / 0 / 5 |
| `l18` (800) | USA | **800 / 800** | 1,772,613; 795 | 800 / 800; 80 / 80 (12.3 M, 0); 3 / 3 | 795 / 0 / 5 |
| | Europe | **800 / 800** | 1,911,376; 795 | 800 / 800; 80 / 80 (13.1 M, 0); 3 / 3 | 795 / 0 / 5 |
| `tedit`: the Track Editor (2,800) | USA | **2,800 / 2,800** | 1,231,906; 2,795 | 2,800 / 2,800; 280 / 280 (43.0 M, 0); 10 / 10 | 2,795 / 1 / 4 |
| | Europe | **2,800 / 2,800** | 1,400,481; 2,797 | 2,800 / 2,800; 280 / 280 (45.9 M, 0); 10 / 10 | 2,797 / 1 / 2 |
| **total** | | **13,600 / 13,600 lists** | 13.95 M commands; 13,041 VIF1 kicks | **13,600 / 13,600 flips; 1,360 / 1,360 stored pictures (216.0 M pixels, 0 differ); 49 / 49 keyframes** | |

Later sessions of this batch pass the same: two longer Europe level 5 attract races (`l5long` 1,450 cycles, `l5hunt`
3,600: 20.3) and the four new-scene sessions of 20.7 (5,800 cycles each): **in all, 16 sessions and 41,850 live lists
equal byte for byte, 41,850 flips hash-equal in the chained replay.**
No VIF1 guard, no unsupported state, no unplaced upload, no vertex beyond the viewport in any of them. The `DLST`
chunks are 0.46-1.2 MB raw a cycle (0.15-0.32 MB stored; the textures a list re-sends dominate), 460 MB to 1,036 MB raw
a session; a session dump is 221-454 MB. `dlst` takes 26-64 s a dump (the reader is the time), the chained replay 34-120 s.
A list is equal only when every one of its bytes is (the commands' fields, the payload arena with its zero padding;
the `GsState` and vertex records carry no padding). Planted (`rc_replay <dump> --consumer dlst --fault list:<cycle>[:<byte>]`,
one byte of that cycle's off-line encoding flipped; USA `l17`): in the arena (cycle 400) `FAIL` naming command 426 (op 7,
`SCREEN_PRIMS`), payload byte 72; in the header (cycle 401, byte 20) `FAIL` naming the arena size; in a command (cycle
402, byte 100) `FAIL` naming command 2's field `a`. Each run: 799 of 800 equal, exit 1.

### 20.3 Screenshots (check 2)

Two kinds, saved in each build's `frame-dumps/live/`:

- **In the live-check sessions** (exact mode: the emulated GS is the software rasteriser, the reference): `native_<c>`
  and `gs_<c>` of the same present at 2-3 cycles each, levels 5, 17, 18, the front end (title menu, race) and the Track
  Editor, both versions (`<build>/frame-dumps/live/<session>-shots/`): **24 pairs, 3,809,280 pixels, every pixel equal**
  in R, G, B (`tools/shot_compare.py`); with the new-scene and the two longer Europe level 5 sessions (20.7, below) 193
  pairs, all equal.
- **In normal play** (`tools/play.sh` settings with `PS2X_GS_HW_SCALE=1`, so the emulated picture is the hardware GS
  renderer's 640 x lines), 2-3 shots a scene, levels 5, 17, 18, 21, the front end and the Track Editor
  (`<build>/frame-dumps/live/perf/<version>-<scene>-shots/`): 28 pairs, 88.9% (USA) and 89.5% (Europe) of the pixels
  differ, mean channel difference 17.6 / 21.6, PSNR 17.3 / 14.4 dB. This is not a correctness measure: the two pictures
  show the same scene (see any pair) but not the same moment, since the emulated picture is the GS thread's latest and
  in a race it is a game picture or two apart from the native one (a weapon's flash shown in one and not the other gives
  the largest differences, up to a mean of 168); and the hardware renderer rasterises and filters in its own way (the
  static Track Editor: 61-94% of pixels differ, by 2-4 levels on average). What decides correctness is the exact-mode
  pairs above and the chained replay.
  One normal-play pair looked suspicious: Europe level 5 cycle 1400 of one run, an under-water moment (bubbles over the
  picture), where the native picture is washed out bright and the hardware renderer's is dark and blocky (mean
  difference 136). The attract race is not the same from run to run, so that moment could not be recorded in exact
  mode; instead a 3,600-cycle Europe level 5 session in exact mode (`l5hunt`, `live_check.sh` with a screenshot pair every
  20 cycles) gave **150 / 150 pairs equal on every pixel**, its 3,600 live lists equal byte for byte and its chained
  replay `PASS`, and the 1,450-cycle `l5long` session the same (3 pairs, 1,450 lists). The under-water frame itself is
  not verified (20.9).

### 20.4 Frame rates and the reader's time on the EE thread (check 3; not optimised)

`RC_RENDERER=vk RC_VK_PICTURE=native` with play.sh's settings (`PS2X_GS_HW_SCALE=1` for the screenshots), RTX 2060, an
otherwise idle machine; game frames a second per 5 s (`RC_DEBUG_DUMP`, `tools/fps.py`, the samples after the load),
the reader's time from the `[vk-native]` lines (per 300-cycle window for the two parts of `playtest2`, the run's total
for the others). Targets: USA 60, Europe 50 (both reached by `gs`, 20.6).

| scene | USA fps: median (range) | USA reader a cycle (largest); a VIF1 kick | Europe fps | Europe reader a cycle (largest); a VIF1 kick |
|---|---|---|---|---|
| front end (`playtest2`'s menus, cycles 300-900 USA, 300-1,200 Europe) | 41 (40.4-41.8) | 21.3-21.8 ms; 21.0-21.6 | 30-36 (29.4-36.4) | 20.5-24.8 ms; 20.3-24.5 |
| `playtest2`'s race (from cycle 1,200 / 1,500) | 30.0 (27.4-30.0) | 27.1-29.5 ms; 26.7-29.1 | 25.0 | 32.9 ms; 32.4 |
| level 5 (`RC_BOOT_LEVEL`, 90 s) | 28.2 (21.8-32.0) | 30.9 ms (53.6); 30.6 | 25.0 (23.2-29.6) | 31.2 ms (64.7); 30.9 |
| level 17 | 24.0 (21.0-27.8) | 35.1 ms (69.2); 34.9 | 23.4 (17.8-26.8) | 34.8 ms (55.8); 34.4 |
| level 18 | 16.4 (14.8-19.6) | 54.7 ms (78.9); 54.7 | 16.6 (14.4-18.0) | 51.3 ms (83.5); 51.1 |
| level 21 | 19.8 (14.0-24.4) | 41.6 ms (102.8); 41.3 | 20.4 (17.0-24.8) | 38.6 ms (63.8); 38.3 |
| Track Editor (`tedit_walk.py 1`, 150 s) | **60.0** (60.0-60.2) | 7.0 ms (45.4); 6.5 | **50.2** (48.8-50.2) | 8.2 ms (47.4); 7.7 |

**Only the Track Editor holds the target.** Wherever a race or the front end's 3D is drawn the game runs at a third to
two thirds of full speed (16-41 fps), as decision U2 foresaw: the C++ VU1 references run on the EE thread for every K2
kick (one kick a cycle), 21-55 ms of it against a 16.7 / 20 ms field. The reader's time now also includes putting K2's
primitives into the list (the cull and the vertex records the live 2D reader skipped): `playtest2`'s race measured 25.3
ms a cycle with the 2D only (`RENDERER_2D_DESIGN.md` 20.4) and 27.1-29.5 ms now, so about 2-4 ms of a race cycle. The
main thread is not the limit: the presenter's GPU work is 0.8-1.7 ms a frame (20.5) and its CPU work (the list's build
and the cover map, 1.5-2.7 ms off-line, 18.5) runs beside the EE thread.

### 20.5 U3: K2's GPU time, from the live game's own timestamps (check 4)

The same runs (20.4); the presenter's timestamps around each VIF1 kick's draws (20.1 item 5); `nvidia-smi` sampled every
5 s beside each run. Read mode, the pixel rule.

| scene | USA K2 a VIF1 kick: mean / largest | Europe | the frame's whole list on the GPU, mean, USA / Europe | GPU clock (median of the samples; P-state) |
|---|---|---|---|---|
| front end (menus) | 0.34-0.41 / 0.43 ms | 0.35-0.44 / 0.50 ms | 0.9-1.1 ms (both) | 945 MHz USA, 1,905 Europe (P0 mostly) |
| `playtest2`'s race | 0.75-0.86 / 1.15 ms | 0.80 / 0.91 ms | 1.0-1.1 ms (both) | |
| level 5 | 0.76 / 2.86 ms | 0.80 / 2.91 ms | 0.79 / 0.84 ms | 1,905-1,920 MHz, P0 |
| level 17 | 0.75 / 2.76 ms | 0.81 / 2.89 ms | 0.79 / 0.84 ms | 1,905 MHz, P0 |
| level 18 | 0.80 / 2.84 ms | 0.85 / 3.12 ms | 0.82 / 0.88 ms | 1,755-1,905 MHz, P0 |
| **level 21** | **1.67 / 4.27 ms** | **1.72 / 4.72 ms** | 1.70 / 1.74 ms | 1,905 MHz, P0 |
| Track Editor | 0.36 / 1.78 ms | 0.41 / 0.64 ms | 1.6-1.7 ms (K1: the editor's 2D) | 765-780 MHz (P0 a third of the time) |

**Against 10.4's limit (8 ms a K2 kick on average): every scene is under it by a factor of 4.6 or more; level 21, the
heaviest, takes 1.7 ms a kick on average and 4.7 ms at most.** Live, the game keeps the GPU in P0 at 1.6-1.9 GHz in races
(the emulated GS's hardware renderer runs beside it), where the off-line replays of 18.5 left it at 300-525 MHz; 18.5's
9.05 ms for the same level at about a quarter of this clock is consistent with 1.7 ms here (the time is fill, which
scales with the clock). The Track Editor's GPU stays at a lower clock because little is drawn. The timestamps cost
nothing measurable (the barriers the list records are those without them).

**But the clock is the game's whole GPU load, not the native renderer's.** With play.sh's settings the emulated GS's
hardware renderer runs on the same GPU beside the native one, and it is what keeps the driver in P0. Run with the
emulated GS in software (`PS2X_GS_HW=0 PS2X_GS_GPU=0 PS2X_GS_THREAD=0`), so that the native renderer is the GPU's only
work, the driver keeps the RTX 2060 in P8 (300-375 MHz on 24 of 25 samples) and K2 takes **11.9 ms a kick on level 21
(largest 35.9)**, Europe level 5 4.6 ms (largest 10.7): the same lists, a fifth of the clock. At the frame rates of 20.4
(10-20 fps there) the GPU is busy about a sixth of the time, too little for the driver to clock up. So: **at the
clock the game runs at today, U3 passes by a wide margin; whether it still does once the native renderer is the only
GPU work (P2, when the emulated GS is switched off) depends on the driver raising the clock at the full frame rate,
which was not measured** (the frame rate cannot be had before P2). Nothing in the shader was changed (U3 = B: report
only). Ways to settle it then: time it at P2's frame rate, or ask the driver for its performance mode during play
(`GPUPowerMizerMode`, a desktop setting, left alone here).

### 20.6 Recording off, the oracle, `playtest2`, the validation layer (check 5)

- **Recording off:** without `RC_FRAME_RECORD` nothing of the `DLST` path runs (the reader's sink asks
  `rc::rec::drawListWanted`, false without a recording) and without `RC_VK_PICTURE=native` nothing of `render/live` is
  made. `RC_SCENE_LOG=... timeout 70 tools/play.sh replay playtest2`, idle machine: **USA race scene 3020 at frame 658,
  60.0 fps**, with the default renderer and with `RC_RENDERER=vk`; **Europe at frame 1167, 50.0-50.2 fps**, both: as
  before.
- **The oracle:** `rc_oracle` links nothing of `render/`, and `ninja -C build-oracle rc_oracle` / `build-oracle-eu` had no
  work. `tools/oracle.sh --all --seed 6161` (USA): **1,632 ok, 0 FAIL** (470 s, four jobs); `RC_VERSION=eu tools/oracle.sh
  --all --seed 6162`: **1,563 ok, 0 FAIL** (877 s).
- **Validation:** `RC_RENDERER=vk RC_VK_PICTURE=native RC_VK_VALIDATION=1 tools/play.sh replay playtest2` (the Khronos
  layer with synchronisation validation, 75 s through the front end into the race, the K2 timestamps on): USA 1,752
  lists drawn, Europe 1,883, **0 validation errors** in both. The chained replay with the layer and synchronisation
  validation (`--validation`, `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true`) on the USA `l17` session and the Europe memory
  card cycles: `PASS`, 0 errors.
- The game relinked in both trees (it links `rc_live` and the capture); `rc_replay`, `rc_gs2d_test` and `rc_record_test`
  build; `rc_record_test` passes (the reader's new `chunk` call changes nothing for a dump without `DLST`).

### 20.7 New scenes (section 12; check 6)

Recorded in exact mode through `tools/live_check.sh` (so from boot, with the native renderer live and `DLST`; every 30th
reference picture stored), then the live check (`dlst`, `vk --verify`) on the whole dump and every earlier step's check on
the new scene's cycles (`--cycles a-b`): `emu --verify` (A2: GIF output, register files, picture hashes, stored pictures,
GS memory at keyframes), `tex --verify`, `blit --verify`, `gif2d --verify` (a), `vif --verify`, `gif2d --vk --vk-blend
read --verify` ((b) beside the machine, (c) K1 and (d) K2 per kick, every pixel in R, G, B, A, Z) and the chained replay.

| scene (how reached) | version | cycles (new scene's) | `dlst` | `vk` | emu, tex, blit, gif2d, vif, gif2d --vk |
|---|---|---|---|---|---|
| **the Track Editor's race** (`tedit_walk.py 1 9000` with walk 2's script: Random Track, Play Track, Single Race, the race on the built track) | USA | 5,800 (4,300-5,799) | 5,800 / 5,800 equal | `PASS` | all `PASS`: (c) 3,200 K1 kicks, (d) 1,600 K2 kicks, 786 M pixels equal |
| | Europe | 5,800 (4,300-5,799) | 5,800 / 5,800 equal | `PASS` | all `PASS`: (c) 3,192 K1 kicks, (d) 1,596 K2 kicks, 784 M pixels equal |
| **the memory card** (walk 3's script: Save Track: the name entry, the card selection with both cards' icons, "Save successful", Load Track) | USA | 5,800 (3,900-5,799) | 5,800 / 5,800 equal | `PASS` | all `PASS`: (c) 3,800 K1 kicks, (d) 1,900 K2 kicks, 934 M pixels equal |
| | Europe | 5,800 (3,900-5,799) | 5,800 / 5,800 equal | `PASS` | all `PASS`: (c) 3,800 K1 kicks, (d) 1,900 K2 kicks, 934 M pixels equal |

Screenshots of these sessions (4 a session) equal the software GS's picture of the same present on every pixel (16
pairs). The memory card screens are the heaviest 2D yet: up to 171 barriers in one K1 kick (the 67 dumps had 97 at most,
18.6) and 172 in one cycle (126 before), all still correct; the Track Editor's race draws K2 with the editor's own track
renderer (`TEditRenderGameTrack`) and 21 barriers a K2 kick.

**Not reached:**

- **The podium and the results screens.** They need a race finished by the player. No pad recording does it: the only
  one with input after a level load (`playtest-1262`, epoch 1) drifts from what it was recorded against (replayed at
  normal speed: 8th place, still on lap 1 at 5:09, field 23,400), the older ones lose their driving at the load (NOTES
  "Known, not new"), `front_walk.py` and `tedit_walk.py` do not drive, and the Track Editor's race runs on with the
  player on the grid. A new recording made by hand (`tools/play.sh record`) of a won race would reach both.
- **More two-player.** The pad replay drives port 0 only (`ps2_host_input.cpp`), so the Track Editor's Multiplayer Race
  stops at "Player 2, select a vehicle" (reached and seen in a normal run), and `RC_BOOT_LEVEL`'s demo is set up "not
  split screen" (`overrides/rc_revenge_pro_overrides.cpp`). A normal boot left in the front end for 6 minutes ran four
  attract demos (GS-survey scenes 0x4020, 0x4090, 0x4100, 0x4160: levels 2, 9, 16, 22), none split. The two-player races
  already in the 67 dumps (3.1) stay the only ones. Reaching more needs a second pad in the
  replay (a runtime change) or a split-screen switch for `RC_BOOT_LEVEL` (an override change, which the oracle links);
  neither was made in this batch.

### 20.8 The default

Section 12 makes `RC_VK_PICTURE=native` the default under `RC_RENDERER=vk` once batch 4 passes every check. Every
correctness check passes (20.2, 20.3, 20.6, 20.7) and U3 is answered (20.5), but **check 3, the frame rate, does not:
16-41 fps wherever K2 is drawn against 60 / 50** (20.4). The default therefore stays `gs` (unset `RC_VK_PICTURE` shows the
emulated GS picture, exactly as before); `RC_VK_PICTURE=native` is the opt-in and F7 toggles in a native session. It
becomes the default when P2 brings the references off the EE thread (step 9) and the frame rate holds.

### 20.9 Open

- **The frame rate (U2), P2's:** the C++ VU1 references on the EE thread are 21-55 ms a race or front-end cycle; putting
  K2 in the list added about 2-4 ms of it. Nothing was optimised.
- **U3 at P2's clock** (20.5): K2 is 1.7 ms a kick on level 21 with the GPU in P0 as the game runs today, but 11.9 ms
  when the native renderer is the GPU's only work and the driver stays in P8. Time it again when the frame rate allows
  (P2), or hold the clock; if it is still over 8 ms, what may change in the shader without losing equality is the
  question 18.7 left (fewer shaded fragments: depth before shading where the alpha test allows, front-to-back opaque).
- **Scenes not reached** (20.7): the podium and the results screens (need a hand-made recording of a won race), more
  two-player (needs a second pad in the pad replay or a split-screen switch for `RC_BOOT_LEVEL`).
- The live check needs a dump from boot (20.1 item 2); a ring dump's `DLST` cannot be checked. If that is wanted (glitch
  hunting with F10), the reader would have to number textures from the dump's start, i.e. restart its texture ids at
  each keyframe live, which changes the live lists.
- The heaviest 2D is now the memory card screens (171 barriers in one K1 kick): correct, and 7.1 ms of GPU time for that
  kick off-line at the low clock; not timed live.
- One normal-play screenshot (Europe level 5, an under-water moment with bubbles) shows a much brighter native picture
  than the hardware renderer's; the moment did not recur in the exact-mode sessions (20.3), so it is not verified
  either way. A ring dump in exact mode (`RC_FRAME_RECORD_AT=ring:300`, F10 when it happens) would settle it.
- After a save-state load the live reader reseeds from the loaded machine and every GS register is unknown until the
  game writes it (20.6 of the 2D design); no live session here loaded a state.
- The originals under `frame-dumps/pre-r/` (6.5 GB) can go: step 8's checks are done.

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

**Decided by the user (2026-11-20): A.** Batch R (section 13) runs next, before batch 2.

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
   4 ms on average. Batch 2 measures it; reducing it is P2's (section 10.4).

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

- **U1 and batch R** (not built, as instructed): until the runtime's VU1 cycle limit goes, the 1,054 kicks with a cut
  water run fail (b) without the machine, and beside it the reader is given the machine's state after the cut.
- The 72 poisoned lanes: the references could be made to keep them (5.2's second half); nothing needs them now.
- `FrameBlit` joins `FrameReader` with batch 3.
- Step 7's batch 5 (live 2D) can now take its PATH1 registers from `FrameReader` (FBA_1 is the only register no
  path writes; the keyframe gives 0, the default).
- The reader's time (16.3) is the references' as 3.10 predicted; nothing in batch 1 reduces it (U2).

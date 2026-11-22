#pragma once

// The VIF1 chain reader (renderer plan P1 step 8; RENDERER_3D_DESIGN.md sections 4-6). A channel 1 kick is walked with
// the host's chain walker (render/record/chain_walk) and its VIF codes decoded into the reader's own VIF1 registers,
// VU data memory and micro memory; at each MSCAL the C++ reference of the loaded program (decomp/vu1, the Io entry) is
// run on that memory and the 32 vector registers, under the kick's MXCSR with rounding toward zero as the runtime runs
// the VU1 (the lean build, Options::library, adds flush to zero and denormals are zero itself), and its XGKICK packets
// go to a Path1Sink. The state persists across kicks and cycles; a replay seeds it
// from a keyframe's VIF1, VU1M and VU1R, the live reader starts from reset with the game.
//
// Eight VIF codes are implemented (NOP, STCYCL, OFFSET, BASE, MPG, MSCAL, FLUSHA and UNPACK V4-32 with CL = WL, mode
// 0, no mask; ITOP, STMOD, STMASK, STROW, STCOL, FLUSH, FLUSHE and MARK set their register or do nothing, as the
// runtime). Anything else stops the kick with a named guard: the packets made so far stand, the caller is told.
// No runtime header is included: guest RAM is read through RamView only.

#include "../record/consumer.h"
#include "../shape/fast_run.h"
#include "vu1_programs.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rc::rd
{
    // The VIF1 registers the reader keeps (the fields of rc::rec::Vif1Regs it reads and writes).
    struct VifRegs
    {
        uint32_t cycle = 0, mode = 0, mask = 0, base = 0, ofst = 0, tops = 0, itops = 0, top = 0, itop = 0;
        bool dbf = false;
        uint32_t row[4] = {}, col[4] = {};
    };

    // One run of a VU1 program (one MSCAL).
    struct Vu1Run
    {
        int program = -1;           // index into the program table (vu1_programs.h)
        uint32_t pc = 0, top = 0, itop = 0;
        uint32_t index = 0;         // MSCAL number in the kick
        uint32_t streamOffset = 0;  // of the MSCAL code
    };

    struct Path1Sink
    {
        virtual void runBegin(const Vu1Run &) {}
        virtual void packet(const uint8_t *data, uint32_t bytes) = 0; // one XGKICK
        // (P2 step 9 batch 3) A fast path's triangle packet (160 bytes, its tag the triangle tag with EOP) as its nine
        // data quadwords. False: not taken; the packet comes through packet() instead.
        virtual bool triangle(const uint32_t * /*nine*/) { return false; }
        // The state after the run. A checker beside the emulated machine may replace it (a run the machine cut short).
        virtual void runEnd(const Vu1Run &, uint8_t * /*vuData*/, float (*/*vf*/)[4]) {}
        virtual ~Path1Sink() = default;
    };

    enum VifGuard : uint8_t
    {
        kVifGuardNone = 0,
        kVifGuardCode,      // VIF_CODE: a code the reader does not implement
        kVifGuardUnpack,    // VIF_UNPACK: an UNPACK other than V4-32, CL = WL, mode 0, unmasked
        kVifGuardTruncated, // VIF_TRUNCATED: a code or its data cut by the end of the stream, or bytes left over
        kVifGuardPage,      // VIF_PAGE: a range on a page the RAM view does not hold (off-line only)
        kVifGuardProgram,   // VU_PROGRAM: micro memory holds no program the table knows
        kVifGuardEntry,     // VU_ENTRY: an MSCAL to an address that is not an entry of the program
        kVifGuardRunaway,   // VU_RUNAWAY: a run sent more packets than any program can (kMaxRunPackets), or a packet
                            // of 64 K quadwords (copyGifPacket's cut): garbage input
        kVifGuardCount
    };
    const char *vifGuardName(uint32_t guard);

    // Planted faults (RENDERER_3D_DESIGN.md 11.6), each from `cycle` on (n: which one in that cycle, where it applies).
    enum class VifFault : uint8_t
    {
        None,
        UnpackDest,  // every UNPACK of the cycle written one quadword further on
        TopsSwap,    // MSCAL leaves TOPS as it was (no double buffer) in the cycle
        OffsetDbf,   // OFFSET does not clear DBF in the cycle
        DropMpg,     // the n-th MPG of the cycle is not copied into micro memory
        SkipMscal,   // the n-th MSCAL of the cycle does not run its program
        NoMxcsr,     // the references run under the caller's rounding mode in the cycle
        Lane,        // VF25.w (n = register * 4 + lane, default VF25.w) is not carried between runs in the cycle: zeroed before each
        Packet,      // (applied by FrameReader) the n-th PATH1 packet of the cycle that writes A+D state is lost
        VuMemory,    // VU data memory is not carried across kicks in the cycle: zeroed at each kick's start
    };
    bool parseVifFault(const std::string &name, VifFault &out);
    const char *vifFaultName(VifFault f);

    // Which build of the references runs at an MSCAL (RENDERER_P2_DESIGN.md 4.1): strict (rc::vu1, every result
    // checked in double precision: the judge) or lean (rc::vu1l, the host's operations under RZ + FTZ + DAZ: the same
    // packets, VU memory and registers in about half the time).
    enum class Vu1Library : uint8_t
    {
        Strict,
        Lean,
    };
    bool parseVu1Library(const std::string &name, Vu1Library &out);
    const char *vu1LibraryName(Vu1Library l);

    class Vif1Reader
    {
    public:
        struct Options
        {
            Vu1Library library = Vu1Library::Strict;
            // (P2 step 9 batch 2, the fast reader) An MSCAL to a direct entry (TRdr 0x2050, Env 0x2370, TERdr 0x2198:
            // the program sends the packet at TOP unchanged and leaves every register as it was) copies that packet to
            // the sink without running a reference (RENDERER_P2_DESIGN.md 4.2's table).
            bool directEntries = false;
            // (P2 step 9 batches 3 and 4, RENDERER_P2_DESIGN.md 4.2) With the lean library: an MSCAL to a program with a
            // fast path (render/shape: TRdr entry 0, Env entries 0 and 0x78, the skin program, TERdr entry 0) runs the path
            // instead of the reference, unless the path declines the run (4.3); every other run is the lean reference's,
            // counted as declined.
            bool fastPaths = false;
            rc::shape::FastFault fastFault = rc::shape::FastFault::None; // planted, from fastFaultCycle on
            uint8_t fastFaultPrograms = 0xFF;                           // ... in these programs' paths (bit per rc::vu1::Program)
            int64_t fastFaultCycle = 0;
        };
        // More XGKICKs than any run can send (the largest kick of the 67 dumps sends 63,013): the run is abandoned.
        static constexpr uint32_t kMaxRunPackets = 1u << 20;
        struct Stats
        {
            uint64_t kicks = 0, codes = 0, codesByOp[128] = {}, unpackBytes = 0, unpacks = 0, mpgs = 0, mpgBytes = 0;
            uint64_t mscals = 0, runs[kVu1ProgramCount] = {}, packets = 0, packetBytes = 0, interruptBits = 0;
            uint64_t guards[kVifGuardCount] = {}, programChanges = 0, fbrstResets = 0, seeds = 0;
            uint64_t directRuns = 0; // runs of a direct entry answered by Options::directEntries
            rc::shape::FastStats fast; // Options::fastPaths
            uint64_t refNs = 0, walkNs = 0, sinkNs = 0; // in TSC ticks (nsPerTick): the references; the walk and decode; the sink's time
            uint64_t refNsByProgram[kVu1ProgramCount] = {}; // refNs by the program run (the table's index), fast paths included
        };

        Vif1Reader();
        explicit Vif1Reader(const Options &options);
        void setOptions(const Options &options) { m_options = options; }
        const Options &options() const { return m_options; }

        void reset();                                  // the machine's reset state
        bool seed(const rc::rec::Keyframe &kf);        // VIF1, VU1M, VU1R; false if one is missing
        void hw(uint32_t address, uint64_t value);     // VIF1 register stores (FBRST bit 0 resets the registers)
        // Walks and decodes one channel 1 kick. `mxcsr` is the EE thread's MXCSR at the kick (KickView::mxcsr off-line,
        // _mm_getcsr() live); the references run under it with 0x6000 set. False: a guard stopped the kick.
        bool kick(const rc::rec::KickView &k, const rc::rec::RamView &ram, uint32_t mxcsr, Path1Sink &sink);

        const VifRegs &regs() const { return m_vif; }
        const uint8_t *vuData() const { return m_data.data(); }
        const uint8_t *vuMicro() const { return m_micro.data(); }
        const float (*vf() const)[4] { return m_vf; }
        uint8_t *vuDataMutable() { return m_data.data(); }
        float (*vfMutable())[4] { return m_vf; }
        const Stats &stats() const { return m_stats; }
        double nsPerTick() const; // for the times in Stats

        // The last guard: which, where, and a line for the log.
        uint32_t lastGuard() const { return m_guard; }
        const std::string &lastGuardText() const { return m_guardText; }

        void setFault(VifFault f, int64_t cycle, uint32_t n) { m_fault = f; m_faultCycle = cycle; m_faultN = n; }
        // Off: the lanes a reference does not reproduce keep the reference's values (to measure them; vu1_programs.h).
        void setPoison(bool on) { m_poison = on; }

    private:
        struct Decoder;
        bool guard(uint32_t g, const rc::rec::KickView &k, uint32_t offset, uint32_t code, const char *what);
        bool runProgram(const rc::rec::KickView &k, uint32_t pc, uint32_t offset, uint32_t mxcsr, Path1Sink &sink);

        Options m_options;
        VifRegs m_vif;
        std::vector<uint8_t> m_data, m_micro; // 16 KB each
        alignas(16) float m_vf[32][4] = {};
        std::vector<uint8_t> m_direct; // Options::directEntries: the packet copied from VU memory
        bool m_poison = true;
        int m_program = -1;
        bool m_programDirty = true;
        uint64_t m_wholeHash = 0;
        uint32_t m_mscalInKick = 0;
        uint32_t m_guard = kVifGuardNone;
        std::string m_guardText;
        Stats m_stats;
        VifFault m_fault = VifFault::None;
        int64_t m_faultCycle = -1;
        uint32_t m_faultN = 0, m_faultMpg = 0, m_faultMscal = 0;
        uint64_t m_faultCounterCycle = ~0ull;
        uint64_t m_calNs0 = 0, m_calTicks0 = 0, m_calNs1 = 0, m_calTicks1 = 0;
    };
}

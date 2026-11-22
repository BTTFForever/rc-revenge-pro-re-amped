#pragma once

// The fast paths of the fast reader (renderer plan P2 step 9; RENDERER_P2_DESIGN.md 4.2, 4.3). A fast path is a VU1
// program's common path transcribed into SSE, one VU vector operation to one SSE operation in the program's order,
// under MXCSR round toward zero + flush to zero + denormals are zero (the lean build's mode, decomp/vu1/vu_lean.h), with
// the operand classes that mode cannot treat as the VU does detected and the triangle handed to the reference's own
// per-triangle step (lean build) instead. Its output is the reference's: the same packets, or for a triangle packet the
// same nine data quadwords handed straight to the GIF reader (FastSink::triangle). Proven by check (e): the fast
// reader's lists against the strict reader's (rc_replay --consumer lists).
//
// Batch 3: TRdr_VUShapeProg entry 0 (track cells, cars and scenery without the env map, RDRPOLY's and RDRALPHA's
// buffers). Batch 4: EnvRdr_VUShapeProg entries 0 and 0x78 (the shiny pass, with and without the environment map),
// Rdr_VUSkinProg, TERdr_VUShapeProg entry 0. Every other run declines (the caller runs the lean reference for the whole
// run, as before): water by design, Rdr_VUShapeProg (never met), the direct entries (the reader copies their packet).
//
// No runtime header, no GIF reader: what the path makes goes to a FastSink (the VIF1 reader's adapter of its Path1Sink).

#include <cstdint>

namespace rc::shape
{
    // Where a fast run sends what it makes, in the program's order.
    struct FastSink
    {
        // One XGKICK exactly as the program would send it (the header, texture and register blocks, the flush packets,
        // everything a hard triangle's step sends, and a triangle the sink declined, rebuilt).
        virtual void packet(const uint8_t *data, uint32_t bytes) = 0;
        // A triangle packet whose GIF tag in VU memory is the triangle tag with EOP (one PACKED tag, NLOOP 3, NREG 3,
        // ST RGBAQ XYZ2: 160 bytes): its nine data quadwords (36 words). False: nothing was done, and the path sends
        // the 160-byte packet through packet() instead.
        virtual bool triangle(const uint32_t *nine) = 0;

    protected:
        ~FastSink() = default;
    };

    // The programs, as rc::vu1::Program numbers them (vu1_ref.h): kTrdr 0, kEnv 1, kWater 2, kSkin 3, kTerdr 4, kRdr 5.
    constexpr uint32_t kFastProgramSlots = 8;

    // Planted faults in the fast paths (check (e) must fail with each: RENDERER_P2_DESIGN.md 9). Each applies to the
    // paths named by FastOptions::faultPrograms (all by default) where the path has the step it changes.
    enum class FastFault : uint8_t
    {
        None,
        Bias,      // the depth bias left out (TRdr, Env: the corners' z; TERdr: the quick path's z; skin has none)
        Fused,     // one MADD of the corner transform (the z row's) fused: the product not rounded before the sum
        RoundX,    // the conversion of X to 1/16 pixel rounds to nearest instead of truncating
        EdgeTwo,   // "beyond one edge" decided on the first two corners only
        HardQuick, // a triangle with one or two corners behind the near plane drawn by the quick path
        LastStep,  // (batch 4) an env-mapped run's (or a skin run's) last triangle not handed to the reference's step:
                   // VF26-VF28 (VF25.w, VF26, VF29 for skin) not left as the program leaves them
        LitRegs,   // (batch 4) TERdr: the light registers VF24-VF30 not reloaded after a run whose last lit triangle was fast
        Count
    };
    // `name`, or `name@program` (program trdr, env, skin, terdr) to plant it in that path only.
    bool parseFastFault(const char *name, FastFault &out, uint8_t *programs = nullptr);
    const char *fastFaultName(FastFault f);

    // What a path did, per program.
    struct FastCounts
    {
        uint64_t runs = 0;          // runs taken by the fast path
        uint64_t declined = 0;      // runs left to the lean reference (no path for the program or entry, or declined)
        uint64_t declinedParse = 0; // ... of them: runs whose input did not parse within the buffer (4.3)
        uint64_t in = 0, culled = 0, allBehind = 0, edge = 0, quick = 0;
        uint64_t hard = 0;          // triangles handed to the reference's step (hard or odd)
        uint64_t odd = 0;           // ... of them for a value the mode cannot treat as the VU does (4.1's third column)
        uint64_t lastSteps = 0;     // a run's last triangle handed to the step for the registers it leaves (env-mapped, skin)
        uint64_t direct = 0;        // quick triangles taken by FastSink::triangle
        uint64_t rebuilt = 0;       // quick triangles sent as a packet (the sink declined, or the slot's tag is not the triangle tag)
        uint64_t blocks = 0;        // texture blocks
        uint64_t envRuns = 0;       // Env: runs with the environment map on (constant 10.z)

        void add(const FastCounts &o)
        {
            runs += o.runs, declined += o.declined, declinedParse += o.declinedParse, in += o.in, culled += o.culled, allBehind += o.allBehind;
            edge += o.edge, quick += o.quick, hard += o.hard, odd += o.odd, lastSteps += o.lastSteps, direct += o.direct, rebuilt += o.rebuilt;
            blocks += o.blocks, envRuns += o.envRuns;
        }
    };

    struct FastStats
    {
        FastCounts by[kFastProgramSlots];  // by the reader's program index (vu1_ref.h's Program)
        uint32_t maxExtent = 0;            // the largest input a run walked, quadwords from where its input starts

        FastCounts total() const
        {
            FastCounts t;
            for (const FastCounts &c : by)
                t.add(c);
            return t;
        }
    };

    struct FastOptions
    {
        FastFault fault = FastFault::None;
        uint8_t faultPrograms = 0xFF; // bit per program: where the fault is planted
    };

    // Whether `program` (rc::vu1::Program) has a fast path at entry `pc` (a byte address).
    bool hasFastPath(uint32_t program, uint32_t pc);

    // The program's run at `pc` on the reader's VU data memory and registers, as the reference would run it (the lean
    // build's step for hard triangles), its packets to `sink`. Sets the lean mode itself (on top of the caller's MXCSR,
    // which must round toward zero) and restores it. VU memory is left as the program leaves it but in its work areas
    // (the packet buffer, the clip lists, the link slot: D-P2-2); the registers as the program leaves them in every lane
    // the program's later runs can read, and in the lanes fastNotReproduced names something the caller poisons. False,
    // with nothing done: the run is declined (no path, or its input does not parse within the buffer, 4.3), and the
    // caller runs the reference.
    bool runFast(uint32_t program, uint32_t pc, uint8_t *vuData, float (*vf)[4], uint32_t top, FastSink &sink, FastStats &stats,
                 const FastOptions &options);

    // The register lanes (bit per lane x, y, z, w) a fast run of `program` does not leave as the program does, beyond
    // those every reference leaves unreproduced (rc::rd::Vu1ProgramInfo::notReproduced, poisoned after every run anyway).
    // The caller poisons them after the run.
    const uint8_t *fastNotReproduced(uint32_t program);

    // TRdr, Env, TERdr: the last triangle's normal and colours (VF17.w, VF18-VF20) and the quick path's screen positions
    // (VF05-VF07 xyz), which the program writes for every triangle it reads (Env: for every quick one; an env-mapped
    // run's last triangle is the reference's step, which leaves VF17-VF20 right, but not VF05-VF07 xyz).
    constexpr uint8_t kTrdrFastNotReproduced[32] = {0, 0, 0, 0, 0, 0x7, 0x7, 0x7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x8, 0xF, 0xF, 0xF};
    // Skin: none. The program writes every register it writes for every triangle it reads (VF01-VF04, VF11-VF20,
    // VF24-VF30), and the run's last triangle is the reference's step.
    constexpr uint8_t kSkinFastNotReproduced[32] = {};

    // The input a run may walk from TOP before it is declined (quadwords; 4.3): the input buffer of the programs' double
    // buffer. The largest input in the dumps is 383 quadwords (s5/l17). The skin program reads from quadword 512 (its
    // bones are 0-511), up to the end of VU memory.
    constexpr uint32_t kMaxRunInput = 0x180u;
    constexpr uint32_t kSkinInput = 512u, kMaxSkinInput = 512u;

    // The paths (fast_trdr.cpp, fast_env.cpp, fast_terdr.cpp, fast_skin.cpp); runFast dispatches.
    bool runTrdr(uint8_t *vuData, float (*vf)[4], uint32_t top, FastSink &sink, FastStats &stats, const FastOptions &options);
    bool runEnv(uint8_t *vuData, float (*vf)[4], uint32_t pc, uint32_t top, FastSink &sink, FastStats &stats, const FastOptions &options);
    bool runTerdr(uint8_t *vuData, float (*vf)[4], uint32_t top, FastSink &sink, FastStats &stats, const FastOptions &options);
    bool runSkin(uint8_t *vuData, float (*vf)[4], FastSink &sink, FastStats &stats, const FastOptions &options);
}

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
// buffers). Every other run declines (the caller runs the lean reference for the whole run, as before).
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

    // Planted faults in the fast paths (check (e) must fail with each: RENDERER_P2_DESIGN.md 9).
    enum class FastFault : uint8_t
    {
        None,
        Bias,      // the depth bias left out of the corners' z
        Fused,     // one MADD of the corner transform (the z row's) fused: the product not rounded before the sum
        RoundX,    // the conversion of X to 1/16 pixel rounds to nearest instead of truncating
        EdgeTwo,   // "beyond one edge" decided on the first two corners only
        HardQuick, // a triangle with one or two corners behind the near plane drawn by the quick path
        Count
    };
    bool parseFastFault(const char *name, FastFault &out);
    const char *fastFaultName(FastFault f);

    struct FastStats
    {
        uint64_t runs = 0;              // runs taken by a fast path
        uint64_t declined[8] = {};      // runs left to the lean reference, by the reader's program index (vu1_ref.h's Program)
        uint64_t declinedParse = 0;     // ... of them: TRdr runs whose input did not parse within the buffer (4.3)
        uint64_t in = 0, culled = 0, allBehind = 0, edge = 0, quick = 0;
        uint64_t hard = 0;              // triangles handed to the reference's step
        uint64_t odd = 0;               // ... of them for a value the mode cannot treat as the VU does (4.1's third column)
        uint64_t direct = 0;            // quick triangles taken by FastSink::triangle
        uint64_t rebuilt = 0;           // quick triangles sent as a packet (the sink declined, or the slot's tag is not the triangle tag)
        uint64_t blocks = 0;            // texture blocks
        uint32_t maxExtent = 0;         // the largest input a run walked, quadwords from TOP
    };

    struct FastOptions
    {
        FastFault fault = FastFault::None;
    };

    // TRdr_VUShapeProg's entry 0 on the reader's VU data memory and registers, as the reference would run it (the
    // lean build's step for hard triangles), its packets to `sink`. Sets the lean mode itself (on top of the caller's
    // MXCSR, which must round toward zero) and restores it. VU memory is left as the program leaves it but in its work
    // areas (the packet buffer, the clip lists, the link slot: D-P2-2); the registers as the program leaves them in
    // every lane the program's later runs can read, and poisoned (rc::rd's signalling pattern) in the lanes this path
    // does not reproduce. False, with nothing done: the run is declined (its input does not parse within the buffer,
    // 4.3), and the caller runs the reference.
    bool runTrdr(uint8_t *vuData, float (*vf)[4], uint32_t top, FastSink &sink, FastStats &stats, const FastOptions &options);

    // The register lanes (bit per lane x, y, z, w) a fast TRdr run does not leave as the program does, beyond those every
    // reference leaves unreproduced (rc::rd::Vu1ProgramInfo::notReproduced, poisoned after every run anyway): the
    // last triangle's normal and colours (VF17.w, VF18-VF20) and the quick path's screen positions (VF05-VF07 xyz),
    // which the program writes for every triangle it reads. The caller poisons them after the run.
    constexpr uint8_t kTrdrFastNotReproduced[32] = {0, 0, 0, 0, 0, 0x7, 0x7, 0x7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x8, 0xF, 0xF, 0xF};

    // The input a run may walk from TOP before it is declined (quadwords; 4.3): the input buffer of the programs' double
    // buffer. The largest input in the dumps is 383 quadwords (s5/l17).
    constexpr uint32_t kMaxRunInput = 0x180u;
}

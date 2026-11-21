#pragma once

// rc_replay's shapestat consumer: the lean build of the VU1 references and the per-run classification
// (shape_lean.cpp). Measurement for RENDERER_P2_DESIGN.md; nothing here is the renderer.

#include <cstdint>
#include <vector>

namespace rc::replay::lean
{
    // What one run's input triangles do in the program (entry 0 of a shape program).
    struct RunClass
    {
        bool direct = false;     // a direct entry (the packet at TOP): no triangles
        uint32_t blocks = 0;     // texture blocks (TRdr / TERdr: not counted, 1)
        uint32_t in = 0;         // triangles read
        uint32_t culled = 0;     // rejected by the facing test
        uint32_t quick = 0;      // all three corners in front, no guard-band flag: one packet
        uint32_t allBehind = 0;  // all three behind the near plane: nothing
        uint32_t nearCut = 0;    // one or two corners behind: the near-plane paths
        uint32_t guardNone = 0;  // a guard-band flag, nothing drawn (beyond one edge, or clipped away)
        uint32_t guardEarly = 0; // ... of them: all three corners beyond one edge (the scissor routine's first test)
        uint32_t guardFan = 0;   // a guard-band flag, drawn as a fan
        uint32_t guardFanTriangles = 0;
        bool plain = false;      // every triangle culled or quick
        bool garbage = false;    // the walk did not end within VU memory's capacity
        bool boxTested = false, boxPlain = false; // the bounding-box test says the run is plain (not skin)
    };

    // The MXCSR a lean run needs, from the kick's.
    uint32_t csr(uint32_t kickCsr);
    // Runs the lean build of a program (vu1_ref.h's order) in place; the packets are appended. False: no such program.
    bool run(int program, uint8_t *memory, float (*vf)[4], uint32_t startPc, uint32_t top, std::vector<uint8_t> &bytes, std::vector<uint32_t> &sizes);
    void classify(int program, uint8_t *memory, const float (*vf)[4], uint32_t startPc, uint32_t top, RunClass &out);
    uint64_t selfTest(uint64_t cases, uint64_t *done);

    // The track program's common path written with SSE (one VU vector operation, one SSE operation), for its time
    // and its bytes: culled, all-behind and beyond-one-edge triangles give nothing, a quick triangle gives the nine
    // quadwords the program stores (144 bytes appended to `out`), anything else is counted as hard (the near-plane
    // and guard-band clippers, or a value the host mode cannot treat as the VU does) and left out.
    struct FastCount
    {
        uint32_t in = 0, culled = 0, quick = 0, allBehind = 0, guardEarly = 0, hard = 0, odd = 0, blocks = 0;
    };
    void fastTrdr(const uint8_t *memory, const float (*vf)[4], uint32_t top, std::vector<uint8_t> &out, FastCount &count);
}

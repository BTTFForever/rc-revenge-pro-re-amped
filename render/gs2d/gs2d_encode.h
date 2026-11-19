#pragma once

// The draw list encoded back into GIF packets (renderer plan P1 step 7; RENDERER_2D_DESIGN.md 10.2, check (b)): every
// GsState field as an A+D register write, each primitive as PRIM (the state's, type = the primitive's class) then
// RGBAQ, ST, UV and XYZ2 (XYZF2 where the vertex came so) per vertex, the clear as its sprite. Fed to the emulated GS in
// place of the kick it came from, it must give the same pictures and GS memory: that proves the parse, the primitive
// assembly, and that GsState holds everything a draw depends on.
//
// The GS register file is restored at the end: the registers whose value the encoded writes leave different from the
// original stream's (the reader's file after the kick) are written back, so the emulated machine's register file before
// the next kick, which `emu --verify` checks, is the recorded one. Vertex registers are written back under PRIM type 7
// (the reference ignores vertices of the reserved type), then PRIM and PRMODECONT last; TRXDIR, HWREG, SIGNAL, FINISH
// and LABEL have side effects and are only counted when they differ. No dependencies but gs_state.h, C++17.

#include "gs_state.h"

#include <cstdint>
#include <vector>

namespace rc::rd
{
    // GsState fields by index, for planted faults and reports.
    enum GsField : int
    {
        kFieldFrame, kFieldZbuf, kFieldScissor, kFieldXyoffset, kFieldTest, kFieldAlpha, kFieldTex0, kFieldTex1, kFieldClamp,
        kFieldMiptbp1, kFieldMiptbp2, kFieldTexa, kFieldPrim, kFieldPabe, kFieldFba, kFieldColclamp, kFieldDthe, kFieldCount
    };
    const char *gsFieldName(int field);

    struct GsEncodeFault
    {
        int zeroField = -1;      // GsField written as 0 in every STATE
        int64_t dropPrim = -1;   // the n-th primitive (clears included) of the encoded range left out
    };

    struct GsEncodeResult
    {
        std::vector<uint8_t> gif;   // PACKED A+D tags, the last with EOP
        uint32_t writes = 0, prims = 0, states = 0;
        uint32_t restored = 0;      // registers written back at the end
        uint32_t unrestorable = 0;  // registers with side effects left different
        uint8_t firstUnrestorable = 0;
    };

    // Encodes list commands [first, last). `before`: the GS register file before the kick (0x63 values); `after`: the
    // file the original stream left, `afterKnown[r]` whether that value is known (only known ones are restored).
    GsEncodeResult encodeDrawList(const DrawList &list, uint32_t first, uint32_t last, const uint64_t *before, const uint64_t *after,
                                  const bool *afterKnown, const GsEncodeFault &fault = {});
}

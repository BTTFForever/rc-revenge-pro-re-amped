#pragma once

// The DLST chunk's payload (renderer plan P1 step 8 batch 4; RENDERER_FRAME_RECORD.md section 9, RENDERER_3D_DESIGN.md
// section 12 "The live check"): one display cycle's draw list as the live frame reader published it, so that the list
// an off-line FrameReader makes from the recorded kicks can be compared with it byte for byte.
//
//   DlstHeader (32 bytes) | DrawCmd x cmdCount (32 bytes each, as in memory) | the payload arena (payloadBytes)
//
// The arena is copied as the list holds it (every payload 8-byte aligned, the gaps zero), so two lists are equal exactly
// when their encodings are. Written by render/live/live_host.cpp (the game) through rc::rec::recordDrawList; read by
// rc_replay --consumer dlst.

#include "../gs2d/gs_state.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace rc::rd
{
    struct DlstHeader
    {
        char magic[4];        // "DLS1"
        uint32_t cmdCount;
        uint64_t cycle;       // END.cycle of the list's cycle
        uint64_t payloadBytes;
        uint32_t k2Kicks;     // VIF1 kicks the reader ran in the cycle
        uint32_t flags;       // 0
    };
    static_assert(sizeof(DlstHeader) == 32, "DlstHeader is 32 bytes");

    inline void encodeDlst(const DrawList &list, uint64_t cycle, uint32_t k2Kicks, std::vector<uint8_t> &out)
    {
        DlstHeader h{};
        std::memcpy(h.magic, "DLS1", 4);
        h.cmdCount = static_cast<uint32_t>(list.cmds.size());
        h.cycle = cycle;
        h.payloadBytes = list.payload.size();
        h.k2Kicks = k2Kicks;
        const size_t cmdBytes = list.cmds.size() * sizeof(DrawCmd);
        out.resize(sizeof(h) + cmdBytes + list.payload.size());
        std::memcpy(out.data(), &h, sizeof(h));
        if (cmdBytes)
            std::memcpy(out.data() + sizeof(h), list.cmds.data(), cmdBytes);
        if (!list.payload.empty())
            std::memcpy(out.data() + sizeof(h) + cmdBytes, list.payload.data(), list.payload.size());
    }

    // False: not a DLST payload (bad magic or sizes).
    inline bool decodeDlstHeader(const uint8_t *data, uint64_t bytes, DlstHeader &h)
    {
        if (bytes < sizeof(DlstHeader))
            return false;
        std::memcpy(&h, data, sizeof(h));
        return std::memcmp(h.magic, "DLS1", 4) == 0 &&
               bytes == sizeof(DlstHeader) + static_cast<uint64_t>(h.cmdCount) * sizeof(DrawCmd) + h.payloadBytes;
    }
}

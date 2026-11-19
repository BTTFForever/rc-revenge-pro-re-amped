#pragma once

// Keyframe state <-> the runtime (RENDERER_FRAME_RECORD.md section 3.4). Needs ps2_runtime (library rc_record_emu).
// Batch 1: the plain state chunks, taken from a running machine. Batch 2 adds EMUS and loading into a headless runtime.

#include "consumer.h"

#include <cstdint>
#include <vector>

class PS2Runtime;

namespace rc::rec
{
    // The plain state chunks of a keyframe, owned.
    struct KeyframeState
    {
        std::vector<uint8_t> gsMemory; // GSVR
        GsPrivRegs gsPriv{};           // GSPR
        uint64_t gsRegs[kGsRegCount]{};// GSRG
        std::vector<uint8_t> vu1Memory;// VU1M: micro then data memory
        Vu1Regs vu1Regs{};             // VU1R
        Vif1Regs vif1{};               // VIF1
        // Chunks pointing into this object (valid while it lives). final: GSVR, GSPR, GSRG only.
        std::vector<StateChunk> chunks(bool final) const;
    };

    // Waits for the VU1/GIF threads and the GS backend, then copies the state. False if the GS has no memory yet.
    bool captureKeyframeState(PS2Runtime &runtime, KeyframeState &out);
}

#pragma once

// Keyframe state <-> the runtime (RENDERER_FRAME_RECORD.md section 3.4). Needs ps2_runtime (library rc_record_emu).
// Batch 1: the plain state chunks, taken from a running machine. Batch 2: EMUS (the runtime's opaque graphics state,
// ps2x::savestate::saveGraphics) and loading a keyframe into a headless runtime (rc_replay's emu consumer).

#include "consumer.h"

#include <cstdint>
#include <string>
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
        std::vector<uint8_t> emu;      // EMUS (ps2x::savestate::saveGraphics; GS memory is GSVR)
        // Chunks pointing into this object (valid while it lives). final: GSVR, GSPR, GSRG only.
        std::vector<StateChunk> chunks(bool final) const;
    };

    // Waits for the VU1/GIF threads and the GS backend, then copies the state. False if the GS has no memory yet.
    bool captureKeyframeState(PS2Runtime &runtime, KeyframeState &out);

    // The EMUS layout this build writes and loads (FileHeader.emuStateVersion).
    uint32_t emuStateVersion();

    // Loads a keyframe's EMUS and GSVR into a runtime (the emu consumer's headless machine). False with `error` set
    // when the keyframe lacks them or the blob does not load.
    bool loadKeyframeState(PS2Runtime &runtime, const Keyframe &kf, std::string &error);
}

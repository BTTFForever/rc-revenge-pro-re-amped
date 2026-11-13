// The RAM dump the game writes with RC_RAMDUMP (overrides/rc_revenge_pro_overrides.cpp) and the offline oracle
// (rc_oracle.cpp) reads: the machine state a call of a game function needs, taken at the start of a frame's
// GfxP2_SwapBuffers call on the main game thread.
//
// Layout: RamDumpHeader, then the R5900Context of the calling thread (contextSize bytes, as this build lays it out),
// main RAM (PS2_RAM_SIZE), the scratchpad (PS2_SCRATCHPAD_SIZE), VU0 micro memory and VU0 data memory (4 KB each).
// Interrupts, timers, DMA, GS, VU1 and IOP state are not saved: the oracle runs with them idle.
#pragma once

#include <cstdint>

namespace rc {

struct RamDumpHeader
{
    char magic[8];            // "RCDUMP1"
    uint32_t contextSize;     // sizeof(R5900Context) in the writing build
    uint32_t level;           // RC_BOOT_LEVEL, 0 for none
    uint64_t frame;           // game frames since the level started (since boot if none had)
    uint64_t totalFrames;     // game frames since boot
    uint32_t guestHeapBase, guestHeapEnd, guestHeapLimit; // the runtime's own guest heap
    uint32_t reserved[5];
};

constexpr char kRamDumpMagic[8] = {'R', 'C', 'D', 'U', 'M', 'P', '1', '\0'};

} // namespace rc

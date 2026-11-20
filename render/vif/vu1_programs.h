#pragma once

// The VU1 program table of the VIF1 reader (RENDERER_3D_DESIGN.md 5.3). A program is recognised by the FNV-1a 64 hash
// of micro memory from 0 to the program's length: what follows it is another program's tail or zeros. The lengths
// and prefix hashes are the images' (decomp/generated/vusrc/*.bin, vu1_images/: the image without its trailing zeros),
// confirmed against the dumps by the vif consumer (it prints the program each upload selects). One FNV pass over the
// first 9,184 bytes gives the hash at all six lengths.

#include "vu1_ref.h"

#include <cstdint>

namespace rc::rd
{
    struct Vu1ProgramInfo
    {
        rc::vu1::Program program;
        const char *name;
        uint32_t length;       // bytes of micro memory the hash covers
        uint64_t prefixHash;   // FNV-1a 64 over those bytes
        uint64_t imageHash;    // the runtime's key: the same bytes followed by zeros to 16 KB
        // The vector register lanes the reference does not leave as the program does (bit per lane x, y, z, w of each
        // register), measured beside the emulated VU1 on the exact dumps (RENDERER_3D_DESIGN.md, "Batch 1 as built"):
        // none of them reaches a packet or VU memory in any later run, so the reader writes a signalling NaN pattern
        // into them after each run of the program (vu1PoisonBits), and a dependence would fail check (b) at once.
        uint8_t notReproduced[32];
    };
    constexpr uint32_t kVu1ProgramCount = 6;
    const Vu1ProgramInfo &vu1Program(uint32_t index);

    // The program micro memory (16 KB) holds, as an index into the table, or -1 (guard VU_PROGRAM). `hash` receives
    // the whole image's FNV-1a hash for the log when nothing matches.
    int matchVu1Program(const uint8_t *micro, uint64_t *wholeHash = nullptr);
    uint64_t fnv1a64(const uint8_t *data, uint32_t bytes);
    // The pattern written into a lane the reference does not reproduce: a signalling NaN naming its register and lane.
    inline uint32_t vu1PoisonBits(uint32_t reg, uint32_t lane) { return 0x7F80005Au | ((reg * 4u + lane) << 8); }
    inline bool isVu1Poison(uint32_t bits, uint32_t reg, uint32_t lane) { return bits == vu1PoisonBits(reg, lane); }
}

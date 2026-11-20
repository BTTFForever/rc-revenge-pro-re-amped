#include "vu1_programs.h"

namespace rc::rd
{
    namespace
    {
        constexpr uint64_t kOffset = 0xCBF29CE484222325ull, kPrime = 0x100000001B3ull;

        // The lanes the references do not leave as the programs do, measured beside the emulated VU1 (vif --with-emu
        // --vif-no-poison) on twelve exact dumps of both versions (RENDERER_3D_DESIGN.md, Batch 1 as built): the union over
        // the six programs, poisoned after a run of any of them. VF08.xyz, VF11-VF16, VF17.xyz, VF24-VF28 (but the w of
        // VF25 and VF26), VF29.z, VF30, VF31: the references keep these values in C++ locals or skip the work the
        // microcode does on a rejected triangle. Never in it: VF05.w-VF07.w, VF25.w, VF26.w, VF29.w, the lanes that reach
        // a later run's packets (3.4), which every reference leaves exactly as the program does.
        #define RC_VU1_NOT_REPRODUCED {0, 0, 0, 0, 0, 0, 0, 0, 0x7, 0, 0, 0x7, 0xF, 0xF, 0xF, 0xF, 0xF, 0x7, 0, 0, 0, 0, 0, 0, 0xF, 0x7, 0x7, 0xF, 0xF, 0x4, 0xF, 0xF}

        // Ordered by length (the single FNV pass reads them off in order).
        const Vu1ProgramInfo kTable[kVu1ProgramCount] = {
            {rc::vu1::kTrdr, "TRdr_VUShapeProg", 8328, 0x5666277DE85AA04Dull, 0xC3952C0FAEB52EADull, RC_VU1_NOT_REPRODUCED},
            {rc::vu1::kTerdr, "TERdr_VUShapeProg", 8656, 0xBC67609046FF6447ull, 0xBAC581270F262987ull, RC_VU1_NOT_REPRODUCED},
            {rc::vu1::kRdr, "Rdr_VUShapeProg", 8896, 0xC09F4A2ECE566D21ull, 0x6EC8B9FF8FA44221ull, RC_VU1_NOT_REPRODUCED},
            {rc::vu1::kSkin, "Rdr_VUSkinProg", 8904, 0x9F7CF923AB686A0Cull, 0xEC08B22243D4DC8Cull, RC_VU1_NOT_REPRODUCED},
            {rc::vu1::kEnv, "EnvRdr_VUShapeProg", 9128, 0xF60F50C9DB326773ull, 0xAB0E7743A9BA7393ull, RC_VU1_NOT_REPRODUCED},
            {rc::vu1::kWater, "Rdr_VUWaterProg", 9184, 0x333011350D174A27ull, 0xE96770E4143D2DA7ull, RC_VU1_NOT_REPRODUCED},
        };
    }

    const Vu1ProgramInfo &vu1Program(uint32_t index) { return kTable[index < kVu1ProgramCount ? index : 0]; }

    uint64_t fnv1a64(const uint8_t *data, uint32_t bytes)
    {
        uint64_t h = kOffset;
        for (uint32_t i = 0; i < bytes; ++i)
        {
            h ^= data[i];
            h *= kPrime;
        }
        return h;
    }

    int matchVu1Program(const uint8_t *micro, uint64_t *wholeHash)
    {
        uint64_t h = kOffset;
        uint32_t at = 0;
        int found = -1;
        for (uint32_t p = 0; p < kVu1ProgramCount; ++p)
        {
            for (; at < kTable[p].length; ++at)
            {
                h ^= micro[at];
                h *= kPrime;
            }
            if (h == kTable[p].prefixHash && found < 0)
                found = static_cast<int>(p);
        }
        if (wholeHash)
        {
            for (; at < 0x4000u; ++at)
            {
                h ^= micro[at];
                h *= kPrime;
            }
            *wholeHash = h;
        }
        return found;
    }
}

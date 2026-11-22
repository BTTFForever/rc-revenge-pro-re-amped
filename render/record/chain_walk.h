#pragma once

// The host's DMA walker (RENDERER_FRAME_RECORD.md section 4.2): the stream a kick delivers, read from a RamView,
// exactly as the runtime's PS2Memory::writeIORegister builds it (third_party/ps2recomp/ps2xRuntime/src/lib/
// ps2_memory.cpp): tag ids 0-7, the two-deep ASR stack, IRQ with TIE ending the chain, the tag's upper 8 bytes on VIF
// channels with CHCR.TTE, payloads wrapping at the end of RDRAM / the scratchpad, the 2^20 tag cap; normal (and
// interleave) mode as one range from MADR. A walk that reproduces a kick's recorded streamHash read the same bytes in
// the same order. Step 8's VIF reader builds on it.

#include "consumer.h"

#include <cstdint>

namespace rc::rec
{
    struct WalkRange
    {
        uint32_t streamOffset; // where the range's bytes start in the stream (for a tag: where its TTE bytes or the
                               // payload that follows go)
        uint32_t physAddr;     // offset into RDRAM, or into the scratchpad when `scratch`
        uint32_t bytes;        // a tag: 16 (of which 8 enter the stream with TTE on a VIF channel, else none)
        bool scratch;
        bool isTag;
        const uint8_t *data;   // the bytes in the RAM view
    };

    struct WalkVisitor
    {
        virtual void range(const WalkRange &) = 0;
        virtual ~WalkVisitor() = default;
    };

    struct WalkResult
    {
        uint32_t dmaTags = 0;
        uint64_t streamBytes = 0;
        uint64_t streamHash = 0;
        uint32_t absentPageReads = 0;  // pages read that the RAM view does not hold (a consumer bug or a bad dump)
        uint32_t untranslatable = 0;   // addresses the runtime could not translate (TLB-mapped): that range is skipped
        bool tagCap = false;           // stopped at 2^20 tags
        uint32_t tagIds[8] = {};       // tags by id
    };

    // Translates a DMA address as PS2Memory::translateAddress does (scratchpad, the uncached RDRAM mirror, KSEG0/1;
    // false for TLB-mapped KSEG2/3, which RC never uses for DMA).
    bool translateDmaAddress(uint32_t address, uint32_t &offset, bool &scratch);

    // `hash` false: streamHash is left 0 (streamBytes is still the stream's length) and no payload byte is read here.
    WalkResult walkChain(const KickView &kick, const RamView &ram, WalkVisitor *visitor = nullptr, bool hash = true);
}

#pragma once

// The RAM image a consumer of the page shadow rebuilds (RENDERER_FRAME_RECORD.md sections 3.2 and 6.4; renderer plan P2
// step 9 batch 6, RENDERER_P2_DESIGN.md 8.2): 32 MB of RDRAM and the 16 KB scratchpad, which pages have been captured
// and how often each was. rc::rec::Reader keeps one off-line (PAGE records), rc::live::KickQueue's reader thread one live
// (the pages its own PageTracker hands over with each kick). At a kick it equals guest RAM on every byte of the kick's
// stream; other bytes are whatever they were when their page was last captured.

#include "consumer.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace rc::rec
{
    class RamImage
    {
    public:
        // Allocates and zeroes everything: no page present, every generation 0.
        void reset();
        // A new segment: every page absent again (the bytes and generations stay).
        void forget();
        // `count` captured pages from `firstPage` (page indices as PageTracker's: the scratchpad's from kRamPages on), each
        // kPageBytes of `data`. False (nothing copied) when the run does not fit.
        bool apply(uint32_t firstPage, uint32_t count, const uint8_t *data);
        RamView view() const { return RamView{m_ram.get(), m_scratch.get(), m_present.data(), m_generation.data()}; }
        bool allocated() const { return m_ram != nullptr; }

    private:
        std::unique_ptr<uint8_t[]> m_ram, m_scratch;
        std::vector<uint8_t> m_present;
        std::vector<uint32_t> m_generation;
    };
}

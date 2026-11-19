#pragma once

// The page shadow (RENDERER_FRAME_RECORD.md section 3.2): a host copy of every 4 KB page of guest RAM (and the
// scratchpad, page indices 8192..8195) a kick's stream touched. At a kick, the pages the stream touches that are new or
// changed since their last capture are copied and handed out as runs of consecutive pages, before the kick.

#include "format.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace rc::rec
{
    class PageTracker
    {
    public:
        PageTracker();
        // Every page unknown again (a keyframe starts a new segment).
        void reset();
        // A kick starts: the next touches belong to it.
        void beginKick();
        // A byte range of the kick's stream (offset into RDRAM, or into the scratchpad when `scratch`).
        void touch(uint32_t offset, uint32_t bytes, bool scratch);
        // Compares every page touched since beginKick with its copy; emits (firstPage, count, data) for each run of
        // consecutive new or changed pages, in page order (data: count * kPageBytes, valid until the next call).
        // Returns the number of pages emitted.
        uint32_t collect(const uint8_t *ram, const uint8_t *scratchpad,
                         const std::function<void(uint32_t, uint32_t, const uint8_t *)> &emit);

        uint64_t pagesCompared() const { return m_compared; }
        uint64_t pagesEmitted() const { return m_emitted; }

    private:
        std::unique_ptr<uint8_t[]> m_copies;   // kPageCount * kPageBytes
        std::vector<uint8_t> m_known;          // per page
        std::vector<uint32_t> m_stamp;         // per page: the kick that last touched it
        std::vector<uint32_t> m_touched;       // pages touched by this kick
        uint32_t m_kick = 0;
        uint64_t m_compared = 0, m_emitted = 0;
    };
}

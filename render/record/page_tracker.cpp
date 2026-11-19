#include "page_tracker.h"

#include <algorithm>
#include <cstring>

namespace rc::rec
{
    PageTracker::PageTracker()
        : m_copies(new uint8_t[static_cast<size_t>(kPageCount) * kPageBytes]), m_known(kPageCount, 0u), m_stamp(kPageCount, 0u)
    {
        m_touched.reserve(4096);
    }

    void PageTracker::reset()
    {
        std::fill(m_known.begin(), m_known.end(), 0u);
    }

    void PageTracker::beginKick()
    {
        ++m_kick;
        if (m_kick == 0u) // wrapped: forget old stamps
        {
            std::fill(m_stamp.begin(), m_stamp.end(), 0u);
            m_kick = 1u;
        }
        m_touched.clear();
    }

    void PageTracker::touch(uint32_t offset, uint32_t bytes, bool scratch)
    {
        if (bytes == 0u)
            return;
        const uint32_t limit = scratch ? kScratchpadBytes : kRamBytes;
        if (offset >= limit)
            return;
        const uint32_t last = std::min<uint64_t>(static_cast<uint64_t>(offset) + bytes, limit) - 1u;
        const uint32_t base = scratch ? kRamPages : 0u;
        for (uint32_t page = base + offset / kPageBytes, end = base + last / kPageBytes; page <= end; ++page)
        {
            if (m_stamp[page] != m_kick)
            {
                m_stamp[page] = m_kick;
                m_touched.push_back(page);
            }
        }
    }

    uint32_t PageTracker::collect(const uint8_t *ram, const uint8_t *scratchpad,
                                  const std::function<void(uint32_t, uint32_t, const uint8_t *)> &emit)
    {
        std::sort(m_touched.begin(), m_touched.end());
        uint32_t emitted = 0u;
        uint32_t runFirst = 0u, runCount = 0u;
        const auto flush = [&]()
        {
            if (runCount)
            {
                emit(runFirst, runCount, m_copies.get() + static_cast<size_t>(runFirst) * kPageBytes);
                emitted += runCount;
                runCount = 0u;
            }
        };
        for (uint32_t page : m_touched)
        {
            const uint8_t *live = page < kRamPages ? ram + static_cast<size_t>(page) * kPageBytes
                                                   : scratchpad + static_cast<size_t>(page - kRamPages) * kPageBytes;
            uint8_t *copy = m_copies.get() + static_cast<size_t>(page) * kPageBytes;
            ++m_compared;
            if (m_known[page] && std::memcmp(copy, live, kPageBytes) == 0)
                continue;
            std::memcpy(copy, live, kPageBytes);
            m_known[page] = 1u;
            if (runCount && runFirst + runCount == page)
                ++runCount;
            else
            {
                flush();
                runFirst = page;
                runCount = 1u;
            }
        }
        flush();
        m_touched.clear();
        m_emitted += emitted;
        return emitted;
    }
}

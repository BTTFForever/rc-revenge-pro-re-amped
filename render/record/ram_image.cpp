#include "ram_image.h"

#include <algorithm>
#include <cstring>

namespace rc::rec
{
    void RamImage::reset()
    {
        if (!m_ram)
        {
            m_ram.reset(new uint8_t[kRamBytes]);
            m_scratch.reset(new uint8_t[kScratchpadBytes]);
        }
        std::memset(m_ram.get(), 0, kRamBytes);
        std::memset(m_scratch.get(), 0, kScratchpadBytes);
        m_present.assign(kPageCount, 0u);
        m_generation.assign(kPageCount, 0u);
    }

    void RamImage::forget() { std::fill(m_present.begin(), m_present.end(), 0u); }

    bool RamImage::apply(uint32_t firstPage, uint32_t count, const uint8_t *data)
    {
        if (!m_ram || firstPage > kPageCount || count > kPageCount - firstPage)
            return false;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t page = firstPage + i;
            uint8_t *dst = page < kRamPages ? m_ram.get() + static_cast<size_t>(page) * kPageBytes
                                            : m_scratch.get() + static_cast<size_t>(page - kRamPages) * kPageBytes;
            std::memcpy(dst, data + static_cast<size_t>(i) * kPageBytes, kPageBytes);
            m_present[page] = 1u;
            ++m_generation[page];
        }
        return true;
    }
}

#include "gif_walk.h"

#include <algorithm>
#include <cstring>

namespace rc::rd
{
    namespace
    {
        inline uint64_t le64(const uint8_t *p)
        {
            uint64_t v;
            std::memcpy(&v, p, 8);
            return v;
        }
    }

    void GifWalker::registerWrite(uint8_t addr, uint64_t value, GifSink &sink)
    {
        if (addr == 0x01)
            m_vq = static_cast<uint32_t>(value >> 32);
        else if (addr == 0x04 || addr == 0x05 || addr == 0x0C || addr == 0x0D)
            sink.vertexQ(m_vq);
        sink.reg(addr, value);
    }

    void GifWalker::packed(uint8_t d, uint64_t lo, uint64_t hi, GifSink &sink)
    {
        ++m_stats.regWrites;
        switch (d)
        {
        case 0x0: sink.reg(0x00, lo & 0x7FFu); break; // PRIM
        case 0x1:                                    // RGBAQ: R, G, B, A in the low byte of each 32-bit field
            sink.reg(0x01, (lo & 0xFFu) | ((lo >> 32) & 0xFFu) << 8 | (hi & 0xFFu) << 16 | ((hi >> 32) & 0xFFu) << 24 |
                               static_cast<uint64_t>(m_q) << 32);
            break;
        case 0x2: // ST, and Q for the next RGBAQ
            m_q = static_cast<uint32_t>(hi);
            m_vq = m_q;
            sink.reg(0x02, lo);
            break;
        case 0x3: sink.reg(0x03, (lo & 0x3FFFu) | ((lo >> 32) & 0x3FFFu) << 16); break; // UV
        case 0x4:                                                                          // XYZF2 (ADC: XYZF3)
            sink.vertexQ(m_vq);
            sink.reg((hi >> 47) & 1u ? 0x0C : 0x04,
                     (lo & 0xFFFFu) | ((lo >> 32) & 0xFFFFu) << 16 | ((hi >> 4) & 0xFFFFFFu) << 32 | ((hi >> 36) & 0xFFu) << 56);
            break;
        case 0x5: // XYZ2 (ADC: XYZ3)
            sink.vertexQ(m_vq);
            sink.reg((hi >> 47) & 1u ? 0x0D : 0x05, (lo & 0xFFFFu) | ((lo >> 32) & 0xFFFFu) << 16 | (hi & 0xFFFFFFFFu) << 32);
            break;
        case 0xA: sink.reg(0x0A, ((hi >> 36) & 0xFFu) << 56); break; // FOG
        case 0xE: registerWrite(static_cast<uint8_t>(hi & 0xFFu), lo, sink); break; // A+D
        case 0xF: break;                                                         // NOP
        default: registerWrite(d, lo, sink); break;                              // TEX0_1/2, CLAMP_1/2, XYZF3, XYZ3, reserved
        }
    }

    bool GifWalker::resume(const uint8_t *data, uint32_t bytes, uint32_t &at, GifSink &sink)
    {
        if (m_flg >= 2u) // IMAGE (and the disabled form 3, which takes its data the same way)
        {
            const uint32_t n = std::min(m_imageLeft, bytes - at);
            if (n)
            {
                sink.image(data + at, n);
                m_stats.imageBytes += n;
            }
            at += n;
            m_imageLeft -= n;
            m_open = m_imageLeft != 0u;
            return !m_open;
        }
        const bool isPacked = m_flg == 0u;
        const uint32_t item = isPacked ? 16u : 8u;
        while (m_loops != 0u)
        {
            while (m_regIndex < m_nreg)
            {
                if (at + item > bytes)
                    return false;
                if (isPacked)
                    packed(m_regs[m_regIndex], le64(data + at), le64(data + at + 8), sink);
                else if (m_regs[m_regIndex] < 0xEu) // REGLIST: the 64 bits are the register; A+D and NOP do nothing
                {
                    ++m_stats.regWrites;
                    registerWrite(m_regs[m_regIndex], le64(data + at), sink);
                }
                at += item;
                ++m_regIndex;
            }
            m_regIndex = 0;
            --m_loops;
        }
        if (!isPacked && m_oddReglist)
            at += 8u; // REGLIST data is padded to a whole quadword
        m_open = false;
        return true;
    }

    void GifWalker::feed(const uint8_t *data, uint32_t bytes, GifSink &sink)
    {
        uint32_t at = 0;
        if (m_open && !resume(data, bytes, at, sink))
        {
            m_stats.droppedBytes += bytes > at ? bytes - at : 0u;
            return;
        }
        while (at + 16u <= bytes)
        {
            const uint64_t lo = le64(data + at), hi = le64(data + at + 8);
            at += 16u;
            ++m_stats.tags;
            const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
            m_flg = static_cast<uint8_t>((lo >> 58) & 3u);
            m_nreg = static_cast<uint8_t>((lo >> 60) & 0xFu);
            if (m_nreg == 0u)
                m_nreg = 16;
            if ((lo >> 15) & 1u)
                ++m_stats.eop;
            m_q = 0x3F800000u;
            m_vq = 0x3F800000u;
            if ((lo >> 46) & 1u)
            {
                if (m_flg == 0u)
                    sink.reg(0x00, (lo >> 47) & 0x7FFu);
                else
                    ++m_stats.preOutsidePacked;
            }
            for (uint32_t i = 0; i < 16u; ++i)
                m_regs[i] = static_cast<uint8_t>((hi >> (i * 4u)) & 0xFu);
            m_loops = nloop;
            m_regIndex = 0;
            m_imageLeft = nloop * 16u;
            m_oddReglist = ((nloop * m_nreg) & 1u) != 0u;
            m_open = true;
            if (m_flg == 0u)
                ++m_stats.packed;
            else if (m_flg == 1u)
                ++m_stats.reglist;
            else
                ++m_stats.image;
            if (!resume(data, bytes, at, sink))
                break;
        }
        if (at < bytes)
            m_stats.droppedBytes += bytes - at;
    }
}

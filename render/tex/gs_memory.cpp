#include "gs_memory.h"

#include <algorithm>
#include <cstring>

namespace rc::rd
{
    namespace
    {
        constexpr uint32_t kMask = kGsBytes - 1u;
    }

    GsMemory::GsMemory() : m_mem(new uint8_t[kGsBytes]), m_blockSerial(new uint64_t[kGsBlocks]) { clear(); }

    void GsMemory::clear()
    {
        std::memset(m_mem.get(), 0, kGsBytes);
        ++m_serial;
        ++m_writes;
        std::fill(m_blockSerial.get(), m_blockSerial.get() + kGsBlocks, m_serial);
        std::fill(m_pageSerial, m_pageSerial + kGsPages, m_serial);
        if (m_origin)
            std::memset(m_origin.get(), kOriginLoaded, kGsBytes / 4u);
        m_open = false;
    }

    void GsMemory::load(const uint8_t *image)
    {
        std::memcpy(m_mem.get(), image, kGsBytes);
        ++m_serial;
        ++m_writes;
        std::fill(m_blockSerial.get(), m_blockSerial.get() + kGsBlocks, m_serial);
        std::fill(m_pageSerial, m_pageSerial + kGsPages, m_serial);
        if (m_origin)
            std::memset(m_origin.get(), kOriginLoaded, kGsBytes / 4u);
        m_open = false;
    }

    void GsMemory::trackOrigins(bool on)
    {
        if (!on)
            m_origin.reset();
        else if (!m_origin)
        {
            m_origin.reset(new uint8_t[kGsBytes / 4u]);
            std::memset(m_origin.get(), kOriginLoaded, kGsBytes / 4u);
        }
    }

    void GsMemory::markDrawnPage(uint32_t page)
    {
        if (m_origin)
            std::memset(m_origin.get() + (page & (kGsPages - 1u)) * (kGsPageBytes / 4u), kOriginDrawn, kGsPageBytes / 4u);
    }

    void GsMemory::markAllDrawn()
    {
        if (m_origin)
            std::memset(m_origin.get(), kOriginDrawn, kGsBytes / 4u);
    }

    void GsMemory::setTransferRegisters(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, uint64_t trxdir)
    {
        m_bitbltbuf = bitbltbuf;
        m_trxpos = trxpos;
        m_trxreg = trxreg;
        m_trxdir = trxdir;
        m_open = false;
    }

    void GsMemory::writeRegister(uint8_t addr, uint64_t value)
    {
        switch (addr)
        {
        case kRegBitbltbuf: m_bitbltbuf = value; break;
        case kRegTrxpos: m_trxpos = value; break;
        case kRegTrxreg: m_trxreg = value; break;
        case kRegTrxdir:
            m_trxdir = value;
            begin();
            break;
        case kRegHwreg:
        {
            uint8_t bytes[8];
            std::memcpy(bytes, &value, 8);
            imageData(bytes, 8);
            break;
        }
        default: break;
        }
    }

    void GsMemory::begin()
    {
        if (m_open && m_done < m_total)
            ++m_stats.unfinished;
        m_open = false;
        const uint32_t dir = static_cast<uint32_t>(m_trxdir & 3u);
        if (dir == 1u)
        {
            ++m_stats.localToHost;
            return;
        }
        if (dir == 2u)
        {
            ++m_stats.localToLocal;
            return;
        }
        if (dir != 0u)
            return; // 3: transfers off
        m_dbp = static_cast<uint32_t>((m_bitbltbuf >> 32) & 0x3FFFu);
        m_dbw = std::max<uint32_t>(static_cast<uint32_t>((m_bitbltbuf >> 48) & 0x3Fu), 1u);
        m_dpsm = static_cast<uint8_t>((m_bitbltbuf >> 56) & 0x3Fu);
        m_dsax = static_cast<uint32_t>((m_trxpos >> 32) & 0x7FFu);
        m_dsay = static_cast<uint32_t>((m_trxpos >> 48) & 0x7FFu);
        m_rrw = static_cast<uint32_t>(m_trxreg & 0xFFFu);
        m_rrh = static_cast<uint32_t>((m_trxreg >> 32) & 0xFFFu);
        m_total = m_rrw * m_rrh;
        m_done = 0;
        FormatInfo f;
        if (m_total == 0u || !gsFormat(m_dpsm, f))
        {
            ++m_stats.unsupported;
            return;
        }
        m_open = true;
        m_countedChanged = false;
        m_changed = 0;
        m_carryBytes = 0;
        m_lastBlock = ~0u;
        ++m_serial;
        ++m_stats.transfers;
        ++m_stats.byPsm[m_dpsm];
        if (m_stats.transfers == m_dropAt)
            m_open = false; // a planted fault: its data goes nowhere
        if (gsBlockOf(layoutTables(), m_dpsm, m_dbp, m_dbw, m_dsax, m_dsay) < kTextureFirstBlock)
        {
            ++m_stats.intoFrame;
            if (m_log)
                m_log->push_back({m_dbp, m_dbw, m_dpsm, m_dsax, m_dsay, m_rrw, m_rrh, m_serial});
        }
    }

    inline void GsMemory::mark(uint32_t block)
    {
        ++m_changed;
        ++m_writes;
        if (block == m_lastBlock)
            return;
        m_lastBlock = block;
        if (m_blockSerial[block] != m_serial)
        {
            m_blockSerial[block] = m_serial;
            m_pageSerial[block >> 5] = m_serial;
            ++m_stats.blockMarks;
        }
    }

    void GsMemory::imageData(const uint8_t *data, size_t bytes)
    {
        if (!m_open)
        {
            m_stats.excessBytes += bytes;
            return;
        }
        const LayoutTables &t = layoutTables();
        uint8_t *mem = m_mem.get();
        size_t at = 0;
        uint32_t done = m_done;
        const uint32_t total = m_total, rrw = m_rrw;
        // The rectangle's pixels arrive row by row; x and y wrap at 2048 as the GS's coordinates do.
        uint32_t x = done % rrw, y = done / rrw;
        auto step = [&]
        {
            ++done;
            if (++x == rrw)
            {
                x = 0;
                ++y;
            }
        };
        switch (m_dpsm)
        {
        case kPsmCt32:
            while (done < total && bytes - at >= 4u)
            {
                const uint32_t a = gsAddr32(t, m_dbp, m_dbw, (m_dsax + x) & 0x7FFu, (m_dsay + y) & 0x7FFu) & kMask;
                if (m_origin)
                    m_origin[a >> 2] = kOriginUploaded;
                if (std::memcmp(mem + a, data + at, 4) != 0)
                {
                    std::memcpy(mem + a, data + at, 4);
                    mark(a >> 8);
                }
                at += 4;
                step();
            }
            break;
        case kPsmCt24:
            // Three bytes a pixel: a pixel may straddle two packets, so up to two bytes wait for the next call.
            while (done < total && at < bytes)
            {
                m_carry[m_carryBytes++] = data[at++];
                if (m_carryBytes < 3u)
                    continue;
                m_carryBytes = 0;
                const uint32_t a = gsAddr32(t, m_dbp, m_dbw, (m_dsax + x) & 0x7FFu, (m_dsay + y) & 0x7FFu) & kMask;
                if (std::memcmp(mem + a, m_carry, 3) != 0)
                {
                    std::memcpy(mem + a, m_carry, 3); // the top byte stays
                    mark(a >> 8);
                }
                step();
            }
            break;
        case kPsmCt16:
        case kPsmCt16s:
            while (done < total && bytes - at >= 2u)
            {
                const uint32_t px = (m_dsax + x) & 0x7FFu, py = (m_dsay + y) & 0x7FFu;
                const uint32_t a = (m_dpsm == kPsmCt16 ? gsAddr16(t, m_dbp, m_dbw, px, py) : gsAddr16s(t, m_dbp, m_dbw, px, py)) & kMask;
                if (std::memcmp(mem + a, data + at, 2) != 0)
                {
                    std::memcpy(mem + a, data + at, 2);
                    mark(a >> 8);
                }
                at += 2;
                step();
            }
            break;
        case kPsmT8:
            while (done < total && at < bytes)
            {
                const uint32_t a = gsAddr8(t, m_dbp, m_dbw, (m_dsax + x) & 0x7FFu, (m_dsay + y) & 0x7FFu) & kMask;
                const uint8_t v = data[at++];
                if (mem[a] != v)
                {
                    mem[a] = v;
                    mark(a >> 8);
                }
                step();
            }
            break;
        case kPsmT4:
            // A source byte is two pixels, the low nibble first; a rectangle with an odd pixel count drops the last
            // byte's high nibble.
            while (done < total && at < bytes)
            {
                const uint8_t packed = data[at++];
                for (uint32_t half = 0; half < 2u && done < total; ++half)
                {
                    const uint32_t n = gsNibble4(t, m_dbp, m_dbw, (m_dsax + x) & 0x7FFu, (m_dsay + y) & 0x7FFu);
                    const uint32_t a = (n >> 1) & kMask, shift = (n & 1u) * 4u;
                    const uint8_t v = static_cast<uint8_t>((mem[a] & ~(0x0Fu << shift)) | (((packed >> (half * 4u)) & 0x0Fu) << shift));
                    if (mem[a] != v)
                    {
                        mem[a] = v;
                        mark(a >> 8);
                    }
                    step();
                }
            }
            break;
        default: break;
        }
        m_stats.pixels += done - m_done;
        m_stats.pixelsChanged += m_changed;
        if (m_changed && !m_countedChanged)
        {
            m_countedChanged = true;
            ++m_stats.transfersChanging;
        }
        m_changed = 0;
        m_stats.bytes += at;
        m_stats.bytesByPsm[m_dpsm] += at;
        m_done = done;
        if (done >= total)
        {
            m_open = false;
            m_stats.excessBytes += bytes - at;
        }
    }

    uint32_t GsMemory::read32(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) const
    {
        uint32_t v;
        std::memcpy(&v, m_mem.get() + (gsAddr32(layoutTables(), bp, bw, x, y) & kMask), 4);
        return v;
    }
    uint32_t GsMemory::read16(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) const
    {
        uint16_t v;
        std::memcpy(&v, m_mem.get() + (gsAddr16(layoutTables(), bp, bw, x, y) & kMask), 2);
        return v;
    }
    uint32_t GsMemory::read16s(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) const
    {
        uint16_t v;
        std::memcpy(&v, m_mem.get() + (gsAddr16s(layoutTables(), bp, bw, x, y) & kMask), 2);
        return v;
    }
    uint32_t GsMemory::read8(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) const
    {
        return m_mem[gsAddr8(layoutTables(), bp, bw, x, y) & kMask];
    }
    uint32_t GsMemory::read4(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) const
    {
        const uint32_t n = gsNibble4(layoutTables(), bp, bw, x, y);
        return (m_mem[(n >> 1) & kMask] >> ((n & 1u) * 4u)) & 0x0Fu;
    }
}

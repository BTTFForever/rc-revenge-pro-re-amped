#include "tex_decode.h"

#include <algorithm>
#include <cstring>

namespace rc::rd
{
    namespace
    {
        inline bool clut16(uint8_t cpsm) { return cpsm == kPsmCt16 || cpsm == kPsmCt16s; }
        inline bool clut32(uint8_t cpsm) { return cpsm == kPsmCt32 || cpsm == kPsmCt24; }
    }

    bool Clut::write(const GsMemory &mem, const Tex0 &t, uint64_t texclut)
    {
        if (!t.indexed())
            return false;
        switch (t.cld)
        {
        case 1: break;
        case 2: m_cbp[0] = t.cbp; break;
        case 3: m_cbp[1] = t.cbp; break;
        case 4:
            if (m_cbp[0] == t.cbp)
                return false;
            m_cbp[0] = t.cbp;
            break;
        case 5:
            if (m_cbp[1] == t.cbp)
                return false;
            m_cbp[1] = t.cbp;
            break;
        default: return false; // 0: keep the buffer; 6, 7: reserved
        }
        load(mem, t, texclut);
        return true;
    }

    void Clut::load(const GsMemory &mem, const Tex0 &t, uint64_t texclut)
    {
        const bool four = t.psm == kPsmT4, is16 = clut16(t.cpsm);
        if (!is16 && !clut32(t.cpsm))
            return;
        ++m_loads;
        ++m_version;
        const uint32_t count = four ? 16u : 256u;
        const uint32_t base = (static_cast<uint32_t>(t.csa) & (is16 ? 0x1Fu : 0x0Fu)) << 4;
        // An 8-bit texture's 32-bit CSM1 palette fills the buffer in place from entry CSA * 16 up.
        const bool inPlace = t.csm == 0u && !is16 && !four;
        const uint32_t cbw = static_cast<uint32_t>(texclut & 0x3Fu), cou = static_cast<uint32_t>((texclut >> 6) & 0x3Fu),
                       cov = static_cast<uint32_t>((texclut >> 12) & 0x3FFu);
        for (uint32_t e = inPlace ? base : 0u; e < count; ++e)
        {
            uint32_t x, y, bw = 1;
            if (t.csm == 0u)
            {
                const uint32_t slot = clutSlot(e);
                x = slot & 0x0Fu;
                y = slot >> 4;
            }
            else
            {
                bw = cbw ? cbw : 1u;
                x = (cou << 4) + e;
                y = cov;
            }
            uint32_t raw;
            switch (t.cpsm)
            {
            case kPsmCt32: raw = mem.read32(t.cbp, bw, x, y); break;
            case kPsmCt24: raw = mem.read32(t.cbp, bw, x, y) & 0x00FFFFFFu; break;
            case kPsmCt16: raw = mem.read16(t.cbp, bw, x, y); break;
            default: raw = mem.read16s(t.cbp, bw, x, y); break;
            }
            const uint32_t dst = (inPlace ? e : base + e) & (is16 ? 0x1FFu : 0x0FFu);
            m_buf[dst] = static_cast<uint16_t>(raw);
            if (!is16)
                m_buf[dst + 256u] = static_cast<uint16_t>(raw >> 16);
        }
    }

    bool Clut::writeMemo(const GsMemory &mem, const Tex0 &t, uint64_t texclut, ClutLoadMemo &memo)
    {
        // write()'s decision, unchanged.
        if (!t.indexed())
            return false;
        switch (t.cld)
        {
        case 1: break;
        case 2: m_cbp[0] = t.cbp; break;
        case 3: m_cbp[1] = t.cbp; break;
        case 4:
            if (m_cbp[0] == t.cbp)
                return false;
            m_cbp[0] = t.cbp;
            break;
        case 5:
            if (m_cbp[1] == t.cbp)
                return false;
            m_cbp[1] = t.cbp;
            break;
        default: return false;
        }
        // load()'s, split into what it reads (kept in the memo) and where it puts it.
        const bool four = t.psm == kPsmT4, is16 = clut16(t.cpsm);
        if (!is16 && !clut32(t.cpsm))
            return true;
        ++m_loads;
        const uint32_t count = four ? 16u : 256u;
        const uint32_t base = (static_cast<uint32_t>(t.csa) & (is16 ? 0x1Fu : 0x0Fu)) << 4;
        const bool inPlace = t.csm == 0u && !is16 && !four;
        const uint32_t first = inPlace ? base : 0u;
        if (memo.writes != mem.writes() && !(memo.faultIgnoreWrites && memo.writes != ~0ull))
        {
            memo.index.clear();
            memo.raw.clear();
            memo.writes = mem.writes();
        }
        const uint64_t key = static_cast<uint64_t>(t.cbp) | static_cast<uint64_t>(t.cpsm) << 14 | static_cast<uint64_t>(t.csm) << 18 |
                             static_cast<uint64_t>(t.csa) << 19 | static_cast<uint64_t>(four) << 24 | (t.csm ? (texclut & 0x3FFFFFull) << 25 : 0ull);
        const ClutLoadMemo::Values *values;
        if (auto it = memo.index.find(key); it != memo.index.end())
        {
            values = &memo.raw[it->second];
            ++memo.hits;
        }
        else
        {
            ++memo.reads;
            memo.index.emplace(key, static_cast<uint32_t>(memo.raw.size()));
            memo.raw.emplace_back();
            ClutLoadMemo::Values &out = memo.raw.back();
            const uint32_t cbw = static_cast<uint32_t>(texclut & 0x3Fu), cou = static_cast<uint32_t>((texclut >> 6) & 0x3Fu),
                           cov = static_cast<uint32_t>((texclut >> 12) & 0x3FFu);
            for (uint32_t e = first; e < count; ++e)
            {
                uint32_t x, y, bw = 1;
                if (t.csm == 0u)
                {
                    const uint32_t slot = clutSlot(e);
                    x = slot & 0x0Fu;
                    y = slot >> 4;
                }
                else
                {
                    bw = cbw ? cbw : 1u;
                    x = (cou << 4) + e;
                    y = cov;
                }
                uint32_t raw;
                switch (t.cpsm)
                {
                case kPsmCt32: raw = mem.read32(t.cbp, bw, x, y); break;
                case kPsmCt24: raw = mem.read32(t.cbp, bw, x, y) & 0x00FFFFFFu; break;
                case kPsmCt16: raw = mem.read16(t.cbp, bw, x, y); break;
                default: raw = mem.read16s(t.cbp, bw, x, y); break;
                }
                out.lo[e] = static_cast<uint16_t>(raw);
                out.hi[e] = static_cast<uint16_t>(raw >> 16);
            }
            values = &out;
        }
        // Where load() puts entry e: (inPlace ? e : base + e) masked to the buffer's half (8 bits) or whole (16 bits).
        bool changed = false;
        const uint32_t mask = is16 ? 0x1FFu : 0x0FFu, d0 = inPlace ? first : base + first, n = count - first;
        if (d0 + n - 1u <= mask)
        {
            // No wrap: one run of entries (two for a 32-bit palette).
            if (std::memcmp(m_buf + d0, values->lo + first, n * 2u) != 0)
            {
                std::memcpy(m_buf + d0, values->lo + first, n * 2u);
                changed = true;
            }
            if (!is16 && std::memcmp(m_buf + 256u + d0, values->hi + first, n * 2u) != 0)
            {
                std::memcpy(m_buf + 256u + d0, values->hi + first, n * 2u);
                changed = true;
            }
        }
        else
            for (uint32_t e = first; e < count; ++e)
            {
                const uint32_t dst = (inPlace ? e : base + e) & mask;
                changed |= m_buf[dst] != values->lo[e];
                m_buf[dst] = values->lo[e];
                if (!is16)
                {
                    changed |= m_buf[dst + 256u] != values->hi[e];
                    m_buf[dst + 256u] = values->hi[e];
                }
            }
        if (changed)
            ++m_version;
        return true;
    }

    uint32_t Clut::lookup(const Tex0 &t, uint32_t index, const Texa &texa) const
    {
        const bool four = t.psm == kPsmT4, is16 = clut16(t.cpsm);
        const uint32_t base = (static_cast<uint32_t>(t.csa) & (is16 ? 0x1Fu : 0x0Fu)) << 4;
        if (four)
            index &= 0x0Fu;
        uint32_t at = (base + index) & (is16 ? 0x1FFu : 0x0FFu);
        if (!is16 && t.csm == 0u && !four)
            at = std::min((index & 0xF0u) + base, 240u) + (index & 0x0Fu);
        if (is16)
            return expand16(m_buf[at], texa);
        const uint32_t raw = static_cast<uint32_t>(m_buf[at]) | static_cast<uint32_t>(m_buf[at + 256u]) << 16;
        return t.cpsm == kPsmCt24 ? expand24(raw, texa) : raw;
    }

    void Clut::palette(const Tex0 &t, const Texa &texa, uint32_t *out) const
    {
        const uint32_t count = t.psm == kPsmT4 ? 16u : 256u;
        for (uint32_t i = 0; i < count; ++i)
            out[i] = lookup(t, i, texa);
    }

    MipLevel mipLevel(const Tex0 &t, uint64_t miptbp1, uint64_t miptbp2, uint32_t level)
    {
        MipLevel m;
        m.width = std::max(t.width() >> level, 1u);
        m.height = std::max(t.height() >> level, 1u);
        if (level == 0u)
        {
            m.tbp = t.tbp0;
            m.tbw = t.tbw;
            return m;
        }
        const uint64_t reg = level <= 3u ? miptbp1 : miptbp2;
        const uint32_t shift = ((level - 1u) % 3u) * 20u;
        m.tbp = static_cast<uint32_t>((reg >> shift) & 0x3FFFu);
        m.tbw = static_cast<uint32_t>((reg >> (shift + 14u)) & 0x3Fu);
        return m;
    }

    bool decodeTexels(const GsMemory &mem, const Tex0 &t, uint32_t tbp, uint32_t tbw, uint32_t width, uint32_t height, const Texa &texa,
                      const uint32_t *palette, uint8_t *out)
    {
        const LayoutTables &lt = layoutTables();
        const uint8_t *m = mem.data();
        constexpr uint32_t mask = kGsBytes - 1u;
        auto put = [&](size_t i, uint32_t v) { std::memcpy(out + i * 4u, &v, 4); };
        size_t i = 0;
        switch (t.psm)
        {
        case kPsmCt32:
            for (uint32_t y = 0; y < height; ++y)
                for (uint32_t x = 0; x < width; ++x, ++i)
                    std::memcpy(out + i * 4u, m + (gsAddr32(lt, tbp, tbw, x, y) & mask), 4);
            return true;
        case kPsmCt24:
            for (uint32_t y = 0; y < height; ++y)
                for (uint32_t x = 0; x < width; ++x, ++i)
                    put(i, expand24(mem.read32(tbp, tbw, x, y), texa));
            return true;
        case kPsmCt16:
            for (uint32_t y = 0; y < height; ++y)
                for (uint32_t x = 0; x < width; ++x, ++i)
                    put(i, expand16(mem.read16(tbp, tbw, x, y), texa));
            return true;
        case kPsmCt16s:
            for (uint32_t y = 0; y < height; ++y)
                for (uint32_t x = 0; x < width; ++x, ++i)
                    put(i, expand16(mem.read16s(tbp, tbw, x, y), texa));
            return true;
        case kPsmT8:
            if (!palette)
                return false;
            for (uint32_t y = 0; y < height; ++y)
                for (uint32_t x = 0; x < width; ++x, ++i)
                    put(i, palette[m[gsAddr8(lt, tbp, tbw, x, y) & mask]]);
            return true;
        case kPsmT4:
            if (!palette)
                return false;
            for (uint32_t y = 0; y < height; ++y)
                for (uint32_t x = 0; x < width; ++x, ++i)
                {
                    const uint32_t n = gsNibble4(lt, tbp, tbw, x, y);
                    put(i, palette[(m[(n >> 1) & mask] >> ((n & 1u) * 4u)) & 0x0Fu]);
                }
            return true;
        default: return false;
        }
    }

    bool decodeTexture(const GsMemory &mem, const Tex0 &t, const Texa &texa, uint8_t *out)
    {
        uint32_t palette[256];
        if (t.indexed())
        {
            Clut clut;
            clut.load(mem, t);
            clut.palette(t, texa, palette);
        }
        return decodeTexels(mem, t, t.tbp0, t.tbw, t.width(), t.height(), texa, palette, out);
    }
}

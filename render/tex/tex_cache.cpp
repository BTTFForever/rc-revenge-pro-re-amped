#include "tex_cache.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace rc::rd
{
    namespace
    {
        inline bool usesTexa(const Tex0 &t)
        {
            const bool texel = t.psm == kPsmCt16 || t.psm == kPsmCt16s || t.psm == kPsmCt24;
            const bool pal = t.indexed() && t.cpsm != kPsmCt32;
            return texel || pal;
        }
        Texa texaOf(uint32_t key) { return {static_cast<uint8_t>(key & 0xFFu), static_cast<uint8_t>((key >> 8) & 1u), static_cast<uint8_t>(key >> 16)}; }
    }

    const CachedTexture &TextureCache::get(const Tex0 &t, const Texa &texa, const Clut *clut)
    {
        return get(t, mipLevel(t, 0, 0, 0), texa, clut);
    }

    const CachedTexture &TextureCache::get(const Tex0 &t, const MipLevel &place, const Texa &texa, const Clut *clut)
    {
        ++m_stats.lookups;
        TexKey key;
        key.tbp = place.tbp;
        key.tbw = static_cast<uint8_t>(place.tbw);
        key.width = static_cast<uint16_t>(place.width);
        key.height = static_cast<uint16_t>(place.height);
        key.psm = t.psm;
        if (t.indexed())
        {
            key.cbp = t.cbp;
            key.cpsm = t.cpsm;
            key.csm = t.csm;
            key.csa = t.csa;
        }
        key.texa = usesTexa(t) ? texa.key() | 0x80000000u : 0u;
        std::unique_ptr<CachedTexture> &slot = m_entries[key];
        if (!slot)
        {
            slot = std::make_unique<CachedTexture>();
            CachedTexture &e = *slot;
            e.key = key;
            e.tex0 = t;
            coveredBlocks(t.psm, place.tbp, place.tbw, place.width, place.height, e.blocks);
            std::sort(e.blocks.begin(), e.blocks.end());
            e.blocks.erase(std::unique(e.blocks.begin(), e.blocks.end()), e.blocks.end());
            e.paletteBlocksAt = static_cast<uint32_t>(e.blocks.size());
            if (t.indexed() && t.csm == 0u)
                coveredBlocks(t.cpsm, t.cbp, 1, t.psm == kPsmT4 ? 8 : 16, t.psm == kPsmT4 ? 2 : 16, e.blocks);
            decode(e, texa, clut);
            return e;
        }
        CachedTexture &e = *slot;
        const uint64_t now = m_mem.serial();
        if (!clut && e.checkedAt == now)
        {
            ++m_stats.quickHits;
            return e;
        }
        ++m_stats.validations;
        bool again = stale(e);
        if (!again && clut && t.indexed())
        {
            // The reader's CLUT buffer decides, not memory: decode again when its colours differ from the ones used.
            uint32_t pal[256];
            clut->palette(t, texa, pal);
            again = std::memcmp(pal, e.palette, (t.psm == kPsmT4 ? 16u : 256u) * 4u) != 0;
        }
        if (again)
        {
            ++m_stats.redecodes;
            decode(e, texa, clut);
        }
        e.checkedAt = now;
        return e;
    }

    bool TextureCache::stale(const CachedTexture &e) const
    {
        for (const uint16_t b : e.blocks)
            if (m_mem.blockSerial(b) > e.decodedAt)
                return true;
        return false;
    }

    void TextureCache::decode(CachedTexture &e, const Texa &texa, const Clut *clut)
    {
        const auto t0 = std::chrono::steady_clock::now();
        const Tex0 &t = e.tex0;
        const size_t texels = static_cast<size_t>(e.key.width) * e.key.height;
        e.rgba.resize(texels * 4u);
        if (t.indexed())
        {
            if (clut)
                clut->palette(t, texa, e.palette);
            else
            {
                Clut fromMemory;
                fromMemory.load(m_mem, t);
                fromMemory.palette(t, texa, e.palette);
            }
        }
        e.valid = decodeTexels(m_mem, t, e.key.tbp, e.key.tbw, e.key.width, e.key.height, texa, e.palette, e.rgba.data());
        e.decodedAt = e.checkedAt = m_mem.serial();
        ++e.generation;
        uint32_t above = 0, at = 0, zero = 0;
        uint8_t lo = 0xFF, hi = 0;
        const uint8_t *a = e.rgba.data() + 3;
        for (size_t i = 0; i < texels; ++i, a += 4)
        {
            const uint8_t v = *a;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            above += v > 0x80u;
            at += v == 0x80u;
            zero += v == 0u;
        }
        e.alphaMin = lo;
        e.alphaMax = hi;
        e.alphaAbove80 = above;
        e.alphaAt80 = at;
        e.alphaZero = zero;
        ++m_stats.decodes;
        m_stats.texels += texels;
        m_stats.decodeNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    }

    uint32_t TextureCache::refreshAll()
    {
        uint32_t n = 0;
        const uint64_t now = m_mem.serial();
        for (auto &it : m_entries)
        {
            CachedTexture &e = *it.second;
            if (e.checkedAt == now)
                continue;
            if (stale(e))
            {
                ++m_stats.redecodes;
                decode(e, texaOf(e.key.texa), nullptr);
                ++n;
            }
            e.checkedAt = now;
        }
        return n;
    }

    void TextureCache::decodeAll()
    {
        for (auto &it : m_entries)
            decode(*it.second, texaOf(it.second->key.texa), nullptr);
    }
}

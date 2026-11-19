#pragma once

// The host texture cache over the shadow of GS memory (renderer plan P1 step 5; RENDERER_FRAME_RECORD.md section 17).
// A texture is keyed by what decides its texels: where it is (TBP, TBW), its format and size, and for T8/T4 where its
// palette is and how it is read (CBP, CPSM, CSM, CSA), plus TEXA where a 16- or 24-bit format takes its alpha from it.
// TCC, TFX and CLD are not part of the key: they change how texels are used, not what they are.
//
// An entry remembers the memory serial it was decoded at and the blocks its texels and palette occupy; get() decodes
// again when one of those blocks was written since (an animated texture's next frame, the Track Editor's map). Each
// decode bumps the entry's generation: a GPU uploader re-uploads an entry whose generation it has not seen, so the
// upload is incremental with no further bookkeeping (about 20 small textures a race frame, see section 17).
// Entries live until clear(); eviction is step 9's concern.

#include "tex_decode.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace rc::rd
{
    struct TexKey
    {
        uint32_t tbp = 0, cbp = 0, texa = 0;
        uint16_t width = 0, height = 0;
        uint8_t tbw = 0, psm = 0, cpsm = 0, csm = 0, csa = 0;
        bool operator==(const TexKey &o) const
        {
            return tbp == o.tbp && cbp == o.cbp && texa == o.texa && width == o.width && height == o.height && tbw == o.tbw && psm == o.psm &&
                   cpsm == o.cpsm && csm == o.csm && csa == o.csa;
        }
    };
    struct TexKeyHash
    {
        size_t operator()(const TexKey &k) const
        {
            uint64_t h = k.tbp | static_cast<uint64_t>(k.cbp) << 14 | static_cast<uint64_t>(k.psm) << 28 | static_cast<uint64_t>(k.tbw) << 34 |
                         static_cast<uint64_t>(k.width) << 40 | static_cast<uint64_t>(k.height) << 51;
            h ^= (static_cast<uint64_t>(k.texa) << 7) ^ (static_cast<uint64_t>(k.cpsm) << 3) ^ (static_cast<uint64_t>(k.csa) << 58) ^ k.csm;
            h *= 0x9E3779B97F4A7C15ull;
            return static_cast<size_t>(h ^ (h >> 29));
        }
    };

    struct CachedTexture
    {
        TexKey key;
        Tex0 tex0;                    // as first asked for (its format fields; the key holds the level's place and size)
        std::vector<uint8_t> rgba;    // width * height * 4: R, G, B, A with the GS's raw alpha
        uint32_t generation = 0;      // 1 after the first decode, +1 at each later one
        uint64_t decodedAt = 0;       // GsMemory::serial() at the decode
        uint64_t checkedAt = 0;       // ... at the last validation
        std::vector<uint16_t> blocks; // texel blocks, then the palette's blocks from `paletteBlocksAt`
        uint32_t paletteBlocksAt = 0;
        uint32_t palette[256] = {};   // the colours it was decoded with (T8/T4)
        bool valid = false;           // false: a format the decoder does not handle
        // Alpha summary of the last decode (raw alpha; 0x80 = 1.0).
        uint8_t alphaMin = 0, alphaMax = 0;
        uint32_t alphaAbove80 = 0, alphaAt80 = 0, alphaZero = 0;
        // Over every decode since the entry was made.
        uint8_t alphaMaxEver = 0, alphaMinEver = 0xFF;
        uint32_t alphaAbove80Most = 0;
    };

    class TextureCache
    {
    public:
        struct Stats
        {
            uint64_t lookups = 0, quickHits = 0, validations = 0, decodes = 0, redecodes = 0, texels = 0, decodeNs = 0;
        };

        explicit TextureCache(const GsMemory &mem) : m_mem(mem) {}

        // Level 0 of `t`. clut null: the palette as memory holds it now, which is what a CLD 1 write of TEX0 loads (all
        // RC does). Otherwise the palette comes from the reader's CLUT buffer.
        const CachedTexture &get(const Tex0 &t, const Texa &texa, const Clut *clut = nullptr);
        // Any level: `place` from mipLevel().
        const CachedTexture &get(const Tex0 &t, const MipLevel &place, const Texa &texa, const Clut *clut = nullptr);

        // Validates every entry against memory as it is now; returns how many were decoded again.
        uint32_t refreshAll();
        // Decodes every entry again whatever its state (for measuring a full decode).
        void decodeAll();
        void clear() { m_entries.clear(); }

        template <class F> void forEach(F &&f) const
        {
            for (const auto &e : m_entries)
                f(*e.second);
        }
        size_t size() const { return m_entries.size(); }
        const Stats &stats() const { return m_stats; }

    private:
        bool stale(const CachedTexture &e) const;
        void decode(CachedTexture &e, const Texa &texa, const Clut *clut);

        const GsMemory &m_mem;
        std::unordered_map<TexKey, std::unique_ptr<CachedTexture>, TexKeyHash> m_entries;
        Stats m_stats;
    };
}

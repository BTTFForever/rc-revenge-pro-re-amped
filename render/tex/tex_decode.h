#pragma once

// Texture decode from the shadow of GS memory (renderer plan P1 step 5; RENDERER_FRAME_RECORD.md section 17): TEX0
// (and MIPTBP for the lower levels) to RGBA8, 4 bytes a texel R, G, B, A with the GS's raw alpha (0x80 = 1.0), which is
// what the GS's texture function receives. Formats: PSMCT32, PSMCT24, PSMCT16/16S (alpha from TEXA), PSMT8 and PSMT4
// through the CLUT buffer (CT32 or CT16 palettes, CSM1 and CSM2).
// RC samples only T8/T4 with CT32 palettes (CSM1, CSA 0, CLD 1), CT32 and CT16 (survey); the rest follows the GS and
// the emulated GS's code and is covered by rc_tex_test only.

#include "gs_memory.h"

#include <cstdint>

namespace rc::rd
{
    struct Tex0
    {
        uint32_t tbp0 = 0, cbp = 0;
        uint8_t tbw = 0, psm = 0, tw = 0, th = 0, tcc = 0, tfx = 0, cpsm = 0, csm = 0, csa = 0, cld = 0;

        static Tex0 unpack(uint64_t v)
        {
            Tex0 t;
            t.tbp0 = static_cast<uint32_t>(v & 0x3FFFu);
            t.tbw = static_cast<uint8_t>((v >> 14) & 0x3Fu);
            t.psm = static_cast<uint8_t>((v >> 20) & 0x3Fu);
            t.tw = static_cast<uint8_t>((v >> 26) & 0xFu);
            t.th = static_cast<uint8_t>((v >> 30) & 0xFu);
            t.tcc = static_cast<uint8_t>((v >> 34) & 1u);
            t.tfx = static_cast<uint8_t>((v >> 35) & 3u);
            t.cbp = static_cast<uint32_t>((v >> 37) & 0x3FFFu);
            t.cpsm = static_cast<uint8_t>((v >> 51) & 0xFu);
            t.csm = static_cast<uint8_t>((v >> 55) & 1u);
            t.csa = static_cast<uint8_t>((v >> 56) & 0x1Fu);
            t.cld = static_cast<uint8_t>((v >> 61) & 7u);
            return t;
        }
        uint64_t pack() const
        {
            return static_cast<uint64_t>(tbp0) | static_cast<uint64_t>(tbw) << 14 | static_cast<uint64_t>(psm) << 20 |
                   static_cast<uint64_t>(tw) << 26 | static_cast<uint64_t>(th) << 30 | static_cast<uint64_t>(tcc) << 34 |
                   static_cast<uint64_t>(tfx) << 35 | static_cast<uint64_t>(cbp) << 37 | static_cast<uint64_t>(cpsm) << 51 |
                   static_cast<uint64_t>(csm) << 55 | static_cast<uint64_t>(csa) << 56 | static_cast<uint64_t>(cld) << 61;
        }
        uint32_t width() const { return 1u << (tw > 10 ? 10 : tw); }
        uint32_t height() const { return 1u << (th > 10 ? 10 : th); }
        bool indexed() const { return psm == kPsmT8 || psm == kPsmT4; }
    };

    struct Texa
    {
        uint8_t ta0 = 0, aem = 0, ta1 = 0;
        static Texa unpack(uint64_t v)
        {
            return {static_cast<uint8_t>(v & 0xFFu), static_cast<uint8_t>((v >> 15) & 1u), static_cast<uint8_t>((v >> 32) & 0xFFu)};
        }
        uint32_t key() const { return static_cast<uint32_t>(ta0) | static_cast<uint32_t>(aem) << 8 | static_cast<uint32_t>(ta1) << 16; }
    };

    // A 16-bit pixel as the GS widens it for texturing: 5 bits to the top of 8, alpha from TEXA.
    inline uint32_t expand16(uint32_t c, const Texa &texa)
    {
        const uint32_t rgb = ((c & 0x1Fu) << 3) | ((c >> 5) & 0x1Fu) << 11 | ((c >> 10) & 0x1Fu) << 19;
        const uint32_t a = (c & 0x8000u) ? texa.ta1 : ((texa.aem && rgb == 0u) ? 0u : texa.ta0);
        return rgb | a << 24;
    }
    inline uint32_t expand24(uint32_t c, const Texa &texa)
    {
        const uint32_t rgb = c & 0x00FFFFFFu;
        return rgb | static_cast<uint32_t>((texa.aem && rgb == 0u) ? 0u : texa.ta0) << 24;
    }

    // The position of entry i of a CSM1 palette inside its 16 x 16 (8-bit) or 8 x 2 (4-bit) image.
    inline uint32_t clutSlot(uint32_t i) { return (i & ~0x18u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1); }

    // The GS's CLUT buffer: 512 16-bit entries; a 32-bit palette keeps its low halves in [0, 256) and its high halves
    // in [256, 512). A TEX0 (or TEX2) write loads it as CLD says; sampling reads the buffer, not memory, so a palette
    // rewritten in memory takes effect only at the next load.
    class Clut
    {
    public:
        // The write of TEX0/TEX2 `t` (texclut: TEXCLUT, for CSM2). True when the buffer was loaded.
        bool write(const GsMemory &mem, const Tex0 &t, uint64_t texclut = 0);
        // The load itself, whatever CLD says.
        void load(const GsMemory &mem, const Tex0 &t, uint64_t texclut = 0);
        // Entry `index` (a texel's value) as the texture function sees it.
        uint32_t lookup(const Tex0 &t, uint32_t index, const Texa &texa) const;
        // The 16 or 256 colours texture `t` indexes, widened (out: 256 entries).
        void palette(const Tex0 &t, const Texa &texa, uint32_t *out) const;
        uint64_t loads() const { return m_loads; }
        const uint16_t *raw() const { return m_buf; }

    private:
        uint16_t m_buf[512] = {};
        uint32_t m_cbp[2] = {~0u, ~0u};
        uint64_t m_loads = 0;
    };

    // The lower levels of a mipmapped texture: TEX0 is level 0, MIPTBP1 levels 1-3, MIPTBP2 levels 4-6.
    struct MipLevel
    {
        uint32_t tbp = 0, tbw = 0, width = 0, height = 0;
    };
    MipLevel mipLevel(const Tex0 &t, uint64_t miptbp1, uint64_t miptbp2, uint32_t level);

    // Decodes width x height texels at (tbp, tbw) in t's format into out (width * height * 4 bytes). For T8/T4
    // `palette` holds the widened colours (Clut::palette). False for a format outside the list above.
    bool decodeTexels(const GsMemory &mem, const Tex0 &t, uint32_t tbp, uint32_t tbw, uint32_t width, uint32_t height, const Texa &texa,
                      const uint32_t *palette, uint8_t *out);
    // Level 0 of `t` with the palette loaded from memory as a CLD 1 write of t would.
    bool decodeTexture(const GsMemory &mem, const Tex0 &t, const Texa &texa, uint8_t *out);

    // The blocks a width x height buffer at (bp, bw, psm) occupies, appended to `out` (unsorted, may repeat when the
    // buffer folds onto itself).
    template <class Vector> void coveredBlocks(uint8_t psm, uint32_t bp, uint32_t bw, uint32_t width, uint32_t height, Vector &out)
    {
        FormatInfo f;
        if (!gsFormat(psm, f))
            return;
        const LayoutTables &t = layoutTables();
        for (uint32_t y = 0; y < height; y += f.blockH)
            for (uint32_t x = 0; x < width; x += f.blockW)
                out.push_back(static_cast<uint16_t>(gsBlockOf(t, psm, bp, bw, x, y)));
    }
}

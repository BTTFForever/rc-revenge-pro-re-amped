// rc_tex_test: the shadow texture memory and decode of render/tex without the game or a dump (renderer plan P1 step 5,
// RENDERER_FRAME_RECORD.md section 17).
//
//   rc_tex_test [<gs_tables.bin>]
//       Self test: the layout is a permutation of each page; uploads through the GIF walker read back; T4 packing and
//       odd rectangles; CLUT loading by CLD; decode of every format; mip levels; the cache's dirty tracking. With
//       decomp/generated/gs_tables.bin (tools/gs_tables_export.cpp: the runtime's own tables) the layout and every
//       upload are also compared against that independent model.
//   rc_tex_test bbk <tiles.bin> <list.txt> <out.bin>
//       For tools/tex_check.py: uploads a level file's GS memory image as the game does (64 x 64 CT32 tiles from block
//       0x1e00), decodes each texture of the list ("psm tbp tbw width height cbp" a line, numbers in hex) and writes
//       the RGBA texels one after another.
//   rc_tex_test bench
//       Time of a full 64 x 64 texture upload and decode in each format.
#include "../../render/tex/tex_cache.h"
#include "../../render/tex/tex_shadow.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace rc::rd;

namespace
{
    int g_failures = 0, g_checks = 0;
    void check(bool ok, const char *what)
    {
        ++g_checks;
        if (!ok)
        {
            ++g_failures;
            std::printf("FAIL %s\n", what);
        }
    }

    uint64_t g_seed = 0x243F6A8885A308D3ull;
    uint32_t rnd()
    {
        g_seed ^= g_seed << 13;
        g_seed ^= g_seed >> 7;
        g_seed ^= g_seed << 17;
        return static_cast<uint32_t>(g_seed >> 16);
    }

    // The runtime's tables as tools/gs_tables_export.cpp wrote them: per format a pixel index inside a page.
    struct RefFormat
    {
        uint32_t sx = 0, sy = 0;
        std::vector<uint16_t> table;
    };
    std::map<uint32_t, RefFormat> g_ref;
    bool loadRef(const char *path)
    {
        std::FILE *f = std::fopen(path, "rb");
        if (!f)
            return false;
        uint32_t head[4];
        while (std::fread(head, 4, 4, f) == 4)
        {
            RefFormat r;
            r.sx = head[1];
            r.sy = head[2];
            r.table.resize(head[3]);
            if (std::fread(r.table.data(), 2, head[3], f) != head[3])
                break;
            g_ref[head[0]] = std::move(r);
        }
        std::fclose(f);
        return !g_ref.empty();
    }
    // Pixel index (in pixels of the format from the start of memory), the runtime's GSSwizzle::PixelIndex.
    uint32_t refIndex(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
    {
        const RefFormat &r = g_ref[psm];
        const uint32_t perRow = (bw * 64u) >> r.sx;
        const uint32_t page = (bp >> 5) + (y >> r.sy) * perRow + (x >> r.sx);
        const uint32_t entry = ((bp & 31u) << (r.sx + r.sy)) + ((y & ((1u << r.sy) - 1u)) << r.sx) + (x & ((1u << r.sx) - 1u));
        return (page << (r.sx + r.sy)) + r.table[entry];
    }
    // The host's pixel index in the same unit.
    uint32_t myIndex(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
    {
        const LayoutTables &t = layoutTables();
        switch (psm)
        {
        case kPsmCt32: return gsAddr32(t, bp, bw, x, y) >> 2;
        case kPsmCt16: return gsAddr16(t, bp, bw, x, y) >> 1;
        case kPsmCt16s: return gsAddr16s(t, bp, bw, x, y) >> 1;
        case kPsmT8: return gsAddr8(t, bp, bw, x, y);
        default: return gsNibble4(t, bp, bw, x, y);
        }
    }

    // GIF packet builder.
    struct Packet
    {
        std::vector<uint8_t> bytes;
        void u64(uint64_t v)
        {
            for (int i = 0; i < 8; ++i)
                bytes.push_back(static_cast<uint8_t>(v >> (8 * i)));
        }
        void tag(uint32_t nloop, bool eop, uint32_t flg, uint32_t nreg, uint64_t regs)
        {
            u64(nloop | (eop ? 0x8000u : 0u) | static_cast<uint64_t>(flg) << 58 | static_cast<uint64_t>(nreg & 15u) << 60);
            u64(regs);
        }
        void ad(uint8_t reg, uint64_t value)
        {
            u64(value);
            u64(reg);
        }
        // A whole upload: four A+D writes and the image.
        void upload(uint8_t psm, uint32_t dbp, uint32_t dbw, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const std::vector<uint8_t> &data)
        {
            tag(4, false, 0, 1, 0xE);
            ad(kRegBitbltbuf, static_cast<uint64_t>(dbp) << 32 | static_cast<uint64_t>(dbw) << 48 | static_cast<uint64_t>(psm) << 56);
            ad(kRegTrxpos, static_cast<uint64_t>(x) << 32 | static_cast<uint64_t>(y) << 48);
            ad(kRegTrxreg, w | static_cast<uint64_t>(h) << 32);
            ad(kRegTrxdir, 0);
            const uint32_t qwords = static_cast<uint32_t>((data.size() + 15u) / 16u);
            tag(qwords, true, 2, 0, 0);
            bytes.insert(bytes.end(), data.begin(), data.end());
            bytes.resize(bytes.size() + (qwords * 16u - data.size()), 0);
        }
    };

    uint32_t bitsOf(uint8_t psm)
    {
        FormatInfo f;
        gsFormat(psm, f);
        return f.bits;
    }
    std::vector<uint8_t> randomImage(uint8_t psm, uint32_t w, uint32_t h)
    {
        const uint32_t bits = bitsOf(psm);
        std::vector<uint8_t> d((static_cast<size_t>(w) * h * bits + 7u) / 8u);
        for (uint8_t &b : d)
            b = static_cast<uint8_t>(rnd());
        return d;
    }
    uint32_t readPixel(const GsMemory &m, uint8_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
    {
        switch (psm)
        {
        case kPsmCt32: return m.read32(bp, bw, x, y);
        case kPsmCt24: return m.read32(bp, bw, x, y) & 0xFFFFFFu;
        case kPsmCt16: return m.read16(bp, bw, x, y);
        case kPsmCt16s: return m.read16s(bp, bw, x, y);
        case kPsmT8: return m.read8(bp, bw, x, y);
        default: return m.read4(bp, bw, x, y);
        }
    }
    uint32_t sourcePixel(const std::vector<uint8_t> &d, uint8_t psm, size_t i)
    {
        switch (bitsOf(psm))
        {
        case 32: return d[i * 4] | d[i * 4 + 1] << 8 | d[i * 4 + 2] << 16 | static_cast<uint32_t>(d[i * 4 + 3]) << 24;
        case 24: return d[i * 3] | d[i * 3 + 1] << 8 | d[i * 3 + 2] << 16;
        case 16: return d[i * 2] | d[i * 2 + 1] << 8;
        case 8: return d[i];
        default: return (d[i / 2] >> ((i & 1u) * 4u)) & 15u;
        }
    }

    void testLayout()
    {
        // Each format maps a page's pixels onto the page's storage one to one.
        const uint8_t formats[] = {kPsmCt32, kPsmCt16, kPsmCt16s, kPsmT8, kPsmT4};
        const uint32_t pageW[] = {64, 64, 64, 128, 128}, pageH[] = {32, 64, 64, 64, 128};
        for (int f = 0; f < 5; ++f)
        {
            const uint32_t n = pageW[f] * pageH[f];
            std::vector<uint8_t> seen(n, 0);
            bool ok = true;
            for (uint32_t y = 0; y < pageH[f]; ++y)
                for (uint32_t x = 0; x < pageW[f]; ++x)
                {
                    const uint32_t i = myIndex(formats[f], 0, 2, x, y);
                    if (i >= n || seen[i]++)
                        ok = false;
                }
            check(ok, "layout: a page is a permutation");
        }
        if (g_ref.empty())
        {
            std::printf("layout: no gs_tables.bin given, the comparison against the runtime's tables is skipped\n");
            return;
        }
        uint64_t compared = 0, different = 0;
        for (const auto &[psm, ref] : g_ref)
        {
            // Every pixel of a page for every block offset, then random buffers.
            for (uint32_t bp = 0; bp < 32; ++bp)
                for (uint32_t y = 0; y < (1u << ref.sy); ++y)
                    for (uint32_t x = 0; x < (1u << ref.sx); ++x, ++compared)
                        different += myIndex(psm, bp, 2, x, y) != refIndex(psm, bp, 2, x, y);
            for (int i = 0; i < 200000; ++i, ++compared)
            {
                const uint32_t bp = rnd() & 0x3FFFu, bw = 1u + rnd() % 16u, x = rnd() & 0x3FFu, y = rnd() & 0x1FFu;
                different += myIndex(psm, bp, bw, x, y) != refIndex(psm, bp, bw, x, y);
            }
        }
        std::printf("layout: %" PRIu64 " addresses compared with the runtime's tables (%zu formats), %" PRIu64 " different\n", compared,
                    g_ref.size(), different);
        check(different == 0, "layout equals the runtime's tables");
    }

    void testUploads()
    {
        const uint8_t formats[] = {kPsmCt32, kPsmCt24, kPsmCt16, kPsmCt16s, kPsmT8, kPsmT4};
        uint64_t uploads = 0, pixels = 0, bad = 0, refBad = 0, refPixels = 0;
        for (int round = 0; round < 600; ++round)
        {
            const uint8_t psm = formats[round % 6];
            TexShadow shadow;
            // Background noise, so a write that should not happen shows.
            for (uint32_t i = 0; i < kGsBytes; ++i)
                shadow.memory().dataForTests()[i] = static_cast<uint8_t>(rnd());
            std::vector<uint8_t> before(shadow.memory().data(), shadow.memory().data() + kGsBytes);
            uint32_t bw = 1u + rnd() % 8u;
            if (psm == kPsmT8 || psm == kPsmT4)
                bw = (bw + 1u) & ~1u; // these formats' pages are two buffer-width units wide: an odd width folds rows onto each other
            const uint32_t w = 1u + rnd() % (bw * 64u), h = 1u + rnd() % 96u;
            const uint32_t x = (round & 1) ? rnd() % (bw * 64u - w + 1u) : 0u, y = (round & 2) ? rnd() % 64u : 0u;
            const uint32_t bp = 0x1E00u + (rnd() % 0x1000u);
            const std::vector<uint8_t> img = randomImage(psm, w, h);
            Packet p;
            p.upload(psm, bp, bw, x, y, w, h, img);
            // Feed in two pieces, cut at a quadword inside the image when there is one.
            const uint32_t cut = static_cast<uint32_t>(p.bytes.size() > 128u ? 96u + ((rnd() % ((p.bytes.size() - 96u) / 16u)) * 16u) : p.bytes.size());
            shadow.path3(p.bytes.data(), cut);
            shadow.path3(p.bytes.data() + cut, static_cast<uint32_t>(p.bytes.size()) - cut);
            ++uploads;
            // Expected memory: the background with each pixel written at the reference address.
            const bool haveRef = g_ref.count(psm == kPsmCt24 ? 0u : psm) != 0;
            for (uint32_t j = 0; j < h; ++j)
                for (uint32_t i = 0; i < w; ++i, ++pixels)
                {
                    const uint32_t src = sourcePixel(img, psm, static_cast<size_t>(j) * w + i);
                    bad += readPixel(shadow.memory(), psm, bp, bw, x + i, y + j) != src;
                    if (haveRef)
                    {
                        const uint32_t idx = refIndex(psm == kPsmCt24 ? 0u : psm, bp, bw, x + i, y + j);
                        uint8_t *b = before.data();
                        switch (bitsOf(psm))
                        {
                        case 32: std::memcpy(b + ((idx * 4u) & (kGsBytes - 1u)), &src, 4); break;
                        case 24: std::memcpy(b + ((idx * 4u) & (kGsBytes - 1u)), &src, 3); break;
                        case 16: std::memcpy(b + ((idx * 2u) & (kGsBytes - 1u)), &src, 2); break;
                        case 8: b[idx & (kGsBytes - 1u)] = static_cast<uint8_t>(src); break;
                        default:
                        {
                            uint8_t &byte = b[(idx >> 1) & (kGsBytes - 1u)];
                            byte = static_cast<uint8_t>((byte & ~(15u << ((idx & 1u) * 4u))) | src << ((idx & 1u) * 4u));
                        }
                        }
                        ++refPixels;
                    }
                }
            if (haveRef)
                refBad += std::memcmp(before.data(), shadow.memory().data(), kGsBytes) != 0;
            check(!shadow.memory().transferOpen() && shadow.memory().stats().transfers == 1 && shadow.memory().stats().excessBytes < 16u,
                  "upload: one whole transfer");
        }
        std::printf("uploads: %" PRIu64 " rectangles in 6 formats, %" PRIu64 " pixels read back, %" PRIu64 " wrong; %" PRIu64
                    " pixels placed by the runtime's tables, %" PRIu64 " memory images differ\n",
                    uploads, pixels, bad, refPixels, refBad);
        check(bad == 0, "uploads read back");
        check(refBad == 0, "uploads equal the reference model's memory");
    }

    void testWalker()
    {
        struct Log final : GifSink
        {
            std::vector<std::pair<uint8_t, uint64_t>> regs;
            uint32_t imageBytes = 0;
            void reg(uint8_t a, uint64_t v) override { regs.push_back({a, v}); }
            void image(const uint8_t *, uint32_t n) override { imageBytes += n; }
        } log;
        Packet p;
        // PACKED with PRE: PRIM, then ST, RGBAQ, XYZ2 twice (the second with ADC).
        p.tag(2, false, 0, 3, 0x512);
        // PRE and PRIM 0x5b go into the tag's low quadword.
        {
            uint64_t lo;
            std::memcpy(&lo, p.bytes.data(), 8);
            lo |= 1ull << 46 | 0x5Bull << 47;
            std::memcpy(p.bytes.data(), &lo, 8);
        }
        for (int v = 0; v < 2; ++v)
        {
            p.u64(0x3F0000003F800000ull);                    // ST
            p.u64(0x40000000u);                              // Q = 2.0
            p.u64(0x10ull | 0x20ull << 32);                  // R, G
            p.u64(0x30ull | 0x90ull << 32);                  // B, A
            p.u64(0x1234ull | 0x5678ull << 32);              // X, Y
            p.u64(0x00ABCDEFull | (v ? 1ull << 47 : 0ull));  // Z, ADC
        }
        // REGLIST, three registers once (odd: padded).
        p.tag(1, false, 1, 3, 0x016);
        p.u64(0x1111);
        p.u64(0x2222);
        p.u64(0x3333);
        p.u64(0); // padding
        // IMAGE, two quadwords, EOP.
        p.tag(2, true, 2, 0, 0);
        p.bytes.resize(p.bytes.size() + 32, 0x55);
        GifWalker w;
        // In three pieces: the cut falls inside the PACKED data and inside the image.
        w.feed(p.bytes.data(), 48, log);
        w.feed(p.bytes.data() + 48, static_cast<uint32_t>(p.bytes.size()) - 48 - 16, log);
        check(w.tagOpen(), "walker: the image tag is open across packets");
        w.feed(p.bytes.data() + p.bytes.size() - 16, 16, log);
        const std::vector<std::pair<uint8_t, uint64_t>> want = {
            {0x00, 0x5B},
            {0x02, 0x3F0000003F800000ull}, {0x01, 0x4000000090302010ull}, {0x05, 0x00ABCDEF56781234ull},
            {0x02, 0x3F0000003F800000ull}, {0x01, 0x4000000090302010ull}, {0x0D, 0x00ABCDEF56781234ull},
            {0x06, 0x1111}, {0x01, 0x2222}, {0x00, 0x3333},
        };
        check(log.regs == want, "walker: PACKED and REGLIST register writes");
        check(log.imageBytes == 32 && !w.tagOpen() && w.stats().tags == 3 && w.stats().eop == 1 && w.stats().droppedBytes == 0, "walker: image and tags");
    }

    void put32(TexShadow &s, uint32_t bp, uint32_t w, uint32_t h, const std::vector<uint32_t> &px)
    {
        std::vector<uint8_t> d(px.size() * 4u);
        std::memcpy(d.data(), px.data(), d.size());
        Packet p;
        p.upload(kPsmCt32, bp, 1, 0, 0, w, h, d);
        s.path3(p.bytes.data(), static_cast<uint32_t>(p.bytes.size()));
    }

    void testDecode()
    {
        TexShadow s;
        GsMemory &m = s.memory();
        const Texa texa{0x80, 0, 0x00};
        // An 8-bit texture 64 x 32 at block 0x2000 with a 16 x 16 palette at 0x2100.
        std::vector<uint8_t> t8 = randomImage(kPsmT8, 64, 32);
        Packet p;
        p.upload(kPsmT8, 0x2000, 1, 0, 0, 64, 32, t8);
        s.path3(p.bytes.data(), static_cast<uint32_t>(p.bytes.size()));
        std::vector<uint32_t> pal(256);
        for (uint32_t &c : pal)
            c = rnd();
        put32(s, 0x2100, 16, 16, pal);
        Tex0 t;
        t.tbp0 = 0x2000;
        t.tbw = 1;
        t.psm = kPsmT8;
        t.tw = 6;
        t.th = 5;
        t.cbp = 0x2100;
        t.cpsm = kPsmCt32;
        t.cld = 1;
        check(Tex0::unpack(t.pack()).pack() == t.pack() && Tex0::unpack(t.pack()).cbp == 0x2100, "TEX0 packs and unpacks");
        std::vector<uint8_t> out(64 * 32 * 4);
        check(decodeTexture(m, t, texa, out.data()), "decode T8");
        uint32_t bad = 0;
        for (uint32_t i = 0; i < 64u * 32u; ++i)
        {
            uint32_t v;
            std::memcpy(&v, out.data() + i * 4u, 4);
            bad += v != pal[clutSlot(t8[i])]; // the image holds entry i at slot clutSlot(i), and the slot map is its own inverse
        }
        check(bad == 0, "decode T8 through a CSM1 palette");
        // A 4-bit texture 32 x 16 with an 8 x 2 palette.
        std::vector<uint8_t> t4 = randomImage(kPsmT4, 32, 16);
        Packet p4;
        p4.upload(kPsmT4, 0x2200, 1, 0, 0, 32, 16, t4);
        s.path3(p4.bytes.data(), static_cast<uint32_t>(p4.bytes.size()));
        std::vector<uint32_t> pal4(16);
        for (uint32_t &c : pal4)
            c = rnd();
        put32(s, 0x2300, 8, 2, pal4);
        Tex0 u = t;
        u.tbp0 = 0x2200;
        u.psm = kPsmT4;
        u.tw = 5;
        u.th = 4;
        u.cbp = 0x2300;
        check(decodeTexture(m, u, texa, out.data()), "decode T4");
        bad = 0;
        for (uint32_t i = 0; i < 32u * 16u; ++i)
        {
            uint32_t v;
            std::memcpy(&v, out.data() + i * 4u, 4);
            bad += v != pal4[(t4[i / 2] >> ((i & 1u) * 4u)) & 15u];
        }
        check(bad == 0, "decode T4 through an 8 x 2 palette");
        // 16 bit with TEXA: bit 15 clear takes TA0, set takes TA1; AEM makes black transparent.
        std::vector<uint8_t> t16 = randomImage(kPsmCt16, 64, 16);
        t16[0] = t16[1] = 0; // black, bit 15 clear
        Packet p16;
        p16.upload(kPsmCt16, 0x2400, 1, 0, 0, 64, 16, t16);
        s.path3(p16.bytes.data(), static_cast<uint32_t>(p16.bytes.size()));
        Tex0 c16;
        c16.tbp0 = 0x2400;
        c16.tbw = 1;
        c16.psm = kPsmCt16;
        c16.tw = 6;
        c16.th = 4;
        const Texa aem{0x70, 1, 0x20};
        check(decodeTexels(m, c16, 0x2400, 1, 64, 16, aem, nullptr, out.data()), "decode CT16");
        bad = 0;
        for (uint32_t i = 0; i < 64u * 16u; ++i)
        {
            const uint32_t c = t16[i * 2] | t16[i * 2 + 1] << 8;
            const uint32_t rgb = ((c & 31u) << 3) | ((c >> 5) & 31u) << 11 | ((c >> 10) & 31u) << 19;
            const uint32_t a = (c & 0x8000u) ? 0x20u : (rgb == 0u ? 0u : 0x70u);
            uint32_t v;
            std::memcpy(&v, out.data() + i * 4u, 4);
            bad += v != (rgb | a << 24);
        }
        check(bad == 0 && out[3] == 0, "decode CT16 with TEXA");
        // Mip levels: 32 x 32 CT32 with levels 1-3 at their own blocks.
        std::vector<std::vector<uint32_t>> levels;
        const uint32_t tbps[4] = {0x2500, 0x2540, 0x2560, 0x2570};
        for (uint32_t l = 0; l < 4; ++l)
        {
            std::vector<uint32_t> px((32u >> l) * (32u >> l));
            for (uint32_t &c : px)
                c = rnd();
            put32(s, tbps[l], 32u >> l, 32u >> l, px);
            levels.push_back(px);
        }
        Tex0 mt;
        mt.tbp0 = tbps[0];
        mt.tbw = 1;
        mt.psm = kPsmCt32;
        mt.tw = mt.th = 5;
        const uint64_t miptbp1 = static_cast<uint64_t>(tbps[1]) | 1ull << 14 | static_cast<uint64_t>(tbps[2]) << 20 | 1ull << 34 |
                                 static_cast<uint64_t>(tbps[3]) << 40 | 1ull << 54;
        TextureCache cache(m);
        bad = 0;
        for (uint32_t l = 0; l < 4; ++l)
        {
            const MipLevel place = mipLevel(mt, miptbp1, 0, l);
            const CachedTexture &e = cache.get(mt, place, texa);
            bad += !e.valid || e.key.width != (32u >> l) || std::memcmp(e.rgba.data(), levels[l].data(), levels[l].size() * 4u) != 0;
        }
        check(bad == 0, "mip levels decode from MIPTBP1");

        // CLUT buffer and CLD.
        Clut clut;
        Tex0 c = t;
        c.cld = 0;
        check(!clut.write(m, c) && clut.loads() == 0, "CLD 0 keeps the buffer");
        c.cld = 1;
        check(clut.write(m, c) && clut.lookup(c, 0x08, texa) == pal[0x10] && clut.lookup(c, 0xFF, texa) == pal[0xFF], "CLD 1 loads");
        // Rewrite the palette in memory: sampling still sees the loaded one until the next load.
        std::vector<uint32_t> pal2(256);
        for (uint32_t &v : pal2)
            v = rnd();
        put32(s, 0x2100, 16, 16, pal2);
        check(clut.lookup(c, 0x01, texa) == pal[0x01], "the buffer keeps its colours when memory changes");
        c.cld = 2;
        check(clut.write(m, c) && clut.lookup(c, 0x01, texa) == pal2[0x01], "CLD 2 loads and remembers CBP0");
        put32(s, 0x2100, 16, 16, pal);
        c.cld = 4;
        check(!clut.write(m, c) && clut.lookup(c, 0x01, texa) == pal2[0x01], "CLD 4 with CBP0 unchanged does not load");
        c.cld = 5;
        check(clut.write(m, c) && clut.lookup(c, 0x01, texa) == pal[0x01], "CLD 5 with a new CBP1 loads");
        check(!clut.write(m, c), "CLD 5 with CBP1 unchanged does not load");
        // A 4-bit palette at CSA 2 lands at entries 32-47 and leaves the others.
        Tex0 c4 = u;
        c4.csa = 2;
        const uint32_t keep = clut.lookup(c, 0x30, texa);
        clut.load(m, c4);
        check(clut.lookup(c4, 5, texa) == pal4[5] && clut.lookup(c, 0x25, texa) == pal4[5] && clut.lookup(c, 0x30, texa) == keep,
              "a 4-bit palette loads at CSA * 16");

        // The cache: dirty tracking by block.
        TextureCache tc(m);
        const CachedTexture &e8 = tc.get(t, texa);
        check(e8.generation == 1 && e8.valid, "cache: first decode");
        check(tc.get(t, texa).generation == 1 && tc.stats().quickHits == 1, "cache: unchanged memory is a quick hit");
        put32(s, 0x3000, 8, 8, std::vector<uint32_t>(64, 0x12345678u)); // elsewhere
        check(tc.get(t, texa).generation == 1 && tc.stats().validations == 1 && tc.stats().redecodes == 0, "cache: a write elsewhere keeps the texture");
        Packet again;
        std::vector<uint8_t> t8b = randomImage(kPsmT8, 16, 16);
        again.upload(kPsmT8, 0x2000, 1, 48, 16, 16, 16, t8b); // one block of the texture
        s.path3(again.bytes.data(), static_cast<uint32_t>(again.bytes.size()));
        const CachedTexture &e8b = tc.get(t, texa);
        uint32_t v;
        std::memcpy(&v, e8b.rgba.data() + (16u * 64u + 48u) * 4u, 4);
        check(e8b.generation == 2 && v == pal[clutSlot(t8b[0])], "cache: a write into the texture decodes it again");
        put32(s, 0x2100, 16, 16, pal2); // the palette
        std::memcpy(&v, tc.get(t, texa).rgba.data(), 4);
        check(tc.get(t, texa).generation == 3 && v == pal2[clutSlot(t8[0])], "cache: a new palette decodes it again");
        // With a reader's CLUT buffer the buffer's colours decide.
        Clut held;
        held.load(m, t);
        put32(s, 0x2100, 16, 16, pal);
        std::memcpy(&v, tc.get(t, texa, &held).rgba.data(), 4);
        check(v == pal2[clutSlot(t8[0])], "cache: the CLUT buffer's colours are used while it is not reloaded");
        put32(s, 0x2100, 16, 16, pal2);
        check(tc.refreshAll() == 1 && tc.refreshAll() == 0 && tc.get(t, texa).generation == 5, "cache: refreshAll decodes what changed");
        // Alpha summary.
        std::vector<uint32_t> ramp(64);
        for (uint32_t i = 0; i < 64; ++i)
            ramp[i] = i << 26; // alpha 0, 4, ... 252
        put32(s, 0x3100, 8, 8, ramp);
        Tex0 at;
        at.tbp0 = 0x3100;
        at.tbw = 1;
        at.psm = kPsmCt32;
        at.tw = at.th = 3;
        const CachedTexture &ea = tc.get(at, texa);
        check(ea.alphaMin == 0 && ea.alphaMax == 252 && ea.alphaAbove80 == 31 && ea.alphaAt80 == 1 && ea.alphaZero == 1, "cache: alpha summary");
    }

    int bbk(const char *tilesPath, const char *listPath, const char *outPath)
    {
        std::FILE *f = std::fopen(tilesPath, "rb");
        if (!f)
            return 2;
        std::vector<uint8_t> tiles;
        uint8_t buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
            tiles.insert(tiles.end(), buf, buf + n);
        std::fclose(f);
        TexShadow shadow;
        shadow.memory().clear();
        // BBLoad: one 64 x 64 CT32 tile per 0x4000 bytes, from block 0x1e00 up to the end of memory.
        uint32_t tile = 0;
        for (size_t at = 0; at + 0x4000u <= tiles.size(); at += 0x4000u, ++tile)
        {
            const uint32_t address = 0x1E0000u + tile * 0x4000u;
            if (address >= kGsBytes)
                break;
            Packet p;
            p.upload(kPsmCt32, address >> 8, 1, 0, 0, 64, 64, std::vector<uint8_t>(tiles.begin() + at, tiles.begin() + at + 0x4000));
            shadow.path3(p.bytes.data(), static_cast<uint32_t>(p.bytes.size()));
        }
        std::FILE *list = std::fopen(listPath, "r");
        std::FILE *out = std::fopen(outPath, "wb");
        if (!list || !out)
            return 2;
        unsigned psm, tbp, tbw, w, h, cbp;
        uint32_t count = 0;
        const Texa texa{0x80, 0, 0};
        std::vector<uint8_t> rgba;
        while (std::fscanf(list, "%x %x %x %x %x %x", &psm, &tbp, &tbw, &w, &h, &cbp) == 6)
        {
            Tex0 t;
            t.psm = static_cast<uint8_t>(psm);
            t.tbp0 = tbp;
            t.tbw = static_cast<uint8_t>(tbw);
            t.cbp = cbp;
            t.cpsm = kPsmCt32;
            rgba.assign(static_cast<size_t>(w) * h * 4u, 0);
            uint32_t palette[256];
            if (t.indexed())
            {
                Clut clut;
                clut.load(shadow.memory(), t);
                clut.palette(t, texa, palette);
            }
            if (!decodeTexels(shadow.memory(), t, tbp, tbw, w, h, texa, palette, rgba.data()))
                std::fprintf(stderr, "format 0x%x is not decoded\n", psm);
            std::fwrite(rgba.data(), 1, rgba.size(), out);
            ++count;
        }
        std::fclose(list);
        std::fclose(out);
        std::printf("%u tiles uploaded, %u textures decoded\n", tile, count);
        return 0;
    }

    int bench()
    {
        const uint8_t formats[] = {kPsmCt32, kPsmCt16, kPsmT8, kPsmT4};
        const char *names[] = {"CT32", "CT16", "T8", "T4"};
        for (int f = 0; f < 4; ++f)
        {
            TexShadow s;
            const std::vector<uint8_t> img = randomImage(formats[f], 64, 64);
            Packet p;
            p.upload(formats[f], 0x2000, 1, 0, 0, 64, 64, img);
            Tex0 t;
            t.tbp0 = 0x2000;
            t.tbw = 1;
            t.psm = formats[f];
            t.tw = t.th = 6;
            t.cbp = 0x3000;
            std::vector<uint8_t> out(64 * 64 * 4);
            const int rounds = 2000;
            auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < rounds; ++i)
                s.path3(p.bytes.data(), static_cast<uint32_t>(p.bytes.size()));
            auto t1 = std::chrono::steady_clock::now();
            for (int i = 0; i < rounds; ++i)
                decodeTexture(s.memory(), t, Texa{0x80, 0, 0}, out.data());
            auto t2 = std::chrono::steady_clock::now();
            const double up = std::chrono::duration<double, std::micro>(t1 - t0).count() / rounds;
            const double de = std::chrono::duration<double, std::micro>(t2 - t1).count() / rounds;
            std::printf("%-4s 64 x 64: upload %6.1f us (%5.1f ns a pixel, %zu bytes), decode %6.1f us (%5.1f ns a texel)\n", names[f], up,
                        up * 1000.0 / 4096.0, img.size(), de, de * 1000.0 / 4096.0);
        }
        return 0;
    }
}

int main(int argc, char **argv)
{
    if (argc >= 5 && std::strcmp(argv[1], "bbk") == 0)
        return bbk(argv[2], argv[3], argv[4]);
    if (argc >= 2 && std::strcmp(argv[1], "bench") == 0)
        return bench();
    if (argc >= 2 && !loadRef(argv[1]))
    {
        std::fprintf(stderr, "%s: not a gs_tables.bin\n", argv[1]);
        return 2;
    }
    testLayout();
    testWalker();
    testUploads();
    testDecode();
    std::printf("%d checks, %d failed\n%s\n", g_checks, g_failures, g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}

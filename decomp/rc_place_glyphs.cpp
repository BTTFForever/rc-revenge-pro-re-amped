// Place numbers from 9th on in the style of the level's own (rc_cars.h, NOTES.md "Car limits").
#include "rc_cars.h"
#include "rc_decomp.h"

#include "ps2_runtime.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/ps2_gs_memory.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace rc::cars {

namespace {

template <typename T>
T &at(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

uint32_t clutSwizzle(uint32_t i) { return (i & 0xe7u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1); }

struct Decoded
{
    int w = 0, h = 0, fmt = 0;
    std::vector<uint8_t> index;  // w*h (paletted formats)
    std::vector<uint32_t> rgba;  // w*h
    std::vector<uint32_t> clut;  // 256 or 16
};

// A texture as the GS holds it (its first level), from a copy of GS memory.
Decoded decode(std::vector<uint8_t> &vram, uint32_t texture)
{
    Decoded d;
    const _sGfxTexture &t = at<_sGfxTexture>(texture);
    d.w = t.wd;
    d.h = t.ht;
    d.fmt = t.fmt;
    const uint32_t tbp = static_cast<uint32_t>(t.idxList[0]), tbw = std::max(1, t.wd / 64);
    const bool p8 = t.fmt == 0x13, p4 = t.fmt == 0x14;
    if (p8 || p4)
    {
        const uint32_t n = p8 ? 256u : 16u;
        for (uint32_t i = 0; i < n; ++i)
        {
            const uint32_t s = p8 ? clutSwizzle(i) : i;
            const uint32_t cx = p8 ? (s & 15u) : (s & 7u), cy = p8 ? (s >> 4) : (s >> 3);
            d.clut.push_back(GSMem::ReadCT32(vram.data(), static_cast<uint32_t>(t.clutVramAddr), 1u, cx, cy));
        }
    }
    for (int y = 0; y < d.h; ++y)
        for (int x = 0; x < d.w; ++x)
        {
            uint32_t v;
            if (p8 || p4)
            {
                const uint32_t i = p8 ? GSMem::ReadP8(vram.data(), tbp, tbw, x, y) : GSMem::ReadP4(vram.data(), tbp, tbw, x, y);
                d.index.push_back(static_cast<uint8_t>(i));
                v = d.clut[i];
            }
            else if (t.fmt == 0)
                v = GSMem::ReadCT32(vram.data(), tbp, tbw, x, y);
            else
                v = GSMem::ReadCT16(vram.data(), tbp, tbw, x, y);
            d.rgba.push_back(v);
        }
    return d;
}

void writePam(const std::string &path, const Decoded &d)
{
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (!f)
        return;
    std::fprintf(f, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", d.w, d.h);
    for (uint32_t v : d.rgba)
    {
        // The GS's alpha: 0x80 is opaque.
        const uint8_t px[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
                               static_cast<uint8_t>(std::min<uint32_t>(255u, (v >> 24) * 2u))};
        std::fwrite(px, 1, 4, f);
    }
    std::fclose(f);
}

} // namespace

// RC_PLACE_DUMP=<dir>: the level's place sprites (1-8) as PAM images, with their formats in the log (once a level).
void dumpPlaceSprites(uint32_t res)
{
    static uint32_t dumped = 0;
    const char *dir = std::getenv("RC_PLACE_DUMP");
    if (!dir || !g_runtime || dumped == res)
        return;
    dumped = res;
    std::vector<uint8_t> vram;
    g_runtime->gs().copyVram(vram);
    static std::once_flag tables;
    std::call_once(tables, [] { GSMem::InitLookupTables(); });
    for (int place = 0; place < 8; ++place)
    {
        const uint32_t sprite = at<uint32_t>(res + 0x24u + 4u * static_cast<uint32_t>(place));
        if (!sprite)
            continue;
        const uint32_t texture = at<uint32_t>(sprite);
        const _sGfxTexture &t = at<_sGfxTexture>(texture);
        std::fprintf(stderr, "[places] level %u place %d: sprite 0x%x %ux%u, texture 0x%x %dx%d fmt 0x%x flags 0x%x tbp 0x%x clut 0x%x\n",
                     G(mainVarG).level, place + 1, sprite, at<uint8_t>(sprite + 6), at<uint8_t>(sprite + 7), texture, t.wd, t.ht, t.fmt,
                     t.flags, t.idxList[0], t.clutVramAddr);
        writePam(std::string(dir) + "/level" + std::to_string(G(mainVarG).level) + "_place" + std::to_string(place + 1) + ".pam", decode(vram, texture));
    }
}

} // namespace rc::cars

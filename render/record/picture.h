#pragma once

// The reference picture (RENDERER_FRAME_RECORD.md section 3.5): a width x height rectangle at a frame buffer page,
// read from GS local memory through the PSMCT32 layout, 4 bytes a pixel R, G, B, A exactly as stored (alpha raw,
// 0x80 = opaque). Used live (capture.cpp, exact mode) and off-line (rc_replay emu). No dependencies; the tables are
// the GS's (the runtime's GSPSMCT32::addrPSMCT32 computes the same).

#include <cstdint>
#include <cstring>

namespace rc::rec
{
    inline uint32_t ct32Address(uint32_t block, uint32_t width, uint32_t x, uint32_t y)
    {
        static constexpr uint8_t blockTable[4][8] = {
            {0, 1, 4, 5, 16, 17, 20, 21},
            {2, 3, 6, 7, 18, 19, 22, 23},
            {8, 9, 12, 13, 24, 25, 28, 29},
            {10, 11, 14, 15, 26, 27, 30, 31},
        };
        static constexpr uint8_t columnTable[8][8] = {
            {0, 1, 4, 5, 8, 9, 12, 13},     {2, 3, 6, 7, 10, 11, 14, 15},   {16, 17, 20, 21, 24, 25, 28, 29},
            {18, 19, 22, 23, 26, 27, 30, 31}, {32, 33, 36, 37, 40, 41, 44, 45}, {34, 35, 38, 39, 42, 43, 46, 47},
            {48, 49, 52, 53, 56, 57, 60, 61}, {50, 51, 54, 55, 58, 59, 62, 63},
        };
        const uint32_t pagesPerRow = width ? width : 1u;
        const uint32_t page = (block >> 5u) + (y >> 5u) * pagesPerRow + (x >> 6u);
        const uint32_t blockId = (block & 0x1Fu) + blockTable[(y >> 3u) & 3u][(x >> 3u) & 7u];
        return (page << 13u) + ((blockId >> 5u) << 13u) + (blockId & 0x1Fu) * 256u + static_cast<uint32_t>(columnTable[y & 7u][x & 7u]) * 4u;
    }

    // vram: 4 MB of GS memory as the runtime stores it; fbp: DISPFB2.FBP (2048-word pages); fbw: buffer width in 64s
    // (10 for RC). out: width * height * 4 bytes.
    inline void readPicture(const uint8_t *vram, uint32_t vramBytes, uint32_t fbp, uint32_t fbw, uint32_t width, uint32_t height, uint8_t *out)
    {
        const uint32_t mask = vramBytes - 1u; // a power of two
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
                std::memcpy(out + (static_cast<size_t>(y) * width + x) * 4u, vram + (ct32Address(fbp * 32u, fbw, x, y) & mask), 4);
    }
}

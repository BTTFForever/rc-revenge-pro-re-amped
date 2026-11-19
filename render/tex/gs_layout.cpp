#include "gs_layout.h"

namespace rc::rd
{
    namespace
    {
        // Blocks inside a page. 32-bit (and 8-bit) formats: 8 blocks a row, Z-ordered in 2 x 2 groups. 16-bit (and
        // 4-bit) formats: 4 blocks a row, the same order turned on its side.
        constexpr uint8_t kBlock32[4][8] = {
            {0, 1, 4, 5, 16, 17, 20, 21},
            {2, 3, 6, 7, 18, 19, 22, 23},
            {8, 9, 12, 13, 24, 25, 28, 29},
            {10, 11, 14, 15, 26, 27, 30, 31},
        };
        constexpr uint8_t kBlock16[8][4] = {
            {0, 2, 8, 10},   {1, 3, 9, 11},   {4, 6, 12, 14},   {5, 7, 13, 15},
            {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31},
        };
        constexpr uint8_t kBlock16s[8][4] = {
            {0, 2, 16, 18}, {1, 3, 17, 19}, {8, 10, 24, 26},  {9, 11, 25, 27},
            {4, 6, 20, 22}, {5, 7, 21, 23}, {12, 14, 28, 30}, {13, 15, 29, 31},
        };

        LayoutTables build()
        {
            LayoutTables t{};
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 8; ++x)
                    t.block32[y][x] = kBlock32[y][x];
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 4; ++x)
                {
                    t.block16[y][x] = kBlock16[y][x];
                    t.block16s[y][x] = kBlock16s[y][x];
                }
            // A block is four columns of 64 bytes, a column two pixel rows (32 and 16 bit) or four (8 and 4 bit).
            // 32 bit: a column holds 16 words, two rows of 8 interleaved in pairs.
            for (uint32_t y = 0; y < 8; ++y)
                for (uint32_t x = 0; x < 8; ++x)
                    t.column32[y][x] = static_cast<uint8_t>((y >> 1) * 16u + (x >> 1) * 4u + (y & 1u) * 2u + (x & 1u));
            // 16 bit: a column holds 32 words, two rows of 16; the left and right halves of a row interleave.
            for (uint32_t y = 0; y < 8; ++y)
                for (uint32_t x = 0; x < 16; ++x)
                    t.column16[y][x] = static_cast<uint8_t>((y >> 1) * 32u + ((x & 7u) >> 1) * 8u + (y & 1u) * 4u + (x & 1u) * 2u + (x >> 3));
            // 8 bit: a column holds four rows of 16 bytes. Rows 2 and 3 of a column go into the odd bytes with the two
            // halves of each 8-pixel group swapped, and odd columns swap where even ones don't.
            for (uint32_t y = 0; y < 16; ++y)
                for (uint32_t x = 0; x < 16; ++x)
                {
                    const uint32_t c = y >> 2, r = y & 3u;
                    const uint32_t xx = (x & 7u) ^ ((((r >> 1) ^ c) & 1u) ? 4u : 0u);
                    t.column8[y][x] = static_cast<uint8_t>(c * 64u + (xx >> 1) * 16u + (xx & 1u) * 4u + (r & 1u) * 8u + (x >> 3) * 2u + (r >> 1));
                }
            // 4 bit: a column holds four rows of 32 nibbles, arranged as the 8-bit bytes are with four 8-pixel groups.
            for (uint32_t y = 0; y < 16; ++y)
                for (uint32_t x = 0; x < 32; ++x)
                {
                    const uint32_t c = y >> 2, r = y & 3u;
                    const uint32_t xx = (x & 7u) ^ ((((r >> 1) ^ c) & 1u) ? 4u : 0u);
                    t.column4[y][x] = static_cast<uint16_t>(c * 128u + (xx >> 1) * 32u + (xx & 1u) * 8u + (r & 1u) * 16u + (x >> 3) * 2u + (r >> 1));
                }
            return t;
        }

        LayoutTables &tables()
        {
            static LayoutTables t = build();
            return t;
        }
    }

    const LayoutTables &layoutTables() { return tables(); }
    LayoutTables &layoutTablesForTests() { return tables(); }
}

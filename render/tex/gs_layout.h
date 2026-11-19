#pragma once

// GS local memory layout (renderer plan P1 step 5; RENDERER_FRAME_RECORD.md section 17): where a pixel of a buffer
// (base block, width in 64-pixel units, format) lives in the 4 MB of GS memory. Written from the GS's block and column
// arrangement, not taken from the runtime's tables: the shadow of texture memory is checked against the emulated GS's
// memory bit for bit, which only means something if the two were made independently (rc_tex_test compares them too).
//
// Memory is 512 pages of 8,192 bytes, a page 32 blocks of 256 bytes, a block 4 columns of 64 bytes.
//   format   page (pixels)  block     a pixel
//   CT32/24  64 x 32        8 x 8     32 bits (CT24: the low 24)
//   CT16(S)  64 x 64        16 x 8    16 bits
//   T8       128 x 64       16 x 16   8 bits
//   T4       128 x 128      32 x 16   4 bits
// No dependencies, C++17.

#include <cstdint>

namespace rc::rd
{
    constexpr uint32_t kGsBytes = 4u * 1024u * 1024u;
    constexpr uint32_t kGsBlockBytes = 256u;
    constexpr uint32_t kGsBlocks = kGsBytes / kGsBlockBytes; // 16,384
    constexpr uint32_t kGsPageBytes = 8192u;
    constexpr uint32_t kGsPages = kGsBytes / kGsPageBytes;   // 512

    enum : uint8_t
    {
        kPsmCt32 = 0x00,
        kPsmCt24 = 0x01,
        kPsmCt16 = 0x02,
        kPsmCt16s = 0x0A,
        kPsmT8 = 0x13,
        kPsmT4 = 0x14,
    };

    struct LayoutTables
    {
        uint8_t block32[4][8];   // block number inside a page by (y / 8, x / 8); T8's is the same by (y / 16, x / 16)
        uint8_t block16[8][4];   // by (y / 8, x / 16); T4's is the same by (y / 16, x / 32)
        uint8_t block16s[8][4];
        uint8_t column32[8][8];    // 32-bit word inside a block
        uint8_t column16[8][16];   // 16-bit word inside a block
        uint8_t column8[16][16];   // byte inside a block
        uint16_t column4[16][32];  // nibble inside a block
    };

    // The tables in use. layoutTablesForTests() returns the same object writable, for planting a layout error.
    const LayoutTables &layoutTables();
    LayoutTables &layoutTablesForTests();

    // Byte address of a pixel (not yet wrapped to 4 MB). bw: the buffer width in 64-pixel units (TBW, DBW, FBW).
    inline uint32_t gsAddr32(const LayoutTables &t, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
    {
        const uint32_t page = (bp >> 5) + (y >> 5) * bw + (x >> 6);
        const uint32_t block = (bp & 31u) + t.block32[(y >> 3) & 3u][(x >> 3) & 7u];
        return (page << 13) + (block << 8) + (static_cast<uint32_t>(t.column32[y & 7u][x & 7u]) << 2);
    }
    inline uint32_t gsAddr16(const LayoutTables &t, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
    {
        const uint32_t page = (bp >> 5) + (y >> 6) * bw + (x >> 6);
        const uint32_t block = (bp & 31u) + t.block16[(y >> 3) & 7u][(x >> 4) & 3u];
        return (page << 13) + (block << 8) + (static_cast<uint32_t>(t.column16[y & 7u][x & 15u]) << 1);
    }
    inline uint32_t gsAddr16s(const LayoutTables &t, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
    {
        const uint32_t page = (bp >> 5) + (y >> 6) * bw + (x >> 6);
        const uint32_t block = (bp & 31u) + t.block16s[(y >> 3) & 7u][(x >> 4) & 3u];
        return (page << 13) + (block << 8) + (static_cast<uint32_t>(t.column16[y & 7u][x & 15u]) << 1);
    }
    inline uint32_t gsAddr8(const LayoutTables &t, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
    {
        const uint32_t page = (bp >> 5) + (y >> 6) * (bw >> 1) + (x >> 7);
        const uint32_t block = (bp & 31u) + t.block32[(y >> 4) & 3u][(x >> 4) & 7u];
        return (page << 13) + (block << 8) + t.column8[y & 15u][x & 15u];
    }
    // Nibble address: byte = n >> 1, the low nibble when n is even.
    inline uint32_t gsNibble4(const LayoutTables &t, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
    {
        const uint32_t page = (bp >> 5) + (y >> 7) * (bw >> 1) + (x >> 7);
        const uint32_t block = (bp & 31u) + t.block16[(y >> 4) & 7u][(x >> 5) & 3u];
        return (page << 14) + (block << 9) + t.column4[y & 15u][x & 31u];
    }

    // A format's block size in pixels and bits a pixel; false for a format the renderer does not handle.
    struct FormatInfo
    {
        uint8_t blockW, blockH, bits;
    };
    inline bool gsFormat(uint8_t psm, FormatInfo &f)
    {
        switch (psm)
        {
        case kPsmCt32: f = {8, 8, 32}; return true;
        case kPsmCt24: f = {8, 8, 24}; return true;
        case kPsmCt16:
        case kPsmCt16s: f = {16, 8, 16}; return true;
        case kPsmT8: f = {16, 16, 8}; return true;
        case kPsmT4: f = {32, 16, 4}; return true;
        default: return false;
        }
    }

    // The block (0..16383, wrapped) a pixel lies in.
    inline uint32_t gsBlockOf(const LayoutTables &t, uint8_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
    {
        switch (psm)
        {
        case kPsmCt32:
        case kPsmCt24: return (gsAddr32(t, bp, bw, x, y) >> 8) & (kGsBlocks - 1u);
        case kPsmCt16: return (gsAddr16(t, bp, bw, x, y) >> 8) & (kGsBlocks - 1u);
        case kPsmCt16s: return (gsAddr16s(t, bp, bw, x, y) >> 8) & (kGsBlocks - 1u);
        case kPsmT8: return (gsAddr8(t, bp, bw, x, y) >> 8) & (kGsBlocks - 1u);
        case kPsmT4: return (gsNibble4(t, bp, bw, x, y) >> 9) & (kGsBlocks - 1u);
        default: return 0;
        }
    }
}

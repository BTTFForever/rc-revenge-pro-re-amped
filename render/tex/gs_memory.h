#pragma once

// The host's shadow of GS local memory (renderer plan P1 step 5; RENDERER_FRAME_RECORD.md section 17). It holds what
// host-to-local transfers put there, in the GS's own layout, so a texture can be read back in any format (a level's
// texture data is a raw image of GS memory uploaded as 32-bit tiles and sampled as 4-, 8-, 16- and 32-bit textures).
// Draws never write it: frame and Z memory (blocks below 0x1e00) hold only what was uploaded there.
//
// Dirty tracking: every transfer gets a serial number, and each 256-byte block remembers the serial of the last
// transfer that wrote it. A texture cache keeps the serial it decoded at and re-decodes when one of its blocks is
// newer (tex_cache.h); the same numbers tell a GPU uploader which host textures to refresh.
// No dependencies beyond gs_layout, C++17.

#include "gs_layout.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace rc::rd
{
    enum : uint8_t
    {
        kRegBitbltbuf = 0x50,
        kRegTrxpos = 0x51,
        kRegTrxreg = 0x52,
        kRegTrxdir = 0x53,
        kRegHwreg = 0x54,
    };

    class GsMemory
    {
    public:
        struct Stats
        {
            uint64_t transfers = 0;        // TRXDIR writes starting a host-to-local transfer
            uint64_t pixels = 0, bytes = 0;
            uint64_t byPsm[64] = {};       // transfers by destination format
            uint64_t bytesByPsm[64] = {};
            uint64_t blockMarks = 0;       // blocks written, counted once per transfer
            uint64_t intoFrame = 0;        // transfers whose first pixel lies below the texture area
            uint64_t unfinished = 0;       // transfers replaced by the next TRXDIR before all their pixels arrived
            uint64_t excessBytes = 0;      // image data with no transfer open
            uint64_t unsupported = 0;      // a destination format outside gs_layout's, or a zero-sized rectangle
            uint64_t localToHost = 0, localToLocal = 0; // never seen in RC (survey): counted, not performed
        };

        GsMemory();

        void clear();                       // all zero, every block marked written
        void load(const uint8_t *image);    // 4 MB as the GS holds it (a keyframe's GSVR); every block marked written

        const uint8_t *data() const { return m_mem.get(); }
        uint8_t *dataForTests() { return m_mem.get(); }

        // A GS register write. Only BITBLTBUF, TRXPOS, TRXREG, TRXDIR and HWREG matter here; others are ignored.
        void writeRegister(uint8_t addr, uint64_t value);
        // IMAGE data of the GIF (or HWREG): the open transfer's next pixels.
        void imageData(const uint8_t *data, size_t bytes);

        uint64_t bitbltbuf() const { return m_bitbltbuf; }
        uint64_t trxpos() const { return m_trxpos; }
        uint64_t trxreg() const { return m_trxreg; }
        uint64_t trxdir() const { return m_trxdir; }
        void setTransferRegisters(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, uint64_t trxdir); // from a keyframe
        bool transferOpen() const { return m_open; }

        // Dirty tracking.
        uint64_t serial() const { return m_serial; }                        // of the newest transfer (or load)
        uint64_t blockSerial(uint32_t block) const { return m_blockSerial[block & (kGsBlocks - 1u)]; }
        uint64_t pageSerial(uint32_t page) const { return m_pageSerial[page & (kGsPages - 1u)]; }

        // Pixel reads through the layout.
        uint32_t read32(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) const;
        uint32_t read16(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) const;
        uint32_t read16s(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) const;
        uint32_t read8(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) const;
        uint32_t read4(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) const;

        const Stats &stats() const { return m_stats; }

        static constexpr uint32_t kTextureFirstBlock = 0x1E00u; // RC: frame and Z memory end here (survey)

    private:
        void begin();
        void mark(uint32_t block);

        std::unique_ptr<uint8_t[]> m_mem;
        std::unique_ptr<uint64_t[]> m_blockSerial;
        uint64_t m_pageSerial[kGsPages] = {};
        uint64_t m_serial = 0;
        uint64_t m_bitbltbuf = 0, m_trxpos = 0, m_trxreg = 0, m_trxdir = 3;

        // The open transfer.
        bool m_open = false;
        uint32_t m_dbp = 0, m_dbw = 1, m_dsax = 0, m_dsay = 0, m_rrw = 0, m_rrh = 0;
        uint8_t m_dpsm = 0;
        uint32_t m_done = 0, m_total = 0;
        uint32_t m_lastBlock = ~0u;
        uint8_t m_carry[4] = {};
        uint32_t m_carryBytes = 0;
        Stats m_stats;
    };
}

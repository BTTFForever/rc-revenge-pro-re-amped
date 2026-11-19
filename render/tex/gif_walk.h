#pragma once

// A GIF packet walker (renderer plan P1 step 5): GIF tags in PACKED, REGLIST and IMAGE form turned into GS register
// writes and image data, in order. PACKED vertex and colour data are delivered in register form (what the GS's own
// register holds after the write), so a sink sees one kind of write whatever the packet's form. A tag may span feed()
// calls, as it may span the GIF's packets. Step 5 uses it for the transfers; step 7's reader can build on it.
// No dependencies, C++17.

#include <cstdint>

namespace rc::rd
{
    struct GifSink
    {
        // addr: the GS register address (0x00-0x62). Vertex kicks arrive as 0x04 XYZF2 / 0x05 XYZ2, a PACKED vertex
        // with its ADC bit set as 0x0C XYZF3 / 0x0D XYZ3.
        virtual void reg(uint8_t addr, uint64_t value) = 0;
        virtual void image(const uint8_t *data, uint32_t bytes) = 0;
        virtual ~GifSink() = default;
    };

    class GifWalker
    {
    public:
        struct Stats
        {
            uint64_t tags = 0, packed = 0, reglist = 0, image = 0, imageBytes = 0, regWrites = 0;
            uint64_t eop = 0;
            uint64_t preOutsidePacked = 0; // tags with PRE set in REGLIST or IMAGE form (the GS ignores PRE there)
            uint64_t droppedBytes = 0;     // a packet's tail too short for the next item or tag
        };

        void reset() { *this = GifWalker(); }
        // One packet (a multiple of 16 bytes in practice). Continues the open tag, then reads tags to the end.
        void feed(const uint8_t *data, uint32_t bytes, GifSink &sink);
        bool tagOpen() const { return m_open; }
        const Stats &stats() const { return m_stats; }

    private:
        bool resume(const uint8_t *data, uint32_t bytes, uint32_t &at, GifSink &sink);
        void packed(uint8_t descriptor, uint64_t lo, uint64_t hi, GifSink &sink);

        bool m_open = false;
        uint8_t m_flg = 0, m_nreg = 0, m_regIndex = 0;
        uint8_t m_regs[16] = {};
        uint32_t m_loops = 0, m_imageLeft = 0;
        bool m_oddReglist = false;
        uint32_t m_q = 0x3F800000u; // the Q a PACKED ST left for the next RGBAQ (1.0 at each tag)
        Stats m_stats;
    };
}

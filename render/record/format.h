#pragma once

// The frame record's file format (RENDERER_FRAME_RECORD.md section 6.4; normative here). No dependencies, C++17.
//
// Little-endian. A file is a sequence of chunks (ChunkHeader + payload, padded to 8 bytes): RCFR (FileHeader, first,
// never compressed), KEYF (a keyframe: KeyframeHeader then that many nested state chunks), CYCL (one display cycle's
// records, one zlib stream), REFI (a reference picture), INDX (the index, at close), ENDF (empty, last). A file without
// ENDF was cut short; a reader scans it up to its last whole chunk. Unknown chunk ids and record types are skipped by
// their sizes.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rc::rec
{
    constexpr uint32_t kFormatVersion = 1u;
    constexpr uint32_t kPageBytes = 4096u;
    constexpr uint32_t kRamBytes = 32u * 1024u * 1024u;
    constexpr uint32_t kScratchpadBytes = 16u * 1024u;
    constexpr uint32_t kRamPages = kRamBytes / kPageBytes;              // 8192
    constexpr uint32_t kScratchPages = kScratchpadBytes / kPageBytes;   // 4: page indices 8192..8195
    constexpr uint32_t kPageCount = kRamPages + kScratchPages;
    constexpr uint32_t kGsMemBytes = 4u * 1024u * 1024u;
    constexpr uint32_t kGsRegCount = 0x63u;

    constexpr uint32_t fourcc(const char (&s)[5])
    {
        return static_cast<uint32_t>(static_cast<uint8_t>(s[0])) | static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 8 |
               static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 16 | static_cast<uint32_t>(static_cast<uint8_t>(s[3])) << 24;
    }

    // Chunk ids (as the 4 bytes appear in the file).
    constexpr uint32_t kChunkFile = fourcc("RCFR");
    constexpr uint32_t kChunkTagNames = fourcc("TNAM");
    constexpr uint32_t kChunkKeyframe = fourcc("KEYF");
    constexpr uint32_t kChunkCycle = fourcc("CYCL");
    constexpr uint32_t kChunkPicture = fourcc("REFI");
    constexpr uint32_t kChunkIndex = fourcc("INDX");
    constexpr uint32_t kChunkEnd = fourcc("ENDF");
    // State chunks nested in a KEYF.
    constexpr uint32_t kStateGsMemory = fourcc("GSVR");   // GS local memory, kGsMemBytes, as the runtime stores it
    constexpr uint32_t kStateGsPriv = fourcc("GSPR");     // the privileged registers: 20 x u64 (GsPrivRegs)
    constexpr uint32_t kStateGsRegs = fourcc("GSRG");     // the general register file: u64[kGsRegCount] by address
    constexpr uint32_t kStateVu1Memory = fourcc("VU1M");  // VU1 micro memory then data memory, 16 KB each
    constexpr uint32_t kStateVu1Regs = fourcc("VU1R");    // Vu1Regs
    constexpr uint32_t kStateVif1 = fourcc("VIF1");       // Vif1Regs
    constexpr uint32_t kStateEmu = fourcc("EMUS");        // opaque runtime state (batch 2)
    constexpr uint32_t kStatePendingTags = fourcc("TAGP");// tags not yet consumed (batch 3)

    constexpr uint32_t kChunkCompressed = 1u; // ChunkHeader.flags bit 0: the payload is a zlib stream

    struct ChunkHeader
    {
        uint32_t id;
        uint32_t flags;
        uint32_t crc32;       // zlib's crc32 of the raw (uncompressed) payload
        uint32_t reserved;
        uint64_t storedBytes; // bytes that follow (before padding)
        uint64_t rawBytes;
    };
    static_assert(sizeof(ChunkHeader) == 32);

    constexpr uint32_t kFileExact = 1u << 0;      // exact mode (RENDERER_FRAME_RECORD.md 3.5)
    constexpr uint32_t kFilePictures = 1u << 1;   // REFI chunks present
    constexpr uint32_t kFileTags = 1u << 2;       // TAG records present
    constexpr uint32_t kFileComplete = 1u << 3;   // closed normally (also: indexOffset != 0, ENDF present)

    struct FileHeader
    {
        char magic[8];             // "RCFREC1\0"
        uint32_t formatVersion;    // kFormatVersion
        uint32_t headerBytes;      // sizeof(FileHeader)
        uint32_t gameVersion;      // 0 USA (SLUS-20153), 1 Europe (SLES-50077)
        uint32_t flags;            // kFile*
        uint32_t width;            // 640
        uint32_t height;           // 240 USA, 256 Europe
        uint32_t fieldRateMilliHz; // 59940 / 50000
        uint32_t ramBytes, scratchpadBytes, pageBytes, gsMemBytes;
        uint32_t emuStateVersion;  // EMUS layout (0: none)
        uint32_t pad;
        char elfName[16];
        char gsBackend[16];
        char build[40];            // the fork's `git describe`
        uint64_t indexOffset;      // file offset of the INDX chunk header; 0 = not closed
    };
    static_assert(sizeof(FileHeader) == 144);
    constexpr char kMagic[8] = {'R', 'C', 'F', 'R', 'E', 'C', '1', '\0'};

    constexpr uint32_t kKeyVramExact = 1u << 0;
    constexpr uint32_t kKeyFinal = 1u << 1;
    constexpr uint32_t kKeyAfterStateLoad = 1u << 2;

    struct KeyframeHeader
    {
        uint64_t cycle;     // the display cycle that follows (for a final keyframe: one past the last)
        uint64_t gameFrame; // all ones when unknown
        uint64_t field;
        uint32_t flags;     // kKey*
        uint32_t stateChunkCount;
    };
    static_assert(sizeof(KeyframeHeader) == 32);

    struct GsPrivRegs // GSPR: the runtime's GSRegisters order
    {
        uint64_t pmode, smode1, smode2, srfsh, synch1, synch2, syncv, dispfb1, display1, dispfb2, display2, extbuf, extdata,
            extwrite, bgcolor, csr, vsyncTick, imr, busdir, siglblid;
    };
    static_assert(sizeof(GsPrivRegs) == 160);

    struct Vu1Regs // VU1R
    {
        float vf[32][4];
        int32_t vi[16];
        float acc[4];
        float q, p, i;
        uint32_t r, pc, mac, clip, status;
    };
    static_assert(sizeof(Vu1Regs) == 512 + 64 + 16 + 32);

    struct Vif1Regs // VIF1: the runtime's VIFRegisters
    {
        uint32_t stat, fbrst, err, mark, cycle, mode, num, mask, code, itops, base, ofst, tops, itop, top;
        uint32_t row[4], col[4];
        uint32_t pad;
    };
    static_assert(sizeof(Vif1Regs) == 96);

    struct IndexHeader
    {
        uint64_t segmentCount;
        uint64_t cycleCount;
    };
    struct IndexSegment
    {
        uint64_t offset;     // file offset of the segment's KEYF chunk header
        uint64_t firstCycle; // END.cycle of its first cycle
        uint64_t cycleCount;
    };
    struct IndexCycle
    {
        uint64_t cyclOffset; // file offset of the CYCL chunk header
        uint64_t refiOffset; // of its REFI chunk, 0 = none
    };

    // Records inside a CYCL chunk's (decompressed) payload: RecHeader then the body; bytes includes the header and is a
    // multiple of 8.
    enum RecType : uint16_t
    {
        RecPage = 1,
        RecHw = 2,
        RecCrt = 3,
        RecKick = 4,
        RecTag = 5,
        RecOdd = 6,
        RecEnd = 7,
        RecNote = 8,
    };

    struct RecHeader
    {
        uint16_t type;
        uint16_t flags;
        uint32_t bytes;
    };
    static_assert(sizeof(RecHeader) == 8);

    struct RecPageBody // then count * kPageBytes
    {
        uint32_t firstPage; // 0..8191 RDRAM, 8192..8195 scratchpad
        uint32_t count;
    };

    struct RecHwBody
    {
        uint32_t address, width;
        uint64_t value;
        uint32_t callerRa, pad;
    };
    static_assert(sizeof(RecHwBody) == 24);

    struct RecCrtBody
    {
        int32_t interlaced, mode, ffmd;
        uint32_t pad;
    };

    constexpr uint16_t kKickHasGifHash = 1u << 0;
    constexpr uint16_t kKickHasRegs = 1u << 1;
    constexpr uint16_t kKickTte = 1u << 2;

    struct RecKickBody // then spanCount x RecSpan, labelCount x u32 (padded to 8), u64 regs[kGsRegCount] if hasRegs
    {
        uint32_t index, chcr, madr, qwc, tadr, asr0, asr1, callerRa;
        uint8_t channel, mode;
        uint16_t kflags;
        uint32_t dmaTags, streamBytes, gifBytes, mxcsr, fbrst, spanCount, labelCount;
        uint64_t field, streamHash, gifHash;
    };
    static_assert(sizeof(RecKickBody) == 88);

    struct RecSpan
    {
        uint32_t streamOffset, guestAddr, tagSeq;
        uint8_t op;
        uint8_t pad[3];
    };
    static_assert(sizeof(RecSpan) == 16);

    struct RecTagBody // then payloadBytes (padded to 8)
    {
        uint32_t seq;
        uint16_t kind;
        uint8_t op, depth;
        uint32_t anchor, parent, payloadBytes, pad;
    };
    static_assert(sizeof(RecTagBody) == 24);

    struct RecOddBody
    {
        uint32_t kind, address;
        uint64_t value;
    };

    struct RecEndBody
    {
        uint64_t cycle, gameFrame, field, hostNs, dispfb2, display2, pmode, refHash;
        uint32_t refWidth, refHeight, refFbp, kickCount, hasImage, pad;
    };
    static_assert(sizeof(RecEndBody) == 88);

    constexpr uint32_t pad8(uint64_t n) { return static_cast<uint32_t>((8u - (n & 7u)) & 7u); }

    // A streaming 64-bit hash. Normative: four 64-bit lanes seeded as below, fed 32-byte blocks of four little-endian
    // u64 words (lane k takes word k: lane = (lane ^ word) * 0x100000001B3; lane ^= lane >> 29), the last partial block
    // zero-padded; the result is lanes[0] ^ lanes[1]*3 ^ lanes[2]*5 ^ lanes[3]*7, then x ^= length * 0x9E3779B97F4A7C15,
    // x ^= x >> 31. The lane step is GSRecording::HashVram's. Chunking never changes the result.
    class Hash64
    {
    public:
        void update(const void *data, size_t bytes)
        {
            const uint8_t *p = static_cast<const uint8_t *>(data);
            m_length += bytes;
            if (m_fill)
            {
                const size_t take = bytes < 32u - m_fill ? bytes : 32u - m_fill;
                std::memcpy(m_block + m_fill, p, take);
                m_fill += take;
                p += take;
                bytes -= take;
                if (m_fill < 32u)
                    return;
                block(m_block);
                m_fill = 0;
            }
            while (bytes >= 32u)
            {
                block(p);
                p += 32;
                bytes -= 32;
            }
            if (bytes)
            {
                std::memcpy(m_block, p, bytes);
                m_fill = bytes;
            }
        }
        uint64_t finish() const
        {
            uint64_t lanes[4] = {m_lanes[0], m_lanes[1], m_lanes[2], m_lanes[3]};
            if (m_fill)
            {
                uint8_t last[32] = {};
                std::memcpy(last, m_block, m_fill);
                step(lanes, last);
            }
            uint64_t x = lanes[0] ^ (lanes[1] * 3u) ^ (lanes[2] * 5u) ^ (lanes[3] * 7u);
            x ^= static_cast<uint64_t>(m_length) * 0x9E3779B97F4A7C15ull;
            x ^= x >> 31;
            return x;
        }
        uint64_t length() const { return m_length; }
        static uint64_t of(const void *data, size_t bytes)
        {
            Hash64 h;
            h.update(data, bytes);
            return h.finish();
        }

    private:
        static void step(uint64_t lanes[4], const uint8_t *p)
        {
            for (int lane = 0; lane < 4; ++lane)
            {
                uint64_t word;
                std::memcpy(&word, p + lane * 8, 8);
                lanes[lane] = (lanes[lane] ^ word) * 0x100000001B3ull;
                lanes[lane] ^= lanes[lane] >> 29u;
            }
        }
        void block(const uint8_t *p) { step(m_lanes, p); }

        uint64_t m_lanes[4] = {0x9E3779B97F4A7C15ull, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull};
        uint8_t m_block[32] = {};
        size_t m_fill = 0;
        uint64_t m_length = 0;
    };
}

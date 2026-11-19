#pragma once

// Reads a dump (RENDERER_FRAME_RECORD.md section 6.4): chunks -> a rebuilt RAM image -> Consumer calls, in event order.
// A dump that was cut short (no index, no ENDF) is scanned and read up to its last whole chunk.

#include "consumer.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace rc::rec
{
    class Reader
    {
    public:
        ~Reader();
        // Reads the header and the index (or scans the chunks when the dump was not closed). False: unusable dump.
        bool open(const std::string &path);
        const FileHeader &header() const { return m_header; }
        bool complete() const { return m_complete; }   // closed with an index and ENDF
        bool truncated() const { return m_truncated; } // a partial chunk at the end
        const std::vector<IndexSegment> &segments() const { return m_segments; }
        const std::vector<IndexCycle> &cycles() const { return m_cycles; }
        uint64_t fileBytes() const { return m_fileBytes; }
        const std::string &tagNames() const { return m_tagNames; } // the TNAM chunk's text, empty without one

        // Replays from the start of the last segment whose first cycle is not after `firstCycle`, through `lastCycle`
        // (END.cycle numbers). Events of cycles before firstCycle only update the RAM image. False on a read error
        // (error() says which); consumers have then seen everything before it.
        bool run(const std::vector<Consumer *> &consumers, uint64_t firstCycle = 0, uint64_t lastCycle = ~0ull);

        // Checks every chunk's CRC and every record's size, and the index against a scan. Fills `report`.
        bool verify(std::string &report);

        const std::string &error() const { return m_error; }

        struct ChunkStats
        {
            uint64_t count = 0, stored = 0, raw = 0;
        };
        // Filled by run() and verify(): by chunk id, and by record type (raw bytes).
        std::map<uint32_t, ChunkStats> chunkStats;
        uint64_t recordCount[9] = {}, recordBytes[9] = {};
        uint64_t badChunks = 0;

    private:
        struct ChunkRef
        {
            ChunkHeader header{};
            uint64_t offset = 0; // of the header
        };
        bool readChunkHeader(uint64_t offset, ChunkHeader &h);
        bool readPayload(const ChunkRef &ref, std::vector<uint8_t> &raw, bool checkCrc = true);
        bool scan(std::vector<ChunkRef> &chunks, bool &sawEnd, bool &partial);
        bool fail(const std::string &message);
        bool parseKeyframe(const std::vector<uint8_t> &payload, Keyframe &kf, std::vector<std::vector<uint8_t>> &store);

        std::FILE *m_file = nullptr;
        std::string m_path;
        FileHeader m_header{};
        uint64_t m_fileBytes = 0;
        bool m_complete = false, m_truncated = false;
        std::vector<IndexSegment> m_segments;
        std::vector<IndexCycle> m_cycles;
        std::vector<ChunkRef> m_chunks; // every whole chunk after RCFR, in file order
        std::string m_error;
        std::string m_tagNames;

        // The rebuilt image.
        std::unique_ptr<uint8_t[]> m_ram, m_scratch;
        std::vector<uint8_t> m_present;
        std::vector<uint32_t> m_generation;
    };
}

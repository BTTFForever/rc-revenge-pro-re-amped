#pragma once

// The file writer (RENDERER_FRAME_RECORD.md section 6.4) as a consumer: it serialises the events of each cycle into
// records and hands whole cycles and keyframes to a worker thread, which compresses them (zlib) and writes the chunks,
// so the EE thread only copies. close() writes the index and ENDF and patches the header.
//
// (batch 4) Ring mode (RC_FRAME_RECORD_AT=ring:<n>): the worker keeps the encoded chunks in memory instead, whole
// segments at a time (a segment is a keyframe and the cycles after it), and drops the oldest segment while the others
// still hold at least n cycles. A final keyframe is kept apart; dump(path) then writes a complete dump of the ring plus
// that final keyframe to a file, on the worker thread, and the ring goes on.

#include "consumer.h"

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rc::rec
{
    class Writer final : public Consumer
    {
    public:
        // zlibLevel 1 by default; 0 stores chunks uncompressed.
        explicit Writer(std::string path, int zlibLevel = 1);
        struct Ring
        {
            uint64_t cycles; // keep at least this many cycles (whole segments)
        };
        explicit Writer(Ring ring, int zlibLevel = 1);
        bool ringMode() const { return m_ringMode; }
        // Ring mode: write the ring and the last final keyframe to `path` (on the worker thread, after everything
        // queued before it).
        void dump(std::string path);
        ~Writer() override;
        bool ok() const { return m_ok; }
        const std::string &path() const { return m_path; }

        void open(const FileInfo &info) override;
        void keyframe(const Keyframe &kf) override;
        void cycleBegin(uint64_t cycle) override;
        void pages(uint32_t firstPage, uint32_t count, const uint8_t *data) override;
        void hw(uint32_t address, uint32_t width, uint64_t value, uint32_t callerRa) override;
        void crt(int interlaced, int mode, int ffmd) override;
        void tag(const TagView &tag) override;
        void kick(const KickView &kick, const RamView &ram) override;
        void odd(int kind, uint32_t address, uint64_t value) override;
        void note(const std::string &text) override;
        void cycleEnd(const CycleInfo &info, const uint8_t *refRgba) override;
        void close() override;

        struct Stats
        {
            uint64_t recordCount[9] = {}, recordBytes[9] = {}; // by RecType (raw)
            uint64_t cycles = 0, keyframes = 0;
            uint64_t cycleRawBytes = 0, cycleStoredBytes = 0, keyRawBytes = 0, keyStoredBytes = 0, fileBytes = 0;
            uint64_t maxCycleRaw = 0, maxCycleStored = 0;
        };
        // Complete after close().
        const Stats &stats() const { return m_stats; }
        // Ring mode, any time (the worker's view, a little behind the EE thread).
        struct RingStats
        {
            uint64_t segments = 0, cycles = 0, bytes = 0, peakBytes = 0, dumps = 0;
            uint64_t cycleRawBytes = 0, cycleStoredBytes = 0, cyclesEncoded = 0, keyStoredBytes = 0, keyframes = 0;
        };
        RingStats ringStats();

    private:
        struct Job
        {
            enum Kind { Cycle, Key, Picture, Close, Dump } kind = Cycle;
            std::string path;                                           // Dump
            uint64_t cycle = 0;
            std::vector<uint8_t> raw;                                   // Cycle, Picture
            KeyframeHeader key{};                                       // Key
            std::vector<std::pair<uint32_t, std::vector<uint8_t>>> state; // Key
        };

        void record(uint16_t type, const void *body, uint32_t bodyBytes, const void *extra = nullptr, uint32_t extraBytes = 0u);
        void push(Job &&job, uint64_t bytes);
        void worker();
        uint64_t writeChunk(uint32_t id, const uint8_t *raw, uint64_t rawBytes, bool compress);
        uint64_t appendChunk(std::vector<uint8_t> &out, uint32_t id, const uint8_t *raw, uint64_t rawBytes, bool compress);
        // Ring mode, worker side.
        struct RingChunk
        {
            uint32_t id;
            std::vector<uint8_t> bytes; // the whole chunk: header, payload, padding
        };
        struct RingSegment
        {
            uint64_t firstCycle = 0, cycleCount = 0, bytes = 0;
            std::vector<RingChunk> chunks;
        };
        void ringAdd(uint32_t id, std::vector<uint8_t> &&chunk, uint64_t cycle);
        void ringDump(const std::string &path);

        std::string m_path;
        int m_level;
        std::FILE *m_file = nullptr;
        bool m_ok = false;
        bool m_open = false;
        FileHeader m_header{};
        std::vector<uint8_t> m_cycle; // records of the cycle being built
        bool m_inCycle = false;

        std::thread m_thread;
        std::mutex m_mutex;
        std::condition_variable m_cv, m_space;
        std::deque<Job> m_jobs;
        uint64_t m_pendingBytes = 0;

        // Worker-side.
        uint64_t m_offset = 0;
        std::vector<IndexSegment> m_segments;
        std::vector<IndexCycle> m_cycles;
        Stats m_stats;
        // Ring mode.
        bool m_ringMode = false;
        uint64_t m_ringCycles = 0;
        std::string m_tagNames;
        std::deque<RingSegment> m_ring;
        std::vector<uint8_t> m_ringFinal; // the last final keyframe's chunk
        RingStats m_ringStats;           // under m_mutex
        static constexpr uint32_t kRingFinal = 1u;
        uint64_t m_pendingCycle = 0;     // worker: the job being encoded
        uint32_t m_pendingKind = 0;
    };
}

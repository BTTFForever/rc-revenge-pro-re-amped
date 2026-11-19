#include "writer.h"

#include <zlib.h>

#include <algorithm>
#include <cstring>

namespace rc::rec
{
    namespace
    {
        constexpr uint64_t kMaxPendingBytes = 512ull << 20; // the EE thread waits beyond this

        uint32_t crcOf(const uint8_t *data, uint64_t bytes)
        {
            uLong crc = crc32(0L, Z_NULL, 0);
            while (bytes)
            {
                const uInt step = static_cast<uInt>(std::min<uint64_t>(bytes, 1u << 30));
                crc = crc32(crc, data, step);
                data += step;
                bytes -= step;
            }
            return static_cast<uint32_t>(crc);
        }

        bool deflateAll(const uint8_t *raw, uint64_t rawBytes, int level, std::vector<uint8_t> &out)
        {
            z_stream z{};
            if (deflateInit(&z, level) != Z_OK)
                return false;
            out.resize(deflateBound(&z, static_cast<uLong>(rawBytes)) + 64u);
            z.next_in = const_cast<Bytef *>(raw);
            z.avail_in = static_cast<uInt>(rawBytes);
            z.next_out = out.data();
            z.avail_out = static_cast<uInt>(out.size());
            const int rc = deflate(&z, Z_FINISH);
            out.resize(z.total_out);
            deflateEnd(&z);
            return rc == Z_STREAM_END;
        }
    }

    Writer::Writer(std::string path, int zlibLevel) : m_path(std::move(path)), m_level(zlibLevel)
    {
        m_file = std::fopen(m_path.c_str(), "wb");
        m_ok = m_file != nullptr;
    }

    Writer::~Writer()
    {
        if (m_open)
            close();
        if (m_file)
            std::fclose(m_file);
    }

    uint64_t Writer::appendChunk(std::vector<uint8_t> &out, uint32_t id, const uint8_t *raw, uint64_t rawBytes, bool compress)
    {
        ChunkHeader h{};
        h.id = id;
        h.crc32 = crcOf(raw, rawBytes);
        h.rawBytes = rawBytes;
        std::vector<uint8_t> packed;
        const uint8_t *payload = raw;
        uint64_t stored = rawBytes;
        if (compress && m_level > 0 && rawBytes > 0u && rawBytes < (1ull << 32) && deflateAll(raw, rawBytes, m_level, packed) &&
            packed.size() < rawBytes)
        {
            h.flags = kChunkCompressed;
            payload = packed.data();
            stored = packed.size();
        }
        h.storedBytes = stored;
        const size_t at = out.size();
        out.resize(at + sizeof(h) + stored + pad8(stored));
        std::memcpy(out.data() + at, &h, sizeof(h));
        if (stored)
            std::memcpy(out.data() + at + sizeof(h), payload, stored);
        std::memset(out.data() + at + sizeof(h) + stored, 0, pad8(stored));
        return stored;
    }

    uint64_t Writer::writeChunk(uint32_t id, const uint8_t *raw, uint64_t rawBytes, bool compress)
    {
        std::vector<uint8_t> out;
        const uint64_t stored = appendChunk(out, id, raw, rawBytes, compress);
        if (m_ok && std::fwrite(out.data(), 1, out.size(), m_file) != out.size())
            m_ok = false;
        m_offset += out.size();
        return stored;
    }

    void Writer::open(const FileInfo &info)
    {
        if (!m_ok || m_open)
            return;
        m_header = info.header;
        std::memcpy(m_header.magic, kMagic, sizeof(kMagic));
        m_header.formatVersion = kFormatVersion;
        m_header.headerBytes = sizeof(FileHeader);
        m_header.indexOffset = 0;
        m_header.flags &= ~kFileComplete;
        writeChunk(kChunkFile, reinterpret_cast<const uint8_t *>(&m_header), sizeof(m_header), false);
        std::fflush(m_file);
        m_open = true;
        m_thread = std::thread(&Writer::worker, this);
    }

    void Writer::push(Job &&job, uint64_t bytes)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_space.wait(lock, [&] { return m_pendingBytes < kMaxPendingBytes || m_jobs.empty(); });
        m_pendingBytes += bytes;
        m_jobs.push_back(std::move(job));
        m_cv.notify_one();
    }

    void Writer::keyframe(const Keyframe &kf)
    {
        if (!m_open)
            return;
        Job job;
        job.kind = Job::Key;
        job.key = kf.header;
        job.key.stateChunkCount = static_cast<uint32_t>(kf.chunks.size());
        uint64_t bytes = 0;
        for (const StateChunk &c : kf.chunks)
        {
            job.state.emplace_back(c.id, std::vector<uint8_t>(c.data, c.data + c.bytes));
            bytes += c.bytes;
        }
        push(std::move(job), bytes);
    }

    void Writer::record(uint16_t type, const void *body, uint32_t bodyBytes, const void *extra, uint32_t extraBytes)
    {
        const uint32_t payload = bodyBytes + extraBytes;
        RecHeader h{type, 0u, static_cast<uint32_t>(sizeof(RecHeader) + payload + pad8(payload))};
        const size_t at = m_cycle.size();
        m_cycle.resize(at + h.bytes);
        uint8_t *p = m_cycle.data() + at;
        std::memcpy(p, &h, sizeof(h));
        if (bodyBytes)
            std::memcpy(p + sizeof(h), body, bodyBytes);
        if (extraBytes)
            std::memcpy(p + sizeof(h) + bodyBytes, extra, extraBytes);
        std::memset(p + sizeof(h) + payload, 0, pad8(payload));
        if (type < 9u)
        {
            ++m_stats.recordCount[type];
            m_stats.recordBytes[type] += h.bytes;
        }
    }

    void Writer::cycleBegin(uint64_t)
    {
        m_cycle.clear();
        m_inCycle = true;
    }

    void Writer::pages(uint32_t firstPage, uint32_t count, const uint8_t *data)
    {
        if (!m_open)
            return;
        RecPageBody body{firstPage, count};
        record(RecPage, &body, sizeof(body), data, count * kPageBytes);
    }

    void Writer::hw(uint32_t address, uint32_t width, uint64_t value, uint32_t callerRa)
    {
        if (!m_open)
            return;
        RecHwBody body{address, width, value, callerRa, 0u};
        record(RecHw, &body, sizeof(body));
    }

    void Writer::crt(int interlaced, int mode, int ffmd)
    {
        if (!m_open)
            return;
        RecCrtBody body{interlaced, mode, ffmd, 0u};
        record(RecCrt, &body, sizeof(body));
    }

    void Writer::tag(const TagView &tag)
    {
        if (!m_open)
            return;
        RecTagBody body{tag.seq, tag.kind, tag.op, tag.depth, tag.anchor, tag.parent, tag.payloadBytes, 0u};
        record(RecTag, &body, sizeof(body), tag.payload, tag.payloadBytes);
    }

    void Writer::kick(const KickView &k, const RamView &)
    {
        if (!m_open)
            return;
        RecKickBody body{};
        body.index = k.index;
        body.chcr = k.chcr;
        body.madr = k.madr;
        body.qwc = k.qwc;
        body.tadr = k.tadr;
        body.asr0 = k.asr0;
        body.asr1 = k.asr1;
        body.callerRa = k.callerRa;
        body.channel = k.channel;
        body.mode = k.mode;
        body.kflags = static_cast<uint16_t>((k.hasGifHash ? kKickHasGifHash : 0u) | (k.regs ? kKickHasRegs : 0u) | (k.tte ? kKickTte : 0u));
        body.dmaTags = k.dmaTags;
        body.streamBytes = k.streamBytes;
        body.gifBytes = k.gifBytes;
        body.mxcsr = k.mxcsr;
        body.fbrst = k.fbrst;
        body.spanCount = k.spanCount;
        body.labelCount = k.labelCount;
        body.field = k.field;
        body.streamHash = k.streamHash;
        body.gifHash = k.gifHash;
        std::vector<uint8_t> extra;
        extra.reserve(k.spanCount * sizeof(RecSpan) + k.labelCount * 4u + 8u + (k.regs ? kGsRegCount * 8u : 0u));
        for (uint32_t i = 0; i < k.spanCount; ++i)
        {
            RecSpan s{k.spans[i].streamOffset, k.spans[i].guestAddr, k.spans[i].tag ? k.spans[i].tag->seq : 0u, k.spans[i].op, {}};
            const uint8_t *p = reinterpret_cast<const uint8_t *>(&s);
            extra.insert(extra.end(), p, p + sizeof(s));
        }
        for (uint32_t i = 0; i < k.labelCount; ++i)
        {
            const uint32_t seq = k.labels[i] ? k.labels[i]->seq : 0u;
            const uint8_t *p = reinterpret_cast<const uint8_t *>(&seq);
            extra.insert(extra.end(), p, p + 4);
        }
        extra.resize(extra.size() + pad8(extra.size()), 0u);
        if (k.regs)
        {
            const uint8_t *p = reinterpret_cast<const uint8_t *>(k.regs);
            extra.insert(extra.end(), p, p + kGsRegCount * 8u);
        }
        record(RecKick, &body, sizeof(body), extra.data(), static_cast<uint32_t>(extra.size()));
    }

    void Writer::odd(int kind, uint32_t address, uint64_t value)
    {
        if (!m_open)
            return;
        RecOddBody body{static_cast<uint32_t>(kind), address, value};
        record(RecOdd, &body, sizeof(body));
    }

    void Writer::note(const std::string &text)
    {
        if (!m_open)
            return;
        record(RecNote, text.data(), static_cast<uint32_t>(text.size()));
    }

    void Writer::cycleEnd(const CycleInfo &info, const uint8_t *refRgba)
    {
        if (!m_open)
            return;
        record(RecEnd, &info.end, sizeof(info.end));
        Job job;
        job.kind = Job::Cycle;
        job.cycle = info.end.cycle;
        job.raw.swap(m_cycle);
        m_cycle.clear();
        const uint64_t bytes = job.raw.size();
        push(std::move(job), bytes);
        if (refRgba && info.end.hasImage)
        {
            Job pic;
            pic.kind = Job::Picture;
            pic.cycle = info.end.cycle;
            pic.raw.assign(refRgba, refRgba + static_cast<size_t>(info.end.refWidth) * info.end.refHeight * 4u);
            const uint64_t picBytes = pic.raw.size();
            push(std::move(pic), picBytes);
        }
        m_inCycle = false;
    }

    void Writer::worker()
    {
        for (;;)
        {
            Job job;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [&] { return !m_jobs.empty(); });
                job = std::move(m_jobs.front());
                m_jobs.pop_front();
            }
            uint64_t done = 0;
            if (job.kind == Job::Close)
                break;
            if (job.kind == Job::Cycle)
            {
                done = job.raw.size();
                if (!m_segments.empty() && m_segments.back().cycleCount == 0u)
                    m_segments.back().firstCycle = job.cycle;
                if (!m_segments.empty())
                    ++m_segments.back().cycleCount;
                m_cycles.push_back({m_offset, 0u});
                const uint64_t stored = writeChunk(kChunkCycle, job.raw.data(), job.raw.size(), true);
                ++m_stats.cycles;
                m_stats.cycleRawBytes += job.raw.size();
                m_stats.cycleStoredBytes += stored;
                m_stats.maxCycleRaw = std::max<uint64_t>(m_stats.maxCycleRaw, job.raw.size());
                m_stats.maxCycleStored = std::max<uint64_t>(m_stats.maxCycleStored, stored);
            }
            else if (job.kind == Job::Picture)
            {
                done = job.raw.size();
                if (!m_cycles.empty())
                    m_cycles.back().refiOffset = m_offset;
                writeChunk(kChunkPicture, job.raw.data(), job.raw.size(), true);
            }
            else if (job.kind == Job::Key)
            {
                std::vector<uint8_t> payload(sizeof(KeyframeHeader));
                std::memcpy(payload.data(), &job.key, sizeof(KeyframeHeader));
                uint64_t raw = sizeof(KeyframeHeader);
                for (auto &[id, data] : job.state)
                {
                    appendChunk(payload, id, data.data(), data.size(), true);
                    raw += data.size();
                    done += data.size();
                }
                if (!(job.key.flags & kKeyFinal))
                    m_segments.push_back({m_offset, job.key.cycle, 0u});
                writeChunk(kChunkKeyframe, payload.data(), payload.size(), false);
                ++m_stats.keyframes;
                m_stats.keyRawBytes += raw;
                m_stats.keyStoredBytes += payload.size();
            }
            if (m_ok)
                std::fflush(m_file);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_pendingBytes -= std::min(m_pendingBytes, done);
            }
            m_space.notify_all();
        }
    }

    void Writer::close()
    {
        if (!m_open)
            return;
        Job job;
        job.kind = Job::Close;
        push(std::move(job), 0u);
        m_thread.join();
        m_open = false;
        // Index.
        std::vector<uint8_t> index(sizeof(IndexHeader) + m_segments.size() * sizeof(IndexSegment) + m_cycles.size() * sizeof(IndexCycle));
        IndexHeader ih{m_segments.size(), m_cycles.size()};
        std::memcpy(index.data(), &ih, sizeof(ih));
        if (!m_segments.empty())
            std::memcpy(index.data() + sizeof(ih), m_segments.data(), m_segments.size() * sizeof(IndexSegment));
        if (!m_cycles.empty())
            std::memcpy(index.data() + sizeof(ih) + m_segments.size() * sizeof(IndexSegment), m_cycles.data(),
                        m_cycles.size() * sizeof(IndexCycle));
        const uint64_t indexOffset = m_offset;
        writeChunk(kChunkIndex, index.data(), index.size(), true);
        writeChunk(kChunkEnd, nullptr, 0u, false);
        m_stats.fileBytes = m_offset;
        // Patch the header: index offset, complete; the chunk's CRC with it.
        m_header.indexOffset = indexOffset;
        m_header.flags |= kFileComplete;
        ChunkHeader h{};
        h.id = kChunkFile;
        h.crc32 = crcOf(reinterpret_cast<const uint8_t *>(&m_header), sizeof(m_header));
        h.storedBytes = h.rawBytes = sizeof(m_header);
        if (m_ok)
        {
            std::fflush(m_file);
            std::fseek(m_file, 0, SEEK_SET);
            std::fwrite(&h, sizeof(h), 1, m_file);
            std::fwrite(&m_header, sizeof(m_header), 1, m_file);
            std::fflush(m_file);
        }
        std::fclose(m_file);
        m_file = nullptr;
    }
}

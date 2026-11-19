#include "reader.h"

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <deque>

namespace rc::rec
{
    namespace
    {
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

        bool inflateAll(const uint8_t *in, uint64_t inBytes, uint8_t *out, uint64_t outBytes)
        {
            z_stream z{};
            if (inflateInit(&z) != Z_OK)
                return false;
            z.next_in = const_cast<Bytef *>(in);
            z.avail_in = static_cast<uInt>(inBytes);
            z.next_out = out;
            z.avail_out = static_cast<uInt>(outBytes);
            const int rc = inflate(&z, Z_FINISH);
            const bool ok = rc == Z_STREAM_END && z.total_out == outBytes;
            inflateEnd(&z);
            return ok;
        }

        std::string idName(uint32_t id)
        {
            std::string s(4, ' ');
            for (int i = 0; i < 4; ++i)
                s[i] = static_cast<char>((id >> (8 * i)) & 0xFF);
            return s;
        }

        // Tags kept for the kicks that refer to them: the current cycle's and the two before (section 5.3).
        struct TagStore
        {
            struct Entry
            {
                TagView view;
                std::vector<uint8_t> payload;
            };
            std::deque<std::vector<Entry>> cycles{std::vector<Entry>{}};
            const TagView *find(uint32_t seq) const
            {
                for (const auto &c : cycles)
                    for (const Entry &e : c)
                        if (e.view.seq == seq)
                            return &e.view;
                return nullptr;
            }
            void add(const TagView &v, const uint8_t *payload)
            {
                Entry e;
                e.view = v;
                e.payload.assign(payload, payload + v.payloadBytes);
                cycles.back().push_back(std::move(e));
                Entry &stored = cycles.back().back();
                stored.view.payload = stored.payload.data();
            }
            void nextCycle()
            {
                cycles.emplace_back();
                while (cycles.size() > 3)
                    cycles.pop_front();
            }
            void clear() { cycles.assign(1, {}); }
        };
    }

    Reader::~Reader()
    {
        if (m_file)
            std::fclose(m_file);
    }

    bool Reader::fail(const std::string &message)
    {
        if (m_error.empty())
            m_error = message;
        return false;
    }

    bool Reader::readChunkHeader(uint64_t offset, ChunkHeader &h)
    {
        if (offset + sizeof(ChunkHeader) > m_fileBytes)
            return false;
        if (std::fseek(m_file, static_cast<long>(offset), SEEK_SET) != 0)
            return false;
        return std::fread(&h, sizeof(h), 1, m_file) == 1;
    }

    bool Reader::readPayload(const ChunkRef &ref, std::vector<uint8_t> &raw, bool checkCrc)
    {
        const ChunkHeader &h = ref.header;
        std::vector<uint8_t> stored(h.storedBytes);
        if (std::fseek(m_file, static_cast<long>(ref.offset + sizeof(ChunkHeader)), SEEK_SET) != 0 ||
            (h.storedBytes && std::fread(stored.data(), 1, stored.size(), m_file) != stored.size()))
            return fail("short read in chunk " + idName(h.id) + " at " + std::to_string(ref.offset));
        if (h.flags & kChunkCompressed)
        {
            raw.resize(h.rawBytes);
            if (!inflateAll(stored.data(), stored.size(), raw.data(), raw.size()))
                return fail("bad zlib stream in chunk " + idName(h.id) + " at " + std::to_string(ref.offset));
        }
        else
            raw.swap(stored);
        if (checkCrc && crcOf(raw.data(), raw.size()) != h.crc32)
        {
            ++badChunks;
            return fail("CRC mismatch in chunk " + idName(h.id) + " at " + std::to_string(ref.offset));
        }
        return true;
    }

    bool Reader::scan(std::vector<ChunkRef> &chunks, bool &sawEnd, bool &partial)
    {
        chunks.clear();
        sawEnd = partial = false;
        ChunkHeader first{};
        if (!readChunkHeader(0, first))
            return false;
        uint64_t offset = sizeof(ChunkHeader) + first.storedBytes + pad8(first.storedBytes);
        while (offset < m_fileBytes)
        {
            ChunkHeader h{};
            if (!readChunkHeader(offset, h))
            {
                partial = true;
                break;
            }
            const uint64_t next = offset + sizeof(ChunkHeader) + h.storedBytes + pad8(h.storedBytes);
            if (h.storedBytes > m_fileBytes || next > m_fileBytes + 7u || offset + sizeof(ChunkHeader) + h.storedBytes > m_fileBytes)
            {
                partial = true;
                break;
            }
            chunks.push_back({h, offset});
            if (h.id == kChunkEnd)
            {
                sawEnd = true;
                break;
            }
            offset = next;
        }
        return true;
    }

    bool Reader::open(const std::string &path)
    {
        m_path = path;
        m_file = std::fopen(path.c_str(), "rb");
        if (!m_file)
            return fail("cannot open " + path);
        std::fseek(m_file, 0, SEEK_END);
        m_fileBytes = static_cast<uint64_t>(std::ftell(m_file));
        ChunkHeader h{};
        if (!readChunkHeader(0, h) || h.id != kChunkFile || h.storedBytes < sizeof(FileHeader) || h.flags != 0u)
            return fail("not a frame record (no RCFR chunk)");
        std::vector<uint8_t> raw;
        if (!readPayload({h, 0}, raw))
            return false;
        std::memcpy(&m_header, raw.data(), sizeof(FileHeader));
        if (std::memcmp(m_header.magic, kMagic, sizeof(kMagic)) != 0)
            return fail("bad magic");
        if (m_header.formatVersion > kFormatVersion)
            return fail("format version " + std::to_string(m_header.formatVersion) + " is newer than this reader's");
        if (m_header.ramBytes != kRamBytes || m_header.pageBytes != kPageBytes || m_header.scratchpadBytes != kScratchpadBytes)
            return fail("unexpected memory sizes in the header");
        bool sawEnd = false, partial = false;
        if (!scan(m_chunks, sawEnd, partial))
            return fail("cannot scan chunks");
        m_truncated = partial;
        // The index, when the dump was closed; otherwise derived from the scan (segment first cycles unknown: 0).
        bool haveIndex = false;
        if (m_header.indexOffset)
        {
            for (const ChunkRef &c : m_chunks)
            {
                if (c.offset != m_header.indexOffset || c.header.id != kChunkIndex)
                    continue;
                std::vector<uint8_t> index;
                if (!readPayload(c, index) || index.size() < sizeof(IndexHeader))
                    break;
                IndexHeader ih;
                std::memcpy(&ih, index.data(), sizeof(ih));
                const uint64_t need = sizeof(ih) + ih.segmentCount * sizeof(IndexSegment) + ih.cycleCount * sizeof(IndexCycle);
                if (need != index.size())
                    break;
                m_segments.resize(ih.segmentCount);
                m_cycles.resize(ih.cycleCount);
                if (ih.segmentCount)
                    std::memcpy(m_segments.data(), index.data() + sizeof(ih), ih.segmentCount * sizeof(IndexSegment));
                if (ih.cycleCount)
                    std::memcpy(m_cycles.data(), index.data() + sizeof(ih) + ih.segmentCount * sizeof(IndexSegment),
                                ih.cycleCount * sizeof(IndexCycle));
                haveIndex = true;
            }
        }
        if (!haveIndex)
        {
            m_segments.clear();
            m_cycles.clear();
            for (const ChunkRef &c : m_chunks)
            {
                if (c.header.id == kChunkKeyframe)
                    m_segments.push_back({c.offset, 0u, 0u});
                else if (c.header.id == kChunkCycle)
                {
                    m_cycles.push_back({c.offset, 0u});
                    if (!m_segments.empty())
                        ++m_segments.back().cycleCount;
                }
                else if (c.header.id == kChunkPicture && !m_cycles.empty())
                    m_cycles.back().refiOffset = c.offset;
            }
            // A final keyframe is not a segment: drop segments with no cycles at the end.
            while (!m_segments.empty() && m_segments.back().cycleCount == 0u)
                m_segments.pop_back();
            // Each segment's first cycle number, from the END of its first cycle.
            for (IndexSegment &seg : m_segments)
            {
                for (const ChunkRef &c : m_chunks)
                {
                    if (c.offset <= seg.offset || c.header.id != kChunkCycle)
                        continue;
                    std::vector<uint8_t> body;
                    if (readPayload(c, body, false))
                        for (uint64_t at = 0; at + sizeof(RecHeader) <= body.size();)
                        {
                            RecHeader rh;
                            std::memcpy(&rh, body.data() + at, sizeof(rh));
                            if (rh.bytes < sizeof(RecHeader) || at + rh.bytes > body.size())
                                break;
                            if (rh.type == RecEnd && rh.bytes >= sizeof(RecHeader) + sizeof(RecEndBody))
                                std::memcpy(&seg.firstCycle, body.data() + at + sizeof(RecHeader), sizeof(uint64_t));
                            at += rh.bytes;
                        }
                    break;
                }
            }
            m_error.clear();
        }
        m_complete = haveIndex && sawEnd && !partial;
        return true;
    }

    bool Reader::parseKeyframe(const std::vector<uint8_t> &payload, Keyframe &kf, std::vector<std::vector<uint8_t>> &store)
    {
        if (payload.size() < sizeof(KeyframeHeader))
            return fail("short keyframe");
        std::memcpy(&kf.header, payload.data(), sizeof(KeyframeHeader));
        kf.chunks.clear();
        store.clear();
        store.reserve(kf.header.stateChunkCount);
        uint64_t at = sizeof(KeyframeHeader);
        for (uint32_t i = 0; i < kf.header.stateChunkCount; ++i)
        {
            if (at + sizeof(ChunkHeader) > payload.size())
                return fail("keyframe state chunk out of bounds");
            ChunkHeader h;
            std::memcpy(&h, payload.data() + at, sizeof(h));
            const uint64_t body = at + sizeof(ChunkHeader);
            if (body + h.storedBytes > payload.size())
                return fail("keyframe state chunk out of bounds");
            std::vector<uint8_t> raw;
            if (h.flags & kChunkCompressed)
            {
                raw.resize(h.rawBytes);
                if (!inflateAll(payload.data() + body, h.storedBytes, raw.data(), raw.size()))
                    return fail("bad zlib stream in keyframe state " + idName(h.id));
            }
            else
                raw.assign(payload.data() + body, payload.data() + body + h.storedBytes);
            if (crcOf(raw.data(), raw.size()) != h.crc32)
            {
                ++badChunks;
                return fail("CRC mismatch in keyframe state " + idName(h.id));
            }
            auto &cs = chunkStats[h.id];
            ++cs.count;
            cs.stored += h.storedBytes;
            cs.raw += h.rawBytes;
            store.push_back(std::move(raw));
            kf.chunks.push_back({h.id, store.back().data(), store.back().size()});
            at = body + h.storedBytes + pad8(h.storedBytes);
        }
        return true;
    }

    bool Reader::run(const std::vector<Consumer *> &consumers, uint64_t firstCycle, uint64_t lastCycle)
    {
        if (!m_file)
            return fail("not open");
        if (!m_ram)
        {
            m_ram.reset(new uint8_t[kRamBytes]);
            m_scratch.reset(new uint8_t[kScratchpadBytes]);
        }
        std::memset(m_ram.get(), 0, kRamBytes);
        std::memset(m_scratch.get(), 0, kScratchpadBytes);
        m_present.assign(kPageCount, 0u);
        m_generation.assign(kPageCount, 0u);
        RamView view{m_ram.get(), m_scratch.get(), m_present.data(), m_generation.data()};

        // Where to start.
        uint64_t startOffset = 0;
        for (const IndexSegment &s : m_segments)
            if (s.firstCycle <= firstCycle && s.cycleCount)
                startOffset = s.offset;
        size_t ci = 0;
        while (ci < m_chunks.size() && m_chunks[ci].offset < startOffset)
            ++ci;

        FileInfo info;
        info.header = m_header;
        info.path = m_path;
        for (Consumer *c : consumers)
            c->open(info);

        TagStore tags;
        std::vector<uint8_t> raw, picture;
        std::vector<std::vector<uint8_t>> keyStore;
        std::vector<Span> spans;
        std::vector<const TagView *> labels;
        uint32_t cycleIndex = 0;
        bool ok = true;
        for (; ci < m_chunks.size() && ok; ++ci)
        {
            const ChunkRef &ref = m_chunks[ci];
            const uint32_t id = ref.header.id;
            if (id != kChunkKeyframe && id != kChunkCycle)
            {
                auto &cs = chunkStats[id];
                ++cs.count;
                cs.stored += ref.header.storedBytes;
                cs.raw += ref.header.rawBytes;
                continue; // REFI is read with its cycle; INDX, ENDF, TNAM and unknown chunks are skipped
            }
            if (!readPayload(ref, raw))
            {
                ok = false;
                break;
            }
            auto &cs = chunkStats[id];
            ++cs.count;
            cs.stored += ref.header.storedBytes;
            cs.raw += ref.header.rawBytes;
            if (id == kChunkKeyframe)
            {
                Keyframe kf;
                if (!parseKeyframe(raw, kf, keyStore))
                {
                    ok = false;
                    break;
                }
                if (!(kf.header.flags & kKeyFinal))
                {
                    // A new segment: the image starts empty (section 3.4).
                    std::fill(m_present.begin(), m_present.end(), 0u);
                }
                for (Consumer *c : consumers)
                    c->keyframe(kf);
                continue;
            }
            // CYCL: find its END first (the last record) to know the cycle number.
            RecEndBody end{};
            bool haveEnd = false;
            for (uint64_t at = 0; at + sizeof(RecHeader) <= raw.size();)
            {
                RecHeader rh;
                std::memcpy(&rh, raw.data() + at, sizeof(rh));
                if (rh.bytes < sizeof(RecHeader) || at + rh.bytes > raw.size() || (rh.bytes & 7u))
                {
                    ok = fail("bad record size in the cycle at " + std::to_string(ref.offset));
                    break;
                }
                if (rh.type == RecEnd && rh.bytes >= sizeof(RecHeader) + sizeof(RecEndBody))
                {
                    std::memcpy(&end, raw.data() + at + sizeof(RecHeader), sizeof(end));
                    haveEnd = true;
                }
                at += rh.bytes;
            }
            if (!ok)
                break;
            if (!haveEnd)
            {
                ok = fail("cycle without END at " + std::to_string(ref.offset));
                break;
            }
            if (end.cycle > lastCycle)
                break;
            const bool deliver = end.cycle >= firstCycle;
            const uint8_t *refRgba = nullptr;
            if (deliver && end.hasImage && ci + 1 < m_chunks.size() && m_chunks[ci + 1].header.id == kChunkPicture)
            {
                if (readPayload(m_chunks[ci + 1], picture) &&
                    picture.size() == static_cast<size_t>(end.refWidth) * end.refHeight * 4u)
                    refRgba = picture.data();
            }
            if (deliver)
                for (Consumer *c : consumers)
                    c->cycleBegin(end.cycle);
            for (uint64_t at = 0; at + sizeof(RecHeader) <= raw.size() && ok;)
            {
                RecHeader rh;
                std::memcpy(&rh, raw.data() + at, sizeof(rh));
                const uint8_t *body = raw.data() + at + sizeof(RecHeader);
                const uint32_t bodyBytes = rh.bytes - static_cast<uint32_t>(sizeof(RecHeader));
                at += rh.bytes;
                if (rh.type < 9u)
                {
                    ++recordCount[rh.type];
                    recordBytes[rh.type] += rh.bytes;
                }
                auto need = [&](uint64_t n)
                {
                    if (bodyBytes < n)
                        ok = fail("short record of type " + std::to_string(rh.type) + " in the cycle at " + std::to_string(ref.offset));
                    return ok;
                };
                switch (rh.type)
                {
                case RecPage:
                {
                    if (!need(sizeof(RecPageBody)))
                        break;
                    RecPageBody pb;
                    std::memcpy(&pb, body, sizeof(pb));
                    if (!need(sizeof(pb) + static_cast<uint64_t>(pb.count) * kPageBytes) || pb.firstPage + pb.count > kPageCount)
                    {
                        ok = fail("bad PAGE record");
                        break;
                    }
                    const uint8_t *src = body + sizeof(pb);
                    for (uint32_t i = 0; i < pb.count; ++i)
                    {
                        const uint32_t page = pb.firstPage + i;
                        uint8_t *dst = page < kRamPages ? m_ram.get() + static_cast<size_t>(page) * kPageBytes
                                                        : m_scratch.get() + static_cast<size_t>(page - kRamPages) * kPageBytes;
                        std::memcpy(dst, src + static_cast<size_t>(i) * kPageBytes, kPageBytes);
                        m_present[page] = 1u;
                        ++m_generation[page];
                    }
                    if (deliver)
                        for (Consumer *c : consumers)
                            c->pages(pb.firstPage, pb.count, src);
                    break;
                }
                case RecHw:
                {
                    if (!need(sizeof(RecHwBody)))
                        break;
                    RecHwBody hb;
                    std::memcpy(&hb, body, sizeof(hb));
                    if (deliver)
                        for (Consumer *c : consumers)
                            c->hw(hb.address, hb.width, hb.value, hb.callerRa);
                    break;
                }
                case RecCrt:
                {
                    if (!need(sizeof(RecCrtBody)))
                        break;
                    RecCrtBody cb;
                    std::memcpy(&cb, body, sizeof(cb));
                    if (deliver)
                        for (Consumer *c : consumers)
                            c->crt(cb.interlaced, cb.mode, cb.ffmd);
                    break;
                }
                case RecTag:
                {
                    if (!need(sizeof(RecTagBody)))
                        break;
                    RecTagBody tb;
                    std::memcpy(&tb, body, sizeof(tb));
                    if (!need(sizeof(tb) + static_cast<uint64_t>(tb.payloadBytes)))
                        break;
                    TagView v;
                    v.seq = tb.seq;
                    v.kind = tb.kind;
                    v.op = tb.op;
                    v.depth = tb.depth;
                    v.anchor = tb.anchor;
                    v.parent = tb.parent;
                    v.payloadBytes = tb.payloadBytes;
                    tags.add(v, body + sizeof(tb));
                    if (deliver)
                        for (Consumer *c : consumers)
                            c->tag(*tags.find(tb.seq));
                    break;
                }
                case RecKick:
                {
                    if (!need(sizeof(RecKickBody)))
                        break;
                    RecKickBody kb;
                    std::memcpy(&kb, body, sizeof(kb));
                    uint64_t extra = sizeof(kb) + static_cast<uint64_t>(kb.spanCount) * sizeof(RecSpan);
                    const uint64_t labelsAt = extra;
                    extra += static_cast<uint64_t>(kb.labelCount) * 4u;
                    extra += pad8(extra);
                    const uint64_t regsAt = extra;
                    if (kb.kflags & kKickHasRegs)
                        extra += kGsRegCount * 8u;
                    if (!need(extra))
                        break;
                    spans.resize(kb.spanCount);
                    for (uint32_t i = 0; i < kb.spanCount; ++i)
                    {
                        RecSpan s;
                        std::memcpy(&s, body + sizeof(kb) + i * sizeof(RecSpan), sizeof(s));
                        spans[i] = {s.streamOffset, s.guestAddr, tags.find(s.tagSeq), s.op};
                    }
                    labels.resize(kb.labelCount);
                    for (uint32_t i = 0; i < kb.labelCount; ++i)
                    {
                        uint32_t seq;
                        std::memcpy(&seq, body + labelsAt + i * 4u, 4);
                        labels[i] = tags.find(seq);
                    }
                    KickView k;
                    k.cycle = end.cycle;
                    k.index = kb.index;
                    k.channel = kb.channel;
                    k.mode = kb.mode;
                    k.tte = (kb.kflags & kKickTte) != 0;
                    k.chcr = kb.chcr;
                    k.madr = kb.madr;
                    k.qwc = kb.qwc;
                    k.tadr = kb.tadr;
                    k.asr0 = kb.asr0;
                    k.asr1 = kb.asr1;
                    k.callerRa = kb.callerRa;
                    k.mxcsr = kb.mxcsr;
                    k.fbrst = kb.fbrst;
                    k.dmaTags = kb.dmaTags;
                    k.streamBytes = kb.streamBytes;
                    k.gifBytes = kb.gifBytes;
                    k.streamHash = kb.streamHash;
                    k.gifHash = kb.gifHash;
                    k.hasGifHash = (kb.kflags & kKickHasGifHash) != 0;
                    k.field = kb.field;
                    k.spans = spans.data();
                    k.spanCount = kb.spanCount;
                    k.labels = labels.data();
                    k.labelCount = kb.labelCount;
                    alignas(8) static thread_local uint64_t regs[kGsRegCount];
                    if (kb.kflags & kKickHasRegs)
                    {
                        std::memcpy(regs, body + regsAt, sizeof(regs));
                        k.regs = regs;
                    }
                    if (deliver)
                        for (Consumer *c : consumers)
                            c->kick(k, view);
                    break;
                }
                case RecOdd:
                {
                    if (!need(sizeof(RecOddBody)))
                        break;
                    RecOddBody ob;
                    std::memcpy(&ob, body, sizeof(ob));
                    if (deliver)
                        for (Consumer *c : consumers)
                            c->odd(static_cast<int>(ob.kind), ob.address, ob.value);
                    break;
                }
                case RecNote:
                {
                    std::string text(reinterpret_cast<const char *>(body), bodyBytes);
                    while (!text.empty() && text.back() == '\0')
                        text.pop_back();
                    if (deliver)
                        for (Consumer *c : consumers)
                            c->note(text);
                    break;
                }
                case RecEnd:
                {
                    CycleInfo ciInfo;
                    ciInfo.end = end;
                    ciInfo.index = cycleIndex;
                    if (deliver)
                        for (Consumer *c : consumers)
                            c->cycleEnd(ciInfo, refRgba);
                    break;
                }
                default:
                    break; // unknown record types are skipped by their size
                }
            }
            tags.nextCycle();
            ++cycleIndex;
        }
        for (Consumer *c : consumers)
            c->close();
        return ok;
    }

    bool Reader::verify(std::string &report)
    {
        report.clear();
        bool ok = true;
        uint64_t cycles = 0, keys = 0;
        std::vector<uint64_t> keyOffsets, cyclOffsets;
        std::vector<uint8_t> raw;
        std::vector<std::vector<uint8_t>> store;
        for (const ChunkRef &ref : m_chunks)
        {
            const std::string where = idName(ref.header.id) + " at " + std::to_string(ref.offset);
            m_error.clear();
            if (ref.header.id == kChunkCycle)
                cyclOffsets.push_back(ref.offset);
            if (!readPayload(ref, raw))
            {
                report += "  " + m_error + "\n";
                ok = false;
                continue;
            }
            if (ref.header.id == kChunkKeyframe)
            {
                Keyframe kf;
                if (!parseKeyframe(raw, kf, store))
                {
                    report += "  " + m_error + "\n";
                    ok = false;
                }
                if (!(kf.header.flags & kKeyFinal))
                    keyOffsets.push_back(ref.offset);
                ++keys;
            }
            else if (ref.header.id == kChunkCycle)
            {
                ++cycles;
                bool sawEnd = false;
                uint64_t at = 0;
                for (; at + sizeof(RecHeader) <= raw.size();)
                {
                    RecHeader rh;
                    std::memcpy(&rh, raw.data() + at, sizeof(rh));
                    if (rh.bytes < sizeof(RecHeader) || (rh.bytes & 7u) || at + rh.bytes > raw.size())
                    {
                        report += "  bad record size in " + where + "\n";
                        ok = false;
                        break;
                    }
                    sawEnd = rh.type == RecEnd;
                    at += rh.bytes;
                }
                if (at != raw.size())
                {
                    report += "  trailing bytes in " + where + "\n";
                    ok = false;
                }
                if (!sawEnd)
                {
                    report += "  " + where + " does not end with END\n";
                    ok = false;
                }
            }
        }
        m_error.clear();
        if (m_header.indexOffset)
        {
            bool match = m_cycles.size() == cyclOffsets.size();
            for (size_t i = 0; match && i < cyclOffsets.size(); ++i)
                match = m_cycles[i].cyclOffset == cyclOffsets[i];
            bool segMatch = m_segments.size() <= keyOffsets.size();
            for (size_t i = 0; segMatch && i < m_segments.size(); ++i)
                segMatch = m_segments[i].offset == keyOffsets[i];
            if (!match || !segMatch)
            {
                report += "  the index does not match the chunks\n";
                ok = false;
            }
        }
        report = "chunks " + std::to_string(m_chunks.size() + 1) + ", keyframes " + std::to_string(keys) + ", cycles " +
                 std::to_string(cycles) + (m_complete ? ", complete" : (m_truncated ? ", cut short (partial chunk at the end)" : ", not closed")) +
                 (ok ? ": ok\n" : ": FAILED\n") + report;
        return ok;
    }
}

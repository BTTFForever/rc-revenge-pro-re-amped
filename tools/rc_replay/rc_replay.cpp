// rc_replay: feeds a frame record dump (.rcfr, RENDERER_FRAME_RECORD.md) to consumers.
//
//   rc_replay <dump> [--consumer stats] [--cycles a-b] [--verify] [--quiet]
//
// stats: per cycle and in total, kicks by channel and mode, stream bytes, DMA tags by id, pages and bytes by record
// type, odd events. With --verify: the host's chain walker (render/record/chain_walk) over the rebuilt RAM image
// reproduces every kick's stream hash, length and tag count, reads no page the image lacks, every resolved tag span
// lies on a DMA tag or a quadword of its range, and the dump holds no ODD event (but GfxP2_Init's two start-up VIF1
// FIFO stores, which are counted apart). Exit 0 pass, 1 a check failed,
// 2 unusable dump. The emu consumer (bit-exact replay through the runtime's VIF1, VU1 and software GS) comes with
// batch 2. One build replays both versions' dumps.
#include "chain_walk.h"
#include "consumer.h"
#include "reader.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace rc::rec;

namespace
{
    const char *recName(int t)
    {
        static const char *names[9] = {"?", "PAGE", "HW", "CRT", "KICK", "TAG", "ODD", "END", "NOTE"};
        return t >= 0 && t < 9 ? names[t] : "?";
    }

    std::string chunkName(uint32_t id)
    {
        std::string s(4, ' ');
        for (int i = 0; i < 4; ++i)
            s[i] = static_cast<char>((id >> (8 * i)) & 0xFF);
        return s;
    }

    struct SpanCheck final : WalkVisitor
    {
        std::vector<WalkRange> ranges;
        void range(const WalkRange &r) override { ranges.push_back(r); }
        bool valid(uint32_t offset) const
        {
            for (const WalkRange &r : ranges)
            {
                if (r.isTag && offset == r.streamOffset)
                    return true;
                if (!r.isTag && offset >= r.streamOffset && offset <= r.streamOffset + r.bytes && (offset - r.streamOffset) % 16u == 0u)
                    return true;
            }
            return false;
        }
    };

    class Stats final : public Consumer
    {
    public:
        Stats(bool verify, bool quiet) : m_verify(verify), m_quiet(quiet) {}

        void open(const FileInfo &info) override
        {
            const FileHeader &h = info.header;
            std::printf("dump %s\n  %s (%s), %ux%u at %.3f Hz, backend %.16s, build %.40s, format %u%s%s\n", info.path.c_str(),
                        h.gameVersion ? "Europe" : "USA", std::string(h.elfName, strnlen(h.elfName, 16)).c_str(), h.width, h.height,
                        h.fieldRateMilliHz / 1000.0, h.gsBackend, h.build, h.formatVersion, (h.flags & kFileExact) ? ", exact" : ", plain",
                        (h.flags & kFileComplete) ? "" : ", not closed");
        }
        void keyframe(const Keyframe &kf) override
        {
            ++m_keyframes;
            if (!m_quiet)
                std::printf("keyframe%s at cycle %" PRIu64 " (field %" PRIu64 "), %zu state chunks\n", (kf.header.flags & kKeyFinal) ? " (final)" : "",
                            kf.header.cycle, kf.header.field, kf.chunks.size());
            if (!(kf.header.flags & kKeyFinal) && (!kf.find(kStateGsMemory) || kf.find(kStateGsMemory)->bytes != kGsMemBytes))
                fail("keyframe at cycle " + std::to_string(kf.header.cycle) + " without 4 MB of GS memory");
        }
        void cycleBegin(uint64_t) override { m_c = Cycle{}; }
        void pages(uint32_t, uint32_t count, const uint8_t *) override
        {
            m_c.pages += count;
            m_t.pages += count;
        }
        void hw(uint32_t, uint32_t, uint64_t, uint32_t) override
        {
            ++m_c.hw;
            ++m_t.hw;
        }
        void odd(int kind, uint32_t address, uint64_t value) override
        {
            ++m_c.odd;
            // Known and harmless (RENDERER_SURVEY.md, "Traffic outside the planned kicks"): GfxP2_Init's two EE stores
            // into the VIF1 FIFO at start-up, before any VIF1 kick; sceGsResetGraph resets VIF1 (FBRST, recorded as
            // HW) before the first one.
            if (kind == 2 && address == 0x10005000u && !m_sawVif1Kick)
            {
                ++m_t.knownOdd;
                return;
            }
            ++m_t.odd;
            if (m_t.odd <= 20)
                std::printf("  ODD kind %d address 0x%08x value 0x%" PRIx64 "\n", kind, address, value);
        }
        void note(const std::string &text) override { std::printf("  note: %s\n", text.c_str()); }
        void kick(const KickView &k, const RamView &ram) override
        {
            ++m_c.kicks;
            ++m_t.kicks;
            m_sawVif1Kick |= k.channel == 1;
            const int slot = std::min<int>(k.channel, 2) * 2 + (k.mode == 1 ? 1 : 0);
            ++m_c.byKind[slot];
            ++m_t.byKind[slot];
            m_c.stream += k.streamBytes;
            m_t.stream += k.streamBytes;
            m_c.tags += k.dmaTags;
            m_t.tags += k.dmaTags;
            m_t.maxStream = std::max<uint64_t>(m_t.maxStream, k.streamBytes);
            if (!m_verify)
                return;
            SpanCheck spans;
            const WalkResult w = walkChain(k, ram, k.spanCount ? &spans : nullptr);
            ++m_t.walked;
            for (int i = 0; i < 8; ++i)
                m_t.tagIds[i] += w.tagIds[i];
            const std::string where = "cycle " + std::to_string(k.cycle) + " kick " + std::to_string(k.index) + " (ch" +
                                      std::to_string(k.channel) + (k.mode == 1 ? " chain" : " normal") + ")";
            if (w.streamHash != k.streamHash || w.streamBytes != k.streamBytes || (k.mode == 1 && w.dmaTags != k.dmaTags))
            {
                char buf[256];
                std::snprintf(buf, sizeof(buf), ": stream %" PRIu64 " bytes hash %016" PRIx64 " tags %u, recorded %u bytes hash %016" PRIx64 " tags %u",
                              w.streamBytes, w.streamHash, w.dmaTags, k.streamBytes, k.streamHash, k.dmaTags);
                fail(where + buf);
            }
            else
                ++m_t.hashOk;
            if (w.absentPageReads)
                fail(where + ": read " + std::to_string(w.absentPageReads) + " page(s) the image does not hold");
            if (w.untranslatable)
                fail(where + ": " + std::to_string(w.untranslatable) + " untranslatable address(es)");
            for (uint32_t i = 0; i < k.spanCount; ++i)
                if (!spans.valid(k.spans[i].streamOffset))
                    fail(where + ": tag span at stream offset " + std::to_string(k.spans[i].streamOffset) + " is not on a tag or quadword");
        }
        void cycleEnd(const CycleInfo &info, const uint8_t *) override
        {
            ++m_cycles;
            if (!m_quiet)
                std::printf("cycle %" PRIu64 " (game frame %" PRId64 ", field %" PRIu64 ", FBP 0x%x): %u kicks (ch1 chain %u, ch2 normal %u, "
                            "ch2 chain %u, other %u), stream %.1f KB, %u DMA tags, %u pages, %u hw, %u odd\n",
                            info.end.cycle, static_cast<int64_t>(info.end.gameFrame), info.end.field, info.end.refFbp, m_c.kicks, m_c.byKind[3],
                            m_c.byKind[4], m_c.byKind[5], m_c.kicks - m_c.byKind[3] - m_c.byKind[4] - m_c.byKind[5], m_c.stream / 1024.0,
                            m_c.tags, m_c.pages, m_c.hw, m_c.odd);
            if (info.end.kickCount != m_c.kicks)
                fail("cycle " + std::to_string(info.end.cycle) + ": END says " + std::to_string(info.end.kickCount) + " kicks, " +
                     std::to_string(m_c.kicks) + " recorded");
        }

        void summary(const Reader &r) const
        {
            const double cycles = std::max<double>(1.0, static_cast<double>(m_cycles));
            std::printf("total: %" PRIu64 " cycles, %" PRIu64 " keyframes, %" PRIu64 " kicks (ch0 %" PRIu64 "/%" PRIu64 ", ch1 normal %" PRIu64
                        " chain %" PRIu64 ", ch2 normal %" PRIu64 " chain %" PRIu64 "), stream %.1f MB (%.2f MB a cycle, largest kick %.2f MB), "
                        "%" PRIu64 " DMA tags, %" PRIu64 " pages captured (%.1f a cycle), %" PRIu64 " hw stores, %" PRIu64 " odd events (+%" PRIu64
                        " known start-up VIF1 FIFO stores)\n",
                        m_cycles, m_keyframes, m_t.kicks, m_t.byKind[0], m_t.byKind[1], m_t.byKind[2], m_t.byKind[3], m_t.byKind[4], m_t.byKind[5],
                        m_t.stream / 1048576.0, m_t.stream / cycles / 1048576.0, m_t.maxStream / 1048576.0, m_t.tags, m_t.pages, m_t.pages / cycles,
                        m_t.hw, m_t.odd, m_t.knownOdd);
            if (m_verify)
                std::printf("verify: %" PRIu64 " of %" PRIu64 " kicks walked by the host's chain walker reproduce the recorded stream hash; tags by id "
                            "refe %" PRIu64 " cnt %" PRIu64 " next %" PRIu64 " ref %" PRIu64 " refs %" PRIu64 " call %" PRIu64 " ret %" PRIu64
                            " end %" PRIu64 "\n",
                            m_t.hashOk, m_t.walked, m_t.tagIds[0], m_t.tagIds[1], m_t.tagIds[2], m_t.tagIds[3], m_t.tagIds[4], m_t.tagIds[5],
                            m_t.tagIds[6], m_t.tagIds[7]);
            std::printf("records (raw):");
            for (int t = 1; t < 9; ++t)
                if (r.recordCount[t])
                    std::printf(" %s %" PRIu64 " %.1f MB (%.1f KB a cycle);", recName(t), r.recordCount[t], r.recordBytes[t] / 1048576.0,
                                r.recordBytes[t] / cycles / 1024.0);
            std::printf("\nchunks:");
            for (const auto &[id, cs] : r.chunkStats)
                std::printf(" %s %" PRIu64 " %.1f MB raw -> %.1f MB stored;", chunkName(id).c_str(), cs.count, cs.raw / 1048576.0,
                            cs.stored / 1048576.0);
            const auto cy = r.chunkStats.find(kChunkCycle);
            if (cy != r.chunkStats.end())
                std::printf("\nper cycle: %.1f KB raw, %.1f KB stored", cy->second.raw / cycles / 1024.0, cy->second.stored / cycles / 1024.0);
            std::printf("\nfile %.1f MB\n", r.fileBytes() / 1048576.0);
        }

        bool failed() const { return m_failures != 0; }
        uint64_t failures() const { return m_failures; }
        uint64_t odds() const { return m_t.odd; }

    private:
        void fail(const std::string &what)
        {
            if (++m_failures <= 30)
                std::printf("FAIL %s\n", what.c_str());
        }
        struct Cycle
        {
            uint32_t kicks = 0, byKind[6] = {}, tags = 0, pages = 0, hw = 0, odd = 0;
            uint64_t stream = 0;
        };
        struct Total
        {
            uint64_t kicks = 0, byKind[6] = {}, tags = 0, pages = 0, hw = 0, odd = 0, knownOdd = 0, stream = 0, maxStream = 0, walked = 0,
                     hashOk = 0;
            uint64_t tagIds[8] = {};
        };
        bool m_verify, m_quiet;
        bool m_sawVif1Kick = false;
        Cycle m_c;
        Total m_t;
        uint64_t m_cycles = 0, m_keyframes = 0, m_failures = 0;
    };

    void usage()
    {
        std::fprintf(stderr, "usage: rc_replay <dump.rcfr> [--consumer stats] [--cycles a-b] [--verify] [--quiet]\n");
    }
}

int main(int argc, char **argv)
{
    std::string path, consumer = "stats";
    bool verify = false, quiet = false;
    uint64_t first = 0, last = ~0ull;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--consumer" && i + 1 < argc)
            consumer = argv[++i];
        else if (a == "--verify")
            verify = true;
        else if (a == "--quiet")
            quiet = true;
        else if (a == "--cycles" && i + 1 < argc)
        {
            const std::string range = argv[++i];
            const size_t dash = range.find('-');
            first = std::strtoull(range.c_str(), nullptr, 10);
            last = dash == std::string::npos ? first : (dash + 1 < range.size() ? std::strtoull(range.c_str() + dash + 1, nullptr, 10) : ~0ull);
        }
        else if (a[0] != '-' && path.empty())
            path = a;
        else
        {
            usage();
            return 2;
        }
    }
    if (path.empty())
    {
        usage();
        return 2;
    }
    if (consumer != "stats")
    {
        std::fprintf(stderr, "consumer '%s' is not built yet (only stats; emu comes with batch 2)\n", consumer.c_str());
        return 2;
    }
    Reader reader;
    if (!reader.open(path))
    {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), reader.error().c_str());
        return 2;
    }
    Stats stats(verify, quiet);
    const bool ok = reader.run({&stats}, first, last);
    stats.summary(reader);
    if (!reader.complete())
        std::printf("note: the dump was not closed (%s); read up to its last whole chunk\n",
                    reader.truncated() ? "a partial chunk at the end" : "no index");
    if (!ok)
    {
        std::printf("read error: %s\n", reader.error().c_str());
        return 2;
    }
    if (verify)
    {
        const bool pass = !stats.failed() && stats.odds() == 0;
        std::printf("%s (%" PRIu64 " failures, %" PRIu64 " odd events)\n", pass ? "PASS" : "FAIL", stats.failures(), stats.odds());
        return pass ? 0 : 1;
    }
    return 0;
}

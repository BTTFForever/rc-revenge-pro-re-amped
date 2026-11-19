// rc_record_test: the frame record container's round trip without the game (RENDERER_FRAME_RECORD.md section 12,
// batch 1). Builds guest RAM with DMA chains of every tag kind, records synthetic cycles through the page tracker and
// the writer as the live capture does, reads the dump back and checks every kick with the host's chain walker; then
// cuts the file at several points and checks each cut dump reads up to its last whole chunk; then flips a byte and
// checks the reader notices.
//
//   rc_record_test <scratch folder>      exit 0 pass, 1 fail
#include "chain_walk.h"
#include "consumer.h"
#include "page_tracker.h"
#include "reader.h"
#include "writer.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace rc::rec;

namespace
{
    int g_failures = 0;
    void check(bool ok, const std::string &what)
    {
        if (!ok)
        {
            ++g_failures;
            std::printf("FAIL %s\n", what.c_str());
        }
    }

    struct Machine
    {
        std::unique_ptr<uint8_t[]> ram{new uint8_t[kRamBytes]()};
        std::unique_ptr<uint8_t[]> spr{new uint8_t[kScratchpadBytes]()};
        RamView view() const { return {ram.get(), spr.get(), nullptr, nullptr}; }

        void tag(uint32_t at, uint32_t id, uint32_t qwc, uint32_t addr, bool irq = false, uint64_t upper = 0)
        {
            const uint64_t t = (qwc & 0xFFFFu) | static_cast<uint64_t>(id & 7u) << 28 | (irq ? 1ull << 31 : 0ull) |
                               static_cast<uint64_t>(addr) << 32;
            std::memcpy(ram.get() + at, &t, 8);
            std::memcpy(ram.get() + at + 8, &upper, 8);
        }
        void fill(uint32_t at, uint32_t bytes, uint32_t seed)
        {
            for (uint32_t i = 0; i < bytes; ++i)
                ram[at + i] = static_cast<uint8_t>((i * 131u + seed * 7u) >> 2);
        }
    };

    // Plays the runtime's part: touches what the walk reads, captures pages, writes the kick.
    struct Recorder
    {
        Machine &m;
        PageTracker tracker;
        Writer &writer;
        uint32_t kickIndex = 0;
        uint64_t cycle = 0;

        struct Toucher final : WalkVisitor
        {
            PageTracker &t;
            explicit Toucher(PageTracker &tracker) : t(tracker) {}
            void range(const WalkRange &r) override { t.touch(r.physAddr, r.bytes, r.scratch); }
        };

        void kick(uint8_t channel, uint32_t chcr, uint32_t madr, uint32_t qwc, uint32_t tadr)
        {
            KickView k;
            k.cycle = cycle;
            k.index = kickIndex++;
            k.channel = channel;
            k.mode = static_cast<uint8_t>((chcr >> 2) & 3u);
            k.tte = (chcr & 0x40u) != 0u;
            k.chcr = chcr;
            k.madr = madr;
            k.qwc = qwc;
            k.tadr = tadr;
            tracker.beginKick();
            Toucher toucher(tracker);
            const WalkResult w = walkChain(k, m.view(), &toucher);
            tracker.collect(m.ram.get(), m.spr.get(), [&](uint32_t f, uint32_t c, const uint8_t *d) { writer.pages(f, c, d); });
            k.dmaTags = w.dmaTags;
            k.streamBytes = static_cast<uint32_t>(w.streamBytes);
            k.streamHash = w.streamHash;
            writer.kick(k, m.view());
        }
    };

    struct Verifier final : Consumer
    {
        uint64_t kicks = 0, ok = 0, cycles = 0, keyframes = 0, finals = 0, hwCount = 0, absent = 0;
        std::vector<uint64_t> streamBytes;
        void keyframe(const Keyframe &kf) override
        {
            ++keyframes;
            if (kf.header.flags & kKeyFinal)
                ++finals;
            const StateChunk *gs = kf.find(kStateGsMemory);
            check(gs && gs->bytes == kGsMemBytes && gs->data[12345] == static_cast<uint8_t>(12345u * 3u), "keyframe GS memory");
        }
        void hw(uint32_t, uint32_t, uint64_t, uint32_t) override { ++hwCount; }
        void kick(const KickView &k, const RamView &ram) override
        {
            ++kicks;
            const WalkResult w = walkChain(k, ram);
            absent += w.absentPageReads;
            if (w.streamHash == k.streamHash && w.streamBytes == k.streamBytes && w.dmaTags == k.dmaTags && !w.absentPageReads)
                ++ok;
            streamBytes.push_back(w.streamBytes);
        }
        void cycleEnd(const CycleInfo &, const uint8_t *) override { ++cycles; }
    };

    std::vector<uint8_t> readFile(const std::string &p)
    {
        std::ifstream f(p, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    void writeFile(const std::string &p, const uint8_t *d, size_t n)
    {
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char *>(d), static_cast<std::streamsize>(n));
    }
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: rc_record_test <scratch folder>\n");
        return 2;
    }
    const std::string dir = argv[1];
    std::filesystem::create_directories(dir);
    const std::string path = dir + "/synthetic.rcfr";

    Machine m;
    // A VIF1 chain at 0x100000 with every tag kind: cnt, ref, refs (into the scratchpad), call/ret (two deep),
    // next, refe; a second chain (GIF) ending in end; payloads straddling pages; data at the very end of RDRAM
    // (the walk wraps).
    m.fill(0x100000, 0x40000, 1);
    m.fill(0x200000, 0x3000, 2);
    m.fill(0x300000, 0x3000, 3);
    m.fill(0x500000, 0x2000, 4);
    m.fill(kRamBytes - 0x20, 0x20, 5);
    for (uint32_t i = 0; i < 0x100; ++i)
        m.spr[0x100 + i] = static_cast<uint8_t>(i ^ 0x5A);
    m.tag(0x100000, 1, 2, 0, false, 0x1111111122222222ull);    // cnt, 2 qw follow
    m.tag(0x100030, 3, 0x180, 0x200FF0);                     // ref: 6 KB from 0x200ff0 (across pages)
    m.tag(0x100040, 4, 0x10, 0x70000100);                    // refs: the scratchpad
    m.tag(0x100050, 5, 1, 0x300000);                         // call 0x300000 (1 qw follows)
    m.tag(0x300000, 5, 0, 0x300800);                         //   call again (two deep)
    m.tag(0x300800, 1, 3, 0);                                //     cnt
    m.tag(0x300840, 6, 0, 0);                                //     ret
    m.tag(0x300010, 3, 2, kRamBytes - 0x20);                 //   ref: the last 32 bytes of RAM
    m.tag(0x300020, 6, 1, 0);                                //   ret (1 qw)
    m.tag(0x100070, 2, 1, 0x500000);                         // next 0x500000
    m.tag(0x500000, 0, 0x20, 0x500100);                      // refe: ends
    // GIF chain at 0x600000: cnt, then end.
    m.fill(0x600000, 0x200, 6);
    m.tag(0x600000, 1, 4, 0);
    m.tag(0x600050, 7, 2, 0);

    Writer writer(path);
    check(writer.ok(), "writer opened");
    FileInfo info;
    info.header.width = 640;
    info.header.height = 240;
    info.header.fieldRateMilliHz = 59940;
    info.header.ramBytes = kRamBytes;
    info.header.scratchpadBytes = kScratchpadBytes;
    info.header.pageBytes = kPageBytes;
    info.header.gsMemBytes = kGsMemBytes;
    std::strcpy(info.header.elfName, "SYNTHETIC");
    writer.open(info);

    std::vector<uint8_t> gs(kGsMemBytes);
    for (uint32_t i = 0; i < kGsMemBytes; ++i)
        gs[i] = static_cast<uint8_t>(i * 3u);
    auto keyframe = [&](uint64_t cycle, bool final, Recorder &rec)
    {
        Keyframe kf;
        kf.header.cycle = cycle;
        kf.header.flags = final ? kKeyFinal : 0u;
        kf.chunks.push_back({kStateGsMemory, gs.data(), gs.size()});
        writer.keyframe(kf);
        if (!final)
            rec.tracker.reset();
    };

    Recorder rec{m, PageTracker{}, writer};
    constexpr int kCycles = 8;
    uint64_t kicks = 0;
    keyframe(100, false, rec);
    for (int c = 0; c < kCycles; ++c)
    {
        rec.cycle = 100 + c;
        rec.kickIndex = 0;
        writer.cycleBegin(rec.cycle);
        if (c == 4)
            keyframe(rec.cycle, false, rec); // a second segment (written before the cycle's records, as capture does)
        writer.hw(0x12000000u, 64u, 0x66u, 0x1234u);
        // Change data between cycles: a payload byte (page re-emitted), a tag's qwc (stream length changes).
        m.ram[0x200FF0 + c] ^= 0xFFu;
        m.tag(0x600000, 1, 4 + (c & 1), 0);
        m.tag(0x600050 + (c & 1) * 16, 7, 2, 0);
        m.spr[0x100 + c] ^= 0x33u;
        rec.kick(2, (1u << 2) | 0x100u, 0, 0, 0x600000);                 // GIF chain
        rec.kick(1, (1u << 2) | 0x40u | 0x100u, 0, 0, 0x100000);         // VIF1 chain with TTE
        rec.kick(2, 0x100u, kRamBytes - 0x10, 3, 0);                     // normal mode wrapping at the end of RAM
        rec.kick(2, 0x100u, 0x70000100, 2, 0);                           // normal mode from the scratchpad
        kicks += 4;
        CycleInfo ci;
        ci.end.cycle = rec.cycle;
        ci.end.kickCount = 4;
        writer.hw(0x12000090u, 64u, (c & 1) ? 0x50u : 0u, 0x1234u);
        writer.cycleEnd(ci, nullptr);
    }
    keyframe(100 + kCycles, true, rec);
    writer.close();
    std::printf("wrote %s: %.1f KB, %" PRIu64 " cycles, %" PRIu64 " keyframes\n", path.c_str(), writer.stats().fileBytes / 1024.0,
                writer.stats().cycles, writer.stats().keyframes);

    // Read back.
    {
        Reader r;
        check(r.open(path), "open: " + r.error());
        check(r.complete(), "closed dump reads as complete");
        check(r.segments().size() == 2 && r.cycles().size() == kCycles, "index: 2 segments, 8 cycles");
        std::string report;
        check(r.verify(report), "verify: " + report);
        std::printf("verify: %s", report.c_str());
        Verifier v;
        check(r.run({&v}), "run: " + r.error());
        std::printf("read back: %" PRIu64 " cycles, %" PRIu64 " kicks, %" PRIu64 " of them reproduced by the walker, %" PRIu64
                    " keyframes (%" PRIu64 " final)\n",
                    v.cycles, v.kicks, v.ok, v.keyframes, v.finals);
        check(v.cycles == kCycles && v.kicks == kicks && v.ok == kicks && v.keyframes == 3 && v.finals == 1 && v.absent == 0,
              "every kick reproduced");
        // Starting from the second segment.
        Verifier v2;
        check(r.run({&v2}, 104), "run from cycle 104");
        check(v2.cycles == 4 && v2.ok == 16, "the second segment replays alone (" + std::to_string(v2.ok) + " kicks ok)");
    }

    // Cut the file at several points: each reads up to its last whole chunk.
    const std::vector<uint8_t> whole = readFile(path);
    std::vector<uint64_t> cyclEnds; // file offset after each CYCL chunk
    {
        uint64_t at = 0;
        while (at + sizeof(ChunkHeader) <= whole.size())
        {
            ChunkHeader h;
            std::memcpy(&h, whole.data() + at, sizeof(h));
            at += sizeof(h) + h.storedBytes + pad8(h.storedBytes);
            if (h.id == kChunkCycle)
                cyclEnds.push_back(at);
        }
    }
    int cuts = 0;
    for (uint64_t cut : {cyclEnds[2], cyclEnds[2] + 1, cyclEnds[5] - 9, static_cast<uint64_t>(whole.size() * 6 / 10), static_cast<uint64_t>(whole.size() - 1)})
    {
        const std::string p = dir + "/cut.rcfr";
        writeFile(p, whole.data(), cut);
        uint64_t whole_cycles = 0;
        for (uint64_t e : cyclEnds)
            whole_cycles += e <= cut;
        Reader r;
        const bool opened = r.open(p);
        check(opened, "cut at " + std::to_string(cut) + ": open: " + r.error());
        if (!opened)
            continue;
        Verifier v;
        const bool ran = r.run({&v});
        const bool expectComplete = cut == whole.size();
        check(ran && v.cycles == whole_cycles && v.ok == v.kicks && r.complete() == expectComplete,
              "cut at " + std::to_string(cut) + ": read " + std::to_string(v.cycles) + " cycles, expected " + std::to_string(whole_cycles));
        std::printf("cut at %" PRIu64 " of %zu bytes: %s, %" PRIu64 " whole cycles read, all %" PRIu64 " kicks reproduced\n", cut, whole.size(),
                    r.complete() ? "complete" : (r.truncated() ? "partial chunk at the end" : "not closed"), v.cycles, v.ok);
        ++cuts;
    }

    // A flipped byte in a PAGE payload is caught (CRC) and a dropped PAGE makes the walk read an absent page.
    {
        std::vector<uint8_t> bad = whole;
        const uint64_t at = cyclEnds[0] - 64; // inside the first cycle's compressed payload
        bad[at] ^= 0x01u;
        const std::string p = dir + "/flipped.rcfr";
        writeFile(p, bad.data(), bad.size());
        Reader r;
        std::string report;
        check(r.open(p), "flipped: open");
        const bool verified = r.verify(report);
        check(!verified, "flipped byte detected");
        std::printf("flipped byte: %s", report.c_str());
    }
    {
        // A tracker that forgets a page: the replay's walk must read an absent page.
        Machine m2;
        m2.fill(0x100000, 0x100, 9);
        m2.tag(0x100000, 3, 0x100, 0x400000); // ref 4 KB at 0x400000
        m2.tag(0x100010, 7, 0, 0);
        m2.fill(0x400000, 0x1000, 3);
        const std::string p = dir + "/dropped.rcfr";
        {
            Writer w(p);
            w.open(info);
            Keyframe kf;
            kf.chunks.push_back({kStateGsMemory, gs.data(), gs.size()});
            w.keyframe(kf);
            w.cycleBegin(0);
            PageTracker t;
            t.beginKick();
            t.touch(0x100000, 0x20, false); // the payload's page is never touched
            t.collect(m2.ram.get(), m2.spr.get(), [&](uint32_t f, uint32_t c, const uint8_t *d) { w.pages(f, c, d); });
            KickView k;
            k.channel = 1;
            k.mode = 1;
            k.chcr = 0x104u;
            k.tadr = 0x100000;
            const WalkResult wr = walkChain(k, m2.view());
            k.dmaTags = wr.dmaTags;
            k.streamBytes = static_cast<uint32_t>(wr.streamBytes);
            k.streamHash = wr.streamHash;
            k.index = 0;
            w.kick(k, m2.view());
            CycleInfo ci;
            ci.end.kickCount = 1;
            w.cycleEnd(ci, nullptr);
            w.close();
        }
        Reader r;
        check(r.open(p), "dropped: open");
        Verifier v;
        r.run({&v});
        check(v.absent > 0 && v.ok == 0, "a dropped page is detected");
        std::printf("dropped page: %" PRIu64 " absent page read(s), %" PRIu64 " of %" PRIu64 " kicks reproduced\n", v.absent, v.ok, v.kicks);
    }

    // (batch 4) Ring mode: keep at least 5 cycles in segments of 3; a dump after cycle 107 holds 103-107 (two segments),
    // one after cycle 111 holds 106-111; each replays whole with its final keyframe.
    {
        Writer ring(Writer::Ring{5});
        check(ring.ok() && ring.ringMode(), "ring writer");
        ring.open(info);
        Recorder rr{m, PageTracker{}, ring};
        auto key = [&](uint64_t cycle, bool final)
        {
            Keyframe kf;
            kf.header.cycle = cycle;
            kf.header.flags = final ? kKeyFinal : 0u;
            kf.chunks.push_back({kStateGsMemory, gs.data(), gs.size()});
            ring.keyframe(kf);
            if (!final)
                rr.tracker.reset();
        };
        const std::string d1 = dir + "/ring1.rcfr", d2 = dir + "/ring2.rcfr";
        for (int c = 0; c < 12; ++c)
        {
            rr.cycle = 100 + c;
            rr.kickIndex = 0;
            ring.cycleBegin(rr.cycle);
            if (c % 3 == 0)
                key(rr.cycle, false);
            m.ram[0x200FF0 + c] ^= 0xFFu;
            rr.kick(2, (1u << 2) | 0x100u, 0, 0, 0x600000);
            rr.kick(1, (1u << 2) | 0x40u | 0x100u, 0, 0, 0x100000);
            CycleInfo ci;
            ci.end.cycle = rr.cycle;
            ci.end.kickCount = 2;
            ring.cycleEnd(ci, nullptr);
            if (c == 7 || c == 11)
            {
                key(rr.cycle + 1, true);
                ring.dump(c == 7 ? d1 : d2);
            }
        }
        ring.close();
        const Writer::RingStats rs = ring.ringStats();
        check(rs.dumps == 2, "two ring dumps");
        struct Want
        {
            const std::string *path;
            uint64_t first, cycles, segments;
        };
        for (const Want &w : {Want{&d1, 103, 5, 2}, Want{&d2, 106, 6, 2}})
        {
            Reader r;
            check(r.open(*w.path), "ring dump: open: " + r.error());
            check(r.complete(), "ring dump complete");
            std::string report;
            check(r.verify(report), "ring dump verify: " + report);
            Verifier v;
            check(r.run({&v}), "ring dump run: " + r.error());
            const bool shape = r.segments().size() == w.segments && r.cycles().size() == w.cycles && !r.segments().empty() &&
                               r.segments()[0].firstCycle == w.first;
            check(shape && v.cycles == w.cycles && v.kicks == 2 * w.cycles && v.ok == v.kicks && v.finals == 1 && v.absent == 0,
                  "ring dump " + *w.path + ": " + std::to_string(r.segments().size()) + " segments, " + std::to_string(v.cycles) + " cycles, " +
                      std::to_string(v.ok) + " of " + std::to_string(v.kicks) + " kicks reproduced");
            std::printf("ring dump: %zu segments from cycle %" PRIu64 ", %" PRIu64 " cycles, all %" PRIu64 " kicks reproduced, final keyframe %s\n",
                        r.segments().size(), r.segments().empty() ? 0 : r.segments()[0].firstCycle, v.cycles, v.ok, v.finals ? "yes" : "no");
        }
    }

    std::printf("%s (%d failures, %d cut files)\n", g_failures ? "FAIL" : "PASS", g_failures, cuts);
    return g_failures ? 1 : 0;
}

// rc_replay: feeds a frame record dump (.rcfr, RENDERER_FRAME_RECORD.md) to consumers.
//
//   rc_replay <dump> [--consumer stats|emu|tex|blit|gif2d|gif2dstat] [--cycles a-b] [--verify] [--quiet]
//             [--png <dir>] [--picture-before <cycle>:<kick> <out.png>] [--path1 <cycle>:<kick> <out.bin>] [--kick-hashes]
//             [--fault page|kick|hw:<cycle>[:<n>]]
//             [--with-emu] [--fault transfer|kick|swizzle|decode:<cycle>:<n>] (tex)
//             [--no-gpu] [--validation] [--fault tile|dispfb|byte|gpubyte:<cycle>[:<n>]] (blit)
//
// stats: per cycle and in total, kicks by channel and mode, stream bytes, DMA tags by id, pages and bytes by record
// type, odd events. With --verify: the host's chain walker (render/record/chain_walk) over the rebuilt RAM image
// reproduces every kick's stream hash, length and tag count, reads no page the image lacks, every resolved tag span
// lies on a DMA tag or a quadword of its range, and the dump holds no ODD event (but GfxP2_Init's two start-up VIF1
// FIFO stores, which are counted apart). Exit 0 pass, 1 a check failed,
// 2 unusable dump.
//
// emu (emu.cpp): a headless runtime replays every recorded store and kick through its own VIF1, VU1 and software GS.
// With --verify on an exact-mode dump every kick's GIF output, every GS register file before a kick, every reference
// picture and GS memory at every later keyframe and the final one must come out bit for bit. --png writes the replayed
// pictures (and on a mismatch the reference and a difference image); --picture-before writes the frame buffer just
// before a kick; --path1 the GIF bytes a kick produced; --kick-hashes prints a GS memory hash after every kick;
// --fault plants one fault (a flipped page byte, a dropped kick, a wrong store) to show the checks catch it.
//
// tex (tex.cpp; renderer plan P1 step 5): the host's shadow of GS texture memory (render/tex). It applies the PATH3
// kicks' host-to-local transfers and with --verify must equal GSVR on every texture block at every later keyframe and
// the final one. --with-emu runs it behind emu: texture memory compared after every kick, every texture any path draws
// with decoded and compared with the emulated GS's own sampling, the alpha histogram by draw state. --png writes the
// decoded textures. --fault plants a dropped transfer or a layout error.
//
// blit (blit.cpp; renderer plan P1 step 6): the displayed picture built from uploads into the frame buffer and DISPFB2
// (render/blit), on the CPU and through the RHI on Vulkan (headless). Each cycle is upload-only (no word of its picture
// may have been drawn) or not applicable; with --verify every upload-only picture must equal the reference bit for
// bit (REFI or END.refHash), and the GPU's read-back the reference with alpha 255 (the presentation transform).
// --no-gpu skips the GPU; --fault plants a dropped tile, a wrong DISPFB2, a flipped byte (CPU and GPU, or GPU only).
// One build replays both versions' dumps.
//
// gif2d (gif2d.cpp; renderer plan P1 step 7, RENDERER_2D_DESIGN.md section 10): the 2D reader (render/gs2d) builds a
// draw list per cycle. With --verify: (a) every register it knows equals the kick's recorded register file; with
// --with-emu, (b) every drawing PATH3 kick is replaced by its draw list encoded back into GIF packets in front of the
// emulated GS, whose own verification must then hold. Prints the section 3 tables from the lists (H lines);
// --no-tables skips them. --fault reg|prim|field:<cycle>:<n> plants a lost register (every write of GS register n in the cycle), a dropped
// primitive (n of the cycle), a zeroed GsState field (n, gs2d_encode.h GsField).
#include "chain_walk.h"
#include "emu.h"
#include "tex.h"
#include "blit.h"
#include "gif2d_survey.h"
#include "gif2d.h"
#include "consumer.h"
#include "reader.h"
#include "../../render/gfx/tags.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "ps2_runtime.h"

#if RC_HAVE_VU1_CODE
void registerGeneratedVu1Programs();
#endif

// No recompiled game code is linked: the runtime's function table is empty (the emu machine runs no guest code).
extern const uint32_t g_ps2RecompiledFunctionTableBase = 0u;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0u;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 0u;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[1] = {};

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
        // (batch 3) The span's stream offset is where the walk delivers the guest address the tag names: a DMA tag's own
        // address, or that address inside (or at the end of) a payload range.
        bool addressMatches(uint32_t offset, uint32_t guestAddr) const
        {
            for (const WalkRange &r : ranges)
            {
                const uint32_t base = r.scratch ? 0x70000000u + r.physAddr : r.physAddr;
                if (r.isTag && offset == r.streamOffset && guestAddr == base)
                    return true;
                if (!r.isTag && offset >= r.streamOffset && offset <= r.streamOffset + r.bytes && guestAddr == base + (offset - r.streamOffset))
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
            // (batch 3) The tag name table.
            std::istringstream names(info.tagNames);
            std::string table, name;
            uint32_t value;
            while (names >> table >> value >> name)
                m_names[table + ":" + std::to_string(value)] = name;
        }
        std::string name(const char *table, uint32_t value) const
        {
            const auto it = m_names.find(std::string(table) + ":" + std::to_string(value));
            return it != m_names.end() ? it->second : std::string(table) + std::to_string(value);
        }
        // A tag's class for the statistics: its kind, and for a Draw or Hud begin what it draws.
        std::string tagClass(const TagView &t) const
        {
            std::string c = name("kind", t.kind);
            if ((t.kind == rc::gfx::KindDraw || t.kind == rc::gfx::KindHud) && t.op == rc::gfx::OpBegin && t.payloadBytes >= sizeof(rc::gfx::DrawKey))
            {
                rc::gfx::DrawKey key;
                std::memcpy(&key, t.payload, sizeof(key));
                c += "/" + name("draw", key.kind);
                if (t.kind == rc::gfx::KindHud)
                    c += "/" + name("hud", key.part);
            }
            return c;
        }
        // What a kick is, by its innermost label (or its channel and mode without one).
        std::string kickClass(const KickView &k) const
        {
            for (uint32_t i = k.labelCount; i-- > 0;)
                if (const TagView *t = k.labels[i]; t && t->payloadBytes >= sizeof(rc::gfx::KickLabel))
                {
                    rc::gfx::KickLabel l;
                    std::memcpy(&l, t->payload, sizeof(l));
                    return name("kick", l.what);
                }
            return std::string("unlabelled ch") + std::to_string(k.channel) + (k.mode == 1 ? " chain" : " normal");
        }
        void tag(const TagView &t) override
        {
            ++m_tagKinds[name("kind", t.kind) + (t.op == rc::gfx::OpBegin ? " begin" : t.op == rc::gfx::OpEnd ? " end" : "")];
            m_lastTagCycle = m_cycle;
            // Anchored: a begin or end of a Draw or Hud span (kick spans and set tags have no anchor).
            if (t.op != rc::gfx::OpSet && t.kind != rc::gfx::KindKick)
            {
                AnchoredTag a;
                a.cycle = m_cycle;
                a.cls = t.op == rc::gfx::OpBegin ? tagClass(t) : std::string();
                a.op = t.op;
                a.parent = t.parent;
                a.anchor = t.anchor;
                m_anchored[t.seq] = a;
            }
            if (t.op == rc::gfx::OpBegin)
                m_beginClass[t.seq] = tagClass(t);
        }
        void keyframe(const Keyframe &kf) override
        {
            ++m_keyframes;
            if (!m_quiet)
                std::printf("keyframe%s%s at cycle %" PRIu64 " (field %" PRIu64 "), %zu state chunks\n", (kf.header.flags & kKeyFinal) ? " (final)" : "",
                            (kf.header.flags & kKeyAfterStateLoad) ? " (after a save-state load)" : "", kf.header.cycle, kf.header.field,
                            kf.chunks.size());
            if (!(kf.header.flags & kKeyFinal) && (!kf.find(kStateGsMemory) || kf.find(kStateGsMemory)->bytes != kGsMemBytes))
                fail("keyframe at cycle " + std::to_string(kf.header.cycle) + " without 4 MB of GS memory");
        }
        void cycleBegin(uint64_t cycle) override
        {
            m_c = Cycle{};
            m_cycle = cycle;
            m_lastCycle = cycle;
        }
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
            // (batch 3) Labels and spans.
            const std::string kc = kickClass(k);
            auto &kcs = m_kickClasses[kc];
            ++kcs.kicks;
            kcs.bytes += k.streamBytes;
            for (uint32_t i = 0; i < k.spanCount; ++i)
            {
                const Span &sp = k.spans[i];
                if (!sp.tag)
                {
                    ++m_spanUnknownTag;
                    continue;
                }
                const auto it = m_anchored.find(sp.tag->seq);
                if (it != m_anchored.end())
                {
                    if (it->second.resolved)
                        ++m_spanTwice;
                    it->second.resolved = true;
                }
                if (sp.op == rc::gfx::OpBegin)
                {
                    ++m_spansIn[tagClass(*sp.tag) + " in " + kc];
                    // Its end in the same kick, at or after it.
                    bool paired = false;
                    for (uint32_t j = 0; j < k.spanCount && !paired; ++j)
                        paired = k.spans[j].op == rc::gfx::OpEnd && k.spans[j].tag && k.spans[j].tag->parent == sp.tag->seq &&
                                 k.spans[j].streamOffset >= sp.streamOffset && k.spans[j].streamOffset <= k.streamBytes;
                    ++(paired ? m_pairsOk : m_pairsOpen);
                    if (sp.streamOffset > k.streamBytes)
                        fail("cycle " + std::to_string(k.cycle) + " kick " + std::to_string(k.index) + ": span beyond the stream");
                }
            }
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
            {
                if (!spans.valid(k.spans[i].streamOffset))
                    fail(where + ": tag span at stream offset " + std::to_string(k.spans[i].streamOffset) + " is not on a tag or quadword");
                if (spans.addressMatches(k.spans[i].streamOffset, k.spans[i].guestAddr))
                    ++m_spanAddrOk;
                else
                    fail(where + ": tag span at stream offset " + std::to_string(k.spans[i].streamOffset) + " does not deliver guest address " +
                         std::to_string(k.spans[i].guestAddr));
            }
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
            if (!m_tagKinds.empty() || !m_names.empty())
            {
                std::printf("tags:");
                for (const auto &[k, n] : m_tagKinds)
                    std::printf(" %s %" PRIu64 ";", k.c_str(), n);
                std::printf("\nkicks by label:");
                for (const auto &[k, v] : m_kickClasses)
                    std::printf(" %s %" PRIu64 " (%.1f MB);", k.c_str(), v.kicks, v.bytes / 1048576.0);
                std::printf("\nspans (begin marks) by tag and kick:");
                for (const auto &[k, n] : m_spansIn)
                    std::printf(" %s %" PRIu64 ";", k.c_str(), n);
                // Anchored tags no kick consumed. Those of the last two cycles may be consumed after the dump ends.
                uint64_t resolved = 0, late = 0;
                std::map<std::string, uint64_t> unresolved;
                for (const auto &[seq, a] : m_anchored)
                {
                    if (a.resolved)
                        ++resolved;
                    else if (a.cycle + 2u > m_lastCycle)
                        ++late;
                    else
                    {
                        std::string cls = a.cls;
                        if (a.op == rc::gfx::OpEnd)
                        {
                            const auto b = m_beginClass.find(a.parent);
                            cls = (b != m_beginClass.end() ? b->second : std::string("?")) + " end";
                        }
                        ++unresolved[cls];
                    }
                }
                uint64_t unresolvedTotal = 0;
                for (const auto &[c, n] : unresolved)
                    unresolvedTotal += n;
                std::printf("\nanchored tags: %zu made in the replayed cycles, %" PRIu64 " resolved to a kick, %" PRIu64
                            " unresolved, %" PRIu64 " of the last two cycles not judged; begin/end pairs in one kick %" PRIu64 ", begins without "
                            "their end in the kick %" PRIu64,
                            m_anchored.size(), resolved, unresolvedTotal, late, m_pairsOk, m_pairsOpen);
                for (const auto &[c, n] : unresolved)
                    std::printf("; unresolved %s %" PRIu64, c.c_str(), n);
                if (m_verify)
                    std::printf("\nverify: %" PRIu64 " spans deliver the guest address their tag names at their stream offset; %" PRIu64
                                " spans name a tag the reader does not hold, %" PRIu64 " tags resolved twice",
                                m_spanAddrOk, m_spanUnknownTag, m_spanTwice);
                std::printf("\n");
            }
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
        struct AnchoredTag
        {
            uint64_t cycle = 0;
            std::string cls;
            uint8_t op = 0;
            uint32_t parent = 0, anchor = 0;
            bool resolved = false;
        };
        struct KickClass
        {
            uint64_t kicks = 0, bytes = 0;
        };
        std::map<std::string, std::string> m_names;
        std::map<std::string, uint64_t> m_tagKinds, m_spansIn;
        std::map<std::string, KickClass> m_kickClasses;
        std::unordered_map<uint32_t, AnchoredTag> m_anchored;
        std::unordered_map<uint32_t, std::string> m_beginClass;
        uint64_t m_cycle = 0, m_lastCycle = 0, m_lastTagCycle = 0;
        uint64_t m_pairsOk = 0, m_pairsOpen = 0, m_spanAddrOk = 0, m_spanUnknownTag = 0, m_spanTwice = 0;
        bool m_verify, m_quiet;
        bool m_sawVif1Kick = false;
        Cycle m_c;
        Total m_t;
        uint64_t m_cycles = 0, m_keyframes = 0, m_failures = 0;
    };

    void usage()
    {
        std::fprintf(stderr, "usage: rc_replay <dump.rcfr> [--consumer stats|emu|tex|blit|gif2d|gif2dstat] [--cycles a-b] [--verify] [--quiet] [--png <dir>]\n"
                             "                 [--picture-before <cycle>:<kick> <out.png>] [--path1 <cycle>:<kick> <out.bin>] [--kick-hashes]\n"
                             "                 [--fault page|kick|hw:<cycle>[:<n>]]\n"
                             "       tex: [--with-emu] [--png <dir>] [--fault transfer|kick:<cycle>:<n> | swizzle:<cycle>:<bits> | decode:0:<bits>]\n"
                             "       gif2d: [--with-emu] [--no-tables] [--fault reg|prim|field:<cycle>:<n>]\n"
                             "       blit: [--no-gpu] [--validation] [--png <dir>] [--fault tile:<cycle>:<n> | dispfb:<cycle> | byte|gpubyte:<cycle>:<pixel>]\n");
    }
}

namespace
{
    int runEmu(Reader &reader, const std::string &path, rc::replay::EmuOptions options, bool verify, bool quiet, uint64_t first, uint64_t last,
               const rc::replay::TexOptions *texOptions = nullptr, bool gif2dSurvey = false, const rc::replay::Gif2dOptions *gif2dOptions = nullptr)
    {
        options.verify = verify;
        options.quiet = quiet;
        options.pngFirst = first;
        // The machine the oracle builds (no window, no ELF, no game code): memory and the GS/VIF/VU1 plumbing, forced
        // to the synchronous software GS.
        rc::replay::prepareEmuEnvironment();
        static PS2Runtime runtime;
        if (!runtime.memory().initialize() || !runtime.syncCoreSubsystems())
        {
            std::fprintf(stderr, "rc_replay: could not set up the runtime\n");
            return 2;
        }
#if RC_HAVE_VU1_CODE
        registerGeneratedVu1Programs();
#endif
        rc::replay::Emu emu(runtime, options);
        const FileHeader &h = reader.header();
        std::printf("dump %s\n  %s, %ux%u, recorded with %.16s, build %.40s, %s%s\n", path.c_str(), h.gameVersion ? "Europe" : "USA", h.width,
                    h.height, h.gsBackend, h.build, (h.flags & kFileExact) ? "exact" : "plain", (h.flags & kFileComplete) ? "" : ", not closed");
        // The emulated machine needs every kick since the segment's keyframe: replay from there, write pictures only
        // for the cycles asked for.
        uint64_t from = 0;
        for (const IndexSegment &seg : reader.segments())
            if (seg.firstCycle <= first)
                from = seg.firstCycle;
        // (step 5) The tex consumer behind the emulated machine: it sees every GIF packet and the machine's memory.
        std::unique_ptr<rc::replay::TexCheck> tex;
        std::vector<Consumer *> consumers{&emu};
        // (step 7's design) The gif2dstat survey in front of the machine: it reads the Z buffer before each kick.
        std::unique_ptr<rc::replay::Gif2dSurvey> survey;
        if (gif2dSurvey)
        {
            survey = std::make_unique<rc::replay::Gif2dSurvey>(&runtime);
            consumers.insert(consumers.begin(), survey.get());
        }
        // (step 7) The gif2d reader in front of the machine: its draw lists replace the drawing PATH3 kicks (check (b)).
        std::unique_ptr<rc::replay::Gif2dCheck> gif2d;
        if (gif2dOptions)
        {
            gif2d = std::make_unique<rc::replay::Gif2dCheck>(*gif2dOptions);
            emu.setKickSubstitute([&gif2d](const rc::rec::KickView &k) { return gif2d->substitute(k); });
            consumers.insert(consumers.begin(), gif2d.get());
        }
        if (texOptions)
        {
            tex = std::make_unique<rc::replay::TexCheck>(*texOptions, &runtime);
            emu.setGifTap([&tex](int p, const uint8_t *data, uint32_t bytes) { tex->gifPacket(p, data, bytes); });
            consumers.push_back(tex.get());
        }
        const bool ok = reader.run(consumers, from, last);
        if (emu.fatal())
        {
            std::printf("unusable for emu: %s\n", emu.fatalError().c_str());
            return 2;
        }
        emu.summary();
        if (tex)
            tex->summary();
        if (survey)
            survey->summary();
        if (gif2d)
            gif2d->summary();
        if (!reader.complete())
            std::printf("note: the dump was not closed (%s); read up to its last whole chunk\n",
                        reader.truncated() ? "a partial chunk at the end" : "no index");
        if (!ok)
        {
            std::printf("read error: %s\n", reader.error().c_str());
            return 2;
        }
        if (verify && (h.flags & kFileExact))
        {
            const bool failed = emu.failed() || (tex && tex->failed()) || (gif2d && gif2d->failed());
            std::printf("%s\n", failed ? "FAIL" : "PASS");
            return failed ? 1 : 0;
        }
        return 0;
    }

    int runTex(Reader &reader, const std::string &path, const rc::replay::TexOptions &options, uint64_t first, uint64_t last)
    {
        const FileHeader &h = reader.header();
        std::printf("dump %s\n  %s, %ux%u, recorded with %.16s, build %.40s, %s%s\n", path.c_str(), h.gameVersion ? "Europe" : "USA", h.width,
                    h.height, h.gsBackend, h.build, (h.flags & kFileExact) ? "exact" : "plain", (h.flags & kFileComplete) ? "" : ", not closed");
        rc::replay::TexCheck tex(options, nullptr);
        // The shadow needs every transfer since the segment's keyframe.
        uint64_t from = 0;
        for (const IndexSegment &seg : reader.segments())
            if (seg.firstCycle <= first)
                from = seg.firstCycle;
        const bool ok = reader.run({&tex}, from, last);
        tex.summary();
        if (!reader.complete())
            std::printf("note: the dump was not closed (%s); read up to its last whole chunk\n",
                        reader.truncated() ? "a partial chunk at the end" : "no index");
        if (!ok)
        {
            std::printf("read error: %s\n", reader.error().c_str());
            return 2;
        }
        if (options.verify)
        {
            std::printf("%s\n", tex.failed() ? "FAIL" : "PASS");
            return tex.failed() ? 1 : 0;
        }
        return 0;
    }
}

namespace
{
    int runGif2d(Reader &reader, const std::string &path, const rc::replay::Gif2dOptions &options, uint64_t first, uint64_t last)
    {
        const FileHeader &h = reader.header();
        std::printf("dump %s\n  %s, %ux%u, recorded with %.16s, build %.40s, %s%s\n", path.c_str(), h.gameVersion ? "Europe" : "USA", h.width,
                    h.height, h.gsBackend, h.build, (h.flags & kFileExact) ? "exact" : "plain", (h.flags & kFileComplete) ? "" : ", not closed");
        rc::replay::Gif2dCheck gif2d(options);
        // The shadow needs every upload since the segment's keyframe.
        uint64_t from = 0;
        for (const IndexSegment &seg : reader.segments())
            if (seg.firstCycle <= first)
                from = seg.firstCycle;
        const bool ok = reader.run({&gif2d}, from, last);
        gif2d.summary();
        if (!reader.complete())
            std::printf("note: the dump was not closed (%s); read up to its last whole chunk\n",
                        reader.truncated() ? "a partial chunk at the end" : "no index");
        if (!ok)
        {
            std::printf("read error: %s\n", reader.error().c_str());
            return 2;
        }
        if (options.verify)
        {
            if (!(h.flags & kFileExact))
                std::printf("note: the dump is not exact (no recorded register files): check (a) compared nothing\n");
            std::printf("%s\n", gif2d.failed() ? "FAIL" : "PASS");
            return gif2d.failed() ? 1 : 0;
        }
        return 0;
    }

    int runBlit(Reader &reader, const std::string &path, const rc::replay::BlitOptions &options, uint64_t first, uint64_t last)
    {
        const FileHeader &h = reader.header();
        std::printf("dump %s\n  %s, %ux%u, recorded with %.16s, build %.40s, %s%s\n", path.c_str(), h.gameVersion ? "Europe" : "USA", h.width,
                    h.height, h.gsBackend, h.build, (h.flags & kFileExact) ? "exact" : "plain", (h.flags & kFileComplete) ? "" : ", not closed");
        rc::replay::BlitCheck blit(options);
        if (!blit.startGpu())
        {
            blit.stopGpu();
            return 2;
        }
        // The shadow needs every upload since the segment's keyframe.
        uint64_t from = 0;
        for (const IndexSegment &seg : reader.segments())
            if (seg.firstCycle <= first)
                from = seg.firstCycle;
        const bool ok = reader.run({&blit}, from, last);
        blit.stopGpu();
        blit.summary();
        if (!reader.complete())
            std::printf("note: the dump was not closed (%s); read up to its last whole chunk\n",
                        reader.truncated() ? "a partial chunk at the end" : "no index");
        if (!ok)
        {
            std::printf("read error: %s\n", reader.error().c_str());
            return 2;
        }
        if (options.verify)
        {
            std::printf("%s\n", blit.failed() ? "FAIL" : "PASS");
            return blit.failed() ? 1 : 0;
        }
        return 0;
    }
}

int main(int argc, char **argv)
{
    std::string path, consumer = "stats";
    bool verify = false, quiet = false, withEmu = false, noGpu = false, validation = false, noTables = false;
    uint64_t first = 0, last = ~0ull;
    rc::replay::EmuOptions emuOptions;
    auto cycleKick = [](const char *text, int64_t &cycle, uint32_t &kick)
    {
        const char *colon = std::strchr(text, ':');
        cycle = std::strtoll(text, nullptr, 10);
        kick = colon ? static_cast<uint32_t>(std::strtoul(colon + 1, nullptr, 10)) : 0u;
    };
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--consumer" && i + 1 < argc)
            consumer = argv[++i];
        else if (a == "--png" && i + 1 < argc)
            emuOptions.pngDir = argv[++i];
        else if (a == "--picture-before" && i + 2 < argc)
        {
            cycleKick(argv[++i], emuOptions.beforeCycle, emuOptions.beforeKick);
            emuOptions.beforeOut = argv[++i];
        }
        else if (a == "--path1" && i + 2 < argc)
        {
            cycleKick(argv[++i], emuOptions.path1Cycle, emuOptions.path1Kick);
            emuOptions.path1Out = argv[++i];
        }
        else if (a == "--kick-hashes")
            emuOptions.kickHashes = true;
        else if (a == "--fault" && i + 1 < argc)
        {
            const std::string f = argv[++i];
            const size_t c1 = f.find(':');
            emuOptions.faultKind = f.substr(0, c1);
            if (c1 != std::string::npos)
            {
                int64_t cycle;
                uint32_t n;
                cycleKick(f.c_str() + c1 + 1, cycle, n);
                emuOptions.faultCycle = cycle;
                emuOptions.faultN = n;
            }
        }
        else if (a == "--with-emu")
            withEmu = true;
        else if (a == "--no-gpu")
            noGpu = true;
        else if (a == "--no-tables")
            noTables = true;
        else if (a == "--validation")
            validation = true;
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
    if (consumer == "gif2dstat")
    {
        Reader reader;
        if (!reader.open(path))
        {
            std::fprintf(stderr, "%s: %s\n", path.c_str(), reader.error().c_str());
            return 2;
        }
        if (withEmu)
            return runEmu(reader, path, emuOptions, false, true, first, last, nullptr, true);
        rc::replay::Gif2dSurvey survey(nullptr);
        const bool ok = reader.run({&survey}, first, last);
        survey.summary();
        return ok ? 0 : 2;
    }
    if (consumer != "stats" && consumer != "emu" && consumer != "tex" && consumer != "blit" && consumer != "gif2d")
    {
        std::fprintf(stderr, "consumer '%s' is not built yet (stats, emu, tex, blit, gif2d, gif2dstat)\n", consumer.c_str());
        return 2;
    }
    Reader reader;
    if (!reader.open(path))
    {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), reader.error().c_str());
        return 2;
    }
    if (consumer == "tex")
    {
        rc::replay::TexOptions texOptions;
        texOptions.verify = verify;
        texOptions.quiet = quiet;
        texOptions.pngDir = emuOptions.pngDir;
        texOptions.faultKind = emuOptions.faultKind;
        texOptions.faultCycle = emuOptions.faultCycle;
        texOptions.faultN = emuOptions.faultN;
        if (!withEmu)
            return runTex(reader, path, texOptions, first, last);
        // Behind emu: the planted fault and the PNGs are the tex consumer's, not the emulated machine's.
        emuOptions.pngDir.clear();
        emuOptions.faultKind.clear();
        return runEmu(reader, path, emuOptions, verify, true, first, last, &texOptions);
    }
    if (consumer == "blit")
    {
        rc::replay::BlitOptions blitOptions;
        blitOptions.verify = verify;
        blitOptions.quiet = quiet;
        blitOptions.gpu = !noGpu;
        blitOptions.validation = validation;
        blitOptions.pngDir = emuOptions.pngDir;
        blitOptions.faultKind = emuOptions.faultKind;
        blitOptions.faultCycle = emuOptions.faultCycle;
        blitOptions.faultN = emuOptions.faultN;
        // Pictures are compared only from the first cycle asked for; the shadow replays from its keyframe.
        return runBlit(reader, path, blitOptions, first, last);
    }
    if (consumer == "gif2d")
    {
        rc::replay::Gif2dOptions g;
        g.verify = verify;
        g.quiet = quiet;
        g.withEmu = withEmu;
        g.tables = !noTables;
        g.faultKind = emuOptions.faultKind;
        g.faultCycle = emuOptions.faultCycle;
        g.faultN = emuOptions.faultN;
        if (withEmu)
        {
            // Behind gif2d: the planted fault is the reader's or the encoder's, not the emulated machine's.
            emuOptions.faultKind.clear();
            return runEmu(reader, path, emuOptions, verify, true, first, last, nullptr, false, &g);
        }
        return runGif2d(reader, path, g, first, last);
    }
    if (consumer == "emu")
        return runEmu(reader, path, emuOptions, verify, quiet, first, last);
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

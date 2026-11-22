// rc_replay: feeds a frame record dump (.rcfr, RENDERER_FRAME_RECORD.md) to consumers.
//
//   rc_replay <dump> [--consumer stats|emu|tex|blit|gif2d|gif2dstat|vifstat|shapestat|vif|live2d|vk|dlst|lists] [--cycles a-b] [--verify] [--quiet]
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
// shapestat (shape_survey.cpp, shape_lean.cpp; RENDERER_P2_DESIGN.md section 3): the units K2's chains call, what each
// run's triangles do in the VU1 program, and a lean build of the references compared with them. No machine.
// vifstat (vif_survey.cpp; RENDERER_3D_DESIGN.md section 3): counts what the VIF1 chains use; with --with-emu also the
// C++ VU1 references against the emulated VU1, PATH1's draw states and the exact blend's barriers per kick
// (--vif-round-nearest: the experiment of that design's decision U1). Output for tools/vif_agg.py.
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
// primitive (n of the cycle), a zeroed GsState field (n, gs2d_encode.h GsField). --vk (step 7 batch 3, gif2d_vk.h; implies
// --with-emu): (c) every drawing PATH3 kick also drawn through Gs2dVk over the emulated frame and Z buffers and compared
// with the emulated GS's result (RENDERER_2D_DESIGN.md 10.3, 16 and 18.4): --vk-blend read (the exact blend by local
// read, every pixel equal; the default where the device has it), ff (fixed function with D-2D-1's bound; the default
// without local read) or both; --vk-bias off|on|both is fixed function's source bias (default on); --vk-fault plants the
// faults of 9.3 and 18.3 in the Vulkan path, --vk-png writes the first failing kick's planes. (Step 8 batch 2,
// RENDERER_3D_DESIGN.md 11.4) (d): every channel 1 kick (K2, from the C++ VU1 references) is judged the same way, every
// pixel equal; the barriers go by covered pixels; --vk-boxes adds the read with batch 4b's box groups for comparison;
// the faults of 10.3 and 11.4 (no-first-barrier, cover-own-batch, no-stamp-other, epoch-no-barrier, gequal-greater,
// z-plus-one, repeat-clamp, no-q, draw-culled, cull-drawn). --vk-nofill is a measurement, not a check: every draw's
// scissor cut to one pixel, so the GPU time reported is the draws, binds and barriers without the fragments (18.5).
//
// vif (vif.cpp; renderer plan P1 step 8 batch 1, RENDERER_3D_DESIGN.md 11.1-11.3): the frame reader (render/frame: the
// VIF1 reader with the C++ VU1 references feeding the GIF reader as PATH1). With --verify: (a) VIF1 registers, VU1
// micro and data memory and the vector registers at every later keyframe, the GS register file at every kick; (b)
// every channel 1 kick's PATH1 bytes against its recorded GIF hash. --with-emu: behind the emulated machine, run by run
// against its VU1 (packets, VU memory, registers), and (c) --vif-substitute bytes|list|list-cull|none: the reader's
// bytes (or the K2 draw list encoded back, without or with cullOffscreen) drawn by the emulated GS in place of the
// machine's PATH1. --fault unpack|tops|dbf|mpg|mscal|mxcsr|lane|packet|vumem:<cycle>[:<n>]; --vif-out <dir> writes the
// first differing run. The gif2d consumer uses the same frame reader (and with --with-emu the vif consumer beside it).
// --vu1 strict|lean (step 9 batch 1, RENDERER_P2_DESIGN.md 4.1): which build of the references the reader runs
// (default strict, the judge; lean: rc_vu1_lean, checks (a) and (b) lean of that design's section 9).
//
// vk (step 8 batch 3, RENDERER_3D_DESIGN.md 11.5 and 19; exact dumps): the chained replay, no emulated machine. Seeded
// at the first keyframe (and after a save-state load), every kick through the frame reader and every cycle's list
// through Gs2dVk; --verify: every flipped picture hashes to END.refHash and equals REFI where stored, and both targets
// and Z equal GSVR at every later keyframe. [--vk-blend read|ff] [--validation] [--png <dir>]
// [--fault depth|clear|texture|flip:<cycle>[:<cycles>]].
#include "chain_walk.h"
#include "emu.h"
#include "tex.h"
#include "blit.h"
#include "gif2d_survey.h"
#include "vif_survey.h"
#include "shape_survey.h"
#include "gif2d.h"
#include "vif.h"
#include "consumer.h"
#include "live2d.h"
#include "dlst.h"
#include "lists.h"
#include "frame_vk.h"
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
        std::fprintf(stderr, "usage: rc_replay <dump.rcfr> [--consumer stats|emu|tex|blit|gif2d|gif2dstat|vifstat] [--cycles a-b] [--verify] [--quiet] [--png <dir>]\n"
                             "                 [--picture-before <cycle>:<kick> <out.png>] [--path1 <cycle>:<kick> <out.bin>] [--kick-hashes]\n"
                             "                 [--fault page|kick|hw:<cycle>[:<n>]]\n"
                             "       tex: [--with-emu] [--png <dir>] [--fault transfer|kick:<cycle>:<n> | swizzle:<cycle>:<bits> | decode:0:<bits>]\n"
                             "       gif2d: [--with-emu] [--no-tables] [--fault reg|prim|field:<cycle>:<n>]\n"
                             "       vif: [--with-emu [--vif-substitute bytes|list|list-cull|none]] [--vif-out <dir>]\n"
                             "            [--fault unpack|tops|dbf|mpg|mscal|mxcsr|lane|packet|vumem:<cycle>[:<n>]]\n"
                             "              [--vk] [--vk-blend read|ff|both] [--vk-boxes] [--vk-nofill] [--vk-bias off|on|both] [--vk-fault <name>[,<name>...]] [--vk-png <dir>] [--validation]\n"
                             "       blit: [--no-gpu] [--validation] [--png <dir>] [--fault tile:<cycle>:<n> | dispfb:<cycle> | byte|gpubyte:<cycle>:<pixel>]\n"
                             "       emu --rereference <out.rcfr> (exact dumps): a copy with the references this machine makes\n"
                             "       live2d (exact dumps; the live 2D renderer, RC_VK_PICTURE=native): [--live-shots <dir>] [--vk-blend read|ff] [--validation] [--png <dir>]\n"
                             "              [--reader strict|fast] [--reader-thread] [--no-gpu] (P2 step 9 batch 6: the reader behind the live KickQueue on its own thread)\n"
                             "       vk (exact dumps; the chained replay, no emulated machine): [--vk-blend read|ff] [--validation] [--png <dir>]\n"
                             "            [--fault depth|clear|texture|flip:<cycle>[:<cycles>]]\n"
                             "       dlst (a dump from boot with RC_FRAME_RECORD_DRAWLISTS=1): the live reader's lists against the recorded DLST chunks\n"
                             "            [--fault list:<cycle>[:<byte>]]\n"
                             "       lists (check (e)): the fast reader against the strict reader [--from-reset] [--fault adc|clut-memo|state-memo|cull|bias|fused|round-x|edge-two|hard-quick|last-step|lit-regs|view-hard|view-ref|kick-cut[@trdr|env|skin|terdr][:<cycle>]]\n"
                             "       vif, lists, vk: [--vu1 strict|lean] (vif) [--reader strict|fast|fast-nopaths|fast-noviews] (vif: the reader timed; vk: strict|fast, the reader drawn)\n");
    }
}

namespace
{
    int runEmu(Reader &reader, const std::string &path, rc::replay::EmuOptions options, bool verify, bool quiet, uint64_t first, uint64_t last,
               const rc::replay::TexOptions *texOptions = nullptr, bool gif2dSurvey = false, const rc::replay::Gif2dOptions *gif2dOptions = nullptr,
               bool vifSurvey = false, bool vifRoundNearest = false, const rc::replay::VifOptions *vifOptions = nullptr,
               const rc::replay::Live2dOptions *live2dOptions = nullptr, const std::string *rereferenceOut = nullptr)
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
                    h.height, h.gsBackend, h.build, (h.flags & kFileExact) ? ((h.flags & kFileRereferenced) ? "exact, re-referenced" : "exact") : "plain", (h.flags & kFileComplete) ? "" : ", not closed");
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
            rc::replay::Gif2dOptions g = *gif2dOptions;
            g.vram = [] { return static_cast<const uint8_t *>(runtime.memory().getGSVRAM()); };
            gif2d = std::make_unique<rc::replay::Gif2dCheck>(g);
            emu.setKickSubstitute([&gif2d](const rc::rec::KickView &k) { return gif2d->substitute(k); });
            // (step 7 batch 3) Check (c): the emulated memory after each kick is the reference for the Vulkan replay.
            if (g.vk)
            {
                emu.setAfterKick([&gif2d](const rc::rec::KickView &k) { gif2d->afterKick(k); });
                std::string why;
                if (!gif2d->startVk(&why))
                {
                    std::printf("gif2d --vk: %s\nFAIL\n", why.c_str());
                    return 2;
                }
            }
            consumers.insert(consumers.begin(), gif2d.get());
        }
        // (step 8) The vif consumer behind the machine: the machine runs each channel 1 kick first, then the frame reader
        // runs it and is compared run by run (and with gif2d, the vif consumer works on gif2d's frame reader).
        std::unique_ptr<rc::replay::VifCheck> vifCheck;
        if (vifOptions || gif2d)
        {
            rc::replay::VifOptions o = vifOptions ? *vifOptions : rc::replay::VifOptions{};
            o.withEmu = true;
            o.verify = verify;
            o.quiet = quiet;
            vifCheck = std::make_unique<rc::replay::VifCheck>(o, &runtime, vifOptions ? nullptr : gif2d->frameReader());
            vifCheck->installVu1Wrappers();
            emu.setPath1Substitute([&vifCheck](const rc::rec::KickView &k, const rc::rec::RamView &ram) { return vifCheck->path1(k, ram); });
            consumers.push_back(vifCheck.get());
        }
        if (texOptions)
        {
            tex = std::make_unique<rc::replay::TexCheck>(*texOptions, &runtime);
            emu.setGifTap([&tex](int p, const uint8_t *data, uint32_t bytes) { tex->gifPacket(p, data, bytes); });
            consumers.push_back(tex.get());
        }
        // (step 8's design) The vifstat survey in front of the machine: it walks the VIF1 chains itself, takes every GIF
        // packet from the machine's tap and repeats each VU1 run with the C++ reference.
        std::unique_ptr<rc::replay::VifSurvey> vif;
        if (vifSurvey)
        {
            vif = std::make_unique<rc::replay::VifSurvey>(&runtime);
            vif->installVu1Wrappers();
            vif->setRoundNearest(vifRoundNearest);
            emu.setGifTap([&vif](int p, const uint8_t *data, uint32_t bytes) { vif->gifPacket(p, data, bytes); });
            emu.setAfterKick([&vif](const rc::rec::KickView &k) { vif->afterKick(k); });
            consumers.insert(consumers.begin(), vif.get());
        }
        // (step 7 batch 5) The live 2D renderer's reader and presenter in front of the machine, judged per displayed picture.
        std::unique_ptr<rc::replay::Live2dCheck> live2d;
        if (live2dOptions)
        {
            rc::replay::Live2dOptions o = *live2dOptions;
            o.vram = [] { return static_cast<const uint8_t *>(runtime.memory().getGSVRAM()); };
            live2d = std::make_unique<rc::replay::Live2dCheck>(o);
            std::string why;
            if (!live2d->start(&why))
            {
                std::printf("live2d: %s\nFAIL\n", why.c_str());
                return 2;
            }
            emu.setAfterKick([&live2d](const rc::rec::KickView &k) { live2d->afterKick(k); });
            consumers.insert(consumers.begin(), live2d.get());
        }
        // (batch R) The re-referenced copy, written behind the machine.
        std::unique_ptr<rc::replay::Rereference> reref;
        if (rereferenceOut)
        {
            reref = std::make_unique<rc::replay::Rereference>(emu, *rereferenceOut, path);
            if (!reref->ok())
            {
                std::printf("rereference: cannot write %s\n", rereferenceOut->c_str());
                return 2;
            }
            consumers.push_back(reref.get());
        }
        const bool ok = reader.run(consumers, from, last);
        if (emu.fatal())
        {
            std::printf("unusable for emu: %s\n", emu.fatalError().c_str());
            if (reref)
            {
                reref.reset();
                std::remove(rereferenceOut->c_str());
            }
            return 2;
        }
        emu.summary();
        if (reref)
        {
            reref->summary();
            reref.reset(); // closes the copy (index, ENDF)
            if (!ok || !reader.complete())
            {
                std::printf("rereference: the source %s; the copy %s is not complete\n", ok ? "was not closed" : "has a read error",
                            rereferenceOut->c_str());
                return 2;
            }
            std::printf("rereference: wrote %s\n", rereferenceOut->c_str());
        }
        if (tex)
            tex->summary();
        if (survey)
            survey->summary();
        if (vif)
            vif->summary();
        if (gif2d)
            gif2d->summary();
        if (vifCheck)
            vifCheck->summary();
        if (live2d)
            live2d->summary();
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
            const bool failed = emu.failed() || (tex && tex->failed()) || (gif2d && gif2d->failed()) || (vifCheck && vifCheck->failed()) ||
                                (live2d && live2d->failed());
            std::printf("%s\n", failed ? "FAIL" : "PASS");
            return failed ? 1 : 0;
        }
        return 0;
    }

    int runTex(Reader &reader, const std::string &path, const rc::replay::TexOptions &options, uint64_t first, uint64_t last)
    {
        const FileHeader &h = reader.header();
        std::printf("dump %s\n  %s, %ux%u, recorded with %.16s, build %.40s, %s%s\n", path.c_str(), h.gameVersion ? "Europe" : "USA", h.width,
                    h.height, h.gsBackend, h.build, (h.flags & kFileExact) ? ((h.flags & kFileRereferenced) ? "exact, re-referenced" : "exact") : "plain", (h.flags & kFileComplete) ? "" : ", not closed");
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
                    h.height, h.gsBackend, h.build, (h.flags & kFileExact) ? ((h.flags & kFileRereferenced) ? "exact, re-referenced" : "exact") : "plain", (h.flags & kFileComplete) ? "" : ", not closed");
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

    int runVif(Reader &reader, const std::string &path, const rc::replay::VifOptions &options, uint64_t first, uint64_t last)
    {
        const FileHeader &h = reader.header();
        std::printf("dump %s\n  %s, %ux%u, recorded with %.16s, build %.40s, %s%s\n", path.c_str(), h.gameVersion ? "Europe" : "USA", h.width,
                    h.height, h.gsBackend, h.build, (h.flags & kFileExact) ? ((h.flags & kFileRereferenced) ? "exact, re-referenced" : "exact") : "plain", (h.flags & kFileComplete) ? "" : ", not closed");
        rc::replay::VifCheck vif(options, nullptr);
        // The readers start at the segment's keyframe.
        uint64_t from = 0;
        for (const IndexSegment &seg : reader.segments())
            if (seg.firstCycle <= first)
                from = seg.firstCycle;
        const bool ok = reader.run({&vif}, from, last);
        vif.summary();
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
            std::printf("%s\n", vif.failed() ? "FAIL" : "PASS");
            return vif.failed() ? 1 : 0;
        }
        return 0;
    }

    // (step 8 batch 3) The chained replay: the frame reader and Gs2dVk alone, no machine.
    int runFrameVk(Reader &reader, const std::string &path, const rc::replay::FrameVkOptions &options, uint64_t first, uint64_t last)
    {
        const FileHeader &h = reader.header();
        std::printf("dump %s\n  %s, %ux%u, recorded with %.16s, build %.40s, %s%s\n", path.c_str(), h.gameVersion ? "Europe" : "USA", h.width,
                    h.height, h.gsBackend, h.build, (h.flags & kFileExact) ? ((h.flags & kFileRereferenced) ? "exact, re-referenced" : "exact") : "plain", (h.flags & kFileComplete) ? "" : ", not closed");
        if (!(h.flags & kFileExact))
        {
            std::printf("vk: the chained replay needs an exact dump (reference hashes, exact GS memory at the keyframes)\n");
            return 2;
        }
        rc::replay::FrameVkCheck vk(options);
        std::string why;
        if (!vk.start(&why))
        {
            std::printf("vk: %s\nFAIL\n", why.c_str());
            return 2;
        }
        // The chain starts at a segment's keyframe.
        uint64_t from = 0;
        for (const IndexSegment &seg : reader.segments())
            if (seg.firstCycle <= first)
                from = seg.firstCycle;
        const bool ok = reader.run({&vk}, from, last);
        vk.summary();
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
            std::printf("%s\n", vk.failed() ? "FAIL" : "PASS");
            return vk.failed() ? 1 : 0;
        }
        return 0;
    }

    int runBlit(Reader &reader, const std::string &path, const rc::replay::BlitOptions &options, uint64_t first, uint64_t last)
    {
        const FileHeader &h = reader.header();
        std::printf("dump %s\n  %s, %ux%u, recorded with %.16s, build %.40s, %s%s\n", path.c_str(), h.gameVersion ? "Europe" : "USA", h.width,
                    h.height, h.gsBackend, h.build, (h.flags & kFileExact) ? ((h.flags & kFileRereferenced) ? "exact, re-referenced" : "exact") : "plain", (h.flags & kFileComplete) ? "" : ", not closed");
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
    bool verify = false, quiet = false, withEmu = false, noGpu = false, validation = false, noTables = false, vk = false, vifRoundNearest = false,
         vifNoPoison = false;
    bool vkBoxes = false, vkNoFill = false, faultHasCount = false, fromReset = false;
    bool readerThread = false; // (P2 step 9 batch 6) live2d: the reader behind the KickQueue on its own thread
    std::string vkBias = "on", vkBlend = "auto", vkFaults, vkPng, vifSubstitute = "bytes", vifOut, liveShots, rereference, vu1Library = "strict", readerKind = "strict";
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
            faultHasCount = c1 != std::string::npos && f.find(':', c1 + 1) != std::string::npos;
            if (c1 != std::string::npos)
            {
                int64_t cycle;
                uint32_t n;
                cycleKick(f.c_str() + c1 + 1, cycle, n);
                emuOptions.faultCycle = cycle;
                emuOptions.faultN = n;
            }
        }
        else if (a == "--vif-round-nearest")
            vifRoundNearest = true;
        else if (a == "--vif-substitute" && i + 1 < argc)
            vifSubstitute = argv[++i];
        else if (a == "--vif-out" && i + 1 < argc)
            vifOut = argv[++i];
        else if (a == "--vif-no-poison")
            vifNoPoison = true;
        else if (a == "--vu1" && i + 1 < argc)
            vu1Library = argv[++i];
        else if (a == "--from-reset")
            fromReset = true;
        else if (a == "--reader" && i + 1 < argc)
            readerKind = argv[++i];
        else if (a == "--with-emu")
            withEmu = true;
        else if (a == "--no-gpu")
            noGpu = true;
        else if (a == "--vk")
            vk = true;
        else if (a == "--vk-bias" && i + 1 < argc)
            vkBias = argv[++i];
        else if (a == "--vk-blend" && i + 1 < argc)
            vkBlend = argv[++i];
        else if (a == "--vk-fault" && i + 1 < argc)
            vkFaults = argv[++i];
        else if (a == "--vk-png" && i + 1 < argc)
            vkPng = argv[++i];
        else if (a == "--vk-boxes")
            vkBoxes = true;
        else if (a == "--vk-nofill")
            vkNoFill = true;
        else if (a == "--live-shots" && i + 1 < argc)
            liveShots = argv[++i];
        else if (a == "--no-tables")
            noTables = true;
        else if (a == "--reader-thread")
            readerThread = true;
        else if (a == "--validation")
            validation = true;
        else if (a == "--rereference" && i + 1 < argc)
            rereference = argv[++i];
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
    if (consumer == "vifstat")
    {
        // (step 8's design) What the VIF1 chains and their PATH1 output use. With --with-emu the emulated machine runs
        // beside it: the C++ VU1 references against the emulated VU1, PATH1's draw states, the exact blend's barriers.
        Reader reader;
        if (!reader.open(path))
        {
            std::fprintf(stderr, "%s: %s\n", path.c_str(), reader.error().c_str());
            return 2;
        }
        if (withEmu)
            return runEmu(reader, path, emuOptions, verify, true, first, last, nullptr, false, nullptr, true, vifRoundNearest);
        rc::replay::VifSurvey survey(nullptr);
        const bool ok = reader.run({&survey}, first, last);
        survey.summary();
        return ok ? 0 : 2;
    }
    if (consumer == "shapestat")
    {
        // (step 9's design, RENDERER_P2_DESIGN.md 3) What a batch reader for K2 would meet: the units the chains call, what
        // each run's triangles do in the program, and the lean build of the references beside the references. No machine.
        Reader reader;
        if (!reader.open(path))
        {
            std::fprintf(stderr, "%s: %s\n", path.c_str(), reader.error().c_str());
            return 2;
        }
        rc::replay::ShapeSurvey survey;
        uint64_t from = 0;
        for (const IndexSegment &seg : reader.segments())
            if (seg.firstCycle <= first)
                from = seg.firstCycle;
        const bool ok = reader.run({&survey}, from, last);
        survey.summary();
        return ok ? 0 : 2;
    }
    if (consumer != "stats" && consumer != "emu" && consumer != "tex" && consumer != "blit" && consumer != "gif2d" && consumer != "vif" &&
        consumer != "live2d" && consumer != "vk" && consumer != "dlst" && consumer != "lists")
    {
        std::fprintf(stderr, "consumer '%s' is not built yet (stats, emu, tex, blit, gif2d, gif2dstat, vifstat, vif, live2d, vk, dlst, lists)\n", consumer.c_str());
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
        if (vk)
        {
            // (c) needs the emulated machine behind the reader.
            withEmu = g.withEmu = g.vk = true;
            g.vkOptions.validation = validation;
            g.vkOptions.quiet = quiet;
            g.vkOptions.pngDir = vkPng;
            g.vkOptions.biasMask = vkBias == "off" ? 1u : vkBias == "on" ? 2u : 3u;
            g.vkOptions.boxes = vkBoxes;
            g.vkOptions.noFill = vkNoFill;
            if (vkBlend != "auto" && vkBlend != "read" && vkBlend != "ff" && vkBlend != "both")
            {
                std::fprintf(stderr, "unknown --vk-blend '%s' (read, ff, both)\n", vkBlend.c_str());
                return 2;
            }
            g.vkOptions.blend = vkBlend;
            size_t at = 0;
            while (at < vkFaults.size())
            {
                const size_t comma = vkFaults.find(',', at);
                const std::string name = vkFaults.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                if (name == "texa")
                    g.vkOptions.faultTexa = true;
                else if (!rc::replay::gif2dVkFault(name, g.vkOptions.faults))
                {
                    std::fprintf(stderr, "unknown --vk-fault '%s' (atst-less, drop-fail, split-per-batch, z-gequal, fix, colour-swap, no-half-pixel, "
                                         "bias-always, texel-row, no-barrier, one-group, no-first-barrier, cover-own-batch, no-stamp-other, epoch-no-barrier, "
                                         "gequal-greater, z-plus-one, repeat-clamp, no-q, draw-culled, cull-drawn, texa)\n",
                                 name.c_str());
                    return 2;
                }
                if (comma == std::string::npos)
                    break;
                at = comma + 1;
            }
            if (!vkFaults.empty())
                std::printf("gif2d --vk: planted fault(s) %s\n", vkFaults.c_str());
        }
        if (withEmu)
        {
            // Behind gif2d: the planted fault is the reader's or the encoder's, not the emulated machine's.
            emuOptions.faultKind.clear();
            return runEmu(reader, path, emuOptions, verify, true, first, last, nullptr, false, &g);
        }
        return runGif2d(reader, path, g, first, last);
    }
    if (consumer == "vif")
    {
        rc::replay::VifOptions v;
        v.verify = verify;
        v.quiet = quiet;
        v.withEmu = withEmu;
        v.substitute = vifSubstitute;
        v.outDir = vifOut;
        v.noPoison = vifNoPoison;
        if (!rc::rd::parseVu1Library(vu1Library, v.library))
        {
            std::fprintf(stderr, "unknown --vu1 '%s' (strict, lean)\n", vu1Library.c_str());
            return 2;
        }
        if (readerKind != "strict" && readerKind != "fast" && readerKind != "fast-nopaths" && readerKind != "fast-noviews")
        {
            std::fprintf(stderr, "unknown --reader '%s' (strict, fast, fast-nopaths, fast-noviews)\n", readerKind.c_str());
            return 2;
        }
        v.fast = readerKind != "strict";
        v.fastPaths = readerKind == "fast" || readerKind == "fast-noviews";
        v.views = readerKind == "fast";
        if (v.fast && withEmu)
        {
            std::fprintf(stderr, "rc_replay vif: --reader fast is off-line only (its check is --consumer lists)\n");
            return 2;
        }
        if (vifSubstitute != "none" && vifSubstitute != "bytes" && vifSubstitute != "list" && vifSubstitute != "list-cull")
        {
            std::fprintf(stderr, "unknown --vif-substitute '%s' (none, bytes, list, list-cull)\n", vifSubstitute.c_str());
            return 2;
        }
        if (!emuOptions.faultKind.empty())
        {
            if (!rc::rd::parseVifFault(emuOptions.faultKind, v.fault))
            {
                std::fprintf(stderr, "unknown vif fault '%s' (unpack, tops, dbf, mpg, mscal, mxcsr, lane, packet, vumem)\n", emuOptions.faultKind.c_str());
                return 2;
            }
            v.faultCycle = emuOptions.faultCycle;
            v.faultN = emuOptions.faultN;
            std::printf("vif: planted fault %s at cycle %" PRId64 " (n %u)\n", emuOptions.faultKind.c_str(), v.faultCycle, v.faultN);
            emuOptions.faultKind.clear();
        }
        if (withEmu)
            return runEmu(reader, path, emuOptions, verify, true, first, last, nullptr, false, nullptr, false, false, &v);
        return runVif(reader, path, v, first, last);
    }
    if (consumer == "vk")
    {
        rc::replay::FrameVkOptions o;
        o.verify = verify;
        o.quiet = quiet;
        o.validation = validation;
        o.blend = vkBlend;
        o.pngDir = emuOptions.pngDir;
        o.dumpPath = path;
        o.fault = emuOptions.faultKind;
        o.faultCycle = emuOptions.faultCycle;
        o.faultCycles = faultHasCount ? emuOptions.faultN : 1u;
        if (readerKind != "strict" && readerKind != "fast")
        {
            std::fprintf(stderr, "rc_replay vk: --reader strict|fast\n");
            return 2;
        }
        o.fastReader = readerKind == "fast";
        if (!o.fault.empty() && o.fault != "depth" && o.fault != "clear" && o.fault != "texture" && o.fault != "flip")
        {
            std::fprintf(stderr, "rc_replay vk: --fault depth|clear|texture|flip:<cycle>[:<cycles>]\n");
            return 2;
        }
        return runFrameVk(reader, path, o, first, last);
    }
    if (consumer == "lists")
    {
        // (P2 step 9 batch 2) Check (e): the fast reader's lists against the strict reader's.
        rc::replay::ListsOptions o;
        o.quiet = quiet;
        o.fromReset = fromReset;
        if (fromReset && (first != 0 || last != ~0ull))
        {
            std::fprintf(stderr, "rc_replay lists --from-reset: the readers start at boot, so the whole dump is read (no --cycles)\n");
            return 2;
        }
        if (!emuOptions.faultKind.empty())
        {
            if (!rc::replay::parseListsFault(emuOptions.faultKind))
            {
                std::fprintf(stderr, "rc_replay lists: --fault adc|clut-memo|state-memo|cull|bias|fused|round-x|edge-two|hard-quick|last-step|lit-regs|view-hard|view-ref|kick-cut[@trdr|env|skin|terdr][:<cycle>]\n");
                return 2;
            }
            o.fault = emuOptions.faultKind;
            o.faultCycle = emuOptions.faultCycle < 0 ? 0 : emuOptions.faultCycle;
            std::printf("lists: planted fault %s in the fast reader from cycle %" PRId64 "\n", o.fault.c_str(), o.faultCycle);
        }
        const FileHeader &h = reader.header();
        std::printf("dump %s\n  %s, %ux%u, build %.40s, %s\n", path.c_str(), h.gameVersion ? "Europe" : "USA", h.width, h.height, h.build,
                    (h.flags & kFileExact) ? "exact" : "plain");
        rc::replay::ListsCheck check(o);
        uint64_t from = first;
        if (o.fromReset)
            from = 0;
        else
            for (const IndexSegment &seg : reader.segments())
                if (seg.firstCycle <= first)
                    from = seg.firstCycle;
        const bool ok = reader.run({&check}, from, last);
        check.summary();
        if (!ok)
        {
            std::printf("read error: %s\n", reader.error().c_str());
            return 2;
        }
        std::printf("%s\n", check.failed() ? "FAIL" : "PASS");
        return check.failed() ? 1 : 0;
    }
    if (consumer == "dlst")
    {
        // (step 8 batch 4) The live check: the live reader's lists (DLST) against the same reader on the recorded kicks.
        if (first != 0 || last != ~0ull)
        {
            std::fprintf(stderr, "rc_replay dlst: the reader starts at boot, so the whole dump is read (no --cycles)\n");
            return 2;
        }
        const FileHeader &h = reader.header();
        std::printf("dump %s\n  %s, %ux%u, recorded with %.16s, build %.40s, %s\n", path.c_str(), h.gameVersion ? "Europe" : "USA", h.width, h.height,
                    h.gsBackend, h.build, (h.flags & kFileExact) ? "exact" : "plain");
        rc::replay::DlstOptions o;
        o.quiet = quiet;
        if (!emuOptions.faultKind.empty())
        {
            if (emuOptions.faultKind != "list")
            {
                std::fprintf(stderr, "rc_replay dlst: --fault list:<cycle>[:<byte>]\n");
                return 2;
            }
            o.faultCycle = emuOptions.faultCycle;
            o.faultByte = faultHasCount ? emuOptions.faultN : 0u;
        }
        rc::replay::DlstCheck check(o);
        const bool ok = reader.run({&check}, 0, ~0ull);
        check.summary();
        if (!ok)
        {
            std::printf("read error: %s\n", reader.error().c_str());
            return 2;
        }
        if (check.refused())
        {
            std::printf("UNUSABLE (%s)\n", reader.chunkStats.count(kChunkDrawList) ? "not from boot" : "no DLST chunk");
            return 2;
        }
        std::printf("%s\n", check.failed() ? "FAIL" : "PASS");
        return check.failed() ? 1 : 0;
    }
    if (consumer == "live2d")
    {
        // (step 7 batch 5) The live 2D renderer on an exact dump, behind nothing, in front of the machine.
        rc::replay::Live2dOptions l;
        l.quiet = quiet;
        l.validation = validation;
        l.blend = vkBlend;
        l.shotsDir = liveShots;
        if (readerKind != "strict" && readerKind != "fast")
        {
            std::fprintf(stderr, "live2d: --reader strict|fast\n");
            return 2;
        }
        l.fast = readerKind == "fast";
        l.thread = readerThread;
        l.noGpu = noGpu;
        l.plantRace = emuOptions.faultKind == "race";
        if (l.plantRace)
            emuOptions.faultKind.clear();
        l.pngDir = emuOptions.pngDir;
        emuOptions.pngDir.clear();
        return runEmu(reader, path, emuOptions, verify, true, first, last, nullptr, false, nullptr, false, false, nullptr, &l);
    }
    if (consumer == "emu" && !rereference.empty())
    {
        // (step 8 batch R) A copy of the dump with the references the replaying machine makes.
        if (first != 0 || last != ~0ull || verify || !emuOptions.faultKind.empty())
        {
            std::fprintf(stderr, "rc_replay: --rereference replays the whole dump (no --cycles, --verify or --fault)\n");
            return 2;
        }
        if (!(reader.header().flags & kFileExact))
        {
            std::fprintf(stderr, "rc_replay: --rereference needs an exact-mode dump\n");
            return 2;
        }
        emuOptions.rereference = true;
        return runEmu(reader, path, emuOptions, false, quiet, first, last, nullptr, false, nullptr, false, false, nullptr, nullptr, &rereference);
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

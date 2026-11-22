// Live capture: see capture.h and RENDERER_FRAME_RECORD.md sections 3, 6 and 8.
#include "capture.h"

#include "emu_state.h"
#include "page_tracker.h"
#include "picture.h"
#include "writer.h"

#include "../gfx/tags.h"

#include "ps2_capture_hooks.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"

#include <immintrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifndef RC_VERSION_EU
#define RC_VERSION_EU 0
#endif
#ifndef RC_ELF_NAME
#define RC_ELF_NAME "unknown"
#endif
#ifndef RC_BUILD_DESCRIBE
#define RC_BUILD_DESCRIBE "unknown"
#endif

namespace rc::rec
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        uint64_t nsSince(Clock::time_point t)
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t).count());
        }

        struct Trigger
        {
            enum Kind
            {
                Boot,
                Cycle,
                Game,
                Level,
                Scene,
                Key,  // (batch 4) F10 starts, F10 or count stops; again on the next F10
                Ring, // (batch 4) the last `value` cycles in memory; F10 writes them and recording goes on
                Unsupported
            } kind = Boot;
            uint64_t value = 0;
            uint64_t count = 120;
            std::string text;
        };

        Trigger parseTrigger(const char *s)
        {
            Trigger t;
            t.text = s && *s ? s : "key"; // (batch 4) the default trigger: F10
            std::string spec = t.text;
            const size_t plus = spec.find('+');
            if (plus != std::string::npos)
            {
                t.count = std::strtoull(spec.c_str() + plus + 1, nullptr, 10);
                spec.resize(plus);
            }
            const size_t colon = spec.find(':');
            const std::string name = spec.substr(0, colon);
            const char *arg = colon == std::string::npos ? "" : spec.c_str() + colon + 1;
            if (name == "boot")
                t.kind = Trigger::Boot;
            else if (name == "cycle")
                t.kind = Trigger::Cycle, t.value = std::strtoull(arg, nullptr, 10);
            else if (name == "game")
                t.kind = Trigger::Game, t.value = std::strtoull(arg, nullptr, 10);
            else if (name == "level")
                t.kind = Trigger::Level, t.value = std::strtoull(arg, nullptr, 10);
            else if (name == "scene")
                t.kind = Trigger::Scene, t.value = std::strtoull(arg, nullptr, 16);
            else if (name == "key" && colon == std::string::npos)
                t.kind = Trigger::Key;
            else if (name == "ring" && *arg)
                t.kind = Trigger::Ring, t.value = std::strtoull(arg, nullptr, 10);
            else
                t.kind = Trigger::Unsupported;
            if (t.count == 0)
                t.count = 120;
            if (t.kind == Trigger::Ring && t.value == 0)
                t.kind = Trigger::Unsupported;
            return t;
        }

        void copyName(char *dst, size_t size, const char *src)
        {
            std::memset(dst, 0, size);
            std::strncpy(dst, src, size - 1);
        }

        class Capture final : public rc::gfx::detail::Sink
        {
        public:
            // (step 7 batch 5) The capture with no recording: only live consumers (addLive). setRecording makes it record.
            Capture() : m_keyEvery(300u) { m_state = Done; }

            // (step 8 batch 4) RC_FRAME_RECORD_DRAWLISTS=1: a live consumer's list of a recorded cycle, as a DLST chunk.
            // (P2 step 9 batch 6) From any thread: the reader thread closes a cycle's list up to a few cycles after the EE
            // thread closed the cycle, so a list is wanted for any cycle of the recording in progress that has been
            // closed (m_dlFirst..m_dlLast, under m_dlMutex), and the writer takes the chunk wherever it falls; finish()
            // drains the live consumers before it closes the writer.
            bool drawListWanted(uint64_t cycle)
            {
                std::lock_guard<std::mutex> lock(m_dlMutex);
                return wantedLocked(cycle);
            }
            void drawList(uint64_t cycle, std::vector<uint8_t> &&payload)
            {
                std::lock_guard<std::mutex> lock(m_dlMutex);
                if (!wantedLocked(cycle))
                    return;
                m_writer->drawList(cycle, std::move(payload));
                ++m_drawListsWritten;
            }
            bool wantedLocked(uint64_t cycle) const
            {
                return m_dlOpen && static_cast<int64_t>(cycle) >= m_dlFirst && static_cast<int64_t>(cycle) <= m_dlLast;
            }
            // Before the writer closes or writes the ring: every list the live consumers still owe is made.
            void drainLive()
            {
                for (Consumer *c : m_live)
                    c->drain();
            }

            void setRecording(std::string path, Trigger trigger, uint64_t keyEvery)
            {
                if (const char *dl = std::getenv("RC_FRAME_RECORD_DRAWLISTS"); dl && std::strcmp(dl, "1") == 0)
                    m_drawLists = true;
                m_path = std::move(path);
                m_trigger = std::move(trigger);
                m_keyEvery = keyEvery ? keyEvery : m_trigger.kind == Trigger::Ring ? 120u : 300u;
                m_recordingSet = true;
                m_state = Armed;
                // (batch 4) The ring keeps only the reference pictures' hashes unless told otherwise (section 6.2).
                if (m_trigger.kind == Trigger::Ring)
                    m_refEvery = 0;
                // RC_FRAME_RECORD_REF=<n>|hash|off (exact mode): a picture every n-th cycle (default 1), only the hash,
                // or neither. RC_FRAME_RECORD_STRICT=1: stop the run when exact mode is not possible.
                if (const char *ref = std::getenv("RC_FRAME_RECORD_REF"); ref && *ref)
                {
                    if (std::strcmp(ref, "hash") == 0)
                        m_refEvery = 0;
                    else if (std::strcmp(ref, "off") == 0)
                        m_refEvery = 0, m_refHash = false;
                    else
                        m_refEvery = std::max<uint64_t>(1u, std::strtoull(ref, nullptr, 10));
                }
                const char *strict = std::getenv("RC_FRAME_RECORD_STRICT");
                m_strict = strict && *strict && std::strcmp(strict, "0") != 0;
                // (batch 4) A test aid: RC_FRAME_RECORD_F10=<cycle>[,<cycle>...] acts as if F10 was pressed at the close of
                // those display cycles (the real key goes through the hotkey hook).
                if (const char *sim = std::getenv("RC_FRAME_RECORD_F10"); sim && *sim)
                {
                    for (const char *p = sim; *p;)
                    {
                        char *end = nullptr;
                        const uint64_t v = std::strtoull(p, &end, 10);
                        if (end == p)
                            break;
                        m_simPresses.push_back(v);
                        p = *end ? end + 1 : end;
                    }
                    std::sort(m_simPresses.begin(), m_simPresses.end());
                }
            }

            // (batch 4) The n-th dump of this run: "%d" in RC_FRAME_RECORD becomes n; without one, dumps after the first
            // get "-n" before the extension, so a second F10 never overwrites the first dump.
            std::string pathFor(uint64_t n) const
            {
                std::string path = m_path;
                const size_t pct = path.find("%d");
                if (pct != std::string::npos)
                    return path.replace(pct, 2, std::to_string(n));
                if (n == 0)
                    return path;
                const size_t slash = path.find_last_of('/');
                const size_t dot = path.find_last_of('.');
                const size_t at = dot != std::string::npos && (slash == std::string::npos || dot > slash) ? dot : path.size();
                return path.insert(at, "-" + std::to_string(n));
            }

            // F10 (hotkey hook, main thread): only counted here; the EE thread acts on it at the next cycle close.
            void hotkey()
            {
                m_keyPresses.fetch_add(1, std::memory_order_relaxed);
                if (m_recordingSet)
                    std::fprintf(stderr, "[frame-record] F10\n");
            }

            // A save state is about to replace the machine (EE thread, between guest dispatches): the cycle in progress
            // closes here, with the picture the display shows now, so everything recorded so far replays as it was.
            void stateLoading()
            {
                if ((m_state != Recording && !live()) || !m_eeKnown || std::this_thread::get_id() != m_ee)
                    return;
                flushPendingKick();
                if (m_state == Recording)
                {
                    flushDeferred();
                    for (Consumer *c : m_consumers)
                        c->note("save state load: the cycle closes here");
                }
                const uint64_t dispfb2 = m_runtime->memory().gs().dispfb2;
                m_stateLoadCut = true;
                closeCycle(dispfb2, static_cast<uint32_t>(dispfb2 & 0x1FFu));
                m_stateLoadCut = false;
            }

            // The save state is loaded: a new segment starts with a keyframe of the loaded machine (section 3.4).
            void stateLoaded()
            {
                if (!m_eeKnown || std::this_thread::get_id() != m_ee)
                    return;
                if (m_state != Recording)
                {
                    // (batch 5) Live consumers only: they reseed from the loaded machine (RENDERER_3D_DESIGN.md 5.1).
                    if (live())
                        liveKeyframe();
                    return;
                }
                keyframe(false, kKeyAfterStateLoad);
                ++m_stateLoads;
                std::fprintf(stderr, "[frame-record] save state loaded: new segment from display cycle %llu\n",
                             static_cast<unsigned long long>(m_cycle));
            }

            // (step 7 batch 5) A live consumer (RENDERER_FRAME_RECORD.md section 8, "The vk host loop"): it gets every
            // event from boot on the EE thread, whether a dump is being recorded or not: hw, crt, kick (guest RAM in place),
            // cycleBegin / cycleEnd (the closing DISPFB2 and DISPLAY2, no picture), and after a save-state load a keyframe of
            // the loaded machine flagged kKeyAfterStateLoad. open() is called here with the version's picture size.
            void addLive(Consumer *c)
            {
                FileInfo info;
                info.header.gameVersion = RC_VERSION_EU ? 1u : 0u;
                info.header.width = 640u;
                info.header.height = RC_VERSION_EU ? 256u : 240u;
                info.header.fieldRateMilliHz = RC_VERSION_EU ? 50000u : 59940u;
                info.header.ramBytes = kRamBytes;
                info.header.scratchpadBytes = kScratchpadBytes;
                info.header.pageBytes = kPageBytes;
                info.header.gsMemBytes = kGsMemBytes;
                c->open(info);
                c->cycleBegin(m_cycle);
                m_live.push_back(c);
            }

            bool live() const { return !m_live.empty(); }

            // --- hooks -------------------------------------------------------------------------------------------
            void attach(PS2Runtime *runtime)
            {
                m_runtime = runtime;
                if (!m_recordingSet)
                    return;
                std::fprintf(stderr, "[frame-record] armed: %s, trigger %s, keyframe every %llu cycles\n", m_path.c_str(),
                             m_trigger.text.c_str(), static_cast<unsigned long long>(m_keyEvery));
                if (m_trigger.kind == Trigger::Unsupported)
                {
                    std::fprintf(stderr, "[frame-record] trigger '%s' is not supported (boot, cycle:, game:, level:, scene:, key, ring:<n>); not recording\n",
                                 m_trigger.text.c_str());
                    m_state = Done;
                }
                else if (m_trigger.kind == Trigger::Boot || m_trigger.kind == Trigger::Ring)
                    start();
                else if (m_trigger.kind == Trigger::Key)
                    std::fprintf(stderr, "[frame-record] press F10 in the game window to record %llu cycles (F10 again stops early)\n",
                                 static_cast<unsigned long long>(m_trigger.count));
            }

            void kickBegin(uint32_t channelBase, uint32_t chcr, uint32_t madr, uint32_t qwc, uint32_t tadr, uint32_t asr0, uint32_t asr1,
                           uint32_t callerRa)
            {
                if (!m_eeKnown)
                {
                    m_ee = std::this_thread::get_id();
                    m_eeKnown = true;
                }
                else if (!onEe(1))
                    return;
                ++m_kicksThisCycle;
                const uint8_t channel = static_cast<uint8_t>(((channelBase >> 12) & 0xFu) - 8u);
                // (batch 3) Side tags: the kick's ranges with their stream offsets, to resolve anchored tags against (also
                // before recording starts, so the first keyframe's TAGP holds only tags no kick has consumed yet).
                m_resolveKick = m_state != Done && !m_pendingAnchored.empty();
                m_tagRanges.clear();
                m_streamPos = 0;
                m_tteBytes = (chcr & 0x40u) != 0u && channel <= 1u && ((chcr >> 2) & 3u) == 1u ? 8u : 0u;
                const bool recording = m_state == Recording;
                if (!recording && !live())
                    return;
                flushPendingKick();
                m_kickStart = Clock::now();
                if (recording)
                {
                    flushDeferred();
                    if (channel == 0u)
                        emitOdd(ps2x::capture::OddVif0Kick, channelBase, chcr);
                }
                m_kick = KickView{};
                m_kick.cycle = m_cycle;
                m_kick.index = m_kickIndex;
                m_kick.channel = channel;
                m_kick.mode = static_cast<uint8_t>((chcr >> 2) & 3u);
                m_kick.tte = (chcr & 0x40u) != 0u;
                m_kick.chcr = chcr;
                m_kick.madr = madr;
                m_kick.qwc = qwc;
                m_kick.tadr = tadr;
                m_kick.asr0 = asr0;
                m_kick.asr1 = asr1;
                m_kick.callerRa = callerRa;
                if (recording && m_exact)
                {
                    // The GS register file before the kick (everything before it has been drawn: exact mode is synchronous).
                    std::memcpy(m_regs, ps2x::capture::g_gsRawRegs, sizeof(m_regs));
                    m_kick.regs = m_regs;
                    m_gifHash = Hash64{};
                    m_inGif = true;
                }
                // The kick spans open now label the kick (outermost first).
                m_labelViews.clear();
                for (const uint32_t seq : m_openLabels)
                    if (const auto it = m_tags.find(seq); it != m_tags.end())
                        m_labelViews.push_back(&it->second->view);
                m_kick.labels = m_labelViews.data();
                m_kick.labelCount = static_cast<uint32_t>(m_labelViews.size());
                m_labelledKicks += m_labelViews.empty() ? 0u : 1u;
                if (recording)
                    m_tracker->beginKick();
                m_ranges.clear();
                m_inKick = true;
                m_kickRecorded = recording;
            }

            void kickTouch(uint32_t physAddr, uint32_t bytes, bool scratchpad, bool isTag)
            {
                if (m_resolveKick && std::this_thread::get_id() == m_ee)
                {
                    m_tagRanges.push_back({m_streamPos, scratchpad ? 0x70000000u + physAddr : physAddr, bytes, isTag});
                    m_streamPos += isTag ? m_tteBytes : bytes;
                }
                if (!m_inKick || !m_kickRecorded || !onEe(2))
                    return;
                m_tracker->touch(physAddr, bytes, scratchpad);
                if (!isTag && m_kick.mode != 1u)
                    m_ranges.push_back({physAddr, bytes, scratchpad});
            }

            void kickEnd(const uint8_t *flat, uint32_t flatBytes, uint32_t dmaTags)
            {
                if (m_resolveKick && std::this_thread::get_id() == m_ee)
                {
                    m_resolveKick = false;
                    resolveTags();
                    if (!m_inKick)
                        dropResolved(); // not recording: nothing refers to them
                }
                if (!m_inKick || !onEe(3))
                    return;
                m_inKick = false;
                PS2Memory &memory = m_runtime->memory();
                const uint8_t *ram = memory.getRDRAM(), *spr = memory.getScratchpad();
                m_kick.dmaTags = dmaTags;
                if (m_kickRecorded)
                {
                    const uint32_t pages = m_tracker->collect(ram, spr, [&](uint32_t first, uint32_t count, const uint8_t *data)
                                                              {
                                                                  for (Consumer *c : m_consumers)
                                                                      c->pages(first, count, data);
                                                              });
                    m_cyclePages += pages;
                    Hash64 hash;
                    if (m_kick.mode == 1u)
                    {
                        if (flat && flatBytes)
                            hash.update(flat, flatBytes);
                    }
                    else
                        for (const Range &r : m_ranges)
                            hash.update((r.scratch ? spr : ram) + r.offset, r.bytes);
                    m_kick.streamBytes = static_cast<uint32_t>(hash.length());
                    m_kick.streamHash = hash.finish();
                }
                m_kick.field = memory.gs().vsyncTick.load(std::memory_order_relaxed);
                m_kick.mxcsr = _mm_getcsr();
                // As the runtime's VU1 start callback reads it: the running guest thread's context, else the runtime's.
                {
                    R5900Context *ctx = m_runtime->eeScheduler().currentContext();
                    m_kick.fbrst = (ctx ? ctx : &m_runtime->cpu())->vu0_fbrst;
                }
                m_cycleHookNs += nsSince(m_kickStart);
                // (batch 2) The KICK record goes out at kickDone, after the runtime processed the stream, so that it can
                // carry the hash of the GIF bytes the kick produced.
                m_kickPending = true;
            }

            void kickDone()
            {
                if (!m_kickPending || !onEe(6))
                    return;
                flushPendingKick();
            }

            void flushPendingKick()
            {
                if (!m_kickPending)
                    return;
                m_kickPending = false;
                const Clock::time_point t0 = Clock::now();
                if (m_inGif)
                {
                    m_inGif = false;
                    m_kick.hasGifHash = true;
                    m_kick.gifHash = m_gifHash.finish();
                    m_kick.gifBytes = static_cast<uint32_t>(m_gifHash.length());
                }
                PS2Memory &memory = m_runtime->memory();
                const RamView live{memory.getRDRAM(), memory.getScratchpad(), nullptr, nullptr};
                m_kick.spans = m_spans.data();
                m_kick.spanCount = static_cast<uint32_t>(m_spans.size());
                if (m_kickRecorded)
                    for (Consumer *c : m_consumers)
                        c->kick(m_kick, live);
                for (Consumer *c : m_live)
                    c->kick(m_kick, live);
                m_spans.clear();
                dropResolved();
                ++m_kickIndex;
                m_cycleHookNs += nsSince(t0);
            }

            void gifPacket(int, const uint8_t *data, uint32_t bytes)
            {
                // Exact mode only: then every GIF packet is submitted on the EE thread, inside the kick that made it.
                if (!m_exact || m_state != Recording || !data)
                    return;
                if (std::this_thread::get_id() != m_ee)
                {
                    onEe(7);
                    return;
                }
                if (m_inGif)
                    m_gifHash.update(data, bytes);
                else
                    m_strayGifBytes += bytes;
            }

            void hwStore(uint32_t address, uint32_t width, uint64_t value, uint32_t callerRa)
            {
                if (m_eeKnown && !onEe(4))
                    return;
                if (m_state == Recording)
                {
                    flushPendingKick();
                    flushDeferred();
                    for (Consumer *c : m_consumers)
                        c->hw(address, width, value, callerRa);
                }
                if (live())
                {
                    flushPendingKick();
                    for (Consumer *c : m_live)
                        c->hw(address, width, value, callerRa);
                }
                const uint32_t phys = address & 0x1FFFFFFFu;
                if (phys - 0x12000000u >= 0x2000u)
                    return;
                const uint32_t off = phys - 0x12000000u;
                if ((off & ~7u) != 0x90u || (width != 64u && (off & 7u) != 0u))
                    return; // only a store of DISPFB2's low word (or all of it) closes a cycle
                const uint32_t fbp = static_cast<uint32_t>(value & 0x1FFu);
                if (m_kicksThisCycle == 0u && fbp == m_lastCloseFbp)
                    return;
                closeCycle(value, fbp);
            }

            void setCrt(int interlaced, int mode, int ffmd)
            {
                if (m_eeKnown && !onEe(5))
                    return;
                if (m_state == Recording)
                    for (Consumer *c : m_consumers)
                        c->crt(interlaced, mode, ffmd);
                for (Consumer *c : m_live)
                    c->crt(interlaced, mode, ffmd);
            }

            void odd(int kind, uint32_t address, uint64_t value)
            {
                m_oddCount.fetch_add(1, std::memory_order_relaxed);
                if (m_state != Recording)
                    return;
                if (m_eeKnown && std::this_thread::get_id() == m_ee)
                {
                    flushDeferred();
                    emitOdd(kind, address, value);
                }
                else
                    defer(kind, address, value);
            }

            void gameFrame(uint64_t frame, uint64_t levelFrame, uint32_t scene)
            {
                m_gameFrame = frame;
                m_levelFrame = levelFrame;
                m_scene = scene;
            }

            // (batch 3) A side tag from native code (render/gfx/tags.cpp), on the EE thread.
            void tag(const rc::gfx::detail::Event &e) override
            {
                if (m_state == Done)
                    return;
                if (m_eeKnown && std::this_thread::get_id() != m_ee)
                {
                    m_foreign.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                ++m_tagsMade[std::min<uint32_t>(e.kind, 8u)];
                TagView v;
                v.seq = e.seq;
                v.kind = e.kind;
                v.op = e.op;
                v.depth = e.depth;
                v.anchor = e.anchor;
                v.parent = e.parent;
                v.payload = e.payload;
                v.payloadBytes = e.payloadBytes;
                if (m_state == Recording)
                {
                    flushPendingKick();
                    flushDeferred();
                    for (Consumer *c : m_consumers)
                        c->tag(v);
                    ++m_tagsRecorded;
                }
                const bool anchored = e.anchorKind != rc::gfx::AnchorKind::None;
                if (anchored && e.anchor == 0u)
                {
                    ++m_tagsUnanchorable;
                    return;
                }
                if (!anchored && !(e.kind == rc::gfx::KindKick))
                    return;
                if (e.kind == rc::gfx::KindKick && e.op == rc::gfx::OpEnd)
                {
                    // A kick span closes: it labels no later kick.
                    m_openLabels.erase(std::remove(m_openLabels.begin(), m_openLabels.end(), e.parent), m_openLabels.end());
                    m_tags.erase(e.parent);
                    return;
                }
                auto entry = std::make_unique<TagEntry>();
                entry->view = v;
                entry->payload.assign(static_cast<const uint8_t *>(e.payload), static_cast<const uint8_t *>(e.payload) + e.payloadBytes);
                entry->view.payload = entry->payload.data();
                entry->cycle = m_cycle;
                entry->anchored = anchored;
                if (anchored)
                    m_pendingAnchored.push_back(e.seq);
                else
                    m_openLabels.push_back(e.seq);
                m_tags[e.seq] = std::move(entry);
            }

        private:
            enum State
            {
                Armed,
                Recording,
                Done
            };
            struct Range
            {
                uint32_t offset, bytes;
                bool scratch;
            };
            struct Deferred
            {
                int kind;
                uint32_t address;
                uint64_t value;
            };
            // (batch 3) A tag kept until a kick consumes it: an anchored begin or end not yet resolved, or an open kick span.
            struct TagEntry
            {
                TagView view;
                std::vector<uint8_t> payload;
                uint64_t cycle = 0; // made in this display cycle
                bool anchored = false;
                bool resolved = false;
            };
            struct TagRange // one range of the kick's stream (as kickTouch reports them, in order)
            {
                uint32_t streamOffset;
                uint32_t base; // TAG.anchor encoding: RDRAM offset, or 0x70000000 + scratchpad offset
                uint32_t bytes;
                bool isTag;
            };

            // Resolution at the kick (section 5.1): an anchored tag whose anchor lies in one of the kick's ranges gets
            // that range's stream offset plus the distance into it. A DMA tag matches only its own address; a payload
            // range [base, base + bytes] including its end (an end mark after the last packet, an empty span at the
            // end). The first visit in stream order wins; a later visit inside another range is counted as a repeat.
            void resolveTags()
            {
                if (m_pendingAnchored.empty() || m_tagRanges.empty())
                    return;
                struct Cand
                {
                    uint32_t anchor, seq;
                    TagEntry *entry;
                    uint32_t interiorHits;
                };
                std::vector<Cand> cands;
                cands.reserve(m_pendingAnchored.size());
                for (const uint32_t seq : m_pendingAnchored)
                    if (const auto it = m_tags.find(seq); it != m_tags.end() && !it->second->resolved)
                        cands.push_back({it->second->view.anchor, seq, it->second.get(), 0u});
                std::sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b)
                          { return a.anchor != b.anchor ? a.anchor < b.anchor : a.seq < b.seq; });
                for (const TagRange &r : m_tagRanges)
                {
                    const uint32_t last = r.isTag ? r.base : r.base + r.bytes; // inclusive
                    auto it = std::lower_bound(cands.begin(), cands.end(), r.base, [](const Cand &c, uint32_t a) { return c.anchor < a; });
                    for (; it != cands.end() && it->anchor <= last; ++it)
                    {
                        const bool interior = r.isTag || it->anchor < r.base + r.bytes;
                        if (interior && ++it->interiorHits > 1u)
                            ++m_tagRepeats;
                        if (it->entry->resolved)
                            continue;
                        it->entry->resolved = true;
                        ++m_tagsResolved;
                        if (m_inKick)
                            m_spans.push_back({r.isTag ? r.streamOffset : r.streamOffset + (it->anchor - r.base), it->anchor, &it->entry->view,
                                               it->entry->view.op});
                    }
                }
                std::sort(m_spans.begin(), m_spans.end(), [](const Span &a, const Span &b)
                          { return a.streamOffset != b.streamOffset ? a.streamOffset < b.streamOffset : a.tag->seq < b.tag->seq; });
            }

            // Forgets resolved tags once the KICK that refers to them has gone out.
            void dropResolved()
            {
                size_t keep = 0;
                for (size_t i = 0; i < m_pendingAnchored.size(); ++i)
                {
                    const auto it = m_tags.find(m_pendingAnchored[i]);
                    if (it == m_tags.end())
                        continue;
                    if (it->second->resolved)
                        m_tags.erase(it);
                    else
                        m_pendingAnchored[keep++] = m_pendingAnchored[i];
                }
                m_pendingAnchored.resize(keep);
            }

            // At a cycle close: anchored tags no kick consumed two cycle closes after they were made are dropped.
            void expireTags()
            {
                size_t keep = 0;
                for (size_t i = 0; i < m_pendingAnchored.size(); ++i)
                {
                    const auto it = m_tags.find(m_pendingAnchored[i]);
                    if (it == m_tags.end())
                        continue;
                    if (m_cycle >= it->second->cycle + 2u)
                    {
                        if (m_state == Recording)
                            ++m_tagsExpired;
                        m_tags.erase(it);
                    }
                    else
                        m_pendingAnchored[keep++] = m_pendingAnchored[i];
                }
                m_pendingAnchored.resize(keep);
            }

            // TAGP: the tags a later kick may still refer to (unresolved anchored tags, open kick spans), as TAG records.
            std::vector<uint8_t> pendingTagRecords() const
            {
                std::vector<uint8_t> out;
                auto add = [&](const TagEntry &e)
                {
                    const TagView &v = e.view;
                    const uint32_t payload = static_cast<uint32_t>(sizeof(RecTagBody)) + v.payloadBytes;
                    const RecHeader h{RecTag, 0u, static_cast<uint32_t>(sizeof(RecHeader) + payload + pad8(payload))};
                    const RecTagBody b{v.seq, v.kind, v.op, v.depth, v.anchor, v.parent, v.payloadBytes, 0u};
                    const size_t at = out.size();
                    out.resize(at + h.bytes, 0u);
                    std::memcpy(out.data() + at, &h, sizeof(h));
                    std::memcpy(out.data() + at + sizeof(h), &b, sizeof(b));
                    if (v.payloadBytes)
                        std::memcpy(out.data() + at + sizeof(h) + sizeof(b), v.payload, v.payloadBytes);
                };
                std::vector<const TagEntry *> list;
                for (const uint32_t seq : m_openLabels)
                    if (const auto it = m_tags.find(seq); it != m_tags.end())
                        list.push_back(it->second.get());
                for (const uint32_t seq : m_pendingAnchored)
                    if (const auto it = m_tags.find(seq); it != m_tags.end() && !it->second->resolved)
                        list.push_back(it->second.get());
                std::sort(list.begin(), list.end(), [](const TagEntry *a, const TagEntry *b) { return a->view.seq < b->view.seq; });
                for (const TagEntry *e : list)
                    add(*e);
                return out;
            }

            bool onEe(uint32_t what)
            {
                if (std::this_thread::get_id() == m_ee)
                    return true;
                m_foreign.fetch_add(1, std::memory_order_relaxed);
                if (m_state == Recording)
                    defer(ps2x::capture::OddForeignThread, what, 0u);
                return false;
            }

            void defer(int kind, uint32_t address, uint64_t value)
            {
                std::lock_guard<std::mutex> lock(m_deferredMutex);
                m_deferred.push_back({kind, address, value});
                m_hasDeferred.store(true, std::memory_order_release);
            }

            void flushDeferred()
            {
                if (!m_hasDeferred.load(std::memory_order_acquire))
                    return;
                std::vector<Deferred> list;
                {
                    std::lock_guard<std::mutex> lock(m_deferredMutex);
                    list.swap(m_deferred);
                    m_hasDeferred.store(false, std::memory_order_relaxed);
                }
                for (const Deferred &d : list)
                    emitOdd(d.kind, d.address, d.value);
            }

            void emitOdd(int kind, uint32_t address, uint64_t value)
            {
                ++m_oddRecorded;
                for (Consumer *c : m_consumers)
                    c->odd(kind, address, value);
            }

            bool triggered() const
            {
                switch (m_trigger.kind)
                {
                case Trigger::Boot:
                    return true;
                case Trigger::Cycle:
                    return m_cycle >= m_trigger.value;
                case Trigger::Game:
                    return m_gameFrame != ~0ull && m_gameFrame >= m_trigger.value;
                case Trigger::Level:
                    return m_levelFrame != ~0ull && m_levelFrame >= m_trigger.value;
                case Trigger::Scene:
                    return m_scene == m_trigger.value;
                default:
                    return false;
                }
            }

            void keyframe(bool final, uint32_t extraFlags = 0u)
            {
                const Clock::time_point t0 = Clock::now();
                KeyframeState state;
                Keyframe kf;
                kf.header.cycle = m_cycle;
                kf.header.gameFrame = m_gameFrame;
                kf.header.field = m_runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed);
                // Plain mode: GS memory as the backend gives it; exact mode: the software GS's own memory.
                kf.header.flags = (final ? kKeyFinal : 0u) | (m_exact ? kKeyVramExact : 0u) | extraFlags;
                if (!captureKeyframeState(*m_runtime, state))
                    std::fprintf(stderr, "[frame-record] keyframe at cycle %llu: the GS has no memory yet\n",
                                 static_cast<unsigned long long>(m_cycle));
                if (state.gsMemory.empty())
                    state.gsMemory.assign(kGsMemBytes, 0u);
                kf.chunks = state.chunks(final);
                std::vector<uint8_t> pendingTags;
                if (!final && m_tagsAttached)
                {
                    pendingTags = pendingTagRecords();
                    kf.chunks.push_back({kStatePendingTags, pendingTags.data(), pendingTags.size()});
                    kf.header.stateChunkCount = static_cast<uint32_t>(kf.chunks.size());
                    m_tagpRecords += pendingTags.empty() ? 0u : 1u;
                }
                for (Consumer *c : m_consumers)
                    c->keyframe(kf);
                if (extraFlags & kKeyAfterStateLoad)
                    for (Consumer *c : m_live)
                        c->keyframe(kf);
                if (!final)
                {
                    m_tracker->reset();
                    m_segmentStart = Clock::now();
                    m_inSegment = 0;
                    if (m_ring)
                        ringStatus();
                }
                const uint64_t ns = nsSince(t0);
                m_keyNs += ns;
                m_keyMaxNs = std::max(m_keyMaxNs, ns);
                ++m_keyframes;
            }

            // (batch 5) A keyframe of the machine as it is now, for the live consumers only (after a save-state load).
            void liveKeyframe()
            {
                KeyframeState state;
                Keyframe kf;
                kf.header.cycle = m_cycle;
                kf.header.gameFrame = m_gameFrame;
                kf.header.field = m_runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed);
                kf.header.flags = kKeyAfterStateLoad;
                if (!captureKeyframeState(*m_runtime, state))
                    std::fprintf(stderr, "[frame-record] live keyframe at cycle %llu: the GS has no memory yet\n",
                                 static_cast<unsigned long long>(m_cycle));
                if (state.gsMemory.empty())
                    state.gsMemory.assign(kGsMemBytes, 0u);
                kf.chunks = state.chunks(false);
                for (Consumer *c : m_live)
                    c->keyframe(kf);
            }

            void start()
            {
                m_ring = m_trigger.kind == Trigger::Ring;
                const std::string path = pathFor(m_dumpIndex);
                auto writer = m_ring ? std::make_unique<Writer>(Writer::Ring{m_trigger.value}) : std::make_unique<Writer>(path);
                if (!writer->ok())
                {
                    std::fprintf(stderr, "[frame-record] cannot write %s; not recording\n", path.c_str());
                    m_state = Done;
                    return;
                }
                m_hookNsTotal = m_hookNsMax = m_keyNs = m_keyMaxNs = m_keyframes = m_oddRecorded = m_pictures = m_strayGifTotal = 0;
                m_exact = false;
                std::string why;
                if (!ps2x::capture::exactCapable(*m_runtime))
                    why = "the GS is not the synchronous software rasteriser (needs PS2X_GS_GPU=0 PS2X_GS_HW=0 PS2X_GS_THREAD=0 PS2X_MTVU=0)";
                else if (const char *track = std::getenv("RC_TRACK_SHADER"); track && track[0] == '1')
                    why = "RC_TRACK_SHADER=1 replaces VU1 programs";
                else
                    m_exact = true;
                if (!m_exact && m_strict)
                {
                    std::fprintf(stderr, "[frame-record] RC_FRAME_RECORD_STRICT: exact mode is not possible: %s; stopping\n", why.c_str());
                    std::fflush(stderr);
                    std::_Exit(3);
                }
                std::fprintf(stderr, "[frame-record] %s\n", m_exact ? "exact mode: reference pictures, register files and GIF hashes"
                                                                    : ("plain mode: " + why).c_str());
                m_writer = std::move(writer);
                m_consumers = {m_writer.get()};
                m_tracker = std::make_unique<PageTracker>();
                FileInfo info;
                FileHeader &h = info.header;
                h.gameVersion = RC_VERSION_EU ? 1u : 0u;
                h.width = 640u;
                h.height = RC_VERSION_EU ? 256u : 240u;
                h.fieldRateMilliHz = RC_VERSION_EU ? 50000u : 59940u;
                h.ramBytes = kRamBytes;
                h.scratchpadBytes = kScratchpadBytes;
                h.pageBytes = kPageBytes;
                h.gsMemBytes = kGsMemBytes;
                h.emuStateVersion = emuStateVersion();
                if (m_exact)
                    h.flags |= kFileExact | (m_refEvery ? kFilePictures : 0u);
                if (m_tagsAttached)
                {
                    h.flags |= kFileTags;
                    info.tagNames = rc::gfx::nameTable();
                }
                m_picture.assign(static_cast<size_t>(h.width) * h.height * 4u, 0u);
                m_width = h.width;
                m_height = h.height;
                copyName(h.elfName, sizeof(h.elfName), RC_ELF_NAME);
                copyName(h.gsBackend, sizeof(h.gsBackend), backendName().c_str());
                copyName(h.build, sizeof(h.build), RC_BUILD_DESCRIBE);
                for (Consumer *c : m_consumers)
                    c->open(info);
                m_state = Recording;
                m_recorded = 0;
                m_firstCycle = m_cycle;
                if (m_drawLists)
                {
                    std::lock_guard<std::mutex> lock(m_dlMutex);
                    m_dlOpen = true;
                    m_dlFirst = static_cast<int64_t>(m_cycle);
                    m_dlLast = -1;
                }
                if (m_ring)
                    std::fprintf(stderr, "[frame-record] ring: keeping the last %llu display cycles in memory (segments of %llu) from cycle %llu; "
                                         "F10 writes them to %s\n",
                                 static_cast<unsigned long long>(m_trigger.value), static_cast<unsigned long long>(m_keyEvery),
                                 static_cast<unsigned long long>(m_cycle), pathFor(m_dumpIndex).c_str());
                else
                    std::fprintf(stderr, "[frame-record] recording %s from display cycle %llu (game frame %lld) for %llu cycles\n", path.c_str(),
                                 static_cast<unsigned long long>(m_cycle), static_cast<long long>(m_gameFrame),
                                 static_cast<unsigned long long>(m_trigger.count));
                keyframe(false);
                for (Consumer *c : m_consumers)
                    c->cycleBegin(m_cycle);
                resetCycleStats();
            }

            void finish()
            {
                // (P2 step 9 batch 6) The lists of the cycles recorded so far, made on the reader thread, go in first.
                if (m_drawLists)
                {
                    drainLive();
                    std::lock_guard<std::mutex> lock(m_dlMutex);
                    m_dlOpen = false;
                }
                keyframe(true);
                for (Consumer *c : m_consumers)
                    c->close();
                m_state = Done;
                const Writer::Stats &s = m_writer->stats();
                const double cycles = std::max<double>(1.0, static_cast<double>(m_recorded));
                std::fprintf(stderr,
                             "[frame-record] wrote %s: cycles %llu-%llu (%llu), %llu keyframes, %.1f MB on disc; cycles %.1f MB raw -> %.1f MB "
                             "stored (%.0f KB / %.0f KB a cycle; largest %.0f KB / %.0f KB); keyframes %.1f MB raw -> %.1f MB stored\n",
                             m_writer->path().c_str(), static_cast<unsigned long long>(m_firstCycle),
                             static_cast<unsigned long long>(m_firstCycle + m_recorded - 1), static_cast<unsigned long long>(m_recorded),
                             static_cast<unsigned long long>(s.keyframes), s.fileBytes / 1048576.0, s.cycleRawBytes / 1048576.0,
                             s.cycleStoredBytes / 1048576.0, s.cycleRawBytes / cycles / 1024.0, s.cycleStoredBytes / cycles / 1024.0,
                             s.maxCycleRaw / 1024.0, s.maxCycleStored / 1024.0, s.keyRawBytes / 1048576.0, s.keyStoredBytes / 1048576.0);
                static const char *names[9] = {"", "PAGE", "HW", "CRT", "KICK", "TAG", "ODD", "END", "NOTE"};
                std::string line;
                for (int t = 1; t < 9; ++t)
                    if (s.recordCount[t])
                        line += std::string(" ") + names[t] + " " + std::to_string(s.recordCount[t]) + " (" +
                                std::to_string(s.recordBytes[t] / 1024u) + " KB)";
                std::fprintf(stderr, "[frame-record] records:%s\n", line.c_str());
                if (m_drawLists)
                    std::fprintf(stderr, "[frame-record] draw lists (DLST): %llu of %llu cycles, %.1f MB raw -> %.1f MB stored\n",
                                 static_cast<unsigned long long>(s.drawLists), static_cast<unsigned long long>(m_recorded),
                                 s.drawListRawBytes / 1048576.0, s.drawListStoredBytes / 1048576.0);
                std::fprintf(stderr,
                             "[frame-record] recorder time: %.2f ms a cycle on average in the kick hooks (largest %.2f ms), keyframes %.1f ms "
                             "on average (largest %.1f ms); %llu odd events recorded, %llu hook calls off the EE thread\n",
                             m_hookNsTotal / cycles / 1e6, m_hookNsMax / 1e6, m_keyNs / std::max<double>(1.0, m_keyframes) / 1e6,
                             m_keyMaxNs / 1e6, static_cast<unsigned long long>(m_oddRecorded),
                             static_cast<unsigned long long>(m_foreign.load()));
                {
                    static const char *kinds[9] = {"?", "Scene", "View", "Pass", "Draw", "Hud", "Kick", "Label", "Cut"};
                    std::string made;
                    for (int k = 1; k < 9; ++k)
                        if (m_tagsMade[k])
                            made += std::string(" ") + kinds[k] + " " + std::to_string(m_tagsMade[k]);
                    std::fprintf(stderr,
                                 "[frame-record] side tags (since the recorder attached):%s; recorded %llu; anchored resolved %llu, dropped "
                                 "unconsumed %llu (while recording), repeats %llu, unanchorable %llu; kicks with labels %llu\n",
                                 made.empty() ? " none" : made.c_str(), static_cast<unsigned long long>(m_tagsRecorded),
                                 static_cast<unsigned long long>(m_tagsResolved), static_cast<unsigned long long>(m_tagsExpired),
                                 static_cast<unsigned long long>(m_tagRepeats), static_cast<unsigned long long>(m_tagsUnanchorable),
                                 static_cast<unsigned long long>(m_labelledKicks));
                }
                if (m_exact)
                    std::fprintf(stderr, "[frame-record] exact mode: %llu reference pictures stored, %llu GIF bytes outside any kick\n",
                                 static_cast<unsigned long long>(m_pictures), static_cast<unsigned long long>(m_strayGifTotal));
                m_consumers.clear();
                m_writer.reset();
                m_tracker.reset();
                ++m_dumpIndex;
                if (m_trigger.kind == Trigger::Key)
                {
                    // (batch 4) The next F10 records again, into the next file.
                    m_state = Armed;
                    std::fprintf(stderr, "[frame-record] F10 records again, to %s\n", pathFor(m_dumpIndex).c_str());
                    return;
                }
                // Tags are of no use any more: detach the channel (call sites go back to one test of a byte).
                rc::gfx::detail::attach(nullptr);
                m_tagsAttached = false;
                m_tags.clear();
                m_pendingAnchored.clear();
                m_openLabels.clear();
            }

            void resetCycleStats()
            {
                m_cycleHookNs = 0;
                m_cyclePages = 0;
            }

            void closeCycle(uint64_t dispfb2, uint32_t fbp)
            {
                if (m_state == Recording)
                {
                    flushPendingKick();
                    const GSRegisters &g = m_runtime->memory().gs();
                    CycleInfo info;
                    RecEndBody &e = info.end;
                    e.cycle = m_cycle;
                    e.gameFrame = m_gameFrame;
                    e.field = g.vsyncTick.load(std::memory_order_relaxed);
                    e.hostNs = nsSince(m_segmentStart);
                    e.dispfb2 = dispfb2;
                    e.display2 = g.display2;
                    e.pmode = g.pmode;
                    e.refFbp = fbp;
                    e.kickCount = m_kickIndex;
                    info.index = static_cast<uint32_t>(m_recorded);
                    const uint8_t *picture = nullptr;
                    if (m_exact && m_refHash)
                    {
                        // The reference: the frame buffer the closing store shows, read now (section 3.5).
                        const Clock::time_point t0 = Clock::now();
                        PS2Memory &memory = m_runtime->memory();
                        readPicture(memory.getGSVRAM(), kGsMemBytes, fbp, 10u, m_width, m_height, m_picture.data());
                        e.refWidth = m_width;
                        e.refHeight = m_height;
                        e.refHash = Hash64::of(m_picture.data(), m_picture.size());
                        if (m_refEvery && m_recorded % m_refEvery == 0u)
                        {
                            e.hasImage = 1u;
                            picture = m_picture.data();
                            ++m_pictures;
                        }
                        m_cycleHookNs += nsSince(t0);
                    }
                    if (m_strayGifBytes)
                    {
                        const std::string text = std::to_string(m_strayGifBytes) + " GIF bytes outside any kick in this cycle";
                        for (Consumer *c : m_consumers)
                            c->note(text);
                        m_strayGifTotal += m_strayGifBytes;
                        m_strayGifBytes = 0;
                    }
                    for (Consumer *c : m_consumers)
                        c->cycleEnd(info, picture);
                    m_lastRecordedCycle = static_cast<int64_t>(m_cycle);
                    if (m_drawLists)
                    {
                        std::lock_guard<std::mutex> lock(m_dlMutex);
                        m_dlLast = m_lastRecordedCycle;
                    }
                    m_hookNsTotal += m_cycleHookNs;
                    m_hookNsMax = std::max(m_hookNsMax, m_cycleHookNs);
                    m_statusHookMax = std::max(m_statusHookMax, m_cycleHookNs);
                    ++m_recorded;
                    ++m_inSegment;
                }
                if (live())
                {
                    // (batch 5) The live consumers' close: the display registers, no picture.
                    flushPendingKick();
                    const GSRegisters &g = m_runtime->memory().gs();
                    CycleInfo info;
                    RecEndBody &e = info.end;
                    e.cycle = m_cycle;
                    e.gameFrame = m_gameFrame;
                    e.field = g.vsyncTick.load(std::memory_order_relaxed);
                    e.dispfb2 = dispfb2;
                    e.display2 = g.display2;
                    e.pmode = g.pmode;
                    e.refFbp = fbp;
                    e.refWidth = 640u;
                    e.refHeight = RC_VERSION_EU ? 256u : 240u;
                    e.kickCount = m_kickIndex;
                    info.index = static_cast<uint32_t>(m_cycle);
                    for (Consumer *c : m_live)
                        c->cycleEnd(info, nullptr);
                }
                ++m_cycle;
                m_kicksThisCycle = 0;
                m_kickIndex = 0;
                m_lastCloseFbp = fbp;
                for (Consumer *c : m_live)
                    c->cycleBegin(m_cycle);
                expireTags();
                // (batch 4) F10 (or a simulated press) since the last close. A cycle cut by a state load acts on none:
                // the keyframe after the load comes next.
                bool press = false;
                if (!m_stateLoadCut)
                {
                    press = m_keyPresses.exchange(0, std::memory_order_relaxed) != 0u;
                    while (!m_simPresses.empty() && m_simPresses.front() <= m_cycle)
                    {
                        m_simPresses.erase(m_simPresses.begin());
                        std::fprintf(stderr, "[frame-record] F10 (simulated, RC_FRAME_RECORD_F10) at display cycle %llu\n",
                                     static_cast<unsigned long long>(m_cycle));
                        press = true;
                    }
                }
                if (m_state == Recording)
                {
                    if (m_ring)
                    {
                        if (press)
                            ringDump();
                        if (m_inSegment >= m_keyEvery && !m_stateLoadCut)
                            keyframe(false);
                        for (Consumer *c : m_consumers)
                            c->cycleBegin(m_cycle);
                        resetCycleStats();
                    }
                    else if (m_recorded >= m_trigger.count || (press && m_trigger.kind == Trigger::Key))
                        finish();
                    else
                    {
                        if (m_inSegment >= m_keyEvery && !m_stateLoadCut)
                            keyframe(false);
                        for (Consumer *c : m_consumers)
                            c->cycleBegin(m_cycle);
                        resetCycleStats();
                    }
                }
                else if (m_state == Armed && (m_trigger.kind == Trigger::Key ? press : triggered()))
                    start();
            }

            // (batch 4) F10 in ring mode: a final keyframe (the state after the last cycle, for the dump's end check),
            // then the writer's worker writes the ring and it to the next file. Recording goes on.
            void ringDump()
            {
                if (m_drawLists)
                    drainLive();
                keyframe(true);
                const std::string path = pathFor(m_dumpIndex++);
                const Writer::RingStats r = m_writer->ringStats();
                std::fprintf(stderr, "[frame-record] F10: writing the ring (about %llu cycles) to %s\n", static_cast<unsigned long long>(r.cycles),
                             path.c_str());
                m_writer->dump(path);
            }

            // (batch 4) Ring mode, at each new segment: what the ring holds and what the last segment cost. Sizes come from
            // the writer's worker and lag the EE thread by the jobs still queued.
            void ringStatus()
            {
                const Writer::RingStats r = m_writer->ringStats();
                const Writer::Stats &s = m_writer->stats(); // record counts and sizes: EE side
                const uint64_t cycles = m_recorded - m_statusRecorded;
                if (cycles)
                {
                    const double n = static_cast<double>(cycles);
                    auto kb = [&](int t) { return (s.recordBytes[t] - m_statusRecordBytes[t]) / n / 1024.0; };
                    const uint64_t enc = r.cyclesEncoded - m_statusEncoded;
                    std::fprintf(stderr,
                                 "[frame-record] ring: segment of %llu cycles to cycle %llu (scene 0x%x): a cycle %.0f KB raw (PAGE %.0f, KICK %.1f, "
                                 "HW %.1f, TAG %.1f, END %.1f) -> %.0f KB stored; recorder %.2f ms a cycle (largest %.2f); ring %llu cycles in %llu "
                                 "segments, %.1f MB (peak %.1f MB)\n",
                                 static_cast<unsigned long long>(cycles), static_cast<unsigned long long>(m_cycle - 1u), m_scene,
                                 (r.cycleRawBytes - m_statusRaw) / std::max<double>(1.0, enc) / 1024.0, kb(RecPage), kb(RecKick), kb(RecHw), kb(RecTag),
                                 kb(RecEnd), (r.cycleStoredBytes - m_statusStored) / std::max<double>(1.0, enc) / 1024.0,
                                 (m_hookNsTotal - m_statusHookNs) / n / 1e6, m_statusHookMax / 1e6, static_cast<unsigned long long>(r.cycles),
                                 static_cast<unsigned long long>(r.segments), r.bytes / 1048576.0, r.peakBytes / 1048576.0);
                    m_statusEncoded = r.cyclesEncoded;
                    m_statusRaw = r.cycleRawBytes;
                    m_statusStored = r.cycleStoredBytes;
                }
                m_statusRecorded = m_recorded;
                for (int t = 0; t < 9; ++t)
                    m_statusRecordBytes[t] = s.recordBytes[t];
                m_statusHookNs = m_hookNsTotal;
                m_statusHookMax = 0;
            }

            static std::string backendName()
            {
                auto on = [](const char *name)
                {
                    const char *v = std::getenv(name);
                    return v && *v && std::strcmp(v, "0") != 0;
                };
                std::string name = on("PS2X_GS_HW") ? "hw" : on("PS2X_GS_VK") ? "vk" : on("PS2X_GS_GPU") ? "gpu" : "cpu";
                if (on("PS2X_GS_THREAD"))
                    name += "+thread";
                if (on("PS2X_MTVU"))
                    name += "+mtvu";
                return name;
            }

            std::string m_path;
            Trigger m_trigger;
            uint64_t m_keyEvery;
            PS2Runtime *m_runtime = nullptr;
            State m_state = Armed;

            std::thread::id m_ee{};
            bool m_eeKnown = false;
            std::atomic<uint64_t> m_foreign{0}, m_oddCount{0};

            uint64_t m_cycle = 0; // display cycles closed since boot = the number of the open cycle
            uint32_t m_kicksThisCycle = 0, m_kickIndex = 0;
            uint32_t m_lastCloseFbp = ~0u;
            uint64_t m_gameFrame = ~0ull, m_levelFrame = ~0ull;
            uint32_t m_scene = ~0u;

            std::unique_ptr<Writer> m_writer;
            std::unique_ptr<PageTracker> m_tracker;
            std::vector<Consumer *> m_consumers;
            uint64_t m_recorded = 0, m_inSegment = 0, m_firstCycle = 0;
            Clock::time_point m_segmentStart = Clock::now();

            bool m_inKick = false, m_kickPending = false;
            bool m_kickRecorded = false; // the kick in progress goes to the recording consumers (page tracker, hashes)
            bool m_recordingSet = false; // RC_FRAME_RECORD configured a recording
            std::vector<Consumer *> m_live; // (batch 5) live consumers
            bool m_drawLists = false;        // (step 8 batch 4) RC_FRAME_RECORD_DRAWLISTS=1
            std::mutex m_dlMutex;            // (P2 step 9 batch 6) the cycles whose lists are wanted, from any thread
            bool m_dlOpen = false;
            int64_t m_dlFirst = 0, m_dlLast = -1;
            int64_t m_lastRecordedCycle = -1;
            uint64_t m_drawListsWritten = 0;
            KickView m_kick{};
            // Exact mode (batch 2).
            bool m_exact = false, m_strict = false, m_refHash = true, m_inGif = false;
            uint64_t m_refEvery = 1, m_pictures = 0, m_strayGifBytes = 0, m_strayGifTotal = 0;
            uint64_t m_regs[kGsRegCount] = {};
            Hash64 m_gifHash;
            std::vector<uint8_t> m_picture;
            uint32_t m_width = 640, m_height = 240;
            std::vector<Range> m_ranges;
            Clock::time_point m_kickStart{};

            std::mutex m_deferredMutex;
            std::vector<Deferred> m_deferred;
            std::atomic<bool> m_hasDeferred{false};

            // Side tags (batch 3).
        public:
            bool m_tagsAttached = false;

        private:
            std::unordered_map<uint32_t, std::unique_ptr<TagEntry>> m_tags;
            std::vector<uint32_t> m_pendingAnchored, m_openLabels;
            std::vector<TagRange> m_tagRanges;
            std::vector<Span> m_spans;
            std::vector<const TagView *> m_labelViews;
            bool m_resolveKick = false;
            uint32_t m_streamPos = 0, m_tteBytes = 0;
            uint64_t m_tagsMade[9] = {}, m_tagsRecorded = 0, m_tagsResolved = 0, m_tagsExpired = 0, m_tagRepeats = 0, m_tagsUnanchorable = 0,
                     m_labelledKicks = 0, m_tagpRecords = 0;

            uint64_t m_cycleHookNs = 0, m_cyclePages = 0, m_hookNsTotal = 0, m_hookNsMax = 0;
            // Key, ring and state loads (batch 4).
            std::atomic<uint32_t> m_keyPresses{0};
            std::vector<uint64_t> m_simPresses;
            uint64_t m_dumpIndex = 0, m_stateLoads = 0;
            bool m_ring = false, m_stateLoadCut = false;
            uint64_t m_statusRecorded = 0, m_statusEncoded = 0, m_statusRaw = 0, m_statusStored = 0, m_statusHookNs = 0, m_statusHookMax = 0;
            uint64_t m_statusRecordBytes[9] = {};
            uint64_t m_keyNs = 0, m_keyMaxNs = 0, m_keyframes = 0, m_oddRecorded = 0;
        };

        Capture *g_capture = nullptr;

        const ps2x::capture::Hooks kHooks = {
            [](PS2Runtime *rt) { g_capture->attach(rt); },
            [](uint32_t base, uint32_t chcr, uint32_t madr, uint32_t qwc, uint32_t tadr, uint32_t asr0, uint32_t asr1, uint32_t ra)
            { g_capture->kickBegin(base, chcr, madr, qwc, tadr, asr0, asr1, ra); },
            [](uint32_t phys, uint32_t bytes, bool scratch, bool isTag) { g_capture->kickTouch(phys, bytes, scratch, isTag); },
            [](const uint8_t *flat, uint32_t bytes, uint32_t tags) { g_capture->kickEnd(flat, bytes, tags); },
            []() { g_capture->kickDone(); },
            [](int path, const uint8_t *data, uint32_t bytes) { g_capture->gifPacket(path, data, bytes); },
            [](uint32_t address, uint32_t width, uint64_t value, uint32_t ra) { g_capture->hwStore(address, width, value, ra); },
            [](int interlaced, int mode, int ffmd) { g_capture->setCrt(interlaced, mode, ffmd); },
            [](int kind, uint32_t address, uint64_t value) { g_capture->odd(kind, address, value); },
            []() { g_capture->stateLoaded(); },
            []() { g_capture->hotkey(); },
            [](uint64_t frame, uint64_t levelFrame, uint32_t scene) { g_capture->gameFrame(frame, levelFrame, scene); },
            []() { g_capture->stateLoading(); },
        };

        // The capture is made by whichever comes first: RC_FRAME_RECORD (below) or a live consumer (addLiveConsumer, from
        // another static initializer: g_capture is constant-initialised to null, so the order does not matter).
        Capture *ensureCapture()
        {
            if (!g_capture)
            {
                g_capture = new Capture();
                ps2x::capture::install(&kHooks);
            }
            return g_capture;
        }

        const bool g_installed = []
        {
            const char *path = std::getenv("RC_FRAME_RECORD");
            if (!path || !*path)
                return false;
            const char *key = std::getenv("RC_FRAME_RECORD_KEY");
            ensureCapture()->setRecording(path, parseTrigger(std::getenv("RC_FRAME_RECORD_AT")), key ? std::strtoull(key, nullptr, 10) : 0u);
            // (batch 3) The side-tag channel feeds the capture from now on; RC_FRAME_RECORD_TAGS=0 leaves it detached.
            const char *tags = std::getenv("RC_FRAME_RECORD_TAGS");
            if (!tags || std::strcmp(tags, "0") != 0)
            {
                rc::gfx::detail::attach(g_capture);
                g_capture->m_tagsAttached = true;
            }
            return true;
        }();
    }

    void addLiveConsumer(Consumer *consumer)
    {
        if (consumer)
            ensureCapture()->addLive(consumer);
    }

    bool captureInstalled() { return g_installed; }

    bool drawListWanted(uint64_t cycle) { return g_capture && g_capture->drawListWanted(cycle); }

    void recordDrawList(uint64_t cycle, std::vector<uint8_t> &&payload)
    {
        if (g_capture)
            g_capture->drawList(cycle, std::move(payload));
    }
}

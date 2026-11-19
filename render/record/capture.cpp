// Live capture: see capture.h and RENDERER_FRAME_RECORD.md sections 3, 6 and 8.
#include "capture.h"

#include "emu_state.h"
#include "page_tracker.h"
#include "picture.h"
#include "writer.h"

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
                Unsupported
            } kind = Boot;
            uint64_t value = 0;
            uint64_t count = 120;
            std::string text;
        };

        Trigger parseTrigger(const char *s)
        {
            Trigger t;
            t.text = s && *s ? s : "boot+120";
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
            else
                t.kind = Trigger::Unsupported;
            if (t.count == 0)
                t.count = 120;
            return t;
        }

        void copyName(char *dst, size_t size, const char *src)
        {
            std::memset(dst, 0, size);
            std::strncpy(dst, src, size - 1);
        }

        class Capture
        {
        public:
            Capture(std::string path, Trigger trigger, uint64_t keyEvery)
                : m_path(std::move(path)), m_trigger(std::move(trigger)), m_keyEvery(keyEvery ? keyEvery : 300)
            {
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
                const size_t pct = m_path.find("%d");
                if (pct != std::string::npos)
                    m_path.replace(pct, 2, "0");
            }

            // --- hooks -------------------------------------------------------------------------------------------
            void attach(PS2Runtime *runtime)
            {
                m_runtime = runtime;
                std::fprintf(stderr, "[frame-record] armed: %s, trigger %s, keyframe every %llu cycles\n", m_path.c_str(),
                             m_trigger.text.c_str(), static_cast<unsigned long long>(m_keyEvery));
                if (m_trigger.kind == Trigger::Unsupported)
                {
                    std::fprintf(stderr, "[frame-record] trigger '%s' is not supported in this build (boot, cycle:, game:, level:, scene:); not recording\n",
                                 m_trigger.text.c_str());
                    m_state = Done;
                }
                else if (m_trigger.kind == Trigger::Boot)
                    start();
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
                if (m_state != Recording)
                    return;
                flushPendingKick();
                flushDeferred();
                m_kickStart = Clock::now();
                const uint8_t channel = static_cast<uint8_t>(((channelBase >> 12) & 0xFu) - 8u);
                if (channel == 0u)
                    emitOdd(ps2x::capture::OddVif0Kick, channelBase, chcr);
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
                if (m_exact)
                {
                    // The GS register file before the kick (everything before it has been drawn: exact mode is synchronous).
                    std::memcpy(m_regs, ps2x::capture::g_gsRawRegs, sizeof(m_regs));
                    m_kick.regs = m_regs;
                    m_gifHash = Hash64{};
                    m_inGif = true;
                }
                m_tracker->beginKick();
                m_ranges.clear();
                m_inKick = true;
            }

            void kickTouch(uint32_t physAddr, uint32_t bytes, bool scratchpad, bool isTag)
            {
                if (!m_inKick || !onEe(2))
                    return;
                m_tracker->touch(physAddr, bytes, scratchpad);
                if (!isTag && m_kick.mode != 1u)
                    m_ranges.push_back({physAddr, bytes, scratchpad});
            }

            void kickEnd(const uint8_t *flat, uint32_t flatBytes, uint32_t dmaTags)
            {
                if (!m_inKick || !onEe(3))
                    return;
                m_inKick = false;
                PS2Memory &memory = m_runtime->memory();
                const uint8_t *ram = memory.getRDRAM(), *spr = memory.getScratchpad();
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
                m_kick.dmaTags = dmaTags;
                m_kick.streamBytes = static_cast<uint32_t>(hash.length());
                m_kick.streamHash = hash.finish();
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
                for (Consumer *c : m_consumers)
                    c->kick(m_kick, live);
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

            void keyframe(bool final)
            {
                const Clock::time_point t0 = Clock::now();
                KeyframeState state;
                Keyframe kf;
                kf.header.cycle = m_cycle;
                kf.header.gameFrame = m_gameFrame;
                kf.header.field = m_runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed);
                // Plain mode: GS memory as the backend gives it; exact mode: the software GS's own memory.
                kf.header.flags = (final ? kKeyFinal : 0u) | (m_exact ? kKeyVramExact : 0u);
                if (!captureKeyframeState(*m_runtime, state))
                    std::fprintf(stderr, "[frame-record] keyframe at cycle %llu: the GS has no memory yet\n",
                                 static_cast<unsigned long long>(m_cycle));
                if (state.gsMemory.empty())
                    state.gsMemory.assign(kGsMemBytes, 0u);
                kf.chunks = state.chunks(final);
                for (Consumer *c : m_consumers)
                    c->keyframe(kf);
                if (!final)
                {
                    m_tracker->reset();
                    m_segmentStart = Clock::now();
                    m_inSegment = 0;
                }
                const uint64_t ns = nsSince(t0);
                m_keyNs += ns;
                m_keyMaxNs = std::max(m_keyMaxNs, ns);
                ++m_keyframes;
            }

            void start()
            {
                auto writer = std::make_unique<Writer>(m_path);
                if (!writer->ok())
                {
                    std::fprintf(stderr, "[frame-record] cannot write %s; not recording\n", m_path.c_str());
                    m_state = Done;
                    return;
                }
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
                std::fprintf(stderr, "[frame-record] recording from display cycle %llu (game frame %lld) for %llu cycles\n",
                             static_cast<unsigned long long>(m_cycle), static_cast<long long>(m_gameFrame),
                             static_cast<unsigned long long>(m_trigger.count));
                keyframe(false);
                for (Consumer *c : m_consumers)
                    c->cycleBegin(m_cycle);
                resetCycleStats();
            }

            void finish()
            {
                keyframe(true);
                for (Consumer *c : m_consumers)
                    c->close();
                m_state = Done;
                const Writer::Stats &s = m_writer->stats();
                const double cycles = std::max<double>(1.0, static_cast<double>(m_recorded));
                std::fprintf(stderr,
                             "[frame-record] wrote %s: cycles %llu-%llu (%llu), %llu keyframes, %.1f MB on disc; cycles %.1f MB raw -> %.1f MB "
                             "stored (%.0f KB / %.0f KB a cycle; largest %.0f KB / %.0f KB); keyframes %.1f MB raw -> %.1f MB stored\n",
                             m_path.c_str(), static_cast<unsigned long long>(m_firstCycle),
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
                std::fprintf(stderr,
                             "[frame-record] recorder time: %.2f ms a cycle on average in the kick hooks (largest %.2f ms), keyframes %.1f ms "
                             "on average (largest %.1f ms); %llu odd events recorded, %llu hook calls off the EE thread\n",
                             m_hookNsTotal / cycles / 1e6, m_hookNsMax / 1e6, m_keyNs / std::max<double>(1.0, m_keyframes) / 1e6,
                             m_keyMaxNs / 1e6, static_cast<unsigned long long>(m_oddRecorded),
                             static_cast<unsigned long long>(m_foreign.load()));
                if (m_exact)
                    std::fprintf(stderr, "[frame-record] exact mode: %llu reference pictures stored, %llu GIF bytes outside any kick\n",
                                 static_cast<unsigned long long>(m_pictures), static_cast<unsigned long long>(m_strayGifTotal));
                m_consumers.clear();
                m_writer.reset();
                m_tracker.reset();
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
                    m_hookNsTotal += m_cycleHookNs;
                    m_hookNsMax = std::max(m_hookNsMax, m_cycleHookNs);
                    ++m_recorded;
                    ++m_inSegment;
                }
                ++m_cycle;
                m_kicksThisCycle = 0;
                m_kickIndex = 0;
                m_lastCloseFbp = fbp;
                if (m_state == Recording)
                {
                    if (m_recorded >= m_trigger.count)
                        finish();
                    else
                    {
                        if (m_inSegment >= m_keyEvery)
                            keyframe(false);
                        for (Consumer *c : m_consumers)
                            c->cycleBegin(m_cycle);
                        resetCycleStats();
                    }
                }
                else if (m_state == Armed && triggered())
                    start();
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

            uint64_t m_cycleHookNs = 0, m_cyclePages = 0, m_hookNsTotal = 0, m_hookNsMax = 0;
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
            nullptr, // stateLoaded: batch 4
            nullptr, // hotkey: batch 4
            [](uint64_t frame, uint64_t levelFrame, uint32_t scene) { g_capture->gameFrame(frame, levelFrame, scene); },
        };

        const bool g_installed = []
        {
            const char *path = std::getenv("RC_FRAME_RECORD");
            if (!path || !*path)
                return false;
            const char *key = std::getenv("RC_FRAME_RECORD_KEY");
            g_capture = new Capture(path, parseTrigger(std::getenv("RC_FRAME_RECORD_AT")), key ? std::strtoull(key, nullptr, 10) : 300u);
            ps2x::capture::install(&kHooks);
            return true;
        }();
    }

    bool captureInstalled() { return g_installed; }
}

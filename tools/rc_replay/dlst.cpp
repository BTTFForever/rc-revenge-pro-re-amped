// rc_replay's dlst consumer: see dlst.h.
#include "dlst.h"

#include "../../render/frame/dlst.h"
#include "../../render/live/live2d.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace rc::rec;
using namespace rc::rd;

namespace rc::replay
{
    struct DlstCheck::Impl
    {
        DlstOptions opt;
        rc::live::Mailbox box;
        rc::live::Reader reader{box, true}; // from reset, as in the game
        std::vector<rc::live::Published> drained;

        bool firstCycleSeen = false, notFromBoot = false;
        uint64_t firstCycle = 0;
        // The off-line list of the cycle just closed, waiting for its DLST.
        bool pending = false;
        uint64_t pendingCycle = 0;
        std::vector<uint8_t> offline;

        uint64_t cycles = 0, chunks = 0, equal = 0, differ = 0, missing = 0, stray = 0, bytes = 0, commands = 0, k2Kicks = 0;
        uint64_t keyframesPassed = 0;
        std::vector<std::string> failures;
        uint64_t failuresDropped = 0;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();

        void fail(const std::string &line)
        {
            if (failures.size() < 16u)
                failures.push_back(line);
            else
                ++failuresDropped;
        }
        void closePending()
        {
            if (!pending)
                return;
            ++missing;
            fail("cycle " + std::to_string(pendingCycle) + ": no DLST chunk");
            pending = false;
        }
        std::string firstDifference(const uint8_t *live, uint64_t liveBytes);
    };

    DlstCheck::DlstCheck(const DlstOptions &options) : m(std::make_unique<Impl>())
    {
        m->opt = options;
        m->reader.setListSink([this](uint64_t cycle, const DrawList &list, uint32_t k2)
                              {
                                  encodeDlst(list, cycle, k2, m->offline);
                                  if (static_cast<int64_t>(cycle) == m->opt.faultCycle && !m->offline.empty())
                                  {
                                      const size_t at = m->opt.faultByte ? m->opt.faultByte
                                                                         : sizeof(DlstHeader) + list.cmds.size() * sizeof(DrawCmd) + list.payload.size() / 2u;
                                      m->offline[std::min(at, m->offline.size() - 1u)] ^= 0x01u;
                                      std::printf("dlst: planted fault: byte %zu of cycle %" PRIu64 "'s off-line list flipped\n",
                                                  std::min(at, m->offline.size() - 1u), cycle);
                                  }
                                  m->pending = true;
                                  m->pendingCycle = cycle;
                                  m->commands += list.cmds.size();
                                  m->k2Kicks += k2;
                              });
    }

    DlstCheck::~DlstCheck() = default;

    void DlstCheck::open(const FileInfo &info) { m->reader.open(info); }

    void DlstCheck::keyframe(const Keyframe &kf)
    {
        // Live the reader gets no keyframe but one after a save-state load (the capture's live keyframe).
        if (kf.header.flags & kKeyAfterStateLoad)
        {
            m->reader.keyframe(kf);
            ++m->keyframesPassed;
        }
    }

    void DlstCheck::cycleBegin(uint64_t cycle)
    {
        if (!m->firstCycleSeen)
        {
            m->firstCycleSeen = true;
            m->firstCycle = cycle;
            m->notFromBoot = cycle != 0u;
        }
        m->reader.cycleBegin(cycle);
    }

    void DlstCheck::hw(uint32_t address, uint32_t width, uint64_t value, uint32_t callerRa) { m->reader.hw(address, width, value, callerRa); }

    void DlstCheck::kick(const KickView &k, const RamView &ram) { m->reader.kick(k, ram); }

    void DlstCheck::cycleEnd(const CycleInfo &info, const uint8_t *refRgba)
    {
        m->closePending(); // the previous cycle's DLST would have come before this cycle
        m->reader.cycleEnd(info, refRgba);
        m->drained.clear();
        m->box.take(m->drained);
        ++m->cycles;
    }

    std::string DlstCheck::Impl::firstDifference(const uint8_t *live, uint64_t liveBytes)
    {
        DlstHeader b{};
        if (!decodeDlstHeader(live, liveBytes, b))
            return "the recorded DLST is not a draw list (bad magic or size)";
        return dlstFirstDifference(offline.data(), offline.size(), live, liveBytes, "off-line / live");
    }

    std::string dlstFirstDifference(const uint8_t *first, uint64_t firstBytes, const uint8_t *second, uint64_t secondBytes, const char *names)
    {
        DlstHeader a{}, b{};
        if (!decodeDlstHeader(first, firstBytes, a) || !decodeDlstHeader(second, secondBytes, b))
            return "not a draw list (bad magic or size)";
        char text[320];
        if (a.cycle != b.cycle || a.cmdCount != b.cmdCount || a.payloadBytes != b.payloadBytes || a.k2Kicks != b.k2Kicks)
        {
            std::snprintf(text, sizeof text,
                          "header: cycle %" PRIu64 " / %" PRIu64 ", commands %u / %u, payload %" PRIu64 " / %" PRIu64 " bytes, VIF1 kicks %u / %u (%s)",
                          a.cycle, b.cycle, a.cmdCount, b.cmdCount, a.payloadBytes, b.payloadBytes, a.k2Kicks, b.k2Kicks, names);
            if (a.cmdCount == b.cmdCount && a.payloadBytes == b.payloadBytes)
                return text;
            // Name the first differing command too: a list that grew or shrank usually differs well before its end.
            std::string head = text;
            const uint32_t n = std::min(a.cmdCount, b.cmdCount);
            const DrawCmd *ca = reinterpret_cast<const DrawCmd *>(first + sizeof(DlstHeader));
            const DrawCmd *cb = reinterpret_cast<const DrawCmd *>(second + sizeof(DlstHeader));
            for (uint32_t i = 0; i < n; ++i)
                if (ca[i].op != cb[i].op || ca[i].a != cb[i].a || ca[i].b != cb[i].b || ca[i].c != cb[i].c || ca[i].d != cb[i].d ||
                    ca[i].payloadBytes != cb[i].payloadBytes)
                {
                    std::snprintf(text, sizeof text, "; first differing command %u: op %u / %u, a %08x / %08x, b %08x / %08x, c %08x / %08x, d %" PRIx64 " / %" PRIx64,
                                  i, ca[i].op, cb[i].op, ca[i].a, cb[i].a, ca[i].b, cb[i].b, ca[i].c, cb[i].c, ca[i].d, cb[i].d);
                    return head + text;
                }
            return head;
        }
        const uint32_t n = std::min(a.cmdCount, b.cmdCount);
        const DrawCmd *ca = reinterpret_cast<const DrawCmd *>(first + sizeof(DlstHeader));
        const DrawCmd *cb = reinterpret_cast<const DrawCmd *>(second + sizeof(DlstHeader));
        const uint8_t *pa = first + sizeof(DlstHeader) + static_cast<size_t>(a.cmdCount) * sizeof(DrawCmd);
        const uint8_t *pb = second + sizeof(DlstHeader) + static_cast<size_t>(b.cmdCount) * sizeof(DrawCmd);
        for (uint32_t i = 0; i < n; ++i)
        {
            const DrawCmd &x = ca[i], &y = cb[i];
            if (std::memcmp(&x, &y, sizeof(DrawCmd)) != 0)
            {
                std::snprintf(text, sizeof text,
                              "command %u: op %u / %u, a %08x / %08x, b %08x / %08x, c %08x / %08x, payload %u+%u / %u+%u, d %" PRIx64 " / %" PRIx64, i,
                              x.op, y.op, x.a, y.a, x.b, y.b, x.c, y.c, x.payloadOffset, x.payloadBytes, y.payloadOffset, y.payloadBytes, x.d, y.d);
                return text;
            }
            if (x.payloadBytes && x.payloadOffset + static_cast<uint64_t>(x.payloadBytes) <= a.payloadBytes &&
                y.payloadOffset + static_cast<uint64_t>(y.payloadBytes) <= b.payloadBytes)
                for (uint32_t o = 0; o < x.payloadBytes; ++o)
                    if (pa[x.payloadOffset + o] != pb[y.payloadOffset + o])
                    {
                        std::snprintf(text, sizeof text, "command %u (op %u): payload byte %u of %u: %02x / %02x", i, x.op, o, x.payloadBytes,
                                      pa[x.payloadOffset + o], pb[y.payloadOffset + o]);
                        return text;
                    }
        }
        return "the payload arenas differ between payloads (padding)";
    }

    void DlstCheck::chunk(uint32_t id, const uint8_t *data, uint64_t bytes)
    {
        if (id != kChunkDrawList)
            return;
        ++m->chunks;
        DlstHeader h{};
        const bool ok = decodeDlstHeader(data, bytes, h);
        if (!m->pending)
        {
            ++m->stray;
            m->fail("a DLST chunk" + std::string(ok ? " of cycle " + std::to_string(h.cycle) : "") + " with no cycle before it");
            return;
        }
        m->pending = false;
        m->bytes += bytes;
        if (bytes == m->offline.size() && std::memcmp(data, m->offline.data(), bytes) == 0)
        {
            ++m->equal;
            if (!m->opt.quiet)
                std::printf("cycle %" PRIu64 ": %u commands, %" PRIu64 " payload bytes, %u VIF1 kicks: equal\n", m->pendingCycle, h.cmdCount,
                            h.payloadBytes, h.k2Kicks);
            return;
        }
        ++m->differ;
        const std::string why = m->firstDifference(data, bytes);
        m->fail("cycle " + std::to_string(m->pendingCycle) + ": the live list differs: " + why);
        if (!m->opt.quiet)
            std::printf("cycle %" PRIu64 ": DIFFERS: %s\n", m->pendingCycle, why.c_str());
    }

    bool DlstCheck::refused() const { return m->notFromBoot || m->chunks == 0u; }

    bool DlstCheck::failed() const { return refused() || m->differ || m->missing || m->stray || m->equal != m->cycles; }

    void DlstCheck::summary()
    {
        m->closePending();
        const rc::live::Reader::Stats &r = m->reader.stats();
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - m->t0).count();
        if (m->notFromBoot)
            std::printf("dlst: the dump starts at cycle %" PRIu64 ", not at boot: the live reader's lists (texture ids and generations count from boot) "
                        "cannot be compared with one started here; record from boot (RC_FRAME_RECORD_AT=boot+n)\n",
                        m->firstCycle);
        std::printf("dlst: %" PRIu64 " cycles from %" PRIu64 ", %" PRIu64 " DLST chunks (%.1f MB): %" PRIu64 " equal byte for byte, %" PRIu64 " differ, %" PRIu64
                    " cycles without one, %" PRIu64 " stray; %" PRIu64 " commands, %" PRIu64 " VIF1 kicks (%" PRIu64 " guarded), %" PRIu64
                    " keyframes after a state load; reader %.2f ms a cycle; %.1f s\n",
                    m->cycles, m->firstCycle, m->chunks, m->bytes / 1048576.0, m->equal, m->differ, m->missing, m->stray, m->commands, m->k2Kicks,
                    r.guarded, m->keyframesPassed, r.cycles ? r.ns / r.cycles / 1e6 : 0.0, s);
        for (const std::string &f : m->failures)
            std::printf("FAIL dlst %s\n", f.c_str());
        if (m->failuresDropped)
            std::printf("FAIL dlst ... and %" PRIu64 " more\n", m->failuresDropped);
    }
}

// rc_replay's live2d consumer: see live2d.h.
#include "live2d.h"

#include "png.h"

#include "../../render/live/live2d.h"
#include "../../render/tex/gs_layout.h"

#include "rhi.h"
#include "vk/rhi_vk.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

using namespace rc::rec;
using namespace rc::rd;

namespace rc::replay
{
    namespace
    {
        constexpr uint32_t kW = 640u, kH = 256u, kPixels = kW * kH;
        constexpr uint32_t kZbp = 0xa0u;

        // Colour targets 0x0 and 0x50 (CT32) and Z at kZbp (Z32), 640 x 256 each, from emulated GS memory.
        void readPlanes(const uint8_t *vram, std::vector<uint32_t> (&planes)[3])
        {
            const LayoutTables &lt = layoutTables();
            for (uint32_t p = 0; p < 3; ++p)
            {
                planes[p].resize(kPixels);
                for (uint32_t y = 0; y < kH; ++y)
                    for (uint32_t x = 0; x < kW; ++x)
                    {
                        const uint32_t a = p < 2 ? gsAddr32(lt, p ? 0x50u * 32u : 0u, 10u, x, y) : gsAddrZ32(lt, kZbp * 32u, 10u, x, y);
                        std::memcpy(&planes[p][y * kW + x], vram + (a & (kGsBytes - 1u)), 4);
                    }
            }
        }
    }

    struct Live2dCheck::Impl
    {
        Live2dOptions opt;
        rc::live::Mailbox box;
        rc::live::Reader reader{box, false};
        rc::live::Presenter presenter;
        bool rhiUp = false, up = false, seeded = false;
        std::string adapter;
        uint32_t validationErrors = 0;
        std::map<uint64_t, std::string> shots; // cycle -> .rgba path
        // K2's pixels this cycle: bit 0 target 0x0, bit 1 target 0x50, bit 2 Z
        std::vector<uint8_t> k2;
        std::vector<uint32_t> before[3], after[3];
        bool haveBefore = false, rewrite = false;
        uint64_t rewriteKicks = 0;
        std::vector<rc::live::Published> lists;
        bool pngFirstFailWritten = false;

        uint64_t cyclesJudged = 0, cyclesNoRef = 0, cyclesFailing = 0, pixels = 0, k2Pixels = 0, compared = 0, equal = 0, k1Pixels = 0, k1Equal = 0;
        uint64_t k2Kicks = 0, drawFailures = 0, readFailures = 0;
        uint64_t shotsJudged = 0, shotsEqual = 0, shotPixelsDiffer = 0;
        std::vector<std::string> failures;
        std::vector<std::string> shotLines;

        void writePngs(const std::string &tag, uint64_t cycle, const std::vector<uint8_t> &native, const uint8_t *ref, const std::vector<uint8_t> &mask,
                       const std::vector<uint8_t> *live, uint32_t w, uint32_t h);
    };

    Live2dCheck::Live2dCheck(const Live2dOptions &options) : m(std::make_unique<Impl>()) { m->opt = options; }

    Live2dCheck::~Live2dCheck()
    {
        if (m->rhiUp)
        {
            m->presenter.destroy();
            rhi_shutdown();
        }
    }

    bool Live2dCheck::start(std::string *why)
    {
        RhiDeviceDesc desc{};
        desc.sdlWindow = nullptr;
        desc.vsync = false;
        desc.debugLayers = m->opt.validation;
        desc.appName = "rc_replay live2d";
        if (!rhi_init(&desc))
        {
            if (why)
                *why = "rhi_init failed (no Vulkan device?)";
            return false;
        }
        m->rhiUp = true;
        m->adapter = rhi_adapter_name() ? rhi_adapter_name() : "?";
        rc::live::Presenter::Options o;
        o.blend = m->opt.blend == "ff" ? Gs2dVk::kBlendFixed : m->opt.blend == "read" ? Gs2dVk::kBlendRead : Gs2dVk::kBlendAuto;
        if (!m->presenter.create(o, why))
            return false;
        m->up = true;
        if (!m->opt.shotsDir.empty())
        {
            std::error_code ec;
            for (const auto &e : std::filesystem::directory_iterator(m->opt.shotsDir, ec))
            {
                const std::string name = e.path().filename().string();
                if (name.rfind("native_", 0) == 0 && e.path().extension() == ".rgba")
                    m->shots[std::strtoull(name.c_str() + 7, nullptr, 10)] = e.path().string();
            }
        }
        std::printf("live2d: %s, validation %s, blend %s; live screenshots %zu%s%s\n", m->adapter.c_str(),
                    rhi_limits()->validationLayer ? "on" : "off", m->presenter.gs2d().readMode() ? "read" : "fixed function", m->shots.size(),
                    m->opt.shotsDir.empty() ? "" : " in ", m->opt.shotsDir.c_str());
        return true;
    }

    void Live2dCheck::open(const FileInfo &info) { m->reader.open(info); }
    void Live2dCheck::keyframe(const Keyframe &kf)
    {
        m->reader.keyframe(kf);
        // The first keyframe (and one after a save-state load): the targets start as the machine's GS memory was (live they
        // start as at reset, zero, with the game).
        if (m->up && (!m->seeded || (kf.header.flags & kKeyAfterStateLoad)) && !(kf.header.flags & kKeyFinal))
            if (const StateChunk *g = kf.find(kStateGsMemory); g && g->bytes >= kGsMemBytes)
            {
                m->presenter.seedFromMemory(g->data, static_cast<uint32_t>(g->bytes));
                m->seeded = true;
            }
    }

    void Live2dCheck::cycleBegin(uint64_t cycle)
    {
        // K2's pixels are kept across cycles (a buffer drawn by K2 in one cycle can be shown in the next with no draw):
        // a pixel stops being K2's when a clear or an upload (a PATH3 kick of CLEAR / IMAGE_BLIT and no primitive)
        // changes it.
        if (m->k2.size() != kPixels)
            m->k2.assign(kPixels, 0u);
        m->reader.cycleBegin(cycle);
    }

    void Live2dCheck::hw(uint32_t address, uint32_t width, uint64_t value, uint32_t callerRa) { m->reader.hw(address, width, value, callerRa); }

    void Live2dCheck::kick(const KickView &k, const RamView &ram)
    {
        m->reader.kick(k, ram);
        m->haveBefore = m->rewrite = false;
        if (k.channel == 2u)
        {
            const DrawList &l = m->reader.frame().list();
            bool rewrites = false, draws = false;
            for (size_t i = m->reader.frame().gif().kickFirstCommand(); i < l.cmds.size(); ++i)
            {
                rewrites |= l.cmds[i].op == kOpClear || l.cmds[i].op == kOpImageBlit;
                draws |= l.cmds[i].op == kOpScreenPrims;
            }
            m->rewrite = rewrites && !draws;
        }
        if ((k.channel == 1u || m->rewrite) && m->opt.vram)
            if (const uint8_t *vram = m->opt.vram())
            {
                readPlanes(vram, m->before);
                m->haveBefore = true;
            }
    }

    void Live2dCheck::afterKick(const KickView &k)
    {
        if (!m->haveBefore || !m->opt.vram)
            return;
        const uint8_t *vram = m->opt.vram();
        if (!vram)
            return;
        readPlanes(vram, m->after);
        const bool k2 = k.channel == 1u;
        k2 ? ++m->k2Kicks : ++m->rewriteKicks;
        for (uint32_t p = 0; p < 3; ++p)
            for (uint32_t i = 0; i < kPixels; ++i)
                if (m->before[p][i] != m->after[p][i])
                {
                    if (k2)
                        m->k2[i] |= static_cast<uint8_t>(1u << p);
                    else
                        m->k2[i] &= static_cast<uint8_t>(~(1u << p));
                }
        m->haveBefore = false;
    }

    void Live2dCheck::Impl::writePngs(const std::string &tag, uint64_t cycle, const std::vector<uint8_t> &native, const uint8_t *ref,
                                      const std::vector<uint8_t> &mask, const std::vector<uint8_t> *live, uint32_t w, uint32_t h)
    {
        if (opt.pngDir.empty())
            return;
        std::filesystem::create_directories(opt.pngDir);
        const std::string base = opt.pngDir + "/" + tag + "_" + std::to_string(cycle);
        writePng(base + "_native.png", native.data(), w, h);
        if (ref)
            writePng(base + "_ref.png", ref, w, h);
        std::vector<uint8_t> pic(static_cast<size_t>(w) * h * 4u, 0u);
        for (uint32_t i = 0; i < w * h; ++i)
            if (mask[i])
                pic[i * 4u] = pic[i * 4u + 1u] = pic[i * 4u + 2u] = 0xFFu;
        writePng(base + "_k2.png", pic.data(), w, h);
        if (live)
            writePng(base + "_live.png", live->data(), w, h);
        const uint8_t *other = live ? live->data() : ref;
        if (other)
        {
            for (uint32_t i = 0; i < w * h; ++i)
            {
                bool d = false;
                for (uint32_t c = 0; c < 4; ++c)
                    d |= native[i * 4u + c] != other[i * 4u + c];
                const bool judged = live || !mask[i];
                pic[i * 4u] = d && judged ? 0xFFu : 0u;
                pic[i * 4u + 1u] = judged ? 0u : 0x40u;
                pic[i * 4u + 2u] = 0u;
            }
            writePng(base + "_diff.png", pic.data(), w, h);
        }
    }

    void Live2dCheck::cycleEnd(const CycleInfo &info, const uint8_t *refRgba)
    {
        m->reader.cycleEnd(info, refRgba);
        m->lists.clear();
        m->box.take(m->lists);
        if (!m->up || m->lists.empty())
            return;
        // The presenter's frame, as the vk host makes it (then waited for and read back).
        rhi_wait_frame();
        RhiCommandList cl = rhi_begin_commands();
        const rc::live::Presenter::View v = m->presenter.record(cl, m->lists);
        rhi_submit(cl);
        rhi_wait_idle();
        if (m->presenter.stats().failures != m->drawFailures)
            m->drawFailures = m->presenter.stats().failures;
        if (!v.valid)
            return;
        std::vector<uint8_t> native;
        if (!m->presenter.readShown(native))
        {
            ++m->readFailures;
            return;
        }
        const uint32_t w = v.width, h = std::min(v.height, kH);
        const uint32_t fbp = static_cast<uint32_t>(info.end.dispfb2 & 0x1FFu);
        const uint32_t t = fbp == 0x50u ? 1u : 0u;
        std::vector<uint8_t> mask(static_cast<size_t>(w) * h, 0u);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
                mask[y * w + x] = (m->k2[y * kW + x] & ((1u << t) | 4u)) ? 1u : 0u;
        // The cycle's clear colour (the flip's draw environment), for the count of pixels K1 changed.
        uint32_t clear = 0;
        bool haveClear = false;
        for (const DrawCmd &c : m->lists.back().list.cmds)
            if (c.op == kOpClear)
            {
                clear = c.a;
                haveClear = true;
            }

        const uint64_t cycle = info.end.cycle;
        const bool hasRef = refRgba && info.end.refWidth == w && info.end.refHeight == h;
        bool failedHere = false;
        if (hasRef)
        {
            ++m->cyclesJudged;
            uint64_t bad = 0;
            int fx = -1, fy = -1;
            for (uint32_t i = 0; i < w * h; ++i)
            {
                ++m->pixels;
                if (mask[i])
                    ++m->k2Pixels; // (step 8 batch 4) K2 is drawn: its pixels are judged too
                ++m->compared;
                const bool eq = std::memcmp(&native[i * 4u], refRgba + i * 4u, 4) == 0;
                uint32_t word;
                std::memcpy(&word, refRgba + i * 4u, 4);
                const bool k1 = !haveClear || word != clear;
                m->k1Pixels += k1;
                if (eq)
                {
                    ++m->equal;
                    m->k1Equal += k1;
                }
                else if (bad++ == 0)
                {
                    fx = static_cast<int>(i % w);
                    fy = static_cast<int>(i / w);
                }
            }
            if (bad)
            {
                failedHere = true;
                ++m->cyclesFailing;
                if (m->failures.size() < 20u)
                {
                    char line[256];
                    const uint8_t *n = &native[(fy * w + fx) * 4u], *r = refRgba + (fy * w + fx) * 4u;
                    std::snprintf(line, sizeof line, "cycle %" PRIu64 ": %" PRIu64 " pixels differ; first (%d, %d) native %02x%02x%02x%02x reference %02x%02x%02x%02x",
                                  cycle, bad, fx, fy, n[0], n[1], n[2], n[3], r[0], r[1], r[2], r[3]);
                    m->failures.push_back(line);
                }
                if (!m->pngFirstFailWritten)
                {
                    m->pngFirstFailWritten = true;
                    m->writePngs("fail", cycle, native, refRgba, mask, nullptr, w, h);
                }
            }
        }
        else
            ++m->cyclesNoRef;
        if (!m->opt.quiet && hasRef)
            std::printf("cycle %" PRIu64 ": FBP 0x%x, K2 pixels %" PRIu64 "%s\n", cycle, fbp,
                        static_cast<uint64_t>(std::count(mask.begin(), mask.end(), 1u)), failedHere ? ", FAIL" : "");

        if (const auto it = m->shots.find(cycle); it != m->shots.end())
        {
            ++m->shotsJudged;
            std::vector<uint8_t> live(native.size());
            std::FILE *f = std::fopen(it->second.c_str(), "rb");
            const bool read = f && std::fread(live.data(), 1, live.size(), f) == live.size();
            if (f)
                std::fclose(f);
            uint64_t differ = 0;
            for (size_t i = 0; read && i < live.size() / 4u; ++i)
                differ += std::memcmp(&live[i * 4u], &native[i * 4u], 4) != 0;
            if (read && differ == 0)
                ++m->shotsEqual;
            m->shotPixelsDiffer += differ;
            uint64_t k2 = 0, k1 = 0, k1Eq = 0, cmp = 0, eq = 0;
            for (uint32_t i = 0; hasRef && i < w * h; ++i)
            {
                if (mask[i])
                    ++k2;
                ++cmp;
                uint32_t word;
                std::memcpy(&word, refRgba + i * 4u, 4);
                const bool same = std::memcmp(&live[i * 4u], refRgba + i * 4u, 4) == 0;
                eq += same;
                if (!haveClear || word != clear)
                {
                    ++k1;
                    k1Eq += same;
                }
            }
            char line[400];
            std::snprintf(line, sizeof line,
                          "live screenshot of cycle %" PRIu64 ": %s; against the off-line native picture %" PRIu64 " of %u pixels differ; against the reference "
                          "picture%s: %" PRIu64 " K2 pixels, %" PRIu64 " of %" PRIu64 " compared equal (%" PRIu64 " of the %" PRIu64 " not the clear colour)",
                          cycle, read ? "read" : "NOT READ", differ, w * h, hasRef ? "" : " (none in the dump)", k2, eq, cmp, k1Eq, k1);
            m->shotLines.push_back(line);
            m->writePngs("shot", cycle, native, hasRef ? refRgba : nullptr, mask, read ? &live : nullptr, w, h);
            if (!read || differ)
                m->failures.push_back(line);
        }
    }

    bool Live2dCheck::failed() const
    {
        return m->cyclesFailing || m->drawFailures || m->readFailures || m->validationErrors || m->shotsJudged != m->shotsEqual ||
               (m->shots.size() && !m->shotsJudged);
    }

    void Live2dCheck::summary()
    {
        const bool layer = m->rhiUp && rhi_limits() && rhi_limits()->validationLayer;
        const rc::live::Presenter::Stats &ps = m->presenter.stats();
        const rc::live::Reader::Stats &rs = m->reader.stats();
        const double gpu = ps.gpuSamples ? ps.gpuMs / ps.gpuSamples : 0.0, gpuMax = ps.maxGpuMs;
        if (m->rhiUp)
        {
            m->presenter.destroy();
            m->validationErrors += rhi_vk_validation_error_count();
            rhi_shutdown();
            m->rhiUp = m->up = false;
        }
        std::printf("live2d: reader %" PRIu64 " cycles, %.2f ms a cycle (largest %.2f), %" PRIu64 " VIF1 kicks %.2f ms each, %" PRIu64 " guarded; presenter %" PRIu64
                    " lists, GPU %.3f ms a list (largest %.3f), %" PRIu64 " barriers, draw failures %" PRIu64 ", read-back failures %" PRIu64
                    "; validation errors %u%s\n",
                    rs.cycles, rs.cycles ? rs.ns / rs.cycles / 1e6 : 0.0, rs.maxCycleNs / 1e6, rs.path1Kicks, rs.path1Kicks ? rs.path1Ns / rs.path1Kicks / 1e6 : 0.0,
                    rs.guarded, ps.lists, gpu, gpuMax, ps.barriers, m->drawFailures, m->readFailures, m->validationErrors, layer ? "" : " (layer off)");
        std::printf("live2d: %" PRIu64 " cycles judged against the reference picture (%" PRIu64 " without one); %" PRIu64 " pixels, %" PRIu64
                    " of them K2's (drawn, judged), %" PRIu64 " compared: %" PRIu64 " equal; of them %" PRIu64 " not the clear colour: %" PRIu64
                    " equal; %" PRIu64 " VIF1 kicks, %" PRIu64 " clear / upload kicks; failing cycles %" PRIu64 "\n",
                    m->cyclesJudged, m->cyclesNoRef, m->pixels, m->k2Pixels, m->compared, m->equal, m->k1Pixels, m->k1Equal, m->k2Kicks,
                    m->rewriteKicks, m->cyclesFailing);
        for (const std::string &l : m->shotLines)
            std::printf("live2d: %s\n", l.c_str());
        if (!m->shots.empty())
            std::printf("live2d: live screenshots %zu in the folder, %" PRIu64 " of them in the dump's cycles, %" PRIu64 " equal to the off-line picture\n",
                        m->shots.size(), m->shotsJudged, m->shotsEqual);
        for (const std::string &f : m->failures)
            std::printf("FAIL live2d %s\n", f.c_str());
    }
}

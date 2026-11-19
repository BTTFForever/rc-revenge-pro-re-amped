#include "blit.h"

#include "png.h"
#include "../../render/blit/blit_presenter.h"
#include "../../render/blit/frame_blit.h"

#include "vk/rhi_vk.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <vector>

namespace rc::replay
{
    using namespace rc::rec;
    using rc::rd::FrameBlit;

    struct BlitCheck::Impl
    {
        BlitOptions opt;
        FrameBlit blit;
        rc::rd::BlitPresenter presenter;
        bool gpuUp = false, rhiUp = false, gpuWasUp = false;
        std::string adapter;
        uint64_t cycle = 0;
        uint64_t faultTransferBase = 0;
        bool exact = false;

        // Classification
        uint64_t cycles = 0, noReference = 0;
        uint64_t applicable = 0, applicableQuietCycle = 0, applicableDrawElsewhere = 0, applicableKeyframeOnly = 0;
        uint64_t naDrawn = 0, naDrawnThisCycle = 0, naUnsupported = 0, naEqualAnyway = 0;
        uint64_t quietCycles = 0, quietCyclesNa = 0; // cycles with no drawing at all; of them not applicable
        // Checks on upload-only pictures with a reference
        uint64_t cpuChecked = 0, cpuOk = 0, cpuByImage = 0, cpuByHash = 0;
        uint64_t gpuChecked = 0, gpuOk = 0, gpuUploadOk = 0, gpuErrors = 0;
        uint64_t fbpChecked = 0, fbpOk = 0;
        uint64_t transfersIntoFrame = 0;
        double gpuMs = 0.0, cpuMs = 0.0;
        uint64_t failures = 0;
        std::vector<std::string> messages;
        std::string runs; // the classification as runs of cycles: "a-b U" ...
        char runClass = 0;
        uint64_t runFirst = 0, runLast = 0;

        void fail(const std::string &text)
        {
            ++failures;
            if (messages.size() < 20u)
                messages.push_back(text);
        }

        void addRun(char c, uint64_t at)
        {
            if (c == runClass && at == runLast + 1u)
            {
                runLast = at;
                return;
            }
            flushRun();
            runClass = c;
            runFirst = runLast = at;
        }
        void flushRun()
        {
            if (!runClass)
                return;
            char buf[64];
            if (runFirst == runLast)
                std::snprintf(buf, sizeof(buf), " %" PRIu64 "%c", runFirst, runClass);
            else
                std::snprintf(buf, sizeof(buf), " %" PRIu64 "-%" PRIu64 "%c", runFirst, runLast, runClass);
            runs += buf;
            runClass = 0;
        }
    };

    BlitCheck::BlitCheck(BlitOptions options) : m(std::make_unique<Impl>()) { m->opt = std::move(options); }
    BlitCheck::~BlitCheck() { stopGpu(); }

    bool BlitCheck::startGpu()
    {
        Impl &s = *m;
        if (!s.opt.gpu)
            return true;
        RhiDeviceDesc desc{};
        desc.sdlWindow = nullptr; // headless: no swapchain, an off-screen target
        desc.vsync = false;
        desc.debugLayers = s.opt.validation;
        desc.appName = "rc_replay blit";
        if (!rhi_init(&desc))
        {
            std::printf("blit: rhi_init failed (no Vulkan device?); --no-gpu checks the CPU picture alone\n");
            return false;
        }
        s.rhiUp = true;
        s.adapter = rhi_adapter_name() ? rhi_adapter_name() : "?";
        if (!s.presenter.create())
        {
            std::printf("blit: the presenter's pipeline could not be made\n");
            return false;
        }
        s.gpuUp = s.gpuWasUp = true;
        if (!s.opt.quiet)
            std::printf("blit: GPU %s (%s, headless)\n", s.adapter.c_str(), rhi_backend_kind_name(rhi_backend()));
        return true;
    }

    void BlitCheck::stopGpu()
    {
        Impl &s = *m;
        if (!s.rhiUp)
            return;
        s.presenter.destroy();
        s.gpuErrors += rhi_vk_validation_error_count();
        rhi_shutdown();
        s.rhiUp = s.gpuUp = false;
    }

    void BlitCheck::open(const FileInfo &info)
    {
        m->exact = (info.header.flags & kFileExact) != 0;
        m->blit.open(info);
    }
    void BlitCheck::keyframe(const Keyframe &kf) { m->blit.keyframe(kf); }
    void BlitCheck::hw(uint32_t address, uint32_t width, uint64_t value, uint32_t callerRa) { m->blit.hw(address, width, value, callerRa); }

    void BlitCheck::cycleBegin(uint64_t cycle)
    {
        Impl &s = *m;
        s.cycle = cycle;
        s.blit.cycleBegin(cycle);
        if (s.opt.faultKind == "tile" && static_cast<int64_t>(cycle) == s.opt.faultCycle)
            s.blit.memory().dropTransferForTests(s.blit.memory().stats().transfers + s.opt.faultN + 1u);
    }

    void BlitCheck::kick(const KickView &k, const RamView &ram) { m->blit.kick(k, ram); }

    namespace
    {
        uint64_t countDiff(const uint8_t *a, const uint8_t *b, size_t pixels, bool rgbOnly, uint32_t &first)
        {
            uint64_t n = 0;
            first = ~0u;
            for (size_t i = 0; i < pixels; ++i)
            {
                const bool differ = rgbOnly ? std::memcmp(a + i * 4u, b + i * 4u, 3) != 0 : std::memcmp(a + i * 4u, b + i * 4u, 4) != 0;
                if (differ)
                {
                    if (!n)
                        first = static_cast<uint32_t>(i);
                    ++n;
                }
            }
            return n;
        }
    }

    void BlitCheck::cycleEnd(const CycleInfo &info, const uint8_t *refRgba)
    {
        Impl &s = *m;
        if (!s.blit.started())
            return;
        const uint64_t cyc = info.end.cycle;
        const bool faultHere = static_cast<int64_t>(cyc) == s.opt.faultCycle;
        uint64_t savedDispfb2 = 0;
        if (faultHere && s.opt.faultKind == "dispfb")
        {
            savedDispfb2 = s.blit.dispfb2();
            s.blit.setDispfb2ForTests(savedDispfb2 ^ 0x50u);
        }
        if (faultHere && s.opt.faultKind == "byte")
        {
            // The red byte of displayed pixel n, in the shadow's memory (it stays wrong until an upload replaces it).
            const rc::rd::DisplayBuffer fb = rc::rd::DisplayBuffer::decode(s.blit.dispfb2());
            const uint32_t w = info.end.refWidth ? info.end.refWidth : 640u;
            const uint32_t x = s.opt.faultN % w, y = s.opt.faultN / w;
            const uint32_t a = rc::rd::gsAddr32(rc::rd::layoutTables(), fb.fbp * 32u, fb.fbw ? fb.fbw : 1u, x, y) & (rc::rd::kGsBytes - 1u);
            s.blit.memory().dataForTests()[a] ^= 0xFFu;
        }
        const auto t0 = std::chrono::steady_clock::now();
        s.blit.cycleEnd(info, refRgba);
        s.cpuMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (faultHere && s.opt.faultKind == "dispfb")
            s.blit.setDispfb2ForTests(savedDispfb2);
        const FrameBlit::Picture &p = s.blit.picture();
        const FrameBlit::CycleCounts &cc = s.blit.cycleCounts();
        ++s.cycles;
        s.transfersIntoFrame += cc.transfersIntoFrame;
        const bool hasRef = info.end.refWidth != 0u;
        const size_t pixels = static_cast<size_t>(p.width) * p.height;
        const uint64_t hash = Hash64::of(p.rgba.data(), p.rgba.size());
        const bool quiet = !cc.drew();
        s.quietCycles += quiet;
        if (hasRef)
        {
            ++s.fbpChecked;
            if (p.fb.fbp == info.end.refFbp)
                ++s.fbpOk;
            else
                s.fail("cycle " + std::to_string(cyc) + ": DISPFB2 shows FBP 0x" + std::to_string(p.fb.fbp) + ", the recording " +
                       std::to_string(info.end.refFbp));
        }
        else
            ++s.noReference;

        char cls;
        if (!p.supported)
        {
            ++s.naUnsupported;
            cls = 'x';
        }
        else if (p.drawnWords)
        {
            ++s.naDrawn;
            s.naDrawnThisCycle += cc.drew();
            s.quietCyclesNa += quiet;
            if (hasRef && hash == info.end.refHash)
                ++s.naEqualAnyway;
            cls = '-';
        }
        else
        {
            ++s.applicable;
            (quiet ? s.applicableQuietCycle : s.applicableDrawElsewhere)++;
            if (!p.uploadedWords)
                ++s.applicableKeyframeOnly;
            cls = p.uploadedWords ? 'U' : 'K';
        }
        s.addRun(cls, cyc);
        if (!s.opt.quiet)
            std::printf("cycle %" PRIu64 ": FBP 0x%x, %u transfers (%u into frame memory), %u PATH3 vertices, %u VIF1 kicks; picture %s"
                        " (%u uploaded, %u keyframe, %u drawn words)%s\n",
                        cyc, p.fb.fbp, cc.transfers, cc.transfersIntoFrame, cc.path3Vertices, cc.vif1Kicks,
                        cls == 'U' ? "upload-only" : cls == 'K' ? "keyframe memory only" : cls == 'x' ? "unsupported" : "not applicable",
                        p.uploadedWords, p.loadedWords, p.drawnWords, hasRef ? "" : ", no reference");
        if (cls != 'U' && cls != 'K')
            return;
        if (!s.opt.pngDir.empty())
            writePng(s.opt.pngDir + "/blit" + std::to_string(cyc) + ".png", p.rgba.data(), p.width, p.height);
        if (!hasRef)
            return;

        // The CPU picture against the reference: every byte where the dump kept the picture, else its hash.
        ++s.cpuChecked;
        bool cpuOk = hash == info.end.refHash;
        if (refRgba)
        {
            ++s.cpuByImage;
            uint32_t first = 0;
            const uint64_t diff = countDiff(p.rgba.data(), refRgba, pixels, false, first);
            cpuOk = cpuOk && diff == 0;
            if (diff)
                s.fail("cycle " + std::to_string(cyc) + ": the blit picture differs from REFI in " + std::to_string(diff) + " pixels (first at " +
                       std::to_string(first % p.width) + "," + std::to_string(first / p.width) + ")");
        }
        else
            ++s.cpuByHash;
        if (cpuOk)
            ++s.cpuOk;
        else if (!refRgba)
            s.fail("cycle " + std::to_string(cyc) + ": the blit picture's hash is not the reference's");
        if (!cpuOk && !s.opt.pngDir.empty() && refRgba)
            writePng(s.opt.pngDir + "/blit" + std::to_string(cyc) + "_ref.png", refRgba, p.width, p.height);

        // Through the RHI: upload, draw with the presentation transform, read back.
        if (!s.gpuUp)
            return;
        std::vector<uint8_t> input(p.rgba);
        if (faultHere && s.opt.faultKind == "gpubyte" && s.opt.faultN < pixels)
            input[static_cast<size_t>(s.opt.faultN) * 4u] ^= 0xFFu;
        std::vector<uint8_t> displayed, uploaded;
        const auto g0 = std::chrono::steady_clock::now();
        const bool ran = s.presenter.present(input.data(), p.width, p.height, displayed, &uploaded);
        s.gpuMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g0).count();
        ++s.gpuChecked;
        if (!ran)
        {
            s.fail("cycle " + std::to_string(cyc) + ": the GPU blit failed");
            return;
        }
        // Expected on screen: the reference (REFI, or the CPU picture where its hash proved it equal) with alpha 255.
        const uint8_t *ref = refRgba ? refRgba : p.rgba.data();
        uint64_t diff = 0, alphaBad = 0;
        uint32_t first = ~0u;
        for (size_t i = 0; i < pixels; ++i)
        {
            const bool rgb = std::memcmp(displayed.data() + i * 4u, ref + i * 4u, 3) != 0;
            const bool a = displayed[i * 4u + 3u] != 0xFFu;
            if (rgb || a)
            {
                if (!diff)
                    first = static_cast<uint32_t>(i);
                ++diff;
                alphaBad += a;
            }
        }
        uint32_t upFirst = 0;
        const uint64_t upDiff = countDiff(uploaded.data(), p.rgba.data(), pixels, false, upFirst);
        if (diff == 0 && cpuOk)
            ++s.gpuOk;
        else if (diff)
        {
            s.fail("cycle " + std::to_string(cyc) + ": the GPU's displayed picture differs from the reference (alpha 255) in " + std::to_string(diff) +
                   " pixels (" + std::to_string(alphaBad) + " by alpha; first at " + std::to_string(first % p.width) + "," +
                   std::to_string(first / p.width) + ")");
            if (!s.opt.pngDir.empty())
                writePng(s.opt.pngDir + "/blit" + std::to_string(cyc) + "_gpu.png", displayed.data(), p.width, p.height);
        }
        if (upDiff == 0)
            ++s.gpuUploadOk;
        else
            s.fail("cycle " + std::to_string(cyc) + ": the uploaded texture read back differs from the CPU picture in " + std::to_string(upDiff) +
                   " pixels");
    }

    bool BlitCheck::failed() const
    {
        const Impl &s = *m;
        return s.failures != 0 || s.gpuErrors != 0 || (s.opt.gpu && !s.gpuWasUp && s.cpuChecked);
    }

    void BlitCheck::summary() const
    {
        Impl &s = *m;
        s.flushRun();
        const FrameBlit::Stats &b = s.blit.stats();
        const rc::rd::GsMemory::Stats &g = s.blit.memory().stats();
        std::printf("blit: %" PRIu64 " cycles (%" PRIu64 " without a reference); %" PRIu64 " transfers (%" PRIu64 " into frame memory, %.1f MB), %" PRIu64
                    " PATH3 vertex kicks (%" PRIu64 " with an unknown context), %" PRIu64 " VIF1 kicks; %" PRIu64 " keyframes loaded\n",
                    s.cycles, s.noReference, g.transfers, g.intoFrame, g.bytes / 1048576.0, b.path3Vertices, b.path3BlindDraws, b.vif1Kicks,
                    b.keyframesLoaded);
        std::printf("classes: upload-only %" PRIu64 " (%" PRIu64 " cycles with no drawing at all, %" PRIu64 " with drawing elsewhere; %" PRIu64
                    " showing keyframe memory only); not applicable %" PRIu64 " (a draw may have written the picture; %" PRIu64
                    " drew in the cycle itself; %" PRIu64 " equal the reference anyway), unsupported display %" PRIu64 "\n",
                    s.applicable, s.applicableQuietCycle, s.applicableDrawElsewhere, s.applicableKeyframeOnly, s.naDrawn, s.naDrawnThisCycle,
                    s.naEqualAnyway, s.naUnsupported);
        std::printf("cycles with no drawing at all: %" PRIu64 ", of them not applicable %" PRIu64 "\n", s.quietCycles, s.quietCyclesNa);
        std::printf("runs (U upload-only, K keyframe memory only, - not applicable, x unsupported):%s\n", s.runs.c_str());
        std::printf("tracked PATH3 registers against recorded register files: %" PRIu64 " of %" PRIu64 " equal\n", b.regChecks - b.regMismatches,
                    b.regChecks);
        if (b.regMismatches)
            std::printf("note: %" PRIu64 " tracked register values differ from the recorded files\n", b.regMismatches);
        std::printf("verify: DISPFB2 FBP %" PRIu64 "/%" PRIu64 "; CPU pictures %" PRIu64 "/%" PRIu64 " (%" PRIu64 " against REFI bytes, %" PRIu64
                    " against the hash)",
                    s.fbpOk, s.fbpChecked, s.cpuOk, s.cpuChecked, s.cpuByImage, s.cpuByHash);
        if (s.opt.gpu)
            std::printf("; GPU displayed %" PRIu64 "/%" PRIu64 ", GPU upload %" PRIu64 "/%" PRIu64 " on %s; %" PRIu64 " validation errors", s.gpuOk,
                        s.gpuChecked, s.gpuUploadOk, s.gpuChecked, s.adapter.empty() ? "no device" : s.adapter.c_str(), s.gpuErrors);
        std::printf("\n");
        std::printf("cost: CPU picture %.3f ms a cycle", s.cycles ? s.cpuMs / s.cycles : 0.0);
        if (s.gpuChecked)
            std::printf(", GPU upload + draw + read-back %.3f ms a picture", s.gpuMs / s.gpuChecked);
        std::printf("\n");
        for (const std::string &msg : s.messages)
            std::printf("  %s\n", msg.c_str());
        if (s.failures > s.messages.size())
            std::printf("  ... %" PRIu64 " failures in all\n", s.failures);
    }
}

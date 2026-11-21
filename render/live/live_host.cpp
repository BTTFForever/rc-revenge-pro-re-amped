#include "live_host.h"

#include "live2d.h"

#include "../frame/dlst.h"
#include "../record/capture.h"
#include "../../tools/rc_replay/png.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

namespace rc::live
{
    namespace
    {
        struct Host
        {
            Mailbox box;
            std::unique_ptr<Reader> reader;
            std::unique_ptr<Presenter> presenter;
            bool gpu = false;
            std::vector<Published> pending;
            std::string shotDir;
            std::vector<uint64_t> shotAt; // sorted
            bool shotDue = false;
            uint64_t shots = 0;
            uint64_t reportEvery = 600, reportFrames = 0; // the presenter's periodic line (RC_VK_NATIVE_REPORT, in frames with lists)
            uint64_t windowFirstCycle = 0;
        };
        Host *g_host = nullptr;

        bool on(const char *name, const char *value)
        {
            const char *v = std::getenv(name);
            return v && std::string_view(v) == value;
        }
    }

    bool installFromEnvironment()
    {
        if (g_host)
            return true;
        if (!on("RC_VK_PICTURE", "native"))
        {
            if (const char *v = std::getenv("RC_VK_PICTURE"); v && *v && std::string_view(v) != "gs")
                std::fprintf(stderr, "[vk-native] RC_VK_PICTURE=%s is not gs or native; using gs\n", v);
            return false;
        }
        g_host = new Host();
        g_host->reader = std::make_unique<Reader>(g_host->box, true);
        uint64_t every = 600;
        if (const char *v = std::getenv("RC_VK_NATIVE_REPORT"); v && *v)
            every = std::strtoull(v, nullptr, 10);
        g_host->reader->setReportEvery(every);
        g_host->reportEvery = every;
        // (step 8 batch 4) RC_FRAME_RECORD_DRAWLISTS=1: each recorded cycle's list into the dump (DLST), for the live check.
        g_host->reader->setListSink([](uint64_t cycle, const rc::rd::DrawList &list, uint32_t k2Kicks)
                                    {
                                        if (!rc::rec::drawListWanted(cycle))
                                            return;
                                        std::vector<uint8_t> payload;
                                        rc::rd::encodeDlst(list, cycle, k2Kicks, payload);
                                        rc::rec::recordDrawList(cycle, std::move(payload));
                                    });
        if (const char *dir = std::getenv("RC_VK_SHOT_DIR"); dir && *dir)
        {
            g_host->shotDir = dir;
            if (const char *at = std::getenv("RC_VK_SHOT_AT"))
                for (const char *p = at; *p;)
                {
                    char *end = nullptr;
                    const uint64_t c = std::strtoull(p, &end, 10);
                    if (end == p)
                        break;
                    g_host->shotAt.push_back(c);
                    p = *end ? end + 1 : end;
                }
            std::sort(g_host->shotAt.begin(), g_host->shotAt.end());
        }
        rc::rec::addLiveConsumer(g_host->reader.get());
        std::fprintf(stderr, "[vk-native] RC_VK_PICTURE=native: the frame reader runs on the EE thread; the native picture (K2 and K1) is "
                             "presented (F7 toggles the emulated GS picture)\n");
        return true;
    }

    bool nativeEnabled() { return g_host != nullptr; }

    bool createGpu(std::string *why)
    {
        if (!g_host)
            return false;
        Presenter::Options o;
        if (on("RC_VK_BLEND", "ff"))
            o.blend = rc::rd::Gs2dVk::kBlendFixed;
        else if (on("RC_VK_BLEND", "read"))
            o.blend = rc::rd::Gs2dVk::kBlendRead;
        g_host->presenter = std::make_unique<Presenter>();
        if (!g_host->presenter->create(o, why))
        {
            g_host->presenter.reset();
            return false;
        }
        g_host->gpu = true;
        rc::rd::Gs2dVk &vk = g_host->presenter->gs2d();
        std::fprintf(stderr, "[vk-native] Gs2dVk ready: blend %s (local read %s)\n",
                     vk.readMode() ? "by local read (exact)" : "fixed function with the source bias (D-2D-1)", vk.readAvailable() ? "yes" : "no");
        return true;
    }

    void destroyGpu()
    {
        if (g_host && g_host->presenter)
        {
            g_host->presenter->destroy();
            g_host->presenter.reset();
        }
        if (g_host)
            g_host->gpu = false;
    }

    HostView recordFrame(RhiCommandList cl)
    {
        HostView out;
        if (!g_host || !g_host->gpu)
            return out;
        std::vector<Published> &lists = g_host->pending;
        lists.clear();
        g_host->box.take(lists);
        const Presenter::View v = g_host->presenter->record(cl, lists);
        if (!g_host->shotDir.empty() && !g_host->shotAt.empty() && v.valid && v.cycle >= g_host->shotAt.front() && !lists.empty())
            g_host->shotDue = true;
        if (!lists.empty() && g_host->reportEvery && ++g_host->reportFrames >= g_host->reportEvery)
        {
            // (step 8 batch 4) The presenter's window: GPU time of the frames' lists and K2's (U3).
            const Presenter::Stats w = g_host->presenter->takeWindow();
            std::fprintf(stderr,
                         "[vk-native] presenter cycles %llu-%llu: %llu frames with lists, %llu lists; GPU %.3f ms a frame (largest %.3f); K2 %.3f ms "
                         "a VIF1 kick (largest %.3f, %llu timed, %llu not), %.3f ms a frame (largest %.3f); %.1f barriers, %.0f draws a frame\n",
                         static_cast<unsigned long long>(g_host->windowFirstCycle), static_cast<unsigned long long>(v.cycle),
                         static_cast<unsigned long long>(w.framesWithLists), static_cast<unsigned long long>(w.lists),
                         w.gpuSamples ? w.gpuMs / w.gpuSamples : 0.0, w.maxGpuMs, w.k2KicksTimed ? w.k2Ms / w.k2KicksTimed : 0.0,
                         w.maxK2KickMs, static_cast<unsigned long long>(w.k2KicksTimed), static_cast<unsigned long long>(w.k2KicksUntimed),
                         w.k2FramesTimed ? w.k2FrameMs / w.k2FramesTimed : 0.0, w.maxK2FrameMs,
                         w.framesWithLists ? static_cast<double>(w.barriers) / w.framesWithLists : 0.0,
                         w.framesWithLists ? static_cast<double>(w.draws) / w.framesWithLists : 0.0);
            g_host->reportFrames = 0;
            g_host->windowFirstCycle = v.cycle + 1u;
        }
        out.texture = v.texture;
        out.width = v.width;
        out.height = v.height;
        out.texHeight = v.texHeight;
        out.valid = v.valid;
        return out;
    }

    void afterPresent(const uint8_t *gsPixels, uint32_t gsWidth, uint32_t gsHeight)
    {
        if (!g_host || !g_host->gpu || !g_host->shotDue)
            return;
        g_host->shotDue = false;
        Presenter &p = *g_host->presenter;
        const Presenter::View &v = p.view();
        while (!g_host->shotAt.empty() && g_host->shotAt.front() <= v.cycle)
            g_host->shotAt.erase(g_host->shotAt.begin());
        std::vector<uint8_t> rgba;
        if (!p.readShown(rgba))
        {
            std::fprintf(stderr, "[vk-native] screenshot of cycle %llu: read-back failed\n", static_cast<unsigned long long>(v.cycle));
            return;
        }
        const std::string base = g_host->shotDir + "/native_" + std::to_string(v.cycle);
        bool ok = rc::replay::writePng(base + ".png", rgba.data(), v.width, v.height);
        if (std::FILE *f = std::fopen((base + ".rgba").c_str(), "wb"))
        {
            ok = std::fwrite(rgba.data(), 1, rgba.size(), f) == rgba.size() && ok;
            std::fclose(f);
        }
        else
            ok = false;
        ++g_host->shots;
        std::fprintf(stderr, "[vk-native] screenshot: cycle %llu (%ux%u) -> %s.png%s\n", static_cast<unsigned long long>(v.cycle), v.width,
                     v.height, base.c_str(), ok ? "" : " (write failed)");
        // (step 8 batch 4) The emulated GS picture handed to the same present, beside it (gs_<cycle>.png / .rgba, its own size).
        if (gsPixels && gsWidth && gsHeight)
        {
            const std::string gs = g_host->shotDir + "/gs_" + std::to_string(v.cycle);
            bool gok = rc::replay::writePng(gs + ".png", gsPixels, gsWidth, gsHeight);
            if (std::FILE *f = std::fopen((gs + ".rgba").c_str(), "wb"))
            {
                const size_t n = static_cast<size_t>(gsWidth) * gsHeight * 4u;
                gok = std::fwrite(gsPixels, 1, n, f) == n && gok;
                std::fclose(f);
            }
            std::fprintf(stderr, "[vk-native] screenshot: emulated GS picture of the same present (%ux%u) -> %s.png%s\n", gsWidth, gsHeight,
                         gs.c_str(), gok ? "" : " (write failed)");
        }
    }

    void report()
    {
        if (!g_host)
            return;
        const Reader::Stats &r = g_host->reader->stats();
        std::fprintf(stderr,
                     "[vk-native] reader: %llu cycles, %.2f ms a cycle on the EE thread (largest %.2f); %llu VIF1 kicks, %.2f ms each (largest %.2f); "
                     "%llu PATH3 kicks; %llu guarded VIF1 kicks; %llu frame uploads not drawn (not CT32 / FBW 10); %llu lists published, "
                     "%llu reduced to textures and uploads\n",
                     static_cast<unsigned long long>(r.cycles), r.cycles ? r.ns / r.cycles / 1e6 : 0.0, r.maxCycleNs / 1e6,
                     static_cast<unsigned long long>(r.path1Kicks), r.path1Kicks ? r.path1Ns / r.path1Kicks / 1e6 : 0.0, r.maxPath1Ns / 1e6,
                     static_cast<unsigned long long>(r.path3Kicks), static_cast<unsigned long long>(r.guarded),
                     static_cast<unsigned long long>(g_host->reader->frame().gif().stats().frameUploadsOther),
                     static_cast<unsigned long long>(g_host->box.pushed()), static_cast<unsigned long long>(g_host->box.stripped()));
        if (g_host->presenter)
        {
            const Presenter::Stats &s = g_host->presenter->stats();
            const rc::rd::Gs2dVk::Stats &g = g_host->presenter->gs2d().stats();
            std::fprintf(stderr,
                         "[vk-native] presenter: %llu frames, %llu with lists, %llu lists drawn (%llu failed), %llu "
                         "other FBP; GPU %.3f ms a frame with lists on average (largest %.3f, %llu samples); K2 %.3f ms a VIF1 kick (largest %.3f, "
                         "%llu timed), %.3f ms a frame (largest %.3f); %llu draws, %llu reading primitives, "
                         "%llu barriers; Gs2dVk: %llu primitives, %llu clears, %llu textures, %llu unsupported states; %llu screenshots\n",
                         static_cast<unsigned long long>(s.frames), static_cast<unsigned long long>(s.framesWithLists),
                         static_cast<unsigned long long>(s.lists), static_cast<unsigned long long>(s.failures),
                         static_cast<unsigned long long>(s.otherFbp),
                         s.gpuSamples ? s.gpuMs / s.gpuSamples : 0.0, s.maxGpuMs, static_cast<unsigned long long>(s.gpuSamples),
                         s.k2KicksTimed ? s.k2Ms / s.k2KicksTimed : 0.0, s.maxK2KickMs, static_cast<unsigned long long>(s.k2KicksTimed),
                         s.k2FramesTimed ? s.k2FrameMs / s.k2FramesTimed : 0.0, s.maxK2FrameMs,
                         static_cast<unsigned long long>(s.draws), static_cast<unsigned long long>(s.readPrims),
                         static_cast<unsigned long long>(s.barriers), static_cast<unsigned long long>(g.prims),
                         static_cast<unsigned long long>(g.clears), static_cast<unsigned long long>(g.textures),
                         static_cast<unsigned long long>(g.unsupported), static_cast<unsigned long long>(g_host->shots));
        }
    }
}

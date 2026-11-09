// RC: draws through ps2-port-engine's Vulkan renderer when PS2X_GS_VK=1. Our GS frontend still parses the GIF stream
// for its own state, and this backend ignores what it submits; the engine gets the raw packets through GS::s_rawGifSink.
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ps2host/gs.h"
#include "ps2host/run.h"
#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_threaded_backend.h"

namespace ps2host
{
    extern std::function<void(u64)> g_rc_frame_handler;
    int RcDisplayedFrame(std::vector<u32> *pixels, u32 *width, u32 *height);
}

namespace
{
    std::mutex g_engine;
    bool g_started = false;

    bool ensureStarted()
    {
        if (g_started)
            return true;
        std::string error;
        if (!ps2host::StartGpuRenderer(&error, ps2host::GpuMode::Primary, false))
        {
            std::cerr << "[gs-vk] " << error << std::endl;
            return false;
        }
        g_started = true;
        return true;
    }

    void feed(const uint8_t *data, uint32_t sizeBytes)
    {
        std::lock_guard<std::mutex> lock(g_engine);
        if (ensureStarted())
            ps2host::gs::TheGs().Words(3, reinterpret_cast<const uint32_t *>(data), sizeBytes / 4u);
    }

    class VulkanBackend final : public GSRasterBackend
    {
        std::vector<uint8_t> m_last;
        uint32_t m_lastWidth = 0u, m_lastHeight = 0u;

    public:
        void Initialize(uint8_t *, uint32_t) override {}
        void Reset() override {}
        void Submit(const GSPrimitiveBatch &) override {}
        void LoadClut(const GSTex0Reg &, const GSTexClutReg &) override {}
        void BeginTransfer(const GSTransferCommand &) override {}
        void UploadImage(const uint8_t *, uint32_t) override {}
        void Flush() override {}
        void TextureFlush() override {}
        void Sync(GSSyncReason) override {}
        bool ClearFramebuffer(const GSContext &, uint32_t) override { return false; }
        uint32_t ConsumeLocalToHostBytes(uint8_t *, uint32_t) override { return 0u; }
        uint32_t ReadVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) const override { return 0u; }
        void WriteVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
        void SnapshotVram(std::vector<uint8_t> &) const override {}
        GSTransferSnapshot GetTransferSnapshot() const override { return {}; }

        PresentationFrame Present(const GSPresentationRequest &r) override
        {
            PresentationFrame frame;
            std::lock_guard<std::mutex> lock(g_engine);
            if (!ensureStarted())
                return frame;
            auto &g = ps2host::gs::TheGs();
            g.SetPrivileged(0x0000u, r.pmode);
            g.SetPrivileged(0x0020u, r.smode2);
            g.SetPrivileged(0x0070u, r.dispfb1);
            g.SetPrivileged(0x0080u, r.display1);
            g.SetPrivileged(0x0090u, r.dispfb2);
            g.SetPrivileged(0x00A0u, r.display2);
            g.SetPrivileged(0x00E0u, r.bgcolor);
            static uint64_t counter = 0;
            using Clock = std::chrono::steady_clock;
            const auto t0 = Clock::now();
            if (ps2host::g_rc_frame_handler)
                ps2host::g_rc_frame_handler(counter++);
            const auto t1 = Clock::now();
            std::vector<uint32_t> pixels;
            uint32_t width = 0, height = 0;
            const int state = ps2host::RcDisplayedFrame(&pixels, &width, &height);
            const bool ok = state == 1 && width != 0u && height != 0u;
            const auto t2 = Clock::now();
            static uint64_t good = 0;
            good += ok ? 1u : 0u;
            uint32_t fbp = 0, fbw = 0, psm = 0;
            const bool shown = g.Displayed(&fbp, &fbw, &psm);
            if (counter % 30u == 1u)
                std::cerr << "[gs-vk] present " << counter << " ok=" << good << " handler_ms=" << std::chrono::duration<double, std::milli>(t1 - t0).count()
                          << " read_ms=" << std::chrono::duration<double, std::milli>(t2 - t1).count() << " " << width << "x" << height << " displayed=" << shown << " fbp=" << fbp
                          << " fbw=" << fbw << " psm=" << psm << " prims=" << g.primitives() << " uploads=" << g.uploads() << std::endl;
            if (state == 2)
            {
                // The read-back has not finished: show the previous picture again.
                if (m_last.empty())
                    return frame;
                frame.pixels = m_last;
                frame.width = m_lastWidth;
                frame.height = m_lastHeight;
                return frame;
            }
            if (!ok)
            {
                // Nothing drawn by the GPU yet (videos are written straight to memory): read local memory.
                if (!g.Picture(&pixels, &width, &height) || width == 0u || height == 0u)
                    return frame;
                for (uint32_t &pixel : pixels)
                    pixel = (pixel & 0xFF00FF00u) | ((pixel & 0xFFu) << 16) | ((pixel >> 16) & 0xFFu);
            }
            for (uint32_t &pixel : pixels)
                pixel |= 0xFF000000u;
            frame.pixels.resize(pixels.size() * 4u);
            std::memcpy(frame.pixels.data(), pixels.data(), frame.pixels.size());
            frame.width = width;
            frame.height = height;
            m_last = frame.pixels;
            m_lastWidth = width;
            m_lastHeight = height;
            return frame;
        }
    };

    const bool g_registered = []
    {
        const char *enabled = std::getenv("PS2X_GS_VK");
        if (enabled && *enabled == '1')
        {
            GSThreadedBackend::s_backendFactory = []() -> std::unique_ptr<GSRasterBackend> { return std::make_unique<VulkanBackend>(); };
            GS::s_rawGifSink = &feed;
        }
        return true;
    }();
}

// RC: what the renderer needs from ps2-port-engine's host, without its window or settings.
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>

#include "ps2gpu/gs_renderer.h"
#include "ps2host/dma.h"
#include "ps2host/gs.h"
#include "ps2host/profile.h"
#include "ps2host/run.h"
#include "ps2host/settings.h"
#include "ps2host/texture_pack.h"
#include "ps2host/timeline.h"

namespace ps2gpu {
std::unique_ptr<Gpu> CreateD3D11Gpu(std::string *) { return nullptr; }
std::unique_ptr<GsRenderer> CreateD3D11GsRenderer(Gpu &, std::string *) { return nullptr; }
}

namespace ps2host {
std::uint32_t g_dma_source = 0;
std::function<void(u64)> g_rc_frame_handler;

u64 BlankPeriodNs() { return 16683333u; }
u64 FrameDue(u64) { return 0; }
u64 FrameEndedNs(u64) { return 0; }
bool FramePacingDefault() { return false; }
void NoteRendererThread() {}
bool SampleRendererThread() { return false; }
void SetGsFrameHandler(std::function<void(u64)> handler) { g_rc_frame_handler = std::move(handler); }
void SetPictureSource(std::function<bool(std::vector<u32> *, u32 *, u32 *)>) {}
u32 SupersamplingFactor() { return 1; }
bool VirtualClock() { return false; }
void WaitForGpuPipelines() {}
ps2gpu::Display *WindowDisplay() { return nullptr; }
ps2gpu::Gpu *WindowGpu() { return nullptr; }
bool WindowPixelsOnly() { return false; }

const std::string &SettingsPath()
{
    static const std::string empty;
    return empty;
}
std::string Setting(const std::string &key, const std::string &fallback)
{
    if (key == "resolution")
        if (const char *scale = std::getenv("PS2X_GS_VK_SCALE"))
            return std::string(scale) + "x";
    return fallback;
}
bool SettingOn(const std::string &, bool fallback) { return fallback; }

Timed::Timed(Section) {}
Timed::~Timed() {}

namespace timeline {
bool On() { return false; }
u64 Now() { return 0; }
void Span(const char *, u64, u64) {}
}

namespace texture_pack {
bool Active() { return false; }
std::size_t Count() { return 0; }
bool Enabled() { return false; }
std::shared_ptr<const Image> Find(const Name &, bool *pending)
{
    if (pending)
        *pending = false;
    return nullptr;
}
std::uint64_t Generation() { return 0; }
std::string Name::Text() const { return std::string(); }
Name NameOf(const gs::Memory &, std::uint64_t, std::uint64_t, std::uint64_t, const std::uint32_t *) { return Name{}; }
std::size_t Open(const std::string &) { return 0; }
void SetBlockCompression(bool) {}
void SetEnabled(bool) {}
bool Wanted(std::uint32_t) { return false; }
}
}

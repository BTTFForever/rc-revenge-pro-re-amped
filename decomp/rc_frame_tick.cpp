// Enhancement: logic on video-rate ticks while pictures are drawn faster (rc_frame_tick.h).
#include "rc_frame_tick.h"

#include "rc_decomp.h"
#include "rc_settings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace rc::frametick {

namespace {

using Clock = std::chrono::steady_clock;

Owner g_owner = Owner::None;
std::atomic<uint32_t> g_normalPeriod{0}; // the video mode's vblank period, while the vblank runs faster

// the tick clock
bool g_started = false, g_logic = true;
uint64_t g_lastTickUs = 0, g_nextTickUs = 0; // emulated time (vblanks x their period)
float g_blend = 1.0f;
uint64_t g_ticks = 0, g_passes = 0;

template <typename T>
T &at(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

uint32_t currentPeriod() { return static_cast<uint32_t>(g_runtime->eeScheduler().vblankPeriod().count()); }

bool logEnabled()
{
    static const bool on = [] { const char *v = std::getenv("RC_TICK_LOG"); return v && *v && *v != '0'; }();
    return on;
}

} // namespace

uint32_t videoPeriodUs()
{
    const uint32_t normal = g_normalPeriod.load(std::memory_order_relaxed);
    return normal != 0u ? normal : currentPeriod();
}

uint32_t raceHz()
{
    const uint32_t hz = static_cast<uint32_t>(settings::raceRefreshHz()); // Game Options > Display
    if (hz == 0u || 1000000u / hz >= videoPeriodUs())                    // not faster than the video mode
        return 0u;
    return hz;
}

Owner owner() { return g_owner; }
bool fast() { return g_normalPeriod.load(std::memory_order_relaxed) != 0u; }

void setFast(Owner owner, bool want)
{
    EeScheduler &scheduler = g_runtime->eeScheduler();
    const uint32_t hz = want ? raceHz() : 0u;
    const uint32_t normal = videoPeriodUs();
    if (hz == 0u)
    {
        if (fast())
        {
            scheduler.setVBlankPeriodMicroseconds(normal);
            g_normalPeriod.store(0u, std::memory_order_relaxed);
            g_logic = true;
        }
        g_owner = want ? owner : Owner::None;
        return;
    }
    g_owner = owner;
    const uint32_t period = 1000000u / hz;
    if (fast() && currentPeriod() == period)
        return;
    g_normalPeriod.store(normal, std::memory_order_relaxed);
    scheduler.setVBlankPeriodMicroseconds(period);
    resync();
    if (logEnabled())
        std::fprintf(stderr, "[tick] %s: vblank %u us -> %u us, logic every %u us\n", owner == Owner::Race ? "race" : "front end", normal,
                     period, normal);
}

void reapply()
{
    if (g_owner == Owner::Race)
        setFast(Owner::Race, true);
    // the front end re-evaluates its rate at every pass
}

void resync() { g_started = false; }

// The clock is the emulated vblank's: each pass starts at a vblank and its picture is shown at the next, so the
// vblanks counted (times their period) are exactly the times the pictures stand for. The vblank is paced to the host
// clock, so ticks keep real time.
bool beginPass()
{
    ++g_passes;
    EeScheduler &scheduler = g_runtime->eeScheduler();
    const uint64_t vsync = scheduler.currentVSyncTick();
    static uint64_t lastVsync = 0;
    static uint64_t emulatedUs = 0;
    emulatedUs += (vsync - lastVsync) * currentPeriod();
    lastVsync = vsync;
    if (!fast() || (g_owner == Owner::Race && !raceGating()))
    {
        g_logic = true;
        g_blend = 1.0f;
        g_started = false;
        ++g_ticks;
        return true;
    }
    const uint64_t period = g_normalPeriod.load(std::memory_order_relaxed);
    if (!g_started || emulatedUs >= g_nextTickUs)
    {
        if (!g_started || emulatedUs - g_nextTickUs > period) // the first pass, or passes stopped for longer than a tick
        {
            g_lastTickUs = emulatedUs;
            g_nextTickUs = emulatedUs + period;
        }
        else
        {
            g_lastTickUs = g_nextTickUs;
            g_nextTickUs += period;
        }
        g_started = true;
        g_logic = true;
        ++g_ticks;
    }
    else
        g_logic = false;
    g_blend = std::clamp(static_cast<float>(emulatedUs - g_lastTickUs) / static_cast<float>(period), 0.0f, 1.0f);
    return g_logic;
}

bool logicPass() { return g_logic; }
float blend() { return g_blend; }
uint64_t ticks() { return g_ticks; }
uint64_t passes() { return g_passes; }

bool raceGating()
{
    static const bool on = [] { const char *v = std::getenv("RC_FRAME_TICK"); return !(v && *v == '0'); }();
    return on;
}

bool videoFieldElapsed()
{
    static uint64_t elapsed = 0;
    const uint32_t normal = g_normalPeriod.load(std::memory_order_relaxed);
    if (normal == 0u || (g_owner == Owner::Race && !raceGating()))
    {
        elapsed = 0;
        return true;
    }
    elapsed += currentPeriod();
    if (elapsed < normal)
        return false;
    elapsed -= normal;
    if (elapsed >= normal)
        elapsed = 0;
    return true;
}

namespace {
uint64_t g_rainSteps = 0, g_glintSteps = 0;
}
void countRainStep() { ++g_rainSteps; }
void countGlintStep() { ++g_glintSteps; }

void logSecond()
{
    if (!logEnabled())
        return;
    static auto last = Clock::now();
    static uint64_t passes0 = 0, ticks0 = 0;
    static uint32_t tex0 = 0, vsync0 = 0, sec = 0;
    static int32_t fade0 = 0, vol0 = 0, user0 = 0;
    static uint64_t glint0 = 0, rain0 = 0;
    const uint32_t tex = at<uint32_t>(sym::framecount);
    const uint32_t vsync = at<uint32_t>(sym::displayVarG + 0x64u); // uiVSyncInc
    const int32_t fade = at<int32_t>(sym::rcmusicVarG + 0x14u), vol = at<int32_t>(sym::rcmusicVarG + 0x18u), user = at<int32_t>(sym::rcmusicVarG + 0x20u);
    const auto now = Clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now.time_since_epoch()).count();
    // music fade start/end and volume changes, as they happen (the fade's length, a held slider's repeat)
    if ((fade != 0) != (fade0 != 0))
        std::fprintf(stderr, "[tick-ev] %.1f ms: music fade %s (fade %d, vol %d)\n", ms, fade != 0 ? "starts" : "ends", fade, vol);
    if (user != user0)
        std::fprintf(stderr, "[tick-ev] %.1f ms: music volume setting %d -> %d\n", ms, user0, user);
    fade0 = fade;
    vol0 = vol;
    user0 = user;
    if (now - last < std::chrono::seconds(1))
        return;
    const double s = std::chrono::duration<double>(now - last).count();
    const double glintSteps = (g_glintSteps - glint0) / s, rainSteps = (g_rainSteps - rain0) / s;
    glint0 = g_glintSteps;
    rain0 = g_rainSteps;
    std::fprintf(stderr, "[tick] %u: %.1f passes/s, %.1f ticks/s, animated-texture frames %.1f/s, vblank counter %.1f/s, glint steps %.1f/s, rain "
                 "steps %.1f/s, owner %d fast %d\n", ++sec, (g_passes - passes0) / s, (g_ticks - ticks0) / s, (tex - tex0) / s, (vsync - vsync0) / s,
                 glintSteps, rainSteps, static_cast<int>(g_owner), fast() ? 1 : 0);
    last = now;
    passes0 = g_passes;
    ticks0 = g_ticks;
    tex0 = tex;
    vsync0 = vsync;
}

} // namespace rc::frametick

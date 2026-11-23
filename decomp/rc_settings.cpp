// The enhanced recomp's settings (rc_settings.h).
#include "rc_settings.h"

#include "runtime/ps2_display_options.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace rc::settings {

namespace {

constexpr float kAspects[AspectCount] = {4.0f / 3.0f, 16.0f / 9.0f, 16.0f / 10.0f, 21.0f / 9.0f};
constexpr const char *kAspectNames[AspectCount] = {"4:3", "16:9", "16:10", "21:9"};

std::mutex g_mutex;
Settings g_settings;
std::string g_path;

bool envSet(const char *name, const char *&value)
{
    value = std::getenv(name);
    return value && *value;
}

int aspectFromText(const std::string &text)
{
    for (int i = 0; i < AspectCount; ++i)
        if (text == kAspectNames[i])
            return i;
    float w = 0, h = 0;
    if (std::sscanf(text.c_str(), "%f:%f", &w, &h) == 2 && h > 0)
    {
        const float r = w / h;
        int best = 0;
        for (int i = 1; i < AspectCount; ++i)
            if (std::abs(kAspects[i] - r) < std::abs(kAspects[best] - r))
                best = i;
        return best;
    }
    return Aspect4x3;
}

int fpsIndex(int hz)
{
    int best = 0;
    for (int i = 1; i < kRaceFpsCount; ++i)
        if (std::abs(kRaceFps[i] - hz) < std::abs(kRaceFps[best] - hz))
            best = i;
    return best;
}

constexpr const char *kVsyncNames[] = {"off", "on", "adaptive"};

int vsyncFromText(const std::string &text)
{
    if (text == "adaptive" || text == "-1")
        return VsyncAdaptive;
    if (text == "on" || text == "1")
        return VsyncOn;
    return VsyncOff;
}

int swapInterval(int vsync) { return vsync == VsyncAdaptive ? -1 : vsync == VsyncOn ? 1 : 0; }

std::string defaultPath()
{
    std::string base;
    if (const char *x = std::getenv("XDG_CONFIG_HOME"); x && *x)
        base = x;
    else if (const char *home = std::getenv("HOME"); home && *home)
        base = std::string(home) + "/.config";
    else
        base = ".";
    return base + "/rc-revenge-pro/settings.ini";
}

void readFile(Settings &s)
{
    std::ifstream in(g_path);
    std::string line;
    while (std::getline(in, line))
    {
        const auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos)
            continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        const int number = std::atoi(value.c_str());
        if (key == "aspect")
            s.aspect = aspectFromText(value);
        else if (key == "hud")
            s.hudCorrected = value != "stretched";
        else if (key == "fullscreen")
            s.fullscreen = number != 0;
        else if (key == "scale")
            s.scale = std::clamp(number, 1, 6);
        else if (key == "race_fps")
            s.raceFps = fpsIndex(number);
        else if (key == "smooth_motion")
            s.smoothMotion = number != 0;
        else if (key == "vsync")
            s.vsync = vsyncFromText(value);
        else if (key == "vrr")
            s.vrr = number != 0;
    }
}

void writeFile(const Settings &s)
{
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(g_path).parent_path(), ec);
    std::ofstream out(g_path);
    if (!out)
    {
        std::fprintf(stderr, "[settings] cannot write %s\n", g_path.c_str());
        return;
    }
    out << "# RC Revenge Pro enhanced recomp settings (also in Game Options > Enhancements)\n"
        << "aspect=" << kAspectNames[s.aspect] << "\n"
        << "hud=" << (s.hudCorrected ? "corrected" : "stretched") << "\n"
        << "fullscreen=" << (s.fullscreen ? 1 : 0) << "\n"
        << "scale=" << s.scale << "\n"
        << "race_fps=" << kRaceFps[s.raceFps] << "\n"
        << "smooth_motion=" << (s.smoothMotion ? 1 : 0) << "\n"
        << "vsync=" << kVsyncNames[s.vsync] << "\n"
        << "vrr=" << (s.vrr ? 1 : 0) << "\n";
}

// The presentation follows the aspect; the window loop takes requests from the game thread.
void applyLive(const Settings &before, const Settings &s, bool first)
{
    if (first || before.aspect != s.aspect)
        ps2_host_display::setAspect(kAspects[s.aspect]);
    if (!first && before.fullscreen != s.fullscreen)
        ps2_host_display::requestFullscreen(s.fullscreen);
    if (first || before.vsync != s.vsync)
        ps2_host_display::requestSwapInterval(swapInterval(s.vsync));
    if (first || before.vrr != s.vrr)
        ps2_host_display::setVariableRefresh(s.vrr);
}

// Before the runtime starts: the file, then the environment, then what the runtime reads from the environment.
__attribute__((constructor(101))) void load()
{
    g_path = defaultPath();
    Settings s;
    readFile(s);
    const char *v = nullptr;
    if (envSet("RC_ASPECT", v))
        s.aspect = aspectFromText(v);
    else if (envSet("RC_WIDESCREEN", v))
        s.aspect = *v == '0' ? Aspect4x3 : Aspect16x9;
    if (envSet("RC_HUD_ANCHOR", v))
        s.hudCorrected = *v != '0';
    if (envSet("PS2X_FULLSCREEN", v))
        s.fullscreen = *v != '0';
    if (envSet("PS2X_GS_HW_SCALE", v))
        s.scale = std::clamp(std::atoi(v), 1, 6);
    if (envSet("PS2X_REFRESH_HZ", v))
        s.raceFps = fpsIndex(std::atoi(v));
    if (envSet("RC_INTERP", v))
        s.smoothMotion = *v != '0';
    if (envSet("PS2X_VSYNC", v))
        s.vsync = vsyncFromText(v);
    if (envSet("PS2X_VRR", v))
        s.vrr = *v != '0';
    // NVIDIA's driver allows G-SYNC/VRR for the window only when told so before the GL context is made (it matters
    // when the NVIDIA GPU drives the screen; with PRIME offload the compositor's GPU does). Set either way: harmless.
    setenv("__GL_GSYNC_ALLOWED", "1", 0);
    setenv("__GL_VRR_ALLOWED", "1", 0);
    g_settings = s;
    // The runtime reads these itself.
    setenv("PS2X_GS_HW_SCALE", std::to_string(s.scale).c_str(), 1);
    if (s.fullscreen)
        setenv("PS2X_FULLSCREEN", "1", 1);
    if (kRaceFps[s.raceFps] > 60)
        setenv("PS2X_REFRESH_HZ", std::to_string(kRaceFps[s.raceFps]).c_str(), 1);
    if (s.aspect != Aspect4x3)
        ps2_host_display::setInitialWindowSize(static_cast<int>(720 * kAspects[s.aspect] + 0.5f), 720);
    applyLive(s, s, true);
    std::fprintf(stderr, "[settings] %s: aspect %s, hud %s, fullscreen %d, scale %d, race fps %d, smooth motion %d, vsync %s, vrr %d\n", g_path.c_str(),
                 kAspectNames[s.aspect], s.hudCorrected ? "corrected" : "stretched", s.fullscreen, s.scale, kRaceFps[s.raceFps], s.smoothMotion,
                 kVsyncNames[s.vsync], s.vrr);
}

} // namespace

const Settings &get() { return g_settings; }

void set(const Settings &s)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const Settings before = g_settings;
    g_settings = s;
    applyLive(before, s, false);
    if (kRaceFps[s.raceFps] != kRaceFps[before.raceFps])
        ps2_host_display::requestTargetFps(std::max(60, kRaceFps[s.raceFps]));
    if (s.vrr != before.vrr)
        std::fprintf(stderr, "[settings] variable refresh %s\n", s.vrr ? "on" : "off");
    writeFile(s);
}

float aspectRatio() { return kAspects[get().aspect]; }
float hscale() { return (4.0f / 3.0f) / aspectRatio(); }
int raceRefreshHz()
{
    int hz = kRaceFps[get().raceFps];
    // VRR: kept a few frames below the screen's top rate, so the screen follows each picture instead of waiting a
    // whole refresh (the usual cap: 141 on a 144 Hz screen).
    const int screen = ps2_host_display::monitorRefreshHz();
    if (get().vrr && screen > 60 && hz > screen - 3)
        hz = screen - 3;
    return hz;
}
const char *filePath() { return g_path.c_str(); }

} // namespace rc::settings

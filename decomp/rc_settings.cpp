// The enhanced recomp's settings (rc_settings.h).
#include "rc_settings.h"

#include "runtime/ps2_display_options.h"
#include "runtime/ps2_input_bindings.h"

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

constexpr const char *kPromptNames[PromptsCount] = {"ps2", "match", "off"};

constexpr const char *kSplitNames[SplitCount] = {"horizontal", "vertical"};

int splitFromText(const std::string &text)
{
    return text == "vertical" || text == "v" || text == "1" ? SplitVertical : SplitHorizontal;
}

int carsFromNumber(int n) { return n <= 0 ? 0 : std::clamp(n, 2, 8); }

int promptsFromText(const std::string &text)
{
    for (int i = 0; i < PromptsCount; ++i)
        if (text == kPromptNames[i])
            return i;
    return PromptsPs2;
}

// A percentage to the nearest scale step.
int uiScaleIndex(int percent)
{
    int best = 0;
    for (int i = 1; i < kUiScaleCount; ++i)
        if (std::abs(kUiScales[i] - percent) < std::abs(kUiScales[best] - percent))
            best = i;
    return best;
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

namespace B = ps2_input_bindings;

// The file's controls_version (0: none, a file from before the Auto device). Version 2 files keep the devices as saved;
// in older ones the old defaults (player 1 keyboard+pad1, player 2 pad2), which were always written whether chosen or
// not, become Auto.
int g_controlsVersion = 0;
bool g_sawDevice[2] = {false, false};

// A controls line: p1_device=..., p1_key_<control>=..., p1_pad_<control>=... (players 1 and 2).
bool readControl(const std::string &key, const std::string &value, B::Player players[2])
{
    if (key.size() < 4 || key[0] != 'p' || (key[1] != '1' && key[1] != '2') || key[2] != '_')
        return false;
    B::Player &p = players[key[1] - '1'];
    const std::string rest = key.substr(3);
    if (rest == "device")
    {
        if (const int d = B::parseDevice(value.c_str()); d >= 0)
        {
            p.device = d;
            g_sawDevice[key[1] - '1'] = true;
        }
        else
            std::fprintf(stderr, "[settings] %s=%s: unknown device, Auto\n", key.c_str(), value.c_str());
        return true;
    }
    const bool isKey = rest.rfind("key_", 0) == 0, isPad = rest.rfind("pad_", 0) == 0;
    if (!isKey && !isPad)
        return false;
    const std::string control = rest.substr(4);
    for (int c = 0; c < B::ControlCount; ++c)
        if (control == B::controlKey(c))
        {
            const int source = isKey ? B::parseKeySource(value.c_str()) : B::parsePadSource(value.c_str());
            if (source >= 0)
                (isKey ? p.key : p.pad)[c] = source;
            else
                std::fprintf(stderr, "[settings] %s=%s: unknown, kept the default\n", key.c_str(), value.c_str());
            return true;
        }
    return false;
}

B::Player *g_loadPlayers = nullptr; // during load() only (it runs before this file's dynamic initialisers)

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
        else if (key == "hud_scale")
            s.hudScale = uiScaleIndex(number);
        else if (key == "menu_scale")
            s.menuScale = uiScaleIndex(number);
        else if (key == "button_prompts")
            s.prompts = promptsFromText(value);
        else if (key == "split")
            s.split = splitFromText(value);
        else if (key == "cars_single")
            s.carsSingle = carsFromNumber(number);
        else if (key == "cars_multi")
            s.carsMulti = carsFromNumber(number);
        else if (key == "controls_version")
            g_controlsVersion = number;
        else if (g_loadPlayers)
            readControl(key, value, g_loadPlayers);
    }
}

void writeControls(std::ofstream &out)
{
    out << "# Controls (also in Game Options > Controls): per player the device (auto, keyboard, pad1-pad4, keyboard+pad1,\n"
        << "# keyboard+pad2, none; auto: player 1 the keyboard and every gamepad player 2 does not use, player 2 the next\n"
        << "# gamepad; a chosen gamepad that is not connected counts as auto until it is), then each DualShock 2 control's key\n"
        << "# (raylib names: A, F1, space, left_shift, ...) and gamepad input (SDL names: a, b, x, y, dpup, leftshoulder,\n"
        << "# start, back, +righttrigger, -leftx, ...); none unbinds\n"
        << "controls_version=2\n";
    for (int i = 0; i < 2; ++i)
    {
        const B::Player p = B::player(i);
        out << "p" << i + 1 << "_device=" << B::deviceKey(p.device) << "\n";
        for (int c = 0; c < B::ControlCount; ++c)
            out << "p" << i + 1 << "_key_" << B::controlKey(c) << "=" << B::sourceKey(p.key[c]) << "\n";
        for (int c = 0; c < B::ControlCount; ++c)
            out << "p" << i + 1 << "_pad_" << B::controlKey(c) << "=" << B::sourceKey(p.pad[c]) << "\n";
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
    out << "# RC Revenge Pro enhanced recomp settings (also in Game Options > Display and Controls)\n"
        << "aspect=" << kAspectNames[s.aspect] << "\n"
        << "hud=" << (s.hudCorrected ? "corrected" : "stretched") << "\n"
        << "fullscreen=" << (s.fullscreen ? 1 : 0) << "\n"
        << "scale=" << s.scale << "\n"
        << "race_fps=" << kRaceFps[s.raceFps] << "\n"
        << "smooth_motion=" << (s.smoothMotion ? 1 : 0) << "\n"
        << "vsync=" << kVsyncNames[s.vsync] << "\n"
        << "vrr=" << (s.vrr ? 1 : 0) << "\n"
        << "hud_scale=" << kUiScales[s.hudScale] << "\n"
        << "menu_scale=" << kUiScales[s.menuScale] << "\n"
        << "button_prompts=" << kPromptNames[s.prompts] << "\n"
        << "split=" << kSplitNames[s.split] << "\n"
        << "# Cars in a race (Game Setup > Number of Cars, up to 12): single player, two players; 0 = the memory card's\n"
        << "cars_single=" << s.carsSingle << "\n"
        << "cars_multi=" << s.carsMulti << "\n";
    writeControls(out);
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
    B::Player players[2] = {B::defaults(0), B::defaults(1)};
    g_loadPlayers = players;
    readFile(s);
    if (g_controlsVersion < 2)
    {
        // An older file: its devices were the old defaults unless changed; those become Auto (a superset of them).
        const int oldDefault[2] = {B::DeviceKeyboardPad1, B::DevicePad2};
        for (int i = 0; i < 2; ++i)
            if (g_sawDevice[i] && players[i].device == oldDefault[i])
            {
                players[i].device = B::DeviceAuto;
                std::fprintf(stderr, "[settings] player %d device: %s (the old default) is now Auto\n", i + 1, B::deviceKey(oldDefault[i]));
            }
    }
    const char *v = nullptr;
    if (envSet("RC_ASPECT", v))
        s.aspect = aspectFromText(v);
    else if (envSet("RC_WIDESCREEN", v))
        s.aspect = *v == '0' ? Aspect4x3 : Aspect16x9;
    if (envSet("RC_HUD_ANCHOR", v))
        s.hudCorrected = *v != '0';
    if (envSet("RC_HUD_SCALE", v))
        s.hudScale = uiScaleIndex(std::atoi(v));
    if (envSet("RC_MENU_SCALE", v))
        s.menuScale = uiScaleIndex(std::atoi(v));
    if (envSet("RC_BUTTON_PROMPTS", v))
        s.prompts = promptsFromText(v);
    if (envSet("RC_SPLIT", v))
        s.split = splitFromText(v);
    if (envSet("RC_CARS_SINGLE", v))
        s.carsSingle = carsFromNumber(std::atoi(v));
    if (envSet("RC_CARS_MULTI", v))
        s.carsMulti = carsFromNumber(std::atoi(v));
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
    // Controls: the file's, then PS2X_PAD1_DEVICE / PS2X_PAD2_DEVICE and PS2X_BIND_<button> (player 1's keyboard).
    for (int i = 0; i < 2; ++i)
        if (envSet(i == 0 ? "PS2X_PAD1_DEVICE" : "PS2X_PAD2_DEVICE", v))
        {
            if (const int d = B::parseDevice(v); d >= 0)
                g_loadPlayers[i].device = d;
            else
                std::fprintf(stderr, "[settings] unknown device %s\n", v);
        }
    static const char *const kBind[16] = {"UP", "DOWN", "LEFT", "RIGHT", "CROSS", "CIRCLE", "SQUARE", "TRIANGLE", "L1", "R1", "L2", "R2", "START", "SELECT", "L3", "R3"};
    for (int c = 0; c < 16; ++c)
        if (envSet(("PS2X_BIND_" + std::string(kBind[c])).c_str(), v))
            if (const int source = B::parseKeySource(v); source >= 0)
                g_loadPlayers[0].key[c] = source;
    for (int i = 0; i < 2; ++i)
        B::setPlayer(i, g_loadPlayers[i]);
    g_loadPlayers = nullptr;
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
    std::fprintf(stderr, "[settings] %s: aspect %s, hud %s, fullscreen %d, scale %d, race fps %d, smooth motion %d, vsync %s, vrr %d, hud scale %d%%, menu scale %d%%, prompts %s, split %s, cars %d/%d\n", g_path.c_str(),
                 kAspectNames[s.aspect], s.hudCorrected ? "corrected" : "stretched", s.fullscreen, s.scale, kRaceFps[s.raceFps], s.smoothMotion,
                 kVsyncNames[s.vsync], s.vrr, kUiScales[s.hudScale], kUiScales[s.menuScale], kPromptNames[s.prompts], kSplitNames[s.split], s.carsSingle, s.carsMulti);
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

void saveControls()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    writeFile(g_settings);
}

} // namespace rc::settings

#include "ps2_host_input.h"
#include "runtime/ps2_input_options.h"
#include "ps2_host_backend.h"
#include "ps2_host_platform.h"
#include "ps2_host_gamepad.h"
#include "runtime/ps2_input_bindings.h"

#include <algorithm>
#include <cstdio>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ps2_host_input
{
    namespace
    {
        using ps2x::iop::PadInputState;
        using ps2x::iop::PadPressure;
        namespace PadButton = ps2x::iop::PadButton;

        // RC: input from raylib's window, or from the host platform when one is installed (ps2_host_platform.h,
        // RC_RENDERER=vk). Both take raylib's codes, so every binding below means the same key in both modes.
        const ps2_host_platform::Hooks *platform() { return ps2_host_platform::hooks(); }
        bool hostKeyDown(int key) { return platform() ? platform()->keyDown(key) : IsKeyDown(key); }
        bool hostKeyPressed(int key) { return platform() ? platform()->keyPressed(key) : IsKeyPressed(key); }
        bool hostMouseButtonDown(int button) { return platform() ? platform()->mouseButtonDown(button) : IsMouseButtonDown(button); }
        bool hostMouseButtonPressed(int button) { return platform() ? platform()->mouseButtonPressed(button) : IsMouseButtonPressed(button); }
        bool hostWindowFocused() { return platform() ? platform()->focused() : IsWindowFocused(); }
        void hostEnableCursor()
        {
            if (platform())
                platform()->setMouseCaptured(false);
            else
                EnableCursor();
        }
        void hostDisableCursor()
        {
            if (platform())
                platform()->setMouseCaptured(true);
            else
                DisableCursor();
        }
        Vector2 hostMouseDelta()
        {
            if (!platform())
                return GetMouseDelta();
            Vector2 delta{};
            platform()->mouseDelta(&delta.x, &delta.y);
            return delta;
        }
        float hostFrameTime() { return platform() ? ps2_host_platform::g_frameSeconds.load(std::memory_order_relaxed) : GetFrameTime(); }
        enum class BindingKind
        {
            None,
            Keyboard,
            Mouse,
            Shift,
            Control
        };

        struct HostBinding
        {
            BindingKind kind;
            int code;
        };

        // The DualShock 2's buttons in ps2_input_bindings::Control order.
        constexpr std::array<uint16_t, 16> kControlButtons{{
            PadButton::Up, PadButton::Down, PadButton::Left, PadButton::Right, PadButton::Cross, PadButton::Circle,
            PadButton::Square, PadButton::Triangle, PadButton::L1, PadButton::R1, PadButton::L2, PadButton::R2,
            PadButton::Start, PadButton::Select, PadButton::L3, PadButton::R3,
        }};
        // PS2X_BIND_<control> (player 1's keyboard, for one run), in Control order.
        constexpr std::array<const char *, 16> kBindEnv{{
            "PS2X_BIND_UP", "PS2X_BIND_DOWN", "PS2X_BIND_LEFT", "PS2X_BIND_RIGHT", "PS2X_BIND_CROSS", "PS2X_BIND_CIRCLE",
            "PS2X_BIND_SQUARE", "PS2X_BIND_TRIANGLE", "PS2X_BIND_L1", "PS2X_BIND_R1", "PS2X_BIND_L2", "PS2X_BIND_R2",
            "PS2X_BIND_START", "PS2X_BIND_SELECT", "PS2X_BIND_L3", "PS2X_BIND_R3",
        }};

        constexpr std::array<std::pair<std::string_view, HostBinding>, 24> kNamedBindings{{
            {"NONE", {BindingKind::None, 0}},
            {"UP", {BindingKind::Keyboard, KEY_UP}},
            {"DOWN", {BindingKind::Keyboard, KEY_DOWN}},
            {"LEFT", {BindingKind::Keyboard, KEY_LEFT}},
            {"RIGHT", {BindingKind::Keyboard, KEY_RIGHT}},
            {"SPACE", {BindingKind::Keyboard, KEY_SPACE}},
            {"BACKSPACE", {BindingKind::Keyboard, KEY_BACKSPACE}},
            {"ENTER", {BindingKind::Keyboard, KEY_ENTER}},
            {"TAB", {BindingKind::Keyboard, KEY_TAB}},
            {"ESCAPE", {BindingKind::Keyboard, KEY_ESCAPE}},
            {"LEFT_SHIFT", {BindingKind::Keyboard, KEY_LEFT_SHIFT}},
            {"RIGHT_SHIFT", {BindingKind::Keyboard, KEY_RIGHT_SHIFT}},
            {"SHIFT", {BindingKind::Shift, 0}},
            {"LEFT_CTRL", {BindingKind::Keyboard, KEY_LEFT_CONTROL}},
            {"RIGHT_CTRL", {BindingKind::Keyboard, KEY_RIGHT_CONTROL}},
            {"LEFT_CONTROL", {BindingKind::Keyboard, KEY_LEFT_CONTROL}},
            {"RIGHT_CONTROL", {BindingKind::Keyboard, KEY_RIGHT_CONTROL}},
            {"CTRL", {BindingKind::Control, 0}},
            {"CONTROL", {BindingKind::Control, 0}},
            {"MOUSE_LEFT", {BindingKind::Mouse, MOUSE_BUTTON_LEFT}},
            {"MOUSE_RIGHT", {BindingKind::Mouse, MOUSE_BUTTON_RIGHT}},
            {"MOUSE_MIDDLE", {BindingKind::Mouse, MOUSE_BUTTON_MIDDLE}},
            {"LMB", {BindingKind::Mouse, MOUSE_BUTTON_LEFT}},
            {"RMB", {BindingKind::Mouse, MOUSE_BUTTON_RIGHT}},
        }};

        std::optional<HostBinding> parseBinding(std::string_view value)
        {
            std::string name(value);
            for (char &ch : name)
            {
                if (ch == ' ' || ch == '-')
                    ch = '_';
                else
                    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            }
            if (name.size() == 1u)
            {
                if (name[0] >= 'A' && name[0] <= 'Z')
                    return HostBinding{BindingKind::Keyboard, KEY_A + name[0] - 'A'};
                if (name[0] >= '0' && name[0] <= '9')
                    return HostBinding{BindingKind::Keyboard, KEY_ZERO + name[0] - '0'};
            }
            for (const auto &[key, binding] : kNamedBindings)
            {
                if (name == key)
                    return binding;
            }
            if (name.size() >= 2u && name[0] == 'F')
            {
                char *end = nullptr;
                const long number = std::strtol(name.c_str() + 1, &end, 10);
                if (*end == '\0' && number >= 1 && number <= 12)
                    return HostBinding{BindingKind::Keyboard, static_cast<int>(KEY_F1 + number - 1)};
            }
            return std::nullopt;
        }

        constexpr std::array<std::pair<PadPressure, uint16_t>, static_cast<size_t>(PadPressure::Count)> kPressureButtons{{
            {PadPressure::Right, PadButton::Right},
            {PadPressure::Left, PadButton::Left},
            {PadPressure::Up, PadButton::Up},
            {PadPressure::Down, PadButton::Down},
            {PadPressure::Triangle, PadButton::Triangle},
            {PadPressure::Circle, PadButton::Circle},
            {PadPressure::Cross, PadButton::Cross},
            {PadPressure::Square, PadButton::Square},
            {PadPressure::L1, PadButton::L1},
            {PadPressure::R1, PadButton::R1},
            {PadPressure::L2, PadButton::L2},
            {PadPressure::R2, PadButton::R2},
        }};

        struct ScriptStep
        {
            double start = 0.0;
            double end = 0.0;
            bool fields = false;
            uint16_t buttons = 0u;
            std::array<uint8_t, 4> axes{0x80u, 0x80u, 0x80u, 0x80u};
        };

        struct AxisName
        {
            std::string_view name;
            size_t axis;
            uint8_t value;
        };

        constexpr std::array<AxisName, 8> kAxisNames{{
            {"lleft", 0u, 0x00u},
            {"lright", 0u, 0xFFu},
            {"lup", 1u, 0x00u},
            {"ldown", 1u, 0xFFu},
            {"rleft", 2u, 0x00u},
            {"rright", 2u, 0xFFu},
            {"rup", 3u, 0x00u},
            {"rdown", 3u, 0xFFu},
        }};

        constexpr std::array<std::pair<std::string_view, uint16_t>, 16> kButtonNames{{
            {"select", PadButton::Select},
            {"l3", PadButton::L3},
            {"r3", PadButton::R3},
            {"start", PadButton::Start},
            {"up", PadButton::Up},
            {"right", PadButton::Right},
            {"down", PadButton::Down},
            {"left", PadButton::Left},
            {"l2", PadButton::L2},
            {"r2", PadButton::R2},
            {"l1", PadButton::L1},
            {"r1", PadButton::R1},
            {"triangle", PadButton::Triangle},
            {"circle", PadButton::Circle},
            {"cross", PadButton::Cross},
            {"square", PadButton::Square},
        }};

        std::vector<std::string_view> split(std::string_view text, char separator)
        {
            std::vector<std::string_view> parts;
            size_t begin = 0u;
            while (begin <= text.size())
            {
                const size_t end = std::min(text.find(separator, begin), text.size());
                parts.push_back(text.substr(begin, end - begin));
                begin = end + 1u;
            }
            return parts;
        }

        std::optional<ScriptStep> parseStep(std::string_view text)
        {
            constexpr double kDefaultHoldSeconds = 1.0;
            constexpr double kDefaultHoldFields = 10.0;
            const std::vector<std::string_view> fields = split(text, ':');
            if (fields.size() < 2u || fields.size() > 3u)
                return std::nullopt;
            try
            {
                ScriptStep step;
                std::string_view start = fields[0];
                step.fields = !start.empty() && start.back() == 'v';
                if (step.fields)
                    start.remove_suffix(1u);
                step.start = std::stod(std::string(start));
                const double defaultHold = step.fields ? kDefaultHoldFields : kDefaultHoldSeconds;
                step.end = step.start + (fields.size() == 3u ? std::stod(std::string(fields[2])) : defaultHold);
                for (std::string_view name : split(fields[1], '+'))
                {
                    const auto axis = std::find_if(kAxisNames.begin(), kAxisNames.end(), [&](const AxisName &entry)
                                                   { return entry.name == name; });
                    if (axis != kAxisNames.end())
                    {
                        step.axes[axis->axis] = axis->value;
                        continue;
                    }
                    const auto found = std::find_if(kButtonNames.begin(), kButtonNames.end(), [&](const auto &entry)
                                                    { return entry.first == name; });
                    if (found == kButtonNames.end())
                        return std::nullopt;
                    step.buttons |= found->second;
                }
                return step;
            }
            catch (...)
            {
                return std::nullopt;
            }
        }

        std::vector<ScriptStep> loadScript()
        {
            std::vector<ScriptStep> script;
            const char *value = std::getenv("PS2X_PAD_SCRIPT");
            if (!value || !*value)
                return script;
            for (std::string_view entry : split(value, ','))
            {
                if (const auto step = parseStep(entry))
                    script.push_back(*step);
                else
                    std::cerr << "[input] ignoring pad script step '" << entry << "'" << std::endl;
            }
            std::cerr << "[input] pad script: " << script.size() << " step(s) on port 0" << std::endl;
            return script;
        }

        struct ScriptedInput
        {
            uint16_t buttons = 0u;
            std::array<uint8_t, 4> axes{0x80u, 0x80u, 0x80u, 0x80u};
            bool active = false;
        };

        ScriptedInput scriptedInput(uint64_t vsyncTick)
        {
            static const std::vector<ScriptStep> script = loadScript();
            static const auto origin = std::chrono::steady_clock::now();
            ScriptedInput input;
            if (script.empty())
                return input;
            const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - origin).count();
            for (const ScriptStep &step : script)
            {
                const double clock = step.fields ? static_cast<double>(vsyncTick) : now;
                if (clock >= step.start && clock < step.end)
                {
                    input.buttons |= step.buttons;
                    for (size_t i = 0; i < input.axes.size(); ++i)
                    {
                        if (step.axes[i] != 0x80u)
                        {
                            input.axes[i] = step.axes[i];
                            input.active = true;
                        }
                    }
                }
            }
            input.active = input.active || input.buttons != 0u;
            return input;
        }

        // PS2X_PAD_LIVE=<file>: presses appended to the file while the game runs (a test driving a running game; no
        // window focus or key injection needed), run in order. A line "<buttons hex> <hold ms> <gap ms> [port]" holds
        // those buttons on pad port 0 (or the port given: 1 is player 2) on top of the host's own input; buttons as in
        // the pad record (0x4000 cross, 0x1000 triangle, 0x0008 start, 0x0010 up, 0x0040 down, 0x0080 left, 0x0020
        // right). A line "pad<n> <button or +/-axis> <hold ms> <gap ms>" presses a virtual SDL gamepad
        // (PS2X_SDL_VIRTUAL_PADS; SDL's names: a, b, x, y, dpup, start, +righttrigger, -leftx, ...), through the whole
        // gamepad path. Run from the window loop (sample); the ports' presses are read by padState.
        std::atomic<uint16_t> g_live[kPadPorts]{};

        void runLiveInput()
        {
            static const char *path = []() -> const char * { const char *v = std::getenv("PS2X_PAD_LIVE"); return v && *v ? v : nullptr; }();
            if (!path)
                return;
            struct Press { int target; uint16_t buttons; std::string control; int hold, gap; }; // target: port, or 100 + virtual pad
            using Clock = std::chrono::steady_clock;
            struct Queue
            {
                std::deque<Press> presses;
                Clock::time_point started{};
                bool running = false, virtualDown = false;
            };
            static long offset = 0;
            static std::string partial;
            static std::map<int, Queue> queues; // each port and virtual pad runs its own lines, at the same time as the others
            static Clock::time_point lastRead{};
            const auto now = Clock::now();
            if (now - lastRead > std::chrono::milliseconds(20))
            {
                lastRead = now;
                if (std::FILE *file = std::fopen(path, "r"))
                {
                    std::fseek(file, 0, SEEK_END);
                    const long size = std::ftell(file);
                    if (size < offset)
                        offset = 0; // the file was replaced
                    std::fseek(file, offset, SEEK_SET);
                    char text[256];
                    size_t got;
                    while ((got = std::fread(text, 1, sizeof(text), file)) > 0)
                        partial.append(text, got);
                    offset = size;
                    std::fclose(file);
                    size_t eol;
                    while ((eol = partial.find('\n')) != std::string::npos)
                    {
                        const std::string line = partial.substr(0, eol);
                        partial.erase(0, eol + 1);
                        int pad = 0, hold = 100, gap = 100, port = 0;
                        char control[64] = {};
                        unsigned buttons = 0;
                        if (std::sscanf(line.c_str(), "pad%d %63s %d %d", &pad, control, &hold, &gap) >= 2 && pad >= 1)
                            queues[100 + pad - 1].presses.push_back({100 + pad - 1, 0, control, hold, gap});
                        else if (std::sscanf(line.c_str(), "%x %d %d %d", &buttons, &hold, &gap, &port) >= 1)
                            queues[port == 1 ? 1 : 0].presses.push_back({port == 1 ? 1 : 0, static_cast<uint16_t>(buttons), {}, hold, gap});
                    }
                }
            }
            struct Publish
            {
                uint16_t out[kPadPorts]{};
                ~Publish()
                {
                    for (uint32_t i = 0; i < kPadPorts; ++i)
                        g_live[i].store(out[i], std::memory_order_relaxed);
                }
            } publish;
            for (auto &[target, q] : queues)
                while (!q.presses.empty())
                {
                    if (!q.running)
                    {
                        q.running = true;
                        q.started = now;
                    }
                    const Press &p = q.presses.front();
                    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - q.started).count();
                    const bool held = age < p.hold;
                    if (target >= 100)
                    {
                        if (held != q.virtualDown)
                        {
                            q.virtualDown = held;
                            if (!ps2_host_gamepad::virtualSet(target - 100, p.control.c_str(), held))
                                std::cerr << "[input] live: no virtual gamepad " << (target - 99) << " or control '" << p.control << "'" << std::endl;
                        }
                    }
                    else if (held)
                        publish.out[target] = p.buttons;
                    if (age < p.hold + p.gap)
                        break;
                    q.presses.pop_front();
                    q.running = false;
                    q.virtualDown = false;
                }
        }

        // PS2X_PAD_RECORD=<file> writes port 0's input as it changes; PS2X_PAD_REPLAY=<file> plays such a file back instead
        // of reading the host. Each line is "epoch frame buttons lx ly rx ry", in hex after the frame.
        //
        // The clock is the epoch in its high 32 bits and frames since that epoch began in its low 32: the game advances the
        // epoch at each point where the time taken varies, such as a finished level load, so that loading can't move input.
        struct PadRecordLine
        {
            uint64_t epoch;
            uint64_t frame;
            uint16_t buttons;
            std::array<uint8_t, 4> axes;
        };

        std::atomic<const std::atomic<uint64_t> *> g_inputFrameCounter{nullptr};

        const std::vector<PadRecordLine> &padReplay()
        {
            static const std::vector<PadRecordLine> lines = []
            {
                std::vector<PadRecordLine> out;
                const char *path = std::getenv("PS2X_PAD_REPLAY");
                if (!path || !*path)
                    return out;
                std::FILE *file = std::fopen(path, "r");
                if (!file)
                {
                    std::cerr << "[input] cannot open pad replay " << path << std::endl;
                    return out;
                }
                char text[128];
                while (std::fgets(text, sizeof(text), file))
                {
                    unsigned long long epoch, frame;
                    unsigned buttons, lx, ly, rx, ry;
                    if (std::sscanf(text, "%llu %llu %x %x %x %x %x", &epoch, &frame, &buttons, &lx, &ly, &rx, &ry) == 7)
                        out.push_back({epoch, frame, static_cast<uint16_t>(buttons),
                                       {static_cast<uint8_t>(lx), static_cast<uint8_t>(ly), static_cast<uint8_t>(rx), static_cast<uint8_t>(ry)}});
                }
                std::fclose(file);
                if (out.empty())
                    std::cerr << "[input] pad replay " << path << " has no usable lines (made before epochs were added?)" << std::endl;
                std::cerr << "[input] pad replay: " << out.size() << " change(s)" << std::endl;
                return out;
            }();
            return lines;
        }

        void recordPad(uint64_t frame, uint16_t buttons, const std::array<uint8_t, 4> &axes)
        {
            static std::mutex mutex;
            static std::FILE *const file = []() -> std::FILE *
            {
                const char *path = std::getenv("PS2X_PAD_RECORD");
                return path && *path ? std::fopen(path, "w") : nullptr;
            }();
            if (file == nullptr)
                return;
            static uint16_t lastButtons = 0u;
            static std::array<uint8_t, 4> lastAxes{0x80u, 0x80u, 0x80u, 0x80u};
            std::lock_guard<std::mutex> lock(mutex);
            if (buttons == lastButtons && axes == lastAxes)
                return;
            lastButtons = buttons;
            lastAxes = axes;
            std::fprintf(file, "%llu %llu %04x %02x %02x %02x %02x\n", static_cast<unsigned long long>(frame >> 32), static_cast<unsigned long long>(frame & 0xFFFFFFFFull),
                         buttons, axes[0], axes[1], axes[2], axes[3]);
            std::fflush(file); // Survives the game being closed or crashing.
        }

        std::mutex g_mutex;
        std::array<PadInputState, kPadPorts> g_state = []
        {
            std::array<PadInputState, kPadPorts> state{};
            state[0].connected = true;
            return state;
        }();

        constexpr uint8_t kAxisNeutral = 0x7Fu;

        uint8_t axisToByte(float value)
        {
            const float clamped = std::clamp(value, -1.0f, 1.0f);
            const float scaled = std::round(static_cast<float>(kAxisNeutral) + clamped * (clamped < 0.0f ? 127.0f : 128.0f));
            return static_cast<uint8_t>(std::clamp(scaled, 0.0f, 255.0f));
        }

        uint8_t keyAxis(int negative, int positive)
        {
            const bool low = hostKeyDown(negative);
            const bool high = hostKeyDown(positive);
            if (low == high)
                return 0x80u;
            return low ? 0x00u : 0xFFu;
        }

        std::atomic<const std::atomic<uint64_t> *> g_fieldCounter{nullptr};
        std::atomic<uint32_t> g_invertRightDefault{0u};
        std::atomic<bool> g_mouseCameraEnabled{false};
        std::atomic<bool> g_mouseStickEnabled{true};
        bool g_mouseLocked = false;
        int g_ignoredMouseButton = -1;
        bool g_skipMouseDelta = false;
        bool g_mouseMoved = false;
        float g_mouseRightX = 0.0f;
        float g_mouseRightY = 0.0f;
        MouseCameraInput g_mouseCameraInput;
        std::chrono::steady_clock::time_point g_mouseInputStart;

        void recordMouseCameraInput(Vector2 delta, bool active)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!active)
            {
                g_mouseCameraInput = {};
                return;
            }
            g_mouseCameraInput.active = true;
            if (delta.x == 0.0f && delta.y == 0.0f)
                return;
            const auto now = std::chrono::steady_clock::now();
            if ((g_mouseCameraInput.x == 0.0f && g_mouseCameraInput.y == 0.0f) ||
                now - g_mouseInputStart > std::chrono::milliseconds(100))
            {
                g_mouseCameraInput.x = 0.0f;
                g_mouseCameraInput.y = 0.0f;
                g_mouseInputStart = now;
            }
            g_mouseCameraInput.x += delta.x;
            g_mouseCameraInput.y += delta.y;
        }

        bool isHostBindingDown(const HostBinding &binding)
        {
            switch (binding.kind)
            {
            case BindingKind::Keyboard:
                return hostKeyDown(binding.code);
            case BindingKind::Mouse:
                return g_mouseLocked && binding.code != g_ignoredMouseButton && hostMouseButtonDown(binding.code);
            case BindingKind::Shift:
                return hostKeyDown(KEY_LEFT_SHIFT) || hostKeyDown(KEY_RIGHT_SHIFT);
            case BindingKind::Control:
                return hostKeyDown(KEY_LEFT_CONTROL) || hostKeyDown(KEY_RIGHT_CONTROL);
            default:
                return false;
            }
        }

        float positiveSetting(const char *name, float fallback)
        {
            if (const char *value = std::getenv(name))
            {
                char *end = nullptr;
                const float parsed = std::strtof(value, &end);
                if (end != value && *end == '\0' && std::isfinite(parsed) && parsed > 0.0f)
                    return parsed;
            }
            return fallback;
        }

        float mouseSensitivity()
        {
            static const float sensitivity = positiveSetting("PS2X_MOUSE_SENSITIVITY", 1.0f);
            return sensitivity;
        }

        float mouseVerticalSensitivity()
        {
            static const float sensitivity = positiveSetting("PS2X_MOUSE_VERTICAL_SENSITIVITY", 1.0f);
            return sensitivity;
        }

        Vector2 mouseStick(Vector2 desired)
        {
            const float distance = std::hypot(desired.x, desired.y);
            if (distance <= 0.0f)
                return {};
            constexpr float deadzone = 0.4f;
            const float magnitude = deadzone + (1.0f - deadzone) * std::min(distance, 1.0f);
            const float scale = magnitude / distance;
            return {desired.x * scale, desired.y * scale};
        }

        void sampleMouse()
        {
            g_mouseMoved = false;
            if (!g_mouseCameraEnabled.load(std::memory_order_relaxed))
            {
                if (g_mouseLocked)
                    hostEnableCursor();
                g_mouseLocked = false;
                g_ignoredMouseButton = -1;
                recordMouseCameraInput({}, false);
                return;
            }
            if (!hostWindowFocused() || hostKeyPressed(KEY_ESCAPE))
            {
                if (g_mouseLocked)
                    hostEnableCursor();
                g_mouseLocked = false;
                g_ignoredMouseButton = -1;
                recordMouseCameraInput({}, false);
                return;
            }
            if (!g_mouseLocked)
            {
                int button = -1;
                if (hostMouseButtonPressed(MOUSE_BUTTON_LEFT))
                    button = MOUSE_BUTTON_LEFT;
                else if (hostMouseButtonPressed(MOUSE_BUTTON_RIGHT))
                    button = MOUSE_BUTTON_RIGHT;
                else if (hostMouseButtonPressed(MOUSE_BUTTON_MIDDLE))
                    button = MOUSE_BUTTON_MIDDLE;
                if (button != -1)
                {
                    hostDisableCursor();
                    g_mouseLocked = true;
                    g_ignoredMouseButton = button;
                    g_skipMouseDelta = true;
                }
                recordMouseCameraInput({}, g_mouseLocked);
                return;
            }
            if (g_ignoredMouseButton != -1 && !hostMouseButtonDown(g_ignoredMouseButton))
                g_ignoredMouseButton = -1;
            if (g_skipMouseDelta)
            {
                g_skipMouseDelta = false;
                recordMouseCameraInput({}, true);
                return;
            }

            const Vector2 delta = hostMouseDelta();
            recordMouseCameraInput(delta, true);
            if (!g_mouseStickEnabled.load(std::memory_order_relaxed))
                return;
            const float distance = std::hypot(delta.x, delta.y);
            if (distance <= 0.0f)
                return;
            const float frameTime = std::max(hostFrameTime(), 1.0f / 240.0f);
            const float scale = mouseSensitivity() / (frameTime * 900.0f);
            const Vector2 stick = mouseStick({delta.x * scale, delta.y * scale * mouseVerticalSensitivity()});
            g_mouseRightX = stick.x;
            g_mouseRightY = stick.y;
            g_mouseMoved = true;
        }

        uint32_t rightStickInvert()
        {
            if (const char *value = std::getenv("PS2X_INVERT_RIGHT_STICK"))
            {
                const std::string_view text(value);
                uint32_t axes = 0u;
                if (text.find_first_of("xX") != std::string_view::npos || text == "1")
                    axes |= kInvertRightX;
                if (text.find_first_of("yY") != std::string_view::npos || text == "1")
                    axes |= kInvertRightY;
                return axes;
            }
            return g_invertRightDefault.load(std::memory_order_relaxed);
        }

        void applyPressure(PadInputState &state)
        {
            for (const auto &[pressure, button] : kPressureButtons)
                state.pressure[static_cast<size_t>(pressure)] = (state.buttons & button) != 0u ? 0xFFu : 0x00u;
        }

        // ---- The players' bindings (runtime/ps2_input_bindings.h) ----
        namespace B = ps2_input_bindings;
        std::mutex g_bindMutex;
        std::array<B::Player, kPadPorts> g_players;
        bool g_playersSet = false;
        std::atomic<uint8_t> g_motors[kPadPorts][2]{}; // the game's small and large motor per port

        // Player 1's keyboard from PS2X_BIND_* (one run), over what it had.
        void applyBindEnvironment(B::Player &p)
        {
            for (size_t i = 0; i < kBindEnv.size(); ++i)
                if (const char *value = std::getenv(kBindEnv[i]); value && *value)
                {
                    const int source = B::parseKeySource(value);
                    if (source >= 0)
                        p.key[i] = source;
                    else
                        std::cerr << "[input] invalid " << kBindEnv[i] << "='" << value << "', ignored" << std::endl;
                }
        }

        void ensurePlayers() // g_bindMutex held
        {
            if (g_playersSet)
                return;
            g_playersSet = true;
            for (uint32_t i = 0; i < kPadPorts; ++i)
                g_players[i] = B::defaults(static_cast<int>(i));
            applyBindEnvironment(g_players[0]);
        }

        // How far a source is pressed, 0..1; *analog says it was a trigger or stick (pressure follows it).
        float sourceValue(int source, bool keyboard, int gamepad, bool *analog)
        {
            const int kind = source & 0xF0000, code = source & 0xFFFF;
            switch (kind)
            {
            case B::kKey: return keyboard && hostKeyDown(code) ? 1.0f : 0.0f;
            case B::kMouse: return keyboard && isHostBindingDown({BindingKind::Mouse, code}) ? 1.0f : 0.0f;
            case B::kAnyShift: return keyboard && isHostBindingDown({BindingKind::Shift, 0}) ? 1.0f : 0.0f;
            case B::kAnyControl: return keyboard && isHostBindingDown({BindingKind::Control, 0}) ? 1.0f : 0.0f;
            case B::kPadButton: return gamepad >= 0 && ps2_host_gamepad::button(gamepad, code) ? 1.0f : 0.0f;
            case B::kPadAxis:
            {
                if (gamepad < 0)
                    return 0.0f;
                const float v = ps2_host_gamepad::axis(gamepad, code >> 1) * ((code & 1) ? -1.0f : 1.0f);
                if (analog && v > 0.0f)
                    *analog = true;
                return std::clamp(v, 0.0f, 1.0f);
            }
            default: return 0.0f;
            }
        }

        // ---- Rebinding: the next new press ----
        struct Capture
        {
            bool active = false, fresh = false, keyboard = false;
            int pad = -1, result = B::CaptureWaiting, source = 0;
            std::chrono::steady_clock::time_point deadline{}, releaseUntil{};
            std::array<bool, 32> buttonArmed{};
            std::array<bool, 8> axisArmed{};
        };
        std::mutex g_captureMutex;
        Capture g_capture;
        std::atomic<bool> g_suppress{false}; // the game sees released pads (a capture, and a moment after)

        void runCapture() // the window loop
        {
            std::lock_guard<std::mutex> lock(g_captureMutex);
            Capture &c = g_capture;
            const auto now = std::chrono::steady_clock::now();
            if (!c.active)
            {
                g_suppress.store(now < c.releaseUntil, std::memory_order_relaxed);
                return;
            }
            g_suppress.store(true, std::memory_order_relaxed);
            const auto finish = [&](int result, int source)
            {
                c.active = false;
                c.result = result;
                c.source = source;
                c.releaseUntil = now + std::chrono::milliseconds(300); // the press that bound it is not seen by the game
            };
            const bool padOk = c.pad >= 0 && ps2_host_gamepad::present(c.pad);
            if (c.fresh)
            {
                // Only presses that start after this: held buttons and deflected axes count once released.
                c.fresh = false;
                for (int b = 0; b < 32; ++b)
                    c.buttonArmed[b] = !(padOk && ps2_host_gamepad::button(c.pad, b));
                for (int a = 0; a < 6; ++a)
                    c.axisArmed[a] = !(padOk && std::fabs(ps2_host_gamepad::axis(c.pad, a)) > 0.3f);
                return;
            }
            // Escape and Delete work for a gamepad-only player too.
            if (hostKeyPressed(KEY_ESCAPE))
                return finish(B::CaptureCancelled, 0);
            if (hostKeyPressed(KEY_DELETE) || hostKeyPressed(KEY_BACKSPACE))
                return finish(B::CaptureClear, 0);
            if (c.keyboard)
            {
                for (int k = 32; k <= 348; ++k)
                    if (k != KEY_F11 && hostKeyPressed(k)) // F11 stays the fullscreen key
                        return finish(B::CaptureKey, B::kKey | k);
            }
            if (padOk)
            {
                // The standard buttons, misc1 (share/capture) and the four paddles; not the touchpad click or the
                // misc2-6 inputs some pads report constantly (a Steam Controller's capacitive grips and pads).
                for (int b = 0; b < 20; ++b)
                {
                    const bool down = ps2_host_gamepad::button(c.pad, b);
                    if (down && c.buttonArmed[b])
                        return finish(B::CapturePad, B::kPadButton | b);
                    if (!down)
                        c.buttonArmed[b] = true;
                }
                for (int a = 0; a < 6; ++a)
                {
                    const float v = ps2_host_gamepad::axis(c.pad, a);
                    if (c.axisArmed[a] && std::fabs(v) > 0.6f)
                        return finish(B::CapturePad, B::kPadAxis | a << 1 | (v < 0.0f ? 1 : 0));
                    if (std::fabs(v) < 0.3f)
                        c.axisArmed[a] = true;
                }
            }
            if (now > c.deadline)
                finish(B::CaptureTimedOut, 0);
        }

        PadInputState readPort(uint32_t port)
        {
            PadInputState state;
            B::Player pl;
            {
                std::lock_guard<std::mutex> lock(g_bindMutex);
                ensurePlayers();
                pl = g_players[port];
            }
            const bool keyboard = B::deviceHasKeyboard(pl.device);
            const int slot = B::deviceGamepad(pl.device);
            const int gamepad = slot >= 0 && ps2_host_gamepad::present(slot) ? slot : -1;
            state.connected = pl.device != B::DeviceNone && (keyboard || gamepad >= 0);
            if (!state.connected)
                return state;
            state.leftX = kAxisNeutral;
            state.leftY = kAxisNeutral;
            state.rightX = kAxisNeutral;
            state.rightY = kAxisNeutral;

            float value[B::ControlCount]{};
            bool analog[B::ControlCount]{};
            for (int c = 0; c < B::ControlCount; ++c)
                value[c] = std::max(sourceValue(pl.key[c], keyboard, -1, &analog[c]), sourceValue(pl.pad[c], false, gamepad, &analog[c]));
            for (size_t c = 0; c < kControlButtons.size(); ++c)
                if (value[c] >= (analog[c] ? 0.2f : 0.5f)) // a trigger or stick counts as a press from a fifth of its travel
                    state.buttons |= kControlButtons[c];
            applyPressure(state);
            // An analog source gives its pressure (a pressure-sensitive button on a trigger).
            for (const auto &[pressure, button] : kPressureButtons)
                for (size_t c = 0; c < kControlButtons.size(); ++c)
                    if (kControlButtons[c] == button && analog[c] && (state.buttons & button) != 0u)
                        state.pressure[static_cast<size_t>(pressure)] = static_cast<uint8_t>(std::lround(std::clamp(value[c], 0.0f, 1.0f) * 255.0f));
            const auto stick = [&](int negative, int positive) { return value[positive] - value[negative]; };
            state.leftX = axisToByte(stick(B::LeftStickLeft, B::LeftStickRight));
            state.leftY = axisToByte(stick(B::LeftStickUp, B::LeftStickDown));
            const uint32_t invert = rightStickInvert();
            const float rightX = stick(B::RightStickLeft, B::RightStickRight), rightY = stick(B::RightStickUp, B::RightStickDown);
            state.rightX = axisToByte((invert & kInvertRightX) != 0u ? -rightX : rightX);
            state.rightY = axisToByte((invert & kInvertRightY) != 0u ? -rightY : rightY);
            if (keyboard && port == 0u && g_mouseMoved && g_mouseStickEnabled.load(std::memory_order_relaxed))
            {
                state.rightX = axisToByte((invert & kInvertRightX) != 0u ? -g_mouseRightX : g_mouseRightX);
                state.rightY = axisToByte((invert & kInvertRightY) != 0u ? -g_mouseRightY : g_mouseRightY);
            }
            return state;
        }

        // The game's motors to the gamepad each port reads; other gamepads still.
        void runRumble()
        {
            uint8_t small[ps2_host_gamepad::kSlots]{}, large[ps2_host_gamepad::kSlots]{};
            std::array<B::Player, kPadPorts> players;
            {
                std::lock_guard<std::mutex> lock(g_bindMutex);
                ensurePlayers();
                players = g_players;
            }
            for (uint32_t port = 0; port < kPadPorts; ++port)
            {
                const int slot = B::deviceGamepad(players[port].device);
                if (slot < 0)
                    continue;
                small[slot] = std::max(small[slot], g_motors[port][0].load(std::memory_order_relaxed));
                large[slot] = std::max(large[slot], g_motors[port][1].load(std::memory_order_relaxed));
            }
            for (int slot = 0; slot < ps2_host_gamepad::kSlots; ++slot)
                ps2_host_gamepad::setRumble(slot, small[slot], large[slot]);
        }
    }

    void setRightStickInvertDefault(uint32_t axes)
    {
        g_invertRightDefault.store(axes, std::memory_order_relaxed);
    }

    void setMouseCameraEnabled(bool enabled)
    {
        g_mouseCameraEnabled.store(enabled, std::memory_order_relaxed);
    }

    void setInputFrameCounter(const std::atomic<uint64_t> *counter)
    {
        g_inputFrameCounter.store(counter);
    }

    void setFieldCounter(const std::atomic<uint64_t> *counter)
    {
        g_fieldCounter.store(counter);
    }

    void sample()
    {
        sampleMouse();
        ps2_host_gamepad::update();
        runLiveInput();
        runCapture();
        runRumble();
        std::array<PadInputState, kPadPorts> state{};
        for (uint32_t port = 0; port < kPadPorts; ++port)
            state[port] = readPort(port);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_state = state;
    }

    void setMouseStickEnabled(bool enabled)
    {
        g_mouseStickEnabled.store(enabled, std::memory_order_relaxed);
    }

    MouseCameraInput consumeMouseCameraInput()
    {
        MouseCameraInput input;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            input = g_mouseCameraInput;
            g_mouseCameraInput.x = 0.0f;
            g_mouseCameraInput.y = 0.0f;
            if (std::chrono::steady_clock::now() - g_mouseInputStart > std::chrono::milliseconds(100))
            {
                input.x = 0.0f;
                input.y = 0.0f;
            }
        }
        const uint32_t invert = rightStickInvert();
        input.x *= mouseSensitivity() * ((invert & kInvertRightX) != 0u ? -1.0f : 1.0f);
        input.y *= mouseSensitivity() * mouseVerticalSensitivity() * ((invert & kInvertRightY) != 0u ? -1.0f : 1.0f);
        return input;
    }

    void releaseMouse()
    {
        if (g_mouseLocked)
            hostEnableCursor();
        g_mouseLocked = false;
        g_ignoredMouseButton = -1;
        g_mouseMoved = false;
        recordMouseCameraInput({}, false);
    }

    ps2x::iop::PadInputState padState(uint32_t port)
    {
        if (port >= kPadPorts)
            return {};
        PadInputState state;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            state = g_state[port];
        }
        if (g_suppress.load(std::memory_order_relaxed)) // rebinding: the game sees the pad released
        {
            const bool connected = state.connected;
            state = PadInputState{};
            state.connected = connected;
            state.leftX = state.leftY = state.rightX = state.rightY = kAxisNeutral;
        }
        const auto addLive = [&]
        {
            if (const uint16_t live = g_live[port].load(std::memory_order_relaxed); live != 0u)
            {
                if (!state.connected)
                {
                    state.connected = true;
                    state.leftX = state.leftY = state.rightX = state.rightY = kAxisNeutral;
                }
                state.buttons |= live;
                applyPressure(state);
            }
        };
        if (port != 0u)
            addLive();
        if (port == 0u)
        {
            const std::atomic<uint64_t> *counter = g_fieldCounter.load();
            const ScriptedInput scripted = scriptedInput(counter ? counter->load() : 0u);
            if (scripted.active)
            {
                state.connected = true;
                state.buttons |= scripted.buttons;
                if (scripted.axes[0] != 0x80u)
                    state.leftX = scripted.axes[0];
                if (scripted.axes[1] != 0x80u)
                    state.leftY = scripted.axes[1];
                if (scripted.axes[2] != 0x80u)
                    state.rightX = scripted.axes[2];
                if (scripted.axes[3] != 0x80u)
                    state.rightY = scripted.axes[3];
                applyPressure(state);
            }

            const std::atomic<uint64_t> *frameCounter = g_inputFrameCounter.load();
            const uint64_t frame = frameCounter ? frameCounter->load() : (counter ? counter->load() : 0u);
            const std::vector<PadRecordLine> &replay = padReplay();
            if (!replay.empty())
            {
                // Changes are delivered in order and each is held long enough for the game to see it, so a press and release
                // recorded on the same frame still count. An epoch the game has moved past is skipped, input neutral.
                static std::mutex replayMutex;
                static size_t next = 0u;
                static PadRecordLine current{0u, 0u, 0u, {0x80u, 0x80u, 0x80u, 0x80u}};
                static auto lastApplied = std::chrono::steady_clock::now();
                const uint64_t epoch = frame >> 32, epochFrame = frame & 0xFFFFFFFFull;
                std::lock_guard<std::mutex> replayLock(replayMutex);
                while (next < replay.size() && replay[next].epoch < epoch)
                {
                    ++next;
                    current = PadRecordLine{epoch, 0u, 0u, {0x80u, 0x80u, 0x80u, 0x80u}};
                }
                const auto now = std::chrono::steady_clock::now();
                if (next < replay.size() && replay[next].epoch == epoch && replay[next].frame <= epochFrame &&
                    now - lastApplied >= std::chrono::milliseconds(35))
                {
                    current = replay[next++];
                    lastApplied = now;
                }
                state = PadInputState{};
                state.connected = true;
                state.buttons = current.buttons;
                state.leftX = current.axes[0];
                state.leftY = current.axes[1];
                state.rightX = current.axes[2];
                state.rightY = current.axes[3];
                applyPressure(state);
            }

            addLive(); // after a replay, which sets the whole state
            recordPad(frame, static_cast<uint16_t>(state.buttons), {state.leftX, state.leftY, state.rightX, state.rightY});
        }
        return state;
    }
}

namespace ps2_host_input
{
    void setPadVibration(uint32_t port, uint8_t small, uint8_t large)
    {
        if (port >= kPadPorts)
            return;
        const uint8_t oldSmall = g_motors[port][0].exchange(small, std::memory_order_relaxed);
        const uint8_t oldLarge = g_motors[port][1].exchange(large, std::memory_order_relaxed);
        const bool changed = oldSmall != small || oldLarge != large;
        static const bool log = std::getenv("PS2X_RUMBLE_LOG") != nullptr;
        if (log && changed)
            std::cerr << "[input] port " << port << " motors small " << int(small) << " large " << int(large) << std::endl;
    }
}

namespace ps2_input_bindings
{
    namespace
    {
        constexpr const char *kControlNames[ControlCount] = {
            "Up", "Down", "Left", "Right", "Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "Start", "Select", "L3", "R3",
            "Left stick left", "Left stick right", "Left stick up", "Left stick down",
            "Right stick left", "Right stick right", "Right stick up", "Right stick down"};
        constexpr const char *kControlKeys[ControlCount] = {
            "up", "down", "left", "right", "cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "start", "select", "l3", "r3",
            "lstick_left", "lstick_right", "lstick_up", "lstick_down", "rstick_left", "rstick_right", "rstick_up", "rstick_down"};
        constexpr const char *kDeviceNames[DeviceCount] = {"None", "Keyboard", "Gamepad 1", "Gamepad 2", "Gamepad 3", "Gamepad 4",
                                                           "Keyboard + Gamepad 1", "Keyboard + Gamepad 2"};
        constexpr const char *kDeviceKeys[DeviceCount] = {"none", "keyboard", "pad1", "pad2", "pad3", "pad4", "keyboard+pad1", "keyboard+pad2"};

        struct KeyName { int key; const char *name; const char *label; };
        constexpr KeyName kKeyNames[] = {
            {KEY_SPACE, "space", "Space"}, {KEY_APOSTROPHE, "apostrophe", "'"}, {KEY_COMMA, "comma", ","}, {KEY_MINUS, "minus", "-"},
            {KEY_PERIOD, "period", "."}, {KEY_SLASH, "slash", "/"}, {KEY_SEMICOLON, "semicolon", ";"}, {KEY_EQUAL, "equal", "="},
            {KEY_LEFT_BRACKET, "left_bracket", "["}, {KEY_BACKSLASH, "backslash", "\\"}, {KEY_RIGHT_BRACKET, "right_bracket", "]"},
            {KEY_GRAVE, "grave", "`"}, {KEY_ESCAPE, "escape", "Esc"}, {KEY_ENTER, "enter", "Enter"}, {KEY_TAB, "tab", "Tab"},
            {KEY_BACKSPACE, "backspace", "Backspace"}, {KEY_INSERT, "insert", "Insert"}, {KEY_DELETE, "delete", "Delete"},
            {KEY_RIGHT, "right", "Right"}, {KEY_LEFT, "left", "Left"}, {KEY_DOWN, "down", "Down"}, {KEY_UP, "up", "Up"},
            {KEY_PAGE_UP, "page_up", "Page Up"}, {KEY_PAGE_DOWN, "page_down", "Page Down"}, {KEY_HOME, "home", "Home"}, {KEY_END, "end", "End"},
            {KEY_CAPS_LOCK, "caps_lock", "Caps Lock"}, {KEY_SCROLL_LOCK, "scroll_lock", "Scroll Lock"}, {KEY_NUM_LOCK, "num_lock", "Num Lock"},
            {KEY_PRINT_SCREEN, "print_screen", "Print Screen"}, {KEY_PAUSE, "pause", "Pause"},
            {KEY_KP_DECIMAL, "kp_decimal", "Num ."}, {KEY_KP_DIVIDE, "kp_divide", "Num /"}, {KEY_KP_MULTIPLY, "kp_multiply", "Num *"},
            {KEY_KP_SUBTRACT, "kp_subtract", "Num -"}, {KEY_KP_ADD, "kp_add", "Num +"}, {KEY_KP_ENTER, "kp_enter", "Num Enter"}, {KEY_KP_EQUAL, "kp_equal", "Num ="},
            {KEY_LEFT_SHIFT, "left_shift", "Left Shift"}, {KEY_LEFT_CONTROL, "left_ctrl", "Left Ctrl"}, {KEY_LEFT_ALT, "left_alt", "Left Alt"},
            {KEY_LEFT_SUPER, "left_super", "Left Super"}, {KEY_RIGHT_SHIFT, "right_shift", "Right Shift"}, {KEY_RIGHT_CONTROL, "right_ctrl", "Right Ctrl"},
            {KEY_RIGHT_ALT, "right_alt", "Right Alt"}, {KEY_RIGHT_SUPER, "right_super", "Right Super"}, {KEY_KB_MENU, "menu", "Menu"},
        };

        std::string lower(std::string_view text)
        {
            std::string out(text);
            for (char &ch : out)
                ch = ch == ' ' || ch == '-' ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return out;
        }

        int padButtonSource(int b) { return kPadButton | b; }
        int padAxisSource(int a, bool negative) { return kPadAxis | a << 1 | (negative ? 1 : 0); }
        // SDL's gamepad enums (fixed by its ABI), so the defaults do not need SDL's headers.
        enum { South, East, West, North, Back, Guide, StartB, LeftStick, RightStick, LeftShoulder, RightShoulder, DUp, DDown, DLeft, DRight };
        enum { LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger };
    } // namespace

    const char *controlName(int c) { return c >= 0 && c < ControlCount ? kControlNames[c] : "?"; }
    const char *controlKey(int c) { return c >= 0 && c < ControlCount ? kControlKeys[c] : "?"; }
    const char *deviceName(int d) { return d >= 0 && d < DeviceCount ? kDeviceNames[d] : "?"; }
    const char *deviceKey(int d) { return d >= 0 && d < DeviceCount ? kDeviceKeys[d] : "none"; }
    int parseDevice(const char *text)
    {
        const std::string t = lower(text ? text : "");
        for (int d = 0; d < DeviceCount; ++d)
            if (t == kDeviceKeys[d])
                return d;
        return -1;
    }
    int deviceGamepad(int d)
    {
        switch (d)
        {
        case DevicePad1: case DeviceKeyboardPad1: return 0;
        case DevicePad2: case DeviceKeyboardPad2: return 1;
        case DevicePad3: return 2;
        case DevicePad4: return 3;
        default: return -1;
        }
    }

    std::string sourceName(int s)
    {
        const int kind = s & 0xF0000, code = s & 0xFFFF;
        switch (kind)
        {
        case kKey:
            if (code >= KEY_A && code <= KEY_Z)
                return std::string(1, static_cast<char>('A' + code - KEY_A));
            if (code >= KEY_ZERO && code <= KEY_NINE)
                return std::string(1, static_cast<char>('0' + code - KEY_ZERO));
            if (code >= KEY_F1 && code <= KEY_F12)
                return "F" + std::to_string(code - KEY_F1 + 1);
            if (code >= KEY_KP_0 && code <= KEY_KP_9)
                return "Num " + std::to_string(code - KEY_KP_0);
            for (const KeyName &k : kKeyNames)
                if (k.key == code)
                    return k.label;
            return "Key " + std::to_string(code);
        case kMouse: return code == MOUSE_BUTTON_LEFT ? "Mouse Left" : code == MOUSE_BUTTON_RIGHT ? "Mouse Right" : "Mouse Middle";
        case kAnyShift: return "Shift";
        case kAnyControl: return "Ctrl";
        case kPadButton: return "Pad " + ps2_host_gamepad::buttonLabel(code);
        case kPadAxis:
        {
            const int a = code >> 1;
            const bool neg = code & 1;
            switch (a)
            {
            case LeftTrigger: return "Pad LT";
            case RightTrigger: return "Pad RT";
            case LeftX: return neg ? "Pad LS Left" : "Pad LS Right";
            case LeftY: return neg ? "Pad LS Up" : "Pad LS Down";
            case RightX: return neg ? "Pad RS Left" : "Pad RS Right";
            case RightY: return neg ? "Pad RS Up" : "Pad RS Down";
            default: return "Pad axis";
            }
        }
        default: return "None";
        }
    }

    std::string sourceKey(int s)
    {
        const int kind = s & 0xF0000, code = s & 0xFFFF;
        switch (kind)
        {
        case kKey:
            if (code >= KEY_A && code <= KEY_Z)
                return std::string(1, static_cast<char>('A' + code - KEY_A));
            if (code >= KEY_ZERO && code <= KEY_NINE)
                return std::string(1, static_cast<char>('0' + code - KEY_ZERO));
            if (code >= KEY_F1 && code <= KEY_F12)
                return "F" + std::to_string(code - KEY_F1 + 1);
            if (code >= KEY_KP_0 && code <= KEY_KP_9)
                return "kp_" + std::to_string(code - KEY_KP_0);
            for (const KeyName &k : kKeyNames)
                if (k.key == code)
                    return k.name;
            return "key" + std::to_string(code);
        case kMouse: return code == MOUSE_BUTTON_LEFT ? "mouse_left" : code == MOUSE_BUTTON_RIGHT ? "mouse_right" : "mouse_middle";
        case kAnyShift: return "shift";
        case kAnyControl: return "ctrl";
        case kPadButton:
        {
            const char *k = ps2_host_gamepad::buttonKey(code);
            return k ? k : "button" + std::to_string(code);
        }
        case kPadAxis:
        {
            const char *k = ps2_host_gamepad::axisKey(code >> 1);
            return std::string((code & 1) ? "-" : "+") + (k ? k : "axis" + std::to_string(code >> 1));
        }
        default: return "none";
        }
    }

    int parseKeySource(const char *text)
    {
        if (!text)
            return -1;
        const std::string t = lower(text);
        if (t.empty() || t == "none")
            return 0;
        if (t.size() == 1u && t[0] >= 'a' && t[0] <= 'z')
            return kKey | (KEY_A + t[0] - 'a');
        if (t.size() == 1u && t[0] >= '0' && t[0] <= '9')
            return kKey | (KEY_ZERO + t[0] - '0');
        if (t.size() >= 2u && t[0] == 'f' && std::isdigit(static_cast<unsigned char>(t[1])))
        {
            const int n = std::atoi(t.c_str() + 1);
            if (n >= 1 && n <= 12)
                return kKey | (KEY_F1 + n - 1);
        }
        if (t.rfind("kp_", 0) == 0 && t.size() == 4u && std::isdigit(static_cast<unsigned char>(t[3])))
            return kKey | (KEY_KP_0 + t[3] - '0');
        if (t.rfind("key", 0) == 0 && t.size() > 3u && std::isdigit(static_cast<unsigned char>(t[3])))
            return kKey | std::atoi(t.c_str() + 3);
        for (const KeyName &k : kKeyNames)
            if (t == k.name)
                return kKey | k.key;
        // The names PS2X_BIND_* took before.
        if (t == "left_control") return kKey | KEY_LEFT_CONTROL;
        if (t == "right_control") return kKey | KEY_RIGHT_CONTROL;
        if (t == "shift") return kAnyShift;
        if (t == "ctrl" || t == "control") return kAnyControl;
        if (t == "mouse_left" || t == "lmb") return kMouse | MOUSE_BUTTON_LEFT;
        if (t == "mouse_right" || t == "rmb") return kMouse | MOUSE_BUTTON_RIGHT;
        if (t == "mouse_middle") return kMouse | MOUSE_BUTTON_MIDDLE;
        return -1;
    }

    int parsePadSource(const char *text)
    {
        if (!text)
            return -1;
        const std::string t = lower(text);
        if (t.empty() || t == "none")
            return 0;
        if (t[0] == '+' || t[0] == '-')
        {
            const int a = ps2_host_gamepad::axisFromKey(t.c_str() + 1);
            return a >= 0 ? padAxisSource(a, t[0] == '-') : -1;
        }
        const int b = ps2_host_gamepad::buttonFromKey(t.c_str());
        return b >= 0 ? padButtonSource(b) : -1;
    }

    Player defaults(int player)
    {
        Player p;
        p.device = player == 0 ? DeviceKeyboardPad1 : DevicePad2;
        const int pad[ControlCount] = {
            padButtonSource(DUp), padButtonSource(DDown), padButtonSource(DLeft), padButtonSource(DRight),
            padButtonSource(South), padButtonSource(East), padButtonSource(West), padButtonSource(North),
            padButtonSource(LeftShoulder), padButtonSource(RightShoulder), padAxisSource(LeftTrigger, false), padAxisSource(RightTrigger, false),
            padButtonSource(StartB), padButtonSource(Back), padButtonSource(LeftStick), padButtonSource(RightStick),
            padAxisSource(LeftX, true), padAxisSource(LeftX, false), padAxisSource(LeftY, true), padAxisSource(LeftY, false),
            padAxisSource(RightX, true), padAxisSource(RightX, false), padAxisSource(RightY, true), padAxisSource(RightY, false)};
        std::copy(std::begin(pad), std::end(pad), p.pad);
        // Keyboard: player 1 as the runtime always had it (WASD the left stick); player 2 a second block, if chosen.
        static constexpr int kP1[ControlCount] = {KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_F, KEY_E, KEY_R, KEY_SPACE, KEY_Q, KEY_LEFT_SHIFT,
                                                  KEY_LEFT_CONTROL, KEY_RIGHT_SHIFT, KEY_ENTER, KEY_TAB, KEY_H, KEY_G,
                                                  KEY_A, KEY_D, KEY_W, KEY_S, 0, 0, 0, 0};
        static constexpr int kP2[ControlCount] = {KEY_I, KEY_K, KEY_J, KEY_L, KEY_N, KEY_M, KEY_B, KEY_U, KEY_O, KEY_P,
                                                  KEY_Y, KEY_SEVEN, KEY_ZERO, KEY_NINE, 0, 0,
                                                  0, 0, 0, 0, 0, 0, 0, 0};
        const int *keys = player == 0 ? kP1 : kP2;
        for (int c = 0; c < ControlCount; ++c)
            p.key[c] = keys[c] ? (kKey | keys[c]) : 0;
        return p;
    }

    Player player(int player)
    {
        using namespace ps2_host_input;
        if (player < 0 || player >= static_cast<int>(kPadPorts))
            return {};
        std::lock_guard<std::mutex> lock(g_bindMutex);
        ensurePlayers();
        return g_players[static_cast<size_t>(player)];
    }

    void setPlayer(int player, const Player &p)
    {
        using namespace ps2_host_input;
        if (player < 0 || player >= static_cast<int>(kPadPorts))
            return;
        std::lock_guard<std::mutex> lock(g_bindMutex);
        ensurePlayers();
        g_players[static_cast<size_t>(player)] = p;
    }

    int gamepadCount() { return ps2_host_gamepad::count(); }
    std::string gamepadName(int slot) { return ps2_host_gamepad::name(slot); }

    void beginCapture(bool keyboard, int pad, int timeoutMs)
    {
        using namespace ps2_host_input;
        std::lock_guard<std::mutex> lock(g_captureMutex);
        g_capture.active = true;
        g_capture.fresh = true;
        g_capture.keyboard = keyboard;
        g_capture.pad = pad;
        g_capture.result = CaptureWaiting;
        g_capture.source = 0;
        g_capture.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        g_suppress.store(true, std::memory_order_relaxed);
    }

    int captureResult(int *source)
    {
        using namespace ps2_host_input;
        std::lock_guard<std::mutex> lock(g_captureMutex);
        if (g_capture.active)
            return CaptureWaiting;
        if (source)
            *source = g_capture.source;
        const int r = g_capture.result;
        g_capture.result = CaptureWaiting;
        return r;
    }

    void cancelCapture()
    {
        using namespace ps2_host_input;
        std::lock_guard<std::mutex> lock(g_captureMutex);
        if (g_capture.active)
        {
            g_capture.active = false;
            g_capture.result = CaptureWaiting;
            g_capture.releaseUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        }
    }
}

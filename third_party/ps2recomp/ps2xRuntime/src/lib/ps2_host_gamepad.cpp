#include "ps2_host_gamepad.h"
#include "ps2_host_platform.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <dirent.h>
#include <fstream>
#include <initializer_list>
#include <unistd.h>

#if PS2X_HAS_SDL3
#include <SDL3/SDL.h>
#endif

namespace ps2_host_gamepad
{
#if PS2X_HAS_SDL3
    namespace
    {
        struct Slot
        {
            SDL_Gamepad *pad = nullptr;
            SDL_JoystickID id = 0;
            std::string name;
            SDL_GamepadType type = SDL_GAMEPAD_TYPE_STANDARD; // for the button prompts' labels
            uint8_t small = 0, large = 0; // the motors last sent
            uint64_t rumbleSent = 0;      // SDL_GetTicks of the last send
            uint32_t loggedButtons = 0;   // PS2X_GAMEPAD_LOG: the buttons and axes last logged
            int loggedAxes[SDL_GAMEPAD_AXIS_COUNT]{};
            bool lizardOff = false;       // a 2026 Steam Controller whose lizard mode this game turned off
            bool lizardWarned = false;    // its refusal logged once
            uint64_t lizardSent = 0;      // SDL_GetTicks of the last lizard-off reports
        };
        std::array<Slot, kSlots> g_slots;
        std::atomic<uint16_t> g_rumble[kSlots]{}; // requested: small << 8 | large
        bool g_initialised = false, g_failed = false;
        std::vector<SDL_Joystick *> g_virtual;

        bool SDLCALL virtualRumble(void *userdata, Uint16 low, Uint16 high)
        {
            static const bool log = std::getenv("PS2X_RUMBLE_LOG") != nullptr;
            if (log)
                std::fprintf(stderr, "[gamepad] virtual pad %d rumble low %u high %u\n", static_cast<int>(reinterpret_cast<intptr_t>(userdata)), low, high);
            return true;
        }

        std::vector<SDL_JoystickID> g_virtualIds;

        // A virtual pad's effect (PS2X_SDL_VIRTUAL_PAD_TYPE=steam: the Steam Controller's feature reports): logged.
        bool SDLCALL virtualEffect(void *userdata, const void *data, int size)
        {
            const auto *b = static_cast<const uint8_t *>(data);
            std::fprintf(stderr, "[gamepad] virtual pad %d effect (%d bytes): %02x %02x %02x %02x %02x %02x\n",
                         static_cast<int>(reinterpret_cast<intptr_t>(userdata)), size, b[0], size > 1 ? b[1] : 0, size > 2 ? b[2] : 0,
                         size > 3 ? b[3] : 0, size > 4 ? b[4] : 0, size > 5 ? b[5] : 0);
            return true;
        }

        // Virtual gamepad i (0-based) attached and opened, in g_virtual[i].
        bool attachOne(int i)
        {
            SDL_VirtualJoystickDesc desc;
            SDL_INIT_INTERFACE(&desc);
            desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
            desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
            desc.nbuttons = SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1;
            desc.button_mask = (1u << (SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1)) - 1u;
            desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1u;
            desc.name = "RC virtual gamepad";
            // PS2X_SDL_VIRTUAL_PAD_TYPE=ps4|xbox (test aid): a vendor and product SDL knows, so the pad's type (and its
            // face buttons' labels) are those of a DualShock 4 or an Xbox 360 controller.
            if (const char *t = std::getenv("PS2X_SDL_VIRTUAL_PAD_TYPE"))
            {
                if (std::strcmp(t, "ps4") == 0)
                {
                    desc.vendor_id = 0x054c;
                    desc.product_id = 0x09cc;
                }
                else if (std::strcmp(t, "xbox") == 0)
                {
                    desc.vendor_id = 0x045e;
                    desc.product_id = 0x028e;
                }
                else if (std::strcmp(t, "steam") == 0) // the 2026 Steam Controller over Bluetooth (its lizard-mode reports)
                {
                    desc.vendor_id = 0x28de;
                    desc.product_id = 0x1303;
                }
            }
            desc.userdata = reinterpret_cast<void *>(static_cast<intptr_t>(i + 1));
            desc.Rumble = virtualRumble;
            desc.SendEffect = virtualEffect;
            const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
            SDL_Joystick *joy = id ? SDL_OpenJoystick(id) : nullptr;
            if (!joy)
            {
                std::fprintf(stderr, "[gamepad] virtual gamepad %d: %s\n", i + 1, SDL_GetError());
                return false;
            }
            // Triggers rest at the axis minimum.
            SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
            SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
            if (static_cast<int>(g_virtual.size()) <= i)
            {
                g_virtual.resize(static_cast<size_t>(i) + 1u, nullptr);
                g_virtualIds.resize(static_cast<size_t>(i) + 1u, 0);
            }
            g_virtual[static_cast<size_t>(i)] = joy;
            g_virtualIds[static_cast<size_t>(i)] = id;
            std::fprintf(stderr, "[gamepad] virtual gamepad %d attached (test aid)\n", i + 1);
            return true;
        }

        void attachVirtual()
        {
            const char *v = std::getenv("PS2X_SDL_VIRTUAL_PADS");
            const int n = v ? std::atoi(v) : 0;
            for (int i = 0; i < n && i < kSlots; ++i)
                attachOne(i);
        }

        bool logging()
        {
            static const bool on = [] { const char *v = std::getenv("PS2X_GAMEPAD_LOG"); return v && *v && *v != '0'; }();
            return on;
        }

        // A Valve controller (Steam Controller, Steam Deck) is read through its hidraw node (SDL's HIDAPI Steam drivers,
        // which also turn its keyboard-and-mouse "lizard mode" off). Without the steam-devices udev rules that node
        // is root's only, and SDL falls back to the kernel's evdev pad (lizard mode stays on): say so once.
        void checkValveHidraw()
        {
#if defined(__linux__)
            DIR *dir = opendir("/sys/class/hidraw");
            if (!dir)
                return;
            while (const dirent *e = readdir(dir))
            {
                if (std::strncmp(e->d_name, "hidraw", 6) != 0)
                    continue;
                std::ifstream uevent(std::string("/sys/class/hidraw/") + e->d_name + "/device/uevent");
                std::string line, id, name;
                while (std::getline(uevent, line))
                {
                    if (line.rfind("HID_ID=", 0) == 0)
                        id = line.substr(7);
                    else if (line.rfind("HID_NAME=", 0) == 0)
                        name = line.substr(9);
                }
                if (id.find(":000028DE:") == std::string::npos)
                    continue;
                const std::string node = std::string("/dev/") + e->d_name;
                const bool ok = access(node.c_str(), R_OK | W_OK) == 0;
                if (logging() || !ok)
                    std::fprintf(stderr, "[gamepad] Valve device %s (%s) at %s: %s\n", name.c_str(), id.c_str(), node.c_str(),
                                 ok ? "readable, SDL's HIDAPI driver can open it"
                                    : "NOT readable by this user: install Steam's udev rules (the steam-devices package, "
                                      "60-steam-input.rules: KERNEL==\"hidraw*\", ATTRS{idVendor}==\"28de\", MODE=\"0660\", TAG+=\"uaccess\") and "
                                      "reconnect it; until then it is read as the kernel's evdev pad and its lizard mode stays on");
            }
            closedir(dir);
#endif
        }

        // Lizard mode: a Valve controller with no game reading it types keys and moves the mouse (A Enter, B Escape,
        // d-pad arrows, right trackpad the mouse) through its own keyboard-and-mouse interface ("Steam Ctrl (BT)" and
        // the like). SDL's driver for the 2015 Steam Controller (wired, dongle, Bluetooth) turns it off when it opens one
        // (ID_CLEAR_DIGITAL_MAPPINGS, trackpads to none) and back on when it closes it. Its driver for the 2026 one
        // (SDL_hidapi_steam_triton.c, 3.4.20 and SDL's main) only sends SETTING_LIZARD_MODE = off every 3 s, and the
        // keys keep coming (seen: 28de:1303 over Bluetooth typed Enter, arrows and Escape all through a session). Linux's
        // hid-steam turns that model's lizard mode off with ID_CLEAR_DIGITAL_MAPPINGS plus SETTING_LIZARD_MODE 0, and on
        // with ID_SET_DEFAULT_DIGITAL_MAPPINGS plus ID_LOAD_DEFAULT_SETTINGS: the same reports go through SDL here
        // (SDL_SendGamepadEffect hands a 64-byte buffer, report id 1, to the controller as a feature report). Off when
        // it is opened and again every 3 s; on when it is closed or the game ends. If the game dies, hid-steam turns it
        // on again when the last program holding its hidraw node lets go. PS2X_STEAM_LIZARD=1 leaves it alone.
        bool isSteamController2026(Uint16 vendor, Uint16 product)
        {
            // 1302 wired, 1303 Bluetooth, 1304 / 1305 its wireless pucks (SDL's controller_list.h, Linux's hid-steam).
            return vendor == 0x28de && product >= 0x1302 && product <= 0x1305;
        }

        bool steamFeature(SDL_Gamepad *pad, std::initializer_list<uint8_t> cmd)
        {
            uint8_t buffer[64] = {1}; // report id 1 (REPORT_ID_FEATURES_CONTROLLER), then type, length, payload
            size_t i = 1;
            for (uint8_t b : cmd)
                buffer[i++] = b;
            return SDL_SendGamepadEffect(pad, buffer, sizeof(buffer));
        }

        void steamLizard(Slot &s, int slot, bool on)
        {
            const uint64_t start = SDL_GetTicksNS();
            bool ok;
            if (on)
                ok = steamFeature(s.pad, {0x85, 0}) &  // ID_SET_DEFAULT_DIGITAL_MAPPINGS: Enter, Escape, arrows back
                     steamFeature(s.pad, {0x8e, 0});   // ID_LOAD_DEFAULT_SETTINGS: lizard mode and the mouse back
            else
                ok = steamFeature(s.pad, {0x81, 0}) &          // ID_CLEAR_DIGITAL_MAPPINGS: no keys
                     steamFeature(s.pad, {0x87, 3, 9, 0, 0});  // ID_SET_SETTINGS_VALUES: SETTING_LIZARD_MODE (9) = off
            const double ms = static_cast<double>(SDL_GetTicksNS() - start) / 1e6;
            const bool first = !on && !s.lizardOff;
            if (!ok && (on || !s.lizardWarned))
            {
                std::fprintf(stderr, "[gamepad] slot %d: Steam Controller lizard mode %s: the controller refused the reports (%s)\n", slot + 1,
                             on ? "on" : "off", SDL_GetError());
                s.lizardWarned = true;
            }
            else if (ok && (on || first))
                std::fprintf(stderr, "[gamepad] slot %d: Steam Controller lizard mode %s (%s, 2 feature reports accepted in %.1f ms)\n", slot + 1,
                             on ? "on again" : "off", on ? "its keyboard and mouse back" : "no keys or mouse from it while the game has it",
                             ms);
            s.lizardOff = !on && (ok || s.lizardOff);
            s.lizardSent = SDL_GetTicks();
        }

        void steamLizardUpdate(Slot &s, int slot)
        {
            static const bool keep = [] { const char *v = std::getenv("PS2X_STEAM_LIZARD"); return v && *v == '1'; }();
            if (keep || !s.pad || !isSteamController2026(SDL_GetGamepadVendor(s.pad), SDL_GetGamepadProduct(s.pad)))
                return;
            if (s.lizardSent == 0 || SDL_GetTicks() - s.lizardSent >= 3000)
                steamLizard(s, slot, false);
        }

        // Before a gamepad is closed: lizard mode back on if this game turned it off (and the controller is still there).
        void steamLizardRestore(Slot &s, int slot)
        {
            if (s.lizardOff && SDL_GamepadConnected(s.pad))
                steamLizard(s, slot, true);
            s.lizardOff = false;
        }

        void logGamepad(SDL_Gamepad *pad, int slot)
        {
            if (!logging())
                return;
            SDL_Joystick *joy = SDL_GetGamepadJoystick(pad);
            char guid[64] = {};
            SDL_GUIDToString(SDL_GetJoystickGUID(joy), guid, sizeof(guid));
            const char *path = SDL_GetGamepadPath(pad);
            char *mapping = SDL_GetGamepadMapping(pad);
            Uint16 vendor = SDL_GetGamepadVendor(pad), product = SDL_GetGamepadProduct(pad);
            const char *serial = SDL_GetGamepadSerial(pad);
            // SDL names no driver; the path tells: a hidraw node (or "HID:...") is HIDAPI, /dev/input/event* the kernel's
            // evdev pad, "Virtual" a virtual one.
            const std::string p = path ? path : "";
            const char *driver = p.find("hidraw") != std::string::npos || p.rfind("HID", 0) == 0 ? "HIDAPI"
                                 : p.find("/dev/input/event") != std::string::npos               ? "evdev (kernel)"
                                 : p.empty()                                                     ? "virtual or unknown"
                                                                                                 : "other";
            std::fprintf(stderr, "[gamepad] slot %d: \"%s\" vid %04x pid %04x type %d guid %s path %s driver %s serial %s rumble %d\n  mapping %s\n",
                         slot + 1, SDL_GetGamepadName(pad), vendor, product, static_cast<int>(SDL_GetGamepadType(pad)), guid,
                         path ? path : "-", driver, serial ? serial : "-",
                         SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false) ? 1 : 0,
                         mapping ? mapping : "-");
            SDL_free(mapping);
        }

        void init()
        {
            g_initialised = true;
            // Gamepads only: raylib (GLFW) owns the window, keyboard and mouse. Read whether or not SDL thinks a
            // window of its own is focused (it has none).
            SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
            SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
            // Valve's controllers through SDL's HIDAPI drivers (the 2015 and 2026 Steam Controllers, wired, dongle or
            // Bluetooth, and the Steam Deck): full input and lizard mode off. When Steam runs it creates its own
            // virtual pad for them; SDL leaves that out unless the game was started by Steam.
            SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
            SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAM, "1");
            SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAMDECK, "1");
            checkValveHidraw();
            if (const char *db = std::getenv("PS2X_GAMECONTROLLERDB"); db && *db)
                SDL_SetHint(SDL_HINT_GAMECONTROLLERCONFIG_FILE, db);
            if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
            {
                std::fprintf(stderr, "[gamepad] SDL gamepad support unavailable: %s (keyboard only)\n", SDL_GetError());
                g_failed = true;
                return;
            }
            if (const char *db = std::getenv("PS2X_GAMECONTROLLERDB"); db && *db)
            {
                const int added = SDL_AddGamepadMappingsFromFile(db);
                std::fprintf(stderr, "[gamepad] %d mapping(s) from %s\n", added, db);
            }
            attachVirtual();
            std::fprintf(stderr, "[gamepad] SDL %d.%d.%d gamepads (no window)\n", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION);
        }

        void open(SDL_JoystickID id)
        {
            for (const Slot &s : g_slots)
                if (s.pad && s.id == id)
                    return;
            for (int i = 0; i < kSlots; ++i)
                if (!g_slots[i].pad)
                {
                    SDL_Gamepad *pad = SDL_OpenGamepad(id);
                    if (!pad)
                        return;
                    g_slots[i].pad = pad;
                    g_slots[i].id = id;
                    const char *n = SDL_GetGamepadName(pad);
                    g_slots[i].name = n ? n : "Gamepad";
                    g_slots[i].type = SDL_GetGamepadType(pad);
                    g_slots[i].small = g_slots[i].large = 0;
                    SDL_SetGamepadPlayerIndex(pad, i);
                    std::fprintf(stderr, "[gamepad] gamepad %d connected: %s\n", i + 1, g_slots[i].name.c_str());
                    logGamepad(pad, i);
                    return;
                }
        }

        void close(SDL_JoystickID id)
        {
            for (int i = 0; i < kSlots; ++i)
                if (g_slots[i].pad && g_slots[i].id == id)
                {
                    std::fprintf(stderr, "[gamepad] gamepad %d disconnected: %s\n", i + 1, g_slots[i].name.c_str());
                    steamLizardRestore(g_slots[i], i);
                    SDL_CloseGamepad(g_slots[i].pad);
                    g_slots[i] = Slot{};
                }
        }

        void applyRumble()
        {
            const uint64_t now = SDL_GetTicks();
            for (int i = 0; i < kSlots; ++i)
            {
                Slot &s = g_slots[i];
                if (!s.pad)
                    continue;
                const uint16_t want = g_rumble[i].load(std::memory_order_relaxed);
                const uint8_t small = static_cast<uint8_t>(want >> 8), large = static_cast<uint8_t>(want);
                const bool changed = small != s.small || large != s.large;
                // A rumble lasts the duration given: renewed while on.
                if (!changed && (small == 0 && large == 0 || now - s.rumbleSent < 500))
                    continue;
                s.small = small;
                s.large = large;
                s.rumbleSent = now;
                // DualShock 2: the large motor is the low-frequency one (0-255), the small one high-frequency (on/off).
                SDL_RumbleGamepad(s.pad, static_cast<Uint16>(large * 257u), small ? 0xFFFFu : 0u, small || large ? 1000u : 0u);
            }
        }
    } // namespace

    void update()
    {
        if (!g_initialised)
            init();
        if (g_failed)
            return;
        // With a host platform (RC_RENDERER=vk) its window loop pumps SDL's events; here (raylib's window) this does.
        if (!ps2_host_platform::hooks())
        {
            SDL_Event event;
            while (SDL_PollEvent(&event))
                ;
        }
        else
            SDL_UpdateGamepads();
        if (logging())
        {
            // Every joystick SDL sees (a pad seen twice, as HIDAPI and as the kernel's evdev twin, would show here).
            static int lastCount = -1;
            int jn = 0;
            SDL_JoystickID *js = SDL_GetJoysticks(&jn);
            if (jn != lastCount)
            {
                lastCount = jn;
                std::fprintf(stderr, "[gamepad] SDL joysticks: %d\n", jn);
                for (int k = 0; k < jn; ++k)
                    std::fprintf(stderr, "[gamepad]   id %u \"%s\" path %s vid %04x pid %04x gamepad %d\n", js[k], SDL_GetJoystickNameForID(js[k]),
                                 SDL_GetJoystickPathForID(js[k]) ? SDL_GetJoystickPathForID(js[k]) : "-", SDL_GetJoystickVendorForID(js[k]),
                                 SDL_GetJoystickProductForID(js[k]), SDL_IsGamepad(js[k]) ? 1 : 0);
            }
            SDL_free(js);
        }
        // Hotplug, from the list of gamepads (events may go to the other loop).
        int n = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&n);
        for (int i = 0; i < kSlots; ++i)
            if (g_slots[i].pad)
            {
                bool still = false;
                for (int k = 0; k < n; ++k)
                    still = still || ids[k] == g_slots[i].id;
                if (!still || !SDL_GamepadConnected(g_slots[i].pad))
                    close(g_slots[i].id);
            }
        for (int k = 0; k < n; ++k)
            open(ids[k]);
        SDL_free(ids);
        for (int i = 0; i < kSlots; ++i)
            steamLizardUpdate(g_slots[i], i);
        if (logging())
            for (int i = 0; i < kSlots; ++i)
                if (Slot &sl = g_slots[i]; sl.pad)
                {
                    // Input changes (buttons pressed or released, axes past a quarter of their travel), to check a pad.
                    uint32_t buttons = 0;
                    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && b < 32; ++b)
                        buttons |= SDL_GetGamepadButton(sl.pad, static_cast<SDL_GamepadButton>(b)) ? 1u << b : 0u;
                    bool axesChanged = false;
                    int axes[SDL_GAMEPAD_AXIS_COUNT];
                    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a)
                    {
                        axes[a] = SDL_GetGamepadAxis(sl.pad, static_cast<SDL_GamepadAxis>(a)) / 8192; // quarters
                        axesChanged = axesChanged || axes[a] != sl.loggedAxes[a];
                    }
                    if (buttons != sl.loggedButtons || axesChanged)
                    {
                        std::string pressed;
                        for (int b = 0; b < 32; ++b)
                            if (buttons & (1u << b))
                                pressed += std::string(" ") + (buttonKey(b) ? buttonKey(b) : "?");
                        std::fprintf(stderr, "[gamepad] slot %d input:%s axes lx %d ly %d rx %d ry %d lt %d rt %d (quarters)\n", i + 1,
                                     pressed.empty() ? " none" : pressed.c_str(), axes[0], axes[1], axes[2], axes[3], axes[4], axes[5]);
                        sl.loggedButtons = buttons;
                        std::memcpy(sl.loggedAxes, axes, sizeof(axes));
                    }
                }
        applyRumble();
    }

    void shutdown()
    {
        if (!g_initialised || g_failed)
            return;
        for (int i = 0; i < kSlots; ++i)
            if (g_slots[i].pad)
            {
                steamLizardRestore(g_slots[i], i);
                SDL_CloseGamepad(g_slots[i].pad); // SDL's 2015 Steam Controller driver restores its lizard mode here
                g_slots[i] = Slot{};
            }
    }

    bool present(int slot) { return slot >= 0 && slot < kSlots && g_slots[slot].pad; }
    bool button(int slot, int b) { return present(slot) && SDL_GetGamepadButton(g_slots[slot].pad, static_cast<SDL_GamepadButton>(b)); }
    float axis(int slot, int a)
    {
        if (!present(slot))
            return 0.0f;
        const Sint16 v = SDL_GetGamepadAxis(g_slots[slot].pad, static_cast<SDL_GamepadAxis>(a));
        return v < 0 ? v / 32768.0f : v / 32767.0f;
    }
    int count()
    {
        int n = 0;
        for (const Slot &s : g_slots)
            n += s.pad ? 1 : 0;
        return n;
    }
    std::string name(int slot) { return present(slot) ? g_slots[slot].name : std::string(); }

    std::string buttonLabel(int b)
    {
        SDL_GamepadType type = SDL_GAMEPAD_TYPE_STANDARD;
        for (const Slot &s : g_slots)
            if (s.pad)
            {
                type = SDL_GetGamepadType(s.pad);
                break;
            }
        switch (SDL_GetGamepadButtonLabelForType(type, static_cast<SDL_GamepadButton>(b)))
        {
        case SDL_GAMEPAD_BUTTON_LABEL_A: return "A";
        case SDL_GAMEPAD_BUTTON_LABEL_B: return "B";
        case SDL_GAMEPAD_BUTTON_LABEL_X: return "X";
        case SDL_GAMEPAD_BUTTON_LABEL_Y: return "Y";
        case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return "Cross";
        case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return "Circle";
        case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return "Square";
        case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "Triangle";
        default: break;
        }
        switch (b)
        {
        case SDL_GAMEPAD_BUTTON_BACK: return "Back";
        case SDL_GAMEPAD_BUTTON_GUIDE: return "Guide";
        case SDL_GAMEPAD_BUTTON_START: return "Start";
        case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "LS";
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "RS";
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "LB";
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "RB";
        case SDL_GAMEPAD_BUTTON_DPAD_UP: return "Up";
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "Down";
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "Left";
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "Right";
        default: break;
        }
        const char *k = buttonKey(b);
        return k ? k : "?";
    }
    int buttonLabelOf(int slot, int b)
    {
        if (!present(slot))
            return 0;
        return static_cast<int>(SDL_GetGamepadButtonLabelForType(g_slots[slot].type, static_cast<SDL_GamepadButton>(b)));
    }
    int type(int slot) { return present(slot) ? static_cast<int>(g_slots[slot].type) : 0; }
    const char *buttonKey(int b) { return SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(b)); }
    const char *axisKey(int a) { return SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(a)); }
    int buttonFromKey(const char *key) { return SDL_GetGamepadButtonFromString(key); }
    int axisFromKey(const char *key) { return SDL_GetGamepadAxisFromString(key); }

    void setRumble(int slot, uint8_t small, uint8_t large)
    {
        if (slot >= 0 && slot < kSlots)
            g_rumble[slot].store(static_cast<uint16_t>((small ? 1u : 0u) << 8 | large), std::memory_order_relaxed);
    }

    bool virtualSet(int index, const char *control, bool down)
    {
        if (index < 0 || index >= static_cast<int>(g_virtual.size()) || !control)
            return false;
        // "unplug" and "plug" (on the press): the virtual gamepad disconnected and connected again.
        if (std::strcmp(control, "unplug") == 0 || std::strcmp(control, "plug") == 0)
        {
            if (!down)
                return true;
            SDL_Joystick *&joy = g_virtual[static_cast<size_t>(index)];
            if (control[0] == 'u' && joy)
            {
                SDL_CloseJoystick(joy);
                SDL_DetachVirtualJoystick(g_virtualIds[static_cast<size_t>(index)]);
                joy = nullptr;
                std::fprintf(stderr, "[gamepad] virtual gamepad %d detached (test aid)\n", index + 1);
                return true;
            }
            return control[0] == 'p' && !joy ? attachOne(index) : true;
        }
        SDL_Joystick *joy = g_virtual[static_cast<size_t>(index)];
        if (!joy)
            return false;
        if (control[0] == '+' || control[0] == '-')
        {
            const int a = axisFromKey(control + 1);
            if (a < 0)
                return false;
            const bool trigger = a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || a == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
            const Sint16 value = !down ? (trigger ? -32768 : 0) : control[0] == '+' ? 32767 : -32768;
            return SDL_SetJoystickVirtualAxis(joy, a, value);
        }
        const int b = buttonFromKey(control);
        return b >= 0 && SDL_SetJoystickVirtualButton(joy, b, down);
    }
#else
    void update() {}
    void shutdown() {}
    bool present(int) { return false; }
    bool button(int, int) { return false; }
    float axis(int, int) { return 0.0f; }
    int count() { return 0; }
    std::string name(int) { return {}; }
    std::string buttonLabel(int) { return "?"; }
    int buttonLabelOf(int, int) { return 0; }
    int type(int) { return 0; }
    const char *buttonKey(int) { return nullptr; }
    const char *axisKey(int) { return nullptr; }
    int buttonFromKey(const char *) { return -1; }
    int axisFromKey(const char *) { return -1; }
    void setRumble(int, uint8_t, uint8_t) {}
    bool virtualSet(int, const char *, bool) { return false; }
#endif
}

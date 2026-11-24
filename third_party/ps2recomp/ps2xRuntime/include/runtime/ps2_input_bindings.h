#pragma once
// RC: a virtual DualShock 2 per player (pad ports 0 and 1), each control bound to a keyboard key and a gamepad
// button or axis. Gamepads come from SDL3's gamepad API (its controller mapping database, hotplug, rumble), next to
// the runtime's raylib window; the keyboard from the window. The game side (rc_settings, rc_enhance_menu) keeps the
// bindings in the settings file and offers them in the game's menus.
//
// Thread use: setPlayer/player/beginCapture/captureResult from any thread (the game's); the window loop samples.

#include <cstdint>
#include <string>

namespace ps2_input_bindings
{
    // The controls of a DualShock 2: its 16 buttons, then each stick direction.
    enum Control : int
    {
        Up, Down, Left, Right, Cross, Circle, Square, Triangle, L1, R1, L2, R2, Start, Select, L3, R3,
        LeftStickLeft, LeftStickRight, LeftStickUp, LeftStickDown, RightStickLeft, RightStickRight, RightStickUp, RightStickDown,
        ControlCount
    };
    const char *controlName(int control);   // "Cross", "Left stick left"
    const char *controlKey(int control);    // settings key: "cross", "lstick_left"

    // What a player reads. Auto (the default): player 1 the keyboard and every connected gamepad player 2 does not
    // use, player 2 the next connected gamepad (the first gamepad connected after player 1's, kept while it stays
    // connected). A chosen gamepad that is not connected counts as Auto until it is (logged).
    enum Device : int
    {
        DeviceAuto, DeviceKeyboard, DevicePad1, DevicePad2, DevicePad3, DevicePad4, DeviceKeyboardPad1, DeviceKeyboardPad2, DeviceNone,
        DeviceCount
    };
    const char *deviceName(int device);     // "Keyboard + Gamepad 1"
    const char *deviceKey(int device);      // settings value: "keyboard+pad1"
    int parseDevice(const char *text);      // -1 if unknown
    inline bool deviceHasKeyboard(int d) { return d == DeviceKeyboard || d == DeviceKeyboardPad1 || d == DeviceKeyboardPad2; }
    int deviceGamepad(int device);          // the gamepad slot it reads (0-3), or -1

    // An input source: 0 none; kKey | raylib key; kPadButton | SDL_GamepadButton; kPadAxis | axis << 1 | negative;
    // kMouse | mouse button; kAnyShift, kAnyControl.
    constexpr int kKey = 0x10000, kPadButton = 0x20000, kPadAxis = 0x30000, kMouse = 0x40000, kAnyShift = 0x50000, kAnyControl = 0x60000;
    inline bool isKeyboardSource(int s) { return s != 0 && (s & 0xF0000) != kPadButton && (s & 0xF0000) != kPadAxis; }
    std::string sourceName(int source);     // for menus: "F", "Left Shift", "Pad A", "Pad RT", "Pad Left X-"
    std::string sourceKey(int source);      // for the settings file: "F", "left_shift", "a", "+righttrigger", "-leftx"
    int parseKeySource(const char *text);   // -1 if unknown ("none": 0)
    int parsePadSource(const char *text);   // -1 if unknown ("none": 0)

    struct Player
    {
        int device = DeviceAuto;
        int key[ControlCount]{}; // keyboard (or mouse) source per control
        int pad[ControlCount]{}; // gamepad source per control
    };
    Player defaults(int player); // both Auto
    Player player(int player);
    void setPlayer(int player, const Player &p);

    // Gamepads: how many slots are filled and each one's name (empty if none).
    int gamepadCount();
    std::string gamepadName(int slot);
    bool gamepadPresent(int slot);

    // What a player reads now: its device resolved against the connected gamepads (Auto, or a chosen gamepad that is
    // not connected: fallback). activePad: the gamepad of the set last used (its rumble goes there), else the first.
    struct Active
    {
        bool keyboard = false;
        unsigned pads = 0; // a bit per gamepad slot
        int activePad = -1;
        bool fallback = false;
    };
    Active active(int player);
    std::string activeName(int player); // "Keyboard + Steam Controller", "nothing"

    // Button prompts: the device whose bindings a player's prompts show, the one of the devices it reads (in the menus,
    // for player 1, any device) pressed last; before any press a gamepad if it has one, else the keyboard.
    struct PromptDevice
    {
        bool gamepad = false;
        int slot = -1;
    };
    PromptDevice promptDevice(int player);
    // A gamepad's label for a button: an SDL_GamepadButtonLabel (1 A, 2 B, 3 X, 4 Y, 5 Cross, 6 Circle, 7 Square,
    // 8 Triangle), 0 unknown.
    int gamepadButtonLabel(int slot, int sdlButton);

    // The game is in a menu (called each menu frame; lapses a moment after the calls stop): the keyboard and every
    // connected gamepad no player reads also drive player 1's port, with player 1's bindings (the defaults for any
    // menu control left unbound), so a device setting can never lock the menus.
    void menuFrame();

    // Rebinding: the next newly pressed key (when keyboard is true) or button/axis of gamepad slot pad (-1: none).
    // Escape cancels, Delete or Backspace clears; after timeoutMs it gives up. While a capture runs the game sees
    // released pads. captureResult: 0 still waiting, else one of these with the source in *source.
    enum CaptureResult : int { CaptureWaiting = 0, CaptureKey, CapturePad, CaptureClear, CaptureCancelled, CaptureTimedOut };
    void beginCapture(bool keyboard, int pad, int timeoutMs);
    int captureResult(int *source);
    void cancelCapture();
}

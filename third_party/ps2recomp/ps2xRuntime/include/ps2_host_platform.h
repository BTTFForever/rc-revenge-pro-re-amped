#pragma once

// RC (renderer plan P0 step 3): a host window and input platform in place of raylib's window.
//
// By default the runtime opens a raylib window, presents through it and reads keys and gamepads from it; nothing
// here changes that. A program may install Hooks before PS2Runtime::initialize (RC does it for RC_RENDERER=vk, from
// a static initializer in platform/vk_host.cpp); the runtime then opens no raylib window, keeps raylib's audio
// device, paces its host loop with std::chrono instead of raylib's timer, hands each presented picture to `present`
// and reads input through these calls. The oracle and every program that installs nothing see no change.
//
// All calls are made on the thread that called PS2Runtime::initialize and run (the main thread). Codes are
// raylib's (KEY_*, MOUSE_BUTTON_*, GAMEPAD_BUTTON_*, GAMEPAD_AXIS_*), so the bindings (PS2X_BIND_*) mean the same
// keys in both modes.

#include <atomic>
#include <cstdint>

namespace ps2_host_platform
{
    struct Hooks
    {
        bool (*open)(const char *title); // the window and device; false stops initialisation
        void (*close)();
        bool (*pumpEvents)();            // once per host frame; false once the window was asked to close
        bool (*minimized)();
        bool (*focused)();
        void (*windowSize)(int *width, int *height);
        void (*toggleFullscreen)();
        // pixels: RGBA8, width * height * 4 bytes, or null when the picture has not changed since the last call;
        // width or height 0: no picture yet. aspect: the shape to show it at (ps2_host_display::aspect()).
        void (*present)(const uint8_t *pixels, uint32_t width, uint32_t height, float aspect);
        // Input as of the last pumpEvents.
        bool (*keyDown)(int key);
        bool (*keyPressed)(int key); // went down since the pumpEvents before the last one
        bool (*mouseButtonDown)(int button);
        bool (*mouseButtonPressed)(int button);
        void (*mouseDelta)(float *x, float *y);
        void (*setMouseCaptured)(bool captured); // raylib's DisableCursor (true) / EnableCursor (false)
        bool (*gamepadAvailable)(int gamepad);
        bool (*gamepadButtonDown)(int gamepad, int button);
        float (*gamepadAxis)(int gamepad, int axis); // raylib's ranges: sticks -1..1, triggers -1 (released)..1
        // Optional (may be null: always). False while the host does not show the emulated GS picture (RC_VK_PICTURE=native
        // with the native picture on screen): the host loop then skips the VU1/GIF drain, the GS latch and the read-back,
        // and calls present with no picture.
        bool (*wantsGsPicture)();
    };

    inline const Hooks *g_hooks = nullptr;
    // The host loop's last frame time in seconds (raylib's GetFrameTime while hooks are installed).
    inline std::atomic<float> g_frameSeconds{1.0f / 60.0f};

    inline void install(const Hooks *hooks) { g_hooks = hooks; }
    inline const Hooks *hooks() { return g_hooks; }
}

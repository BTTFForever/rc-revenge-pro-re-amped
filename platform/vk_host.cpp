// RC_RENDERER=vk (renderer plan P0 step 3): the host window, input and presentation on SDL3 and the vendored RHI
// (third_party/ico_rhi, Vulkan) in place of the runtime's raylib window. Installed before main through the runtime's
// host platform hooks (third_party/ps2recomp/ps2xRuntime/include/ps2_host_platform.h); unset or RC_RENDERER=gs leaves
// the runtime exactly as it was. raylib's audio device keeps playing (it needs no window).
//
// What it does: one resizable SDL3 window with a Vulkan swapchain (RHI device on the discrete GPU when there is one);
// each host frame clears it and draws the emulated GS's displayed picture (the runtime's latched presentation frame,
// the same image the raylib window shows) letterboxed at the display's aspect. This is a stand-in until the native
// renderer (P1) draws the frame itself. Keyboard, mouse and up to four gamepads are read from SDL3 and answered in
// raylib's codes, so ps2_host_input.cpp's bindings (PS2X_BIND_*), pad scripts and recordings work unchanged.
//
// Switches: RC_VK_VSYNC=0 presents without vsync (default on; the host loop is held to 60 per second either way);
// the RHI's own ICO_VK_* switches (ICO_VK_VALIDATION=1, ICO_VK_DEVICE=<index or name part>) apply.
//
// (Renderer plan P1 step 7 batch 5) RC_VK_PICTURE=native presents the native picture instead (render/live/live_host.h:
// the frame reader on the EE thread, the cycle's list, 3D and 2D, drawn through Gs2dVk). F7 switches
// between it and the emulated GS picture; the window title says which is shown. RC_VK_PRESENT_FILTER=nearest draws the
// native picture with nearest filtering (default linear, as the GS picture). Unset or "gs": exactly as before.
#include "ps2_host_platform.h"

#include "../render/live/live_host.h"

#include "rhi.h"
#include "vk/rhi_vk.h"

#include "present_frag.h"
#include "present_vert.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace
{
    // raylib's codes (raylib.h; GLFW's key numbers), kept here so this file does not need raylib's header.
    namespace rl
    {
        constexpr int KeySpace = 32, KeyApostrophe = 39, KeyComma = 44, KeyMinus = 45, KeyPeriod = 46, KeySlash = 47,
                      KeyZero = 48, KeySemicolon = 59, KeyEqual = 61, KeyA = 65, KeyLeftBracket = 91, KeyBackslash = 92,
                      KeyRightBracket = 93, KeyGrave = 96, KeyEscape = 256, KeyEnter = 257, KeyTab = 258,
                      KeyBackspace = 259, KeyInsert = 260, KeyDelete = 261, KeyRight = 262, KeyLeft = 263, KeyDown = 264,
                      KeyUp = 265, KeyPageUp = 266, KeyPageDown = 267, KeyHome = 268, KeyEnd = 269, KeyF1 = 290,
                      KeyLeftShift = 340, KeyLeftControl = 341, KeyLeftAlt = 342, KeyLeftSuper = 343,
                      KeyRightShift = 344, KeyRightControl = 345, KeyRightAlt = 346, KeyRightSuper = 347;
        constexpr int MouseLeft = 0, MouseRight = 1, MouseMiddle = 2;
        // GAMEPAD_BUTTON_*: 1..17 in this order; GAMEPAD_AXIS_*: 0..5
        enum : int
        {
            PadUnknown,
            PadLeftFaceUp,
            PadLeftFaceRight,
            PadLeftFaceDown,
            PadLeftFaceLeft,
            PadRightFaceUp,
            PadRightFaceRight,
            PadRightFaceDown,
            PadRightFaceLeft,
            PadLeftTrigger1,
            PadLeftTrigger2,
            PadRightTrigger1,
            PadRightTrigger2,
            PadMiddleLeft,
            PadMiddle,
            PadMiddleRight,
            PadLeftThumb,
            PadRightThumb,
        };
        constexpr int AxisLeftX = 0, AxisLeftY = 1, AxisRightX = 2, AxisRightY = 3, AxisLeftTrigger = 4, AxisRightTrigger = 5;
    }

    SDL_Scancode scancodeFor(int key)
    {
        if (key >= rl::KeyA && key < rl::KeyA + 26)
            return static_cast<SDL_Scancode>(SDL_SCANCODE_A + (key - rl::KeyA));
        if (key >= rl::KeyZero + 1 && key <= rl::KeyZero + 9)
            return static_cast<SDL_Scancode>(SDL_SCANCODE_1 + (key - rl::KeyZero - 1));
        if (key >= rl::KeyF1 && key < rl::KeyF1 + 12)
            return static_cast<SDL_Scancode>(SDL_SCANCODE_F1 + (key - rl::KeyF1));
        switch (key)
        {
        case rl::KeyZero: return SDL_SCANCODE_0;
        case rl::KeySpace: return SDL_SCANCODE_SPACE;
        case rl::KeyApostrophe: return SDL_SCANCODE_APOSTROPHE;
        case rl::KeyComma: return SDL_SCANCODE_COMMA;
        case rl::KeyMinus: return SDL_SCANCODE_MINUS;
        case rl::KeyPeriod: return SDL_SCANCODE_PERIOD;
        case rl::KeySlash: return SDL_SCANCODE_SLASH;
        case rl::KeySemicolon: return SDL_SCANCODE_SEMICOLON;
        case rl::KeyEqual: return SDL_SCANCODE_EQUALS;
        case rl::KeyLeftBracket: return SDL_SCANCODE_LEFTBRACKET;
        case rl::KeyBackslash: return SDL_SCANCODE_BACKSLASH;
        case rl::KeyRightBracket: return SDL_SCANCODE_RIGHTBRACKET;
        case rl::KeyGrave: return SDL_SCANCODE_GRAVE;
        case rl::KeyEscape: return SDL_SCANCODE_ESCAPE;
        case rl::KeyEnter: return SDL_SCANCODE_RETURN;
        case rl::KeyTab: return SDL_SCANCODE_TAB;
        case rl::KeyBackspace: return SDL_SCANCODE_BACKSPACE;
        case rl::KeyInsert: return SDL_SCANCODE_INSERT;
        case rl::KeyDelete: return SDL_SCANCODE_DELETE;
        case rl::KeyRight: return SDL_SCANCODE_RIGHT;
        case rl::KeyLeft: return SDL_SCANCODE_LEFT;
        case rl::KeyDown: return SDL_SCANCODE_DOWN;
        case rl::KeyUp: return SDL_SCANCODE_UP;
        case rl::KeyPageUp: return SDL_SCANCODE_PAGEUP;
        case rl::KeyPageDown: return SDL_SCANCODE_PAGEDOWN;
        case rl::KeyHome: return SDL_SCANCODE_HOME;
        case rl::KeyEnd: return SDL_SCANCODE_END;
        case rl::KeyLeftShift: return SDL_SCANCODE_LSHIFT;
        case rl::KeyLeftControl: return SDL_SCANCODE_LCTRL;
        case rl::KeyLeftAlt: return SDL_SCANCODE_LALT;
        case rl::KeyLeftSuper: return SDL_SCANCODE_LGUI;
        case rl::KeyRightShift: return SDL_SCANCODE_RSHIFT;
        case rl::KeyRightControl: return SDL_SCANCODE_RCTRL;
        case rl::KeyRightAlt: return SDL_SCANCODE_RALT;
        case rl::KeyRightSuper: return SDL_SCANCODE_RGUI;
        default: return SDL_SCANCODE_UNKNOWN;
        }
    }

    constexpr int kMaxGamepads = 4; // raylib's MAX_GAMEPADS

    struct Host
    {
        SDL_Window *window = nullptr;
        bool vsync = true;
        bool closeRequested = false;
        std::array<bool, SDL_SCANCODE_COUNT> keys{};
        std::array<bool, SDL_SCANCODE_COUNT> previousKeys{};
        uint32_t mouseButtons = 0u;
        uint32_t previousMouseButtons = 0u;
        float mouseX = 0.0f, mouseY = 0.0f;
        bool mouseCaptured = false;
        // raylib numbers gamepads by GLFW joystick slot: the lowest free one at connection.
        std::array<SDL_Gamepad *, kMaxGamepads> gamepads{};

        // presentation
        bool gpuReady = false;
        RhiBindGroupLayout layouts[3]{};
        RhiShader vertex{}, fragment{};
        RhiPipeline pipeline{};
        RhiSampler sampler{};
        RhiSampler nearest{}; // RC_VK_PRESENT_FILTER=nearest (native picture only)
        bool presentNearest = false;
        RhiTexture picture{};
        uint32_t pictureWidth = 0u, pictureHeight = 0u;
        bool pictureFilled = false; // in SHADER_READ with a picture in it
        std::array<RhiBuffer, RHI_FRAMES_IN_FLIGHT> upload{};
        std::array<uint64_t, RHI_FRAMES_IN_FLIGHT> uploadSize{};
        uint64_t presents = 0u, uploads = 0u, skipped = 0u;

        // RC_VK_PICTURE=native (step 7 batch 5)
        bool native = false;     // the live 2D renderer is running
        bool showNative = false; // F7 toggles
        uint32_t f7Presses = 0;  // F7 key-down events (not repeats) since the last present
        bool nativeShown = false; // the last present showed the native picture
        std::string title;
    };

    Host g_host;

    RhiShader makeShader(RhiShaderStage stage, const uint32_t *code, size_t size, const char *name)
    {
        RhiShaderDesc desc{};
        desc.stage = stage;
        desc.bytecode = code;
        desc.bytecodeSize = size;
        desc.entryPoint = "main";
        desc.debugName = name;
        return rhi_create_shader(&desc);
    }

    bool createGpuObjects()
    {
        static const RhiBindSlot pictureSlots[2] = {{1u, RHI_BIND_SAMPLED_TEXTURE, 1u << RHI_STAGE_FRAGMENT},
                                                    {1u, RHI_BIND_SAMPLER, 1u << RHI_STAGE_FRAGMENT}};
        const RhiBindGroupLayoutDesc group0{nullptr, 0u, "present group0"};
        const RhiBindGroupLayoutDesc group1{nullptr, 0u, "present group1"};
        const RhiBindGroupLayoutDesc group2{pictureSlots, 2u, "present picture"};
        Host &h = g_host;
        h.layouts[0] = rhi_create_bind_group_layout(&group0);
        h.layouts[1] = rhi_create_bind_group_layout(&group1);
        h.layouts[2] = rhi_create_bind_group_layout(&group2);
        h.vertex = makeShader(RHI_STAGE_VERTEX, spv_present_vert, sizeof(spv_present_vert), "present.vert");
        h.fragment = makeShader(RHI_STAGE_FRAGMENT, spv_present_frag, sizeof(spv_present_frag), "present.frag");
        RhiSamplerDesc samplerDesc{};
        samplerDesc.mag = samplerDesc.min = samplerDesc.mip = RHI_FILTER_LINEAR;
        samplerDesc.s = samplerDesc.t = RHI_WRAP_CLAMP;
        samplerDesc.maxAnisotropy = 1.0f;
        h.sampler = rhi_create_sampler(&samplerDesc);
        samplerDesc.mag = samplerDesc.min = samplerDesc.mip = RHI_FILTER_NEAREST;
        h.nearest = rhi_create_sampler(&samplerDesc);
        RhiPipelineDesc desc{};
        desc.vertex = h.vertex;
        desc.fragment = h.fragment;
        desc.layouts = h.layouts;
        desc.layoutCount = 3u;
        desc.topology = RHI_TOPO_TRIANGLE_LIST;
        desc.cullNone = true;
        desc.blend[0].enable = false;
        desc.blend[0].writeMask = 0xFu;
        desc.colorFormats[0] = rhi_swapchain_format();
        desc.colorCount = 1u;
        desc.depthFormat = RHI_FMT_UNKNOWN;
        desc.debugName = "present";
        h.pipeline = rhi_create_pipeline(&desc);
        return h.layouts[0].id && h.layouts[1].id && h.layouts[2].id && h.vertex.id && h.fragment.id && h.sampler.id &&
               h.pipeline.id;
    }

    void destroyGpuObjects()
    {
        Host &h = g_host;
        rhi_wait_idle();
        for (RhiBuffer &buffer : h.upload)
            if (buffer.id)
                rhi_destroy_buffer(buffer);
        if (h.picture.id)
            rhi_destroy_texture(h.picture);
        if (h.pipeline.id)
            rhi_destroy_pipeline(h.pipeline);
        if (h.vertex.id)
            rhi_destroy_shader(h.vertex);
        if (h.fragment.id)
            rhi_destroy_shader(h.fragment);
        if (h.sampler.id)
            rhi_destroy_sampler(h.sampler);
        if (h.nearest.id)
            rhi_destroy_sampler(h.nearest);
        for (RhiBindGroupLayout &layout : h.layouts)
            if (layout.id)
                rhi_destroy_bind_group_layout(layout);
    }

    // RC_VK_PICTURE=native: the title says which picture is shown.
    void updateTitle()
    {
        Host &h = g_host;
        if (!h.window || !rc::live::nativeEnabled())
            return;
        const std::string t = h.title + (h.showNative ? " [native picture; F7: emulated GS]"
                                                      : " [emulated GS picture; F7: native]");
        SDL_SetWindowTitle(h.window, t.c_str());
    }

    bool open(const char *title)
    {
        Host &h = g_host;
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
        {
            std::fprintf(stderr, "[vk-host] SDL_Init: %s\n", SDL_GetError());
            return false;
        }
        h.window = SDL_CreateWindow(title ? title : "PS2", 640, 480, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
        if (!h.window)
        {
            std::fprintf(stderr, "[vk-host] SDL_CreateWindow: %s\n", SDL_GetError());
            SDL_Quit();
            return false;
        }
        const char *vsync = std::getenv("RC_VK_VSYNC");
        h.vsync = !(vsync && std::string_view(vsync) == "0");
        RhiDeviceDesc device{};
        device.sdlWindow = h.window;
        device.vsync = h.vsync;
        device.debugLayers = false; // ICO_VK_VALIDATION=1 asks for the layer
        device.appName = "rc_revenge_pro";
        if (!rhi_init(&device))
        {
            std::fprintf(stderr, "[vk-host] rhi_init failed\n");
            SDL_DestroyWindow(h.window);
            h.window = nullptr;
            SDL_Quit();
            return false;
        }
        h.gpuReady = createGpuObjects();
        h.title = title ? title : "PS2";
        if (rc::live::nativeEnabled())
        {
            std::string why;
            h.native = h.gpuReady && rc::live::createGpu(&why);
            if (!h.native)
                std::fprintf(stderr, "[vk-host] RC_VK_PICTURE=native: the native renderer cannot run here (%s); showing the emulated GS picture\n",
                             why.empty() ? "presentation objects failed" : why.c_str());
            h.showNative = h.native;
            const char *filter = std::getenv("RC_VK_PRESENT_FILTER");
            h.presentNearest = filter && std::string_view(filter) == "nearest";
            updateTitle();
        }
        std::fprintf(stderr, "[vk-host] SDL %d.%d.%d (%s), adapter \"%s\", swapchain format %d, present mode %s%s\n",
                     SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
                     SDL_VERSIONNUM_MICRO(SDL_GetVersion()), SDL_GetCurrentVideoDriver(), rhi_adapter_name(),
                     static_cast<int>(rhi_swapchain_format()), rhi_present_mode_name(),
                     h.gpuReady ? "" : "; presentation objects failed, clearing only");
        return true;
    }

    void close()
    {
        Host &h = g_host;
        if (!h.window)
            return;
        if (rc::live::nativeEnabled())
        {
            rc::live::report();
            rc::live::destroyGpu();
        }
        destroyGpuObjects();
        const uint32_t errors = rhi_vk_validation_error_count();
        rhi_shutdown();
        for (SDL_Gamepad *&pad : h.gamepads)
        {
            if (pad)
                SDL_CloseGamepad(pad);
            pad = nullptr;
        }
        SDL_DestroyWindow(h.window);
        h.window = nullptr;
        SDL_Quit();
        std::fprintf(stderr, "[vk-host] closed: %llu presents, %llu picture uploads, %llu skipped; %u validation error(s)\n",
                     static_cast<unsigned long long>(h.presents), static_cast<unsigned long long>(h.uploads),
                     static_cast<unsigned long long>(h.skipped), errors);
    }

    void addGamepad(SDL_JoystickID id)
    {
        Host &h = g_host;
        for (SDL_Gamepad *pad : h.gamepads)
            if (pad && SDL_GetGamepadID(pad) == id)
                return;
        for (int i = 0; i < kMaxGamepads; ++i)
        {
            if (!h.gamepads[i])
            {
                h.gamepads[i] = SDL_OpenGamepad(id);
                if (h.gamepads[i])
                    std::fprintf(stderr, "[vk-host] gamepad %d: %s\n", i, SDL_GetGamepadName(h.gamepads[i]));
                return;
            }
        }
    }

    void removeGamepad(SDL_JoystickID id)
    {
        for (SDL_Gamepad *&pad : g_host.gamepads)
        {
            if (pad && SDL_GetGamepadID(pad) == id)
            {
                SDL_CloseGamepad(pad);
                pad = nullptr;
            }
        }
    }

    bool pumpEvents()
    {
        Host &h = g_host;
        h.previousKeys = h.keys;
        h.previousMouseButtons = h.mouseButtons;
        h.mouseX = h.mouseY = 0.0f;
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            switch (event.type)
            {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                h.closeRequested = true;
                break;
            case SDL_EVENT_MOUSE_MOTION:
                h.mouseX += event.motion.xrel;
                h.mouseY += event.motion.yrel;
                break;
            case SDL_EVENT_KEY_DOWN:
                // RC_VK_PICTURE=native's toggle: an F7 key-down event (not a repeat), named with its keyboard, so a press
                // can be told from a stray one (the keyboard state can change across a focus change).
                if (h.native && event.key.scancode == SDL_SCANCODE_F7 && !event.key.repeat)
                {
                    ++h.f7Presses;
                    const char *kb = SDL_GetKeyboardNameForID(event.key.which);
                    std::fprintf(stderr, "[vk-host] F7 key down (keyboard %u \"%s\")\n", static_cast<unsigned>(event.key.which), kb ? kb : "?");
                }
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                addGamepad(event.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                removeGamepad(event.gdevice.which);
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                std::fprintf(stderr, "[vk-host] keyboard focus %s\n", event.type == SDL_EVENT_WINDOW_FOCUS_GAINED ? "gained" : "lost");
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                if (event.window.data1 > 0 && event.window.data2 > 0)
                    rhi_resize_swapchain(static_cast<uint32_t>(event.window.data1), static_cast<uint32_t>(event.window.data2), h.vsync);
                break;
            default:
                break;
            }
        }
        int count = 0;
        const bool *state = SDL_GetKeyboardState(&count);
        for (int i = 0; i < SDL_SCANCODE_COUNT; ++i)
            h.keys[i] = i < count && state[i];
        h.mouseButtons = SDL_GetMouseState(nullptr, nullptr);
        return !h.closeRequested;
    }

    bool minimized() { return g_host.window && (SDL_GetWindowFlags(g_host.window) & SDL_WINDOW_MINIMIZED) != 0; }
    bool focused() { return g_host.window && SDL_GetKeyboardFocus() == g_host.window; }

    void windowSize(int *width, int *height)
    {
        *width = *height = 0;
        if (g_host.window)
            SDL_GetWindowSize(g_host.window, width, height);
    }

    void toggleFullscreen()
    {
        if (!g_host.window)
            return;
        const bool fullscreen = (SDL_GetWindowFlags(g_host.window) & SDL_WINDOW_FULLSCREEN) != 0;
        SDL_SetWindowFullscreenMode(g_host.window, nullptr); // borderless at the desktop's mode, as raylib's F11
        SDL_SetWindowFullscreen(g_host.window, !fullscreen);
    }

    void present(const uint8_t *pixels, uint32_t width, uint32_t height, float aspect)
    {
        Host &h = g_host;
        if (!h.window || rhi_device_lost())
            return;
        if (h.native && h.f7Presses)
        {
            if (h.f7Presses & 1u)
                h.showNative = !h.showNative;
            h.f7Presses = 0;
            updateTitle();
            std::fprintf(stderr, "[vk-host] F7: showing the %s picture\n", h.showNative ? "native" : "emulated GS");
        }
        rhi_wait_frame();
        const uint32_t slot = rhi_frame_slot();
        // RC_VK_PICTURE=native: every list the EE thread published since the last frame is drawn, whichever picture is
        // shown (the targets and textures must follow the game).
        rc::live::HostView native{};
        if (h.native)
        {
            RhiCommandList cl = rhi_begin_commands();
            native = rc::live::recordFrame(cl);
            rhi_submit(cl);
        }
        const bool showNative = h.native && h.showNative && native.valid && native.height;
        h.nativeShown = showNative;
        if (h.gpuReady && pixels && width && height && !showNative)
        {
            if (!h.picture.id || h.pictureWidth != width || h.pictureHeight != height)
            {
                std::fprintf(stderr, "[vk-host] emulated GS picture %ux%u\n", width, height);
                if (h.picture.id)
                    rhi_destroy_texture(h.picture);
                RhiTextureDesc desc{};
                desc.width = width;
                desc.height = height;
                desc.mipLevels = 1u;
                desc.format = RHI_FMT_RGBA8_UNORM;
                desc.usage = RHI_TEX_SAMPLED | RHI_TEX_COPY_DST;
                desc.debugName = "GS picture";
                h.picture = rhi_create_texture(&desc);
                h.pictureWidth = width;
                h.pictureHeight = height;
                h.pictureFilled = false;
            }
            const uint64_t bytes = static_cast<uint64_t>(width) * height * 4u;
            if (!h.upload[slot].id || h.uploadSize[slot] < bytes)
            {
                if (h.upload[slot].id)
                    rhi_destroy_buffer(h.upload[slot]);
                const RhiBufferDesc desc{bytes, RHI_BUF_COPY_SRC, RHI_MEM_UPLOAD, "GS picture upload"};
                h.upload[slot] = rhi_create_buffer(&desc);
                h.uploadSize[slot] = h.upload[slot].id ? bytes : 0u;
            }
            void *mapped = h.picture.id && h.upload[slot].id ? rhi_map_buffer(h.upload[slot]) : nullptr;
            if (mapped)
            {
                std::memcpy(mapped, pixels, bytes);
                rhi_unmap_buffer(h.upload[slot]);
                RhiCommandList cl = rhi_begin_commands();
                const RhiTextureBarrier toCopy{h.picture, h.pictureFilled ? RHI_STATE_SHADER_READ : RHI_STATE_UNDEFINED,
                                               RHI_STATE_COPY_DST};
                rhi_cmd_barrier(cl, &toCopy, 1u);
                rhi_cmd_copy_buffer_to_texture(cl, h.upload[slot], 0u, width * 4u, h.picture, 0u, RhiRect{0, 0, width, height});
                const RhiTextureBarrier toRead{h.picture, RHI_STATE_COPY_DST, RHI_STATE_SHADER_READ};
                rhi_cmd_barrier(cl, &toRead, 1u);
                rhi_submit(cl);
                h.pictureFilled = true;
                ++h.uploads;
            }
        }

        RhiTexture backbuffer = rhi_acquire_backbuffer();
        uint32_t targetWidth = 0u, targetHeight = 0u;
        if (!backbuffer.id || !rhi_swapchain_size(&targetWidth, &targetHeight) || !targetWidth || !targetHeight)
        {
            int pw = 0, ph = 0;
            SDL_GetWindowSizeInPixels(h.window, &pw, &ph);
            if (pw > 0 && ph > 0)
                rhi_resize_swapchain(static_cast<uint32_t>(pw), static_cast<uint32_t>(ph), h.vsync);
            ++h.skipped;
            return;
        }
        RhiCommandList cl = rhi_begin_commands();
        const RhiTextureBarrier toTarget{backbuffer, RHI_STATE_UNDEFINED, RHI_STATE_RENDER_TARGET};
        rhi_cmd_barrier(cl, &toTarget, 1u);
        RhiRenderPassDesc pass{};
        pass.color[0].texture = backbuffer;
        pass.color[0].load = RHI_LOAD_CLEAR;
        pass.color[0].clear[3] = 1.0f; // black, as the raylib window's ClearBackground(BLACK)
        pass.color[0].store = RHI_STORE_STORE;
        pass.colorCount = 1u;
        pass.width = targetWidth;
        pass.height = targetHeight;
        rhi_cmd_begin_render_pass(cl, &pass);
        const RhiTexture shown = showNative ? native.texture : h.picture;
        if (h.gpuReady && shown.id && (showNative || (h.pictureFilled && width && height)))
        {
            // The raylib loop's letterbox: the widest rectangle of the display's aspect, centred.
            const float screenWidth = static_cast<float>(targetWidth), screenHeight = static_cast<float>(targetHeight);
            const float shape = aspect > 0.0f ? aspect : 4.0f / 3.0f;
            float drawWidth = screenWidth, drawHeight = screenWidth / shape;
            if (drawHeight > screenHeight)
            {
                drawHeight = screenHeight;
                drawWidth = screenHeight * shape;
            }
            const float x0 = (screenWidth - drawWidth) * 0.5f, y0 = (screenHeight - drawHeight) * 0.5f;
            // The native target is 640 x 256 of which the picture is the top `height` rows: the viewport is stretched
            // down by texHeight / height and the scissor keeps the letterbox.
            const float stretch = showNative && native.texHeight > native.height ? static_cast<float>(native.texHeight) / native.height : 1.0f;
            const RhiViewport viewport{x0, y0, drawWidth, drawHeight * stretch, 0.0f, 1.0f};
            rhi_cmd_set_viewport(cl, &viewport);
            const int32_t sx = static_cast<int32_t>(x0), sy = static_cast<int32_t>(y0);
            const RhiRect scissor = showNative ? RhiRect{sx, sy, std::min<uint32_t>(targetWidth - static_cast<uint32_t>(sx), static_cast<uint32_t>(drawWidth + 0.5f)),
                                                         std::min<uint32_t>(targetHeight - static_cast<uint32_t>(sy), static_cast<uint32_t>(drawHeight + 0.5f))}
                                               : RhiRect{0, 0, targetWidth, targetHeight};
            rhi_cmd_set_scissor(cl, &scissor);
            RhiBinding bindings[2]{};
            bindings[0].slot = 1u;
            bindings[0].type = RHI_BIND_SAMPLED_TEXTURE;
            bindings[0].texture = shown;
            bindings[0].aspect = RHI_ASPECT_COLOR;
            bindings[1].slot = 1u;
            bindings[1].type = RHI_BIND_SAMPLER;
            bindings[1].sampler = showNative && h.presentNearest && h.nearest.id ? h.nearest : h.sampler;
            const RhiBindGroupDesc group0{h.layouts[0], nullptr, 0u};
            const RhiBindGroupDesc group1{h.layouts[1], nullptr, 0u};
            const RhiBindGroupDesc group2{h.layouts[2], bindings, 2u};
            rhi_cmd_set_pipeline(cl, h.pipeline);
            rhi_cmd_set_bind_group(cl, 0u, rhi_create_bind_group(&group0));
            rhi_cmd_set_bind_group(cl, 1u, rhi_create_bind_group(&group1));
            rhi_cmd_set_bind_group(cl, 2u, rhi_create_bind_group(&group2));
            rhi_cmd_draw(cl, 3u, 0u, 1u);
        }
        rhi_cmd_end_render_pass(cl);
        const RhiTextureBarrier toPresent{backbuffer, RHI_STATE_RENDER_TARGET, RHI_STATE_PRESENT};
        rhi_cmd_barrier(cl, &toPresent, 1u);
        rhi_submit(cl);
        rhi_present();
        ++h.presents;
        if (h.native)
            rc::live::afterPresent(pixels, width, height);
    }

    // The emulated GS picture is read back only while it can be shown: the GS picture is on, F7 was pressed, the native
    // picture is not ready, or a screenshot pair waits for it.
    bool wantsGsPicture()
    {
        const Host &h = g_host;
        return !h.native || !h.showNative || h.f7Presses != 0u || !h.nativeShown || rc::live::shotPending();
    }

    bool keyDown(int key)
    {
        const SDL_Scancode code = scancodeFor(key);
        return code != SDL_SCANCODE_UNKNOWN && g_host.keys[code];
    }

    bool keyPressed(int key)
    {
        const SDL_Scancode code = scancodeFor(key);
        return code != SDL_SCANCODE_UNKNOWN && g_host.keys[code] && !g_host.previousKeys[code];
    }

    uint32_t mouseMask(int button)
    {
        switch (button)
        {
        case rl::MouseLeft: return SDL_BUTTON_LMASK;
        case rl::MouseRight: return SDL_BUTTON_RMASK;
        case rl::MouseMiddle: return SDL_BUTTON_MMASK;
        default: return 0u;
        }
    }

    bool mouseButtonDown(int button) { return (g_host.mouseButtons & mouseMask(button)) != 0u; }
    bool mouseButtonPressed(int button)
    {
        const uint32_t mask = mouseMask(button);
        return (g_host.mouseButtons & mask) != 0u && (g_host.previousMouseButtons & mask) == 0u;
    }

    void mouseDelta(float *x, float *y)
    {
        *x = g_host.mouseX;
        *y = g_host.mouseY;
    }

    void setMouseCaptured(bool captured)
    {
        if (g_host.window && captured != g_host.mouseCaptured)
            SDL_SetWindowRelativeMouseMode(g_host.window, captured);
        g_host.mouseCaptured = captured;
    }

    SDL_Gamepad *gamepadAt(int gamepad)
    {
        return gamepad >= 0 && gamepad < kMaxGamepads ? g_host.gamepads[gamepad] : nullptr;
    }

    bool gamepadAvailable(int gamepad) { return gamepadAt(gamepad) != nullptr; }

    // raylib's trigger as a button: the GLFW axis (-1..1) above 0.1.
    bool triggerDown(SDL_Gamepad *pad, SDL_GamepadAxis axis)
    {
        return SDL_GetGamepadAxis(pad, axis) / 32767.0f * 2.0f - 1.0f > 0.1f;
    }

    bool gamepadButtonDown(int gamepad, int button)
    {
        SDL_Gamepad *pad = gamepadAt(gamepad);
        if (!pad)
            return false;
        switch (button)
        {
        case rl::PadLeftFaceUp: return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
        case rl::PadLeftFaceRight: return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        case rl::PadLeftFaceDown: return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        case rl::PadLeftFaceLeft: return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
        case rl::PadRightFaceUp: return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_NORTH);
        case rl::PadRightFaceRight: return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST);
        case rl::PadRightFaceDown: return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH);
        case rl::PadRightFaceLeft: return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST);
        case rl::PadLeftTrigger1: return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
        case rl::PadLeftTrigger2: return triggerDown(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
        case rl::PadRightTrigger1: return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
        case rl::PadRightTrigger2: return triggerDown(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        case rl::PadMiddleLeft: return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK);
        case rl::PadMiddle: return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_GUIDE);
        case rl::PadMiddleRight: return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START);
        case rl::PadLeftThumb: return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_STICK);
        case rl::PadRightThumb: return SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
        default: return false;
        }
    }

    float gamepadAxis(int gamepad, int axis)
    {
        SDL_Gamepad *pad = gamepadAt(gamepad);
        const bool trigger = axis == rl::AxisLeftTrigger || axis == rl::AxisRightTrigger;
        if (!pad)
            return trigger ? -1.0f : 0.0f; // raylib's answer for a missing pad
        static constexpr SDL_GamepadAxis kAxes[6] = {SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX,
                                                     SDL_GAMEPAD_AXIS_RIGHTY, SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
                                                     SDL_GAMEPAD_AXIS_RIGHT_TRIGGER};
        if (axis < 0 || axis >= 6)
            return 0.0f;
        const float value = static_cast<float>(SDL_GetGamepadAxis(pad, kAxes[axis]));
        // GLFW's ranges, which raylib passes on: sticks -1..1, triggers -1 (released) to 1 (SDL: 0..32767).
        return trigger ? value / 32767.0f * 2.0f - 1.0f : std::clamp(value / 32767.0f, -1.0f, 1.0f);
    }

    const ps2_host_platform::Hooks kHooks{
        open,
        close,
        pumpEvents,
        minimized,
        focused,
        windowSize,
        toggleFullscreen,
        present,
        keyDown,
        keyPressed,
        mouseButtonDown,
        mouseButtonPressed,
        mouseDelta,
        setMouseCaptured,
        gamepadAvailable,
        gamepadButtonDown,
        gamepadAxis,
        wantsGsPicture,
    };

    // Installed before main, so before the runtime is made. RC_RENDERER unset or "gs": the raylib window as before.
    const bool kInstalled = []
    {
        const char *renderer = std::getenv("RC_RENDERER");
        if (!renderer || !*renderer || std::string_view(renderer) == "gs")
            return false;
        if (std::string_view(renderer) != "vk")
        {
            std::fprintf(stderr, "[vk-host] RC_RENDERER=%s is not gs or vk; using gs\n", renderer);
            return false;
        }
        ps2_host_platform::install(&kHooks);
        std::fprintf(stderr, "[vk-host] RC_RENDERER=vk: SDL3 window and input, RHI presentation, raylib audio only\n");
        // RC_VK_PICTURE=native: the live 2D renderer's reader joins the capture's consumers now, before the runtime exists.
        rc::live::installFromEnvironment();
        return true;
    }();
}

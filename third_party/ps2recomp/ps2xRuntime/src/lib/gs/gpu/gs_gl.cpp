#include "gs_gl.h"
#include "runtime/gs/gs_shared_present.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <string_view>
#include <unordered_map>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace
{
    constexpr int kContextMajorVersion = 0x2091;
    constexpr int kContextMinorVersion = 0x2092;
    constexpr int kContextFlags = 0x2094;
    constexpr int kContextProfileMask = 0x9126;
    constexpr int kContextCoreProfileBit = 0x0001;

    using CreateContextAttribs = HGLRC(WINAPI *)(HDC, HGLRC, const int *);

    struct SharedSlot
    {
        GSSharedPresent::Frame frame;
        GSGL::GLsync produced = nullptr;
        GSGL::GLsync consumed = nullptr;
        GSGL::GLuint hashBuffer = 0;
        GSGL::GLuint hashDeviceBuffer = 0;
        const uint32_t *hash = nullptr;
        bool reading = false;
    };

    std::mutex s_sharedMutex;
    std::array<SharedSlot, 8> s_sharedSlots;
    GSGL::Api s_hostApi;
    HGLRC s_hostContext = nullptr;
    HDC s_hostDc = nullptr;
    PIXELFORMATDESCRIPTOR s_hostFormat{};
    int s_hostFormatIndex = 0;
    int s_readingSlot = -1;
    uint64_t s_sharedSequence = 0;
    std::atomic<bool> s_sharedActive{false};

    bool completed(const GSGL::Api &gl, GSGL::GLsync fence)
    {
        if (!fence)
            return true;
        const auto status = gl.ClientWaitSync(fence, 0u, 0u);
        return status == GSGL::kAlreadySignaled || status == GSGL::kConditionSatisfied;
    }

    void releaseReading()
    {
        if (s_readingSlot < 0)
            return;
        SharedSlot &slot = s_sharedSlots[s_readingSlot];
        slot.consumed = s_hostApi.FenceSync(GSGL::kSyncGpuCommandsComplete, 0u);
        s_hostApi.Flush();
        slot.reading = false;
        s_readingSlot = -1;
    }

    HMODULE openGlModule()
    {
        static HMODULE module = LoadLibraryA("opengl32.dll");
        return module;
    }

    void *loadProc(const char *name)
    {
        void *proc = reinterpret_cast<void *>(wglGetProcAddress(name));
        const auto value = reinterpret_cast<intptr_t>(proc);
        if (value == 0 || value == 1 || value == 2 || value == 3 || value == -1)
            proc = reinterpret_cast<void *>(GetProcAddress(openGlModule(), name));
        return proc;
    }

    template <typename T>
    bool load(T &target, const char *name, std::string &missing)
    {
        target = reinterpret_cast<T>(loadProc(name));
        if (!target)
        {
            missing += missing.empty() ? name : std::string(", ") + name;
            return false;
        }
        return true;
    }

    LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        return DefWindowProcA(window, message, wparam, lparam);
    }
}

namespace
{
    void loadContextApi(GSGL::Api &a, std::string &missing)
    {
#include "gs_gl_api_load.inl"
    }
}

namespace GSSharedPresent
{
    void CaptureHostContext()
    {
        const char *value = std::getenv("PS2X_GS_DIRECT_PRESENT");
        if (!value || *value != '1')
            return;
        const HDC dc = wglGetCurrentDC();
        const HGLRC context = wglGetCurrentContext();
        if (!dc || !context)
            return;
        std::string missing;
        GSGL::Api api;
        load(api.FenceSync, "glFenceSync", missing);
        load(api.ClientWaitSync, "glClientWaitSync", missing);
        load(api.DeleteSync, "glDeleteSync", missing);
        load(api.Flush, "glFlush", missing);
        if (!missing.empty())
        {
            std::fprintf(stderr, "[gs-present] shared presentation unavailable: %s\n", missing.c_str());
            return;
        }
        std::lock_guard<std::mutex> lock(s_sharedMutex);
        s_hostFormatIndex = GetPixelFormat(dc);
        if (!DescribePixelFormat(dc, s_hostFormatIndex, sizeof(s_hostFormat), &s_hostFormat))
            return;
        s_hostApi = api;
        s_hostContext = context;
        s_hostDc = dc;
        wglMakeCurrent(nullptr, nullptr);
    }

    void RestoreHostContext()
    {
        if (s_hostDc && s_hostContext)
            wglMakeCurrent(s_hostDc, s_hostContext);
    }

    bool Active()
    {
        return s_sharedActive.load(std::memory_order_acquire);
    }

    bool Acquire(Frame &frame)
    {
        if (!Active())
            return false;
        std::lock_guard<std::mutex> lock(s_sharedMutex);
        int newest = -1;
        uint64_t sequence = s_readingSlot >= 0 ? s_sharedSlots[s_readingSlot].frame.sequence : 0u;
        for (int i = 0; i < static_cast<int>(s_sharedSlots.size()); ++i)
        {
            const SharedSlot &slot = s_sharedSlots[i];
            if (!slot.reading && slot.frame.sequence > sequence && slot.produced && completed(s_hostApi, slot.produced))
            {
                newest = i;
                sequence = slot.frame.sequence;
            }
        }
        if (newest >= 0)
        {
            releaseReading();
            SharedSlot &slot = s_sharedSlots[newest];
            s_hostApi.DeleteSync(slot.produced);
            slot.produced = nullptr;
            if (slot.frame.hashValid)
                slot.frame.contentHash = static_cast<uint64_t>(slot.hash[0]) | (static_cast<uint64_t>(slot.hash[1]) << 32u);
            slot.reading = true;
            s_readingSlot = newest;
        }
        if (s_readingSlot < 0)
            return false;
        frame = s_sharedSlots[s_readingSlot].frame;
        return true;
    }

    void ShutdownHost()
    {
        std::lock_guard<std::mutex> lock(s_sharedMutex);
        releaseReading();
        s_sharedActive.store(false, std::memory_order_release);
        s_hostContext = nullptr;
        s_hostDc = nullptr;
    }
}

namespace GSGL
{
    bool SharedAvailable(const Api &gl)
    {
        std::lock_guard<std::mutex> lock(s_sharedMutex);
        for (const SharedSlot &slot : s_sharedSlots)
            if (!slot.reading && completed(gl, slot.produced) && completed(gl, slot.consumed))
                return true;
        return false;
    }

    bool PublishShared(const Api &gl, GLuint buffer, uint32_t width, uint32_t height, uint64_t renderSequence,
                       GLuint hashProgram, GLint hashLocation)
    {
        std::lock_guard<std::mutex> lock(s_sharedMutex);
        if (!s_sharedActive.load(std::memory_order_relaxed))
            return false;
        SharedSlot *available = nullptr;
        for (SharedSlot &slot : s_sharedSlots)
        {
            if (slot.reading || !completed(gl, slot.produced) || !completed(gl, slot.consumed))
                continue;
            if (!available || slot.frame.sequence < available->frame.sequence)
                available = &slot;
        }
        if (!available)
            return false;
        SharedSlot &slot = *available;
        if (slot.produced)
            gl.DeleteSync(slot.produced);
        if (slot.consumed)
            gl.DeleteSync(slot.consumed);
        slot.produced = slot.consumed = nullptr;
        slot.frame.hashValid = false;
        if (hashProgram)
        {
            if (!slot.hashBuffer)
            {
                gl.GenBuffers(1, &slot.hashBuffer);
                gl.BindBuffer(kShaderStorageBuffer, slot.hashBuffer);
                const GLbitfield flags = kMapReadBit | kMapPersistentBit | kMapCoherentBit;
                gl.BufferStorage(kShaderStorageBuffer, 8, nullptr, flags);
                slot.hash = static_cast<const uint32_t *>(gl.MapBufferRange(kShaderStorageBuffer, 0, 8, flags));
            }
            if (slot.hash)
            {
                static const bool deviceHash = [] {
                    const char *setting = std::getenv("PS2X_FRAME_HASH_DEVICE");
                    return !setting || std::string_view(setting) != "0";
                }();
                if (deviceHash && !slot.hashDeviceBuffer)
                {
                    gl.GenBuffers(1, &slot.hashDeviceBuffer);
                    gl.BindBuffer(kShaderStorageBuffer, slot.hashDeviceBuffer);
                    gl.BufferData(kShaderStorageBuffer, 8, nullptr, kDynamicDraw);
                }
                const GLuint output = deviceHash ? slot.hashDeviceBuffer : slot.hashBuffer;
                const uint32_t zero = 0u;
                gl.BindBuffer(kShaderStorageBuffer, output);
                gl.ClearBufferData(kShaderStorageBuffer, kR32UI, kRedInteger, kUnsignedInt, &zero);
                gl.BindBufferBase(kShaderStorageBuffer, 7u, buffer);
                gl.BindBufferBase(kShaderStorageBuffer, 10u, output);
                gl.MemBarrier(kAllBarrierBits);
                gl.UseProgram(hashProgram);
                gl.Uniform1ui(hashLocation, width * height);
                gl.DispatchCompute((width * height + 255u) / 256u, 1u, 1u);
                gl.MemBarrier(kAllBarrierBits);
                if (deviceHash)
                {
                    gl.BindBuffer(kCopyReadBuffer, output);
                    gl.BindBuffer(kCopyWriteBuffer, slot.hashBuffer);
                    gl.CopyBufferSubData(kCopyReadBuffer, kCopyWriteBuffer, 0, 0, 8);
                    gl.MemBarrier(kAllBarrierBits);
                }
                slot.frame.hashValid = true;
            }
        }
        gl.BindBuffer(kPixelUnpackBuffer, 0u);
        if (!slot.frame.texture)
            gl.GenTextures(1, &slot.frame.texture);
        gl.BindTexture(kTexture2D, slot.frame.texture);
        if (slot.frame.width != width || slot.frame.height != height)
        {
            gl.TexImage2D(kTexture2D, 0, kRgba8, width, height, 0, kRgba, kUnsignedByte, nullptr);
            gl.TexParameteri(kTexture2D, kTextureMinFilter, kLinear);
            gl.TexParameteri(kTexture2D, kTextureMagFilter, kLinear);
            gl.TexParameteri(kTexture2D, kTextureWrapS, kClampToEdge);
            gl.TexParameteri(kTexture2D, kTextureWrapT, kClampToEdge);
        }
        gl.MemBarrier(kAllBarrierBits);
        gl.BindBuffer(kPixelUnpackBuffer, buffer);
        gl.TexSubImage2D(kTexture2D, 0, 0, 0, width, height, kRgba, kUnsignedByte, nullptr);
        gl.MemBarrier(kAllBarrierBits);
        gl.BindBuffer(kPixelUnpackBuffer, 0u);
        gl.BindTexture(kTexture2D, 0u);
        slot.frame.width = width;
        slot.frame.height = height;
        slot.frame.sequence = ++s_sharedSequence;
        slot.frame.renderSequence = renderSequence;
        slot.produced = gl.FenceSync(kSyncGpuCommandsComplete, 0u);
        gl.Flush();
        return true;
    }

    void DestroyShared(const Api &gl)
    {
        std::lock_guard<std::mutex> lock(s_sharedMutex);
        s_sharedActive.store(false, std::memory_order_release);
        for (SharedSlot &slot : s_sharedSlots)
        {
            if (slot.produced)
                gl.DeleteSync(slot.produced);
            if (slot.consumed)
                gl.DeleteSync(slot.consumed);
            if (slot.frame.texture)
                gl.DeleteTextures(1, &slot.frame.texture);
            if (slot.hashBuffer)
                gl.DeleteBuffers(1, &slot.hashBuffer);
            if (slot.hashDeviceBuffer)
                gl.DeleteBuffers(1, &slot.hashDeviceBuffer);
            slot = {};
        }
        s_readingSlot = -1;
    }

    Context::~Context()
    {
        Destroy();
    }

    bool Context::Create(std::string &error)
    {
        Destroy();
        WNDCLASSA windowClass{};
        windowClass.style = CS_OWNDC;
        windowClass.lpfnWndProc = windowProc;
        windowClass.hInstance = GetModuleHandleA(nullptr);
        windowClass.lpszClassName = "PS2XGSGpuContext";
        RegisterClassA(&windowClass);
        HWND window = CreateWindowExA(0, windowClass.lpszClassName, "gs", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, nullptr, nullptr,
                                      windowClass.hInstance, nullptr);
        if (!window)
        {
            error = "CreateWindowEx failed";
            return false;
        }
        HDC dc = GetDC(window);
        PIXELFORMATDESCRIPTOR descriptor{};
        descriptor.nSize = sizeof(descriptor);
        descriptor.nVersion = 1;
        descriptor.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        descriptor.iPixelType = PFD_TYPE_RGBA;
        descriptor.cColorBits = 32;
        const int format = s_hostContext ? s_hostFormatIndex : ChoosePixelFormat(dc, &descriptor);
        if (s_hostContext)
            descriptor = s_hostFormat;
        if (format == 0 || !SetPixelFormat(dc, format, &descriptor))
        {
            ReleaseDC(window, dc);
            DestroyWindow(window);
            error = "no pixel format";
            return false;
        }
        HGLRC legacy = wglCreateContext(dc);
        if (!legacy || !wglMakeCurrent(dc, legacy))
        {
            if (legacy)
                wglDeleteContext(legacy);
            ReleaseDC(window, dc);
            DestroyWindow(window);
            error = "wglCreateContext failed";
            return false;
        }
        const auto createAttribs = reinterpret_cast<CreateContextAttribs>(wglGetProcAddress("wglCreateContextAttribsARB"));
        HGLRC context = nullptr;
        if (createAttribs)
        {
            const int attributes[] = {kContextMajorVersion, 4, kContextMinorVersion, 6, kContextProfileMask, kContextCoreProfileBit,
                                      kContextFlags, 0, 0};
            context = createAttribs(dc, s_hostContext, attributes);
            m_shared = context && s_hostContext;
            if (!context && s_hostContext)
            {
                std::fprintf(stderr, "[gs-present] WGL sharing failed (%lu); using RAM presentation\n", GetLastError());
                context = createAttribs(dc, nullptr, attributes);
            }
        }
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(legacy);
        if (!context || !wglMakeCurrent(dc, context))
        {
            if (context)
                wglDeleteContext(context);
            ReleaseDC(window, dc);
            DestroyWindow(window);
            error = "OpenGL 4.6 core context unavailable";
            return false;
        }
        m_window = window;
        m_dc = dc;
        m_context = context;

        std::string missing;
        loadContextApi(m_api, missing);
        if (!missing.empty())
        {
            error = "missing GL functions: " + missing;
            Destroy();
            return false;
        }
        if (m_shared)
        {
            s_sharedActive.store(true, std::memory_order_release);
            std::fprintf(stderr, "[gs-present] shared GPU textures active\n");
        }
        return true;
    }

    void *Context::CreateWorker() const
    {
        const auto createAttribs = reinterpret_cast<CreateContextAttribs>(wglGetProcAddress("wglCreateContextAttribsARB"));
        if (!createAttribs || !m_context || !m_dc)
            return nullptr;
        const int attributes[] = {kContextMajorVersion, 4, kContextMinorVersion, 6, kContextProfileMask, kContextCoreProfileBit,
                                  kContextFlags, 0, 0};
        return createAttribs(static_cast<HDC>(m_dc), static_cast<HGLRC>(m_context), attributes);
    }

    bool Context::MakeWorkerCurrent(void *worker) const
    {
        return worker && m_dc && wglMakeCurrent(static_cast<HDC>(m_dc), static_cast<HGLRC>(worker));
    }

    void Context::DestroyWorker(void *worker)
    {
        if (!worker)
            return;
        if (wglGetCurrentContext() == static_cast<HGLRC>(worker))
            wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(static_cast<HGLRC>(worker));
    }

    void Context::Destroy()
    {
        if (m_context)
        {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(static_cast<HGLRC>(m_context));
            m_context = nullptr;
        }
        if (m_dc && m_window)
            ReleaseDC(static_cast<HWND>(m_window), static_cast<HDC>(m_dc));
        m_dc = nullptr;
        if (m_window)
            DestroyWindow(static_cast<HWND>(m_window));
        m_window = nullptr;
        m_api = {};
        m_shared = false;
    }
}
#else
// Linux GS context through EGL, adapted from the God of War PC port
// (github.com/KIexster/god-of-war-recomp, patches/ps2recomp-gs-egl-linux.patch, GPL-3.0).
// libEGL is loaded at run time and the context has no surface.
#include <dlfcn.h>
#include <type_traits>

namespace
{
    using EGLDisplay = void *;
    using EGLConfig = void *;
    using EGLContext = void *;
    using EGLSurface = void *;
    using EGLint = int32_t;
    using EGLBoolean = unsigned int;
    using EGLenum = unsigned int;

    constexpr EGLint kEglNone = 0x3038;
    constexpr EGLint kEglRenderableType = 0x3040;
    constexpr EGLint kEglOpenGlBit = 0x0008;
    constexpr EGLint kEglSurfaceType = 0x3033;
    constexpr EGLenum kEglOpenGlApi = 0x30A2;
    constexpr EGLint kEglContextMajorVersion = 0x3098;
    constexpr EGLint kEglContextMinorVersion = 0x30FB;
    constexpr EGLint kEglContextProfileMask = 0x30FD;
    constexpr EGLint kEglContextCoreProfileBit = 0x0001;
    constexpr EGLenum kEglPlatformSurfacelessMesa = 0x31DD;

    struct Egl
    {
        void *(*GetProcAddress)(const char *) = nullptr;
        EGLDisplay (*GetDisplay)(void *) = nullptr;
        EGLDisplay (*GetPlatformDisplay)(EGLenum, void *, const intptr_t *) = nullptr;
        EGLBoolean (*Initialize)(EGLDisplay, EGLint *, EGLint *) = nullptr;
        EGLBoolean (*BindAPI)(EGLenum) = nullptr;
        EGLBoolean (*ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *) = nullptr;
        EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *) = nullptr;
        EGLBoolean (*DestroyContext)(EGLDisplay, EGLContext) = nullptr;
        EGLBoolean (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = nullptr;
        EGLContext (*GetCurrentContext)() = nullptr;
        EGLint (*GetError)() = nullptr;
        EGLDisplay display = nullptr;
        EGLConfig config = nullptr;
        bool ready = false;
    };

    Egl &egl()
    {
        static Egl e = [] {
            Egl r;
            void *lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
            if (!lib)
                return r;
            const auto sym = [lib](auto &target, const char *name) {
                target = reinterpret_cast<std::remove_reference_t<decltype(target)>>(dlsym(lib, name));
                return target != nullptr;
            };
            bool ok = sym(r.GetProcAddress, "eglGetProcAddress") && sym(r.GetDisplay, "eglGetDisplay") &&
                      sym(r.Initialize, "eglInitialize") && sym(r.BindAPI, "eglBindAPI") &&
                      sym(r.ChooseConfig, "eglChooseConfig") && sym(r.CreateContext, "eglCreateContext") &&
                      sym(r.DestroyContext, "eglDestroyContext") && sym(r.MakeCurrent, "eglMakeCurrent") &&
                      sym(r.GetCurrentContext, "eglGetCurrentContext") && sym(r.GetError, "eglGetError");
            sym(r.GetPlatformDisplay, "eglGetPlatformDisplay");
            if (!ok)
                return r;
            // Prefer Mesa's surfaceless platform, then the default display.
            if (r.GetPlatformDisplay)
                r.display = r.GetPlatformDisplay(kEglPlatformSurfacelessMesa, nullptr, nullptr);
            EGLint major = 0, minor = 0;
            if (!r.display || !r.Initialize(r.display, &major, &minor))
            {
                r.display = r.GetDisplay(nullptr);
                if (!r.display || !r.Initialize(r.display, &major, &minor))
                    return r;
            }
            if (!r.BindAPI(kEglOpenGlApi))
                return r;
            const EGLint configAttributes[] = {kEglRenderableType, kEglOpenGlBit, kEglSurfaceType, 0, kEglNone};
            EGLint count = 0;
            if (!r.ChooseConfig(r.display, configAttributes, &r.config, 1, &count) || count == 0)
                r.config = nullptr; // EGL_KHR_no_config_context
            r.ready = true;
            return r;
        }();
        return e;
    }

    EGLContext createEglContext(EGLContext share)
    {
        Egl &e = egl();
        if (!e.ready)
            return nullptr;
        for (const EGLint minor : {6, 5, 3})
        {
            const EGLint attributes[] = {kEglContextMajorVersion, 4, kEglContextMinorVersion, minor,
                                         kEglContextProfileMask, kEglContextCoreProfileBit, kEglNone};
            e.BindAPI(kEglOpenGlApi); // the bound API is per thread
            if (EGLContext context = e.CreateContext(e.display, e.config, share, attributes))
                return context;
        }
        return nullptr;
    }

    // On NVIDIA, EGL contexts can't be made current in a process whose window has a GLX context (the X11 window here),
    // so there the renderer's context is GLX as well. It lives in an off-screen pbuffer on its own X connection.
    struct Glx
    {
        void *(*OpenDisplay)(const char *) = nullptr;
        void *(*GetProcAddress)(const unsigned char *) = nullptr;
        void **(*ChooseFBConfig)(void *, int, const int *, int *) = nullptr;
        void *(*CreatePbuffer)(void *, void *, const int *) = nullptr;
        int (*MakeContextCurrent)(void *, void *, void *, void *) = nullptr;
        void (*DestroyContext)(void *, void *) = nullptr;
        void *(*GetCurrentContext)() = nullptr;
        void *(*CreateContextAttribs)(void *, void *, void *, int, const int *) = nullptr;
        void *display = nullptr;
        void *config = nullptr;
        void *pbuffer = nullptr;
        bool active = false;
        std::mutex mutex;
    };

    Glx &glx()
    {
        static Glx g;
        return g;
    }

    bool startGlx()
    {
        Glx &g = glx();
        if (g.display)
            return true;
        void *x11 = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
        void *lib = dlopen("libGL.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!x11 || !lib)
            return false;
        const auto from = [](void *library, auto &target, const char *name) {
            target = reinterpret_cast<std::remove_reference_t<decltype(target)>>(dlsym(library, name));
            return target != nullptr;
        };
        int (*initThreads)() = nullptr;
        if (!from(x11, g.OpenDisplay, "XOpenDisplay") || !from(x11, initThreads, "XInitThreads") ||
            !from(lib, g.GetProcAddress, "glXGetProcAddressARB") || !from(lib, g.ChooseFBConfig, "glXChooseFBConfig") ||
            !from(lib, g.CreatePbuffer, "glXCreatePbuffer") || !from(lib, g.MakeContextCurrent, "glXMakeContextCurrent") ||
            !from(lib, g.DestroyContext, "glXDestroyContext") || !from(lib, g.GetCurrentContext, "glXGetCurrentContext"))
            return false;
        g.CreateContextAttribs = reinterpret_cast<decltype(g.CreateContextAttribs)>(
            g.GetProcAddress(reinterpret_cast<const unsigned char *>("glXCreateContextAttribsARB")));
        if (!g.CreateContextAttribs)
            return false;
        initThreads();
        void *display = g.OpenDisplay(nullptr);
        if (!display)
            return false;
        const int configAttributes[] = {0x8010 /* DRAWABLE_TYPE */, 0x4 /* PBUFFER_BIT */, 0x8011 /* RENDER_TYPE */, 1 /* RGBA_BIT */, 0};
        int count = 0;
        void **configs = g.ChooseFBConfig(display, 0, configAttributes, &count);
        if (!configs || count == 0)
            return false;
        g.config = configs[0];
        const int pbufferAttributes[] = {0x8041 /* WIDTH */, 1, 0x8040 /* HEIGHT */, 1, 0};
        g.pbuffer = g.CreatePbuffer(display, g.config, pbufferAttributes);
        if (!g.pbuffer)
            return false;
        g.display = display;
        return true;
    }

    void *createGlxContext(void *share)
    {
        Glx &g = glx();
        std::lock_guard<std::mutex> lock(g.mutex);
        for (const int minor : {6, 5, 3})
        {
            const int attributes[] = {0x2091 /* MAJOR */, 4, 0x2092 /* MINOR */, minor, 0x9126 /* PROFILE_MASK */, 1 /* CORE */, 0};
            if (void *context = g.CreateContextAttribs(g.display, g.config, share, 1, attributes))
                return context;
        }
        return nullptr;
    }

    bool makeGlxCurrent(void *context)
    {
        Glx &g = glx();
        std::lock_guard<std::mutex> lock(g.mutex);
        return g.MakeContextCurrent(g.display, context ? g.pbuffer : nullptr, context ? g.pbuffer : nullptr, context) != 0;
    }

    void *loadProc(const char *name)
    {
        static void *gl = dlopen("libOpenGL.so.0", RTLD_NOW | RTLD_LOCAL);
        if (glx().active)
        {
            void *direct = gl ? dlsym(gl, name) : nullptr;
            return direct ? direct : glx().GetProcAddress(reinterpret_cast<const unsigned char *>(name));
        }
        void *proc = egl().GetProcAddress ? egl().GetProcAddress(name) : nullptr;
        if (!proc && gl)
            proc = dlsym(gl, name);
        return proc;
    }

    template <typename T>
    bool load(T &target, const char *name, std::string &missing)
    {
        target = reinterpret_cast<T>(loadProc(name));
        if (!target)
        {
            missing += missing.empty() ? name : std::string(", ") + name;
            return false;
        }
        return true;
    }
}

namespace
{
    void loadContextApi(GSGL::Api &a, std::string &missing)
    {
#include "gs_gl_api_load.inl"
    }
}


namespace
{
    // Skips OpenGL calls that would not change anything. The renderer sets all of its state for every draw, tens of
    // thousands of times a second, and NVIDIA's driver does real work for each call even when the value is unchanged.
    // One context per thread, so the record of what is set is per thread and starts blank whenever a context is made current.
    namespace statecache
    {
        using namespace GSGL;
        using GLSL = GSGL::Api;
        constexpr GLenum kTexture2D = 0x0DE1, kFramebuffer = 0x8D40, kReadFramebuffer = 0x8CA8, kDrawFramebuffer = 0x8CA9, kElementArray = 0x8893;

        GLSL real;

        struct TextureParams
        {
            GLint value[4] = {-1, -1, -1, -1};
        };

        struct State
        {
            int64_t readFb = -1, drawFb = -1, program = -1, vao = -1;
            GLint viewport[4] = {-1, -1, -1, -1}, scissor[4] = {-1, -1, -1, -1};
            bool viewportKnown = false, scissorKnown = false;
            int64_t depthFunc = -1, depthMask = -1, colorMask = -1;
            GLenum blendEquation[2] = {0, 0}, blendFunc[4] = {0, 0, 0, 0};
            float blendColor[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
            bool blendColorKnown = false;
            int64_t activeUnit = -1;
            int64_t texture2d[32];
            std::unordered_map<GLenum, int64_t> buffers;
            std::array<std::pair<GLenum, bool>, 16> caps{};
            int capCount = 0;
            std::unordered_map<GLuint, TextureParams> params;

            State()
            {
                for (int64_t &t : texture2d)
                    t = -1;
            }
        };

        thread_local State t_state;

        void reset() { t_state = State{}; }

        int parameterSlot(GLenum name)
        {
            switch (name)
            {
            case 0x2801: return 0; // MIN_FILTER
            case 0x2800: return 1; // MAG_FILTER
            case 0x2802: return 2; // WRAP_S
            case 0x2803: return 3; // WRAP_T
            default: return -1;
            }
        }

        void BindFramebuffer(GLenum target, GLuint id)
        {
            State &s = t_state;
            const int64_t value = id;
            if (target == kFramebuffer)
            {
                if (s.readFb == value && s.drawFb == value)
                    return;
                s.readFb = s.drawFb = value;
            }
            else if (target == kReadFramebuffer)
            {
                if (s.readFb == value)
                    return;
                s.readFb = value;
            }
            else if (target == kDrawFramebuffer)
            {
                if (s.drawFb == value)
                    return;
                s.drawFb = value;
            }
            real.BindFramebuffer(target, id);
        }

        void Viewport(GLint x, GLint y, GLsizei w, GLsizei h)
        {
            State &s = t_state;
            if (s.viewportKnown && s.viewport[0] == x && s.viewport[1] == y && s.viewport[2] == w && s.viewport[3] == h)
                return;
            s.viewportKnown = true;
            s.viewport[0] = x, s.viewport[1] = y, s.viewport[2] = w, s.viewport[3] = h;
            real.Viewport(x, y, w, h);
        }

        void Scissor(GLint x, GLint y, GLsizei w, GLsizei h)
        {
            State &s = t_state;
            if (s.scissorKnown && s.scissor[0] == x && s.scissor[1] == y && s.scissor[2] == w && s.scissor[3] == h)
                return;
            s.scissorKnown = true;
            s.scissor[0] = x, s.scissor[1] = y, s.scissor[2] = w, s.scissor[3] = h;
            real.Scissor(x, y, w, h);
        }

        void setCap(GLenum cap, bool on)
        {
            State &s = t_state;
            for (int i = 0; i < s.capCount; ++i)
            {
                if (s.caps[i].first != cap)
                    continue;
                if (s.caps[i].second == on)
                    return;
                s.caps[i].second = on;
                if (on)
                    real.Enable(cap);
                else
                    real.Disable(cap);
                return;
            }
            if (s.capCount < static_cast<int>(s.caps.size()))
                s.caps[s.capCount++] = {cap, on};
            if (on)
                real.Enable(cap);
            else
                real.Disable(cap);
        }

        void Enable(GLenum cap) { setCap(cap, true); }
        void Disable(GLenum cap) { setCap(cap, false); }

        void DepthFunc(GLenum func)
        {
            if (t_state.depthFunc == func)
                return;
            t_state.depthFunc = func;
            real.DepthFunc(func);
        }

        void DepthMask(unsigned char mask)
        {
            if (t_state.depthMask == mask)
                return;
            t_state.depthMask = mask;
            real.DepthMask(mask);
        }

        void ColorMask(unsigned char r, unsigned char g, unsigned char b, unsigned char a)
        {
            const int64_t value = r | g << 8 | b << 16 | a << 24;
            if (t_state.colorMask == value)
                return;
            t_state.colorMask = value;
            real.ColorMask(r, g, b, a);
        }

        void BlendEquationSeparate(GLenum rgb, GLenum alpha)
        {
            State &s = t_state;
            if (s.blendEquation[0] == rgb && s.blendEquation[1] == alpha)
                return;
            s.blendEquation[0] = rgb, s.blendEquation[1] = alpha;
            real.BlendEquationSeparate(rgb, alpha);
        }

        void BlendFuncSeparate(GLenum a, GLenum b, GLenum c, GLenum d)
        {
            State &s = t_state;
            if (s.blendFunc[0] == a && s.blendFunc[1] == b && s.blendFunc[2] == c && s.blendFunc[3] == d)
                return;
            s.blendFunc[0] = a, s.blendFunc[1] = b, s.blendFunc[2] = c, s.blendFunc[3] = d;
            real.BlendFuncSeparate(a, b, c, d);
        }

        void BlendColor(float r, float g, float b, float a)
        {
            State &s = t_state;
            if (s.blendColorKnown && s.blendColor[0] == r && s.blendColor[1] == g && s.blendColor[2] == b && s.blendColor[3] == a)
                return;
            s.blendColorKnown = true;
            s.blendColor[0] = r, s.blendColor[1] = g, s.blendColor[2] = b, s.blendColor[3] = a;
            real.BlendColor(r, g, b, a);
        }

        void UseProgram(GLuint program)
        {
            if (t_state.program == program)
                return;
            t_state.program = program;
            real.UseProgram(program);
        }

        void ActiveTexture(GLenum unit)
        {
            if (t_state.activeUnit == unit)
                return;
            t_state.activeUnit = unit;
            real.ActiveTexture(unit);
        }

        int currentUnit()
        {
            const int64_t unit = t_state.activeUnit < 0 ? 0 : t_state.activeUnit - 0x84C0;
            return unit >= 0 && unit < 32 ? static_cast<int>(unit) : -1;
        }

        void BindTexture(GLenum target, GLuint id)
        {
            if (target == kTexture2D && t_state.activeUnit >= 0)
            {
                const int unit = currentUnit();
                if (unit >= 0)
                {
                    if (t_state.texture2d[unit] == id)
                        return;
                    t_state.texture2d[unit] = id;
                }
            }
            real.BindTexture(target, id);
        }

        void TexParameteri(GLenum target, GLenum name, GLint value)
        {
            const int slot = parameterSlot(name);
            const int unit = currentUnit();
            if (target == kTexture2D && slot >= 0 && unit >= 0 && t_state.activeUnit >= 0 && t_state.texture2d[unit] > 0)
            {
                TextureParams &params = t_state.params[static_cast<GLuint>(t_state.texture2d[unit])];
                if (params.value[slot] == value)
                    return;
                params.value[slot] = value;
            }
            real.TexParameteri(target, name, value);
        }

        void BindVertexArray(GLuint vao)
        {
            if (t_state.vao == vao)
                return;
            t_state.vao = vao;
            real.BindVertexArray(vao);
        }

        void BindBuffer(GLenum target, GLuint id)
        {
            if (target != kElementArray) // That binding belongs to the vertex array.
            {
                const auto found = t_state.buffers.find(target);
                if (found != t_state.buffers.end() && found->second == id)
                    return;
                t_state.buffers[target] = id;
            }
            real.BindBuffer(target, id);
        }

        void BindBufferRange(GLenum target, GLuint index, GLuint id, GLintptr offset, GLsizeiptr size)
        {
            t_state.buffers[target] = id; // Also binds the general target.
            real.BindBufferRange(target, index, id, offset, size);
        }

        void BindBufferBase(GLenum target, GLuint index, GLuint id)
        {
            t_state.buffers[target] = id;
            real.BindBufferBase(target, index, id);
        }

        // Deleting something that is bound silently unbinds it, and its name can come back for a new object.
        void DeleteTextures(GLsizei count, const GLuint *ids)
        {
            State &s = t_state;
            for (GLsizei i = 0; i < count; ++i)
            {
                s.params.erase(ids[i]);
                for (int64_t &bound : s.texture2d)
                    if (bound == ids[i])
                        bound = -1;
            }
            real.DeleteTextures(count, ids);
        }

        void DeleteFramebuffers(GLsizei count, const GLuint *ids)
        {
            t_state.readFb = t_state.drawFb = -1;
            real.DeleteFramebuffers(count, ids);
        }

        void DeleteBuffers(GLsizei count, const GLuint *ids)
        {
            t_state.buffers.clear();
            real.DeleteBuffers(count, ids);
        }

        void DeleteVertexArrays(GLsizei count, const GLuint *ids)
        {
            t_state.vao = -1;
            real.DeleteVertexArrays(count, ids);
        }

        void DeleteProgram(GLuint program)
        {
            t_state.program = -1;
            real.DeleteProgram(program);
        }

        void install(GLSL &api)
        {
            static std::once_flag once;
            std::call_once(once, [&] { real = api; });
            reset();
#define RC_CACHE(name) \
    if (api.name)      \
        api.name = name
            RC_CACHE(BindFramebuffer);
            RC_CACHE(Viewport);
            RC_CACHE(Scissor);
            RC_CACHE(Enable);
            RC_CACHE(Disable);
            RC_CACHE(DepthFunc);
            RC_CACHE(DepthMask);
            RC_CACHE(ColorMask);
            RC_CACHE(BlendEquationSeparate);
            RC_CACHE(BlendFuncSeparate);
            RC_CACHE(BlendColor);
            RC_CACHE(UseProgram);
            RC_CACHE(ActiveTexture);
            RC_CACHE(BindTexture);
            RC_CACHE(TexParameteri);
            RC_CACHE(BindVertexArray);
            RC_CACHE(BindBuffer);
            RC_CACHE(BindBufferRange);
            RC_CACHE(BindBufferBase);
            RC_CACHE(DeleteTextures);
            RC_CACHE(DeleteFramebuffers);
            RC_CACHE(DeleteBuffers);
            RC_CACHE(DeleteVertexArrays);
            RC_CACHE(DeleteProgram);
#undef RC_CACHE
        }
    }
}

namespace GSGL
{
    bool PublishShared(const Api &, GLuint, uint32_t, uint32_t, uint64_t, GLuint, GLint) { return false; }
    bool SharedAvailable(const Api &) { return false; }
    void DestroyShared(const Api &) {}

    Context::~Context()
    {
        Destroy();
    }

    // The context is not shared with the window, so presentation reads frames back to RAM.
    bool Context::Create(std::string &error)
    {
        Destroy();
        Egl &e = egl();
        EGLContext context = e.ready ? createEglContext(nullptr) : nullptr;
        if (context && e.MakeCurrent(e.display, nullptr, nullptr, context))
        {
            m_dc = e.display;
            m_context = context;
        }
        else
        {
            const int eglError = e.ready ? e.GetError() : 0;
            if (context)
                e.DestroyContext(e.display, context);
            void *glxContext = startGlx() ? createGlxContext(nullptr) : nullptr;
            if (!glxContext || !makeGlxCurrent(glxContext))
            {
                error = "OpenGL 4.x core context unavailable (EGL error " + std::to_string(eglError) + ", GLX fallback failed)";
                return false;
            }
            glx().active = true;
            m_dc = glx().display;
            m_context = glxContext;
        }
        std::string missing;
        loadContextApi(m_api, missing);
        statecache::install(m_api);
        if (!missing.empty())
        {
            error = "missing GL functions: " + missing;
            Destroy();
            return false;
        }
        if (m_api.GetString)
            std::fprintf(stderr, "[gs-gl] EGL: %s / %s\n", reinterpret_cast<const char *>(m_api.GetString(kRenderer)),
                         reinterpret_cast<const char *>(m_api.GetString(kVersion)));
        return true;
    }

    void *Context::CreateWorker() const
    {
        if (glx().active)
            return m_context ? createGlxContext(m_context) : nullptr;
        return m_context ? createEglContext(m_context) : nullptr;
    }

    bool Context::MakeWorkerCurrent(void *worker) const
    {
        statecache::reset();
        if (glx().active)
            return worker && makeGlxCurrent(worker);
        return worker && m_dc && egl().MakeCurrent(m_dc, nullptr, nullptr, worker);
    }

    void Context::DestroyWorker(void *worker)
    {
        if (!worker)
            return;
        if (glx().active)
        {
            if (glx().GetCurrentContext() == worker)
                makeGlxCurrent(nullptr);
            std::lock_guard<std::mutex> lock(glx().mutex);
            glx().DestroyContext(glx().display, worker);
            return;
        }
        Egl &e = egl();
        if (e.GetCurrentContext() == worker)
            e.MakeCurrent(e.display, nullptr, nullptr, nullptr);
        e.DestroyContext(e.display, worker);
    }

    void Context::Destroy()
    {
        if (m_context && glx().active)
        {
            if (glx().GetCurrentContext() == m_context)
                makeGlxCurrent(nullptr);
            std::lock_guard<std::mutex> lock(glx().mutex);
            glx().DestroyContext(glx().display, m_context);
            m_context = nullptr;
        }
        else if (m_context)
        {
            Egl &e = egl();
            if (e.GetCurrentContext() == m_context)
                e.MakeCurrent(e.display, nullptr, nullptr, nullptr);
            e.DestroyContext(e.display, m_context);
            m_context = nullptr;
        }
        m_dc = nullptr;
        m_window = nullptr;
        m_api = {};
        m_shared = false;
    }
}
namespace GSSharedPresent
{
    void CaptureHostContext() {}
    void RestoreHostContext() {}
    bool Active() { return false; }
    bool Acquire(Frame &) { return false; }
    void ShutdownHost() {}
}
#endif

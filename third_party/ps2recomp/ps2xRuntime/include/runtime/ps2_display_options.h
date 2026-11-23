#pragma once

namespace ps2_host_display
{
    void setAspect(float aspect);
    float aspect();
    void setFitWindow(bool enabled);
    void setStartMaximized(bool enabled);
    void setHeadless(bool enabled);
    // The window's first size (0: the default 640x480).
    void setInitialWindowSize(int width, int height);
    // From any thread; the window loop applies them: true fullscreen on the current monitor (or back to a window),
    // and the window loop's frame cap.
    void requestFullscreen(bool fullscreen);
    void requestTargetFps(int fps);
    // The window's swap interval (0 off, 1 v-sync, -1 adaptive: late swaps tear instead of waiting, where the driver
    // has GLX_EXT_swap_control_tear, else 1), applied by the window loop.
    void requestSwapInterval(int interval);
    // Variable refresh: the window loop presents each new picture when it is ready instead of at its own fixed rate.
    void setVariableRefresh(bool enabled);
    // The current monitor's refresh rate as the window loop last saw it (0 until then).
    int monitorRefreshHz();
}

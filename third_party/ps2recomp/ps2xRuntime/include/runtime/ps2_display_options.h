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
}

#pragma once

namespace ps2_host_display
{
    void setAspect(float aspect);
    float aspect();
    void setFitWindow(bool enabled);
    void setStartMaximized(bool enabled);
    void setHeadless(bool enabled);
    // exp/quick-dirty (EXPERIMENT.md): the window's first size (0 = the default 640x480), presentation keyed on the
    // game's buffer flips as well as on vertical blanks (for interpolated in-between pictures), and a flip notice.
    void setInitialWindowSize(int width, int height);
    void setPresentOnFlip(bool enabled);
    void notifyFlip();
}

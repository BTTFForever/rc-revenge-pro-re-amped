// The enhanced recomp's settings (README "Enhanced recomp"): kept in a settings file, changed in the game's own
// Game Options > Enhancements menu (rc_enhance_menu.cpp) and read live by the code they affect.
//
// File: $XDG_CONFIG_HOME/rc-revenge-pro/settings.ini (else ~/.config/...), lines of key=value:
//   aspect=4:3|16:9|16:10|21:9   hud=corrected|stretched   fullscreen=0|1   scale=1..6
//   race_fps=60|90|120|144|165|240   smooth_motion=0|1   vsync=off|on|adaptive   vrr=0|1
//   p1_device=auto|keyboard|pad1..pad4|keyboard+pad1|keyboard+pad2|none, p1_key_cross=F, p1_pad_cross=a, ... (players 1
//   and 2; runtime/ps2_input_bindings.h), controls_version=2 (older files: the old default devices become auto)
// An environment variable, where one exists, wins over the file for that run (RC_ASPECT / RC_WIDESCREEN,
// RC_HUD_ANCHOR, PS2X_FULLSCREEN, PS2X_GS_HW_SCALE, PS2X_REFRESH_HZ, RC_INTERP, PS2X_VSYNC, PS2X_VRR,
// PS2X_PAD1_DEVICE, PS2X_PAD2_DEVICE, PS2X_BIND_<button>), so test scripts keep working.
#pragma once

#include <cstdint>

namespace rc::settings {

enum Vsync : int { VsyncOff = 0, VsyncOn, VsyncAdaptive };
enum Aspect : int { Aspect4x3 = 0, Aspect16x9, Aspect16x10, Aspect21x9, AspectCount };
inline constexpr int kRaceFps[] = {60, 90, 120, 144, 165, 240};
inline constexpr int kRaceFpsCount = 6;

struct Settings
{
    int aspect = Aspect4x3;
    bool hudCorrected = true; // HUD corners pinned and menus pillarboxed in widescreen (else stretched)
    bool fullscreen = false;
    int scale = 2;            // internal resolution (applies at the next start)
    int raceFps = 0;          // index into kRaceFps
    bool smoothMotion = true; // cars and cameras drawn between physics steps
    int vsync = VsyncOff;     // the window's swap interval
    bool vrr = false;         // variable refresh: each new picture presented when it is ready, races capped below the screen's rate
};

const Settings &get();
void set(const Settings &s); // applies what can change live, then saves the file

// What the code reads.
float aspectRatio();                      // presented aspect: 4/3, 16/9, ...
inline bool widescreen() { return get().aspect != Aspect4x3; }
float hscale();                           // (4/3) / aspect: the projection's x scale
inline bool hudAnchor() { return widescreen() && get().hudCorrected; }
inline bool uiPillarbox() { return widescreen() && get().hudCorrected; }
inline bool smoothMotion() { return get().smoothMotion; }
int raceRefreshHz();                      // races' frame rate (60: the original); with VRR, below the screen's rate

const char *filePath();
// The controls (runtime/ps2_input_bindings.h: p1_/p2_ lines of the same file) changed: saves the file.
void saveControls();

} // namespace rc::settings

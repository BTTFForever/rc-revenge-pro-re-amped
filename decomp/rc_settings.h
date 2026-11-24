// The enhanced recomp's settings (README "Enhanced recomp"): kept in a settings file, changed in the game's own
// Game Options > Display page (rc_enhance_menu.cpp) and read live by the code they affect.
//
// File: $XDG_CONFIG_HOME/rc-revenge-pro/settings.ini (else ~/.config/...), lines of key=value:
//   aspect=4:3|16:9|16:10|21:9   hud=corrected|stretched   fullscreen=0|1   scale=1..6
//   race_fps=60|90|120|144|165|240   smooth_motion=0|1   vsync=off|on|adaptive   vrr=0|1
//   hud_scale=50..100   menu_scale=50..100 (percent, steps of 10)   button_prompts=ps2|match|off
//   split=horizontal|vertical (two-player races: player 1 above player 2, or left and right; rc_split.h)
//   cars_single=0|2..12, cars_multi=0|2..12 (cars in a race, Game Setup's Number of Cars; 0: the memory card's; rc_cars.h)
//   p1_device=auto|keyboard|pad1..pad4|keyboard+pad1|keyboard+pad2|none, p1_key_cross=F, p1_pad_cross=a, ... (players 1
//   and 2; runtime/ps2_input_bindings.h), controls_version=2 (older files: the old default devices become auto)
// An environment variable, where one exists, wins over the file for that run (RC_ASPECT / RC_WIDESCREEN,
// RC_HUD_ANCHOR, RC_HUD_SCALE, RC_MENU_SCALE, RC_BUTTON_PROMPTS, RC_SPLIT, RC_CARS_SINGLE, RC_CARS_MULTI, PS2X_FULLSCREEN, PS2X_GS_HW_SCALE, PS2X_REFRESH_HZ, RC_INTERP, PS2X_VSYNC, PS2X_VRR,
// PS2X_PAD1_DEVICE, PS2X_PAD2_DEVICE, PS2X_BIND_<button>), so test scripts keep working.
#pragma once

#include <cstdint>

namespace rc::settings {

enum Vsync : int { VsyncOff = 0, VsyncOn, VsyncAdaptive };
enum Aspect : int { Aspect4x3 = 0, Aspect16x9, Aspect16x10, Aspect21x9, AspectCount };
inline constexpr int kRaceFps[] = {60, 90, 120, 144, 165, 240};
inline constexpr int kRaceFpsCount = 6;
// HUD and menu scale steps (percent): the 2D shrunk toward its anchors, for big high-resolution screens.
inline constexpr int kUiScales[] = {100, 90, 80, 70, 60, 50};
inline constexpr int kUiScaleCount = 6;
// Button prompts in the menus' texts: the PS2's glyphs, what the controls are bound to, or none.
enum Prompts : int { PromptsPs2 = 0, PromptsMatch, PromptsOff, PromptsCount };
// Two-player split screen: the PS2's halves one above the other, or side by side.
enum Split : int { SplitHorizontal = 0, SplitVertical, SplitCount };

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
    int hudScale = 0;         // index into kUiScales: the race HUD
    int menuScale = 0;        // index into kUiScales: the menus (front end, pause menu, results)
    int prompts = PromptsPs2; // button prompts
    int split = SplitHorizontal; // two-player split screen
    int carsSingle = 0;       // cars in a single-player race, 2..12 (rc_cars.h); 0: the game's own (memory card) count
    int carsMulti = 0;        // cars in a two-player race, 2..12; 0: the game's own
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
inline float hudScale() { return static_cast<float>(kUiScales[get().hudScale]) / 100.0f; }
inline float menuScale() { return static_cast<float>(kUiScales[get().menuScale]) / 100.0f; }
inline int prompts() { return get().prompts; }
inline bool splitVertical() { return get().split == SplitVertical; }
int raceRefreshHz();                      // races' frame rate (60: the original); with VRR, below the screen's rate

const char *filePath();
// The controls (runtime/ps2_input_bindings.h: p1_/p2_ lines of the same file) changed: saves the file.
void saveControls();

} // namespace rc::settings

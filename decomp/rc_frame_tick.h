// Enhancement: the game's logic on video-rate ticks while pictures are drawn faster (rc_frame_tick.cpp).
//
// The race and front-end loops step much of their logic once per pass (menus, slider repeat, music fades, the effects
// drawn with ageing particles, animated textures, ...), and a pass lasts one emulated vblank. When the vblank runs
// faster than the video mode (Game Options > Display: Frame rate, above 60 USA / 50 Europe), a host clock says which
// passes are logic ticks (one per video-mode period: 16.68 ms USA, 20 ms Europe) and which only draw a picture between
// two ticks. The loops call beginPass() at the top of each pass and skip their per-pass logic on the others.
#pragma once

#include <cstdint>

namespace rc::frametick {

// Which loop has the vblank faster than the video mode.
enum class Owner { None, Race, Front };

// Sets the emulated vblank to the frame-rate setting's (fast) or back to the video mode's. The race goes fast whenever
// the setting is above the video rate; the front end also needs Smooth motion (its extra pictures are blended).
void setFast(Owner owner, bool fast);
// The Display page changed the settings: the owner re-applies its rate.
void reapply();
Owner owner();
bool fast();                   // the vblank is faster than the video mode now
uint32_t videoPeriodUs();      // the video mode's vblank period (16683 or 20000)
uint32_t raceHz();             // the frame-rate setting (Hz) when it is above the video rate, else 0

// The tick clock. beginPass() at the top of a loop pass: true for a logic tick (every pass when not fast, the first
// fast pass, and then one pass per video period; never two ticks in one pass). blend(): how far this pass is from
// the last tick to the next, 0..1.
bool beginPass();
bool logicPass();              // the current pass is a tick (true outside the fast loops)
float blend();
void resync();                 // the next pass is a tick (after a rate change or a pause in the passes)
uint64_t ticks();              // ticks since start (diagnostics)
uint64_t passes();

// RC_FRAME_TICK=0 turns the race's tick gating off (each pass steps its logic, as before; for comparisons).
bool raceGating();

// The vblank interrupt's frame counter (VSyncCallbackFunction): true when a video-mode field has elapsed, so the
// counter keeps the video rate however fast the vblank runs.
bool videoFieldElapsed();

// RC_TICK_LOG=1: once a second, passes, ticks, animated-texture frames, vblank-counter fields; and music fade and
// volume changes as they happen. For the measurements in PERF_INVESTIGATION.md.
void logSecond();
void countRainStep();          // Vfx3Rain / Vfx3Glint stepped their drops / glints (for the log)
void countGlintStep();

} // namespace rc::frametick

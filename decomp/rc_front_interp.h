// Enhancement: the front end at the high frame rate (FRONTEND_INTERP_DESIGN.md, option A; rc_front_interp.cpp).
//
// FrontEndProcess runs its logic on video-rate ticks (rc_frame_tick.h). Each tick keeps the drawn state (camera 0,
// the scenery objects, the turntable's cars) as "current", the last tick's as "previous"; every pass draws the 3D
// with that state blended by how far the pass is between ticks, and puts the real state back before any logic runs.
// Passes between ticks redraw the 3D only and send the tick's 2D (its part of the GS packet, K1) again.
#pragma once

#include <cstdint>

namespace rc::frontinterp {

// The front end may go fast: Smooth motion on, frame rate above the video rate, RC_FE_INTERP not 0, no frame
// record or GS survey running (they expect new 2D tags in every picture).
bool enabled();

// Tick pass, before the 3D: the scenery's matrices built (as TSORender would), previous = current, current = now.
void tickCapture();

// Blend windows: each apply saves what is there and writes the blend; restore writes the saved state back.
void applyCamera();
void restoreCamera();
void logDrawnCamera(); // RC_FE_INTERP_LOG=2: the camera each picture is drawn from
void applyScenery();
void restoreScenery();
void sceneryFlagsSave();    // render-only passes: around TSORender
void sceneryFlagsRestore();
// The turntable: after CarouselRender on a tick, its cars' drawn state becomes current. carsApply/carsRestore around
// CarouselDraw.
void carsCapture();
void carsApply();
void carsRestore();
// Render-only passes: every object as it was before the pass (draws write tints and draw distances into them).
void objectsSave();
void objectsRestore();

// The 2D: on a tick, the part of the GS packet (K1) between k1Begin and k1End is kept; on a render-only pass,
// k1Replay appends it again.
void k1Begin(int slot = 0);
void k1End(int slot = 0);
void k1Replay(int slot = 0);
void k1Forget(int slot);      // nothing kept yet (a loop starts)
// The race's pause menu between ticks: the 2D packet part from k1Begin(1) to k1End(1).

// RC_FE_INTERP_LOG=1: once a second (passes, ticks, render-only passes, largest steps, K1 bytes); RC_FE_TRACE=<file>:
// one line per tick (the menu state, for the 60-against-144 comparison).
void logPass(bool tick);
void trace(uint32_t tickNo);

// The input recording's clock (overrides), for the trace.
void setInputClock(const void *clock);

} // namespace rc::frontinterp

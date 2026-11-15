// Inputs for the random test of the race HUD (HUDSYS.c). Each has the signature RC_DECOMP_FN_SETUP expects. Most run on
// the dump's real race (whole RAM): a real car with its HUD state varied (position, laps, times, weapons, signs,
// speed), hudVarG, the race type and screen layout; the sprites draw into the dump's GFX_P2 packet buffer.
#pragma once

#include "rc_decomp.h"

namespace rc::fuzz {

void hudNone(R5900Context *ctx, uint64_t &state, uint32_t scratch);   // no arguments
void hudTime(R5900Context *ctx, uint64_t &state, uint32_t scratch);   // a0 a buffer, a1 a lap time
void hudCar(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0 a car, a1 a size (1, 2), a2 a player
void hudPanel(R5900Context *ctx, uint64_t &state, uint32_t scratch);  // a0 a car (or 0), a1 a player
void hudTask(R5900Context *ctx, uint64_t &state, uint32_t scratch);   // a0 data, a1 a time; the countdown varied
void hudSprite(R5900Context *ctx, uint64_t &state, uint32_t scratch); // a0 a sprite, f12 x, f13 y, a1 rgb, f14 otz, f15 angle
void hudSign(R5900Context *ctx, uint64_t &state, uint32_t scratch);   // a0 x, a1 y, a2 size
void hudBorder(R5900Context *ctx, uint64_t &state, uint32_t scratch); // a0 a sprite, a1 x, a2 y, a3 shine
void hudLocked(R5900Context *ctx, uint64_t &state, uint32_t scratch); // no arguments; locked track cameras in scratch
void hudDaemon(R5900Context *ctx, uint64_t &state, uint32_t scratch); // a0 a handle to HUD resource ids

} // namespace rc::fuzz

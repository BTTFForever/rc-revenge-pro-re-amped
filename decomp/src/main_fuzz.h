// Inputs for the random test of MAIN.c. Each has the signature RC_DECOMP_FN_SETUP expects. All run on the dump's
// race (whole RAM: RC_DECOMP_FN_SETUP_HEAP) with sound off; the race order, the cup and its points, the pads, the
// pause menu's state and the car lists are varied.
#pragma once

#include "rc_decomp.h"

namespace rc::fuzz {

void mainNone(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // no arguments (a0/a1 random, unused)
void mainScan(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // no arguments; the pause menu's links varied
void mainResults(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 flag 0-2; race order, cup and points varied
void mainPause(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 &mainVarG; pause, results step, pads, hits
void mainPads(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // a0 number of humans; pads present or not
void mainCars(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // a0 car set 0-4; players, cup list, carousel
void mainScreen(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 0, 1 or random
void mainFrameWindow(R5900Context *ctx, uint64_t &state, uint32_t scratch); // a0 window 0-3 (or random), a1 a time
// Offline only, with the callees scripted fakes (see the .cpp): mode, status and the polled answers from a script.
void mainBoot(R5900Context *ctx, uint64_t &state, uint32_t scratch);           // main: the IOP's reboot polled, MainLoop a fake
void mainLoop(R5900Context *ctx, uint64_t &state, uint32_t scratch);           // MainLoop: modes 1-4 and statuses scripted
void mainTrackEdInit(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // RdrAlphaInit (a tail jump) real (whole RAM)
void mainTrackEdProcess(R5900Context *ctx, uint64_t &state, uint32_t scratch); // frames until the status changes; the fade
void mainTrackEdTerm(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // MemBlockFree (a tail jump) real (whole RAM)
void mainTerm(R5900Context *ctx, uint64_t &state, uint32_t scratch);           // printf (a tail jump) real
void mainPrintf(R5900Context *ctx, uint64_t &state, uint32_t scratch);         // a0-t3, f12-f19 random; $sp in scratch

} // namespace rc::fuzz

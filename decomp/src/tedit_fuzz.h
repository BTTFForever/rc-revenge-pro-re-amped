// Inputs for the random test of TEDIT.c (tedit_fuzz.cpp). Each has the signature RC_DECOMP_FN_SETUP expects.
//
// Every input builds a Track Editor in scratch: the level's edit modules (42, shaped as the game's: 1 x 1, 2 x 2 and
// 3 x 3 pieces with their direction and height changes, slopes, covered units and tiles; on the Track Editor dump the
// level's own modules half the time), the grid of track units (three layers, tiles with their base heights), the
// track's module list and start, the cursor and its modules, the edit menu's slopes, modules and restrictions, the
// permanent and memory card save buffers, and tEditVarG's state from the values the code tests. The game's outside
// calls (the heap, pad keys, sounds, the level load, the car set, font and load/save set-up) are stand-ins that log
// their arguments into scratch and answer from the seed; the heap stand-ins hand out blocks from a scratch arena, so
// the functions compare scratch and the globals only. The *Heap setups leave the real heap in place (functions that
// tail-jump into it) and compare all of RAM.
#pragma once

#include "rc_decomp.h"

namespace rc::fuzz {

void teditPlain(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // no arguments (a3 a leftover slope)
void teditWalk(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // a short track walked over the grid
void teditDisplay(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0-a1 a time and frame; all of RAM
void teditStandIns(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // drawing and menu callees stood in
void teditCameraPaths(R5900Context *ctx, uint64_t &state, uint32_t scratch); // a level's paths, AI nodes; a0 a camera, a1 a node
void teditMapUnit(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0-a1 a position, a2 a tile, t0 an orientation
void teditMenuInput(R5900Context *ctx, uint64_t &state, uint32_t scratch);   // a0 the edit menu's input bits
void teditPlace(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // TEditPlaceModule's five arguments
void teditDelete(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a walked track of one module or more
void teditGridControl(R5900Context *ctx, uint64_t &state, uint32_t scratch); // a0 a time; an edit state, the sticks
void teditGridDisplay(R5900Context *ctx, uint64_t &state, uint32_t scratch); // a0 a time; drawing stood in
void teditHeap(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // the real heap, the dump's blocks
void teditLevelData(R5900Context *ctx, uint64_t &state, uint32_t scratch);   // a0 a level's Track Editor data
void teditUnitCount(R5900Context *ctx, uint64_t &state, uint32_t scratch);   // a0-a1 a position, a2-a3 result buffers
void teditSetUnit(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0-a1 a position, a2 a unit, a3 a layer
void teditMeter(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 a counter, a1 a tile
void teditGridBlocked(R5900Context *ctx, uint64_t &state, uint32_t scratch); // a0-a1 a position
void teditSlopeModule(R5900Context *ctx, uint64_t &state, uint32_t scratch); // a0 a slope or module id, a1 an id
void teditMenuCell(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0 a slope row, a1 a column
void teditRestrict(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0-a1 restriction bits
void teditAINodes(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a ring of AI nodes

} // namespace rc::fuzz

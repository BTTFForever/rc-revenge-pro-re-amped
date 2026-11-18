// Inputs for the random test of CARD.c and LOADSAVE.c (card_fuzz.cpp). Each has the signature RC_DECOMP_FN_SETUP
// expects.
//
// Offline, libmc is a script (card_fuzz.cpp): every sceMc* call is logged into scratch with its arguments (names and
// buffers hashed) and answered from the input's seed. A command started with 0 "returns" its result at the next
// sceMcSync, which in mode 1 may first answer "still running" a few times; sceMcSync with nothing started answers -1
// without writing its result (the caller reads a stale stack word). The answers cover what the code tests: card types
// (PS2, PS1, PocketStation, none), formatted or not, free clusters around the 75 and 206 the game needs, new card and
// unformatted results, open/read/write errors and short counts, directories listing game and track saves (good save
// files have a correct checksum half the time). The game's other outside calls (pad keys, sounds, the menu drawing,
// the progress card, the Track Editor's track buffer, volumes, sceGsSyncV, the heap) are stand-ins that log and
// answer from the seed; the heap stand-ins hand out blocks from a scratch arena, so every function compares scratch
// and the globals only.
#pragma once

#include "rc_decomp.h"

namespace rc::fuzz {

void cardStatusNow(R5900Context *ctx, uint64_t &state, uint32_t scratch);   // a0 a card
void cardStatusASync(R5900Context *ctx, uint64_t &state, uint32_t scratch); // a0 a card, a1 a mode, a2 a status word
void cardIconHeader(R5900Context *ctx, uint64_t &state, uint32_t scratch);  // nine arguments, the last on the stack
void cardFile(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // a0 a card, a1 a name, a2 a size, a3 a buffer
void cardPlain(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 a card
void cardWait(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // a0 a result word
void cardRename(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 a card, a1-a2 names (a2 may be 0)
void cardGetDir(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 a card, a1 a name, a2 a count, a3 a table
void lsCreateDir(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 a card, a1 a directory name
void lsTrackNames(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0 a card; its directory
void lsDefaultName(R5900Context *ctx, uint64_t &state, uint32_t scratch);   // a0 a card, a1 a buffer, a2 overwrite
void lsUniqueName(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0 a card, a1 a typed name, a2 overwrite
void lsGameData(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // the save buffer and the game data
void lsMerge(R5900Context *ctx, uint64_t &state, uint32_t scratch);         // a loaded game state, the game's
void lsInit(R5900Context *ctx, uint64_t &state, uint32_t scratch);          // a0 a mode
void lsAutoLoad(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // no arguments; card 1's answers
void lsFreeSpace(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 a frame count; card statuses
void lsWrite(R5900Context *ctx, uint64_t &state, uint32_t scratch);         // LoadSaveWriteFile: a step, t1 a leftover
void lsExists(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // LoadSaveFileExists: a step, t0 a leftover
void lsErase(R5900Context *ctx, uint64_t &state, uint32_t scratch);         // LoadSaveFileErase: a step, a3 a leftover
void lsRename(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // LoadSaveFileRename: a step, t0 a leftover
void lsSetupClass(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0 the level's load/save data
void lsProcess(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 a frame count; any state
void lsRenderCards(R5900Context *ctx, uint64_t &state, uint32_t scratch);   // no arguments; statuses
void lsModeAvailable(R5900Context *ctx, uint64_t &state, uint32_t scratch); // no arguments; a mode and a card

} // namespace rc::fuzz

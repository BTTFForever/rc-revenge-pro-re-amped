// Inputs for the random test of the front end's camera paths (PATH.c). Each has the signature RC_DECOMP_FN_SETUP
// expects. The front-end level data (its camera list and path list) is built in scratch and frontEndVarG points at it;
// the tags are those of the game's own pathLinks table, so the via lookups find links.
#pragma once

#include "rc_decomp.h"

namespace rc::fuzz {

void pathNone(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // no arguments
void pathCameras(R5900Context *ctx, uint64_t &state, uint32_t scratch);   // a0 a tag, a1 a camera list
void pathPitch(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 a camera list
void pathNode(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 data, a1 a position on the active path
void pathCopy(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 a path, a1 a direction (3, 4 or other)
void pathBuilt(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // no arguments; the constructed path varied
void pathSmoother(R5900Context *ctx, uint64_t &state, uint32_t scratch);  // a0-a3 tags; the constructed path varied
void pathTags(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 a start tag, a1 an end tag
void pathDirection(R5900Context *ctx, uint64_t &state, uint32_t scratch); // a0 a path, a1 a start, a2 an end tag
void pathDaemon(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0 a handle to a tag list
void pathBuilder(R5900Context *ctx, uint64_t &state, uint32_t scratch);   // a0 a destination tag
void pathEnd(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 data; a leg ended (whole RAM)

} // namespace rc::fuzz

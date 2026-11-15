// Inputs for the random test of ANIM.c and TSO.c. Each has the signature RC_DECOMP_FN_SETUP expects. Most run on the
// dump's own level (whole RAM): its animation entries and scenery objects, varied where that keeps their pointers sound.
#pragma once

#include "rc_decomp.h"

namespace rc::fuzz {

void animLevel(R5900Context *ctx, uint64_t &state, uint32_t scratch);   // no arguments; entries, flags and $s1 varied
void animEntry(R5900Context *ctx, uint64_t &state, uint32_t scratch);   // a0 a real entry, varied; $s1 a bone
void animTag(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 a real object's tag or another
void tsoLevel(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // no arguments; the objects varied
void tsoObject(R5900Context *ctx, uint64_t &state, uint32_t scratch);   // a0 a real object, its flags and angles varied
void tsoCallback(R5900Context *ctx, uint64_t &state, uint32_t scratch); // a0 TSOSetSFXCallBack (it keeps its a0)
void tsoViewCone(R5900Context *ctx, uint64_t &state, uint32_t scratch); // a0 a point near the camera, f12 a radius

} // namespace rc::fuzz

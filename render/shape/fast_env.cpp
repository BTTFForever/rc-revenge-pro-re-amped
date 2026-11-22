// The Env program's fast path (RENDERER_P2_DESIGN.md 4.2; renderer plan P2 step 9 batch 4): EnvRdr_VUShapeProg entries 0
// and 0x78 (the shiny pass of cars and other reflective models, and its second-pass entry with the constants still in
// the registers). The track program's path with the colours converted first (the colour registers hold a colour in the
// integer part and a normal in the fraction); under the environment map (constant 10.z) the corner pass of
// environmentCorners in SSE (its two reciprocalLengths a corner scalar, as the reference) and one tinted colour a
// block. An env-mapped run writes VF26-VF28 for every triangle it reads, drawn or not, and VF26.w reaches the water
// program's packets: its last triangle is handed to the reference's step, as if it were hard. fast_track.h has the run.
#define RC_VU1_LEAN 1
#include "fast_track.h"

namespace rc::shape
{
    bool runEnv(uint8_t *mem, float (*vf)[4], uint32_t pc, uint32_t top, FastSink &sink, FastStats &stats, const FastOptions &options)
    {
        return detail::runFamily<detail::Family::Env>(mem, vf, pc, top, sink, stats, options);
    }
}

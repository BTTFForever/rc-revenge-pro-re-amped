// The Track Editor program's fast path (RENDERER_P2_DESIGN.md 4.2; renderer plan P2 step 9 batch 4): TERdr_VUShapeProg
// entry 0. The track program's path with its constants four quadwords earlier and its packet buffer one later, the
// lighting (four directional lights and a colour matrix, lightCorners) in place of the tint, and the depth bias added
// on the quick path after the clip flags instead of in setupTriangle. Every lit triangle (every one that faces the
// viewer) loads VF24-VF30 from VU memory 18-24, and VF25.w, VF26 and VF29 reach later packets: the path reloads them
// after a run whose last lit triangle was its own, and before a hard triangle's step. fast_track.h has the run.
#define RC_VU1_LEAN 1
#include "fast_track.h"

namespace rc::shape
{
    bool runTerdr(uint8_t *mem, float (*vf)[4], uint32_t top, FastSink &sink, FastStats &stats, const FastOptions &options)
    {
        return detail::runFamily<detail::Family::Terdr>(mem, vf, 0u, top, sink, stats, options);
    }
}

// The track program's fast path (RENDERER_P2_DESIGN.md 4.2; renderer plan P2 step 9 batch 3): TRdr_VUShapeProg entry 0
// with its common path in SSE, from tools/rc_replay/shape_lean.cpp's fastTrdr (measured there: 281.5 M quick
// triangles of the 67 dumps, nine quadwords each, equal to the reference's). See fast_run.h; the run itself is
// fast_track.h's, shared with the Env and TERdr programs since batch 4.
//
// The run is the reference's (trdr_shape.cpp body, vu_shape.h runTextureBlocks) step for step: the header packet, the
// constants, the register blocks and texture blocks kicked from VU memory as the program kicks them, the packet
// buffer's position (vi11) and its flush packets. Per triangle the path computes what the program computes up to the
// point where the outcomes part (culled, all behind, beyond one edge, quick, hard); a hard triangle, or one with a value
// the mode cannot treat as the VU does anywhere in it, goes to the reference's own step (rc::vu1l::trdrStep) on a
// ShapeProgram kept for the run, which holds every register lane a hard triangle reads from an earlier one (VF26,
// VF29, VF25.w: only hard triangles write them) and the packet buffer's position.
//
// Also here: runFast (the dispatch), the planted faults' names.
#define RC_VU1_LEAN 1
#include "fast_track.h"

namespace rc::shape
{
    namespace v = rc::vu1l;

    bool parseFastFault(const char *name, FastFault &out, uint8_t *programs)
    {
        const char *at = std::strchr(name, '@');
        const size_t len = at ? static_cast<size_t>(at - name) : std::strlen(name);
        uint8_t mask = 0xFF;
        if (at)
        {
            static const struct
            {
                const char *name;
                uint32_t program;
            } kPrograms[] = {{"trdr", v::kTrdr}, {"env", v::kEnv}, {"skin", v::kSkin}, {"terdr", v::kTerdr}};
            mask = 0;
            for (const auto &p : kPrograms)
                if (std::strcmp(at + 1, p.name) == 0)
                    mask = static_cast<uint8_t>(1u << p.program);
            if (!mask)
                return false;
        }
        for (uint32_t i = 1; i < static_cast<uint32_t>(FastFault::Count); ++i)
        {
            const char *n = fastFaultName(static_cast<FastFault>(i));
            if (std::strlen(n) == len && std::strncmp(name, n, len) == 0)
            {
                out = static_cast<FastFault>(i);
                if (programs)
                    *programs = mask;
                return true;
            }
        }
        return false;
    }

    const char *fastFaultName(FastFault f)
    {
        static const char *const names[] = {"none", "bias", "fused", "round-x", "edge-two", "hard-quick", "last-step", "lit-regs", "view-hard", "view-ref"};
        static_assert(sizeof(names) / sizeof(names[0]) == static_cast<size_t>(FastFault::Count));
        return static_cast<uint32_t>(f) < static_cast<uint32_t>(FastFault::Count) ? names[static_cast<uint32_t>(f)] : "?";
    }

    bool hasFastPath(uint32_t program, uint32_t pc)
    {
        switch (program)
        {
        case v::kTrdr: return pc == 0u;
        case v::kEnv: return pc == 0u || pc == 0x78u;
        case v::kSkin: return pc == 0u;
        case v::kTerdr: return pc == 0u;
        default: return false;
        }
    }

    bool runFast(uint32_t program, uint32_t pc, uint8_t *mem, float (*vf)[4], uint32_t top, FastSink &sink, FastStats &stats, const FastOptions &options,
                 const FastInput &input)
    {
        if (!hasFastPath(program, pc))
            return false;
        switch (program)
        {
        case v::kTrdr: return runTrdr(mem, vf, top, sink, stats, options, input);
        case v::kEnv: return runEnv(mem, vf, pc, top, sink, stats, options, input);
        case v::kSkin: return runSkin(mem, vf, sink, stats, options, input);
        case v::kTerdr: return runTerdr(mem, vf, top, sink, stats, options, input);
        default: return false;
        }
    }

    uint32_t fastInputStart(uint32_t program, uint32_t top) { return program == v::kSkin ? kSkinInput : (top & 0x3FFu); }

    const uint8_t *fastNotReproduced(uint32_t program) { return program == v::kSkin ? kSkinFastNotReproduced : kTrdrFastNotReproduced; }

    bool runTrdr(uint8_t *mem, float (*vf)[4], uint32_t top, FastSink &sink, FastStats &stats, const FastOptions &options, const FastInput &input)
    {
        return detail::runFamily<detail::Family::Trdr>(mem, vf, 0u, top, sink, stats, options, input);
    }
}

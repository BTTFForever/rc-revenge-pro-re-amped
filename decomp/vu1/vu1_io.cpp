// The references' Io entry (vu1_ref.h): the program table's dispatch, kept apart from vu_shape.cpp so a tool built
// from a few of the references (tools/shader_check) links without the others. In the lean build (vu_shape.h) it
// holds only run, which sets the lean mode; the program names and entries are the strict build's (rc_vu1_ref).
#include "vu_shape.h"
#include "vu1_ref.h"

namespace rc::RC_VU1_NAMESPACE {

#if !RC_VU1_LEAN
const char *programName(Program program)
{
    static const char *const names[kProgramCount] = {"TRdr_VUShapeProg", "EnvRdr_VUShapeProg", "Rdr_VUWaterProg", "Rdr_VUSkinProg",
                                                     "TERdr_VUShapeProg", "Rdr_VUShapeProg"};
    return program >= 0 && program < kProgramCount ? names[program] : "?";
}

// The entries of RENDERER_3D_DESIGN.md 5.3 (byte addresses); vu1_check.cpp's table of compared entries.
bool isEntry(Program program, uint32_t startPc)
{
    switch (program)
    {
    case kTrdr: return startPc == 0u || startPc == 0x2050u;
    case kEnv: return startPc == 0u || startPc == 0x78u || startPc == 0x2370u;
    case kTerdr: return startPc == 0u || startPc == 0x2198u;
    case kWater: case kSkin: case kRdr: return startPc == 0u;
    default: return false;
    }
}

#endif

bool run(Program program, Io &io)
{
    if (!isEntry(program, io.startPc))
        return false;
#if RC_VU1_LEAN
    const ScopedMode mode; // the caller's MXCSR with RZ + FTZ + DAZ set, restored on return (and on a throw)
#endif
    // Qualified: in the lean build Io is rc::vu1's, so an unqualified call would also find the strict entry.
    switch (program)
    {
    case kTrdr: RC_VU1_NAMESPACE::runTrdrShapeIo(io); break;
    case kEnv: RC_VU1_NAMESPACE::runEnvShapeIo(io); break;
    case kWater: RC_VU1_NAMESPACE::runWaterShapeIo(io); break;
    case kSkin: RC_VU1_NAMESPACE::runSkinShapeIo(io); break;
    case kTerdr: RC_VU1_NAMESPACE::runTerdrShapeIo(io); break;
    case kRdr: RC_VU1_NAMESPACE::runRdrShapeIo(io); break;
    default: return false;
    }
    return true;
}

} // namespace rc::RC_VU1_NAMESPACE

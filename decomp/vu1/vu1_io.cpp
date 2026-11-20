// The references' Io entry (vu1_ref.h): the program table's dispatch, kept apart from vu_shape.cpp so a tool built
// from a few of the references (tools/shader_check) links without the others.
#include "vu1_ref.h"

namespace rc::vu1 {

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

bool run(Program program, Io &io)
{
    if (!isEntry(program, io.startPc))
        return false;
    switch (program)
    {
    case kTrdr: runTrdrShapeIo(io); break;
    case kEnv: runEnvShapeIo(io); break;
    case kWater: runWaterShapeIo(io); break;
    case kSkin: runSkinShapeIo(io); break;
    case kTerdr: runTerdrShapeIo(io); break;
    case kRdr: runRdrShapeIo(io); break;
    default: return false;
    }
    return true;
}

} // namespace rc::vu1

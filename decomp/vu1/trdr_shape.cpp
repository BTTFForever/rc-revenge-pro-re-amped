// TRdr_VUShapeProg: the VU1 program that draws track geometry, written as ordinary C++.
//
// One run works through a list of textured triangle batches in VU memory. For each triangle it rejects
// backfaces, handles the near plane (up to six rotations of two cutting cases), transforms to the screen,
// and writes a ten-quadword GIF packet. Triangles crossing the screen edge go through a Sutherland-Hodgman
// clipper and come out as a fan.
//
// This is the reference a shader will be written from, and is checked against the original program
// instruction stream (vu1_check.cpp). Each step follows the original order because float rounding depends
// on it. Shared parts are in vu_shape.cpp.
#include "vu_shape.h"

#include "vu1_ref.h"

namespace rc::vu1 {

namespace {

const ShapeLayout kLayout{0x7C, 0x335, 0x3B9, 0x3C6, 26, 46, 195, 216, 237, 238, {5, 6, 7}, 247, QuickWrap::Flush, true, 0, true, true};

void body(ShapeProgram &p, uint32_t startPc, uint32_t top)
{
    if (startPc == 0x2050u)
    {
        p.kick(static_cast<int>(top & 0x3FFu));
        return;
    }

    // Header packet, then the constants.
    p.kick(247);
    p.loadConstants();

    runTextureBlocks(p, static_cast<int>(top & 0x3FFu), [&](int &reader)
    {
        if (!p.setupTriangle(reader))
            return;
        p.applyTint();
        p.drawTriangle();
    });
}

} // namespace

void runTrdrShape(uint8_t *memory, const float registers[32][4], uint32_t startPc, uint32_t top, KickList &kicks)
{
    ShapeProgram p(memory, registers, kLayout, kicks);
    body(p, startPc, top);
}

void runTrdrShape(Io &io)
{
    ShapeProgram p(io.memory, io.vf, kLayout, io.packet, io.user);
    body(p, io.startPc, io.top);
    std::memcpy(io.vf, p.r, sizeof(p.r));
}

} // namespace rc::vu1

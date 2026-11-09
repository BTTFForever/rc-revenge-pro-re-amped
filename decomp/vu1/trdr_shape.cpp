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

void runTrdrShape(uint8_t *memory, const float registers[32][4], uint32_t startPc, uint32_t top, KickList &kicks)
{
    ShapeProgram p(memory, registers, ScissorLinks{0x7C, 0x335, 0x3B9, 0x3C6}, true, kicks);

    if (startPc == 0x2050u)
    {
        p.kick(static_cast<int>(top & 0x3FFu));
        return;
    }

    // Header packet, then the constants.
    p.kick(247);
    p.loadConstants();

    int reader = static_cast<int>(top & 0x3FFu);
    int count = p.loadInt(reader, 2);
    bool startTextureBlock = true;

    while (true)
    {
        // A negative count is a block of GS registers: kick it and step over it.
        while (count < 0)
        {
            p.kick(reader + 1);
            reader += 1 - count;
            count = p.loadInt(reader, 2);
            startTextureBlock = true;
            if (count == 0)
                return;
        }
        if (count == 0)
            return;

        if (startTextureBlock)
        {
            // Four quadwords of texture state go out through a packet at the start of VU memory.
            for (int i = 0; i < 4; ++i)
                p.store(3 + i, p.load(reader + i));
            reader += 4;
            p.kick(0);
            p.vi11 = 26;
            startTextureBlock = false;
        }

        for (; count != 0; --count)
        {
            if (!p.setupTriangle(reader))
                continue;
            p.applyTint();
            p.drawTriangle();
        }

        count = p.loadInt(reader, 2);
        startTextureBlock = true;
    }
}

} // namespace rc::vu1

// TERdr_VUShapeProg: draws Track Editor pieces, written as ordinary C++.
//
// The track program with its constants four quadwords earlier and its packet buffer one later. Instead of a tint, each
// corner is lit: the colour slot holds a normal, which goes through four directional lights and a colour matrix. The
// depth bias is added only on the path that needs no clipping. Shared parts are in vu_shape.cpp.
#include "vu_shape.h"

#include "vu1_ref.h"

namespace rc::vu1 {

namespace {

void lightCorners(ShapeProgram &p)
{
    for (int i = 0; i < 7; ++i)
        p[24 + i] = p.load(18 + i);
    for (int corner = 0; corner < 3; ++corner)
    {
        V &colour = p[17 + corner];
        V light;
        for (int lane = 0; lane < 4; ++lane)
        {
            float acc = fmul(p[24].f[lane], colour.f[0]).value;
            acc = fmadd(acc, p[25].f[lane], colour.f[1]).value;
            const float value = fmadd(acc, p[26].f[lane], colour.f[2]).value;
            light.f[lane] = value > 0.0f ? value : 0.0f;
        }
        for (int lane = 0; lane < 3; ++lane)
        {
            float acc = fmul(p[27].f[lane], light.f[0]).value;
            acc = fmadd(acc, p[28].f[lane], light.f[1]).value;
            acc = fmadd(acc, p[29].f[lane], light.f[2]).value;
            colour.f[lane] = fmadd(acc, p[30].f[lane], light.f[3]).value;
        }
        colour.f[3] = fadd(1.0f, 126.0f).value;
        for (int lane = 0; lane < 4; ++lane)
            colour.f[lane] = ftoi0(colour.f[lane]);
    }
}

} // namespace

namespace {

const ShapeLayout kLayout = []
{
    ShapeLayout l{0x9C, 0x35B, 0x3E8, 0x3F5, 27, 47, 196, 217, 238, 239, {5, 6, 7}, 248, QuickWrap::Flush, true, -4, true, false};
    l.biasInQuickPath = true;
    return l;
}();

void body(ShapeProgram &p, uint32_t startPc, uint32_t top)
{
    if (startPc == 0x2198u)
    {
        p.kick(static_cast<int>(top & 0x3FFu));
        return;
    }

    p.kick(248);
    p.loadConstants();
    runTextureBlocks(p, static_cast<int>(top & 0x3FFu), [&](int &reader)
    {
        if (!p.setupTriangle(reader))
            return;
        lightCorners(p);
        p.drawTriangle();
    });
}

} // namespace

void runTerdrShape(uint8_t *memory, const float registers[32][4], uint32_t startPc, uint32_t top, KickList &kicks)
{
    ShapeProgram p(memory, registers, kLayout, kicks);
    body(p, startPc, top);
}

void runTerdrShapeIo(Io &io)
{
    ShapeProgram p(io.memory, io.vf, kLayout, io.packet, io.user);
    body(p, io.startPc, io.top);
    std::memcpy(io.vf, p.r, sizeof(p.r));
}

} // namespace rc::vu1

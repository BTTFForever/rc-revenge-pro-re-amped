// Rdr_VUShapeProg: draws world and effect shapes through two matrices, written as ordinary C++.
//
// The same triangle format as the track, read from a fixed address, with the backface test optional per triangle and no
// tint. Triangles with all corners in front of the near plane are tested against the screen with a second limit before
// the usual clipping. The depth bias is added before clipping and again as each triangle is written.
// Shared parts are in vu_shape.cpp.
#include "vu_shape.h"

#include "vu1_ref.h"

namespace rc::vu1 {

namespace {

V transformBy(const V rows[4], const V &v)
{
    V out;
    for (int lane = 0; lane < 4; ++lane)
    {
        float acc = fmul(rows[0].f[lane], v.f[0]).value;
        acc = fmadd(acc, rows[1].f[lane], v.f[1]).value;
        acc = fmadd(acc, rows[2].f[lane], v.f[2]).value;
        out.f[lane] = fmadd(acc, rows[3].f[lane], v.f[3]).value;
    }
    return out;
}

void rotateViewport(ShapeProgram &p, int times)
{
    for (int i = 0; i < times; ++i)
    {
        const V v = p[9];
        p[9] = V{{v.f[1], v.f[2], v.f[3], v.f[0]}}; // MR32
    }
}

// All corners in front of the near plane.
void drawUnclipped(ShapeProgram &p)
{
    rotateViewport(p, 3); // The screen test below uses a limit from another lane.
    for (int corner = 0; corner < 3; ++corner)
    {
        p.q = vuDivide(1.0f, p[11 + corner].f[2]);
        p.mulBy(p[11 + corner], p.q, XYW);
        p.mulBy(p[14 + corner], p.q, XYZ);
        p.transform(11 + corner);
        p.clip(11 + corner);
    }
    rotateViewport(p, 1);

    for (uint32_t mask : {0xFFEFBEu, 0xFFDF7Du, 0xFFBEFBu, 0xFF7DF7u})
        if ((p.clipFlags | mask) == 0xFFFFFFu)
            return;

    p.clip(11);
    p.clip(12);
    p.clip(13);
    if ((p.clipFlags & 0xF3CFu) != 0u)
        p.scissor(p.layout.linkQuick, true);
    else
        p.kickTriangle();
}

void drawOne(ShapeProgram &p, int &reader, const V first[4], const V second[4])
{
    const V normal = p.load(reader);
    p[20] = normal;
    for (int corner = 0; corner < 3; ++corner)
    {
        p[11 + corner] = p.load(reader + 1 + corner);
        p[14 + corner] = p.load(reader + 4 + corner);
        p[17 + corner] = p.load(reader + 7 + corner);
    }
    reader += 10;

    if (static_cast<int16_t>(bits(normal.f[3])) == 0)
    {
        V relative{};
        p.subTo(relative, p[11], p[21], XYZ);
        p[25] = relative;
        const float x = fmul(relative.f[0], normal.f[0]).value;
        const float y = fmul(relative.f[1], normal.f[1]).value;
        const float z = fmul(relative.f[2], normal.f[2]).value;
        if (!fadd(fadd(x, y).value, z).negative)
            return;
    }

    for (int corner = 0; corner < 3; ++corner)
        p[11 + corner] = transformBy(second, transformBy(first, p[11 + corner]));
    for (int corner = 0; corner < 3; ++corner)
        p[11 + corner].f[2] = fadd(p[11 + corner].f[2], p[14].f[3]).value;

    p.behind = 0;
    if (fsub(p[11].f[2], p[23].f[2]).negative)
        p.behind |= 128;
    if (fsub(p[12].f[2], p[23].f[2]).negative)
        p.behind |= 64;
    if (fsub(p[13].f[2], p[23].f[2]).negative)
        p.behind |= 32;

    if (p.behind == 0)
        drawUnclipped(p);
    else
        p.drawTriangle();
}

} // namespace

namespace {

const ShapeLayout kLayout = []
{
    ShapeLayout l{0xCC, 0x380, 0x40D, 0x41A, 31, 51, 200, 221, 242, 243, {5, 6, 7}, 0, QuickWrap::None, false, 4, false, false};
    l.biasInKick = true;
    return l;
}();

void body(ShapeProgram &p)
{
    const ShapeLayout &layout = kLayout;

    V first[4], second[4];
    for (int i = 0; i < 4; ++i)
    {
        first[i] = p.load(22 + i);
        second[i] = p.load(26 + i);
        p[1 + i] = first[i];
        p[5 + i] = second[i];
    }
    p[9] = p.load(20);
    p[10] = p.load(21);
    p[21] = p.load(19);
    p[22] = p.load(18);
    p[23] = p.load(16);

    int reader = 256;
    for (;;)
    {
        int16_t count = p.loadInt(reader, 2);
        for (int i = 0; i < 4; ++i)
            p.store(7 + i, p.load(reader + i));
        reader += 4;
        p.kick(0);
        p.vi11 = layout.firstVertex;
        do
            drawOne(p, reader, first, second);
        while (--count != 0);
        if (p.loadInt(reader, 2) == 0)
            return;
    }
}

} // namespace

void runRdrShape(uint8_t *memory, const float registers[32][4], uint32_t, uint32_t, KickList &kicks)
{
    ShapeProgram p(memory, registers, kLayout, kicks);
    body(p);
}

void runRdrShape(Io &io)
{
    ShapeProgram p(io.memory, io.vf, kLayout, io.packet, io.user);
    body(p);
    std::memcpy(io.vf, p.r, sizeof(p.r));
}

} // namespace rc::vu1

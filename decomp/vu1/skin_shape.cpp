// Rdr_VUSkinProg: draws skinned models, written as ordinary C++.
//
// Each triangle is sixteen quadwords: the usual ten, then six giving each corner two bone matrices and a blend. A corner's
// final position is the blend of its stored position through one matrix and a second stored position through the other.
// There is no facing test. Clipping and packet writing are the shared shape code. Checked against the original by
// vu1_check.cpp.
#include "vu_shape.h"

#include "vu1_ref.h"

namespace rc::vu1 {

namespace {

// The registers are the original's: matrices pass through vf01..04 and vf24..27, and the w lanes of vf01..03 and vf29 can
// reach a GS packet.
void loadMatrix(ShapeProgram &p, int firstRegister, int address)
{
    for (int i = 0; i < 4; ++i)
        p[firstRegister + i] = p.load(address + i);
}

void transformBy(ShapeProgram &p, int firstRegister, int reg)
{
    V result;
    const V v = p[reg];
    for (int lane = 0; lane < 4; ++lane)
    {
        float acc = fmul(p[firstRegister].f[lane], v.f[0]).value;
        acc = fmadd(acc, p[firstRegister + 1].f[lane], v.f[1]).value;
        acc = fmadd(acc, p[firstRegister + 2].f[lane], v.f[2]).value;
        result.f[lane] = fmadd(acc, p[firstRegister + 3].f[lane], v.f[3]).value;
    }
    p[reg] = result;
}

void loadAndTransform(ShapeProgram &p, int &reader)
{
    p[8].f[0] = fsub(0.0f, p[23].f[2]).value; // These overwrite the matrix's last row, whose position lanes the
    p[8].f[1] = p[8].f[0];                    // program's w of zero then cancels.
    p[8].f[2] = fsub(0.0f, p[23].f[2]).value;
    p[20] = p.load(reader);
    p[11] = p.load(reader + 1);
    p[12] = p.load(reader + 2);
    p[13] = p.load(reader + 3);
    const int weights = reader + 10;

    const auto address = [&](int quad, int lane) { return static_cast<uint16_t>(p.loadInt(weights + quad, lane)); };

    p[28] = p.load(weights + 1);
    loadMatrix(p, 24, address(0, 3));
    transformBy(p, 24, 28);
    loadMatrix(p, 1, address(0, 2));

    p[29] = p.load(weights + 3);
    transformBy(p, 1, 11);
    loadMatrix(p, 24, address(2, 3));
    transformBy(p, 24, 29);
    loadMatrix(p, 1, address(2, 2));

    p[30] = p.load(weights + 5);
    transformBy(p, 1, 12);
    loadMatrix(p, 24, address(4, 3));
    transformBy(p, 24, 30);
    loadMatrix(p, 1, address(4, 2));
    transformBy(p, 1, 13);

    p[24] = p.load(weights);
    p[25] = p.load(weights + 2);
    p[26] = p.load(weights + 4);
    p.mulBy(p[11], p[24].f[0], XYZ);
    p.mulBy(p[28], p[24].f[1], XYZ);
    p.mulBy(p[12], p[25].f[0], XYZ);
    p.mulBy(p[29], p[25].f[1], XYZ);
    p.mulBy(p[13], p[26].f[0], XYZ);
    p.mulBy(p[30], p[26].f[1], XYZ);
    for (int i = 0; i < 6; ++i)
        p[14 + i] = p.load(reader + 4 + i);
    p[24] = p.load(12);
    p.addTo(p[11], p[11], p[28], XYZ);
    p.addTo(p[12], p[12], p[29], XYZ);
    p.addTo(p[13], p[13], p[30], XYZ);
    p.subTo(p[11], p[11], p[24], XYZW);
    p.subTo(p[12], p[12], p[24], XYZW);
    p.subTo(p[13], p[13], p[24], XYZW);

    for (int corner = 0; corner < 3; ++corner)
        transformBy(p, 5, 11 + corner);

    p.q = vuDivide(1.0f, p[11].f[2]);
    p.behind = 0;
    if (fadd(p[8].f[0], p[11].f[2]).negative)
        p.behind |= 128;
    if (fadd(p[8].f[1], p[12].f[2]).negative)
        p.behind |= 64;
    if (fadd(p[8].f[2], p[13].f[2]).negative)
        p.behind |= 32;

    reader += 16;
}

} // namespace

namespace {

const ShapeLayout kLayout{0xA1, 0x367, 0x401, 0x41B, 26, 46, 195, 216, 237, 238, {1, 2, 3}, 0, QuickWrap::None, false, 0, false, false};

void body(ShapeProgram &p)
{
    const ShapeLayout &layout = kLayout;

    for (int i = 0; i < 4; ++i)
        p[5 + i] = p.load(21 + i);
    p[9] = p.load(15);
    p[10] = p.load(16);
    p[21] = p.load(14);
    p[22] = p.load(13);
    p[23] = p.load(10);

    int reader = 512;
    for (;;)
    {
        p[31] = p.load(reader);
        p.store(4, p[31]);
        p.kick(0);
        int16_t count = p.loadInt(reader, 2);
        ++reader;
        p.vi11 = layout.firstVertex;
        do
        {
            loadAndTransform(p, reader);
            p.drawTriangle();
        } while (--count != 0);
        if (p.loadInt(reader, 2) == 0)
            return;
    }
}

} // namespace

void runSkinShape(uint8_t *memory, const float registers[32][4], uint32_t, uint32_t, KickList &kicks)
{
    ShapeProgram p(memory, registers, kLayout, kicks);
    body(p);
}

void runSkinShapeIo(Io &io)
{
    ShapeProgram p(io.memory, io.vf, kLayout, io.packet, io.user);
    body(p);
    std::memcpy(io.vf, p.r, sizeof(p.r));
}

} // namespace rc::vu1

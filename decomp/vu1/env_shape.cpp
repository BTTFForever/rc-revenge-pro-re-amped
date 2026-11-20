// EnvRdr_VUShapeProg: draws the shiny pass of cars and other reflective models, written as ordinary C++.
//
// The same triangle format and clipping as the track program, with two additions. The colour registers hold a colour
// in their integer part and a surface normal in their fraction. When the environment-map flag (constant 10.z) is on,
// the program works out texture coordinates from that normal and the view direction, and replaces the block's texture
// with the environment map from constant 10.xy; otherwise it just drops the fraction.
//
// The maths is reproduced as it is, including the per-component products that a textbook reflection would not use.
// Checked against the original by vu1_check.cpp.
#include "vu_shape.h"

#include "vu1_ref.h"

namespace rc::vu1 {

namespace {

// Replaces the texture and colour registers of a triangle with the environment-mapped version.
void environmentCorners(ShapeProgram &p)
{
    const V row[3] = {p.load(7), p.load(8), p.load(9)};
    const V settings = p.load(10);
    float scale[3];

    // The normals live in vf26-vf28, as in the original: vf26.w reaches a later run's GS packets (the water program's).
    for (int corner = 0; corner < 3; ++corner)
    {
        V &n = p[26 + corner];
        V &eye = p[14 + corner];
        const V &packed = p[17 + corner];

        // The fraction of each lane, recentred on zero and doubled.
        V whole;
        for (int lane = 0; lane < 4; ++lane)
            whole.f[lane] = itof0(ftoi0(packed.f[lane]));
        p.subTo(n, packed, whole, XYZW);
        for (int lane = 0; lane < 3; ++lane)
            n.f[lane] = fsub(n.f[lane], 0.5f).value;
        p.addTo(n, n, n, XYZW);

        p.subTo(eye, p[11 + corner], p[21], XYZW);
        eye.f[3] = reciprocalLength(eye);
        p.mulBy(eye, eye.f[3], XYZ);

        p.mulLane(n, n, n, XYZW);
        p.mulLane(n, n, eye, XYZW);
        p.mulBy(n, 2.0f, XYZW);
        p.subTo(n, n, eye, XYZW);

        for (int lane = 0; lane < 3; ++lane)
        {
            float acc = fmul(n.f[lane], row[0].f[lane]).value;
            acc = fmadd(acc, n.f[lane], row[1].f[lane]).value;
            n.f[lane] = fmadd(acc, n.f[lane], row[2].f[lane]).value;
        }
        n.f[2] = fadd(n.f[2], 1.0f).value;
        scale[corner] = reciprocalLength(n);
    }

    for (int corner = 0; corner < 3; ++corner)
    {
        V &n = p[26 + corner];
        V &texture = p[14 + corner];
        p.mulBy(n, scale[corner], XY);
        p.mulBy(n, 0.5f, XY);
        texture.f[2] = 1.0f;
        texture.f[3] = 0.0f;
        texture.f[0] = fadd(n.f[0], 0.5f).value;
        texture.f[1] = fadd(n.f[1], 0.5f).value;
    }

    // Intensity: the setting times this block's own value.
    p[14].f[3] = fmul(settings.f[3], p[31].f[3]).value;
    const float intensity = p[14].f[3];
    for (int corner = 0; corner < 3; ++corner)
    {
        V &colour = p[17 + corner];
        for (int lane = 0; lane < 3; ++lane)
        {
            const float scaled = fmul(fadd(0.0f, 127.0f).value, intensity).value;
            colour.f[lane] = ftoi0(scaled < 127.0f ? scaled : 127.0f);
        }
        colour.f[3] = ftoi0(fadd(1.0f, 127.0f).value);
    }
}

} // namespace

namespace {

const ShapeLayout kLayout{0xDE, 0x399, 0x41D, 0x42A, 26, 46, 195, 216, 237, 238, {5, 6, 7}, 247, QuickWrap::None, true, 0, true, true};

void body(ShapeProgram &p, uint32_t startPc, uint32_t top)
{
    if (startPc == 0x2370u)
    {
        p.kick(static_cast<int>(top & 0x3FFu));
        return;
    }
    if (startPc == 0u)
        p.loadConstants();
    // Otherwise the program was started again at its second-pass entry (0x78), with the constants still in registers.

    const bool environment = p.loadInt(10, 2) > 0;
    int reader = static_cast<int>(top & 0x3FFu);
    int16_t count = p.loadInt(reader, 2);
    for (;;)
    {
        // Four quadwords of texture state go out through a packet at the start of VU memory.
        p[31] = p.load(reader);
        if (environment)
        {
            const V settings = p.load(10);
            p[31].f[0] = settings.f[0];
            p[31].f[1] = settings.f[1];
        }
        p.store(3, p[31]);
        for (int i = 1; i < 4; ++i)
            p.store(3 + i, p.load(reader + i));
        reader += 4;
        p.kick(0);
        p.vi11 = 26;

        do
        {
            // The original works out the corners' texture and colour before its facing test, so a triangle it rejects
            // leaves them (and the normals in vf26-vf28) in the registers too: vf26.w reaches a later run's packets.
            const bool facing = p.setupTriangle(reader);
            if (environment)
                environmentCorners(p);
            else
                for (int corner = 0; corner < 3; ++corner)
                    for (int lane = 0; lane < 4; ++lane)
                        p[17 + corner].f[lane] = ftoi0(p[17 + corner].f[lane]);
            if (!facing)
                continue;
            p.applyTint();
            p.drawTriangle();
        } while (--count != 0);

        // A negative count is a block of GS registers: kick it and step over it.
        count = p.loadInt(reader, 2);
        while (count < 0)
        {
            p.kick(reader + 1);
            reader += 1 - count;
            count = p.loadInt(reader, 2);
        }
        if (count == 0)
            return;
    }
}

} // namespace

void runEnvShape(uint8_t *memory, const float registers[32][4], uint32_t startPc, uint32_t top, KickList &kicks)
{
    ShapeProgram p(memory, registers, kLayout, kicks);
    body(p, startPc, top);
}

void runEnvShape(Io &io)
{
    ShapeProgram p(io.memory, io.vf, kLayout, io.packet, io.user);
    body(p, io.startPc, io.top);
    std::memcpy(io.vf, p.r, sizeof(p.r));
}

} // namespace rc::vu1

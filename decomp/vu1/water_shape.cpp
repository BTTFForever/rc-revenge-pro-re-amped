// Rdr_VUWaterProg: draws water, written as ordinary C++.
//
// Each triangle is drawn three times, with GS state changes between the passes: as it is stored, then in a single
// colour from VU memory, then with its texture coordinates pushed out from a point on the surface. The last two passes
// change the triangle in VU memory as they go. The clipping and packet writing are the shared shape code, with the
// program's constants and buffers eight to eleven quadwords further along. Checked against the original by vu1_check.cpp.
#include "vu_shape.h"

#include "vu1_ref.h"

namespace rc::RC_VU1_NAMESPACE {

namespace {

constexpr int kPassCounter = 245; // In lane y; lane x is the scissor routine's return address.

// Moves a triangle's texture coordinates away from a centre point, by an amount that doesn't depend on distance.
void rippleTexture(ShapeProgram &p, int triangle)
{
    p[29] = p.load(11);
    p[24] = p.load(9);
    for (int corner = 0; corner < 3; ++corner)
    {
        V &away = p[11 + corner];
        V &texture = p[14 + corner];
        away = p.load(triangle + 1 + corner);
        away.f[0] = fsub(p[29].f[0], away.f[0]).value;
        away.f[2] = fsub(p[29].f[2], away.f[2]).value;
        away.f[1] = away.f[2]; // MR32.y
        away.f[2] = 0.0f;
        away.f[3] = 1.0f;
        away.f[3] = reciprocalLength(away);
        p.mulBy(away, away.f[3], XY);
        p.mulLane(away, away, p[24], XY);
        texture = p.load(triangle + 4 + corner);
        p.addTo(texture, texture, away, XY);
        std::memcpy(p.mem + ((triangle + 4 + corner) & 0x3FF) * 16, texture.f, 8);
    }
}

const ShapeLayout kLayout{0x73, 0x1BC, 0x249, 0x256, 34, 54, 203, 224, 245, 246, {25, 26, 29}, 255, QuickWrap::FlushFirst, false, 11, false, false};

void body(ShapeProgram &p, uint32_t top)
{
    const ShapeLayout &layout = kLayout;

    p.kick(20);
    p.loadConstants();
    p.storeInt(kPassCounter, 1, 2);

    int reader = static_cast<int>(top & 0x3FFu);
    int16_t count = p.loadInt(reader, 2);
    for (;;)
    {
        const V block = p.load(reader);
        p.store(5, block);
        p.store(13, block);
        for (int i = 1; i < 4; ++i)
            p.store(5 + i, p.load(reader + i));
        reader += 4;
        p.kick(0);
        p.vi11 = layout.firstVertex;

        do
        {
            for (;;)
            {
                if (p.setupTriangle(reader))
                    p.drawTriangle();

                uint16_t passes = static_cast<uint16_t>(p.loadInt(kPassCounter, 1));
                if (passes == 0u)
                    break;
                // The original adds 32767, so the counter runs 2, 0x8001, 0.
                passes = static_cast<uint16_t>(passes + 32767u);
                p.storeInt(kPassCounter, 1, passes);
                reader -= 10;
                if (passes != 0u)
                {
                    p[30] = p.load(10);
                    p[24] = p.load(12);
                    p.store(5, p[24]);
                    p.kick(0);
                    p.kick(17);
                    for (int corner = 0; corner < 3; ++corner)
                        p.store(reader + 7 + corner, p[30]);
                }
                else
                {
                    p.kick(14);
                    rippleTexture(p, reader);
                }
            }

            p.storeInt(kPassCounter, 1, 2);
            p[24] = p.load(13);
            p.store(5, p[24]);
            p.kick(0);
            p.kick(20);
        } while (--count != 0);

        count = p.loadInt(reader, 2);
        if (count == 0)
            return;
    }
}

} // namespace

void runWaterShape(uint8_t *memory, const float registers[32][4], uint32_t, uint32_t top, KickList &kicks, float (*after)[4])
{
    ShapeProgram p(memory, registers, kLayout, kicks);
    body(p, top);
    if (after != nullptr)
        std::memcpy(after, p.r, sizeof(p.r));
}

void runWaterShapeIo(Io &io)
{
    ShapeProgram p(io.memory, io.vf, kLayout, io.packet, io.user);
    body(p, io.top);
    std::memcpy(io.vf, p.r, sizeof(p.r));
}

} // namespace rc::RC_VU1_NAMESPACE

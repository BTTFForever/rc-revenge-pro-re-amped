// The parts shared by the shape-drawing VU1 programs. See vu_shape.h.
#include "vu_shape.h"

#include "vu1_ref.h"

namespace rc::vu1 {

void ShapeProgram::mulLane(V &dst, const V &a, const V &b, int lanes)
{
    for (int i = 0; i < 4; ++i)
        if (lanes & (1 << i))
            dst.f[i] = fmul(a.f[i], b.f[i]).value;
}

void ShapeProgram::mulBy(V &dst, float by, int lanes)
{
    for (int i = 0; i < 4; ++i)
        if (lanes & (1 << i))
            dst.f[i] = fmul(dst.f[i], by).value;
}

void ShapeProgram::addTo(V &dst, const V &a, const V &b, int lanes)
{
    for (int i = 0; i < 4; ++i)
        if (lanes & (1 << i))
            dst.f[i] = fadd(a.f[i], b.f[i]).value;
}

void ShapeProgram::subTo(V &dst, const V &a, const V &b, int lanes)
{
    for (int i = 0; i < 4; ++i)
        if (lanes & (1 << i))
            dst.f[i] = fsub(a.f[i], b.f[i]).value;
}

void ShapeProgram::kick(int quad)
{
    std::vector<uint8_t> packet;
    copyGifPacket(mem, quad, packet);
    kicks->push_back(std::move(packet));
}
void ShapeProgram::clip(int reg)
{
    const uint32_t wBits = bits(r[9].f[3]);
    const int32_t limit = (wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF;
    uint32_t flags = 0u;
    for (uint32_t lane = 0u; lane < 3u; ++lane)
    {
        const uint32_t value = bits(r[reg].f[lane]);
        if (static_cast<int32_t>(value) > limit)
            flags |= 1u << (lane * 2u);
        if (static_cast<int32_t>(value ^ 0x80000000u) > limit)
            flags |= 2u << (lane * 2u);
    }
    clipFlags = ((clipFlags << 6) | flags) & 0xFFFFFFu;
}

// Depth (z) comes from w, then the whole position is scaled by the viewport vector.
void ShapeProgram::transform(int reg)
{
    r[reg].f[2] = r[reg].f[3];
    mulLane(r[reg], r[reg], r[10], XYZW);
}

void ShapeProgram::perspective(int position, int texture, bool scaleTextureByQ)
{
    mulBy(r[position], q, XYW);
    if (scaleTextureByQ)
        mulBy(r[texture], q, XYZ);
    else
        mulBy(r[texture], r[position].f[3], XYZ);
}

void ShapeProgram::rotateForward()
{
    for (int first : {11, 14, 17})
    {
        const V saved = r[first];
        r[first] = r[first + 1];
        r[first + 1] = r[first + 2];
        r[first + 2] = saved;
    }
}

void ShapeProgram::rotateBackward()
{
    for (int first : {11, 14, 17})
    {
        const V saved = r[first + 2];
        r[first + 2] = r[first + 1];
        r[first + 1] = r[first];
        r[first] = saved;
    }
}

void ShapeProgram::kickTriangle()
{
    for (int i = 0; i < 3; ++i)
    {
        V &position = r[11 + i];
        addTo(position, position, r[9], XY);
        position.f[0] = ftoi4(position.f[0]);
        position.f[1] = ftoi4(position.f[1]);
        position.f[2] = ftoi0(position.f[2]);
        store(vi11 + i * 3, r[14 + i]);
        store(vi11 + i * 3 + 1, r[17 + i]);
        store(vi11 + i * 3 + 2, position);
    }
    vi11 += 10;
    kick(vi11 - 11);
    if (vi11 == 46)
    {
        vi11 = 26;
        kick(247);
    }
}

// The common case: all three corners in front of the near plane.
void ShapeProgram::writeQuickTriangle()
{
    r[11].f[0] = fmul(r[11].f[0], q).value;
    r[11].f[1] = fmul(r[11].f[1], q).value;
    r[11].f[3] = fmul(r[11].f[3], q).value;
    mulBy(r[14], q, XYZ);
    store(vi11 + 1, r[17]);
    store(vi11 + 4, r[18]);
    q = vuDivide(1.0f, r[12].f[2]);
    store(vi11 + 7, r[19]);
    transform(11);
    transform(12);
    store(vi11, r[14]);
    mulBy(r[12], q, XYZ); // After the viewport scale, and leaving w alone, unlike the first corner.
    mulBy(r[15], q, XYZ);
    q = vuDivide(1.0f, r[13].f[2]);
    transform(13);
    addTo(r[5], r[11], r[9], XY);
    addTo(r[6], r[12], r[9], XY);
    mulBy(r[13], q, XYZ);
    store(vi11 + 3, r[15]);
    mulBy(r[16], q, XYZ);
    clip(11);
    clip(12);
    clip(13);
    store(vi11 + 6, r[16]);
    r[5].f[0] = ftoi4(r[5].f[0]);
    r[5].f[1] = ftoi4(r[5].f[1]);
    r[5].f[2] = ftoi0(r[11].f[2]);
    addTo(r[7], r[13], r[9], XY);
    r[6].f[0] = ftoi4(r[6].f[0]);
    r[6].f[1] = ftoi4(r[6].f[1]);
    r[6].f[2] = ftoi0(r[12].f[2]);
    r[7].f[0] = ftoi4(r[7].f[0]);
    r[7].f[1] = ftoi4(r[7].f[1]);
    r[7].f[2] = ftoi0(r[13].f[2]);
    store(vi11 + 2, r[5]);
    if ((clipFlags & 0xF3CFu) != 0u)
    {
        scissor(links.quick, true);
        return;
    }
    store(vi11 + 5, r[6]);
    store(vi11 + 8, r[7]);
    vi11 += 10;
    kick(vi11 - 11);
    if (vi11 == 46)
    {
        vi11 = 26;
        if (flushOnQuickWrap)
            kick(247);
    }
}

// One Sutherland-Hodgman pass over a list of (position, texture, integer colour) vertices.
int ShapeProgram::clipStage(int axis, float limit, bool lowerBound, int source, int dest, int count)
{
    V previous[3] = {load(source + count * 3 - 3), load(source + count * 3 - 2), load(source + count * 3 - 1)};
    int written = 0;
    int reader = source;
    const auto outside = [&](float coordinate)
    {
        const bool negative = fsub(limit, coordinate).negative;
        return lowerBound ? !negative : negative;
    };
    for (int i = 0; i < count; ++i)
    {
        const V current[3] = {load(reader), load(reader + 1), load(reader + 2)};
        const int previousOutside = outside(previous[0].f[axis]) ? 1 : 0;
        const int currentOutside = outside(current[0].f[axis]) ? 2 : 0;
        const int state = previousOutside | currentOutside;
        if (state == 0)
        {
            for (int part = 0; part < 3; ++part)
                store(dest++, current[part]);
            ++written;
        }
        else if (state != 3)
        {
            V position = previous[0];
            V texture = previous[1];
            V colour = previous[2];
            V currentColour = current[2];
            for (int lane = 0; lane < 4; ++lane)
            {
                colour.f[lane] = itof0(colour.f[lane]);
                currentColour.f[lane] = itof0(currentColour.f[lane]);
            }
            const float distance = fsub(limit, position.f[axis]).value;
            V positionDelta{}, textureDelta{}, colourDelta{};
            subTo(positionDelta, position, current[0], XYZ);
            const float t = vuDivide(distance, positionDelta.f[axis]);
            mulBy(positionDelta, t, XYZ);
            subTo(textureDelta, texture, current[1], XYZ);
            subTo(colourDelta, colour, currentColour, XYZW);
            mulBy(textureDelta, t, XYZ);
            mulBy(colourDelta, t, XYZW);
            r[29] = colourDelta; // Its w survives into the next near-plane cut, which stores it.
            addTo(position, position, positionDelta, XYZ);
            addTo(texture, texture, textureDelta, XYZ);
            addTo(colour, colour, colourDelta, XYZW);
            for (int lane = 0; lane < 4; ++lane)
                colour.f[lane] = ftoi0(colour.f[lane]);
            store(dest++, position);
            store(dest++, texture);
            store(dest++, colour);
            ++written;
            if (state == 1)
            {
                for (int part = 0; part < 3; ++part)
                    store(dest++, current[part]);
                ++written;
            }
        }
        reader += 3;
        previous[0] = current[0];
        previous[1] = current[1];
        previous[2] = current[2];
    }
    return written;
}

// Clips a triangle against the screen guard band, and draws what is left as a fan.
void ShapeProgram::scissor(int link, bool testFirst)
{
    const uint32_t savedLink = static_cast<uint16_t>(link);
    std::memcpy(mem + 237 * 16, &savedLink, 4);
    if (testFirst)
    {
        // Entirely beyond one edge: the three corners' flags for that edge are all set.
        for (uint32_t mask : {0xFFEFBEu, 0xFFDF7Du, 0xFFBEFBu, 0xFF7DF7u})
        {
            if ((clipFlags | mask) == 0xFFFFFFu)
                return;
        }
    }

    constexpr int kListA = 195;
    constexpr int kListB = 216;
    for (int corner = 0; corner < 3; ++corner)
    {
        store(kListA + corner * 3, r[11 + corner]);
        store(kListA + corner * 3 + 1, r[14 + corner]);
        store(kListA + corner * 3 + 2, r[17 + corner]);
    }

    const float guardX = r[23].f[0];
    const float guardY = r[23].f[1];
    int count = clipStage(0, fadd(0.0f, guardX).value, false, kListA, kListB, 3);
    if (count - 2 <= 0)
        return;
    count = clipStage(0, fsub(0.0f, guardX).value, true, kListB, kListA, count);
    if (count - 2 <= 0)
        return;
    count = clipStage(1, fadd(0.0f, guardY).value, false, kListA, kListB, count);
    if (count - 2 <= 0)
        return;
    count = clipStage(1, fsub(0.0f, guardY).value, true, kListB, kListA, count);
    if (count - 2 <= 0)
        return;

    int reader = kListA;
    const auto loadCorner = [&](int corner, int address)
    {
        r[11 + corner] = load(address);
        r[14 + corner] = load(address + 1);
        r[17 + corner] = load(address + 2);
    };
    loadCorner(0, reader);
    loadCorner(1, reader + 3);
    loadCorner(2, reader + 6);
    reader += 6;
    kickTriangle();
    int remaining = count - 3;
    while (remaining > 0)
    {
        loadCorner(0, kListA);
        loadCorner(1, reader);
        loadCorner(2, reader + 3);
        reader += 3;
        kickTriangle();
        --remaining;
    }
}

// Corners 1 and 2 are behind the near plane: both are slid along their edges to the plane.
void ShapeProgram::clipOneInFront()
{
    const float nearZ = r[23].f[2];
    const float front = fsub(r[11].f[2], nearZ).value;
    const float deltaOne = fsub(r[11].f[2], r[12].f[2]).value;
    const float deltaTwo = fsub(r[11].f[2], r[13].f[2]).value;
    const float tOne = fmul(1.0f, vuDivide(front, deltaOne)).value;
    const float tTwo = fmul(1.0f, vuDivide(front, deltaTwo)).value;

    const auto slide = [&](int position, int texture, float t)
    {
        q = vuDivide(1.0f, nearZ);
        subTo(r[texture], r[texture], r[14], XY);
        mulBy(r[texture], t, XY);
        addTo(r[texture], r[texture], r[14], XY);
        mulBy(r[texture], q, XYZ);
        subTo(r[position], r[position], r[11], XY);
        mulBy(r[position], t, XY);
        addTo(r[position], r[position], r[11], XY);
        mulBy(r[position], q, XYW);
        transform(position);
        clip(position);
    };
    slide(12, 15, tOne);
    slide(13, 16, tTwo);

    q = vuDivide(1.0f, r[11].f[2]);
    perspective(11, 14, false);
    transform(11);
    clip(11);

    if ((clipFlags & 0xF3CFu) != 0u)
        scissor(links.clipOne, true);
    else
        kickTriangle();
}

// Corner 2 is behind the near plane: the shape left is a quad, drawn as two triangles.
void ShapeProgram::clipTwoInFront()
{
    const float nearZ = r[23].f[2];
    const float frontZero = fsub(r[11].f[2], nearZ).value;
    const float frontOne = fsub(r[12].f[2], nearZ).value;
    const float deltaZero = fsub(r[11].f[2], r[13].f[2]).value;
    const float deltaOne = fsub(r[12].f[2], r[13].f[2]).value;
    const float tZero = fmul(1.0f, vuDivide(frontZero, deltaZero)).value;
    const float tOne = fmul(1.0f, vuDivide(frontOne, deltaOne)).value;
    q = vuDivide(1.0f, nearZ);

    const V backColour = r[19];
    // The point on the edge from corner 1, with the z of corner 2. Its w is left over from the last clip.
    V cutTexture = r[29];
    cutTexture.f[2] = fadd(0.0f, r[16].f[2]).value;
    V cutPosition = r[13];
    subTo(cutTexture, r[16], r[15], XY);
    subTo(r[16], r[16], r[14], XY);
    mulBy(cutTexture, tOne, XY);
    mulBy(r[16], tZero, XY);
    addTo(cutTexture, cutTexture, r[15], XY);
    addTo(r[16], r[16], r[14], XY);
    mulBy(cutTexture, q, XYZ);
    mulBy(r[16], q, XYZ);

    subTo(cutPosition, r[13], r[12], XY);
    subTo(r[13], r[13], r[11], XY);
    mulBy(cutPosition, tOne, XY);
    mulBy(r[13], tZero, XY);
    addTo(cutPosition, cutPosition, r[12], XY);
    addTo(r[13], r[13], r[11], XY);
    mulBy(cutPosition, q, XYW);
    mulBy(r[13], q, XYW);
    cutPosition.f[2] = cutPosition.f[3];
    r[13].f[2] = r[13].f[3];
    mulLane(cutPosition, cutPosition, r[10], XYZW);
    mulLane(r[13], r[13], r[10], XYZW);

    q = vuDivide(1.0f, r[12].f[2]);
    perspective(12, 15, false);
    transform(12);
    q = vuDivide(1.0f, r[11].f[2]);
    perspective(11, 14, false);
    transform(11);

    r[29] = cutTexture;

    constexpr int kSecond = 238;
    store(kSecond, r[12]);
    store(kSecond + 1, cutPosition);
    store(kSecond + 2, r[13]);
    store(kSecond + 3, r[15]);
    store(kSecond + 4, cutTexture);
    store(kSecond + 5, r[16]);
    store(kSecond + 6, r[18]);
    store(kSecond + 7, backColour);
    store(kSecond + 8, r[19]);

    scissor(links.clipTwoFirst, false);
    for (int i = 0; i < 9; ++i)
        r[11 + i] = load(kSecond + i);
    scissor(links.clipTwoSecond, false);
}

ShapeProgram::ShapeProgram(uint8_t *memory, const float registers[32][4], const ScissorLinks &scissorLinks, bool flush, KickList &kickList)
    : mem(memory), links(scissorLinks), flushOnQuickWrap(flush), kicks(&kickList)
{
    std::memcpy(r, registers, sizeof(r));
}

void ShapeProgram::loadConstants()
{
    r[1] = load(18);
    r[2] = load(19);
    r[3] = load(20);
    r[4] = load(21);
    r[9] = load(16);
    r[10] = load(17);
    r[21] = load(15);
    r[22] = load(14);
    r[23] = load(12);
}

bool ShapeProgram::setupTriangle(int &reader)
{
    ShapeProgram &p = *this;
    const V normal = p.load(reader);
    p[20] = normal;
    V nearTest;
    nearTest.f[0] = fsub(0.0f, p[23].f[2]).value;
    nearTest.f[1] = nearTest.f[0];
    nearTest.f[2] = fsub(0.0f, p[23].f[2]).value;
    for (int corner = 0; corner < 3; ++corner)
    {
        p[11 + corner] = p.load(reader + 1 + corner);
        p[14 + corner] = p.load(reader + 4 + corner);
        p[17 + corner] = p.load(reader + 7 + corner);
    }
    reader += 10;

    // Facing test: the normal against the direction from the light position to the first corner.
    V relative{};
    p.subTo(relative, p[11], p[21], XYZ);
    float dot[3];
    for (int lane = 0; lane < 3; ++lane)
        dot[lane] = fmul(relative.f[lane], normal.f[lane]).value;

    for (int corner = 0; corner < 3; ++corner)
    {
        V &position = p[11 + corner];
        float result[4];
        for (int lane = 0; lane < 4; ++lane)
        {
            float acc = fmul(p[1].f[lane], position.f[0]).value;
            acc = fmadd(acc, p[2].f[lane], position.f[1]).value;
            acc = fmadd(acc, p[3].f[lane], position.f[2]).value;
            result[lane] = fmadd(acc, p[4].f[lane], position.f[3]).value;
        }
        std::memcpy(position.f, result, sizeof(result));
        position.f[2] = fadd(position.f[2], p[14].f[3]).value; // A depth bias carried in the first w.
    }

    const float firstSum = fadd(dot[0], dot[1]).value;
    const Fmac facing = fadd(firstSum, dot[2]);
    p.q = vuDivide(1.0f, p[11].f[2]);

    const int16_t winding = static_cast<int16_t>(bits(normal.f[3]));
    behind = 0;
    if (fadd(nearTest.f[0], p[11].f[2]).negative)
        behind |= 128;
    if (fadd(nearTest.f[1], p[12].f[2]).negative)
        behind |= 64;
    if (fadd(nearTest.f[2], p[13].f[2]).negative)
        behind |= 32;
    if (winding == (facing.negative ? 128 : 0))
        return false;
    return true;
}

void ShapeProgram::applyTint()
{
    ShapeProgram &p = *this;
    const V tint = p.load(11);
    for (int corner = 0; corner < 3; ++corner)
    {
        V &colour = p[17 + corner];
        for (int lane = 0; lane < 3; ++lane)
            colour.f[lane] = itof0(colour.f[lane]);
        for (int lane = 0; lane < 3; ++lane)
        {
            const float acc = fmul(colour.f[lane], tint.f[3]).value;
            colour.f[lane] = ftoi0(fmadd(acc, tint.f[lane], 1.0f).value);
        }
    }
}

void ShapeProgram::drawTriangle()
{
    ShapeProgram &p = *this;
    switch (behind)
    {
    case 0:
        p.writeQuickTriangle();
        break;
    case 224:
        break;
    case 96:
        p.clipOneInFront();
        break;
    case 160:
        p.rotateForward();
        p.clipOneInFront();
        break;
    case 192:
        p.rotateBackward();
        p.clipOneInFront();
        break;
    case 32:
        p.clipTwoInFront();
        break;
    case 64:
        p.rotateBackward();
        p.clipTwoInFront();
        break;
    case 128:
        p.rotateForward();
        p.clipTwoInFront();
        break;
    default:
        break;
    }
}

void copyGifPacket(const uint8_t *memory, int quad, std::vector<uint8_t> &packet)
{
    int at = quad & 0x3FF;
    for (;;)
    {
        uint64_t tag[2];
        std::memcpy(tag, memory + at * 16, 16);
        const uint32_t nloop = tag[0] & 0x7FFFu;
        const bool endOfPacket = (tag[0] >> 15) & 1u;
        const uint32_t format = (tag[0] >> 58) & 3u;
        uint32_t nreg = (tag[0] >> 60) & 15u;
        if (nreg == 0u)
            nreg = 16u;
        const uint32_t quads = format == 0u ? nloop * nreg : format == 1u ? (nloop * nreg + 1u) / 2u : nloop;
        for (uint32_t i = 0u; i < quads + 1u; ++i)
        {
            packet.insert(packet.end(), memory + at * 16, memory + at * 16 + 16);
            at = (at + 1) & 0x3FF;
        }
        if (endOfPacket)
            break;
    }
}

} // namespace rc::vu1

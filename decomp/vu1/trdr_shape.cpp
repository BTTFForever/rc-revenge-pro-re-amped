// TRdr_VUShapeProg: the VU1 program that draws track geometry, written as ordinary C++.
//
// One run works through a list of textured triangle batches in VU memory. For each triangle it rejects
// backfaces, handles the near plane (up to six rotations of two cutting cases), transforms to the screen,
// and writes a ten-quadword GIF packet. Triangles crossing the screen edge go through a Sutherland-Hodgman
// clipper and come out as a fan.
//
// This is the reference a shader will be written from, and is checked against the original program
// instruction stream (vu1_check.cpp). Each step follows the original order because float rounding depends
// on it. Arithmetic follows the VU: operands are normalised, results flush to zero or clamp, and the
// caller has the host FPU rounding toward zero.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include <emmintrin.h>

#include "vu1_ref.h"

namespace rc::vu1 {

namespace {

enum Lanes : int { X = 1, Y = 2, Z = 4, W = 8, XY = X | Y, XYZ = X | Y | Z, XYW = X | Y | W, XYZW = 15 };

struct V
{
    float f[4];
};

uint32_t bits(float value)
{
    uint32_t out;
    std::memcpy(&out, &value, 4);
    return out;
}

float fromBits(uint32_t value)
{
    float out;
    std::memcpy(&out, &value, 4);
    return out;
}

// An operand as the VU sees it: denormals are zero, infinities and NaNs are the largest finite value.
float norm(float value)
{
    const uint32_t raw = bits(value);
    const uint32_t exponent = raw & 0x7F800000u;
    if (exponent == 0u)
        return fromBits(raw & 0x80000000u);
    if (exponent == 0x7F800000u)
        return fromBits((raw & 0x80000000u) | 0x7F7FFFFFu);
    return value;
}

struct Fmac
{
    float value;
    bool negative;
};

// `single` is the result in single precision, which is the value the VU produces. The exact result
// only decides whether it overflowed or underflowed.
Fmac finish(float single, double exact)
{
    const double magnitude = std::fabs(exact);
    const bool negative = std::signbit(exact);
    if (magnitude > static_cast<double>(std::numeric_limits<float>::max()))
        return {fromBits((negative ? 0x80000000u : 0u) | 0x7F7FFFFFu), negative};
    if (magnitude < static_cast<double>(std::numeric_limits<float>::min()))
        return {fromBits(negative ? 0x80000000u : 0u), negative};
    return {single, negative};
}

Fmac fadd(float a, float b)
{
    const float s = norm(a), t = norm(b);
    return finish(s + t, static_cast<double>(s) + static_cast<double>(t));
}

Fmac fsub(float a, float b)
{
    const float s = norm(a), t = norm(b);
    return finish(s - t, static_cast<double>(s) - static_cast<double>(t));
}

Fmac fmul(float a, float b)
{
    const float s = norm(a), t = norm(b);
    return finish(s * t, static_cast<double>(s) * static_cast<double>(t));
}

// The product is rounded to single precision before the sum, as the hardware does.
Fmac fmadd(float acc, float a, float b)
{
    const float accumulator = norm(acc), s = norm(a), t = norm(b);
    const float product = s * t;
    return finish(accumulator + product, static_cast<double>(accumulator) + static_cast<double>(s) * static_cast<double>(t));
}

float vuDivide(float numeratorRaw, float denominatorRaw)
{
    const float numerator = norm(numeratorRaw);
    const float denominator = norm(denominatorRaw);
    if (denominator == 0.0f)
    {
        const bool opposite = std::signbit(numerator) != std::signbit(denominator);
        return opposite ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
    }
    return norm(numerator / denominator);
}

int32_t ftoiRaw(float normalised, float scale)
{
    const float scaled = scale == 1.0f ? normalised : normalised * scale;
    if (scaled >= 2147483648.0f)
        return 0x7FFFFFFF;
    return _mm_cvttss_si32(_mm_set_ss(scaled));
}

float ftoi0(float value) { return fromBits(static_cast<uint32_t>(ftoiRaw(norm(value), 1.0f))); }
float ftoi4(float value) { return fromBits(static_cast<uint32_t>(ftoiRaw(norm(value), 16.0f))); }
float itof0(float value) { return static_cast<float>(static_cast<int32_t>(bits(value))); }

struct Program
{
    uint8_t *mem;
    V r[32];
    float acc[4]{};
    float q = 0.0f;
    uint32_t clipFlags = 0u;
    int vi11 = 26;
    std::vector<std::vector<uint8_t>> *kicks;

    V &operator[](int index) { return r[index]; }
    V load(int quad) const
    {
        V out;
        std::memcpy(out.f, mem + (quad & 0x3FF) * 16, 16);
        return out;
    }
    void store(int quad, const V &value) { std::memcpy(mem + (quad & 0x3FF) * 16, value.f, 16); }
    int16_t loadInt(int quad, int lane) const
    {
        int16_t out;
        std::memcpy(&out, mem + (quad & 0x3FF) * 16 + lane * 4, 2);
        return out;
    }

    void kick(int quad);
    void mulLane(V &dst, const V &a, const V &b, int lanes);
    void mulBy(V &dst, float by, int lanes);
    void addTo(V &dst, const V &a, const V &b, int lanes);
    void subTo(V &dst, const V &a, const V &b, int lanes);
    void transform(int reg);
    void clip(int reg);
    void perspective(int position, int texture, bool scaleTextureByQ);
    void rotateForward();
    void rotateBackward();
    void kickTriangle();
    void writeQuickTriangle();
    void clipOneInFront();
    void clipTwoInFront();
    void scissor(int link, bool testFirst);
    int clipStage(int axis, float limit, bool lowerBound, int source, int dest, int count);
};

void Program::mulLane(V &dst, const V &a, const V &b, int lanes)
{
    for (int i = 0; i < 4; ++i)
        if (lanes & (1 << i))
            dst.f[i] = fmul(a.f[i], b.f[i]).value;
}

void Program::mulBy(V &dst, float by, int lanes)
{
    for (int i = 0; i < 4; ++i)
        if (lanes & (1 << i))
            dst.f[i] = fmul(dst.f[i], by).value;
}

void Program::addTo(V &dst, const V &a, const V &b, int lanes)
{
    for (int i = 0; i < 4; ++i)
        if (lanes & (1 << i))
            dst.f[i] = fadd(a.f[i], b.f[i]).value;
}

void Program::subTo(V &dst, const V &a, const V &b, int lanes)
{
    for (int i = 0; i < 4; ++i)
        if (lanes & (1 << i))
            dst.f[i] = fsub(a.f[i], b.f[i]).value;
}

// Copies out one GIF packet as the hardware would send it: tags and data up to the end-of-packet tag.
void Program::kick(int quad)
{
    std::vector<uint8_t> packet;
    int at = quad & 0x3FF;
    for (;;)
    {
        uint64_t tag[2];
        std::memcpy(tag, mem + at * 16, 16);
        const uint32_t nloop = tag[0] & 0x7FFFu;
        const bool endOfPacket = (tag[0] >> 15) & 1u;
        const uint32_t format = (tag[0] >> 58) & 3u;
        uint32_t nreg = (tag[0] >> 60) & 15u;
        if (nreg == 0u)
            nreg = 16u;
        const uint32_t quads = format == 0u ? nloop * nreg : format == 1u ? (nloop * nreg + 1u) / 2u : nloop;
        const uint32_t count = quads + 1u;
        for (uint32_t i = 0u; i < count; ++i)
        {
            packet.insert(packet.end(), mem + at * 16, mem + at * 16 + 16);
            at = (at + 1) & 0x3FF;
        }
        if (endOfPacket)
            break;
    }
    kicks->push_back(std::move(packet));
}

void Program::clip(int reg)
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
void Program::transform(int reg)
{
    r[reg].f[2] = r[reg].f[3];
    mulLane(r[reg], r[reg], r[10], XYZW);
}

void Program::perspective(int position, int texture, bool scaleTextureByQ)
{
    mulBy(r[position], q, XYW);
    if (scaleTextureByQ)
        mulBy(r[texture], q, XYZ);
    else
        mulBy(r[texture], r[position].f[3], XYZ);
}

void Program::rotateForward()
{
    for (int first : {11, 14, 17})
    {
        const V saved = r[first];
        r[first] = r[first + 1];
        r[first + 1] = r[first + 2];
        r[first + 2] = saved;
    }
}

void Program::rotateBackward()
{
    for (int first : {11, 14, 17})
    {
        const V saved = r[first + 2];
        r[first + 2] = r[first + 1];
        r[first + 1] = r[first];
        r[first] = saved;
    }
}

void Program::kickTriangle()
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
void Program::writeQuickTriangle()
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
        scissor(0x7C, true);
        return;
    }
    store(vi11 + 5, r[6]);
    store(vi11 + 8, r[7]);
    vi11 += 10;
    kick(vi11 - 11);
    if (vi11 == 46)
    {
        vi11 = 26;
        kick(247);
    }
}

// One Sutherland-Hodgman pass over a list of (position, texture, integer colour) vertices.
int Program::clipStage(int axis, float limit, bool lowerBound, int source, int dest, int count)
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
void Program::scissor(int link, bool testFirst)
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
void Program::clipOneInFront()
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
        scissor(0x335, true);
    else
        kickTriangle();
}

// Corner 2 is behind the near plane: the shape left is a quad, drawn as two triangles.
void Program::clipTwoInFront()
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

    scissor(0x3B9, false);
    for (int i = 0; i < 9; ++i)
        r[11 + i] = load(kSecond + i);
    scissor(0x3C6, false);
}

} // namespace

void runTrdrShape(uint8_t *memory, const float registers[32][4], uint32_t startPc, uint32_t top,
                  std::vector<std::vector<uint8_t>> &kicks)
{
    Program p{memory, {}, {}, 0.0f, 0u, 26, &kicks};
    std::memcpy(p.r, registers, sizeof(p.r));

    if (startPc == 0x2050u)
    {
        p.kick(static_cast<int>(top & 0x3FFu));
        return;
    }

    // Header packet, then the transform and the viewport from fixed VU memory.
    p.kick(247);
    p[1] = p.load(18);
    p[2] = p.load(19);
    p[3] = p.load(20);
    p[4] = p.load(21);
    p[9] = p.load(16);
    p[10] = p.load(17);
    p[21] = p.load(15);
    p[22] = p.load(14);
    p[23] = p.load(12);

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
            int behind = 0;
            if (fadd(nearTest.f[0], p[11].f[2]).negative)
                behind |= 128;
            if (fadd(nearTest.f[1], p[12].f[2]).negative)
                behind |= 64;
            if (fadd(nearTest.f[2], p[13].f[2]).negative)
                behind |= 32;
            if (winding == (facing.negative ? 128 : 0))
                continue;

            // Colours: integer to float, a tint and an offset from VU memory, and back.
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

        count = p.loadInt(reader, 2);
        startTextureBlock = true;
    }
}

} // namespace rc::vu1

// What the game's shape-drawing VU1 programs have in common, written as ordinary C++: VU arithmetic, the near-plane
// and screen-edge clipping, and writing GIF packets. Each program is a thin main loop around this (trdr_shape.cpp,
// env_shape.cpp), and differs from the others in small ways recorded in ShapeProgram's settings.
//
// Arithmetic follows the VU: operands are normalised, results flush to zero or clamp, and the caller has the host FPU
// rounding toward zero. Each step follows the original order because float rounding depends on it.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include <emmintrin.h>

namespace rc::vu1 {

using KickList = std::vector<std::vector<uint8_t>>;

enum Lanes : int { X = 1, Y = 2, Z = 4, W = 8, XY = X | Y, XYZ = X | Y | Z, XYW = X | Y | W, XYZW = 15 };

struct V
{
    float f[4];
};

inline uint32_t bits(float value)
{
    uint32_t out;
    std::memcpy(&out, &value, 4);
    return out;
}

inline float fromBits(uint32_t value)
{
    float out;
    std::memcpy(&out, &value, 4);
    return out;
}

// An operand as the VU sees it: denormals are zero, infinities and NaNs are the largest finite value.
inline float norm(float value)
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
inline Fmac finish(float single, double exact)
{
    const double magnitude = std::fabs(exact);
    const bool negative = std::signbit(exact);
    if (magnitude > static_cast<double>(std::numeric_limits<float>::max()))
        return {fromBits((negative ? 0x80000000u : 0u) | 0x7F7FFFFFu), negative};
    if (magnitude < static_cast<double>(std::numeric_limits<float>::min()))
        return {fromBits(negative ? 0x80000000u : 0u), negative};
    return {single, negative};
}

inline Fmac fadd(float a, float b)
{
    const float s = norm(a), t = norm(b);
    return finish(s + t, static_cast<double>(s) + static_cast<double>(t));
}

inline Fmac fsub(float a, float b)
{
    const float s = norm(a), t = norm(b);
    return finish(s - t, static_cast<double>(s) - static_cast<double>(t));
}

inline Fmac fmul(float a, float b)
{
    const float s = norm(a), t = norm(b);
    return finish(s * t, static_cast<double>(s) * static_cast<double>(t));
}

// The product is rounded to single precision before the sum, as the hardware does.
inline Fmac fmadd(float acc, float a, float b)
{
    const float accumulator = norm(acc), s = norm(a), t = norm(b);
    const float product = s * t;
    return finish(accumulator + product, static_cast<double>(accumulator) + static_cast<double>(s) * static_cast<double>(t));
}

inline float vuDivide(float numeratorRaw, float denominatorRaw)
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

inline int32_t ftoiRaw(float normalised, float scale)
{
    const float scaled = scale == 1.0f ? normalised : normalised * scale;
    if (scaled >= 2147483648.0f)
        return 0x7FFFFFFF;
    return _mm_cvttss_si32(_mm_set_ss(scaled));
}

inline float ftoi0(float value) { return fromBits(static_cast<uint32_t>(ftoiRaw(norm(value), 1.0f))); }
inline float ftoi4(float value) { return fromBits(static_cast<uint32_t>(ftoiRaw(norm(value), 16.0f))); }
inline float itof0(float value) { return static_cast<float>(static_cast<int32_t>(bits(value))); }

// The reciprocal of a vector's length, as the VU's ERLENG computes it: zero for a zero vector.
inline float reciprocalLength(const V &v)
{
    const float x = norm(v.f[0]), y = norm(v.f[1]), z = norm(v.f[2]);
    const float length = std::sqrt(x * x + y * y + z * z);
    return length != 0.0f ? 1.0f / length : length;
}

enum class QuickWrap { Flush, None, FlushFirst };

// Where the programs differ. The clipping routines are the same text in all of them, at different addresses.
struct ShapeLayout
{
    // Return addresses a program stores in VU memory when it calls its scissor routine. They are visible in the memory
    // the program leaves behind.
    int linkQuick, linkClipOne, linkClipTwoFirst, linkClipTwoSecond;
    int firstVertex, wrapAt;     // The GS packet being written cycles between these quadword addresses.
    int listA, listB;            // The clipper's two vertex lists.
    int linkSlot, secondTriangle;
    int quickTemp[3];            // The registers the quick path builds screen positions in. Their w lanes reach the GS.
    int flushQuad;               // The small packet sent when the packet buffer wraps.
    QuickWrap quickWrap;
    bool flushOnClippedWrap;
    int constantsOffset;         // Added to the fixed VU memory addresses of the constants.
    bool hasWindingFlag;         // Otherwise a triangle is drawn only when it faces the viewer.
    bool hasDepthBias;           // A depth bias in the first corner's texture w.
};

struct ShapeProgram
{
    uint8_t *mem;
    V r[32];
    const ShapeLayout &layout;
    KickList *kicks;
    float acc[4]{};
    float q = 0.0f;
    uint32_t clipFlags = 0u;
    int vi11;
    int behind = 0;

    ShapeProgram(uint8_t *memory, const float registers[32][4], const ShapeLayout &shapeLayout, KickList &kickList);

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

    // The VU stores an integer register as a word, zero-extended.
    void storeInt(int quad, int lane, uint16_t value)
    {
        const uint32_t word = value;
        std::memcpy(mem + (quad & 0x3FF) * 16 + lane * 4, &word, 4);
    }

    void kick(int quad);
    void loadConstants(); // The transform, viewport, light position and clip range from fixed VU memory.
    bool setupTriangle(int &reader); // Loads and transforms a triangle; false if it faces away.
    void applyTint();                // Colour registers: integer to float, tint and offset from VU memory, and back.
    void drawTriangle();             // Near-plane cases, then the screen.

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

} // namespace rc::vu1

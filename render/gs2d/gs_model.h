#pragma once

// The CPU model of the 2D packet's pixel pipeline (renderer plan P1 step 7 batch 2; RENDERER_2D_DESIGN.md section 9.1).
// Pure functions over integers for the GS's rules (texture function, alpha test, Z test, blend), the fixed-function
// blend the Vulkan path uses (ffBlend: what it may store), and the reference's rasterisation and per-pixel arithmetic
// (triangle setup and weights, sprites, lines) written from the design's sections 4-8, not copied from the runtime, so
// that agreeing with the emulated GS means something. The same functions are the CPU twin of render/gs2d/gs2d.frag and
// of the replay's line expansion (gs2d_vk), and GsModel draws a whole draw list into CPU frame and Z buffers.
//
// Built with -ffp-contract=off (CMakeLists.txt) like the runtime: every float expression is one IEEE operation per
// operator, left to right, as in the reference.

#include "gs_plan.h"
#include "gs_state.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace rc::rd
{
    struct Rgba
    {
        uint8_t r = 0, g = 0, b = 0, a = 0;
        uint32_t packed() const { return r | static_cast<uint32_t>(g) << 8 | static_cast<uint32_t>(b) << 16 | static_cast<uint32_t>(a) << 24; }
        static Rgba unpack(uint32_t v)
        {
            return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
        }
    };

    // Section 6.3: the texture function, integers. Without TME the caller uses the vertex colour.
    Rgba gsTextureFunction(uint32_t tfx, uint32_t tcc, Rgba texel, Rgba vertex);
    // Section 5.1.
    bool gsAlphaPass(uint32_t atst, uint32_t aref, uint32_t as);
    struct Write
    {
        bool rgb = false, a = false, z = false;
    };
    // Which of RGB, A, Z a pixel with source alpha `as` writes (alpha test and AFAIL on a CT32 frame, ZMSK). The
    // frame mask (FBMSK) is applied by gsPixel.
    Write gsAlphaWrite(const GsState &s, uint32_t as);
    // ZTST: 0 NEVER, 1 ALWAYS, 2 GEQUAL, 3 GREATER, on integers.
    bool gsDepthPass(uint32_t ztst, uint32_t z, uint32_t stored);
    // One colour channel: ((A - B) * C >> 7) + D, clamped to 0..255 (COLCLAMP 1), C = As or FIX (Ad read as `ad`).
    uint32_t gsBlend(uint64_t alpha, uint32_t cs, uint32_t cd, uint32_t as, uint32_t ad = 0x80u);
    // What the fixed-function plan of 4.2 may store for one colour channel: lo == hi where the outcome is certain, lo
    // and lo + 1 on an exact tie of the blender's round-to-nearest. bias: the source bias of 4.3 (off for T1's second
    // run). For a plan without blending this is the shader's clamped integer term.
    struct Ff
    {
        uint32_t lo = 0, hi = 0;
    };
    Ff ffBlend(const GsPlan &p, uint32_t cs, uint32_t cd, uint32_t as, bool bias = true);
    // The level-scaled source term the shader outputs (4.2, 4.3) in 1/256 of a level, before the division by 255:
    // whether the bias was applied. For MIX: a - b where a >= b; MIXSUB: a + b. Used by ffBlend and by the planted fault.
    bool ffBiasApplies(const GsPlan &p, uint32_t cs, uint32_t as);

    // The whole per-pixel pipeline after the texture function (scissor excluded): alpha test, Z test, blend, masks.
    // Returns the new destination; storedZ is updated where Z is written. `written` (optional) tells whether anything
    // was written.
    Rgba gsPixel(const GsState &s, Rgba source, Rgba dest, uint32_t z, uint32_t &storedZ, bool *written = nullptr);

    // Section 8.1: the reference's triangle setup (window coordinates in 1/16 pixel, X - OFX), edge functions with
    // the top-left bias, area made positive, 1 / area in double; pixel bounds before the scissor.
    struct GsTri
    {
        int64_t x[3] = {}, y[3] = {}, a[3] = {}, b[3] = {}, c[3] = {}, bias[3] = {};
        int64_t area = 0;
        double invArea = 0.0;
        int minX = 0, maxX = -1, minY = 0, maxY = -1;
    };
    bool gsTriSetup(const GsVertex v[3], uint32_t ofx, uint32_t ofy, GsTri &t);
    inline int64_t gsTriEdge(const GsTri &t, int i, int px, int py)
    {
        return t.a[i] * (static_cast<int64_t>(px) * 16) + t.b[i] * (static_cast<int64_t>(py) * 16) + t.c[i];
    }
    inline bool gsTriCovers(const GsTri &t, const int64_t e[3])
    {
        return e[0] + t.bias[0] > 0 && e[1] + t.bias[1] > 0 && e[2] + t.bias[2] > 0;
    }
    inline float gsTriWeight(const GsTri &t, int64_t e) { return static_cast<float>(static_cast<double>(e) * t.invArea); }

    // Section 8.2: the integer points a sprite covers, before the scissor (first = ceil(min), last = ceil(max) - 1).
    struct GsRect
    {
        int x0 = 0, x1 = -1, y0 = 0, y1 = -1;
    };
    GsRect coverSprite(const GsVertex v[2], uint32_t ofx, uint32_t ofy);

    // Section 8.3: a line as the reference draws it, one pixel per step of Bresenham between the truncated end points,
    // both end pixels included; colour (IIP) and Z interpolated as the reference does.
    struct GsLinePixel
    {
        int x = 0, y = 0;
        uint32_t z = 0;
        Rgba c;
    };
    void expandLine(const GsVertex v[2], bool iip, uint32_t ofx, uint32_t ofy, std::vector<GsLinePixel> &out);

    // Section 6: the per-pixel quantities of a triangle at an integer sample point, as the reference computes them.
    struct GsTriPixel
    {
        float w[3] = {};
        uint32_t z = 0;
        Rgba vertex;            // interpolated (IIP) or the last vertex's
        float invQ = 0.0f, uf = 0.0f, vf = 0.0f, fu = 0.0f, fv = 0.0f; // texture coordinates (STQ path)
        int iu = 0, iv = 0;     // bilinear: floor(uf - 0.5); nearest: int(uf)
    };
    // The vertex Z as the reference holds it (a float of the integer: exact below 2^24).
    inline double gsVertexZ(uint32_t z) { return static_cast<double>(static_cast<float>(z)); }
    void gsTriPixel(const GsTri &t, const GsVertex v[3], const GsState &s, int px, int py, GsTriPixel &out);
    // The reference's bilinear filter of four texels (gs_cpu_raster_inline.h's arithmetic: lerp in float, round half up).
    Rgba gsBilinear(Rgba c00, Rgba c10, Rgba c01, Rgba c11, float fu, float fv);
    // CLAMP's wrap of a texel coordinate.
    int gsWrap(int coord, int size, uint32_t mode, uint32_t minC, uint32_t maxC);

    // The model of a whole draw list (the tests' second machine): frame buffers FBP 0x0 and 0x50 (640 x 256, CT32 as
    // RGBA words) and one Z buffer (Z24 values), drawn as the reference draws them.
    class GsModel
    {
    public:
        static constexpr uint32_t kWidth = 640, kHeight = 256;
        std::vector<uint32_t> colour[2], depth;
        struct Stats
        {
            uint64_t states = 0, unsupported = 0, prims = 0, pixels = 0, linePixels = 0, clears = 0, textures = 0;
        } stats;

        GsModel();
        void draw(const DrawList &list);

    private:
        struct Texture
        {
            uint32_t width = 0, height = 0;
            std::vector<uint8_t> rgba;
        };
        void shade(int x, int y, Rgba source, uint32_t z);
        Rgba sample(const GsTriPixel &p) const;

        std::unordered_map<uint32_t, Texture> m_textures;
        GsState m_state{};
        GsPlan m_plan{};
        const Texture *m_tex = nullptr;
        bool m_haveState = false;
    };
}

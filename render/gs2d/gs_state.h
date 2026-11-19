#pragma once

// The draw list of the 2D packet (renderer plan P1 step 7; RENDERER_2D_DESIGN.md section 2, RENDERER_FRAME_RECORD.md
// section 9): the finite state block GsState, the vertex GsVertex, the 32-byte command and its payload arena.
// Coordinates, Z, colours and texture coordinates stay in GS units (12.4 fixed point before XYOFFSET, GS Z, raw 0x80
// alpha) until a replay converts them. Little-endian, no pointers, so a closed list is one block that can be published,
// kept in a ring or written to a dump. No dependencies, C++17.

#include <cstdint>
#include <cstring>
#include <vector>

namespace rc::rd
{
    // GS register addresses the 2D reader uses (context 1; context 2 is +1 for the paired ones).
    enum GsReg : uint8_t
    {
        kGsPrim = 0x00, kGsRgbaq = 0x01, kGsSt = 0x02, kGsUv = 0x03, kGsXyzf2 = 0x04, kGsXyz2 = 0x05,
        kGsTex0 = 0x06, kGsClamp = 0x08, kGsFog = 0x0A, kGsXyzf3 = 0x0C, kGsXyz3 = 0x0D,
        kGsTex1 = 0x14, kGsTex2 = 0x16, kGsXyoffset = 0x18, kGsPrmodecont = 0x1A, kGsPrmode = 0x1B, kGsTexclut = 0x1C,
        kGsScanmsk = 0x22, kGsMiptbp1 = 0x34, kGsMiptbp2 = 0x36, kGsTexa = 0x3B, kGsFogcol = 0x3D, kGsTexflush = 0x3F,
        kGsScissor = 0x40, kGsAlpha = 0x42, kGsDimx = 0x44, kGsDthe = 0x45, kGsColclamp = 0x46, kGsTest = 0x47,
        kGsPabe = 0x49, kGsFba = 0x4A, kGsFrame = 0x4C, kGsZbuf = 0x4E,
        kGsBitbltbuf = 0x50, kGsTrxpos = 0x51, kGsTrxreg = 0x52, kGsTrxdir = 0x53, kGsHwreg = 0x54,
        kGsSignal = 0x60, kGsFinish = 0x61, kGsLabel = 0x62,
    };
    constexpr uint32_t kGsRegFile = 0x63u; // the general registers, 0x00-0x62

    // Primitive types (PRIM bits 0-2).
    enum GsPrimType : uint8_t
    {
        kPrimPoint = 0, kPrimLine = 1, kPrimLineStrip = 2, kPrimTriangle = 3, kPrimTriStrip = 4, kPrimTriFan = 5, kPrimSprite = 6,
    };

    // The finite state block (RENDERER_2D_DESIGN.md 2.1): raw registers as the GS holds them, so the re-encoder of
    // check (b), the CPU model and the Vulkan replay read the same thing; everything derived comes from planState
    // (gs_plan.h). 128 bytes, zero-padded. The context registers are those of PRIM.CTXT. tex0, tex1, clamp, miptbp1/2
    // and texa are zero when TME is 0, alpha is zero when ABE is 0, so equal draws compare equal.
    struct GsState
    {
        uint64_t frame;      // FRAME_n     FBP 0-8, FBW 16-21, PSM 24-29, FBMSK 32-63
        uint64_t zbuf;       // ZBUF_n      ZBP 0-8, PSM 24-27, ZMSK 32
        uint64_t scissor;    // SCISSOR_n   SCAX0 0-10, SCAX1 16-26, SCAY0 32-42, SCAY1 48-58
        uint64_t xyoffset;   // XYOFFSET_n  OFX 0-15, OFY 32-47 (12.4)
        uint64_t test;       // TEST_n      ATE 0, ATST 1-3, AREF 4-11, AFAIL 12-13, DATE 14, DATM 15, ZTE 16, ZTST 17-18
        uint64_t alpha;      // ALPHA_n     A 0-1, B 2-3, C 4-5, D 6-7, FIX 32-39
        uint64_t tex0;       // TEX0_n      (tex_decode.h Tex0)
        uint64_t tex1;       // TEX1_n      LCM 0, MXL 2-4, MMAG 5, MMIN 6-8, MTBA 9, L 19-20, K 32-43
        uint64_t clamp;      // CLAMP_n     WMS 0-1, WMT 2-3, MINU 4-13, MAXU 14-23, MINV 24-33, MAXV 34-43
        uint64_t miptbp1, miptbp2;
        uint64_t texa;       // TEXA        TA0 0-7, AEM 15, TA1 32-39
        uint16_t prim;       // PRIM (or PRMODE with PRIM's type when PRMODECONT.AC is 0); type normalised to the
                             // primitive class: point 0, line 1 (strips too), triangle 3 (strips, fans), sprite 6
        uint8_t pabe, fba, colclamp, dthe; // bit 0 of each register
        uint16_t known;      // bit per GsGroup: the reader had those registers from the stream (0: seeded or default)
        uint32_t texture;    // TEXTURE id (0xffffffff when TME is 0)
        uint32_t textureGeneration;
        uint8_t texAlphaMin, texAlphaMax; // the texture's alpha range as decoded; 0, 0 without one
        uint8_t reserved[14];             // zero

        uint32_t primType() const { return prim & 7u; }
        bool iip() const { return (prim >> 3) & 1u; }
        bool tme() const { return (prim >> 4) & 1u; }
        bool fge() const { return (prim >> 5) & 1u; }
        bool abe() const { return (prim >> 6) & 1u; }
        bool aa1() const { return (prim >> 7) & 1u; }
        bool fst() const { return (prim >> 8) & 1u; }
        uint32_t ctxt() const { return (prim >> 9) & 1u; }
        bool operator==(const GsState &o) const { return std::memcmp(this, &o, sizeof(GsState)) == 0; }
        bool operator!=(const GsState &o) const { return !(*this == o); }
    };
    static_assert(sizeof(GsState) == 128, "GsState is 128 bytes");

    // GsState::known bits: register groups.
    enum GsGroup : uint16_t
    {
        kGrpFrame = 1u << 0, kGrpZbuf = 1u << 1, kGrpScissor = 1u << 2, kGrpXyoffset = 1u << 3, kGrpTest = 1u << 4,
        kGrpAlpha = 1u << 5, kGrpTex0 = 1u << 6, kGrpTex1 = 1u << 7, kGrpClamp = 1u << 8, kGrpMiptbp = 1u << 9,
        kGrpTexa = 1u << 10, kGrpPrim = 1u << 11, kGrpPabe = 1u << 12, kGrpFba = 1u << 13, kGrpColclamp = 1u << 14,
        kGrpDthe = 1u << 15,
    };
    constexpr uint32_t kGsGroupCount = 16u;
    const char *gsGroupName(uint32_t bit); // "FRAME", ... by bit index

    // A vertex as sent (RENDERER_2D_DESIGN.md 2.1), 32 bytes. Triangles from strips and fans are stored as separate
    // triangles in the GS's vertex order, each vertex with the RGBAQ, ST and UV latched when it was sent (with IIP 0 the
    // GS uses the last vertex's colour). As built: the design's `uint32_t reserved` holds the fog byte and whether the
    // vertex came as XYZF2/XYZF3, so the re-encoder writes back exactly what was sent.
    struct GsVertex
    {
        uint16_t x, y;   // 12.4 as written, before XYOFFSET
        uint32_t z;      // as written (24 bits from an XYZF register)
        uint8_t r, g, b, a;
        float s, t, q;
        uint16_t u, v;   // UV as written (FST)
        uint8_t fog;     // XYZF's F, or 0
        uint8_t flags;   // kVertexXyzf
        uint16_t reserved;
    };
    static_assert(sizeof(GsVertex) == 32, "GsVertex is 32 bytes");
    enum : uint8_t
    {
        kVertexXyzf = 1u,
    };

    // Command ops (RENDERER_FRAME_RECORD.md section 9 and RENDERER_2D_DESIGN.md section 2).
    enum DrawOp : uint16_t
    {
        kOpImageBlit = 1,
        kOpTexWrite = 2,
        kOpFlip = 3,
        kOpState = 4,       // a: flags (bit 0: the state is unsupported, planState's `unsupported` is set); payload GsState
        kOpTexture = 5,     // a: texture id, b: generation, c: width | height << 16; payload RGBA8 texels, raw GS alpha
        kOpClear = 6,       // a: RGBA, b: Z; payload 2 GsVertex (the sprite as sent)
        kOpScreenPrims = 7, // a: kind (PrimKind), b: count, c: flags, d: first primitive's index in the cycle;
                            // payload GsVertex x 1, 2 or 3 per primitive
        kOpShapeBatch = 8,
        kOpScopeBegin = 9,  // a: tag kind, b: tag sequence number, c: the tag's guest address; payload the tag's
        kOpScopeEnd = 10,
    };
    // SCREEN_PRIMS a (as built: points and sprites other than the clear are kept as kinds 0 and 3, never seen on
    // PATH3, so check (b) re-encodes everything the reader saw).
    enum PrimKind : uint32_t
    {
        kKindPoints = 0, kKindLines = 1, kKindTriangles = 2, kKindSprites = 3,
    };
    inline uint32_t verticesPerPrim(uint32_t kind) { return kind == kKindPoints ? 1u : kind == kKindTriangles ? 3u : 2u; }
    // SCREEN_PRIMS c
    enum : uint32_t
    {
        kPrimsOneColour = 1u,  // every primitive has one colour at all its vertices
        kPrimsRectPairs = 2u,  // consecutive triangle pairs are axis-aligned rectangles
    };

    struct DrawCmd
    {
        uint16_t op;
        uint16_t flags;
        uint32_t a, b, c;
        uint32_t payloadOffset, payloadBytes;
        uint64_t d;
    };
    static_assert(sizeof(DrawCmd) == 32, "DrawCmd is 32 bytes");

    // One display cycle's list: commands and their payload arena.
    struct DrawList
    {
        std::vector<DrawCmd> cmds;
        std::vector<uint8_t> payload;

        void clear()
        {
            cmds.clear();
            payload.clear();
        }
        DrawCmd &add(uint16_t op, const void *data = nullptr, uint32_t bytes = 0)
        {
            DrawCmd c{};
            c.op = op;
            payload.resize((payload.size() + 7u) & ~size_t{7}, 0u); // every payload starts 8-byte aligned
            c.payloadOffset = static_cast<uint32_t>(payload.size());
            c.payloadBytes = bytes;
            if (bytes)
                payload.insert(payload.end(), static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + bytes);
            cmds.push_back(c);
            return cmds.back();
        }
        // Appends to the last command's payload (it must be the last one written).
        void append(const void *data, uint32_t bytes)
        {
            payload.insert(payload.end(), static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + bytes);
            cmds.back().payloadBytes += bytes;
        }
        // The arena is a std::vector<uint8_t> (heap storage, at least 16-byte aligned) and payloads start at multiples
        // of 8, so these views are aligned.
        const uint8_t *data(const DrawCmd &c) const { return payload.data() + c.payloadOffset; }
        const GsState &state(const DrawCmd &c) const { return *reinterpret_cast<const GsState *>(data(c)); }
        const GsVertex *vertices(const DrawCmd &c) const { return reinterpret_cast<const GsVertex *>(data(c)); }
    };
}

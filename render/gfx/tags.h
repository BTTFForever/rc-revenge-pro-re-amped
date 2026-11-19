#pragma once

// Side tags (RENDERER_FRAME_RECORD.md section 5): native code marks what it is about to put into the frame's packets
// (begin / end anchored to the guest address it will write next: the 2D buffer position or the next chain slot), the
// kicks it is about to make (kick spans) and frame state (scene, view). The frame record resolves anchors to stream
// offsets at the kick; later the renderer reads them as draw keys.
//
// Rules for every call site (they keep game behaviour, guest memory, EE cycles and the oracle untouched):
//   1. Only in native functions, after any RC_DIVERT_IF_CHANGED. Arguments are built inside `if (rc::gfx::live())` or
//      by a Span* constructor, from values the function already has: no guest_call, no guest_checkpoint, no hardware
//      access, no allocation from the game's heap.
//   2. The channel reads guest RAM (the anchors) and never writes it; it charges no EE cycles and cannot yield.
//   3. detail::g_live is "a recorder is attached and no comparison runs": every change of rc_decomp.cpp's comparison
//      state goes through detail::setComparing, so random-input runs and both halves of a live comparison log nothing.
//   4. Nothing reads a tag back into game logic: the channel has no query functions.
//
// RC_GFX_TAGS is 1 only for the game's own target (CMakeLists.txt); everywhere else, the oracle included, it is 0 and
// this header compiles every call to nothing (no symbol of render/gfx/tags.cpp is referenced).
//
// The payload structs and enumerators below are the dump's tag layout (format version 1; little-endian, no pointers,
// sizes multiples of 8) and are always defined, so tools can read them.

#include <cstdint>

#ifndef RC_GFX_TAGS
#define RC_GFX_TAGS 0
#endif

namespace rc::gfx
{
    // Tag kinds (TAG.kind).
    enum Kind : uint16_t
    {
        KindScene = 1,
        KindView = 2,
        KindPass = 3,
        KindDraw = 4,
        KindHud = 5,
        KindKick = 6,
        KindLabel = 7,
        KindCut = 8,
    };
    // TAG.op
    enum Op : uint8_t
    {
        OpSet = 0,
        OpBegin = 1,
        OpEnd = 2,
    };
    // DrawKey.kind
    enum DrawKind : uint32_t
    {
        DrawShape = 1,  // TRdr_VUShape: object = the sRdrVUShape
        DrawSkin = 2,   // Rdr_VUSkin: object = the sRdrVUSkin
        DrawHud = 3,    // a Hud* panel function: object = the car, part = HudElement, ordinal = player
        DrawFont = 4,   // FontPrint: object = the string, part = x, ordinal = y (as written)
    };
    // HudTag.element
    enum HudElement : uint32_t
    {
        HudPanel = 1,
        HudTopLeft = 2,
        HudTopRight = 3,
        HudBottomRight = 4,
        HudBottomLeft = 5,
        HudMiddle = 6,
    };
    // HudTag.anchor: where the element sits on the screen (for widescreen placement later)
    enum HudAnchor : uint32_t
    {
        AnchorLeft = 1,
        AnchorRight = 2,
        AnchorTop = 3,
        AnchorBottom = 4,
        AnchorCentre = 5,
        AnchorStretch = 6,
        AnchorTopLeft = 7,
        AnchorTopRight = 8,
        AnchorBottomLeft = 9,
        AnchorBottomRight = 10,
    };
    // KickLabel.what
    enum KickWhat : uint32_t
    {
        KickK1 = 1,              // the 2D packet (GfxP2_RenderDmaSendActual): object = the buffer, a = quadwords
        KickK2 = 2,              // the frame's VIF1 chain (Rdr_EndFrame): object = the first tag, a = gRdr.frameNumber
        KickTextureUpload = 3,   // GfxP2_LoadTex / GfxP2_LoadTexMip: object = the _sGfxTexture, a = GS block, b = level | frame << 8
        KickAnimatedTexture = 4, // GfxTex_Update (all its uploads): a = animated texture count
        KickBackdrop = 5,        // BackdropDisplayBMP24: object = the bitmap, a = buffer
        KickMovie = 6,           // ldimage's loadImage (a movie picture's tiles): object = the first tag
        KickFlip = 7,            // sceGsSwapDBuff (GfxP2_SwapBuffers): the flip and the next frame's draw environment and
                                 // clear (that kick opens the next cycle): object = the sceGsDBuff, a = gRdr.frameNumber
    };

    struct DrawKey
    {
        uint32_t kind, object, part, ordinal; // object: a guest address used as an identity only
    };

    // Payloads (the bytes after TAG's body).
    struct SceneTag // KindScene, op set
    {
        uint64_t gameFrame;   // the GfxP2_SwapBuffers override's counter
        uint32_t rdrFrame;    // gRdr.frameNumber
        uint32_t scene;       // the GS survey's id: mode << 12 | level << 4 | split << 1
        uint32_t flags;
        uint32_t pad;
    };
    static_assert(sizeof(SceneTag) == 24);

    struct ViewTag // KindView, op set
    {
        uint32_t index;          // 0, 1 (split screen)
        int32_t scissor[4];      // x0 y0 x1 y1 (GS scissor, inclusive)
        float yOffset;           // the vertical draw offset (gRdrPolyOffset)
        float position[3];       // the camera (_sRdrCamera.pos)
        float rotation[9];       // _sRdrCamera.mat, row by row
        float dVal, dValX, dValY, minZ, maxZ;
        uint32_t viewAngle;
        uint32_t camera;         // the _sRdrCamera's guest address (identity)
        uint32_t pad;
    };
    static_assert(sizeof(ViewTag) == 104);

    struct PassTag // KindPass, op set
    {
        uint32_t pass, pad;
    };

    struct DrawTag // KindDraw, op begin; followed by float[12] (3 x 4: rotation rows then position) when hasMatrix
    {
        DrawKey key;
        uint32_t flags;
        uint32_t hasMatrix;
    };
    static_assert(sizeof(DrawTag) == 24);

    struct HudTag // what a call site passes
    {
        uint32_t element; // HudElement
        uint32_t anchor;  // HudAnchor
        float box[4];     // x0 y0 x1 y1 in the game's 2D units; all 0 = not known
    };
    struct HudPayload // KindHud, op begin
    {
        DrawKey key;
        HudTag hud;
    };
    static_assert(sizeof(HudPayload) == 40);

    struct KickLabel // KindKick, op begin
    {
        uint32_t what, object, a, b;
    };
    static_assert(sizeof(KickLabel) == 16);

    struct CutTag // KindCut, op set (reserved)
    {
        uint32_t reason, pad;
    };

    // How a begin/end is anchored (not in the dump: TAG.anchor holds the resolved guest address).
    enum class AnchorKind : uint8_t
    {
        None = 0,      // a set tag or a kick span
        Buffer2D = 1,  // gGfxP2.pDmaBuff + buffIdx * 16
        ChainSlot = 2, // &gRdr.pDmaTag[currDmaTag]
        Address = 3,   // beginAt's address (the end repeats it)
    };

    // The name table written as the dump's TNAM chunk: one "table value name" line per enumerator above.
    const char *nameTable();

#if RC_GFX_TAGS
    namespace detail
    {
        extern bool g_live;      // enabled && !comparing
        extern bool g_enabled;   // a recorder (or later the renderer) is attached
        extern bool g_comparing; // a comparison or random-input run is in progress
        inline void setComparing(bool comparing)
        {
            g_comparing = comparing;
            g_live = g_enabled && !comparing;
        }

        // One tag as the channel hands it to the attached consumer (the frame record's capture).
        struct Event
        {
            uint32_t seq;
            uint16_t kind;
            uint8_t op, depth;
            uint32_t anchor;       // guest physical address (RDRAM offset, or 0x70000000 + scratchpad offset); 0 = none
            uint32_t parent;       // the enclosing begin's seq; for an end, its begin's
            AnchorKind anchorKind; // None for set tags and kick spans
            const void *payload;
            uint32_t payloadBytes;
        };
        struct Sink
        {
            virtual void tag(const Event &) = 0;
            virtual ~Sink() = default;
        };
        // Attaches the consumer (null detaches). Tags flow while one is attached and no comparison runs.
        void attach(Sink *sink);
        // The game registers how to read the two buffer anchors (decomp/rc_gfx_tags.cpp); each returns a guest address.
        void setAnchorReader(AnchorKind kind, uint32_t (*reader)());
        // A guest address as TAG.anchor stores it (0 when it does not translate).
        uint32_t physical(uint32_t guestAddress);
    }

    inline bool live() { return detail::g_live; }

    void scene(const SceneTag &);
    void view(const ViewTag &);
    void pass(uint32_t pass);
    uint32_t begin2D(const DrawKey &, const HudTag *hud = nullptr);          // anchor: the 2D buffer's write position
    uint32_t beginChain(const DrawKey &, const float *matrix3x4 = nullptr); // anchor: the next chain slot
    uint32_t beginAt(uint32_t guestAddress, const DrawKey &);
    void end(uint32_t token);                                               // anchor of the same kind, read now
    uint32_t beginKicks(const KickLabel &);
    void endKicks(uint32_t token);
    void label(const char *text);
#else
    namespace detail
    {
        inline void setComparing(bool) {}
    }
    constexpr bool live() { return false; }
    inline void scene(const SceneTag &) {}
    inline void view(const ViewTag &) {}
    inline void pass(uint32_t) {}
    inline uint32_t begin2D(const DrawKey &, const HudTag * = nullptr) { return 0; }
    inline uint32_t beginChain(const DrawKey &, const float * = nullptr) { return 0; }
    inline uint32_t beginAt(uint32_t, const DrawKey &) { return 0; }
    inline void end(uint32_t) {}
    inline uint32_t beginKicks(const KickLabel &) { return 0; }
    inline void endKicks(uint32_t) {}
    inline void label(const char *) {}
#endif

    // Scoped spans: the constructor opens one only while tags are live; the destructor closes what it opened.
    struct Span2D
    {
        uint32_t t;
        explicit Span2D(const DrawKey &k, const HudTag *h = nullptr) : t(live() ? begin2D(k, h) : 0u) {}
        ~Span2D()
        {
            if (t)
                end(t);
        }
        Span2D(const Span2D &) = delete;
        Span2D &operator=(const Span2D &) = delete;
    };
    struct SpanChain
    {
        uint32_t t;
        explicit SpanChain(const DrawKey &k, const float *matrix3x4 = nullptr) : t(live() ? beginChain(k, matrix3x4) : 0u) {}
        ~SpanChain()
        {
            if (t)
                end(t);
        }
        SpanChain(const SpanChain &) = delete;
        SpanChain &operator=(const SpanChain &) = delete;
    };
    struct SpanKicks
    {
        uint32_t t;
        explicit SpanKicks(const KickLabel &k) : t(live() ? beginKicks(k) : 0u) {}
        ~SpanKicks()
        {
            if (t)
                endKicks(t);
        }
        SpanKicks(const SpanKicks &) = delete;
        SpanKicks &operator=(const SpanKicks &) = delete;
    };
}

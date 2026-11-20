#pragma once

// The 2D packet drawn on Vulkan (renderer plan P1 step 7 batch 2; RENDERER_2D_DESIGN.md sections 4-8): a draw list
// (gs_state.h) replayed through the vendored RHI into two RGBA8 colour targets (FBP 0x0 and 0x50, 640 x 256) and one
// D32F depth texture holding Z * 2^-24.
//
// Every primitive becomes a record in one storage buffer (Gs2dPrim): its window coordinates, colours, texture
// coordinates, Z, the reference's edge functions and 1 / area (as double bits) and the plan's per-pixel parameters.
// The vertex shader only places vertices (GS coordinates moved half a pixel, a 4096 x 4096 viewport at -2048 so the
// positions are exact); gs2d.frag computes everything per pixel as the software GS does (weights in doubles, colour,
// texture coordinates, the bilinear filter, texture function, alpha test, the source term of the blend and Z). The
// blend is fixed function with dual-source factors (4.2: OFF, MIX, MIXSUB), and every combination of blend
// configuration x {triangles, points} x depth compare x depth write x colour mask (and the RGB_ONLY-without-Z alpha
// blend) is a pipeline made at create(). Lines are expanded on the CPU (gs_model's expandLine) to one point a pixel;
// the clear is two flat triangles. RGB_ONLY / FB_ONLY / ZB_ONLY with a write on failing pixels (plan.split) is drawn as
// two draws per group of primitives that cannot overlap (5.2), the alpha decided on the CPU where it is one value.
//
// The exact blend (batch 4b, RENDERER_2D_DESIGN.md 18 and 19): where the device has dynamic rendering local read
// (RhiLimits.localRead), the plans with a fractional blend factor (not exactBlend) are drawn by the READ pipelines
// instead: blend disabled, gs2d.frag's GS2D_DSTREAD build reads its own pixel of the target as an input attachment and
// computes the GS rule in integers. Their batches are cut into groups of primitives that cannot overlap (the split's
// rule), and a by-region barrier goes before each group when anything was drawn in the pass since the last one; the
// pass is begun readable. Every other plan is drawn through fixed function as before, in the same pass. Without local
// read (or with Options::blend kBlendFixed) every plan is fixed function: divergence D-2D-1.
//
// Needs a device made with rhi_init (headless is enough) offering dual-source blending, a D32F attachment and
// shaderFloat64 (the RHI edit of decision U2): create() refuses otherwise. One thread.

#include "gs_plan.h"
#include "gs_state.h"

#include "rhi.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace rc::rd
{
    // One primitive (or one flat triangle of the clear, or one point of a line) as the shaders read it: 44 words,
    // std430 (all 4-byte scalars).
    struct Gs2dPrim
    {
        int32_t px[3], py[3];      // window coordinates in 1/16 pixel (X - OFX); flat records: placed so the vertex
                                   // shader's half pixel lands where it should (points: pixel * 16)
        uint32_t rgba[3];          // vertex colours (flat records: rgba[2])
        float s[3], t[3], q[3];
        uint32_t z[3];             // vertex Z as sent (flat records: the final Z in z[2])
        uint32_t invArea[2];       // the double 1.0 / area, low and high words
        int32_t ea[3], eb[3];      // edge functions, area made positive (gsTriSetup)
        uint32_t ec[6];            // c[i] as double bits, low and high words
        uint32_t flags;            // Gs2dPrimFlags
        uint32_t blend;            // term 0-1, factor 2-3, ks 4-7, kd 8-11, config 12-13, fix 16-23, abcd 24-31
        uint32_t test;             // atst 0-2, aref 4-11, tfx 12-13, tcc 14, linear 15, wrapU 16-17, wrapV 18-19, tw 20-23, th 24-27
        uint32_t clampU, clampV;   // region MIN | MAX << 16
        uint32_t fault;            // shader-side planted faults (Gs2dVk::kFault* >> 16)
        uint32_t reserved[3];
    };
    static_assert(sizeof(Gs2dPrim) == 44 * 4, "Gs2dPrim is 44 words");

    enum Gs2dPrimFlags : uint32_t
    {
        kPrimFlat = 1u,          // no weights: colour rgba[2], Z z[2] (the clear, line pixels, points)
        kPrimIip = 2u,
        kPrimTme = 4u,
        kPrimFst = 8u,
        kPrimPartShift = 4u,     // bits 4-5: 0 discard where the alpha test fails, 2 discard where it passes, 3 alpha
                                 // by blend (no discard; out1.a 1 where it passes, 0 where it fails)
        kPrimBias = 1u << 6,     // the source bias of 4.3 is on
        kPrimEdgeBiasShift = 8u, // bits 8-10: the edge functions' top-left bias
        kPrimProbeUv = 1u << 11, // probe build: write uf, vf, fu, fv instead of w0, w1, w2, invQ
    };
    enum Gs2dPart : uint32_t
    {
        kPartPass = 0, kPartFail = 2, kPartAlphaBlend = 3,
    };

    class Gs2dVk
    {
    public:
        static constexpr uint32_t kWidth = 640, kHeight = 256;
        // Planted faults (RENDERER_2D_DESIGN.md 9.3); the TEXA fault is the reader's (Gif2dReader::Options).
        enum Fault : uint32_t
        {
            kFaultAtstLess = 1u << 0,       // ATST LEQUAL drawn as LESS
            kFaultDropFail = 1u << 1,       // the failing part of the split left out
            kFaultSplitPerBatch = 1u << 2,  // the split issued per batch instead of per group
            kFaultZGequal = 1u << 3,        // ZTST GREATER drawn as GEQUAL
            kFaultFixOffByOne = 1u << 4,    // FIX + 1
            kFaultColourSwap = 1u << 5,     // a vertex colour's R and G swapped
            kFaultNoBarrier = 1u << 6,      // (read mode) the barrier before a reading group left out
            kFaultOneGroup = 1u << 7,       // (read mode) a reading batch drawn as one group
            kFaultNoHalfPixel = 1u << 16,   // (shader) the half pixel left out of the position
            kFaultBiasAlways = 1u << 17,    // (shader) the bias applied where a < b
            kFaultTexelRow = 1u << 18,      // (shader) the texel fetch one row down
        };
        // How the plans with a fractional blend factor are drawn (RENDERER_2D_DESIGN.md 18.2).
        enum BlendMode : uint32_t
        {
            kBlendAuto = 0,  // the read where the device has local read, else fixed function
            kBlendFixed = 1, // fixed function (dual-source, 4.2), the bias as sourceBias says: D-2D-1
            kBlendRead = 2,  // the destination read; create() fails without local read
        };
        struct Options
        {
            BlendMode blend = kBlendAuto;
            // 4.3's source bias, for the fixed-function blend only (never set on a reading record). On: per inexact
            // layer -1..+1 against the GS on the RTX 2060's 8-bit blender, against -1..+2 without it (RENDERER_2D_DESIGN.md
            // 15.3, the batch 4 review's choice in 17.1).
            bool sourceBias = true;
            uint32_t faults = 0;
        };
        struct Stats
        {
            uint64_t lists = 0, draws = 0, prims = 0, records = 0, linePixels = 0, points = 0, clears = 0, textures = 0;
            uint64_t splitPrims = 0, splitGroups = 0, decidedOnCpu = 0, unsupported = 0, spritesSkipped = 0;
            uint64_t culled = 0, beyondViewport = 0, zClamped = 0;
            uint64_t readPrims = 0, readGroups = 0, barriers = 0; // the exact blend: primitives drawn by READ, their groups, barriers issued
        };
        // The last draw()'s figures: reading primitives, groups and barriers, and the GPU time of its command list
        // (timestamps from before the first pass to after the last; 0 where the device has no timestamps).
        struct ListStats
        {
            uint64_t readPrims = 0, readGroups = 0, barriers = 0, draws = 0;
            double gpuMs = 0.0;
        };

        Gs2dVk() = default;
        ~Gs2dVk();
        Gs2dVk(const Gs2dVk &) = delete;
        Gs2dVk &operator=(const Gs2dVk &) = delete;

        // Targets, shaders and every pipeline. False (and *why) when the device lacks a feature or creation failed.
        bool create(std::string *why = nullptr);
        void destroy();
        void setOptions(const Options &o) { m_opt = o; }
        const Options &options() const { return m_opt; }

        // The colour target `target` (0: FBP 0x0, 1: FBP 0x50) and the depth texture from CPU planes (640 x 256 RGBA
        // words; Z values below 2^24), both required.
        bool seed(uint32_t target, const uint32_t *rgba, const uint32_t *z);
        // Replays a draw list (TEXTURE, STATE, SCREEN_PRIMS, CLEAR; scope and other commands are skipped) into the targets.
        bool draw(const DrawList &list);
        // Read-back: the colour target as RGBA words, the depth as Z values (false if a depth is not an integer Z).
        bool readColour(uint32_t target, std::vector<uint32_t> &rgba);
        bool readDepth(std::vector<uint32_t> &z);
        // T7: the triangles of `list` drawn with the probe shader into an RGBA32_UINT target: per pixel the bits of
        // w0, w1, w2, invQ (uv false) or of uf, vf, fu, fv (uv true); 640 x 256 x 4 words, zero where nothing drew.
        bool probe(const DrawList &list, bool uv, std::vector<uint32_t> &out);

        const Stats &stats() const { return m_stats; }
        const ListStats &lastList() const { return m_last; }
        // The device has local read (known after create()), and whether the options in force draw by the read.
        bool readAvailable() const { return m_haveRead; }
        bool readMode() const { return m_haveRead && m_opt.blend != kBlendFixed; }

    private:
        struct Draw
        {
            uint32_t target, key, first, count;
            RhiTexture texture;
            RhiRect scissor;
            bool points;
            bool read;    // a READ pipeline (key: readKey)
            bool barrier; // the first draw of a reading group: a barrier before it unless nothing was drawn since the last
        };
        bool build(const DrawList &list, std::vector<Draw> &draws, std::vector<RhiTexture> &fresh, RhiCommandList cl);
        bool upload(const std::vector<Gs2dPrim> &records);
        void transition(RhiCommandList cl, RhiTexture t, RhiState &current, RhiState to);
        uint32_t pipelineKey(uint32_t config, bool alphaBlend, bool points, uint32_t compare, bool depthWrite, uint32_t mask) const;
        uint32_t readKey(bool points, uint32_t compare, bool depthWrite, uint32_t mask) const;

        Options m_opt;
        Stats m_stats;
        ListStats m_last;
        bool m_created = false, m_haveRead = false;
        RhiBindGroupLayout m_layouts[3]{};
        RhiBindGroupLayout m_readLayouts[3]{}; // group 0: the target as an input attachment (slot 0); groups 1 and 2 shared
        RhiShader m_vert{}, m_vertPoints{}, m_frag{}, m_fragProbe{}, m_fragRead{}, m_seedVert{}, m_seedFrag{};
        std::vector<RhiPipeline> m_pipelines; // by pipelineKey
        std::vector<RhiPipeline> m_readPipelines; // by readKey (64, where local read)
        RhiPipeline m_seedPipeline{}, m_probePipeline{};
        RhiSampler m_sampler{};
        RhiTexture m_targets[2]{}, m_depth{}, m_probeTarget{}, m_dummy{};
        RhiState m_targetState[2]{RHI_STATE_UNDEFINED, RHI_STATE_UNDEFINED}, m_depthState = RHI_STATE_UNDEFINED;
        RhiState m_probeState = RHI_STATE_UNDEFINED;
        RhiBuffer m_records{};
        uint64_t m_recordBytes = 0;
        std::unordered_map<uint32_t, RhiTexture> m_textures; // TEXTURE id -> its latest upload
        std::vector<RhiTexture> m_retired;                    // replaced uploads, destroyed after the next submit
        std::vector<Gs2dPrim> m_scratch;
    };
}

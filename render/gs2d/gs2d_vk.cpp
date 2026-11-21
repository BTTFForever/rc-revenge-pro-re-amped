#include "gs2d_vk.h"

#include "gs_model.h"
#include "../tex/tex_decode.h"

#include "gs2d_frag.h"
#include "gs2d_probe_frag.h"
#include "gs2d_read_frag.h"
#include "gs2d_points_vert.h"
#include "gs2d_seed_frag.h"
#include "gs2d_seed_vert.h"
#include "gs2d_vert.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <unordered_set>

namespace rc::rd
{
    namespace
    {
        constexpr uint32_t kConfigs = 3u, kMasks = 4u;
        const uint8_t kMaskValues[kMasks] = {0u, kMaskA, kMaskRgb, kMaskRgba};

        RhiShader makeShader(RhiShaderStage stage, const uint32_t *code, size_t size, const char *name)
        {
            RhiShaderDesc desc{};
            desc.stage = stage;
            desc.bytecode = code;
            desc.bytecodeSize = size;
            desc.entryPoint = "main";
            desc.debugName = name;
            return rhi_create_shader(&desc);
        }

        uint32_t maskIndex(uint8_t mask)
        {
            for (uint32_t i = 0; i < kMasks; ++i)
                if (kMaskValues[i] == mask)
                    return i;
            return kMasks - 1u;
        }

        RhiCompare compareOf(uint32_t ztst)
        {
            switch (ztst & 3u)
            {
            case kDepthNever: return RHI_CMP_NEVER;
            case kDepthAlways: return RHI_CMP_ALWAYS;
            case kDepthGequal: return RHI_CMP_GEQUAL;
            default: return RHI_CMP_GREATER;
            }
        }

        void doubleBits(double d, uint32_t &lo, uint32_t &hi)
        {
            uint64_t b;
            std::memcpy(&b, &d, 8);
            lo = static_cast<uint32_t>(b);
            hi = static_cast<uint32_t>(b >> 32);
        }

        uint32_t packRgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
        {
            return r | static_cast<uint32_t>(g) << 8 | static_cast<uint32_t>(b) << 16 | static_cast<uint32_t>(a) << 24;
        }

        struct Box
        {
            int x0, y0, x1, y1;
            bool disjoint(const Box &o) const { return x1 < o.x0 || o.x1 < x0 || y1 < o.y0 || o.y1 < y0; }
        };

        // The two triangles are halves of one axis-aligned rectangle split along its diagonal (they share only the
        // diagonal, which the fill rule gives to one of them). The two shared corners must be opposite ones: two
        // triangles on three corners each that share a side of the rectangle are cut along different diagonals and
        // overlap (step 8 batch 2: PATH1 sends such pairs, USA level 17 cycle 279; K1's pairs are always halves).
        bool rectangleHalves(const GsVertex *a, const GsVertex *b)
        {
            uint16_t xs[2], ys[2];
            uint32_t nx = 0, ny = 0;
            const GsVertex *v[6] = {a, a + 1, a + 2, b, b + 1, b + 2};
            for (const GsVertex *p : v)
            {
                bool hx = false, hy = false;
                for (uint32_t i = 0; i < nx; ++i)
                    hx |= xs[i] == p->x;
                for (uint32_t i = 0; i < ny; ++i)
                    hy |= ys[i] == p->y;
                if (!hx)
                {
                    if (nx == 2)
                        return false;
                    xs[nx++] = p->x;
                }
                if (!hy)
                {
                    if (ny == 2)
                        return false;
                    ys[ny++] = p->y;
                }
            }
            if (nx != 2 || ny != 2)
                return false;
            uint32_t shared = 0;
            const GsVertex *sh[3] = {nullptr, nullptr, nullptr};
            for (int i = 0; i < 3; ++i)
            {
                for (int j = i + 1; j < 3; ++j)
                    if ((a[i].x == a[j].x && a[i].y == a[j].y) || (b[i].x == b[j].x && b[i].y == b[j].y))
                        return false;
                for (int j = 0; j < 3; ++j)
                    if (a[i].x == b[j].x && a[i].y == b[j].y)
                    {
                        if (shared < 3u)
                            sh[shared] = a + i;
                        ++shared;
                    }
            }
            return shared == 2u && sh[0]->x != sh[1]->x && sh[0]->y != sh[1]->y;
        }
    }

    Gs2dVk::~Gs2dVk() { destroy(); }

    uint32_t Gs2dVk::pipelineKey(uint32_t config, bool alphaBlend, bool points, uint32_t compare, bool depthWrite, uint32_t mask) const
    {
        return (((((config * 2u + (alphaBlend ? 1u : 0u)) * 2u + (points ? 1u : 0u)) * 4u + (compare & 3u)) * 2u + (depthWrite ? 1u : 0u)) * kMasks) +
               maskIndex(static_cast<uint8_t>(mask));
    }

    uint32_t Gs2dVk::readKey(bool points, uint32_t compare, bool depthWrite, uint32_t mask) const
    {
        return (((points ? 1u : 0u) * 4u + (compare & 3u)) * 2u + (depthWrite ? 1u : 0u)) * kMasks + maskIndex(static_cast<uint8_t>(mask));
    }

    bool Gs2dVk::create(std::string *why)
    {
        auto fail = [&](const char *w)
        {
            if (why)
                *why = w;
            destroy();
            return false;
        };
        const RhiLimits *lim = rhi_limits();
        if (!lim)
            return fail("no device");
        if (!lim->dualSourceBlend)
            return fail("the device has no dual-source blending (RENDERER_2D_DESIGN.md 4.2)");
        if (!lim->depth32Attachment)
            return fail("the device has no D32F depth attachment");
        if (!lim->shaderFloat64)
            return fail("the device has no shaderFloat64 (decision U2's fallback is not built)");
        m_haveRead = lim->localRead;
        if (!m_haveRead && m_opt.blend == kBlendRead)
            return fail("the exact blend was asked for, but the device has no dynamic rendering local read (RhiLimits.localRead)");
        if (!m_haveRead)
            std::fprintf(stderr, "gs2d: the device has no local read: the fractional blends are drawn by fixed function with the source "
                                 "bias (divergence D-2D-1, RENDERER_2D_DESIGN.md 12)\n");

        static const RhiBindSlot recordSlots[1] = {{0u, RHI_BIND_STORAGE_BUFFER, (1u << RHI_STAGE_VERTEX) | (1u << RHI_STAGE_FRAGMENT)}};
        static const RhiBindSlot textureSlots[3] = {{1u, RHI_BIND_SAMPLED_TEXTURE, 1u << RHI_STAGE_FRAGMENT},
                                                    {2u, RHI_BIND_SAMPLED_TEXTURE, 1u << RHI_STAGE_FRAGMENT},
                                                    {1u, RHI_BIND_SAMPLER, 1u << RHI_STAGE_FRAGMENT}};
        const RhiBindGroupLayoutDesc g0{nullptr, 0u, "gs2d group0"};
        const RhiBindGroupLayoutDesc g1{recordSlots, 1u, "gs2d records"};
        const RhiBindGroupLayoutDesc g2{textureSlots, 3u, "gs2d textures"};
        m_layouts[0] = rhi_create_bind_group_layout(&g0);
        m_layouts[1] = rhi_create_bind_group_layout(&g1);
        m_layouts[2] = rhi_create_bind_group_layout(&g2);
        m_vert = makeShader(RHI_STAGE_VERTEX, spv_gs2d_vert, sizeof(spv_gs2d_vert), "gs2d.vert");
        m_vertPoints = makeShader(RHI_STAGE_VERTEX, spv_gs2d_points_vert, sizeof(spv_gs2d_points_vert), "gs2d.vert points");
        m_frag = makeShader(RHI_STAGE_FRAGMENT, spv_gs2d_frag, sizeof(spv_gs2d_frag), "gs2d.frag");
        m_fragProbe = makeShader(RHI_STAGE_FRAGMENT, spv_gs2d_probe_frag, sizeof(spv_gs2d_probe_frag), "gs2d.frag probe");
        m_seedVert = makeShader(RHI_STAGE_VERTEX, spv_gs2d_seed_vert, sizeof(spv_gs2d_seed_vert), "gs2d_seed.vert");
        m_seedFrag = makeShader(RHI_STAGE_FRAGMENT, spv_gs2d_seed_frag, sizeof(spv_gs2d_seed_frag), "gs2d_seed.frag");
        if (!m_layouts[0].id || !m_layouts[1].id || !m_layouts[2].id || !m_vert.id || !m_vertPoints.id || !m_frag.id || !m_fragProbe.id ||
            !m_seedVert.id || !m_seedFrag.id)
            return fail("shaders or layouts");
        if (m_haveRead)
        {
            // The exact blend: group 0 holds the target as an input attachment (binding 16, gs2d.frag GS2D_DSTREAD).
            static const RhiBindSlot dstSlots[1] = {{0u, RHI_BIND_INPUT_ATTACHMENT, 1u << RHI_STAGE_FRAGMENT}};
            const RhiBindGroupLayoutDesc g0r{dstSlots, 1u, "gs2d destination"};
            m_readLayouts[0] = rhi_create_bind_group_layout(&g0r);
            m_readLayouts[1] = m_layouts[1];
            m_readLayouts[2] = m_layouts[2];
            m_fragRead = makeShader(RHI_STAGE_FRAGMENT, spv_gs2d_read_frag, sizeof(spv_gs2d_read_frag), "gs2d.frag dstread");
            if (!m_readLayouts[0].id || !m_fragRead.id)
                return fail("the exact blend's shader or layout");
        }
        RhiSamplerDesc sd{};
        sd.mag = sd.min = sd.mip = RHI_FILTER_NEAREST;
        sd.s = sd.t = RHI_WRAP_CLAMP;
        sd.maxAnisotropy = 1.0f;
        m_sampler = rhi_create_sampler(&sd);

        // Every pipeline (4.2 x {triangles, points} x depth compare x depth write x colour mask, and 5.2's alpha by
        // blend), made at once.
        const uint32_t total = kConfigs * 2u * 2u * 4u * 2u * kMasks;
        m_pipelines.assign(total, RhiPipeline{});
        std::vector<RhiPipelineDesc> descs;
        std::vector<uint32_t> keys;
        for (uint32_t config = 0; config < kConfigs; ++config)
            for (uint32_t ab = 0; ab < 2u; ++ab)
                for (uint32_t points = 0; points < 2u; ++points)
                    for (uint32_t cmp = 0; cmp < 4u; ++cmp)
                        for (uint32_t zw = 0; zw < 2u; ++zw)
                            for (uint32_t m = 0; m < kMasks; ++m)
                            {
                                if (ab && zw)
                                    continue; // alpha by blend is RGB_ONLY without a depth write
                                RhiPipelineDesc d{};
                                d.vertex = points ? m_vertPoints : m_vert;
                                d.fragment = m_frag;
                                d.layouts = m_layouts;
                                d.layoutCount = 3u;
                                d.topology = points ? RHI_TOPO_POINT_LIST : RHI_TOPO_TRIANGLE_LIST;
                                d.cullNone = true;
                                RhiBlendState &b = d.blend[0];
                                b.enable = config != kBlendOff || ab;
                                b.srcColor = RHI_BF_ONE;
                                b.dstColor = config == kBlendOff ? RHI_BF_ZERO : RHI_BF_SRC1_COLOR;
                                b.colorOp = config == kBlendMixSub ? RHI_BO_REVERSE_SUBTRACT : RHI_BO_ADD;
                                b.srcAlpha = ab ? RHI_BF_SRC1_ALPHA : RHI_BF_ONE;
                                b.dstAlpha = ab ? RHI_BF_ONE_MINUS_SRC1_ALPHA : RHI_BF_ZERO;
                                b.alphaOp = RHI_BO_ADD;
                                b.writeMask = kMaskValues[m];
                                d.depthStencil.depthTest = true;
                                d.depthStencil.depthWrite = zw != 0u;
                                d.depthStencil.depthCompare = compareOf(cmp);
                                d.colorFormats[0] = RHI_FMT_RGBA8_UNORM;
                                d.colorCount = 1u;
                                d.depthFormat = RHI_FMT_D32F;
                                d.debugName = "gs2d";
                                descs.push_back(d);
                                keys.push_back(pipelineKey(config, ab != 0u, points != 0u, cmp, zw != 0u, kMaskValues[m]));
                            }
        // The exact blend's READ configuration (18.2): {triangles, points} x depth compare x depth write x colour mask,
        // blend disabled, one colour output; the destination is read by the shader (dstRead stays NONE: the
        // rasterization-order form is not used, the barriers order the reads).
        const size_t ffCount = descs.size();
        if (m_haveRead)
        {
            m_readPipelines.assign(2u * 4u * 2u * kMasks, RhiPipeline{});
            for (uint32_t points = 0; points < 2u; ++points)
                for (uint32_t cmp = 0; cmp < 4u; ++cmp)
                    for (uint32_t zw = 0; zw < 2u; ++zw)
                        for (uint32_t m = 0; m < kMasks; ++m)
                        {
                            RhiPipelineDesc d{};
                            d.vertex = points ? m_vertPoints : m_vert;
                            d.fragment = m_fragRead;
                            d.layouts = m_readLayouts;
                            d.layoutCount = 3u;
                            d.topology = points ? RHI_TOPO_POINT_LIST : RHI_TOPO_TRIANGLE_LIST;
                            d.cullNone = true;
                            d.blend[0].enable = false;
                            d.blend[0].writeMask = kMaskValues[m];
                            d.depthStencil.depthTest = true;
                            d.depthStencil.depthWrite = zw != 0u;
                            d.depthStencil.depthCompare = compareOf(cmp);
                            d.colorFormats[0] = RHI_FMT_RGBA8_UNORM;
                            d.colorCount = 1u;
                            d.depthFormat = RHI_FMT_D32F;
                            d.debugName = "gs2d read";
                            descs.push_back(d);
                            keys.push_back(readKey(points != 0u, cmp, zw != 0u, kMaskValues[m]));
                        }
        }
        std::vector<RhiPipeline> made(descs.size());
        rhi_create_pipelines(descs.data(), static_cast<uint32_t>(descs.size()), made.data(), nullptr);
        for (size_t i = 0; i < made.size(); ++i)
        {
            if (!made[i].id)
                return fail("a pipeline could not be made");
            if (i < ffCount)
                m_pipelines[keys[i]] = made[i];
            else
                m_readPipelines[keys[i]] = made[i];
        }

        RhiPipelineDesc seed{};
        seed.vertex = m_seedVert;
        seed.fragment = m_seedFrag;
        seed.layouts = m_layouts;
        seed.layoutCount = 3u;
        seed.topology = RHI_TOPO_TRIANGLE_LIST;
        seed.cullNone = true;
        seed.blend[0].writeMask = kMaskRgba;
        seed.depthStencil.depthTest = true;
        seed.depthStencil.depthWrite = true;
        seed.depthStencil.depthCompare = RHI_CMP_ALWAYS;
        seed.colorFormats[0] = RHI_FMT_RGBA8_UNORM;
        seed.colorCount = 1u;
        seed.depthFormat = RHI_FMT_D32F;
        seed.debugName = "gs2d seed";
        m_seedPipeline = rhi_create_pipeline(&seed);
        RhiPipelineDesc probe{};
        probe.vertex = m_vert;
        probe.fragment = m_fragProbe;
        probe.layouts = m_layouts;
        probe.layoutCount = 3u;
        probe.topology = RHI_TOPO_TRIANGLE_LIST;
        probe.cullNone = true;
        probe.blend[0].writeMask = kMaskRgba;
        probe.colorFormats[0] = RHI_FMT_RGBA32_UINT;
        probe.colorCount = 1u;
        probe.depthFormat = RHI_FMT_UNKNOWN;
        probe.debugName = "gs2d probe";
        m_probePipeline = rhi_create_pipeline(&probe);
        if (!m_seedPipeline.id || !m_probePipeline.id || !m_sampler.id)
            return fail("seed or probe pipeline");

        RhiTextureDesc td{};
        td.width = kWidth;
        td.height = kHeight;
        td.mipLevels = 1u;
        td.format = RHI_FMT_RGBA8_UNORM;
        td.usage = RHI_TEX_RENDER_TARGET | RHI_TEX_COPY_SRC | RHI_TEX_COPY_DST | RHI_TEX_SAMPLED | (m_haveRead ? RHI_TEX_INPUT_ATTACHMENT : 0u);
        td.debugName = "gs2d target FBP 0x0";
        m_targets[0] = rhi_create_texture(&td);
        td.debugName = "gs2d target FBP 0x50";
        m_targets[1] = rhi_create_texture(&td);
        td.format = RHI_FMT_D32F;
        td.usage = RHI_TEX_DEPTH_STENCIL | RHI_TEX_COPY_SRC;
        td.debugName = "gs2d depth";
        m_depth = rhi_create_texture(&td);
        td.format = RHI_FMT_RGBA32_UINT;
        td.usage = RHI_TEX_RENDER_TARGET | RHI_TEX_COPY_SRC;
        td.debugName = "gs2d probe";
        m_probeTarget = rhi_create_texture(&td);
        td.width = td.height = 1u;
        td.format = RHI_FMT_RGBA8_UNORM;
        td.usage = RHI_TEX_SAMPLED | RHI_TEX_COPY_DST;
        td.debugName = "gs2d no texture";
        m_dummy = rhi_create_texture(&td);
        if (!m_targets[0].id || !m_targets[1].id || !m_depth.id || !m_probeTarget.id || !m_dummy.id)
            return fail("targets");
        m_targetState[0] = m_targetState[1] = m_depthState = m_probeState = RHI_STATE_UNDEFINED;

        // The dummy texture's one texel, so its contents are defined.
        rhi_wait_frame();
        const RhiBufferDesc bd{4u, RHI_BUF_COPY_SRC, RHI_MEM_UPLOAD, "gs2d dummy upload"};
        RhiBuffer ub = rhi_create_buffer(&bd);
        if (void *p = rhi_map_buffer(ub))
        {
            std::memset(p, 0, 4);
            rhi_unmap_buffer(ub);
        }
        RhiCommandList cl = rhi_begin_commands();
        const RhiTextureBarrier toCopy{m_dummy, RHI_STATE_UNDEFINED, RHI_STATE_COPY_DST};
        rhi_cmd_barrier(cl, &toCopy, 1u);
        rhi_cmd_copy_buffer_to_texture(cl, ub, 0u, 4u, m_dummy, 0u, RhiRect{0, 0, 1u, 1u});
        const RhiTextureBarrier toRead{m_dummy, RHI_STATE_COPY_DST, RHI_STATE_SHADER_READ};
        rhi_cmd_barrier(cl, &toRead, 1u);
        rhi_submit(cl);
        rhi_wait_idle();
        rhi_destroy_buffer(ub);
        m_created = true;
        return true;
    }

    void Gs2dVk::destroy()
    {
        if (!m_created && !m_layouts[0].id && m_pipelines.empty())
            return;
        rhi_wait_idle();
        for (RhiPipeline &p : m_pipelines)
            if (p.id)
                rhi_destroy_pipeline(p);
        m_pipelines.clear();
        for (RhiPipeline &p : m_readPipelines)
            if (p.id)
                rhi_destroy_pipeline(p);
        m_readPipelines.clear();
        for (RhiPipeline *p : {&m_seedPipeline, &m_probePipeline})
            if (p->id)
            {
                rhi_destroy_pipeline(*p);
                *p = RhiPipeline{};
            }
        for (RhiShader *s : {&m_vert, &m_vertPoints, &m_frag, &m_fragProbe, &m_fragRead, &m_seedVert, &m_seedFrag})
            if (s->id)
            {
                rhi_destroy_shader(*s);
                *s = RhiShader{};
            }
        for (auto &t : m_textures)
            rhi_destroy_texture(t.second);
        m_textures.clear();
        for (RhiTexture t : m_retired)
            rhi_destroy_texture(t);
        m_retired.clear();
        for (RhiTexture *t : {&m_targets[0], &m_targets[1], &m_depth, &m_probeTarget, &m_dummy})
            if (t->id)
            {
                rhi_destroy_texture(*t);
                *t = RhiTexture{};
            }
        for (uint32_t i = 0; i < RHI_FRAMES_IN_FLIGHT; ++i)
        {
            if (m_records[i].id)
                rhi_destroy_buffer(m_records[i]);
            m_records[i] = RhiBuffer{};
            m_recordBytes[i] = 0;
        }
        if (m_sampler.id)
            rhi_destroy_sampler(m_sampler);
        m_sampler = RhiSampler{};
        for (RhiBindGroupLayout &l : m_layouts)
        {
            if (l.id)
                rhi_destroy_bind_group_layout(l);
            l = RhiBindGroupLayout{};
        }
        if (m_readLayouts[0].id)
            rhi_destroy_bind_group_layout(m_readLayouts[0]);
        for (RhiBindGroupLayout &l : m_readLayouts)
            l = RhiBindGroupLayout{};
        m_created = m_haveRead = false;
    }

    void Gs2dVk::transition(RhiCommandList cl, RhiTexture t, RhiState &current, RhiState to)
    {
        if (current == to)
            return;
        const RhiTextureBarrier b{t, current, to};
        rhi_cmd_barrier(cl, &b, 1u);
        current = to;
    }

    bool Gs2dVk::upload(const std::vector<Gs2dPrim> &records)
    {
        // (batch 5) One buffer a frame slot: live, the list of the next frame is written while this one's is in flight.
        const uint32_t slot = rhi_frame_slot();
        RhiBuffer &buffer = m_records[slot];
        uint64_t &capacity = m_recordBytes[slot];
        const uint64_t bytes = std::max<uint64_t>(sizeof(Gs2dPrim), records.size() * sizeof(Gs2dPrim));
        if (bytes > capacity)
        {
            if (buffer.id)
                rhi_destroy_buffer(buffer);
            uint64_t size = 1u << 16;
            while (size < bytes)
                size <<= 1;
            const RhiBufferDesc bd{size, RHI_BUF_STORAGE_READ, RHI_MEM_UPLOAD, "gs2d records"};
            buffer = rhi_create_buffer(&bd);
            capacity = buffer.id ? size : 0u;
            if (!buffer.id)
                return false;
        }
        void *p = rhi_map_buffer(buffer);
        if (!p)
            return false;
        if (!records.empty())
            std::memcpy(p, records.data(), records.size() * sizeof(Gs2dPrim));
        rhi_unmap_buffer(buffer);
        return true;
    }

    bool Gs2dVk::seed(uint32_t target, const uint32_t *rgba, const uint32_t *z)
    {
        if (!m_created || target > 1u || !rgba || !z)
            return false;
        rhi_wait_frame();
        const uint64_t plane = static_cast<uint64_t>(kWidth) * kHeight * 4u;
        const RhiBufferDesc bd{plane * 2u, RHI_BUF_COPY_SRC, RHI_MEM_UPLOAD, "gs2d seed upload"};
        RhiBuffer ub = rhi_create_buffer(&bd);
        uint8_t *p = static_cast<uint8_t *>(rhi_map_buffer(ub));
        if (!p)
            return false;
        std::memcpy(p, rgba, plane);
        std::memcpy(p + plane, z, plane);
        rhi_unmap_buffer(ub);
        RhiTextureDesc td{};
        td.width = kWidth;
        td.height = kHeight;
        td.mipLevels = 1u;
        td.format = RHI_FMT_RGBA8_UNORM;
        td.usage = RHI_TEX_SAMPLED | RHI_TEX_COPY_DST;
        td.debugName = "gs2d seed colour";
        RhiTexture colour = rhi_create_texture(&td);
        td.format = RHI_FMT_R32_UINT;
        td.debugName = "gs2d seed depth";
        RhiTexture depth = rhi_create_texture(&td);

        RhiCommandList cl = rhi_begin_commands();
        const RhiTextureBarrier toCopy[2] = {{colour, RHI_STATE_UNDEFINED, RHI_STATE_COPY_DST}, {depth, RHI_STATE_UNDEFINED, RHI_STATE_COPY_DST}};
        rhi_cmd_barrier(cl, toCopy, 2u);
        rhi_cmd_copy_buffer_to_texture(cl, ub, 0u, kWidth * 4u, colour, 0u, RhiRect{0, 0, kWidth, kHeight});
        rhi_cmd_copy_buffer_to_texture(cl, ub, plane, kWidth * 4u, depth, 0u, RhiRect{0, 0, kWidth, kHeight});
        const RhiTextureBarrier toRead[2] = {{colour, RHI_STATE_COPY_DST, RHI_STATE_SHADER_READ}, {depth, RHI_STATE_COPY_DST, RHI_STATE_SHADER_READ}};
        rhi_cmd_barrier(cl, toRead, 2u);
        transition(cl, m_targets[target], m_targetState[target], RHI_STATE_RENDER_TARGET);
        transition(cl, m_depth, m_depthState, RHI_STATE_DEPTH_WRITE);
        RhiRenderPassDesc pass{};
        pass.color[0].texture = m_targets[target];
        pass.color[0].load = RHI_LOAD_DONT_CARE;
        pass.color[0].store = RHI_STORE_STORE;
        pass.colorCount = 1u;
        pass.depth.texture = m_depth;
        pass.depth.depthLoad = RHI_LOAD_DONT_CARE;
        pass.depth.stencilLoad = RHI_LOAD_DONT_CARE;
        pass.depth.store = RHI_STORE_STORE;
        pass.depth.stencilStore = RHI_STORE_DONT_CARE;
        pass.width = kWidth;
        pass.height = kHeight;
        rhi_cmd_begin_render_pass(cl, &pass);
        const RhiViewport vp{0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 1.0f};
        rhi_cmd_set_viewport(cl, &vp);
        const RhiRect sc{0, 0, kWidth, kHeight};
        rhi_cmd_set_scissor(cl, &sc);
        RhiBinding b[3]{};
        b[0].slot = 1u;
        b[0].type = RHI_BIND_SAMPLED_TEXTURE;
        b[0].texture = colour;
        b[1].slot = 2u;
        b[1].type = RHI_BIND_SAMPLED_TEXTURE;
        b[1].texture = depth;
        b[2].slot = 1u;
        b[2].type = RHI_BIND_SAMPLER;
        b[2].sampler = m_sampler;
        RhiBinding rb{};
        rb.slot = 0u;
        rb.type = RHI_BIND_STORAGE_BUFFER;
        if (!upload({}))
            return false;
        rb.buffer = m_records[rhi_frame_slot()];
        const RhiBindGroupDesc g0{m_layouts[0], nullptr, 0u}, g1{m_layouts[1], &rb, 1u}, g2{m_layouts[2], b, 3u};
        rhi_cmd_set_pipeline(cl, m_seedPipeline);
        rhi_cmd_set_bind_group(cl, 0u, rhi_create_bind_group(&g0));
        rhi_cmd_set_bind_group(cl, 1u, rhi_create_bind_group(&g1));
        rhi_cmd_set_bind_group(cl, 2u, rhi_create_bind_group(&g2));
        rhi_cmd_draw(cl, 3u, 0u, 1u);
        rhi_cmd_end_render_pass(cl);
        rhi_submit(cl);
        rhi_wait_idle();
        rhi_destroy_texture(colour);
        rhi_destroy_texture(depth);
        rhi_destroy_buffer(ub);
        return true;
    }

    bool Gs2dVk::build(const DrawList &list, std::vector<Draw> &draws, std::vector<RhiTexture> &fresh, RhiCommandList cl)
    {
        std::vector<Gs2dPrim> &rec = m_scratch;
        rec.clear();
        GsState state{};
        GsPlan plan{};
        bool haveState = false;
        RhiTexture tex = m_dummy;
        RhiRect scissor{0, 0, 0u, 0u};
        Box sbox{0, 0, -1, -1};
        const uint32_t f = m_opt.faults;
        // The exact blend (18.2): the plans with a fractional factor are drawn by the READ pipelines.
        const bool readNow = readMode();
        auto planReads = [&]() { return readNow && plan.blend != kBlendOff && !plan.exactBlend; };
        bool reads = false, pendingBarrier = false;
        // The pixel rule (RENDERER_3D_DESIGN.md 10.2): the target of the pass the draws go into (record() opens a pass at
        // a change of target and after an IMAGE_BLIT), whether anything was drawn in it, whether a reading primitive was
        // seen in it.
        const bool pixelRule = m_opt.grouping == kGroupPixels;
        int passTarget = -1;
        bool passDrawn = false, passReadSeen = false;
        uint64_t culledSeen = 0;
        // 10.2's lever: no primitive after the list's last reading primitive needs stamping (nothing reads later).
        size_t cmdIndex = 0, lastReadCmd = 0;
        bool anyRead = false;
        if (pixelRule && readNow)
        {
            bool drawable = false, reading = false;
            for (size_t i = 0; i < list.cmds.size(); ++i)
            {
                const DrawCmd &c = list.cmds[i];
                if (c.op == kOpState)
                {
                    const GsPlan pl = planState(list.state(c));
                    drawable = !(pl.unsupported || (c.a & 1u));
                    reading = pl.blend != kBlendOff && !pl.exactBlend;
                }
                else if ((c.op == kOpScreenPrims || c.op == kOpClear) && drawable && reading)
                {
                    lastReadCmd = i;
                    anyRead = true;
                }
            }
        }
        const auto buildStart = std::chrono::steady_clock::now();
        std::chrono::steady_clock::duration coverTime{};
        const uint64_t coverBefore = m_cover.pixelsVisited();

        // Texture uploads: each TEXTURE command a new texture object (an id can change generation within a list).
        struct Up
        {
            RhiTexture t;
            const uint8_t *data;
            uint32_t w, h;
        };
        std::vector<Up> ups;
        std::unordered_map<uint32_t, RhiTexture> current = m_textures;

        auto baseRecord = [&](Gs2dPrim &r)
        {
            std::memset(&r, 0, sizeof(r));
            uint32_t fix = plan.fix;
            if ((f & kFaultFixOffByOne) && plan.factor == kFactorFix)
                fix = (fix + 1u) & 0xFFu;
            r.blend = plan.term | static_cast<uint32_t>(plan.factor) << 2 | static_cast<uint32_t>(plan.ks) << 4 | static_cast<uint32_t>(plan.kd) << 8 |
                      static_cast<uint32_t>(plan.blend) << 12 | fix << 16 | static_cast<uint32_t>(plan.abcd) << 24;
            uint32_t atst = plan.atst;
            if ((f & kFaultAtstLess) && atst == kAtstLequal)
                atst = kAtstLess;
            const Tex0 t0 = Tex0::unpack(state.tex0);
            r.test = atst | static_cast<uint32_t>(plan.aref) << 4 | static_cast<uint32_t>(plan.tfx) << 12 | static_cast<uint32_t>(plan.tcc) << 14 |
                     static_cast<uint32_t>(plan.linear) << 15 | static_cast<uint32_t>(plan.wrapU) << 16 | static_cast<uint32_t>(plan.wrapV) << 18 |
                     static_cast<uint32_t>(std::min<uint32_t>(t0.tw, 10u)) << 20 | static_cast<uint32_t>(std::min<uint32_t>(t0.th, 10u)) << 24;
            if (f & kFaultRepeatAsClamp)
            {
                uint32_t wu = (r.test >> 16) & 3u, wv = (r.test >> 18) & 3u;
                wu = wu == 0u ? 1u : wu;
                wv = wv == 0u ? 1u : wv;
                r.test = (r.test & ~(0xFu << 16)) | wu << 16 | wv << 18;
            }
            r.clampU = static_cast<uint32_t>((state.clamp >> 4) & 0x3FFu) | static_cast<uint32_t>((state.clamp >> 14) & 0x3FFu) << 16;
            r.clampV = static_cast<uint32_t>((state.clamp >> 24) & 0x3FFu) | static_cast<uint32_t>((state.clamp >> 34) & 0x3FFu) << 16;
            r.flags = (state.tme() ? kPrimTme : 0u) | (state.iip() ? kPrimIip : 0u) | (state.fst() ? kPrimFst : 0u) |
                      (m_opt.sourceBias && !planReads() ? kPrimBias : 0u);
            r.fault = (f >> 16) & 7u;
        };
        auto colourOf = [&](const GsVertex &v)
        {
            return (f & kFaultColourSwap) ? packRgba(v.g, v.r, v.b, v.a) : packRgba(v.r, v.g, v.b, v.a);
        };
        // A draw of records [first, end) with the pipeline for the part.
        auto emit = [&](uint32_t first, uint32_t end, bool points, uint32_t part)
        {
            if (end <= first || scissor.w == 0u || scissor.h == 0u)
                return;
            uint32_t ztst = plan.depthCompare;
            if ((f & kFaultZGequal) && ztst == kDepthGreater)
                ztst = kDepthGequal;
            else if ((f & kFaultGequalAsGreater) && ztst == kDepthGequal)
                ztst = kDepthGreater;
            const bool fail = part == kPartFail;
            const bool zw = fail ? plan.failDepthWrite : plan.passDepthWrite;
            const uint32_t mask = fail ? plan.failColorMask : plan.passColorMask;
            const uint32_t key = reads ? readKey(points, ztst, zw, mask) : pipelineKey(plan.blend, part == kPartAlphaBlend, points, ztst, zw, mask);
            for (uint32_t i = first; i < end; ++i)
                rec[i].flags |= part << kPrimPartShift;
            draws.push_back({plan.target, key, first, end - first, tex, scissor, points, reads, reads && pendingBarrier});
            pendingBarrier = false;
            passDrawn = true;
            ++m_stats.draws;
        };
        // The As of a primitive when it is one value for all its pixels (5.2); -1 otherwise.
        auto knownAlpha = [&](const GsVertex *v, uint32_t n) -> int
        {
            const bool flatA = !state.iip() || n == 1u || (v[0].a == v[n - 1].a && (n < 3u || v[1].a == v[0].a));
            if (!flatA)
                return -1;
            const uint32_t va = v[n - 1].a;
            if (!state.tme())
                return static_cast<int>(va);
            if (!plan.tcc)
                return static_cast<int>(va);
            if (state.texAlphaMin != state.texAlphaMax)
                return -1;
            const Rgba t{0, 0, 0, state.texAlphaMin};
            return gsTextureFunction(plan.tfx, plan.tcc, t, {0, 0, 0, static_cast<uint8_t>(va)}).a;
        };
        // Records of one SCREEN_PRIMS batch (or the clear), drawn by the plan: one draw, or pass and fail parts per
        // group of primitives that cannot overlap.
        struct Item
        {
            Gs2dPrim r;
            Box box;
            int alpha;   // known As or -1
            bool pairWithPrevious;
            uint32_t pixel; // points: y * 4096 + x, for the overlap test
        };
        std::vector<Item> items;
        // The pixels an item covers (10.2), from its record: a triangle's edge functions, a flat record's box.
        auto spansOf = [&](const Item &it, std::vector<GsSpan> &out)
        {
            if (it.r.flags & kPrimFlat)
                return GsCoverMap::rect(it.box.x0, it.box.y0, it.box.x1, it.box.y1, out);
            GsTri t;
            for (int k = 0; k < 3; ++k)
            {
                t.a[k] = it.r.ea[k];
                t.b[k] = it.r.eb[k];
                uint64_t bits = static_cast<uint64_t>(it.r.ec[2 * k]) | static_cast<uint64_t>(it.r.ec[2 * k + 1]) << 32;
                double c;
                std::memcpy(&c, &bits, 8);
                t.c[k] = static_cast<int64_t>(c);
                t.bias[k] = (it.r.flags >> (kPrimEdgeBiasShift + k)) & 1u;
            }
            return GsCoverMap::triangle(t, it.box.x0, it.box.y0, it.box.x1, it.box.y1, out);
        };
        // A group [a, b) of a split or reading plan: one draw, or the pass and fail parts (5.2), the barrier flag on the
        // group's first draw.
        auto emitGroup = [&](size_t a, size_t b, bool points, bool barrier)
        {
            if (plan.split)
            {
                ++m_stats.splitGroups;
                ++m_last.splitGroups;
            }
            if (reads)
            {
                ++m_stats.readGroups;
                ++m_last.readGroups;
            }
            pendingBarrier = barrier;
            if (!plan.split)
            {
                const uint32_t first = static_cast<uint32_t>(rec.size());
                for (size_t k = a; k < b; ++k)
                    rec.push_back(items[k].r);
                emit(first, static_cast<uint32_t>(rec.size()), points, plan.alphaByBlend ? kPartAlphaBlend : kPartPass);
                pendingBarrier = false;
                return;
            }
            for (int part = 0; part < 2; ++part)
            {
                const uint32_t first = static_cast<uint32_t>(rec.size());
                for (size_t k = a; k < b; ++k)
                {
                    const Item &it = items[k];
                    if (it.alpha >= 0)
                    {
                        ++m_stats.decidedOnCpu;
                        const bool pass = gsAlphaPass(it.r.test & 7u, plan.aref, static_cast<uint32_t>(it.alpha));
                        if (pass != (part == 0))
                            continue;
                    }
                    rec.push_back(it.r);
                }
                if (part == 1 && (f & kFaultDropFail))
                {
                    rec.resize(first);
                    continue;
                }
                emit(first, static_cast<uint32_t>(rec.size()), points, part == 0 ? kPartPass : kPartFail);
            }
            pendingBarrier = false;
        };
        // The pixel rule (10.2) over one batch.
        auto flushPixels = [&](bool points)
        {
            const auto t0 = std::chrono::steady_clock::now();
            if (static_cast<int>(plan.target) != passTarget)
            {
                m_cover.beginPass();
                passTarget = static_cast<int>(plan.target);
                passDrawn = passReadSeen = false;
            }
            if (plan.split)
                m_stats.splitPrims += items.size();
            if (reads)
            {
                m_stats.readPrims += items.size();
                m_last.readPrims += items.size();
            }
            const bool needSpans = reads || plan.split || passReadSeen;
            if (!plan.split && !reads)
            {
                // One draw; its pixels stamped once a reading primitive has been seen in the pass.
                if (passReadSeen && !(f & kFaultNoStampOther) && anyRead && cmdIndex < lastReadCmd)
                    for (const Item &it : items)
                    {
                        m_spans.clear();
                        spansOf(it, m_spans);
                        m_cover.stamp(m_spans.data(), m_spans.size(), GsCoverMap::kRead);
                    }
                coverTime += std::chrono::steady_clock::now() - t0;
                const uint32_t first = static_cast<uint32_t>(rec.size());
                for (const Item &it : items)
                    rec.push_back(it.r);
                emit(first, static_cast<uint32_t>(rec.size()), points, plan.alphaByBlend ? kPartAlphaBlend : kPartPass);
                items.clear();
                return;
            }
            (void)needSpans;
            if (reads && (f & kFaultCoverOwnBatch) && passReadSeen)
                m_cover.advance(GsCoverMap::kRead);
            size_t groupStart = 0;
            bool groupBarrier = false;
            std::chrono::steady_clock::duration emitTime{};
            for (size_t k = 0; k < items.size(); ++k)
            {
                m_spans.clear();
                spansOf(items[k], m_spans);
                const GsSpan *sp = m_spans.data();
                const size_t n = m_spans.size();
                bool barrier = false;
                if (reads)
                {
                    if (!passReadSeen)
                    {
                        passReadSeen = true;
                        barrier = passDrawn && !(f & kFaultNoFirstBarrier);
                    }
                    else if (!((f & kFaultOneGroup) && k > 0) && m_cover.any(sp, n, GsCoverMap::kRead))
                    {
                        if (f & kFaultEpochNoBarrier)
                            m_cover.advance(GsCoverMap::kRead);
                        else
                            barrier = true;
                    }
                }
                const bool splitHit = plan.split && k > groupStart && !(f & kFaultSplitPerBatch) && m_cover.any(sp, n, GsCoverMap::kSplit);
                if (k > groupStart && (barrier || splitHit))
                {
                    const auto e0 = std::chrono::steady_clock::now();
                    emitGroup(groupStart, k, points, groupBarrier);
                    emitTime += std::chrono::steady_clock::now() - e0;
                    m_cover.advance(GsCoverMap::kSplit);
                    groupStart = k;
                    groupBarrier = false;
                }
                if (barrier)
                {
                    groupBarrier = true;
                    m_cover.advance(GsCoverMap::kRead);
                }
                if (passReadSeen && (reads || !(f & kFaultNoStampOther)))
                    m_cover.stamp(sp, n, GsCoverMap::kRead);
                if (plan.split)
                    m_cover.stamp(sp, n, GsCoverMap::kSplit);
            }
            const auto e0 = std::chrono::steady_clock::now();
            emitGroup(groupStart, items.size(), points, groupBarrier);
            emitTime += std::chrono::steady_clock::now() - e0;
            m_cover.advance(GsCoverMap::kSplit); // a change of state ends a group
            coverTime += std::chrono::steady_clock::now() - t0 - emitTime;
            items.clear();
        };
        auto flush = [&](bool points)
        {
            if (items.empty())
                return;
            reads = planReads();
            if (pixelRule)
            {
                flushPixels(points);
                return;
            }
            if (!plan.split && !reads)
            {
                const uint32_t first = static_cast<uint32_t>(rec.size());
                for (const Item &it : items)
                    rec.push_back(it.r);
                emit(first, static_cast<uint32_t>(rec.size()), points, plan.alphaByBlend ? kPartAlphaBlend : kPartPass);
                items.clear();
                return;
            }
            if (plan.split)
                m_stats.splitPrims += items.size();
            if (reads)
            {
                m_stats.readPrims += items.size();
                m_last.readPrims += items.size();
            }
            size_t i = 0;
            while (i < items.size())
            {
                // The group: a run of items whose pixel sets are pairwise disjoint (5.2; for the exact blend, 18.2).
                size_t end = i + 1;
                if (((f & kFaultSplitPerBatch) && plan.split) || ((f & kFaultOneGroup) && reads))
                    end = items.size();
                else if (points)
                {
                    std::unordered_set<uint32_t> seen{items[i].pixel};
                    while (end < items.size() && seen.insert(items[end].pixel).second)
                        ++end;
                }
                else
                    while (end < items.size())
                    {
                        bool ok = true;
                        for (size_t k = i; k < end && ok; ++k)
                            if (!items[end].box.disjoint(items[k].box) && !(k == end - 1 && items[end].pairWithPrevious))
                                ok = false;
                        if (!ok)
                            break;
                        ++end;
                    }
                if (plan.split)
                    ++m_stats.splitGroups;
                if (reads)
                {
                    ++m_stats.readGroups;
                    ++m_last.readGroups;
                    pendingBarrier = true; // on the group's first draw
                }
                if (!plan.split)
                {
                    // A reading plan without the split: the group in one draw.
                    const uint32_t first = static_cast<uint32_t>(rec.size());
                    for (size_t k = i; k < end; ++k)
                        rec.push_back(items[k].r);
                    emit(first, static_cast<uint32_t>(rec.size()), points, plan.alphaByBlend ? kPartAlphaBlend : kPartPass);
                    pendingBarrier = false;
                    i = end;
                    continue;
                }
                const uint32_t aref = plan.aref;
                for (int part = 0; part < 2; ++part)
                {
                    const uint32_t first = static_cast<uint32_t>(rec.size());
                    for (size_t k = i; k < end; ++k)
                    {
                        const Item &it = items[k];
                        if (it.alpha >= 0)
                        {
                            ++m_stats.decidedOnCpu;
                            const bool pass = gsAlphaPass(it.r.test & 7u, aref, static_cast<uint32_t>(it.alpha));
                            if (pass != (part == 0))
                                continue;
                        }
                        rec.push_back(it.r);
                    }
                    if (part == 1 && (f & kFaultDropFail))
                    {
                        rec.resize(first);
                        continue;
                    }
                    emit(first, static_cast<uint32_t>(rec.size()), points, part == 0 ? kPartPass : kPartFail);
                }
                pendingBarrier = false;
                i = end;
            }
            items.clear();
        };

        m_blits.clear();
        std::vector<const uint8_t *> blitData;
        uint64_t blitBytes = 0;
        for (cmdIndex = 0; cmdIndex < list.cmds.size(); ++cmdIndex)
        {
            const DrawCmd &cmd = list.cmds[cmdIndex];
            switch (cmd.op)
            {
            case kOpImageBlit:
            {
                // (batch 5) CT32 words into a frame buffer, between the draws before and after it.
                const uint32_t x = cmd.b & 0xFFFFu, y = cmd.b >> 16, w = cmd.c & 0xFFFFu, h = cmd.c >> 16;
                if (!w || !h || cmd.payloadBytes < w * h * 4u || x + w > kWidth || y + h > kHeight || (cmd.a != 0u && cmd.a != 0x50u))
                    break;
                Draw d{};
                d.target = cmd.a == 0x50u ? 1u : 0u;
                d.blit = static_cast<int32_t>(m_blits.size());
                passTarget = -1; // record() ends the pass for the copy: the next draw begins another
                m_blits.push_back({d.target, RhiRect{static_cast<int32_t>(x), static_cast<int32_t>(y), w, h}, blitBytes});
                blitData.push_back(list.data(cmd));
                blitBytes += static_cast<uint64_t>(w) * h * 4u;
                draws.push_back(d);
                break;
            }
            case kOpTexture:
            {
                RhiTextureDesc td{};
                td.width = cmd.c & 0xFFFFu;
                td.height = cmd.c >> 16;
                td.mipLevels = 1u;
                td.format = RHI_FMT_RGBA8_UNORM;
                td.usage = RHI_TEX_SAMPLED | RHI_TEX_COPY_DST;
                td.debugName = "gs2d texture";
                if (!td.width || !td.height || cmd.payloadBytes < td.width * td.height * 4u)
                    break;
                RhiTexture t = rhi_create_texture(&td);
                if (!t.id)
                    return false;
                fresh.push_back(t);
                ups.push_back({t, list.data(cmd), td.width, td.height});
                current[cmd.a] = t;
                ++m_stats.textures;
                break;
            }
            case kOpState:
            {
                state = list.state(cmd);
                plan = planState(state);
                haveState = !(plan.unsupported || (cmd.a & 1u));
                if (!haveState)
                    ++m_stats.unsupported;
                tex = m_dummy;
                if (state.tme())
                {
                    auto it = current.find(state.texture);
                    if (it != current.end())
                        tex = it->second;
                }
                const int sx0 = static_cast<int>(state.scissor & 0x7FFu), sx1 = static_cast<int>((state.scissor >> 16) & 0x7FFu);
                const int sy0 = static_cast<int>((state.scissor >> 32) & 0x7FFu), sy1 = static_cast<int>((state.scissor >> 48) & 0x7FFu);
                sbox = {std::max(sx0, 0), std::max(sy0, 0), std::min(sx1, static_cast<int>(kWidth) - 1), std::min(sy1, static_cast<int>(kHeight) - 1)};
                if (sbox.x1 < sbox.x0 || sbox.y1 < sbox.y0)
                    scissor = {0, 0, 0u, 0u};
                else
                    scissor = {sbox.x0, sbox.y0, static_cast<uint32_t>(sbox.x1 - sbox.x0 + 1), static_cast<uint32_t>(sbox.y1 - sbox.y0 + 1)};
                break;
            }
            case kOpClear:
            {
                if (!haveState)
                    break;
                ++m_stats.clears;
                const GsVertex *v = list.vertices(cmd);
                const uint32_t ofx = state.xyoffset & 0xFFFFu, ofy = (state.xyoffset >> 32) & 0xFFFFu;
                GsRect r = coverSprite(v, ofx, ofy);
                r.x0 = std::max(r.x0, sbox.x0);
                r.y0 = std::max(r.y0, sbox.y0);
                r.x1 = std::min(r.x1, sbox.x1);
                r.y1 = std::min(r.y1, sbox.y1);
                if (r.x1 < r.x0 || r.y1 < r.y0)
                    break;
                // Two flat triangles with corners on whole framebuffer coordinates (no pixel centre on an edge).
                const int cx[2] = {r.x0 * 16 - 8, (r.x1 + 1) * 16 - 8}, cy[2] = {r.y0 * 16 - 8, (r.y1 + 1) * 16 - 8};
                const int tri[2][3][2] = {{{0, 0}, {1, 0}, {0, 1}}, {{1, 0}, {1, 1}, {0, 1}}};
                for (const auto &t : tri)
                {
                    Item it{};
                    baseRecord(it.r);
                    for (int k = 0; k < 3; ++k)
                    {
                        it.r.px[k] = cx[t[k][0]];
                        it.r.py[k] = cy[t[k][1]];
                    }
                    it.r.flags = (it.r.flags & ~(kPrimTme | kPrimIip)) | kPrimFlat;
                    it.r.rgba[2] = colourOf(v[1]);
                    it.r.z[2] = static_cast<uint32_t>(gsVertexZ(v[1].z));
                    it.box = {r.x0, r.y0, r.x1, r.y1};
                    it.alpha = v[1].a;
                    it.pairWithPrevious = !items.empty();
                    items.push_back(it);
                }
                flush(false);
                break;
            }
            case kOpScreenPrims:
            {
                if (!haveState)
                    break;
                const GsVertex *v = list.vertices(cmd);
                const uint32_t ofx = state.xyoffset & 0xFFFFu, ofy = (state.xyoffset >> 32) & 0xFFFFu;
                if (cmd.a == kKindSprites)
                {
                    m_stats.spritesSkipped += cmd.b;
                    break;
                }
                if (cmd.a == kKindLines || cmd.a == kKindPoints)
                {
                    std::vector<GsLinePixel> px;
                    for (uint32_t p = 0; p < cmd.b; ++p)
                    {
                        ++m_stats.prims;
                        px.clear();
                        if (cmd.a == kKindLines)
                            expandLine(v + p * 2u, state.iip(), ofx, ofy, px);
                        else
                        {
                            const GsVertex &q = v[p];
                            GsLinePixel l;
                            l.x = static_cast<int>(static_cast<float>(q.x) / 16.0f) - static_cast<int>(ofx >> 4);
                            l.y = static_cast<int>(static_cast<float>(q.y) / 16.0f) - static_cast<int>(ofy >> 4);
                            l.z = static_cast<uint32_t>(gsVertexZ(q.z));
                            l.c = {q.r, q.g, q.b, q.a};
                            px.push_back(l);
                        }
                        for (const GsLinePixel &l : px)
                        {
                            if (l.x < sbox.x0 || l.x > sbox.x1 || l.y < sbox.y0 || l.y > sbox.y1)
                                continue;
                            Item it{};
                            baseRecord(it.r);
                            it.r.flags = (it.r.flags & ~(kPrimTme | kPrimIip)) | kPrimFlat;
                            it.r.px[0] = l.x * 16;
                            it.r.py[0] = l.y * 16;
                            const GsVertex cv{0, 0, 0, l.c.r, l.c.g, l.c.b, l.c.a};
                            it.r.rgba[2] = colourOf(cv);
                            it.r.z[2] = l.z;
                            it.box = {l.x, l.y, l.x, l.y};
                            it.alpha = l.c.a;
                            it.pixel = static_cast<uint32_t>(l.y) * 4096u + static_cast<uint32_t>(l.x);
                            items.push_back(it);
                            ++m_stats.points;
                        }
                        m_stats.linePixels += cmd.a == kKindLines ? px.size() : 0u;
                    }
                    flush(true);
                    break;
                }
                int64_t lastPushed = -1;
                for (uint32_t p = 0; p < cmd.b; ++p)
                {
                    ++m_stats.prims;
                    const GsVertex *tv = v + p * 3u;
                    GsTri t;
                    if (!gsTriSetup(tv, ofx, ofy, t))
                    {
                        ++m_stats.culled;
                        continue;
                    }
                    Box box{std::max(t.minX, sbox.x0), std::max(t.minY, sbox.y0), std::min(t.maxX, sbox.x1), std::min(t.maxY, sbox.y1)};
                    if (box.x1 < box.x0 || box.y1 < box.y0)
                    {
                        ++m_stats.culled;
                        if (!(f & kFaultDrawCulled))
                            continue;
                        // planted fault: drawn anyway (the scissor still applies; the box is the scissor's)
                        box = sbox;
                        ++culledSeen;
                    }
                    if ((f & kFaultCullDrawn) && (m_last.prims++ % 16u) == 15u)
                        continue;
                    for (int k = 0; k < 3; ++k)
                        if (t.x[k] < -4096 * 16 || t.x[k] > 4096 * 16 || t.y[k] < -4096 * 16 || t.y[k] > 4096 * 16)
                        {
                            // drawn anyway; the GPU clips. A 16-bit vertex minus a 16-bit offset cannot get here: check (d) fails on it
                            ++m_stats.beyondViewport;
                            ++m_last.beyondViewport;
                            break;
                        }
                    if (tv[0].z >= (1u << 24) || tv[1].z >= (1u << 24) || tv[2].z >= (1u << 24))
                    {
                        ++m_stats.zAbove24; // the shader clamps the pixel's Z to 2^24 - 1 (RENDERER_3D_DESIGN.md 9.1)
                        ++m_last.zAbove24;
                    }
                    Item it{};
                    baseRecord(it.r);
                    for (int k = 0; k < 3; ++k)
                    {
                        it.r.px[k] = static_cast<int32_t>(t.x[k]);
                        it.r.py[k] = static_cast<int32_t>(t.y[k]);
                        it.r.rgba[k] = colourOf(tv[k]);
                        if (state.fst())
                        {
                            uint32_t uv = static_cast<uint32_t>(tv[k].u) | static_cast<uint32_t>(tv[k].v) << 16;
                            std::memcpy(&it.r.s[k], &uv, 4);
                        }
                        else
                            it.r.s[k] = tv[k].s;
                        it.r.t[k] = tv[k].t;
                        it.r.q[k] = (f & kFaultNoQ) ? 1.0f : tv[k].q;
                        it.r.z[k] = tv[k].z + ((f & kFaultZOffByOne) && k == 0 ? 1u : 0u);
                        it.r.ea[k] = static_cast<int32_t>(t.a[k]);
                        it.r.eb[k] = static_cast<int32_t>(t.b[k]);
                        doubleBits(static_cast<double>(t.c[k]), it.r.ec[2 * k], it.r.ec[2 * k + 1]);
                        it.r.flags |= static_cast<uint32_t>(t.bias[k]) << (kPrimEdgeBiasShift + k);
                    }
                    doubleBits(t.invArea, it.r.invArea[0], it.r.invArea[1]);
                    it.box = box;
                    it.alpha = knownAlpha(tv, 3u);
                    it.pairWithPrevious = (p & 1u) && lastPushed == static_cast<int64_t>(p) - 1 && rectangleHalves(tv - 3, tv);
                    items.push_back(it);
                    lastPushed = p;
                    if (!(f & kFaultCullDrawn))
                        ++m_last.prims;
                }
                flush(false);
                break;
            }
            default:
                break;
            }
        }
        (void)culledSeen;
        m_last.coverPixels = m_cover.pixelsVisited() - coverBefore;
        m_stats.coverPixels += m_last.coverPixels;
        m_last.coverMs = std::chrono::duration<double, std::milli>(coverTime).count();
        m_last.buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - buildStart).count();
        if (!upload(rec))
            return false;
        m_stats.records += rec.size();
        m_blitStage = RhiBuffer{};
        if (blitBytes)
        {
            const RhiBufferDesc bd{blitBytes, RHI_BUF_COPY_SRC, RHI_MEM_UPLOAD, "gs2d image blits"};
            m_blitStage = rhi_create_buffer(&bd);
            uint8_t *p = m_blitStage.id ? static_cast<uint8_t *>(rhi_map_buffer(m_blitStage)) : nullptr;
            if (!p)
                return false;
            for (size_t i = 0; i < m_blits.size(); ++i)
                std::memcpy(p + m_blits[i].offset, blitData[i], static_cast<size_t>(m_blits[i].rect.w) * m_blits[i].rect.h * 4u);
            rhi_unmap_buffer(m_blitStage);
        }

        // The uploads, before any pass.
        if (!ups.empty())
        {
            uint64_t bytes = 0;
            for (const Up &u : ups)
                bytes += static_cast<uint64_t>(u.w) * u.h * 4u;
            const RhiBufferDesc bd{bytes, RHI_BUF_COPY_SRC, RHI_MEM_UPLOAD, "gs2d texture upload"};
            RhiBuffer ub = rhi_create_buffer(&bd);
            uint8_t *p = static_cast<uint8_t *>(rhi_map_buffer(ub));
            if (!p)
                return false;
            uint64_t at = 0;
            for (const Up &u : ups)
            {
                std::memcpy(p + at, u.data, static_cast<size_t>(u.w) * u.h * 4u);
                at += static_cast<uint64_t>(u.w) * u.h * 4u;
            }
            rhi_unmap_buffer(ub);
            at = 0;
            for (const Up &u : ups)
            {
                const RhiTextureBarrier toCopy{u.t, RHI_STATE_UNDEFINED, RHI_STATE_COPY_DST};
                rhi_cmd_barrier(cl, &toCopy, 1u);
                rhi_cmd_copy_buffer_to_texture(cl, ub, at, u.w * 4u, u.t, 0u, RhiRect{0, 0, u.w, u.h});
                const RhiTextureBarrier toRead{u.t, RHI_STATE_COPY_DST, RHI_STATE_SHADER_READ};
                rhi_cmd_barrier(cl, &toRead, 1u);
                at += static_cast<uint64_t>(u.w) * u.h * 4u;
            }
            rhi_destroy_buffer(ub); // deferred by the RHI until the frame's work is done
        }
        // The textures the list leaves current replace the kept ones.
        for (auto &kv : current)
        {
            auto it = m_textures.find(kv.first);
            if (it != m_textures.end() && it->second.id != kv.second.id)
                m_retired.push_back(it->second);
            m_textures[kv.first] = kv.second;
        }
        // Uploads replaced within the list itself.
        for (RhiTexture t : fresh)
        {
            bool kept = false;
            for (auto &kv : m_textures)
                kept |= kv.second.id == t.id;
            if (!kept)
                m_retired.push_back(t);
        }
        return true;
    }

    bool Gs2dVk::draw(const DrawList &list)
    {
        if (!m_created || (m_opt.blend == kBlendRead && !m_haveRead))
            return false;
        rhi_wait_frame();
        RhiCommandList cl = rhi_begin_commands();
        const bool stamps = rhi_timestamps_supported();
        const bool ok = record(list, cl, stamps);
        rhi_submit(cl);
        rhi_wait_idle();
        if (ok && stamps)
        {
            // The slot of this list is recycled (and its timestamps collected) two frames on; the GPU is idle.
            rhi_wait_frame();
            rhi_wait_frame();
            uint64_t ns[2] = {};
            if (rhi_read_timestamps(ns, 2u) >= 2u && ns[1] >= ns[0] && ns[0])
                m_last.gpuMs = static_cast<double>(ns[1] - ns[0]) / 1.0e6;
        }
        return ok;
    }

    bool Gs2dVk::record(const DrawList &list, RhiCommandList cl, bool stamps)
    {
        if (!m_created || (m_opt.blend == kBlendRead && !m_haveRead))
            return false;
        ++m_stats.lists;
        m_last = ListStats{};
        std::vector<Draw> draws;
        std::vector<RhiTexture> fresh;
        if (!build(list, draws, fresh, cl))
            return false;
        const bool readNow = readMode();
        stamps = stamps && rhi_timestamps_supported();
        if (stamps)
            rhi_cmd_write_timestamp(cl, 0u);
        RhiBinding rb{};
        rb.slot = 0u;
        rb.type = RHI_BIND_STORAGE_BUFFER;
        rb.buffer = m_records[rhi_frame_slot()];
        const RhiBindGroupDesc g0{m_layouts[0], nullptr, 0u}, g1{m_layouts[1], &rb, 1u};
        const RhiBindGroup group0 = rhi_create_bind_group(&g0), group1 = rhi_create_bind_group(&g1);
        // The exact blend: group 0 of the READ pipelines, the target as an input attachment.
        RhiBindGroup group0Read[2]{};
        if (readNow)
            for (uint32_t t = 0; t < 2u; ++t)
            {
                RhiBinding b{};
                b.slot = 0u;
                b.type = RHI_BIND_INPUT_ATTACHMENT;
                b.texture = m_targets[t];
                const RhiBindGroupDesc gr{m_readLayouts[0], &b, 1u};
                group0Read[t] = rhi_create_bind_group(&gr);
            }
        std::unordered_map<uint32_t, RhiBindGroup> textureGroups;
        int open = -1;
        bool drawnSinceBarrier = false; // in the open pass, since it began or since the last barrier
        // What is bound in the open pass (RENDERER_3D_DESIGN.md 9.3: only what changed between draws is recorded).
        uint64_t boundPipeline = 0, boundGroup0 = 0, boundGroup2 = 0;
        RhiRect boundScissor{0, 0, 0u, 0u};
        bool haveScissor = false;
        for (const Draw &d : draws)
        {
            if (d.blit >= 0)
            {
                // (batch 5) An upload into the frame buffer: outside a pass, in list order.
                if (open >= 0)
                    rhi_cmd_end_render_pass(cl);
                open = -1;
                const Blit &b = m_blits[static_cast<size_t>(d.blit)];
                transition(cl, m_targets[b.target], m_targetState[b.target], RHI_STATE_COPY_DST);
                rhi_cmd_copy_buffer_to_texture(cl, m_blitStage, b.offset, b.rect.w * 4u, m_targets[b.target], 0u, b.rect);
                continue;
            }
            if (open != static_cast<int>(d.target))
            {
                if (open >= 0)
                    rhi_cmd_end_render_pass(cl);
                // In read mode every pass is begun readable (the target in RENDERING_LOCAL_READ), so the READ draws
                // and the fixed-function ones share it.
                transition(cl, m_targets[d.target], m_targetState[d.target], readNow ? RHI_STATE_RENDER_TARGET_READ : RHI_STATE_RENDER_TARGET);
                transition(cl, m_depth, m_depthState, RHI_STATE_DEPTH_WRITE);
                RhiRenderPassDesc pass{};
                pass.color[0].texture = m_targets[d.target];
                pass.color[0].load = RHI_LOAD_LOAD;
                pass.color[0].store = RHI_STORE_STORE;
                pass.color[0].readable = readNow;
                pass.colorCount = 1u;
                pass.depth.texture = m_depth;
                pass.depth.depthLoad = RHI_LOAD_LOAD;
                pass.depth.stencilLoad = RHI_LOAD_DONT_CARE;
                pass.depth.store = RHI_STORE_STORE;
                pass.depth.stencilStore = RHI_STORE_DONT_CARE;
                pass.width = kWidth;
                pass.height = kHeight;
                rhi_cmd_begin_render_pass(cl, &pass);
                const RhiViewport vp{-4096.0f, -4096.0f, 8192.0f, 8192.0f, 0.0f, 1.0f};
                rhi_cmd_set_viewport(cl, &vp);
                rhi_cmd_set_bind_group(cl, 0u, group0);
                rhi_cmd_set_bind_group(cl, 1u, group1);
                open = static_cast<int>(d.target);
                drawnSinceBarrier = false;
                boundPipeline = 0;
                boundGroup0 = group0.id;
                boundGroup2 = 0;
                haveScissor = false;
                m_last.binds += 2;
            }
            auto it = textureGroups.find(d.texture.id);
            if (it == textureGroups.end())
            {
                RhiBinding b[3]{};
                b[0].slot = 1u;
                b[0].type = RHI_BIND_SAMPLED_TEXTURE;
                b[0].texture = d.texture;
                b[1].slot = 2u;
                b[1].type = RHI_BIND_SAMPLED_TEXTURE;
                b[1].texture = m_dummy;
                b[2].slot = 1u;
                b[2].type = RHI_BIND_SAMPLER;
                b[2].sampler = m_sampler;
                const RhiBindGroupDesc g2{m_layouts[2], b, 3u};
                it = textureGroups.emplace(d.texture.id, rhi_create_bind_group(&g2)).first;
            }
            // 18.1: the group's reads see every earlier write once a by-region barrier lies between them; none is
            // needed when nothing was drawn in the pass since it began (the load) or since the last barrier.
            if (d.read && d.barrier && drawnSinceBarrier && !(m_opt.faults & kFaultNoBarrier))
            {
                rhi_cmd_local_read_barrier(cl, RHI_LOCAL_READ_COLOR);
                ++m_stats.barriers;
                ++m_last.barriers;
                drawnSinceBarrier = false;
            }
            const RhiPipeline pipe = d.read ? m_readPipelines[d.key] : m_pipelines[d.key];
            const RhiBindGroup g0 = d.read ? group0Read[d.target] : group0;
            if (pipe.id != boundPipeline)
            {
                // A pipeline of another layout (READ: group 0 is the target) disturbs the sets bound before it: rebind them.
                rhi_cmd_set_pipeline(cl, pipe);
                boundPipeline = pipe.id;
                boundGroup0 = boundGroup2 = 0;
                ++m_last.binds;
            }
            if (g0.id != boundGroup0)
            {
                rhi_cmd_set_bind_group(cl, 0u, g0);
                rhi_cmd_set_bind_group(cl, 1u, group1);
                boundGroup0 = g0.id;
                boundGroup2 = 0;
                m_last.binds += 2;
            }
            if (it->second.id != boundGroup2)
            {
                rhi_cmd_set_bind_group(cl, 2u, it->second);
                boundGroup2 = it->second.id;
                ++m_last.binds;
            }
            if (!haveScissor || d.scissor.x != boundScissor.x || d.scissor.y != boundScissor.y || d.scissor.w != boundScissor.w ||
                d.scissor.h != boundScissor.h)
            {
                // Options::measureNoFill: one pixel of the scissor, so the list's GPU time is its draws, binds and
                // barriers without the fragments (RENDERER_3D_DESIGN.md 18.5); the picture is wrong.
                const RhiRect one{d.scissor.x, d.scissor.y, 1u, 1u};
                rhi_cmd_set_scissor(cl, m_opt.measureNoFill ? &one : &d.scissor);
                boundScissor = d.scissor;
                haveScissor = true;
                ++m_last.binds;
            }
            if (d.points)
                rhi_cmd_draw(cl, d.count, d.first, 1u);
            else
                rhi_cmd_draw(cl, d.count * 3u, d.first * 3u, 1u);
            drawnSinceBarrier = true;
            ++m_last.draws;
        }
        if (open >= 0)
            rhi_cmd_end_render_pass(cl);
        if (stamps)
            rhi_cmd_write_timestamp(cl, 1u);
        if (m_blitStage.id)
            rhi_destroy_buffer(m_blitStage); // deferred by the RHI until the frame's work is done
        m_blitStage = RhiBuffer{};
        // Replaced uploads: the RHI defers a destroy until the frame slot's work is done, so this is safe before the
        // list is submitted.
        for (RhiTexture t : m_retired)
            rhi_destroy_texture(t);
        m_retired.clear();
        return true;
    }

    RhiTexture Gs2dVk::sampleTarget(RhiCommandList cl, uint32_t target)
    {
        if (!m_created || target > 1u)
            return RhiTexture{};
        transition(cl, m_targets[target], m_targetState[target], RHI_STATE_SHADER_READ);
        return m_targets[target];
    }

    bool Gs2dVk::readColour(uint32_t target, std::vector<uint32_t> &rgba)
    {
        if (!m_created || target > 1u)
            return false;
        rhi_wait_frame();
        RhiCommandList cl = rhi_begin_commands();
        transition(cl, m_targets[target], m_targetState[target], RHI_STATE_COPY_SRC);
        rhi_submit(cl);
        rgba.resize(static_cast<size_t>(kWidth) * kHeight);
        uint32_t pitch = 0;
        return rhi_readback_texture(m_targets[target], RHI_ASPECT_COLOR, rgba.data(), rgba.size() * 4u, &pitch) && pitch == kWidth * 4u;
    }

    bool Gs2dVk::readDepth(std::vector<uint32_t> &z)
    {
        if (!m_created)
            return false;
        rhi_wait_frame();
        RhiCommandList cl = rhi_begin_commands();
        transition(cl, m_depth, m_depthState, RHI_STATE_COPY_SRC);
        rhi_submit(cl);
        std::vector<float> d(static_cast<size_t>(kWidth) * kHeight);
        uint32_t pitch = 0;
        if (!rhi_readback_texture(m_depth, RHI_ASPECT_DEPTH, d.data(), d.size() * 4u, &pitch) || pitch != kWidth * 4u)
            return false;
        z.resize(d.size());
        bool exact = true;
        for (size_t i = 0; i < d.size(); ++i)
        {
            const double v = static_cast<double>(d[i]) * 16777216.0;
            z[i] = static_cast<uint32_t>(v);
            exact &= static_cast<double>(z[i]) == v;
        }
        return exact;
    }

    bool Gs2dVk::probe(const DrawList &list, bool uv, std::vector<uint32_t> &out)
    {
        if (!m_created)
            return false;
        rhi_wait_frame();
        RhiCommandList cl = rhi_begin_commands();
        std::vector<Draw> draws;
        std::vector<RhiTexture> fresh;
        if (!build(list, draws, fresh, cl))
        {
            rhi_submit(cl);
            return false;
        }
        // The probe draws every triangle record (no alpha test): flags carry the mode, records rewritten in place.
        if (uv)
        {
            for (Gs2dPrim &r : m_scratch)
                r.flags |= kPrimProbeUv;
            if (!upload(m_scratch))
                return false;
        }
        transition(cl, m_probeTarget, m_probeState, RHI_STATE_RENDER_TARGET);
        RhiRenderPassDesc pass{};
        pass.color[0].texture = m_probeTarget;
        pass.color[0].load = RHI_LOAD_CLEAR;
        pass.color[0].store = RHI_STORE_STORE;
        pass.colorCount = 1u;
        pass.width = kWidth;
        pass.height = kHeight;
        rhi_cmd_begin_render_pass(cl, &pass);
        const RhiViewport vp{-4096.0f, -4096.0f, 8192.0f, 8192.0f, 0.0f, 1.0f};
        rhi_cmd_set_viewport(cl, &vp);
        RhiBinding rb{};
        rb.slot = 0u;
        rb.type = RHI_BIND_STORAGE_BUFFER;
        rb.buffer = m_records[rhi_frame_slot()];
        const RhiBindGroupDesc g0{m_layouts[0], nullptr, 0u}, g1{m_layouts[1], &rb, 1u};
        rhi_cmd_set_pipeline(cl, m_probePipeline);
        rhi_cmd_set_bind_group(cl, 0u, rhi_create_bind_group(&g0));
        rhi_cmd_set_bind_group(cl, 1u, rhi_create_bind_group(&g1));
        for (const Draw &d : draws)
        {
            if (d.points)
                continue; // points
            RhiBinding b[3]{};
            b[0].slot = 1u;
            b[0].type = RHI_BIND_SAMPLED_TEXTURE;
            b[0].texture = d.texture;
            b[1].slot = 2u;
            b[1].type = RHI_BIND_SAMPLED_TEXTURE;
            b[1].texture = m_dummy;
            b[2].slot = 1u;
            b[2].type = RHI_BIND_SAMPLER;
            b[2].sampler = m_sampler;
            const RhiBindGroupDesc g2{m_layouts[2], b, 3u};
            rhi_cmd_set_bind_group(cl, 2u, rhi_create_bind_group(&g2));
            rhi_cmd_set_scissor(cl, &d.scissor);
            rhi_cmd_draw(cl, d.count * 3u, d.first * 3u, 1u);
        }
        rhi_cmd_end_render_pass(cl);
        transition(cl, m_probeTarget, m_probeState, RHI_STATE_COPY_SRC);
        rhi_submit(cl);
        out.resize(static_cast<size_t>(kWidth) * kHeight * 4u);
        uint32_t pitch = 0;
        const bool ok = rhi_readback_texture(m_probeTarget, RHI_ASPECT_COLOR, out.data(), out.size() * 4u, &pitch) && pitch == kWidth * 16u;
        rhi_wait_idle();
        for (RhiTexture t : m_retired)
            rhi_destroy_texture(t);
        m_retired.clear();
        return ok;
    }
}

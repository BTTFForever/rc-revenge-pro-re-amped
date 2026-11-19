#include "blit_presenter.h"

#include "blit_frag.h"
#include "blit_vert.h"

#include <cstring>

namespace rc::rd
{
    namespace
    {
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
    }

    BlitPresenter::~BlitPresenter() { destroy(); }

    bool BlitPresenter::create()
    {
        static const RhiBindSlot pictureSlots[2] = {{1u, RHI_BIND_SAMPLED_TEXTURE, 1u << RHI_STAGE_FRAGMENT},
                                                    {1u, RHI_BIND_SAMPLER, 1u << RHI_STAGE_FRAGMENT}};
        const RhiBindGroupLayoutDesc group0{nullptr, 0u, "blit group0"};
        const RhiBindGroupLayoutDesc group1{nullptr, 0u, "blit group1"};
        const RhiBindGroupLayoutDesc group2{pictureSlots, 2u, "blit picture"};
        m_layouts[0] = rhi_create_bind_group_layout(&group0);
        m_layouts[1] = rhi_create_bind_group_layout(&group1);
        m_layouts[2] = rhi_create_bind_group_layout(&group2);
        m_vertex = makeShader(RHI_STAGE_VERTEX, spv_blit_vert, sizeof(spv_blit_vert), "blit.vert");
        m_fragment = makeShader(RHI_STAGE_FRAGMENT, spv_blit_frag, sizeof(spv_blit_frag), "blit.frag");
        RhiSamplerDesc samplerDesc{};
        samplerDesc.mag = samplerDesc.min = samplerDesc.mip = RHI_FILTER_NEAREST;
        samplerDesc.s = samplerDesc.t = RHI_WRAP_CLAMP;
        samplerDesc.maxAnisotropy = 1.0f;
        m_sampler = rhi_create_sampler(&samplerDesc);
        RhiPipelineDesc desc{};
        desc.vertex = m_vertex;
        desc.fragment = m_fragment;
        desc.layouts = m_layouts;
        desc.layoutCount = 3u;
        desc.topology = RHI_TOPO_TRIANGLE_LIST;
        desc.cullNone = true;
        desc.blend[0].enable = false;
        desc.blend[0].writeMask = 0xFu;
        desc.colorFormats[0] = RHI_FMT_RGBA8_UNORM;
        desc.colorCount = 1u;
        desc.depthFormat = RHI_FMT_UNKNOWN;
        desc.debugName = "blit";
        m_pipeline = rhi_create_pipeline(&desc);
        m_created = m_layouts[0].id && m_layouts[1].id && m_layouts[2].id && m_vertex.id && m_fragment.id && m_sampler.id && m_pipeline.id;
        return m_created;
    }

    void BlitPresenter::destroy()
    {
        if (!m_created && !m_pipeline.id && !m_picture.id)
            return;
        rhi_wait_idle();
        if (m_upload.id)
            rhi_destroy_buffer(m_upload);
        if (m_picture.id)
            rhi_destroy_texture(m_picture);
        if (m_target.id)
            rhi_destroy_texture(m_target);
        if (m_pipeline.id)
            rhi_destroy_pipeline(m_pipeline);
        if (m_sampler.id)
            rhi_destroy_sampler(m_sampler);
        if (m_vertex.id)
            rhi_destroy_shader(m_vertex);
        if (m_fragment.id)
            rhi_destroy_shader(m_fragment);
        for (RhiBindGroupLayout &l : m_layouts)
            if (l.id)
                rhi_destroy_bind_group_layout(l);
        for (RhiBindGroupLayout &l : m_layouts)
            l = RhiBindGroupLayout{};
        m_vertex = m_fragment = RhiShader{};
        m_pipeline = RhiPipeline{};
        m_sampler = RhiSampler{};
        m_picture = m_target = RhiTexture{};
        m_upload = RhiBuffer{};
        m_width = m_height = 0;
        m_created = false;
    }

    bool BlitPresenter::resize(uint32_t width, uint32_t height)
    {
        if (m_picture.id && m_width == width && m_height == height)
            return true;
        rhi_wait_idle();
        if (m_upload.id)
            rhi_destroy_buffer(m_upload);
        if (m_picture.id)
            rhi_destroy_texture(m_picture);
        if (m_target.id)
            rhi_destroy_texture(m_target);
        RhiTextureDesc tex{};
        tex.width = width;
        tex.height = height;
        tex.mipLevels = 1u;
        tex.format = RHI_FMT_RGBA8_UNORM;
        tex.usage = RHI_TEX_SAMPLED | RHI_TEX_COPY_DST | RHI_TEX_COPY_SRC;
        tex.debugName = "displayed picture";
        m_picture = rhi_create_texture(&tex);
        tex.usage = RHI_TEX_RENDER_TARGET | RHI_TEX_COPY_SRC;
        tex.debugName = "blit target";
        m_target = rhi_create_texture(&tex);
        const RhiBufferDesc buf{static_cast<uint64_t>(width) * height * 4u, RHI_BUF_COPY_SRC, RHI_MEM_UPLOAD, "displayed picture upload"};
        m_upload = rhi_create_buffer(&buf);
        m_width = width;
        m_height = height;
        return m_picture.id && m_target.id && m_upload.id;
    }

    bool BlitPresenter::present(const uint8_t *rgba, uint32_t width, uint32_t height, std::vector<uint8_t> &displayed,
                                std::vector<uint8_t> *uploaded)
    {
        if (!m_created || !width || !height || !resize(width, height))
            return false;
        rhi_wait_frame();
        const size_t bytes = static_cast<size_t>(width) * height * 4u;
        void *mapped = rhi_map_buffer(m_upload);
        if (!mapped)
            return false;
        std::memcpy(mapped, rgba, bytes);
        rhi_unmap_buffer(m_upload);

        RhiCommandList cl = rhi_begin_commands();
        const RhiTextureBarrier toCopy{m_picture, RHI_STATE_UNDEFINED, RHI_STATE_COPY_DST};
        rhi_cmd_barrier(cl, &toCopy, 1u);
        rhi_cmd_copy_buffer_to_texture(cl, m_upload, 0u, width * 4u, m_picture, 0u, RhiRect{0, 0, width, height});
        const RhiTextureBarrier toRead{m_picture, RHI_STATE_COPY_DST, RHI_STATE_SHADER_READ};
        rhi_cmd_barrier(cl, &toRead, 1u);
        const RhiTextureBarrier toTarget{m_target, RHI_STATE_UNDEFINED, RHI_STATE_RENDER_TARGET};
        rhi_cmd_barrier(cl, &toTarget, 1u);
        RhiRenderPassDesc pass{};
        pass.color[0].texture = m_target;
        pass.color[0].load = RHI_LOAD_DONT_CARE;
        pass.color[0].store = RHI_STORE_STORE;
        pass.colorCount = 1u;
        pass.width = width;
        pass.height = height;
        rhi_cmd_begin_render_pass(cl, &pass);
        const RhiViewport viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
        rhi_cmd_set_viewport(cl, &viewport);
        const RhiRect scissor{0, 0, width, height};
        rhi_cmd_set_scissor(cl, &scissor);
        RhiBinding bindings[2]{};
        bindings[0].slot = 1u;
        bindings[0].type = RHI_BIND_SAMPLED_TEXTURE;
        bindings[0].texture = m_picture;
        bindings[0].aspect = RHI_ASPECT_COLOR;
        bindings[1].slot = 1u;
        bindings[1].type = RHI_BIND_SAMPLER;
        bindings[1].sampler = m_sampler;
        const RhiBindGroupDesc g0{m_layouts[0], nullptr, 0u};
        const RhiBindGroupDesc g1{m_layouts[1], nullptr, 0u};
        const RhiBindGroupDesc g2{m_layouts[2], bindings, 2u};
        rhi_cmd_set_pipeline(cl, m_pipeline);
        rhi_cmd_set_bind_group(cl, 0u, rhi_create_bind_group(&g0));
        rhi_cmd_set_bind_group(cl, 1u, rhi_create_bind_group(&g1));
        rhi_cmd_set_bind_group(cl, 2u, rhi_create_bind_group(&g2));
        rhi_cmd_draw(cl, 3u, 0u, 1u);
        rhi_cmd_end_render_pass(cl);
        const RhiTextureBarrier targetToCopy{m_target, RHI_STATE_RENDER_TARGET, RHI_STATE_COPY_SRC};
        rhi_cmd_barrier(cl, &targetToCopy, 1u);
        if (uploaded)
        {
            const RhiTextureBarrier pictureToCopy{m_picture, RHI_STATE_SHADER_READ, RHI_STATE_COPY_SRC};
            rhi_cmd_barrier(cl, &pictureToCopy, 1u);
        }
        rhi_submit(cl);

        uint32_t pitch = 0;
        displayed.resize(bytes);
        if (!rhi_readback_texture(m_target, RHI_ASPECT_COLOR, displayed.data(), bytes, &pitch) || pitch != width * 4u)
            return false;
        if (uploaded)
        {
            uploaded->resize(bytes);
            if (!rhi_readback_texture(m_picture, RHI_ASPECT_COLOR, uploaded->data(), bytes, &pitch) || pitch != width * 4u)
                return false;
        }
        ++m_presents;
        return true;
    }
}

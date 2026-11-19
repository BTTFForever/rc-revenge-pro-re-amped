#pragma once

// The GPU half of the frame-buffer blit (renderer plan P1 step 6; RENDERER_FRAME_RECORD.md section 18): a displayed
// picture (FrameBlit::Picture's RGBA8, alpha raw) uploaded into an RHI texture and drawn one texel a pixel into a
// colour target of the picture's size with the runtime's presentation transform (R, G, B as stored, alpha 1), then
// read back. That is the presenter's path without the window's letterbox, so the read-back can be held against the
// reference bit for bit. Needs a device made with rhi_init (headless, sdlWindow null, is enough); one thread.
// The live renderer will draw the same texture into its swapchain image instead of the off-screen target.

#include "rhi.h"

#include <cstdint>
#include <vector>

namespace rc::rd
{
    class BlitPresenter
    {
    public:
        BlitPresenter() = default;
        ~BlitPresenter();
        BlitPresenter(const BlitPresenter &) = delete;
        BlitPresenter &operator=(const BlitPresenter &) = delete;

        // The pipeline and sampler; false when the device could not make them.
        bool create();
        void destroy();

        // Upload, draw, wait, read back. displayed: width * height * 4 bytes, RGBA, the target as drawn. uploaded (may
        // be null): the texture itself read back, alpha and all, to show the upload kept every byte.
        bool present(const uint8_t *rgba, uint32_t width, uint32_t height, std::vector<uint8_t> &displayed,
                     std::vector<uint8_t> *uploaded = nullptr);

        uint64_t presents() const { return m_presents; }

    private:
        bool resize(uint32_t width, uint32_t height);

        RhiBindGroupLayout m_layouts[3]{};
        RhiShader m_vertex{}, m_fragment{};
        RhiPipeline m_pipeline{};
        RhiSampler m_sampler{};
        RhiTexture m_picture{}, m_target{};
        RhiBuffer m_upload{};
        uint32_t m_width = 0, m_height = 0;
        uint64_t m_presents = 0;
        bool m_created = false;
    };
}

#pragma once

// rc_replay's blit consumer (renderer plan P1 step 6; RENDERER_FRAME_RECORD.md sections 10.2 and 18): the displayed
// picture built from uploads into the frame buffer and DISPFB2 (render/blit FrameBlit), on the CPU and through the
// RHI on Vulkan (render/blit BlitPresenter, headless), against the dump's reference pictures.
//
// Each cycle is classified by the words of GS memory its picture shows: upload-only (no word a draw may have
// written since the keyframe or since an upload replaced it) or not applicable (a draw may have written some: steps
// 7-9). Upload-only pictures with a reference must equal it bit for bit: the CPU picture against REFI (all four bytes)
// or, where the dump keeps only the hash, against END.refHash; the GPU's read-back against the reference after the
// presentation transform (alpha 255), and the uploaded texture read back against the CPU picture (alpha included).

#include "consumer.h"

#include <cstdint>
#include <memory>
#include <string>

namespace rc::replay
{
    struct BlitOptions
    {
        bool verify = false, quiet = false;
        bool gpu = true;        // --no-gpu: the CPU picture only
        bool validation = false; // --validation: the Vulkan validation layer (ICO_VK_VALIDATION overrides)
        std::string pngDir;     // the upload-only pictures (CPU), and on a mismatch the reference and the GPU's
        // --fault tile:<cycle>:<n>     transfer n (from 0) of that cycle loses its data (a dropped movie tile)
        // --fault dispfb:<cycle>       the picture of that cycle is read with DISPFB2's FBP xor 0x50 (the other buffer)
        // --fault byte:<cycle>:<pixel> the red byte of that displayed pixel flipped in the shadow's memory before the read
        // --fault gpubyte:<cycle>:<pixel> the same byte flipped in the GPU's upload only
        std::string faultKind;
        int64_t faultCycle = -1;
        uint32_t faultN = 0;
    };

    class BlitCheck final : public rc::rec::Consumer
    {
    public:
        explicit BlitCheck(BlitOptions options);
        ~BlitCheck() override;

        // Makes the headless Vulkan device and the presenter when options.gpu; false (with a message) if it cannot.
        bool startGpu();
        void stopGpu();

        void open(const rc::rec::FileInfo &info) override;
        void keyframe(const rc::rec::Keyframe &kf) override;
        void cycleBegin(uint64_t cycle) override;
        void hw(uint32_t address, uint32_t width, uint64_t value, uint32_t callerRa) override;
        void kick(const rc::rec::KickView &k, const rc::rec::RamView &ram) override;
        void cycleEnd(const rc::rec::CycleInfo &info, const uint8_t *refRgba) override;

        bool failed() const;
        void summary() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };
}

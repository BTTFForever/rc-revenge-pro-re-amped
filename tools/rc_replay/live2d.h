#pragma once

// rc_replay's live2d consumer (renderer plan P1 step 7 batch 5; RENDERER_2D_DESIGN.md 10.3's live half and section 20;
// step 8 batch 4: K2 drawn, RENDERER_3D_DESIGN.md section 20): the live renderer's own reader and presenter
// (render/live/live2d.h) run on an exact dump, in front of the emulated machine, and every displayed picture is judged.
//
//   - The reader (FrameReader, K2 and K1) starts at the dump's first keyframe instead of reset, publishes each cycle's
//     list; the presenter draws it through Gs2dVk (read mode by default, --vk-blend ff) and the shown picture
//     (target[DISPFB2.FBP]'s top rows) is read back.
//   - Pass, per cycle with a reference picture: every pixel equals the reference (R, G, B, A as stored). Before step 8
//     batch 4 K2's pixels were left out; they are still counted (a pixel a channel 1 kick changed in the shown target or
//     in Z, read from the emulated machine's memory around each such kick, kept until a clear or upload kick).
//   - --live-shots <dir>: the game's own screenshots (RC_VK_SHOT_DIR, native_<cycle>.rgba) of cycles in the dump; at each,
//     the off-line native picture must equal the live one on every pixel: the live session drew what the reader and
//     presenter draw from the recorded kicks.
// --png <dir>: the shot cycles' and the first failing cycle's pictures (native, reference, K2 mask, live, difference).

#include "consumer.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace rc::replay
{
    struct Live2dOptions
    {
        bool quiet = false, validation = false;
        std::string blend = "auto"; // read, ff
        std::string shotsDir, pngDir;
        std::function<const uint8_t *()> vram; // the emulated GS memory (the emu machine behind this consumer)
    };

    class Live2dCheck final : public rc::rec::Consumer
    {
    public:
        explicit Live2dCheck(const Live2dOptions &options);
        ~Live2dCheck() override;

        bool start(std::string *why);

        void open(const rc::rec::FileInfo &info) override;
        void keyframe(const rc::rec::Keyframe &kf) override;
        void cycleBegin(uint64_t cycle) override;
        void hw(uint32_t address, uint32_t width, uint64_t value, uint32_t callerRa) override;
        void kick(const rc::rec::KickView &k, const rc::rec::RamView &ram) override;
        void cycleEnd(const rc::rec::CycleInfo &info, const uint8_t *refRgba) override;
        // The emu machine has run kick k.
        void afterKick(const rc::rec::KickView &k);

        bool failed() const;
        void summary();

    private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };
}

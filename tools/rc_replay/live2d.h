#pragma once

// rc_replay's live2d consumer (renderer plan P1 step 7 batch 5; RENDERER_2D_DESIGN.md 10.3's live half and section 20):
// the live 2D renderer's own reader and presenter (render/live/live2d.h) run on an exact dump, in front of the emulated
// machine, and every displayed picture is judged.
//
//   - The reader (FrameReader with PATH1's primitives left out, FrameBlit) starts at the dump's first keyframe instead of
//     reset, publishes each cycle's list; the presenter draws it through Gs2dVk (read mode by default, --vk-blend ff) and
//     the shown picture (target[DISPFB2.FBP]'s top rows, or the upload-only picture) is read back.
//   - K2's pixels: before and after every channel 1 kick the emulated machine runs, both colour targets (FBP 0x0, 0x50,
//     CT32) and the Z buffer (ZBP 0xa0, Z32) are read from its GS memory; a pixel K2 changed in either the shown target or
//     Z is K2's in that cycle (the native picture has no K2: there it is the clear, and a K1 layer over it blends over
//     something else).
//   - Pass, per cycle with a reference picture: every pixel that is not K2's equals the reference (R, G, B, A as stored).
//     Reported: pixels compared, K2's, and of the compared those K1 changed (not the cycle's clear colour).
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

#pragma once

// The chained replay (renderer plan P1 step 8 batch 3; RENDERER_3D_DESIGN.md 11.5 and section 19): rc_replay
// --consumer vk. The frame reader (VIF1 reader + the C++ VU1 references + the GIF reader, one GS register file) and
// Gs2dVk on an exact dump with no emulated machine beside them.
//
//   - Seeded once: at the dump's first keyframe (and at one after a save-state load, where the recording itself starts
//     again) both colour targets come from GSVR (CT32 at FBP 0x0 and 0x50, 640 x 256), the depth buffer from the Z
//     buffer at ZBP 0xa0 (the low 24 bits), texture memory goes into the reader's shadow, the register file comes from
//     GSRG, the VU1 and VIF1 state from VU1M, VU1R and VIF1. Nothing is seeded after that: no kick's recorded register
//     file is read, and later keyframes are only compared.
//   - Every cycle's list (uploads into the frame buffers, the flip's clear, K2, K1, in the order the game sent them) is
//     drawn through Gs2dVk: one pass per target between the list's changes of target, the exact blend by default.
//   - At every cycle close the top H rows of target[DISPFB2.FBP] are read back: their Hash64 must equal END.refHash,
//     and where the dump stores the picture (REFI) it is compared pixel by pixel.
//   - At every later keyframe, the final one included: both colour targets and the depth buffer must equal the frame
//     buffers and the Z buffer of that keyframe's GSVR over 640 x 256 (colour in all four bytes, Z in its 24 bits).
//
// --png <dir>: on the first failing cycles the Vulkan picture, the reference (where stored) and their difference; on a
// failing keyframe the planes that differ. Without a stored picture the line names the emu --png command that makes
// the reference. --fault depth|clear|texture|flip:<cycle>[:<cycles>] plants one of 11.5's faults for <cycles> cycles
// (default 1; 0: to the end).

#include "consumer.h"

#include <cstdint>
#include <memory>
#include <string>

namespace rc::replay
{
    struct FrameVkOptions
    {
        bool verify = false, quiet = false, validation = false;
        std::string blend = "auto"; // read, ff
        std::string pngDir, dumpPath;
        // Planted faults (11.5): "depth" the depth buffer zeroed after each channel 1 kick's draws (not carried from K2
        // to K1), "clear" the flip's clear left out, "texture" a TEXTURE command for an id sent before left out (the
        // generation after an upload not re-sent), "flip" the picture read from the other target.
        std::string fault;
        int64_t faultCycle = -1;
        uint32_t faultCycles = 1;
    };

    class FrameVkCheck final : public rc::rec::Consumer
    {
    public:
        explicit FrameVkCheck(const FrameVkOptions &options);
        ~FrameVkCheck() override;

        bool start(std::string *why);

        void open(const rc::rec::FileInfo &info) override;
        void keyframe(const rc::rec::Keyframe &kf) override;
        void cycleBegin(uint64_t cycle) override;
        void hw(uint32_t address, uint32_t width, uint64_t value, uint32_t callerRa) override;
        void kick(const rc::rec::KickView &k, const rc::rec::RamView &ram) override;
        void cycleEnd(const rc::rec::CycleInfo &info, const uint8_t *refRgba) override;

        bool failed() const;
        // Shuts the device down (so the validation count is final) and prints the report.
        void summary();

    private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };
}

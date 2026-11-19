#pragma once

// Check (c) of RENDERER_2D_DESIGN.md 10.3 (renderer plan P1 step 7 batch 3): the reader's draw list drawn through
// Gs2dVk (render/gs2d/gs2d_vk) over the emulated GS's own frame and Z buffers, compared with what the emulated GS drew.
//
// rc_replay --consumer gif2d --vk runs the gif2d consumer in front of the emu machine (as --with-emu does). For every
// channel 2 kick that draws, before the machine runs it:
//   1. the colour target(s) the kick draws into (FRAME FBP 0x0 / 0x50, CT32, 640 x 256) and the Z buffer (ZBP, Z32
//      layout, the low 24 bits) are read from the emulated GS memory with render/tex's gs_layout;
//   2. after the machine has run the kick, the same planes are read again: the reference;
//   3. for each source-bias setting asked for (Gs2dVk::Options::sourceBias, off and on by default) the targets and the
//      depth texture are seeded with the planes of 1, the kick's commands (with the state in force and every TEXTURE
//      not given to the GPU yet) are drawn, and colour and depth are read back and compared with 2.
// Pass, per pixel: alpha and Z equal; R, G, B: -L <= Vulkan - reference <= 2L with the bias off, -L <= ... <= L with it
// on (divergence D-2D-1, RENDERER_2D_DESIGN.md 15.3 / 16), where L is the number of the kick's primitives with an
// inexact blend plan (planState: blend on, not exactBlend) that cover the pixel (the reference's coverage rules inside the
// scissor; the design said "whose scissored bounding box holds it", which counts both halves of every rectangle); so equal
// wherever L is 0. The report: per bias, the pixels compared / equal / within the bound / out of it, the difference
// histogram by draw state (the state of the last inexact primitive over the pixel), by L, the largest L, the worst
// differences.

#include "consumer.h"

#include "../../render/gs2d/gs_state.h"

#include <cstdint>
#include <memory>
#include <string>

namespace rc::replay
{
    struct Gif2dVkOptions
    {
        bool validation = false, quiet = false;
        uint32_t biasMask = 3u;  // bit 0: run with the source bias off, bit 1: on
        uint32_t faults = 0;     // Gs2dVk::Fault bits (planted, 9.3)
        bool faultTexa = false;  // the TEXA fault is the reader's (handled by gif2d)
        std::string pngDir;      // the first failing kick's planes (before, Vulkan, reference, difference)
    };

    // Planted fault names for --vk-fault: atst-less, drop-fail, split-per-batch, z-gequal, fix, colour-swap,
    // no-half-pixel, bias-always, texel-row. False for an unknown name.
    bool gif2dVkFault(const std::string &name, uint32_t &bits);

    class Gif2dVkCheck
    {
    public:
        explicit Gif2dVkCheck(const Gif2dVkOptions &options);
        ~Gif2dVkCheck();

        bool start(std::string *why);
        void stop();

        void cycleBegin(uint64_t cycle);
        // The reader has taken kick k: list[first, last) are its commands. judge: the kick drew and nothing else (no
        // transfer); vram: the emulated GS memory before the machine runs the kick.
        void kick(const rc::rec::KickView &k, const rc::rd::DrawList &list, uint32_t first, uint32_t last, bool judge, const uint8_t *vram);
        // The machine has run kick k: vram holds the reference.
        void after(const rc::rec::KickView &k, const uint8_t *vram);
        // Commands of the cycle no kick took (TEXTURE commands are carried to the next judged kick).
        void cycleEnd(const rc::rd::DrawList &list);

        bool failed() const;
        // Shuts the device down (so the validation count is final) and prints the report.
        void summary();

    private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };
}

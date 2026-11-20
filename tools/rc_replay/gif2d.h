#pragma once

// rc_replay's gif2d consumer (renderer plan P1 step 7, RENDERER_2D_DESIGN.md section 10): the 2D reader
// (render/gs2d Gif2dReader) on a dump, with the checks of 10.1 and 10.2.
//
//   (a) register files: at every kick with a recorded register file, every register the reader knows (from the
//       stream or seeded) must equal the recorded value. (Step 8) The reader is the frame reader: PATH1's packets come
//       from the VIF1 reader and its C++ VU1 references into the same register file, so nothing is excused after a
//       channel 1 kick. With --with-emu the vif consumer behind the machine runs the channel 1 kicks beside it
//       (a run the machine cut short is taken as the machine left it, vif.h).
//   (b) --with-emu: the consumer runs in front of the emu consumer; every channel 2 kick that drew is encoded back
//       from the reader's draw list (gs2d_encode) and submitted to the emulated GS in place of the kick, so emu
//       --verify (every picture, every register file before a kick, GS memory at keyframes and at the end) holds only
//       if the draw list is complete.
// It also prints the tables of RENDERER_2D_DESIGN.md section 3 counted from the reader's own draw lists ("H" lines,
// summed by tools/gif2d_agg.py like gif2dstat's).
//
// (c) --vk (step 7 batch 3, gif2d_vk.h): with (b), every drawing kick is also drawn through Gs2dVk over the emulated GS's
//     frame and Z buffers as they were before it and compared with what the emulated GS drew (RENDERER_2D_DESIGN.md
//     10.3, the bound of D-2D-1), with the source bias off and on.
//
// Planted faults: reg:<cycle>:<addr> (the reader loses every write of GS register <addr>, decimal, in that cycle: (a)
// must fail), prim:<cycle>:<n> (primitive n of the cycle left out of the encoded stream: (b)), field:<cycle>:<k>
// (GsState field k written as 0 in every STATE from that cycle on: (b), gs2d_encode.h GsField).

#include "consumer.h"
#include "gif2d_vk.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rc::rd
{
    class FrameReader;
}

namespace rc::replay
{
    struct Gif2dOptions
    {
        bool verify = false, quiet = false, withEmu = false, tables = true;
        std::string faultKind;
        int64_t faultCycle = -1;
        uint32_t faultN = 0;
        // (c)
        bool vk = false;
        Gif2dVkOptions vkOptions;
        std::function<const uint8_t *()> vram; // the emulated GS memory (the emu machine behind this consumer)
    };

    class Gif2dCheck final : public rc::rec::Consumer
    {
    public:
        explicit Gif2dCheck(const Gif2dOptions &options);
        ~Gif2dCheck() override;

        void open(const rc::rec::FileInfo &info) override;
        void keyframe(const rc::rec::Keyframe &kf) override;
        void cycleBegin(uint64_t cycle) override;
        void hw(uint32_t address, uint32_t width, uint64_t value, uint32_t callerRa) override;
        void kick(const rc::rec::KickView &k, const rc::rec::RamView &ram) override;
        void cycleEnd(const rc::rec::CycleInfo &info, const uint8_t *refRgba) override;
        // (step 8) The frame reader it drives; beside the machine the vif consumer attaches to it for channel 1 kicks.
        rc::rd::FrameReader *frameReader();

        // For the emu consumer behind this one: the stream that replaces kick k, or null.
        const std::vector<uint8_t> *substitute(const rc::rec::KickView &k);
        // (c) The emu machine has run kick k.
        void afterKick(const rc::rec::KickView &k);
        // (c) Brings the Vulkan device up; false (and why) when it cannot.
        bool startVk(std::string *why);

        bool failed() const;
        void summary() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };
}

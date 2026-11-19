#pragma once

// rc_replay's gif2d consumer (renderer plan P1 step 7, RENDERER_2D_DESIGN.md section 10): the 2D reader
// (render/gs2d Gif2dReader) on a dump, with the checks of 10.1 and 10.2.
//
//   (a) register files: at every kick with a recorded register file, every register the reader knows (from the
//       stream or seeded) must equal the recorded value. A channel 1 kick makes the registers PATH1 writes unknown
//       (path1Writes); the next channel 2 kick seeds them from its recorded file.
//   (b) --with-emu: the consumer runs in front of the emu consumer; every channel 2 kick that drew is encoded back
//       from the reader's draw list (gs2d_encode) and submitted to the emulated GS in place of the kick, so emu
//       --verify (every picture, every register file before a kick, GS memory at keyframes and at the end) holds only
//       if the draw list is complete.
// It also prints the tables of RENDERER_2D_DESIGN.md section 3 counted from the reader's own draw lists ("H" lines,
// summed by tools/gif2d_agg.py like gif2dstat's).
//
// Planted faults: reg:<cycle>:<addr> (the reader loses every write of GS register <addr>, decimal, in that cycle: (a)
// must fail), prim:<cycle>:<n> (primitive n of the cycle left out of the encoded stream: (b)), field:<cycle>:<k>
// (GsState field k written as 0 in every STATE from that cycle on: (b), gs2d_encode.h GsField).

#include "consumer.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rc::replay
{
    struct Gif2dOptions
    {
        bool verify = false, quiet = false, withEmu = false, tables = true;
        std::string faultKind;
        int64_t faultCycle = -1;
        uint32_t faultN = 0;
    };

    class Gif2dCheck final : public rc::rec::Consumer
    {
    public:
        explicit Gif2dCheck(const Gif2dOptions &options);
        ~Gif2dCheck() override;

        void open(const rc::rec::FileInfo &info) override;
        void keyframe(const rc::rec::Keyframe &kf) override;
        void cycleBegin(uint64_t cycle) override;
        void kick(const rc::rec::KickView &k, const rc::rec::RamView &ram) override;
        void cycleEnd(const rc::rec::CycleInfo &info, const uint8_t *refRgba) override;

        // For the emu consumer behind this one: the stream that replaces kick k, or null.
        const std::vector<uint8_t> *substitute(const rc::rec::KickView &k);

        bool failed() const;
        void summary() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };
}

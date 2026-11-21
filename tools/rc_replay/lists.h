#pragma once

// rc_replay's lists consumer (renderer plan P2 step 9 batch 2; RENDERER_P2_DESIGN.md section 9, check (e)): the fast
// reader (FrameReader::Options::fast) against the strict reader of P1 on one dump, both seeded alike, no emulated machine
// and no GPU. Per kick the GS register file (every register and its provenance) of the two readers must be equal; per
// VU1 run the six register lanes that can reach a later packet (RENDERER_3D_DESIGN.md 3.4: VF05.w, VF06.w, VF07.w,
// VF25.w, VF26.w, VF29.w); per cycle the two draw lists, encoded as DLST encodes them (render/frame/dlst.h), byte for
// byte. The first difference of each kind is named (cycle, kick, register or lane, or command and field or payload byte).
//
// By default both readers are seeded from the dump's first keyframe (and one after a save-state load) with the chained
// replay's options (rc_replay --consumer vk). With --from-reset (a dump recorded from boot: the live sessions of
// tools/live_check.sh) both start from reset with the live reader's options, as the dlst consumer runs it.
//
// Planted faults (--fault <name>[:<cycle>], from that cycle on, in the fast reader only): adc (path1Triangle ignores ADC),
// clut-memo (the CLUT memo ignores memory writes), state-memo (the state-packet memo compares bytes alone). Each must fail.

#include "consumer.h"

#include <cstdint>
#include <memory>
#include <string>

namespace rc::replay
{
    struct ListsOptions
    {
        bool quiet = false;
        bool fromReset = false;  // --from-reset: both readers from reset, no keyframe but one after a state load (the live reader's way)
        std::string fault;       // adc, clut-memo, state-memo (empty: none)
        int64_t faultCycle = 0;
    };

    class ListsCheck final : public rc::rec::Consumer
    {
    public:
        explicit ListsCheck(const ListsOptions &options);
        ~ListsCheck() override;

        void open(const rc::rec::FileInfo &info) override;
        void keyframe(const rc::rec::Keyframe &kf) override;
        void cycleBegin(uint64_t cycle) override;
        void hw(uint32_t address, uint32_t width, uint64_t value, uint32_t callerRa) override;
        void kick(const rc::rec::KickView &k, const rc::rec::RamView &ram) override;
        void cycleEnd(const rc::rec::CycleInfo &info, const uint8_t *refRgba) override;

        bool failed() const;
        void summary();

    private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };

    // False: an unknown fault name.
    bool parseListsFault(const std::string &name);
}

#pragma once

// rc_replay's dlst consumer (renderer plan P1 step 8 batch 4; RENDERER_3D_DESIGN.md section 12 "The live check" and
// section 20): a dump recorded with the live native renderer on (RC_RENDERER=vk RC_VK_PICTURE=native) and
// RC_FRAME_RECORD_DRAWLISTS=1 carries, after each cycle, the list the live frame reader published for it (DLST,
// render/frame/dlst.h). This consumer runs the live reader itself (rc::live::Reader started from reset, as in the game:
// no keyframe but one after a save-state load) on the recorded kicks and compares each cycle's list with the recorded one
// byte for byte (header, commands, payload arena).
//
// The live reader starts at the machine's reset with the game, so the dump must too: a dump recorded from boot
// (RC_FRAME_RECORD_AT=boot+n, first cycle 0). A dump that starts later is refused (its texture ids and generations
// count from boot live and from the dump's start here).
//
// Pass: every recorded cycle has a DLST chunk and every one equals the off-line list. The first difference of a cycle is
// named (command index, op, the field or payload offset).

#include "consumer.h"

#include <cstdint>
#include <memory>
#include <string>

namespace rc::replay
{
    // The first difference of two DLST encodings (header, a command's fields, a payload byte), as a line; `names` says
    // which is which ("off-line / live"). Shared by the dlst and lists consumers.
    std::string dlstFirstDifference(const uint8_t *a, uint64_t aBytes, const uint8_t *b, uint64_t bBytes, const char *names);

    struct DlstOptions
    {
        bool quiet = false;
        // Planted fault (--fault list:<cycle>[:<n>]): byte n of that cycle's off-line encoding flipped (n past the
        // header and the commands, into the arena, when 0); the check must fail there.
        int64_t faultCycle = -1;
        uint32_t faultByte = 0;
    };

    class DlstCheck final : public rc::rec::Consumer
    {
    public:
        explicit DlstCheck(const DlstOptions &options);
        ~DlstCheck() override;

        void open(const rc::rec::FileInfo &info) override;
        void keyframe(const rc::rec::Keyframe &kf) override;
        void cycleBegin(uint64_t cycle) override;
        void hw(uint32_t address, uint32_t width, uint64_t value, uint32_t callerRa) override;
        void kick(const rc::rec::KickView &k, const rc::rec::RamView &ram) override;
        void cycleEnd(const rc::rec::CycleInfo &info, const uint8_t *refRgba) override;
        void chunk(uint32_t id, const uint8_t *data, uint64_t bytes) override;

        bool failed() const;
        bool refused() const; // the dump does not start at boot, or has no DLST chunk
        void summary();

    private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };
}

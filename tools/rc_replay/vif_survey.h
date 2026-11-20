#pragma once

// rc_replay's vifstat consumer (renderer plan P1 step 8's design, RENDERER_3D_DESIGN.md): counts what the VIF1 chains
// (K2) and their PATH1 output use, from an exact dump. Measurement only: it draws nothing and is not the reader.
//
// Without the emulated machine it walks every channel 1 kick's stream with the host's chain walker and decodes the
// VIF codes with a model of the VIF1 registers started from the keyframe's VIF1 and VU1M chunks: codes, UNPACK
// formats with their mask, mode and write cycle, the double buffer, MPG uploads and the program each MSCAL starts.
//
// With --with-emu it runs in front of the emu consumer as well: every run of a VU1 program whose C++ reference exists
// (decomp/vu1) is repeated by the reference on a copy of VU data memory, and its packets and final memory compared
// with the emulated VU1's; a second run of the reference with marked registers shows which registers a program takes
// from an earlier run. Every GIF packet of PATH1 and PATH3 is walked: register writes, the draw states, the geometry
// the reference rasteriser would meet, and per kick the primitives that need the exact blend's destination read with
// the barriers several grouping rules would issue (RENDERER_2D_DESIGN.md 18, 19.5).

#include "consumer.h"

#include <cstdint>
#include <memory>

class PS2Runtime;

namespace rc::replay
{
    class VifSurvey final : public rc::rec::Consumer
    {
    public:
        explicit VifSurvey(PS2Runtime *runtime); // the emu consumer's machine, or null
        ~VifSurvey() override;

        // Registers the surveying wrappers around the six generated VU1 programs (after the generated programs are
        // registered, before the first kick). Only with the emulated machine.
        void installVu1Wrappers();
        // The experiment of the design's decision U1: PATH1's packets reach the machine's GS under the kick's rounding
        // mode instead of the VU1's round toward zero; the pictures are then compared with the recorded references.
        void setRoundNearest(bool on);

        void open(const rc::rec::FileInfo &info) override;
        void keyframe(const rc::rec::Keyframe &kf) override;
        void cycleBegin(uint64_t cycle) override;
        void hw(uint32_t address, uint32_t width, uint64_t value, uint32_t callerRa) override;
        void kick(const rc::rec::KickView &k, const rc::rec::RamView &ram) override;
        void cycleEnd(const rc::rec::CycleInfo &info, const uint8_t *refRgba) override;

        void gifPacket(int path, const uint8_t *data, uint32_t bytes); // the emu consumer's tap
        void afterKick(const rc::rec::KickView &k);                    // the machine has run the kick

        void summary() const; // "V <category>\t<key>\t<n>\t<m>" (sums) and "X <category>\t<key>\t<max>" lines

        struct Impl;

    private:
        std::unique_ptr<Impl> m;
    };
}

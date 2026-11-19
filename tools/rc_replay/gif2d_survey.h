#pragma once

// rc_replay's gif2dstat consumer (renderer plan P1 step 7's design, RENDERER_2D_DESIGN.md): counts what the PATH3
// draws (the 2D packet K1 and the flip's clear) use, from an exact dump. Measurement only: it draws nothing.
//
// Each channel 2 kick starts from its recorded GS register file (so PATH1's writes are in it without running VU1), is
// walked with the GIF walker, and every primitive is assembled and counted: its whole draw state, the distinct values
// of every register a draw depends on, its geometry (flat or Gouraud colour, subpixel fractions, the texel-to-pixel
// mapping per covered pixel as the software GS computes it, texture coordinates outside the texture, Z), lines by
// direction and length, how many inexactly blended layers cover a pixel in a cycle, and whether a 2D primitive can be
// hidden by an earlier one's Z write. With --with-emu it runs in front of the emu consumer and reads the emulated Z
// buffer before each kick: whether the 3D picture under K1 can hide any of it.

#include "consumer.h"

#include <cstdint>
#include <memory>

class PS2Runtime;

namespace rc::replay
{
    class Gif2dSurvey final : public rc::rec::Consumer
    {
    public:
        explicit Gif2dSurvey(PS2Runtime *runtime); // the emu consumer's machine, or null
        ~Gif2dSurvey() override;

        void open(const rc::rec::FileInfo &info) override;
        void cycleBegin(uint64_t cycle) override;
        void kick(const rc::rec::KickView &k, const rc::rec::RamView &ram) override;
        void cycleEnd(const rc::rec::CycleInfo &info, const uint8_t *refRgba) override;

        void summary() const; // "H <category>\t<key>\t<primitives>\t<pixels>" lines, for an aggregating script

    private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };
}

#pragma once

// rc_replay's shapestat consumer (renderer plan P2 step 9's design, RENDERER_P2_DESIGN.md section 3): counts what a
// batch reader for K2 would meet, from a dump, without the emulated machine. Measurement only: it draws nothing.
//
//  - Units: every packet a channel 1 chain calls from the frame's tag array (DMA `call` at depth 0), by its address
//    and a hash of the bytes it delivers: which are the same every time they are called (the shapes' triangle lists)
//    and which are rewritten (the packet rings, the polygon and alpha buffers), with their sizes and runs.
//  - Runs: per VU1 run, what its input triangles do in the program (culled, the quick path, the near-plane and
//    guard-band paths), whether a run is plain (every triangle culled or quick) and whether a bounding-box test would
//    have said so; how often the constants change between runs.
//  - The lean references (shape_lean.cpp): every run repeated by a build of the references whose arithmetic is the
//    host's under round toward zero + FTZ + DAZ; its packets, VU memory and registers compared with the reference's,
//    and its time.
//
// Output: "V <category>\t<key>\t<n>\t<m>" lines (sums) and "X <category>\t<key>\t<max>" lines, as vifstat's
// (tools/vif_agg.py sums them over dumps).

#include "consumer.h"

#include <cstdint>
#include <memory>

namespace rc::replay
{
    class ShapeSurvey final : public rc::rec::Consumer
    {
    public:
        ShapeSurvey();
        ~ShapeSurvey() override;

        void open(const rc::rec::FileInfo &info) override;
        void keyframe(const rc::rec::Keyframe &kf) override;
        void hw(uint32_t address, uint32_t width, uint64_t value, uint32_t callerRa) override;
        void kick(const rc::rec::KickView &k, const rc::rec::RamView &ram) override;
        void cycleEnd(const rc::rec::CycleInfo &info, const uint8_t *refRgba) override;

        void summary() const;

        struct Impl;

    private:
        std::unique_ptr<Impl> m;
    };
}

#pragma once

// rc_replay's vif consumer (renderer plan P1 step 8 batch 1; RENDERER_3D_DESIGN.md section 11.1-11.3, 11.6): the frame
// reader (render/frame: the VIF1 reader with the C++ VU1 references, feeding the GIF reader as PATH1) on a dump.
//
// Without the machine:
//   (a) at every later keyframe the reader's VIF1 registers, micro and data memory and the 32 vector registers equal
//       the keyframe's VIF1, VU1M and VU1R, and its GS register file the keyframe's GSRG; at every kick with a recorded
//       register file, every register of the reader's one register file (PATH3 and PATH1) equals the recorded value,
//       with nothing excused;
//   (b) every channel 1 kick's PATH1 bytes, hashed in order, equal the recorded GIF hash and byte count.
// With --with-emu the consumer runs behind the emu consumer: the machine runs each channel 1 kick first (its six
// generated VU1 programs wrapped: every run's packets, VU memory and registers kept), then the reader runs the same
// kick and is compared with it run by run: (b) packet by packet, with the first difference named and the run written
// out, (a) VU memory and registers after every run and the VIF1 registers and micro memory after every kick. A run the
// machine cut short at its cycle budget is compared up to its end, the reader's packets beyond it are left out and the
// reader takes the machine's state after it (counted, listed; RENDERER_3D_DESIGN.md 6.3). Then (c), --vif-substitute:
//   bytes (default): the reader's PATH1 bytes are handed to the emulated GS in place of the machine's (whose packets
//                    are diverted), so emu --verify (pictures, register files before every kick, GS memory at keyframes,
//                    and the kick's GIF hash) holds only if they are right;
//   list, list-cull: the kick's K2 draw list (cullOffscreen off / on) encoded back into GIF (gs2d_encode) instead;
//   none:            the machine draws its own packets (comparison only).
// Planted faults (--fault <name>:<cycle>[:<n>], RENDERER_3D_DESIGN.md 11.6): unpack, tops, dbf, mpg, mscal, mxcsr,
// lane, packet, vumem (vif1_reader.h VifFault).

#include "consumer.h"
#include "emu.h"

#include "../../render/vif/vif1_reader.h"

#include <cstdint>
#include <memory>
#include <string>

class PS2Runtime;
namespace rc::rd
{
    class FrameReader;
}

namespace rc::replay
{
    struct VifOptions
    {
        bool verify = false, quiet = false, withEmu = false;
        std::string substitute = "bytes"; // with emu: none, bytes, list, list-cull
        rc::rd::VifFault fault = rc::rd::VifFault::None;
        int64_t faultCycle = -1;
        uint32_t faultN = 0;
        std::string outDir;               // where the first differing run is written (default: no file)
        bool noPoison = false;            // --vif-no-poison: the lanes a reference does not reproduce keep its values (measuring them)
        rc::rd::Vu1Library library = rc::rd::Vu1Library::Strict; // --vu1 strict|lean: the build of the references the reader runs
        bool fast = false; // --reader fast (P2 step 9 batch 2): FrameReader::Options::fast; (b) is not available (no PATH1 hash)
    };

    class VifCheck final : public rc::rec::Consumer
    {
    public:
        // runtime: the emu consumer's machine, or null. external: another consumer's frame reader (gif2d --with-emu), which
        // that consumer drives; this one then only runs its channel 1 kicks beside the machine (substitute none) and
        // compares the VU state at keyframes.
        VifCheck(const VifOptions &options, PS2Runtime *runtime, rc::rd::FrameReader *external = nullptr);
        ~VifCheck() override;

        // With the machine: wraps the six generated VU1 programs (after they are registered, before the first kick).
        void installVu1Wrappers();
        // The emu consumer's hook for channel 1 kicks (Emu::setPath1Substitute).
        Emu::Path1Substitute path1(const rc::rec::KickView &k, const rc::rec::RamView &ram);

        void open(const rc::rec::FileInfo &info) override;
        void keyframe(const rc::rec::Keyframe &kf) override;
        void cycleBegin(uint64_t cycle) override;
        void hw(uint32_t address, uint32_t width, uint64_t value, uint32_t callerRa) override;
        void kick(const rc::rec::KickView &k, const rc::rec::RamView &ram) override;
        void cycleEnd(const rc::rec::CycleInfo &info, const uint8_t *refRgba) override;

        bool failed() const;
        void summary() const;

        struct Impl;

    private:
        std::unique_ptr<Impl> m;
    };
}

#pragma once

// rc_replay's tex consumer (renderer plan P1 step 5; RENDERER_FRAME_RECORD.md sections 10.2 and 17): the host's shadow
// of GS texture memory (render/tex) driven from a dump, and its checks.
//
// Alone it applies every host-to-local transfer of the PATH3 kicks and, with --verify, compares the texture area
// (blocks 0x1e00 up) with GSVR at every later keyframe and the final one, and its transfer registers with every
// recorded register file. With --with-emu it runs behind the emu consumer: the texture area is compared with the
// emulated GS's memory after every kick, every texture a draw of any path names is decoded through the cache (so the
// alpha histogram by draw state covers PATH1), and at the end each decoded texture is compared with what the emulated
// GS's rasteriser samples from it (a point-sampled DECAL sprite drawn into the emulated frame buffer and read back).

#include "consumer.h"

#include <cstdint>
#include <memory>
#include <string>

class PS2Runtime;

namespace rc::replay
{
    struct TexOptions
    {
        bool verify = false, quiet = false;
        std::string pngDir; // write every decoded texture as a PNG (RGBA, alpha doubled so 0x80 is opaque)
        // --fault transfer:<cycle>:<n>  transfer n (from 0) of that cycle loses its data (a dropped transfer)
        // --fault kick:<cycle>:<n>      the shadow skips kick n of that cycle; skipping a pixel kick leaves the GIF
        //                               inside its IMAGE tag, which then swallows the next kicks, as the GS's would
        // --fault swizzle:<cycle>:<bits> two neighbouring entries of the 32/16/8/4-bit column table swapped from that
        //                               cycle on (0: from the start)
        // --fault decode:0:<bits>       the same swap made after the last cycle, before the textures are decoded for
        //                               the comparison with the emulated GS's sampling
        std::string faultKind;
        int64_t faultCycle = -1;
        uint32_t faultN = 0;
    };

    class TexCheck final : public rc::rec::Consumer
    {
    public:
        // runtime: the emu consumer's machine when running behind it (--with-emu), else null.
        TexCheck(TexOptions options, PS2Runtime *runtime);
        ~TexCheck() override;

        void open(const rc::rec::FileInfo &info) override;
        void keyframe(const rc::rec::Keyframe &kf) override;
        void cycleBegin(uint64_t cycle) override;
        void tag(const rc::rec::TagView &t) override;
        void kick(const rc::rec::KickView &k, const rc::rec::RamView &ram) override;
        void cycleEnd(const rc::rec::CycleInfo &info, const uint8_t *refRgba) override;
        void close() override;

        // Every GIF packet the emulated machine receives (the emu consumer's tap), any path.
        void gifPacket(int path, const uint8_t *data, uint32_t bytes);

        bool failed() const;
        void summary() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };
}

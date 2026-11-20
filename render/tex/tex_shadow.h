#pragma once

// The shadow of GS texture memory as a consumer of the kick record (renderer plan P1 step 5; RENDERER_FRAME_RECORD.md
// section 17). It starts from a keyframe's GSVR and applies every host-to-local transfer the PATH3 kicks (DMA channel
// 2) carry, through the host's own layout (gs_memory). Live, the same object sits behind the runtime's capture hooks
// and reads guest RAM in place; off-line rc_replay's tex consumer drives it from a dump.
//
// All GS traffic of RC that writes texture memory is on channel 2 (survey): the level image, single textures, palettes
// and the animated textures. The VIF1 chain (PATH1) draws only.

#include "../record/consumer.h"
#include "gif_walk.h"
#include "gs_memory.h"

#include <cstdint>

namespace rc::rd
{
    class TexShadow : public rc::rec::Consumer, private GifSink
    {
    public:
        struct Stats
        {
            uint64_t kicks = 0, streamBytes = 0, absentPages = 0, untranslatable = 0;
            uint64_t applyNs = 0; // time spent walking and applying PATH3 kicks
        };

        GsMemory &memory() { return m_mem; }
        const GsMemory &memory() const { return m_mem; }
        const GifWalker &walker() const { return m_walker; }
        const Stats &stats() const { return m_stats; }

        // Receives the PATH3 register writes the shadow itself has no use for (TEX0, PRIM, vertices...), in order.
        void setObserver(GifSink *observer) { m_observer = observer; }

        // Loads GS memory and the transfer registers from a keyframe (GSVR, GSRG). False without a usable GSVR.
        bool loadKeyframe(const rc::rec::Keyframe &kf);

        // Consumer: a kick on channel 2 is walked (normal mode or chain) and its stream fed to the GIF walker.
        void kick(const rc::rec::KickView &k, const rc::rec::RamView &ram) override;
        // PATH3 bytes from elsewhere (tests).
        void path3(const uint8_t *data, uint32_t bytes);

    private:
        void reg(uint8_t addr, uint64_t value) override;
        void image(const uint8_t *data, uint32_t bytes) override;
        void vertexQ(uint32_t q) override
        {
            if (m_observer)
                m_observer->vertexQ(q);
        }

        GsMemory m_mem;
        GifWalker m_walker;
        GifSink *m_observer = nullptr;
        Stats m_stats;
    };
}

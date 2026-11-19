#include "tex_shadow.h"

#include "../record/chain_walk.h"

#include <chrono>
#include <cstring>

namespace rc::rd
{
    using namespace rc::rec;

    bool TexShadow::loadKeyframe(const Keyframe &kf)
    {
        const StateChunk *vram = kf.find(kStateGsMemory);
        if (!vram || vram->bytes != kGsBytes)
            return false;
        m_mem.load(vram->data);
        if (const StateChunk *regs = kf.find(kStateGsRegs); regs && regs->bytes >= kGsRegCount * 8u)
        {
            uint64_t r[4];
            std::memcpy(r, regs->data + kRegBitbltbuf * 8u, sizeof(r));
            m_mem.setTransferRegisters(r[0], r[1], r[2], r[3]);
        }
        m_walker.reset();
        return true;
    }

    void TexShadow::kick(const KickView &k, const RamView &ram)
    {
        if (k.channel != 2u)
            return;
        const auto t0 = std::chrono::steady_clock::now();
        struct Feed final : WalkVisitor
        {
            TexShadow *self;
            void range(const WalkRange &r) override
            {
                if (!r.isTag && r.bytes)
                    self->m_walker.feed(r.data, r.bytes, *self);
            }
        } feed;
        feed.self = this;
        const WalkResult w = walkChain(k, ram, &feed);
        ++m_stats.kicks;
        m_stats.streamBytes += w.streamBytes;
        m_stats.absentPages += w.absentPageReads;
        m_stats.untranslatable += w.untranslatable;
        m_stats.applyNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    }

    void TexShadow::path3(const uint8_t *data, uint32_t bytes) { m_walker.feed(data, bytes, *this); }

    void TexShadow::reg(uint8_t addr, uint64_t value)
    {
        if (addr >= kRegBitbltbuf && addr <= kRegHwreg)
            m_mem.writeRegister(addr, value);
        else if (m_observer)
            m_observer->reg(addr, value);
    }

    void TexShadow::image(const uint8_t *data, uint32_t bytes) { m_mem.imageData(data, bytes); }
}

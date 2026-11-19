#include "frame_blit.h"

#include <cstring>

namespace rc::rd
{
    using namespace rc::rec;

    namespace
    {
        enum : uint8_t
        {
            kRegPrim = 0x00,
            kRegXyzf2 = 0x04,
            kRegXyz2 = 0x05,
            kRegPrmodecont = 0x1A,
            kRegPrmode = 0x1B,
            kRegScissor1 = 0x40,
            kRegFrame1 = 0x4C,
            kRegZbuf1 = 0x4E,
        };
        // The registers a draw's destination depends on, tracked from PATH3 and checked against recorded files.
        constexpr uint8_t kTracked[] = {kRegPrim, kRegPrmodecont, kRegPrmode, kRegScissor1, kRegScissor1 + 1, kRegFrame1, kRegFrame1 + 1,
                                        kRegZbuf1, kRegZbuf1 + 1};
        constexpr uint32_t kDispfb2 = 0x12000090u;
    }

    FrameBlit::FrameBlit()
    {
        m_shadow.memory().trackOrigins(true);
        m_shadow.setObserver(this);
    }

    void FrameBlit::open(const FileInfo &info)
    {
        if (info.header.width)
            m_width = info.header.width;
        if (info.header.height)
            m_height = info.header.height;
    }

    void FrameBlit::keyframe(const Keyframe &kf)
    {
        // The first keyframe, and one taken after a save-state load, start the shadow; later ones are the machine's
        // memory with its draws in it and are left alone (this consumer's memory holds only uploads).
        if (m_started && !(kf.header.flags & kKeyAfterStateLoad))
            return;
        if (!m_shadow.loadKeyframe(kf))
            return;
        if (const StateChunk *regs = kf.find(kStateGsRegs); regs && regs->bytes >= kGsRegCount * 8u)
        {
            std::memcpy(m_regs, regs->data, kGsRegCount * 8u);
            for (uint8_t a : kTracked)
                m_known[a] = true;
        }
        else
            std::memset(m_known, 0, sizeof(m_known));
        if (const StateChunk *priv = kf.find(kStateGsPriv); priv && priv->bytes >= sizeof(GsPrivRegs))
        {
            GsPrivRegs p;
            std::memcpy(&p, priv->data, sizeof(p));
            m_dispfb2 = p.dispfb2;
        }
        m_started = true;
        ++m_stats.keyframesLoaded;
    }

    void FrameBlit::cycleBegin(uint64_t)
    {
        m_cycle = CycleCounts{};
        const GsMemory::Stats &s = m_shadow.memory().stats();
        m_transfersAtCycle = static_cast<uint32_t>(s.transfers);
        m_intoFrameAtCycle = static_cast<uint32_t>(s.intoFrame);
    }

    void FrameBlit::hw(uint32_t address, uint32_t width, uint64_t value, uint32_t)
    {
        const uint32_t phys = address & 0x1FFFFFFFu;
        if ((phys & ~7u) != kDispfb2)
            return;
        if (width == 64u)
            m_dispfb2 = value;
        else if ((phys & 7u) == 0u)
            m_dispfb2 = (m_dispfb2 & 0xFFFFFFFF00000000ull) | (value & 0xFFFFFFFFull);
        else
            m_dispfb2 = (m_dispfb2 & 0xFFFFFFFFull) | (value << 32);
    }

    void FrameBlit::kick(const KickView &k, const RamView &ram)
    {
        if (!m_started)
            return;
        if (k.regs)
            for (uint8_t a : kTracked)
                if (m_known[a])
                {
                    ++m_stats.regChecks;
                    m_stats.regMismatches += k.regs[a] != m_regs[a];
                }
        if (k.channel == 1u)
        {
            // PATH1: what it draws is the VU1 programs' output (step 8). Anything may have been drawn, and any
            // register written.
            ++m_cycle.vif1Kicks;
            ++m_stats.vif1Kicks;
            m_shadow.memory().markAllDrawn();
            for (uint8_t a : kTracked)
                m_known[a] = false;
            return;
        }
        m_shadow.kick(k, ram);
        const GsMemory::Stats &s = m_shadow.memory().stats();
        m_cycle.transfers = static_cast<uint32_t>(s.transfers) - m_transfersAtCycle;
        m_cycle.transfersIntoFrame = static_cast<uint32_t>(s.intoFrame) - m_intoFrameAtCycle;
    }

    void FrameBlit::reg(uint8_t addr, uint64_t value)
    {
        if (addr == kRegXyz2 || addr == kRegXyzf2)
        {
            drawAt();
            return;
        }
        if (addr < 0x80u)
        {
            m_regs[addr] = value;
            m_known[addr] = true;
        }
    }

    void FrameBlit::markRect(uint32_t basePage, uint32_t fbw, uint32_t pageH, uint64_t scissor)
    {
        const uint32_t x0 = static_cast<uint32_t>(scissor & 0x7FFu), x1 = static_cast<uint32_t>((scissor >> 16) & 0x7FFu);
        const uint32_t y0 = static_cast<uint32_t>((scissor >> 32) & 0x7FFu), y1 = static_cast<uint32_t>((scissor >> 48) & 0x7FFu);
        if (x1 < x0 || y1 < y0)
            return; // an empty scissor draws nothing
        GsMemory &mem = m_shadow.memory();
        if (fbw == 0u)
        {
            mem.markAllDrawn();
            return;
        }
        for (uint32_t py = y0 / pageH; py <= y1 / pageH; ++py)
            for (uint32_t px = x0 / 64u; px <= x1 / 64u; ++px)
                mem.markDrawnPage(basePage + py * fbw + px);
    }

    void FrameBlit::drawAt()
    {
        ++m_cycle.path3Vertices;
        ++m_stats.path3Vertices;
        GsMemory &mem = m_shadow.memory();
        // The context: PRIM's CTXT when PRMODECONT.AC is 1, else PRMODE's.
        const uint8_t attr = (m_regs[kRegPrmodecont] & 1u) ? kRegPrim : kRegPrmode;
        const bool ctxKnown = m_known[kRegPrmodecont] && m_known[attr];
        const uint32_t c = ctxKnown ? static_cast<uint32_t>((m_regs[attr] >> 9) & 1u) : 0u;
        if (!ctxKnown || !m_known[kRegFrame1 + c] || !m_known[kRegScissor1 + c] || !m_known[kRegZbuf1 + c])
        {
            ++m_cycle.path3BlindDraws;
            ++m_stats.path3BlindDraws;
            mem.markAllDrawn();
            return;
        }
        const uint64_t frame = m_regs[kRegFrame1 + c], zbuf = m_regs[kRegZbuf1 + c], scissor = m_regs[kRegScissor1 + c];
        const uint32_t fbp = static_cast<uint32_t>(frame & 0x1FFu), fbw = static_cast<uint32_t>((frame >> 16) & 0x3Fu);
        const uint32_t fpsm = static_cast<uint32_t>((frame >> 24) & 0x3Fu);
        if ((frame >> 32) != 0xFFFFFFFFull) // FBMSK: some colour bit is written
            markRect(fbp, fbw, (fpsm == kPsmCt16 || fpsm == kPsmCt16s) ? 64u : 32u, scissor);
        if (((zbuf >> 32) & 1u) == 0u) // ZMSK clear: Z is written (the frame's width)
        {
            const uint32_t zbp = static_cast<uint32_t>(zbuf & 0x1FFu), zpsm = static_cast<uint32_t>((zbuf >> 24) & 0xFu) | 0x30u;
            markRect(zbp, fbw, (zpsm == 0x32u || zpsm == 0x3Au) ? 64u : 32u, scissor);
        }
    }

    void FrameBlit::buildPicture(uint32_t width, uint32_t height)
    {
        Picture &p = m_picture;
        p.width = width;
        p.height = height;
        p.dispfb2 = m_dispfb2;
        p.fb = DisplayBuffer::decode(m_dispfb2);
        p.supported = p.fb.psm == kPsmCt32 && p.fb.fbw != 0u;
        p.rgba.assign(static_cast<size_t>(width) * height * 4u, 0u);
        p.drawnWords = p.uploadedWords = p.loadedWords = 0u;
        if (!p.supported)
            return;
        const GsMemory &mem = m_shadow.memory();
        const uint8_t *data = mem.data();
        const LayoutTables &t = layoutTables();
        const uint32_t bp = p.fb.fbp * 32u;
        uint32_t counts[3] = {};
        for (uint32_t y = 0; y < height; ++y)
        {
            uint8_t *row = p.rgba.data() + static_cast<size_t>(y) * width * 4u;
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint32_t a = gsAddr32(t, bp, p.fb.fbw, (p.fb.dbx + x) & 0x7FFu, (p.fb.dby + y) & 0x7FFu) & (kGsBytes - 1u);
                std::memcpy(row + x * 4u, data + a, 4);
                ++counts[mem.originAt(a)];
            }
        }
        p.loadedWords = counts[GsMemory::kOriginLoaded];
        p.uploadedWords = counts[GsMemory::kOriginUploaded];
        p.drawnWords = counts[GsMemory::kOriginDrawn];
    }

    void FrameBlit::cycleEnd(const CycleInfo &info, const uint8_t *)
    {
        if (!m_started)
            return;
        ++m_stats.cycles;
        buildPicture(info.end.refWidth ? info.end.refWidth : m_width, info.end.refHeight ? info.end.refHeight : m_height);
    }
}

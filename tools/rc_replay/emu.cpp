// rc_replay's emu consumer: see emu.h and RENDERER_FRAME_RECORD.md section 7.
#include "emu.h"

#include "emu_state.h"
#include "picture.h"
#include "png.h"

#include "ps2_capture_hooks.h"
#include "ps2_runtime.h"

#include <immintrin.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace rc::rec;

namespace rc::replay
{
    namespace
    {
        Emu *g_emu = nullptr;

        // Only the GIF packet hook: every byte submitGifPacket receives while a recorded kick is being replayed.
        const ps2x::capture::Hooks kHooks = [] {
            ps2x::capture::Hooks h{};
            h.gifPacket = [](int path, const uint8_t *data, uint32_t bytes)
            {
                if (g_emu)
                    g_emu->gifPacket(path, data, bytes);
            };
            return h;
        }();

        std::string hex64(uint64_t v)
        {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "%016" PRIx64, v);
            return buf;
        }

        std::string where(uint64_t cycle, uint32_t kick)
        {
            return "cycle " + std::to_string(cycle) + " kick " + std::to_string(kick);
        }
    }

    void prepareEmuEnvironment()
    {
        // The bare GSCpuBackend (GSThreadedBackend::MakeDefault), VU1 and the GIF on the calling thread.
        setenv("PS2X_GS_THREAD", "0", 1);
        setenv("PS2X_GS_GPU", "0", 1);
        setenv("PS2X_GS_HW", "0", 1);
        setenv("PS2X_GS_VK", "0", 1);
        setenv("PS2X_MTVU", "0", 1);
    }

    Emu::Emu(PS2Runtime &runtime, EmuOptions options) : m_rt(runtime), m_opt(std::move(options))
    {
        g_emu = this;
        ps2x::capture::install(&kHooks);
    }

    Emu::~Emu()
    {
        ps2x::capture::install(nullptr);
        g_emu = nullptr;
    }

    void Emu::fail(const std::string &what)
    {
        if (++m_failures <= 40)
            std::printf("FAIL %s\n", what.c_str());
    }

    void Emu::open(const FileInfo &info)
    {
        m_header = info.header;
        m_exact = (m_header.flags & kFileExact) != 0;
        if (m_header.emuStateVersion != emuStateVersion())
            m_fatal = "the dump's emulator state is version " + std::to_string(m_header.emuStateVersion) + ", this build reads version " +
                      std::to_string(emuStateVersion()) + " (re-record it)";
        else if (!ps2x::capture::exactCapable(m_rt))
            m_fatal = "the replay machine is not the synchronous software GS";
        if (m_opt.verify && !m_exact)
            std::printf("note: the dump is not exact (recorded with %.16s): replaying without verifying\n", m_header.gsBackend);
        m_picture.assign(static_cast<size_t>(m_header.width) * m_header.height * 4u, 0u);
    }

    void Emu::keyframe(const Keyframe &kf)
    {
        if (fatal())
            return;
        const bool final = (kf.header.flags & kKeyFinal) != 0;
        // (batch 4) A keyframe after a save-state load holds the loaded machine, not what the cycles before it drew:
        // it is loaded, not compared.
        if (kf.header.flags & kKeyAfterStateLoad)
        {
            ++m_t.keyframesAfterLoad;
            std::printf("keyframe at cycle %" PRIu64 " follows a save-state load: loaded, not compared\n", kf.header.cycle);
        }
        else if (m_started && m_opt.verify && m_exact && (kf.header.flags & kKeyVramExact))
        {
            // GS memory after the replayed cycles against the recorder's.
            const StateChunk *vram = kf.find(kStateGsMemory);
            const uint8_t *mine = m_rt.memory().getGSVRAM();
            uint64_t diff = 0, first = ~0ull;
            if (vram && vram->bytes == kGsMemBytes)
                for (uint64_t i = 0; i < kGsMemBytes; ++i)
                    if (vram->data[i] != mine[i])
                    {
                        if (first == ~0ull)
                            first = i;
                        ++diff;
                    }
            const StateChunk *regs = kf.find(kStateGsRegs);
            uint32_t regDiff = 0, firstReg = ~0u;
            if (regs && regs->bytes == sizeof(ps2x::capture::g_gsRawRegs))
                for (uint32_t r = 0; r < kGsRegCount; ++r)
                {
                    uint64_t v;
                    std::memcpy(&v, regs->data + r * 8u, 8);
                    if (v != ps2x::capture::g_gsRawRegs[r])
                    {
                        if (firstReg == ~0u)
                            firstReg = r;
                        ++regDiff;
                    }
                }
            const bool ok = vram && vram->bytes == kGsMemBytes && diff == 0 && regDiff == 0;
            (final ? m_t.finalChecked : m_t.keyframesChecked)++;
            if (ok)
                (final ? m_t.finalOk : m_t.keyframesOk)++;
            else
            {
                char buf[200];
                std::snprintf(buf, sizeof(buf), "%s keyframe at cycle %" PRIu64 ": %" PRIu64 " bytes of GS memory differ (first at 0x%" PRIx64
                              "), %u GS registers differ (first 0x%x)",
                              final ? "final" : "the", kf.header.cycle, diff, first == ~0ull ? 0ull : first, regDiff, firstReg);
                fail(buf);
            }
        }
        if (final)
            return;
        std::string error;
        if (!loadKeyframeState(m_rt, kf, error))
        {
            m_fatal = "keyframe at cycle " + std::to_string(kf.header.cycle) + ": " + error;
            return;
        }
        ++m_t.keyframesLoaded;
        m_started = true;
    }

    void Emu::cycleBegin(uint64_t cycle)
    {
        m_cycleFirstFail = ~0ull;
        m_cycle = cycle;
        m_cyclePages = m_cycleHw = 0;
    }

    void Emu::pages(uint32_t firstPage, uint32_t count, const uint8_t *data)
    {
        if (!m_started || fatal())
            return;
        PS2Memory &mem = m_rt.memory();
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t page = firstPage + i;
            uint8_t *dst = page < kRamPages ? mem.getRDRAM() + static_cast<size_t>(page) * kPageBytes
                                            : mem.getScratchpad() + static_cast<size_t>(page - kRamPages) * kPageBytes;
            std::memcpy(dst, data + static_cast<size_t>(i) * kPageBytes, kPageBytes);
            if (i == 0 && m_cyclePages == 0 && !m_faultDone && m_opt.faultKind == "page" && static_cast<int64_t>(m_cycle) == m_opt.faultCycle)
            {
                const uint32_t at = m_opt.faultN ? m_opt.faultN % kPageBytes : 2048u;
                dst[at] ^= 0x01u;
                m_faultDone = true;
                std::printf("fault planted: cycle %" PRIu64 ", page 0x%x byte %u flipped\n", m_cycle, page, at);
            }
        }
        ++m_cyclePages;
        m_t.pagesApplied += count;
    }

    void Emu::hw(uint32_t address, uint32_t, uint64_t value, uint32_t)
    {
        if (!m_started || fatal())
            return;
        PS2Memory &mem = m_rt.memory();
        const uint32_t phys = address & 0x1FFFFFFFu;
        ++m_t.hwApplied;
        if (!m_faultDone && m_opt.faultKind == "hw" && static_cast<int64_t>(m_cycle) == m_opt.faultCycle && m_cycleHw == m_opt.faultN)
        {
            value ^= 1u;
            m_faultDone = true;
            std::printf("fault planted: cycle %" PRIu64 ", store %u to 0x%08x stored with bit 0 flipped\n", m_cycle, m_cycleHw, phys);
        }
        ++m_cycleHw;
        if (phys - 0x12000000u < 0x2000u)
        {
            // The recorded value is the register after the store: set it (CSR directly: its write-one-to-clear bits
            // would turn a stored value into another one).
            if (((phys - 0x12000000u) & ~7u) == 0x1000u)
                mem.gs().csr.store(value);
            else
                mem.write64(phys & ~7u, value); // the whole register, also after a 32-bit store to either half
            return;
        }
        // VIF1 and GIF registers: the stored 32-bit value, through the same path the game's store took.
        mem.write32(phys, static_cast<uint32_t>(value));
    }

    void Emu::gifPacket(int path, const uint8_t *data, uint32_t bytes)
    {
        if (!m_collect || !data)
            return;
        if (m_gifTap)
            m_gifTap(path, data, bytes);
        m_gif.update(data, bytes);
        if (m_path1)
            m_path1Bytes.insert(m_path1Bytes.end(), data, data + bytes);
    }

    void Emu::kick(const KickView &k, const RamView &)
    {
        if (!m_started || fatal())
            return;
        ++m_t.kicks;
        PS2Memory &mem = m_rt.memory();
        if (!m_faultDone && m_opt.faultKind == "kick" && static_cast<int64_t>(k.cycle) == m_opt.faultCycle && k.index == m_opt.faultN)
        {
            m_faultDone = true;
            std::printf("fault planted: %s (ch%u) dropped\n", where(k.cycle, k.index).c_str(), k.channel);
            return;
        }
        if (m_opt.verify && m_exact && k.regs)
        {
            // The register file before the kick: equal unless an earlier kick or store went another way.
            ++m_t.regsChecked;
            uint32_t first = ~0u, n = 0;
            for (uint32_t r = 0; r < kGsRegCount; ++r)
                if (k.regs[r] != ps2x::capture::g_gsRawRegs[r])
                {
                    if (first == ~0u)
                        first = r;
                    ++n;
                }
            if (n == 0)
                ++m_t.regsOk;
            else
                fail(where(k.cycle, k.index) + ": " + std::to_string(n) + " GS registers differ before the kick (first 0x" +
                     hex64(first).substr(14) + ": recorded " + hex64(k.regs[first]) + ", replay " + hex64(ps2x::capture::g_gsRawRegs[first]) + ")");
        }
        if (static_cast<int64_t>(k.cycle) == m_opt.beforeCycle && k.index == m_opt.beforeKick)
        {
            // The frame buffer the kick will draw into (FRAME_1), before it.
            const uint64_t frame = ps2x::capture::g_gsRawRegs[0x4C];
            const uint32_t fbp = static_cast<uint32_t>(frame & 0x1FFu), fbw = static_cast<uint32_t>((frame >> 16) & 0x3Fu);
            readPicture(mem.getGSVRAM(), kGsMemBytes, fbp, fbw ? fbw : 10u, m_header.width, m_header.height, m_picture.data());
            if (writePng(m_opt.beforeOut, m_picture.data(), m_header.width, m_header.height))
                std::printf("wrote %s (FBP 0x%x before %s)\n", m_opt.beforeOut.c_str(), fbp, where(k.cycle, k.index).c_str());
        }
        m_path1 = static_cast<int64_t>(k.cycle) == m_opt.path1Cycle && k.index == m_opt.path1Kick;
        m_path1Bytes.clear();

        if (m_substitute)
            if (const std::vector<uint8_t> *bytes = m_substitute(k))
            {
                // (step 7, check (b)) Another stream in place of the kick: PATH3 packets straight to the GS.
                const uint32_t savedCsr = _mm_getcsr();
                m_gif = Hash64{};
                m_collect = true;
                _mm_setcsr(k.mxcsr);
                if (!bytes->empty())
                    mem.submitGifPacket(GifPathId::Path3, bytes->data(), static_cast<uint32_t>(bytes->size()));
                _mm_setcsr(savedCsr);
                m_collect = false;
                ++m_t.substituted;
                m_t.substitutedBytes += bytes->size();
                m_t.gifBytes += m_gif.length();
                return;
            }
        // The kick as the game made it: the channel's registers, then the CHCR store. MXCSR and the VU0 FBRST the
        // VU1 start reads are the recorded ones; the DMAC is on (the recorder sees only kicks that passed its test).
        const uint32_t base = 0x10008000u + static_cast<uint32_t>(k.channel) * 0x1000u;
        mem.m_ioRegisters[0x1000E000u] |= 1u;
        mem.m_ioRegisters[base + 0x10u] = k.madr;
        mem.m_ioRegisters[base + 0x20u] = k.qwc;
        mem.m_ioRegisters[base + 0x30u] = k.tadr;
        mem.m_ioRegisters[base + 0x40u] = k.asr0;
        mem.m_ioRegisters[base + 0x50u] = k.asr1;
        m_rt.cpu().vu0_fbrst = k.fbrst;
        const uint32_t savedCsr = _mm_getcsr();
        m_gif = Hash64{};
        m_collect = true;
        _mm_setcsr(k.mxcsr);
        mem.write32(base, k.chcr);
        _mm_setcsr(savedCsr);
        m_collect = false;
        const uint64_t gifHash = m_gif.finish();
        const uint64_t gifBytes = m_gif.length();
        m_t.gifBytes += gifBytes;
        if (m_opt.kickHashes)
            std::printf("kick %s: GIF %" PRIu64 " bytes %s, GS memory %s\n", where(k.cycle, k.index).c_str(), gifBytes, hex64(gifHash).c_str(),
                        hex64(Hash64::of(mem.getGSVRAM(), kGsMemBytes)).c_str());
        if (m_path1)
        {
            if (std::FILE *f = std::fopen(m_opt.path1Out.c_str(), "wb"))
            {
                std::fwrite(m_path1Bytes.data(), 1, m_path1Bytes.size(), f);
                std::fclose(f);
                std::printf("wrote %s (%zu GIF bytes of %s)\n", m_opt.path1Out.c_str(), m_path1Bytes.size(), where(k.cycle, k.index).c_str());
            }
            m_path1 = false;
        }
        if (m_opt.verify && m_exact && k.hasGifHash)
        {
            ++m_t.gifChecked;
            if (gifHash == k.gifHash && gifBytes == k.gifBytes)
                ++m_t.gifOk;
            else
                fail(where(k.cycle, k.index) + " (ch" + std::to_string(k.channel) + "): GIF output " + std::to_string(gifBytes) + " bytes hash " +
                     hex64(gifHash) + ", recorded " + std::to_string(k.gifBytes) + " bytes hash " + hex64(k.gifHash));
        }
    }

    void Emu::comparePictures(const CycleInfo &info, const uint8_t *replayed, const uint8_t *ref, uint32_t w, uint32_t h)
    {
        uint64_t diff = 0;
        uint32_t fx = 0, fy = 0;
        std::vector<uint8_t> image(static_cast<size_t>(w) * h * 4u, 0u);
        for (uint32_t i = 0; i < w * h; ++i)
        {
            bool differs = false;
            for (int c = 0; c < 4; ++c)
            {
                const int d = std::abs(int(replayed[i * 4 + c]) - int(ref[i * 4 + c]));
                if (d)
                    differs = true;
                if (c < 3)
                    image[i * 4 + c] = static_cast<uint8_t>(std::min(255, d * 8 + (d ? 64 : 0)));
            }
            if (differs && diff++ == 0)
                fx = i % w, fy = i / w;
        }
        ++m_t.picturesChecked;
        if (diff == 0)
        {
            ++m_t.picturesOk;
            return;
        }
        fail("cycle " + std::to_string(info.end.cycle) + ": the replayed picture differs from the reference in " + std::to_string(diff) +
             " pixels (first at " + std::to_string(fx) + "," + std::to_string(fy) + ")");
        if (!m_opt.pngDir.empty())
        {
            const std::string stem = m_opt.pngDir + "/cycle" + std::to_string(info.end.cycle);
            writePng(stem + "_ref.png", ref, w, h);
            writePng(stem + "_diff.png", image.data(), w, h);
        }
    }

    void Emu::cycleEnd(const CycleInfo &info, const uint8_t *refRgba)
    {
        if (!m_started || fatal())
            return;
        ++m_t.cycles;
        const uint32_t w = info.end.refWidth ? info.end.refWidth : m_header.width;
        const uint32_t h = info.end.refHeight ? info.end.refHeight : m_header.height;
        m_picture.resize(static_cast<size_t>(w) * h * 4u);
        // The rectangle at the replayed machine's own DISPFB2 (the recorded closing store was applied as HW).
        const uint32_t fbp = static_cast<uint32_t>(m_rt.memory().gs().dispfb2 & 0x1FFu);
        readPicture(m_rt.memory().getGSVRAM(), kGsMemBytes, fbp, 10u, w, h, m_picture.data());
        const uint64_t hash = Hash64::of(m_picture.data(), m_picture.size());
        m_digest = (m_digest ^ hash) * 0x100000001B3ull;
        if (!m_opt.pngDir.empty() && info.end.cycle >= m_opt.pngFirst)
            writePng(m_opt.pngDir + "/cycle" + std::to_string(info.end.cycle) + ".png", m_picture.data(), w, h);
        if (!m_opt.verify || !m_exact)
            return;
        if (fbp != info.end.refFbp)
            fail("cycle " + std::to_string(info.end.cycle) + ": the replay shows FBP 0x" + hex64(fbp).substr(13) + ", the recording 0x" +
                 hex64(info.end.refFbp).substr(13));
        if (info.end.refWidth)
        {
            ++m_t.refHashChecked;
            if (hash == info.end.refHash)
                ++m_t.refHashOk;
            else if (!refRgba)
                fail("cycle " + std::to_string(info.end.cycle) + ": the replayed picture's hash " + hex64(hash) + " is not the reference's " +
                     hex64(info.end.refHash));
        }
        if (refRgba)
            comparePictures(info, m_picture.data(), refRgba, w, h);
    }

    void Emu::close() {}

    void Emu::summary() const
    {
        std::printf("emu: %" PRIu64 " cycles, %" PRIu64 " kicks replayed (%.1f MB of GIF output), %" PRIu64 " keyframes loaded, %" PRIu64
                    " stores and %" PRIu64 " pages applied; replay digest %016" PRIx64 "\n",
                    m_t.cycles, m_t.kicks, m_t.gifBytes / 1048576.0, m_t.keyframesLoaded, m_t.hwApplied, m_t.pagesApplied, m_digest);
        if (m_t.substituted)
            std::printf("emu: %" PRIu64 " kicks replaced by substitute streams (%" PRIu64 " bytes; their recorded GIF hashes not checked)\n",
                        m_t.substituted, m_t.substitutedBytes);
        if (m_opt.verify && m_exact)
            std::printf("verify: GIF output %" PRIu64 "/%" PRIu64 " kicks; register files %" PRIu64 "/%" PRIu64 "; picture hashes %" PRIu64 "/%" PRIu64
                        "; stored pictures %" PRIu64 "/%" PRIu64 " bit for bit; GS memory at keyframes %" PRIu64 "/%" PRIu64 ", final %" PRIu64 "/%" PRIu64 "\n",
                        m_t.gifOk, m_t.gifChecked, m_t.regsOk, m_t.regsChecked, m_t.refHashOk, m_t.refHashChecked, m_t.picturesOk, m_t.picturesChecked,
                        m_t.keyframesOk, m_t.keyframesChecked, m_t.finalOk, m_t.finalChecked);
    }
}

#include "chain_walk.h"

#include <algorithm>
#include <cstring>

namespace rc::rec
{
    namespace
    {
        constexpr uint32_t kScratchBase = 0x70000000u;

        struct Walker
        {
            const RamView &ram;
            WalkVisitor *visitor;
            WalkResult result;
            Hash64 hash;
            bool hashing = true;
            uint32_t offset = 0; // stream offset

            void checkPages(uint32_t phys, uint32_t bytes, bool scratch)
            {
                if (!ram.present || bytes == 0u)
                    return;
                const uint32_t first = RamView::pageOf(phys, scratch), last = RamView::pageOf(phys + bytes - 1u, scratch);
                for (uint32_t page = first; page <= last; ++page)
                    if (!ram.has(page))
                        ++result.absentPageReads;
            }

            const uint8_t *base(bool scratch) const { return scratch ? ram.scratchpad : ram.ram; }

            // A payload of `bytes` from guest address `address`, as appendData / processPendingTransfers read it.
            void payload(uint32_t address, uint64_t bytes64)
            {
                uint32_t src = 0;
                bool scratch = false;
                if (!translateDmaAddress(address, src, scratch))
                {
                    ++result.untranslatable;
                    return;
                }
                const uint32_t limit = scratch ? kScratchpadBytes : kRamBytes;
                uint32_t bytes = bytes64 > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
                while (bytes > 0u)
                {
                    if (src >= limit)
                        src = 0u;
                    const uint32_t chunk = std::min(bytes, limit - src);
                    checkPages(src, chunk, scratch);
                    const uint8_t *data = base(scratch) + src;
                    if (visitor)
                        visitor->range({offset, src, chunk, scratch, false, data});
                    if (hashing)
                        hash.update(data, chunk);
                    offset += chunk;
                    bytes -= chunk;
                    src += chunk;
                }
            }
        };
    }

    bool translateDmaAddress(uint32_t address, uint32_t &offset, bool &scratch)
    {
        scratch = false;
        if (address >= kScratchBase && address < kScratchBase + kScratchpadBytes)
        {
            scratch = true;
            offset = address - kScratchBase;
            return true;
        }
        if (address & 0x80000000u)
        {
            const uint32_t lower = address & 0x7FFFFFFFu;
            if (lower >= kScratchBase && lower < kScratchBase + kScratchpadBytes)
            {
                scratch = true;
                offset = lower - kScratchBase;
                return true;
            }
        }
        if (address - 0x20000000u < 0x20000000u) // the uncached RDRAM mirror
        {
            offset = address & (kRamBytes - 1u);
            return true;
        }
        if (address - 0x80000000u < 0x40000000u) // KSEG0/KSEG1
        {
            offset = address & 0x1FFFFFFFu;
            return true;
        }
        if (address < 0x80000000u)
        {
            offset = address;
            return true;
        }
        return false; // KSEG2/3: TLB-mapped
    }

    WalkResult walkChain(const KickView &kick, const RamView &ram, WalkVisitor *visitor, bool hash)
    {
        Walker w{ram, visitor, {}, {}, hash, 0u};
        const uint32_t chcr = kick.chcr;
        const uint32_t mode = (chcr >> 2) & 0x3u;
        if (mode != 1u)
        {
            if (kick.qwc > 0u)
                w.payload(kick.madr, static_cast<uint64_t>(kick.qwc) * 16ull);
        }
        else
        {
            const bool isVif = kick.channel == 0u || kick.channel == 1u;
            const bool transferTagData = isVif && (chcr & 0x40u) != 0u;
            const bool tie = (chcr & (1u << 7)) != 0u;
            uint32_t asp = (chcr >> 4) & 0x3u;
            uint32_t asr0 = kick.asr0, asr1 = kick.asr1;
            uint32_t tagAddr = kick.tadr;
            constexpr uint32_t kMaxChainTags = 1u << 20;
            uint32_t tags = 0;
            while (tags < kMaxChainTags)
            {
                uint32_t physTag = 0;
                bool tagScratch = false;
                if (!translateDmaAddress(tagAddr, physTag, tagScratch))
                    break; // the runtime catches the TLB miss and stops
                const uint32_t tagLimit = tagScratch ? kScratchpadBytes : kRamBytes;
                if (static_cast<uint64_t>(physTag) + 16u > tagLimit)
                    break;
                w.checkPages(physTag, 16u, tagScratch);
                const uint8_t *tp = w.base(tagScratch) + physTag;
                uint64_t tag;
                std::memcpy(&tag, tp, 8);
                const uint32_t tagQwc = static_cast<uint32_t>(tag & 0xFFFFu);
                const uint32_t id = static_cast<uint32_t>((tag >> 28) & 0x7u);
                const bool irq = ((tag >> 31) & 1u) != 0u;
                const uint32_t addr = static_cast<uint32_t>((tag >> 32) & 0x7FFFFFFFu);
                ++tags;
                ++w.result.tagIds[id];
                if (visitor)
                    visitor->range({w.offset, physTag, 16u, tagScratch, true, tp});

                uint32_t dataAddr = 0;
                bool endChain = false;
                switch (id)
                {
                case 0:
                    dataAddr = addr;
                    tagAddr = tagAddr + 16u;
                    endChain = true;
                    break;
                case 1:
                    dataAddr = tagAddr + 16u;
                    tagAddr = dataAddr + tagQwc * 16u;
                    break;
                case 2:
                    dataAddr = tagAddr + 16u;
                    tagAddr = addr;
                    break;
                case 3:
                case 4:
                    dataAddr = addr;
                    tagAddr = tagAddr + 16u;
                    break;
                case 5:
                    dataAddr = tagAddr + 16u;
                    {
                        const uint32_t retAddr = dataAddr + tagQwc * 16u;
                        if (asp == 0u)
                        {
                            asr0 = retAddr;
                            asp = 1u;
                        }
                        else if (asp == 1u)
                        {
                            asr1 = retAddr;
                            asp = 2u;
                        }
                    }
                    tagAddr = addr;
                    break;
                case 6:
                    dataAddr = tagAddr + 16u;
                    if (asp == 2u)
                    {
                        tagAddr = asr1;
                        asp = 1u;
                    }
                    else if (asp == 1u)
                    {
                        tagAddr = asr0;
                        asp = 0u;
                    }
                    else
                        endChain = true;
                    break;
                default: // 7
                    dataAddr = tagAddr + 16u;
                    endChain = true;
                    break;
                }
                if (transferTagData)
                {
                    if (w.hashing)
                        w.hash.update(tp + 8, 8u);
                    w.offset += 8u;
                }
                if (tagQwc > 0u)
                    w.payload(dataAddr, static_cast<uint64_t>(tagQwc) * 16ull);
                if (irq && tie)
                    endChain = true;
                if (endChain)
                    break;
            }
            w.result.dmaTags = tags;
            w.result.tagCap = tags >= kMaxChainTags;
        }
        w.result.streamBytes = w.hashing ? w.hash.length() : w.offset;
        w.result.streamHash = w.hashing ? w.hash.finish() : 0u;
        return w.result;
    }
}

// Decompiled from BB.c: loading a level's BBK file, \BB\<level / 10>\<level % 10>.BBK. A BBK is a 0x14ec-byte header
// (sBBHeader: a checksum, then the SRAM, VRAM, DRAM and handles headers) and up to four sections, each a size word and
// that many bytes of zlib data: the sound bank (sent to the sound chip's RAM), the GS memory image (uploaded as 64x64
// 32-bit tiles from 0x1e0000), the game data image (already at its absolute address, the animated textures list at its
// start) and the resource handles. The flag picks the sections: 1 no sound, 2 no textures, 8 no handles; the sound is
// read raw without 0x30, all of it with 0x10, only its ambient effects with 0x20 (the generic ones once only).
//
// Written from the instructions. The Europe version differs only in register use and store order (shared body).

#include "res_fuzz.h"

RC_VERSION_VARIANT(BBLoad)

namespace rc {

bool_ MemBlockAllocLo(ptr32<memHandle> pHandle, uint32 size); // mem.cpp
void MemBlockFree(ptr32<memHandle> pHandle);

namespace {

constexpr uint32_t kHeaderSize = 0x14ecu;
constexpr uint32_t kTextureStart = 0x1e0000u, kTextureEnd = 0x3fffffu; // the GS area the tiles may fill (bytes)

template <typename T>
T &mem(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

resClass &res() { return G(resVarG); }

// BBLoad's frame (0x1730 bytes): the file name at sp+0, the header at sp+0x100, a load image at sp+0x1600 and the
// section's sizes and buffer at sp+0x1660.
struct Frame
{
    const GuestFrame &frame;
    uint32_t name() const { return frame.local<uint8_t>(0).address; }
    uint32_t header() const { return frame.local<uint8_t>(0x100).address; }
    uint32_t loadImage() const { return frame.local<uint8_t>(0x1600).address; }
    uint32_t compressedSize() const { return frame.local<uint8_t>(0x1660).address; }
    uint32_t compressedData() const { return frame.local<uint8_t>(0x1664).address; }
    uint32_t uncompressedSize() const { return frame.local<uint8_t>(0x1668).address; }
    uint32_t field(uint32_t offset) const { return mem<uint32_t>(header() + offset); }
};

// A section: its size word, its bytes read into a buffer from the heap, inflated to dest() (read after the reads, as
// the header's size for it at sizeAt; dest may also note where it goes).
template <typename Dest>
void inflateSection(const Frame &f, int32_t fd, uint32_t sizeAt, Dest dest)
{
    guest_call(sym::FMFileRead, f.compressedSize(), 4, fd);
    mem<uint32_t>(f.compressedData()) = 0;
    MemBlockAllocLo(ptr32<memHandle>(f.compressedData()), mem<uint32_t>(f.compressedSize()));
    guest_call(sym::FMFileRead, mem<uint32_t>(f.compressedData()), mem<uint32_t>(f.compressedSize()), fd);
    const uint32_t size = f.field(sizeAt);
    const uint32_t to = dest();
    mem<uint32_t>(f.uncompressedSize()) = size;
    guest_call(sym::uncompress, to, f.uncompressedSize(), mem<uint32_t>(f.compressedData()), mem<uint32_t>(f.compressedSize()));
    MemBlockFree(ptr32<memHandle>(f.compressedData()));
}

// One 64x64 32-bit tile (0x4000 bytes) to the GS block `block`.
void uploadTile(const Frame &f, int32_t block, uint32_t from)
{
    guest_call(sym::sceGsSetDefLoadImage, f.loadImage(), block, 1, 0, 0, 0, 0x40, 0x40);
    guest_call(sym::FlushCache, 0);
    guest_call(sym::sceGsExecLoadImage, f.loadImage(), from);
    guest_call(sym::sceGsSyncPath, 0, 0);
}

int32_t gsBlock(uint32_t address) { return static_cast<int32_t>(address << 8) >> 16; }

// Effects first..last of the header's table into the sound globals (32 bytes each).
void copyEffects(uint32_t header, int32_t first, int32_t last)
{
    for (int32_t i = first; i <= last; ++i)
        std::memcpy(guest_host(sym::sfxVarG + 0xe8u + static_cast<uint32_t>(i) * 0x20u), guest_host(header + 0x38u + static_cast<uint32_t>(i) * 0x20u),
                    0x20);
}

} // namespace

void BBTransferSRAM(int32 flags, int32 dataSize, int32 dataDest, ptr32<sBBHeader> bbHeader, ptr32<void> pData);

// Sends a sound bank to the sound chip's RAM through an IOP buffer. With flags set, the bank is the generic effects
// followed by the level's: the part past the generic area's first 0x19000 bytes goes to the generic area's end, then the
// rest (the header's sizes) to 0x19000 before the generic area's end... as the header's addresses say.
void BBTransferSRAM(int32 flags, int32 dataSize, int32 dataDest, ptr32<sBBHeader> bbHeader, ptr32<void> pData)
{
    const uint32_t header = bbHeader.address;
    const auto send = [&](uint32_t from, uint32_t size, uint32_t dest, bool sayWhere)
    {
        guest_call(sym::FlushCache, 0);
        const uint32_t iop = guest_call<uint32_t>(sym::sceSifAllocIopHeap, size);
        if (sayWhere)
            guest_call(sym::printf, sym::sec_rodata + 0x268u, iop); // "Allocated IOP memory at : 0x%x\n"
        guest_call(sym::sceSdTransToIOP, from, iop, size, 1);
        guest_call(sym::printf, sym::sec_rodata + 0x288u, size, dest); // "Sending 0x%x bytes to SPU RAM at..."
        guest_call(sym::sceSdRemote, 1, 0x80d0, 1, 0, iop, dest, size); // the transfer
        guest_call(sym::sceSdRemote, 1, 0x80f0, 1, 1);                  // and its wait ($t0-$t2 left as the first call left them)
        return iop;
    };
    if (flags == 0)
    {
        uint32_t iop;
        {
            GuestFrame frame(0x80);
            iop = send(pData.address, static_cast<uint32_t>(dataSize), static_cast<uint32_t>(dataDest), true);
        }
        guest_call(sym::sceSifFreeIopHeap, iop); // a tail jump
        return;
    }
    uint32_t iop;
    {
        GuestFrame frame(0x80);
        constexpr uint32_t kGenericHead = 0x19000u;
        const uint32_t size = mem<uint32_t>(header + 0x2cu) - (mem<uint32_t>(header + 0x28u) + kGenericHead);
        iop = send(pData.address, size, static_cast<uint32_t>(dataDest), true);
        guest_call(sym::sceSifFreeIopHeap, iop);
        const uint32_t rest = mem<uint32_t>(header + 0x24u) - size + kGenericHead;
        const uint32_t restDest = mem<uint32_t>(header + 0x2cu) - kGenericHead;
        guest_call(sym::FlushCache, 0);
        iop = guest_call<uint32_t>(sym::sceSifAllocIopHeap, rest);
        const uint32_t from = pData.address + (mem<uint32_t>(header + 0x2cu) - mem<uint32_t>(header + 0x28u)) - kGenericHead;
        guest_call(sym::sceSdTransToIOP, from, iop, rest, 1);
        guest_call(sym::printf, sym::sec_rodata + 0x288u, rest, restDest);
        guest_call(sym::sceSdRemote, 1, 0x80d0, 1, 0, iop, restDest, rest);
        guest_call(sym::sceSdRemote, 1, 0x80f0, 1, 1);
    }
    guest_call(sym::sceSifFreeIopHeap, iop); // a tail jump
}

ptr32<char> BBLoad(uint32 level, uint32 flag)
{
    GuestFrame frame(0x1730);
    const Frame f{frame};
    guest_call(sym::sprintf, f.name(), sym::sec_rodata + 0x258u, level / 10u, level % 10u); // "\BB\%d\%d.BBK"
    const int32_t fd = guest_call<int32_t>(sym::FMFileOpen, f.name());
    if (fd == -1)
        return ptr32<char>(0xffffffffu);
    const uint32_t header = f.header();
    // The header, read again from the start until it comes whole; after six tries the load error function runs.
    for (int32_t tries = 0;;)
    {
        const uint32_t got = guest_call<uint32_t>(sym::FMFileRead, header, kHeaderSize, fd);
        guest_call(sym::ResBinaryCrc, 0, header + 4u, kHeaderSize - 4u);
        ++tries;
        if (got == kHeaderSize)
            break;
        if (tries >= 6 && res().pfLoadErrorFunction.address != 0)
        {
            tries = 0;
            guest_call(res().pfLoadErrorFunction.address, fd, 0);
        }
        guest_call(sym::FMFileSeek, fd, 0, 0);
    }
    if ((flag & 1u) == 0)
    {
        if ((flag & 0x30u) == 0)
        {
            // The sound bank as it is, uncompressed, to the resource area's top.
            guest_call(sym::FMFileRead, f.compressedSize(), 4, fd);
            guest_call(sym::FMFileRead, res().pTop.address, mem<uint32_t>(f.compressedSize()), fd);
        }
        else
        {
            const uint32_t genericSize = f.field(0x2cu) - f.field(0x28u); // endOfGenericAddr - startOfGenericAddr
            inflateSection(f, fd, 0x24u, [] { return res().pTop.address; });
            uint32_t bank = res().pTop.address, size = f.field(0x24u);
            const bool ambientOnly = (flag & 0x20u) != 0 && G(genericLoaded) != 0;
            if (ambientOnly)
            {
                bank = res().pTop.address + genericSize;
                size = f.field(0x30u) - f.field(0x2cu);
            }
            sfxClass &sfx = G(sfxVarG);
            const uint32_t endOfGeneric = f.field(0x2cu), startOfGeneric = f.field(0x28u);
            sfx.endOfAmbientAddr = static_cast<int32>(f.field(0x30u));
            sfx.startOfGenericAddr = static_cast<int32>(startOfGeneric);
            sfx.endOfGenericAddr = static_cast<int32>(endOfGeneric);
            sfx.firstAmbientIndex = mem<int16>(header + 0x34u);
            sfx.lastAmbientIndex = mem<int16>(header + 0x36u);
            if ((flag & 0x10u) != 0 || ((flag & 0x20u) != 0 && G(genericLoaded) != 0))
            {
                // The level's ambient effects, and their part of the bank after the generic area.
                copyEffects(header, mem<int16>(header + 0x34u), mem<int16>(header + 0x36u));
                BBTransferSRAM(0, static_cast<int32>(size), static_cast<int32>(endOfGeneric), ptr32<sBBHeader>(header), ptr32<void>(bank));
            }
            else if ((flag & 0x20u) != 0)
            {
                // The front end's first load: every effect, and the whole bank in two parts.
                copyEffects(header, 0, 0x8b);
                G(genericLoaded) = 1;
                BBTransferSRAM(1, static_cast<int32>(f.field(0x24u)), static_cast<int32>(startOfGeneric), ptr32<sBBHeader>(header),
                               ptr32<void>(res().pTop.address));
            }
        }
    }
    if ((flag & 2u) == 0)
    {
        // The GS memory image, inflated to the resource area's top and uploaded tile by tile from 0x1e0000. Tiles whose
        // address is past the GS area are skipped. An odd number of 0x4000-byte tiles starts with one on its own.
        uint32_t gs = kTextureStart;
        inflateSection(f, fd, 0x11c0u, [] { return res().pTop.address; });
        uint32_t remaining = f.field(0x11c0u);
        uint32_t from = res().pTop.address;
        if (remaining != 0)
        {
            const uint32_t odd = (0u - remaining) & 0x7fffu;
            bool pairs = true;
            if (odd != 0 && odd >= 0x4000u)
            {
                uploadTile(f, gsBlock(kTextureStart), from);
                gs = kTextureStart | 0x4000u;
                remaining -= 0x4000u;
                from += 0x4000u;
                pairs = remaining != 0;
            }
            while (pairs)
            {
                if (!(kTextureEnd < gs))
                    uploadTile(f, gsBlock(gs), from);
                if (!(kTextureEnd < gs + 0x4000u))
                    uploadTile(f, gsBlock(gs + 0x4000u), from + 0x4000u);
                remaining -= 0x8000u;
                from += 0x8000u;
                gs += 0x8000u;
                pairs = remaining != 0;
            }
        }
        G(nextTexAddr) = gs;
    }
    // The game data, at the absolute address its header gives; it starts with the animated textures' count and list.
    inflateSection(f, fd, 0x14d4u, [&f]
    {
        res().pCurr.address = f.field(0x14dcu);
        return f.field(0x14dcu);
    });
    const uint32_t data = res().pCurr.address;
    G(gNumAnimTextures) = mem<uint32_t>(data);
    std::memcpy(guest_host(sym::gpAnimTextures), guest_host(data + 4u), 0x100);
    if ((flag & 8u) == 0)
        inflateSection(f, fd, 0x14e8u, [] { return res().handlesHandle.address; });
    guest_call(sym::FMFileClose, fd);
    return ptr32<char>(res().pBot.address);
}

} // namespace rc

// Disc reads, the IOP and GS uploads: offline only, with the disc served from the image and the IOP and GS calls logged
// (res_fuzz.cpp); whole RAM (the sections land on the heap and at their own addresses).
RC_DECOMP_FN_BLOCKING_SETUP_HEAP(BBLoad, rc::fuzz::bbLoad)
RC_DECOMP_FN_BLOCKING_SETUP(BBTransferSRAM, rc::fuzz::bbTransfer)

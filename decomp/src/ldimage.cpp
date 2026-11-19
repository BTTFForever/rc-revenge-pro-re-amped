// Decompiled from LDIMAGE.c: the DMA chain that uploads an image to GS memory in 16x16 tiles (each tile its own
// TRXPOS and IMAGE transfer, the last with end of packet), and the register packers it is built from. The movie
// player builds its frames' uploads with it and starts them on the GIF channel with loadImage.

#include "display_fuzz.h"

namespace rc {

namespace {

// DMA channel 2 (GIF).
constexpr uint32_t kD2Chcr = 0x1000a000u, kD2Qwc = 0x1000a020u, kD2Tadr = 0x1000a030u;

template <typename T>
T &mem(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

} // namespace

// Starts the GIF channel on a chain of tags.
void loadImage(ptr32<u_int> tags)
{
    // Side tag: a movie picture's tiles.
    const rc::gfx::SpanKicks picture(rc::gfx::KickLabel{rc::gfx::KickMovie, tags.address, 0u, 0u});
    hw_write32(kD2Tadr, tags.address & 0x0fffffffu);
    hw_write32(kD2Qwc, 0);
    hw_write32(kD2Chcr, 0x105);
}

void setGIFtag(ptr32<u_int> p, u_long regs, u_int nreg, u_int flg, u_int prim, u_int pre, u_int eop, u_int nloop)
{
    const uint32_t at = p.address;
    mem<uint32_t>(at + 0x0u) = (eop << 15) | nloop;
    mem<uint32_t>(at + 0xcu) = static_cast<uint32_t>(regs >> 32);
    mem<uint32_t>(at + 0x4u) = (pre << 14) | (nreg << 28) | (flg << 26) | (prim << 15);
    mem<uint32_t>(at + 0x8u) = static_cast<uint32_t>(regs);
}

void setBITBLTBUF(ptr32<u_int> p, u_int dbp, u_int dbw, u_int dpsm)
{
    const uint32_t at = p.address;
    mem<uint32_t>(at + 0x8u) = 0x50;
    mem<uint32_t>(at + 0x4u) = ((dpsm & 0xffu) << 24) | ((dbw & 0xffffu) << 16) | dbp;
    mem<uint32_t>(at + 0xcu) = 0;
    mem<uint32_t>(at + 0x0u) = 0;
}

void setTRXPOS(ptr32<u_int> p, u_int dir, u_int dsax, u_int dsay)
{
    const uint32_t at = p.address;
    mem<uint32_t>(at + 0x8u) = 0x51;
    mem<uint32_t>(at + 0x4u) = ((dir & 0x1fu) << 27) | ((dsay & 0xffffu) << 16) | dsax;
    mem<uint32_t>(at + 0xcu) = 0;
    mem<uint32_t>(at + 0x0u) = 0;
}

void setTRXREG(ptr32<u_int> p, u_int rrw, u_int rrh)
{
    const uint32_t at = p.address;
    mem<uint32_t>(at + 0x8u) = 0x52;
    mem<uint32_t>(at + 0x0u) = rrw;
    mem<uint32_t>(at + 0x4u) = rrh;
    mem<uint32_t>(at + 0xcu) = 0;
}

void setTRXDIR(ptr32<u_int> p, u_int xdir)
{
    const uint32_t at = p.address;
    mem<uint32_t>(at + 0x8u) = 0x53;
    mem<uint32_t>(at + 0xcu) = 0;
    mem<uint32_t>(at + 0x0u) = xdir;
    mem<uint32_t>(at + 0x4u) = 0;
}

// A source chain tag, as one 64-bit store.
void setDMAscTag(ptr32<u_int> p, u_int spr, ptr32<void> addr, u_int irq, u_int id, u_int pce, u_int qwc)
{
    mem<uint64_t>(p.address) = (static_cast<uint64_t>(spr) << 63) | qwc | (static_cast<uint64_t>(id) << 28) |
                                (static_cast<uint64_t>(irq) << 31) | (static_cast<uint64_t>(addr.address & ~0xfu) << 32) |
                                (static_cast<uint64_t>(pce) << 26);
}

// The chain, written through the uncached mirror: a header (BITBLTBUF to buffer 0 width 10, TRXREG 16x16), then per
// tile, column by column, a CNT tag with TRXPOS and TRXDIR, an IMAGE GIF tag and a REF tag to the tile's 1 KB of
// pixels (skip bytes apart), and a closing END tag.
void setLoadImageTagsYX(ptr32<u_int> tags, ptr32<void> image, int32_t skip, int32_t x, int32_t y, int32_t width,
                        int32_t height)
{
    const int32_t columns = width >> 4, rows = height >> 4;
    uint32_t tag = (tags.address & 0x0fffffffu) | 0x20000000u;
    uint32_t pixels = image.address;
    setDMAscTag(ptr32<u_int>(tag), 0, nullptr, 0, 1, 0, 3);
    tag += 0x10u;
    setGIFtag(ptr32<u_int>(tag), 0xe, 1, 0, 0, 0, 0, 2);
    tag += 0x10u;
    setBITBLTBUF(ptr32<u_int>(tag), 0, 10, 0);
    tag += 0x10u;
    setTRXREG(ptr32<u_int>(tag), 0x10, 0x10);
    tag += 0x10u;
    uint32_t data = tag + 0x10u;
    for (int32_t column = 0; column < columns; ++column)
    {
        for (int32_t row = 0; row < rows; ++row)
        {
            const u_int eop = row == rows - 1 && column == columns - 1 ? 1u : 0u;
            setDMAscTag(ptr32<u_int>(tag), 0, nullptr, 0, 1, 0, 4);
            tag = data + 0x50u;
            setGIFtag(ptr32<u_int>(data), 0xe, 1, 0, 0, 0, 0, 2);
            setTRXPOS(ptr32<u_int>(data + 0x10u), 0, static_cast<u_int>(x) + static_cast<u_int>(column) * 16u,
                      static_cast<u_int>(y) + static_cast<u_int>(row) * 16u);
            setTRXDIR(ptr32<u_int>(data + 0x20u), 0);
            setGIFtag(ptr32<u_int>(data + 0x30u), 0, 0, 2, 0, 0, eop, 0x40);
            const uint32_t source = pixels & 0x0fffffffu;
            pixels += static_cast<uint32_t>(skip);
            setDMAscTag(ptr32<u_int>(data + 0x40u), 0, ptr32<void>(source), 0, 3, 0, 0x40);
            data += 0x60u;
        }
    }
    setDMAscTag(ptr32<u_int>(tag), 0, nullptr, 0, 7, 0, 0);
}

void setLoadImageTags(ptr32<u_int> tags, ptr32<void> image, int32_t x, int32_t y, int32_t width, int32_t height)
{
    setLoadImageTagsYX(tags, image, 0x400, x, y, width, height);
}

void setLoadImageTagsTile(ptr32<u_int> tags, ptr32<void> image, int32_t x, int32_t y, int32_t width, int32_t height)
{
    setLoadImageTagsYX(tags, image, 0, x, y, width, height);
}

} // namespace rc

RC_DECOMP_FN_SETUP(setLoadImageTags, rc::fuzz::imageTags)
RC_DECOMP_FN_SETUP(setLoadImageTagsTile, rc::fuzz::imageTags)
RC_DECOMP_FN_SETUP(setLoadImageTagsYX, rc::fuzz::imageTagsYX)
RC_DECOMP_FN_BLOCKING_SETUP(loadImage, rc::fuzz::hardwareArg)
RC_DECOMP_FN_SETUP(setGIFtag, rc::fuzz::packerArgs)
RC_DECOMP_FN_SETUP(setBITBLTBUF, rc::fuzz::packerArgs)
RC_DECOMP_FN_SETUP(setTRXPOS, rc::fuzz::packerArgs)
RC_DECOMP_FN_SETUP(setTRXREG, rc::fuzz::packerArgs)
RC_DECOMP_FN_SETUP(setTRXDIR, rc::fuzz::packerArgs)
RC_DECOMP_FN_SETUP(setDMAscTag, rc::fuzz::packerArgs)

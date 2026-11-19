// The game's half of the side-tag channel (render/gfx/tags.h, RENDERER_FRAME_RECORD.md section 5): how to read the two
// buffer anchors from guest RAM. Reads only. Without RC_GFX_TAGS (the oracle) this file is empty.
#include "rc_decomp.h"

#if RC_GFX_TAGS
namespace rc {
namespace {

// The 2D packet's write position: gGfxP2.pDmaBuff + buffIdx quadwords.
uint32_t anchor2D()
{
    const sGfxP2 &gfx = G(gGfxP2);
    return gfx.pDmaBuff.address + static_cast<uint32_t>(gfx.buffIdx) * 16u;
}

// The next slot of the frame's DMA chain: &gRdr.pDmaTag[currDmaTag].
uint32_t anchorChain()
{
    const sRdr &rdr = G(gRdr);
    return rdr.pDmaTag.address + static_cast<uint32_t>(rdr.currDmaTag) * 16u;
}

const bool g_registered = []
{
    rc::gfx::detail::setAnchorReader(rc::gfx::AnchorKind::Buffer2D, anchor2D);
    rc::gfx::detail::setAnchorReader(rc::gfx::AnchorKind::ChainSlot, anchorChain);
    return true;
}();

} // namespace
} // namespace rc
#endif

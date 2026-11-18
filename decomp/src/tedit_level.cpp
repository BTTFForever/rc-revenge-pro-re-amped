// Decompiled from TEDIT.c: a built track turned into a race level (level 0x61, loaded with the track's theme): the
// collision from the modules' collision pieces, the AI nodes from their node sets, the start grid and pickups moved
// onto the track, the tiles as the level's landscape, the hud's map and the memory card's icon. See tedit.h.

#include "tedit.h"
#include "tedit_fuzz.h"

namespace rc {

using namespace tedit;

namespace {

R5900Context &context() { return g_runtime->eeScheduler().runningContext(); }

// The collision piece of an editModuleListG entry: moduleToColListG's byte for its module (searched by the id's low
// byte; a module not in the 42 entries reads the one past them): the piece's number, bit 7 mirrored.
uint32_t collisionPiece(uint32_t desc)
{
    const uint32_t module = at<uint8_t>(desc);
    uint32_t i = 0;
    while (i < 0x2au && at<uint16_t>(sym::moduleToColListG + i * 8u) != module)
        ++i;
    return at<uint8_t>(sym::moduleToColListG + i * 8u + 2u);
}

uint32_t countModules()
{
    uint32_t count = 0;
    for (uint32_t node = te().trackDesc.pTrackModuleList.address; node != 0; node = at<uint32_t>(node))
        ++count;
    return count & 0xffffu;
}

// The track's start into the locals of a frame.
void startAt(uint32_t x, uint32_t y, uint32_t elevation, uint32_t direction)
{
    const sTEditClass &t = te();
    at<int32_t>(direction) = t.trackDesc.startOrientation;
    at<int16_t>(elevation) = t.trackDesc.startElevation;
    at<int16_t>(x) = t.trackDesc.startXPos;
    at<int16_t>(y) = t.trackDesc.startYPos;
}

eSlope adjust(uint32_t module, uint32_t x, uint32_t y, uint32_t direction, uint32_t elevation, uint8_t flag)
{
    return TEditAdjustCursorModule(ptr32<_sCursorModule>(module), ptr32<int16>(x), ptr32<int16>(y), ptr32<eDirection>(direction),
                                   ptr32<int16>(elevation), flag);
}

int16_t coordinate(int32_t unit) { return static_cast<int16_t>((unit - 8) << 11); }

// One AI node set copied to its place (TEditConstructAINode); the 7 arguments as the original takes them.
void constructAINode(uint32_t aiModId, uint32_t destination, uint32_t ppLast, uint32_t offset, uint32_t mirror, uint32_t rotated, uint32_t direction);

} // namespace

// The collision builder's arrays for the track: per module its collision piece, where it goes (the unit's middle, at
// the module's height: a mirrored piece's from its far end), how it is turned and whether it is mirrored.
void TEditGeneratePreTColData(ptr32<uint16> pModIds, ptr32<void> pOff, ptr32<uint16> pRFlags, ptr32<uint16> pMFlags)
{
    GuestFrame frame(0xc0);
    const uint32_t sp = frame.local<uint8_t>(0).address; // cursorModule 0x0, x 0x10, y 0x12, elevation 0x14, direction 0x18
    const sTEditClass &t = te();
    uint32_t ids = pModIds.address, off = pOff.address, turns = pRFlags.address, mirrors = pMFlags.address;
    uint32_t node = t.trackDesc.pTrackModuleList.address;
    startAt(sp + 0x10u, sp + 0x12u, sp + 0x14u, sp + 0x18u);
    for (; node != 0; node = at<uint32_t>(node), ids += 2u, off += 8u, turns += 2u, mirrors += 2u)
    {
        const uint32_t desc = editModuleDesc(at<uint32_t>(node + 4u));
        at<uint32_t>(sp + 4u) = desc;
        const uint32_t x = at<uint16_t>(sp + 0x10u), y = at<uint16_t>(sp + 0x12u), elevation = at<uint16_t>(sp + 0x14u);
        uint32_t direction = at<uint32_t>(sp + 0x18u);
        const uint32_t piece = collisionPiece(desc);
        at<uint16_t>(ids) = static_cast<uint16_t>(piece & 0x7fu);
        at<uint16_t>(mirrors) = static_cast<uint16_t>((piece & 0x80u) != 0 ? 1 : 0);
        at<int16_t>(off) = static_cast<int16_t>(coordinate(static_cast<int16_t>(x)) + 0x400);
        at<int16_t>(off + 4u) = static_cast<int16_t>(coordinate(static_cast<int16_t>(y)) + 0x400);
        at<uint16_t>(off + 2u) = static_cast<uint16_t>(elevation);
        if ((at<uint16_t>(desc + 2u) & 1u) != 0)
        {
            const uint32_t module = editModule(at<uint16_t>(desc));
            const int32_t rise = static_cast<int32_t>(0u - (static_cast<uint32_t>(static_cast<int32_t>(at<int16_t>(module + 6u))) << 24)) >> 16;
            at<uint16_t>(off + 2u) = static_cast<uint16_t>(static_cast<uint32_t>(rise) + elevation);
            if (direction <= 3u)
                direction ^= 2u;
        }
        at<uint16_t>(turns) = static_cast<uint16_t>(direction);
        adjust(sp, sp + 0x10u, sp + 0x12u, sp + 0x18u, sp + 0x14u, 0);
    }
}

namespace {

void constructAINode(uint32_t aiModId, uint32_t destination, uint32_t ppLast, uint32_t offset, uint32_t mirror, uint32_t rotated, uint32_t direction)
{
    GuestFrame frame(0xc0);
    const uint32_t sp = frame.local<uint8_t>(0).address; // rotStart 0x0, the swap's vector 0x10
    const uint32_t set = te().pAINode.address + 4u + (aiModId & 0xffffu) * 0x288u;
    const uint32_t origin = set + 0x18u;
    guest_call(sym::MemBlockCopy32, destination, set + 0x6cu, 0x21c);
    const uint32_t before = at<uint32_t>(ppLast);
    const auto h = [](uint32_t address) -> uint16_t & { return at<uint16_t>(address); };
    if ((rotated & 0xffffu) != 0)
    {
        // The set run the other way up: the heights of its ends and of the nodes beside them change places.
        const uint32_t first = destination, last = destination + 0x1b0u;
        h(offset + 2u) = static_cast<uint16_t>(h(offset + 2u) - (at<int16_t>(last + 0x2au) - at<int16_t>(first + 0x2au)));
        const auto swapHeights = [&](uint32_t a, uint32_t b)
        {
            for (const uint32_t field : {0x2au, 0x32u, 0x1au, 0x22u})
                std::swap(h(a + field), h(b + field));
        };
        swapHeights(first, last);
        swapHeights(destination + 0x6cu, destination + 0x144u);
    }
    uint32_t previous = 0, node = destination;
    for (uint32_t i = 0; i < 5u; ++i, node += 0x6cu)
    {
        static constexpr uint32_t kVectors[4] = {0x18u, 0x20u, 0x28u, 0x30u}; // node[0], node[1], centre, overCentre
        if ((mirror & 0xffffu) != 0)
        {
            h(node + 0x18u) = static_cast<uint16_t>(0x800u - (h(node + 0x18u) - h(origin)));
            h(node + 0x20u) = static_cast<uint16_t>(0x800u - (h(node + 0x20u) - h(origin)));
            at<uint64_t>(sp + 0x10u) = at<uint64_t>(node + 0x18u);
            at<uint64_t>(node + 0x18u) = at<uint64_t>(node + 0x20u);
            at<uint64_t>(node + 0x20u) = at<uint64_t>(sp + 0x10u);
            h(node + 0x28u) = static_cast<uint16_t>(0x800u - (h(node + 0x28u) - h(origin)));
            h(node + 0x30u) = static_cast<uint16_t>(0x800u - (h(node + 0x30u) - h(origin)));
        }
        else
            for (const uint32_t v : kVectors)
                h(node + v) = static_cast<uint16_t>(h(node + v) - h(origin));
        for (const uint32_t v : kVectors)
        {
            h(node + v + 2u) = static_cast<uint16_t>(h(node + v + 2u) - h(origin + 2u));
            h(node + v + 4u) = static_cast<uint16_t>(h(node + v + 4u) - h(origin + 4u));
        }
        // Turned to the module's direction about the unit's corner; another direction leaves the last turn's start (or
        // what the stack held).
        switch (direction)
        {
        case 0:
            h(sp) = h(offset);
            h(sp + 2u) = h(offset + 2u);
            h(sp + 4u) = h(offset + 4u);
            break;
        case 1:
            for (const uint32_t v : kVectors)
            {
                const uint16_t x = h(node + v);
                h(node + v) = h(node + v + 4u);
                h(node + v + 4u) = static_cast<uint16_t>(0u - x);
            }
            h(sp) = h(offset);
            h(sp + 2u) = h(offset + 2u);
            h(sp + 4u) = static_cast<uint16_t>(h(offset + 4u) + 0x800u);
            break;
        case 2:
            for (const uint32_t v : kVectors)
            {
                h(node + v) = static_cast<uint16_t>(0u - h(node + v));
                h(node + v + 4u) = static_cast<uint16_t>(0u - h(node + v + 4u));
            }
            h(sp) = static_cast<uint16_t>(h(offset) + 0x800u);
            h(sp + 2u) = h(offset + 2u);
            h(sp + 4u) = static_cast<uint16_t>(h(offset + 4u) + 0x800u);
            break;
        case 3:
            for (const uint32_t v : kVectors)
            {
                const uint16_t x = h(node + v);
                h(node + v) = static_cast<uint16_t>(0u - h(node + v + 4u));
                h(node + v + 4u) = x;
            }
            h(sp) = static_cast<uint16_t>(h(offset) + 0x800u);
            h(sp + 2u) = h(offset + 2u);
            h(sp + 4u) = h(offset + 4u);
            break;
        default:
            break;
        }
        const uint16_t dx = h(sp), dy = h(sp + 2u), dz = h(sp + 4u);
        for (const uint32_t v : kVectors)
        {
            h(node + v) = static_cast<uint16_t>(h(node + v) + dx);
            h(node + v + 2u) = static_cast<uint16_t>(h(node + v + 2u) + dy);
            h(node + v + 4u) = static_cast<uint16_t>(h(node + v + 4u) + dz);
        }
        at<uint32_t>(node + 0x14u) = 0;
        at<uint32_t>(node + 0xcu) = 0;
        if (previous != 0)
        {
            at<uint32_t>(node + 8u) = previous;
            at<uint32_t>(previous + 0x10u) = node;
        }
        else
            at<uint32_t>(node + 8u) = before;
        previous = node;
    }
    at<uint32_t>(ppLast) = previous;
}

} // namespace

// A module's five AI nodes from its node set (pAINode: sets of 0x288 bytes, an origin at 0x18, the nodes from 0x6c):
// taken from the set's origin, mirrored across the unit or run backwards, turned to the direction, moved to `offset`
// and linked on after *ppAILastNode.
void TEditConstructAINode(uint16 aiModId, ptr32<_sAINode> pAINodeDstAddr, ptr32<ptr32<_sAINode>> ppAILastNode, ptr32<void> offset,
                          uint16 aiModMirror, uint16 aiModRotated, eDirection direction)
{
    constructAINode(aiModId, pAINodeDstAddr.address, ppAILastNode.address, offset.address, aiModMirror, aiModRotated, static_cast<uint32_t>(direction));
}

// The level's AI nodes: five a module, in a ring.
void TEditGenerateTrackAINodes()
{
    GuestFrame frame(0xe0);
    const uint32_t sp = frame.local<uint8_t>(0).address; // cursorModule 0x0, offset 0x10, x 0x20, y 0x22, elevation 0x24,
                                                         // pAINode 0x28, pAILastNode 0x2c, direction 0x30
    sTEditClass &t = te();
    startAt(sp + 0x20u, sp + 0x22u, sp + 0x24u, sp + 0x30u);
    at<uint32_t>(sp + 0x2cu) = 0;
    at<uint32_t>(sp + 0x28u) = 0;
    const uint32_t nodes = (countModules() * 5u) & 0xffffu;
    guest_call<int32_t>(sym::MemBlockAllocLo, sp + 0x28u, nodes * 0x6cu);
    sAINodeClass &ai = G(aiNodeVarG);
    uint32_t node = at<uint32_t>(sp + 0x28u);
    ai.noNodesSingle = static_cast<int16_t>(nodes);
    ai.pNode = ptr32<_sAINode>(node);
    for (uint32_t module = t.trackDesc.pTrackModuleList.address; module != 0; module = at<uint32_t>(module), node += 0x21cu)
    {
        const uint32_t desc = editModuleDesc(at<uint32_t>(module + 4u));
        at<uint32_t>(sp + 4u) = desc;
        const uint32_t x = at<uint16_t>(sp + 0x20u), y = at<uint16_t>(sp + 0x22u), elevation = at<uint16_t>(sp + 0x24u);
        const uint32_t piece = collisionPiece(desc);
        uint32_t mirror = piece >> 7, rotated = 0;
        if ((at<uint16_t>(desc + 2u) & 1u) != 0)
        {
            // A mirrored entry: the slopes (modules below 0x1c) run backwards, the turns are mirrored.
            if (at<uint16_t>(desc) < 0x1cu)
                rotated = 1;
            else
                mirror = 1;
        }
        at<int16_t>(sp + 0x10u) = coordinate(static_cast<int16_t>(x));
        at<uint16_t>(sp + 0x12u) = static_cast<uint16_t>(elevation);
        at<int16_t>(sp + 0x14u) = coordinate(static_cast<int16_t>(y));
        constructAINode(piece & 0x7fu, node, sp + 0x2cu, sp + 0x10u, mirror, rotated, at<uint32_t>(sp + 0x30u));
        at<uint32_t>(at<uint32_t>(sp + 0x2cu) + 0x10u) = node + 0x21cu;
        adjust(sp, sp + 0x20u, sp + 0x22u, sp + 0x30u, sp + 0x24u, 0);
    }
    const uint32_t first = ai.pNode.address, last = at<uint32_t>(sp + 0x2cu);
    const uint32_t startNode = at<uint32_t>(first + 0x1b0u);
    at<uint32_t>(last + 0x10u) = first;
    ai.startNode = static_cast<int32_t>(startNode);
    at<uint32_t>(first + 8u) = last;
    guest_call(sym::AINodeCreateLinkInfo);
    TEditGenerateTrackAINodesFinishDist();
}

// The race level's collision: the track again from the permanent save buffer, then the collision builder on its pieces.
void TEditGenerateTCol()
{
    GuestFrame frame(0xe0);
    const uint32_t sp = frame.local<uint8_t>(0).address; // min 0x0, max 0x10, pTrackModule 0x20, pModIds 0x24, pOff 0x28,
                                                         // pRFlags 0x2c, pMFlags 0x30
    sTEditClass &t = te();
    at<uint32_t>(sp + 0x24u) = 0;
    at<uint32_t>(sp + 0x28u) = 0;
    at<uint32_t>(sp + 0x2cu) = 0;
    at<uint32_t>(sp + 0x30u) = 0;
    uint32_t in = t.pSavedTrackData.address;
    std::memcpy(guest_host(kTE + 0x20u), guest_host(in), 0x18u);
    in += 0x18u;
    t.trackDesc.pTrackModuleList = nullptr;
    uint32_t id = at<uint32_t>(in);
    do
    {
        at<uint32_t>(sp + 0x20u) = 0;
        guest_call<int32_t>(sym::MemBlockAllocLo, sp + 0x20u, 8);
        const uint32_t node = at<uint32_t>(sp + 0x20u);
        uint32_t last = t.trackDesc.pTrackModuleList.address;
        at<uint32_t>(node + 4u) = id;
        at<uint32_t>(node) = 0;
        if (last == 0)
            t.trackDesc.pTrackModuleList = ptr32<_sTrackModule>(node);
        else
        {
            while (at<uint32_t>(last) != 0)
                last = at<uint32_t>(last);
            at<uint32_t>(last) = node;
        }
        in += 4u;
        id = at<uint32_t>(in);
    } while (id != 0xffffffffu);
    const uint32_t modules = countModules();
    guest_call<int32_t>(sym::MemBlockAllocLo, sp + 0x24u, modules << 1);
    guest_call<int32_t>(sym::MemBlockAllocLo, sp + 0x28u, modules << 3);
    guest_call<int32_t>(sym::MemBlockAllocLo, sp + 0x2cu, modules << 1);
    guest_call<int32_t>(sym::MemBlockAllocLo, sp + 0x30u, modules << 1);
    TEditGeneratePreTColData(ptr32<uint16>(at<uint32_t>(sp + 0x24u)), ptr32<void>(at<uint32_t>(sp + 0x28u)), ptr32<uint16>(at<uint32_t>(sp + 0x2cu)),
                             ptr32<uint16>(at<uint32_t>(sp + 0x30u)));
    at<int16_t>(sp + 2u) = -0x400;
    at<int16_t>(sp + 4u) = -0x2000;
    at<int16_t>(sp + 0x12u) = 0x400;
    at<int16_t>(sp + 0x14u) = 0x2000;
    at<int16_t>(sp) = -0x2000;
    at<int16_t>(sp + 0x10u) = 0x2000;
    guest_call(sym::TColBuildLvlCollision, modules, at<uint32_t>(sp + 0x24u), at<uint32_t>(sp + 0x28u), at<uint32_t>(sp + 0x2cu),
               at<uint32_t>(sp + 0x30u), sp, sp + 0x10u);
    guest_call<int32_t>(sym::MemBlockFree, sp + 0x30u);
    guest_call<int32_t>(sym::MemBlockFree, sp + 0x2cu);
    guest_call<int32_t>(sym::MemBlockFree, sp + 0x28u);
    guest_call<int32_t>(sym::MemBlockFree, sp + 0x24u);
}

// The level's eight start positions (laid out for a start at the origin facing north) turned and moved to the start grid.
void TEditUpdateStartGridPositions()
{
    GuestFrame frame(0x10);
    const sTEditClass &t = te();
    const uint32_t positions = at<uint32_t>(G(pLvlG).address + 0x30u);
    const uint16_t dx = static_cast<uint16_t>(((t.trackDesc.startXPos - 8) << 11) + 0x400);
    const uint16_t dy = static_cast<uint16_t>(t.trackDesc.startElevation + 0x3f);
    const uint16_t dz = static_cast<uint16_t>(((t.trackDesc.startYPos - 8) << 11) + 0x400);
    const auto h = [](uint32_t address) -> uint16_t & { return at<uint16_t>(address); };
    const uint32_t orientation = static_cast<uint32_t>(t.trackDesc.startOrientation);
    for (uint32_t i = 0; i < 8u && orientation >= 1u && orientation <= 3u; ++i)
    {
        const uint32_t p = positions + i * 8u;
        const uint16_t x = h(p), z = h(p + 4u);
        if (orientation == 1u)
        {
            h(p + 4u) = static_cast<uint16_t>(0u - x);
            h(p) = z;
        }
        else if (orientation == 2u)
        {
            h(p) = static_cast<uint16_t>(0u - x);
            h(p + 4u) = static_cast<uint16_t>(0u - z);
        }
        else
        {
            h(p + 4u) = x;
            h(p) = static_cast<uint16_t>(0u - z);
        }
    }
    for (uint32_t i = 0; i < 8u; ++i)
    {
        const uint32_t p = positions + i * 8u;
        h(p) = static_cast<uint16_t>(h(p) + dx);
        h(p + 2u) = static_cast<uint16_t>(h(p + 2u) + dy);
        h(p + 4u) = static_cast<uint16_t>(h(p + 4u) + dz);
    }
}

// The pickups: at most two for every three modules (15 nodes), a pair at a random node of each stretch (the first not
// in the start's six), on the node's centre and the point above it, 10 up.
void TEditUpdatePickupNumAndPositions()
{
    GuestFrame frame(0x80);
    const sAINodeClass &ai = G(aiNodeVarG);
    const uint32_t pickups = sym::pickupVarG;
    const uint32_t most = static_cast<uint32_t>(ai.noNodesSingle / 15) << 1;
    uint32_t count = at<uint32_t>(pickups + 0x48u);
    if (most < count)
    {
        at<uint32_t>(pickups + 0x48u) = most;
        count = most;
    }
    uint32_t position = at<uint32_t>(pickups + 0x44u);
    if (count >> 1 == 0)
        return;
    uint32_t pair = 0, stretch = 0;
    do
    {
        const uint32_t base = ai.pNode.address + stretch;
        const uint32_t node = base + guest_call<uint32_t>(sym::RandomGetRange, pair == 0 ? 6 : 0, 0xe) * 0x6cu;
        ++pair;
        stretch += 0x654u;
        at<uint16_t>(position + 8u) = at<uint16_t>(node + 0x28u);
        at<uint16_t>(position + 0xau) = static_cast<uint16_t>(at<uint16_t>(node + 0x2au) + 0xau);
        at<uint16_t>(position + 0xcu) = at<uint16_t>(node + 0x2cu);
        position += 0x10u;
        at<uint16_t>(position + 8u) = at<uint16_t>(node + 0x30u);
        at<uint16_t>(position + 0xau) = static_cast<uint16_t>(at<uint16_t>(node + 0x32u) + 0xau);
        at<uint16_t>(position + 0xcu) = at<uint16_t>(node + 0x34u);
        position += 0x10u;
    } while (pair < at<uint32_t>(pickups + 0x48u) >> 1);
}

// The grid built from the track's modules for the race (the module list is freed), and an empty landscape parent of
// 16 x 16 cells of 0x800 for the tiles. With no modules the cursor's slope is the caller's $t0.
void TEditConvertTrackToALF()
{
    uint32_t slope = getRegU32(&context(), 8);
    GuestFrame frame(0x100);
    const uint32_t sp = frame.local<uint8_t>(0).address; // cursorModule 0x40, x 0x50, y 0x52, elevation 0x54,
                                                         // direction 0x60, pLookModule 0x64, pLsParent 0x68
    sTEditClass &t = te();
    at<uint32_t>(sp + 0x68u) = 0;
    TEditClearTrack();
    startAt(sp + 0x50u, sp + 0x52u, sp + 0x54u, sp + 0x60u);
    t.resourceMemoryUsed = 0;
    for (uint32_t module = t.trackDesc.pTrackModuleList.address; module != 0; module = at<uint32_t>(module))
    {
        at<uint32_t>(sp + 0x44u) = editModuleDesc(at<uint32_t>(module + 4u));
        slope = static_cast<uint32_t>(adjust(sp + 0x40u, sp + 0x50u, sp + 0x52u, sp + 0x60u, sp + 0x54u, 2));
    }
    const uint32_t list = t.trackDesc.pTrackModuleList.address;
    t.cursor.yPos = at<int16_t>(sp + 0x52u);
    t.cursor.direction = static_cast<eDirection>(at<int32_t>(sp + 0x60u));
    t.cursor.elevation = at<int16_t>(sp + 0x54u);
    t.cursor.currentSlope = static_cast<eSlope>(slope);
    t.cursor.xPos = at<int16_t>(sp + 0x50u);
    if (list != 0)
    {
        at<uint32_t>(sp + 0x64u) = list;
        for (uint32_t node = list; node != 0;)
        {
            const uint32_t next = at<uint32_t>(node);
            guest_call<int32_t>(sym::MemBlockFree, sp + 0x64u);
            node = next;
            at<uint32_t>(sp + 0x64u) = next;
        }
        t.trackDesc.pTrackModuleList = nullptr;
    }
    guest_call<int32_t>(sym::MemBlockAllocLo, sp + 0x68u, 0x74);
    const uint32_t parent = at<uint32_t>(sp + 0x68u);
    at<int32_t>(parent + 0x68u) = 0x800;
    at<int32_t>(parent + 0x5cu) = -0x4000;
    at<int32_t>(parent + 0x64u) = 0x10;
    at<int32_t>(parent + 0x58u) = -0x4000;
    at<int32_t>(parent + 0x60u) = 0x10;
    at<int32_t>(kTE + 0x274u) = 0x800;
    at<int32_t>(sym::lsVarG + 8u) = 0x800;
    at<uint32_t>(sym::lsVarG) = parent;
    const int32_t middle = at<int32_t>(parent + 0x68u) >> 1;
    at<uint32_t>(kTE + 0x26cu) = parent;
    at<int32_t>(kTE + 0x278u) = middle;
    at<int32_t>(sym::lsVarG + 0xcu) = middle;
}

// The race's track: every tile of the grid, then the level's sky.
void TEditRenderGameTrack()
{
    uint32_t sky;
    {
        GuestFrame frame(0xc0);
        const uint32_t theme = te().trackDesc.themeTypeNdx;
        uint32_t unit = te().track.pTrackUnits.address;
        for (int32_t layer = 0; layer < 3; ++layer)
            for (int32_t y = 0; y < 16; ++y)
                for (int32_t x = 0; x < 16; ++x, unit += 8u)
                    if (at<int16_t>(unit) != -1)
                        guest_call(sym::TileRenderGameROB, x, y, 0x10, 0x10, unit, theme);
        sky = at<uint32_t>(G(pLvlG).address + 0x4cu);
        guest_call(sym::LsCameraToRdr, sym::rdrCameraG);
    }
    guest_call(sym::TRdr_VUShape, sky, sym::rdrCameraG, 0);
}

// The saved track's memory card icon, 16 x 16 at 4 bits (the top row last): 1 where the grid holds a tile on any
// layer, 2 where one of them is tile 0x43 (the start grid).
void TEditGenerateMemCardIconFromLevel(ptr32<int32> pBitMap)
{
    GuestFrame frame(0x60);
    guest_call(sym::MemBlockSet32, pBitMap, 0, 0x80);
    for (int32_t y = 0; y < 16; ++y)
        for (int32_t x = 0; x < 16; ++x)
        {
            const uint32_t units = te().track.pTrackUnits.address;
            bool found = false, start = false;
            for (int32_t layer = 0; layer < 3 && !start; ++layer)
            {
                const int16_t tile = at<int16_t>(units + static_cast<uint32_t>((layer << 8) + y * 16 + x) * 8u);
                if (tile == -1)
                    continue;
                found = true;
                start = tile == 0x43;
            }
            if (!found)
                continue;
            const uint32_t value = start ? 2u : 1u;
            const uint32_t cell = pBitMap.address + static_cast<uint32_t>((x >> 1) + (0xf - y) * 8);
            if ((x & 1) != 0)
                at<uint8_t>(cell) = static_cast<uint8_t>((at<uint8_t>(cell) & 0xfu) + (value << 4));
            else
                at<uint8_t>(cell) = static_cast<uint8_t>(value);
        }
}

// The race's hud map, 64 x 64 at 4 bits: the tile pictures (the level's map sprite, 32 x 32 at 4 bits) spread to a
// byte a pixel, each unit of the track drawn from them 4 x 4 into a 64 x 64 byte map, and that packed (upside down).
void TEditGenerateHudMapForTrackLevel()
{
    GuestFrame frame(0x140);
    const uint32_t sp = frame.local<uint8_t>(0).address; // cursorModule 0x0, x 0x90, y 0x92, elevation 0x94,
                                                         // pHudMapBits 0x98, direction 0x9c
    sTEditClass &t = te();
    at<uint32_t>(sp + 0x98u) = 0;
    t.pHudMapBitsBPP = nullptr;
    t.pHudMapBPP = nullptr;
    guest_call<int32_t>(sym::MemBlockAllocLo, sp + 0x98u, 0x200);
    const uint32_t picture = at<uint32_t>(at<uint32_t>(t.pTrackMapBits.address) + 0x48u);
    std::memcpy(guest_host(at<uint32_t>(sp + 0x98u)), guest_host(picture), 0x200u);
    guest_call<int32_t>(sym::MemBlockAllocLo, kTE + 0x268u, 0x400);
    for (int32_t y = 0; y < 0x20; ++y)
        for (int32_t x = 0; x < 0x20; ++x)
        {
            const uint8_t packed = at<uint8_t>(at<uint32_t>(sp + 0x98u) + static_cast<uint32_t>((x >> 1) + y * 0x10));
            at<uint8_t>(t.pHudMapBitsBPP.address + static_cast<uint32_t>(x + y * 0x20)) = static_cast<uint8_t>((x & 1) != 0 ? packed >> 4 : packed & 0xfu);
        }
    guest_call<int32_t>(sym::MemBlockFree, sp + 0x98u);
    guest_call<int32_t>(sym::MemBlockAllocLo, kTE + 0x264u, 0x1000);
    guest_call(sym::MemBlockSet32, t.pHudMapBPP, 0, 0x1000);
    startAt(sp + 0x90u, sp + 0x92u, sp + 0x94u, sp + 0x9cu);
    for (uint32_t module = t.trackDesc.pTrackModuleList.address; module != 0; module = at<uint32_t>(module))
    {
        at<uint32_t>(sp + 4u) = editModuleDesc(at<uint32_t>(module + 4u));
        adjust(sp, sp + 0x90u, sp + 0x92u, sp + 0x9cu, sp + 0x94u, 8);
    }
    guest_call<int32_t>(sym::MemBlockFree, kTE + 0x268u);
    t.pHudMap = nullptr;
    guest_call<int32_t>(sym::MemBlockAllocLo, kTE + 0x260u, 0x800);
    for (int32_t y = 0; y < 0x40; ++y)
        for (int32_t x = 0; x < 0x40; ++x)
        {
            const uint8_t pixel = at<uint8_t>(t.pHudMapBPP.address + static_cast<uint32_t>(x + y * 0x40));
            const uint32_t cell = t.pHudMap.address + static_cast<uint32_t>((x >> 1) + (0x3f - y) * 0x20);
            if ((x & 1) != 0)
                at<uint8_t>(cell) = static_cast<uint8_t>((at<uint8_t>(cell) & 0xfu) + (pixel << 4));
            else
                at<uint8_t>(cell) = pixel;
        }
    guest_call<int32_t>(sym::MemBlockFree, kTE + 0x264u);
}

// A 4 x 4 tile picture from the spread tile pictures at (xOffset, yOffset) onto the byte map at (x, y): mirrored (flag
// 0x20), then turned a quarter (2), a half (4) or three quarters (8); its zero pixels leave the map as it is.
void TEditRenderTileToHudMap(int16 x, int16 y, int16 xOffset, int16 yOffset, uint32 flags)
{
    const sTEditClass &t = te();
    uint8_t picture[4][4], turned[4][4];
    for (uint32_t r = 0; r < 4u; ++r)
        for (uint32_t c = 0; c < 4u; ++c)
            picture[r][c] = at<uint8_t>(t.pHudMapBitsBPP.address + c + static_cast<uint32_t>(xOffset + (yOffset + static_cast<int32_t>(r)) * 0x20));
    if ((flags & 0x20u) != 0)
        for (uint32_t r = 0; r < 4u; ++r)
        {
            std::swap(picture[r][0], picture[r][3]);
            std::swap(picture[r][1], picture[r][2]);
        }
    std::memcpy(turned, picture, sizeof(turned));
    if ((flags & 2u) != 0)
    {
        for (uint32_t r = 0; r < 4u; ++r)
            for (uint32_t c = 0; c < 4u; ++c)
                turned[c][3u - r] = picture[r][c];
    }
    else if ((flags & 4u) != 0)
    {
        for (uint32_t r = 0; r < 4u; ++r)
            for (uint32_t c = 0; c < 4u; ++c)
                turned[3u - r][3u - c] = picture[r][c];
    }
    else if ((flags & 8u) != 0)
    {
        for (uint32_t r = 0; r < 4u; ++r)
            for (uint32_t c = 0; c < 4u; ++c)
                turned[3u - c][r] = picture[r][c];
    }
    for (uint32_t r = 0; r < 4u; ++r)
        for (uint32_t c = 0; c < 4u; ++c)
            if (turned[r][c] != 0)
                at<uint8_t>(t.pHudMapBPP.address + c + static_cast<uint32_t>(x + (static_cast<int32_t>(r) + y) * 0x40)) = turned[r][c];
}

// The hud map sent to the GS (the level's map sprite's texture, 64 x 64 at 4 bits) and freed.
void TEditCopyHudMapForTrackLevelToVRAM()
{
    GuestFrame frame(0x80);
    const uint32_t image = frame.local<uint8_t>(0).address;
    const uint32_t sprite = at<uint32_t>(at<uint32_t>(G(pLvlG).address + 0x40u) + 8u);
    const int32_t block = at<int16_t>(at<uint32_t>(sprite) + 0x2cu);
    guest_call(sym::sceGsSetDefLoadImage, image, block, 1, 0x14, 0, 0, 0x40, 0x40);
    guest_call(sym::FlushCache, 0);
    guest_call(sym::sceGsExecLoadImage, image, te().pHudMap);
    guest_call(sym::sceGsSyncPath, 0, 0);
    guest_call<int32_t>(sym::MemBlockFree, kTE + 0x260u);
}

} // namespace rc

RC_DECOMP_FN_SETUP(TEditGeneratePreTColData, rc::fuzz::teditLevel)
RC_DECOMP_FN_SETUP(TEditConstructAINode, rc::fuzz::teditAINodeSet)
RC_DECOMP_FN_SETUP(TEditGenerateTrackAINodes, rc::fuzz::teditLevel)
RC_DECOMP_FN_SETUP(TEditGenerateTCol, rc::fuzz::teditLevel)
RC_DECOMP_FN_SETUP(TEditUpdateStartGridPositions, rc::fuzz::teditLevel)
RC_DECOMP_FN_SETUP(TEditUpdatePickupNumAndPositions, rc::fuzz::teditLevel)
RC_DECOMP_FN_SETUP(TEditConvertTrackToALF, rc::fuzz::teditLevel)
RC_DECOMP_FN_SETUP_HEAP(TEditRenderGameTrack, rc::fuzz::teditGameTrack)
RC_DECOMP_FN_SETUP(TEditGenerateMemCardIconFromLevel, rc::fuzz::teditLevel)
RC_DECOMP_FN_SETUP(TEditGenerateHudMapForTrackLevel, rc::fuzz::teditHudMapBuild)
RC_DECOMP_FN_SETUP(TEditRenderTileToHudMap, rc::fuzz::teditHudMap)
RC_DECOMP_FN_BLOCKING_SETUP(TEditCopyHudMapForTrackLevelToVRAM, rc::fuzz::teditHudMap) // sends an image to the GS and waits for it

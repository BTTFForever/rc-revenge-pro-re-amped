// Decompiled from the computer drivers: AINODE.c (the track's route nodes), AI.c and PLAYER.c, and the small VISFX.c
// and INPUT.c accessors they use.
//
// A track's route is a list of nodes, each a gate across the track with a centre point and a second "overtaking"
// centre. Each node links forward to one or two next nodes (a fork), and records for each link the direction and
// distance to it. Nodes on water have link.onwater set to something other than -1, and only boats use them.
// Positions here are the physics position divided by 256, in 16-bit vectors.

#include "ai_fuzz.h"
#include "ee_math.h"

namespace rc {

angle ArcTan(int32 x, int32 y);
void ResTypeSetConstructDaemon(ptr32<char> pType, uint32 constructDaemon, int16 options);
void ResTypeUnSetConstructDaemon(ptr32<char> pType);
uint32 VecLength(ptr32<_sVec16> pVec);
bool_ AColDownCast(ptr32<_sColCellVecPSX> pPos, ptr32<_sColCellVecPSX> pPoi, ptr32<_sColNormalPSX> pNormal, ptr32<uint8> pMatId);

namespace {

const uint32_t kAIControl = 0x1000c0;             // AIControl: a car whose task runs it is driven by the computer.
const uint32_t kAINodeConstructDaemon = 0x1016c8; // Called by the loader for each "AIN" resource.
const uint32_t kPrintf = 0x12e9e8;
const ptr32<char> kAINodeType(0x327c00u); // "AIN"

bool computerDriven(const _sObject &obj)
{
    return obj.pTask->pFnCallback.address == kAIControl;
}

// libgcc's __fixunssfsi.
uint32_t ee_ftoui(ee_float x)
{
    const ee_float half = ee_float(2147483648.0f);
    if (x.v >= half.v)
        return static_cast<uint32_t>(ee_ftoi(x - half)) ^ 0x80000000u;
    return static_cast<uint32_t>(ee_ftoi(x));
}

} // namespace

// The newlib rand in the game's SDK: its state is a word in the reentrancy struct.
uint32_t &randState()
{
    const uint32_t impure = *static_cast<uint32_t *>(guest_host(0x3213f4u));
    return *static_cast<uint32_t *>(guest_host(impure + 0x58u));
}

void srand(uint32 seed)
{
    randState() = seed;
}

int32 rand()
{
    uint32_t &state = randState();
    state = state * 0x41C64E6Du + 0x3039u;
    return static_cast<int32>(state & 0x7FFFFFFFu);
}

// 3DMATH.c. Scales pOut to length 4096 by the length of pVec (the game always passes the same vector twice) and
// returns that length. A zero vector is left alone.
uint32_t vecNormalise(SVECTOR &out, const SVECTOR &v)
{
    const ee_float x = ee_itof(v.vx), y = ee_itof(v.vy), z = ee_itof(v.vz);
    const ee_float squared = x * x + y * y + z * z;
    if (squared.v == 0.0f)
        return 0;
    const ee_float length = ee_sqrt(squared);
    const ee_float scale = ee_float(4096.0f) / length;
    const ee_float outZ = ee_itof(out.vz), outX = ee_itof(out.vx), outY = ee_itof(out.vy);
    out.vz = static_cast<int16>(ee_ftoi(outZ * scale));
    out.vx = static_cast<int16>(ee_ftoi(outX * scale));
    out.vy = static_cast<int16>(ee_ftoi(outY * scale));
    return ee_ftoui(length);
}

uint32 VecNormalise(ptr32<SVECTOR> pOut, ptr32<SVECTOR> pVec)
{
    const SVECTOR v = *pVec;
    return vecNormalise(*pOut, v);
}

// VISFX.c and INPUT.c accessors.

// True for the cars and false for the boats: every car type comes before the first boat type in the enum.
// The comparison is unsigned (sltiu).
int32 VfxAreWeInCar(ptr32<_sObject> pObj)
{
    return static_cast<uint32_t>(pObj->pDef->type) < static_cast<uint32_t>(OBJECT_TYPE_BADDRC_BOAT) ? 1 : 0;
}

ptr32<SVECTOR> VfxCarPositionPoi(ptr32<_sObject> pObj)
{
    const sVec32 &pos = pObj->pPhysics->pInteger->renderPos;
    SVECTOR &out = G(svVelTemp);
    out.vz = static_cast<int16>(static_cast<uint32_t>(pos.z) >> 8);
    out.vx = static_cast<int16>(static_cast<uint32_t>(pos.x) >> 8);
    out.vy = static_cast<int16>(static_cast<uint32_t>(pos.y) >> 8);
    return ptr32<SVECTOR>(guest_addr(&out));
}

ptr32<MATRIX> VfxCarMatrixPoi(ptr32<_sObject> pObj)
{
    return ptr32<MATRIX>(pObj->pPhysics->pInteger.address + offsetof(_sOPhysLibIntegerCarState, mat));
}

ptr32<SVECTOR> VfxCarVelocityPoi(ptr32<_sObject> pObj)
{
    return ptr32<SVECTOR>(pObj->pPhysics->pInteger.address + offsetof(_sOPhysLibIntegerCarState, vel));
}

void InputSetVibrationFlags(eInputPad ePad, uint8 padFlags)
{
    G(inputVarG).vibrationFlags[ePad == INPUT_MULTITAP_1 ? 0 : 1] = padFlags;
}

bool_ InputHeld(uint8 key)
{
    return static_cast<bool_>(G(inputVarG).inactiveBuffer[key] & 1);
}

uint8 InputPressureHit(uint8 key)
{
    return G(inputVarG).pressureBuffer[key];
}

namespace {

const uint32_t kFarAway = 0x10000000; // Starting best distance squared for the nearest-node searches.

// The car's position in 16-bit units, left in svGlob as the original does.
SVECTOR &carPosition(const _sObject &obj)
{
    const sVec32 &pos = obj.pPhysics->pInteger->pos;
    SVECTOR &out = G(svGlob);
    out.vz = static_cast<int16>(static_cast<uint32_t>(pos.z) >> 8);
    out.vx = static_cast<int16>(static_cast<uint32_t>(pos.x) >> 8);
    out.vy = static_cast<int16>(static_cast<uint32_t>(pos.y) >> 8);
    return out;
}

// Cars drive on the land nodes, boats on the water nodes.
bool usableNode(const _sAINode &node, bool inCar)
{
    return inCar ? node.link.onwater != -1 : node.link.onwater == -1;
}

// Inside the node's box on X and Z, and within 400 units of its centre in height.
bool insideNode(const _sAINode &node, const SVECTOR &pos)
{
    if (pos.vx < node.boundsMin[0] || node.boundsMax[0] < pos.vx)
        return false;
    if (pos.vz < node.boundsMin[1] || node.boundsMax[1] < pos.vz)
        return false;
    const int32_t y = node.centre.vy;
    return y < pos.vy + 400 && pos.vy - 400 < y;
}

uint32_t distanceSquared(const SVECTOR &a, const SVECTOR &b)
{
    sVec16 d;
    d.x = static_cast<int16>(a.vx - b.vx);
    d.y = static_cast<int16>(a.vy - b.vy);
    d.z = static_cast<int16>(a.vz - b.vz);
    return vec16LengthSquared(d);
}

// div's remainder as the recompiled code computes it, including the cases x86 traps on.
int32_t ee_rem(int32_t a, int32_t b)
{
    if (b == 0)
        return a;
    if (a == INT32_MIN && b == -1)
        return 0;
    return a % b;
}

// Which of a node's two links the player follows at a fork: a bit per player in link.dir.
int32_t forkBranch(const _sAINode &node, uint8 playerNo)
{
    return (static_cast<int32_t>(node.link.dir) & (1 << (playerNo & 31))) != 0 ? 1 : 0;
}

} // namespace

// AINODE.c

ptr32<SVECTOR> AIN_CarPositionPoi(ptr32<_sObject> pObj)
{
    return ptr32<SVECTOR>(guest_addr(&carPosition(*pObj)));
}

// Returned in a register: the four shorts of svGlob.
uint64 AIN_CarPositionVal(ptr32<_sObject> pObj)
{
    const SVECTOR &pos = carPosition(*pObj);
    uint64_t packed;
    std::memcpy(&packed, &pos, 8);
    return packed;
}

ptr32<MATRIX> AIN_CarMatrixPoi(ptr32<_sObject> pObj)
{
    std::memcpy(&G(matGlob), &pObj->pRendObj->matrix, sizeof(MATRIX));
    return ptr32<MATRIX>(guest_addr(&G(matGlob)));
}

// Returns the matrix through the caller's buffer, as the EE does for structs.
ptr32<MATRIX> AIN_CarMatrixVal(ptr32<MATRIX> pResult, ptr32<_sObject> pObj)
{
    std::memcpy(&G(matGlob), &pObj->pRendObj->matrix, sizeof(MATRIX));
    std::memcpy(pResult.get(), &G(matGlob), sizeof(MATRIX));
    return pResult;
}

// The node the car is inside whose centre is nearest it, or null.
ptr32<_sAINode> AINodeNearest(ptr32<_sObject> pObj)
{
    const SVECTOR &pos = carPosition(*pObj);
    const bool inCar = VfxAreWeInCar(pObj) != 0;
    ptr32<_sAINode> nearest = nullptr;
    const sAINodeClass &ai = G(aiNodeVarG);
    if (!ai.pNode)
        return nearest;
    uint32_t best = kFarAway;
    for (int32_t i = 0; i < ai.noNodesSingle; ++i)
    {
        const ptr32<_sAINode> node = ai.pNode + i;
        if (!usableNode(*node, inCar) || !insideNode(*node, pos))
            continue;
        const uint32_t d = distanceSquared(node->centre, pos);
        if (static_cast<int32_t>(d) < static_cast<int32_t>(best))
        {
            best = d;
            nearest = node;
        }
    }
    return nearest;
}

// As AINodeNearest, but only nodes within 11000 units along the route of the car's last valid node. The answer goes
// in pRecurseSCR->pNearestNode, which is left alone if no node qualifies; the return value is always null.
ptr32<_sAINode> AINodeNearestInRange(ptr32<_sObject> pObj)
{
    sAINodeClass &ai = G(aiNodeVarG);
    const ptr32<_sRecurseSCR> scratch = ai.pRecurseSCR;
    const SVECTOR &pos = carPosition(*pObj);
    const bool inCar = VfxAreWeInCar(pObj) != 0;
    const int32_t along = pObj->carAI.pPrevValidNode->iFinishDist;
    uint32_t best = kFarAway;
    for (int32_t i = 0; i < ai.noNodesSingle; ++i)
    {
        const ptr32<_sAINode> node = ai.pNode + i;
        if (!usableNode(*node, inCar))
            continue;
        int32_t gap = along - node->iFinishDist;
        if (gap < 1)
            gap = node->iFinishDist - along;
        if (gap >= 11000 || !insideNode(*node, pos))
            continue;
        const uint32_t d = distanceSquared(node->centre, pos);
        if (static_cast<int32_t>(d) < static_cast<int32_t>(best))
        {
            best = d;
            scratch->pNearestNode = node;
        }
    }
    return nullptr;
}

// Whether the car is inside any node. Spread over frames: each call tests ten nodes, carrying on from where the last
// call stopped, and the next search starts at the node that matched.
int32 AINodeInAnyNode(ptr32<_sObject> pObj)
{
    const SVECTOR &pos = carPosition(*pObj);
    const bool inCar = VfxAreWeInCar(pObj) != 0;
    sAINodeClass &ai = G(aiNodeVarG);
    if (!ai.pNode)
        return 0;
    int32_t index = ai.iNodeSearcher;
    for (int32_t tried = 0; tried < 10; ++tried)
    {
        const _sAINode &node = ai.pNode[index];
        if (usableNode(node, inCar) && insideNode(node, pos))
        {
            ai.iNodeSearcher = ee_rem(ai.iNodeSearcher + tried, ai.noNodesSingle);
            return 1;
        }
        if (++index == ai.noNodesSingle)
            index = 0;
    }
    ai.iNodeSearcher = ee_rem(ai.iNodeSearcher + 10, ai.noNodesSingle);
    return 0;
}

// How far the car is past its current node's centre, along the route (the average of both directions at a fork).
int32 AINodeDistanceAlong(ptr32<_sObject> pObj)
{
    const _sAINode &node = *pObj->carAI.pCurNode;
    const SVECTOR &pos = carPosition(*pObj);
    sMat &gte = G(gteMat);
    gte.Xx = static_cast<int16>(pos.vx - node.centre.vx);
    gte.Xy = static_cast<int16>(pos.vy - node.centre.vy);
    gte.Xz = static_cast<int16>(pos.vz - node.centre.vz);
    const SVECTOR *forward = node.link.svForwardDir;
    if (!node.next[1])
    {
        gteMulVec16(asVec16(forward[0]));
    }
    else
    {
        sVec16 average;
        average.x = static_cast<int16>((forward[0].vx + forward[1].vx) >> 1);
        average.y = static_cast<int16>((forward[0].vy + forward[1].vy) >> 1);
        average.z = static_cast<int16>((forward[0].vz + forward[1].vz) >> 1);
        gteMulVec16(average);
    }
    return G(gteOut).x;
}

// Where to put a car back on the track: two nodes past the one it got stuck on, else its last valid node, else the
// nearest node it can drive on, else the first node.
ptr32<_sAINode> AINodeGetReSpawnNode(ptr32<_sObject> pObj)
{
    const sAINodeClass &ai = G(aiNodeVarG);
    if (!ai.pNode)
        return nullptr;
    const bool inCar = VfxAreWeInCar(pObj) != 0;
    _sCar_AI &carAI = pObj->carAI;
    if (carAI.pStuckOnNode)
    {
        const ptr32<_sAINode> stuck = carAI.pStuckOnNode;
        carAI.pStuckOnNode = nullptr;
        return stuck->next[0]->next[0];
    }
    ptr32<_sAINode> node = carAI.pPrevValidNode;
    if (!node)
    {
        uint32_t best = kFarAway;
        const SVECTOR &pos = carPosition(*pObj);
        for (int32_t i = 0; i < ai.noNodesSingle; ++i)
        {
            const ptr32<_sAINode> candidate = ai.pNode + i;
            if (!usableNode(*candidate, inCar))
                continue;
            const uint32_t d = distanceSquared(candidate->centre, pos);
            if (static_cast<int32_t>(d) < static_cast<int32_t>(best))
            {
                best = d;
                node = candidate;
            }
        }
    }
    return node ? node : ai.pNode;
}

// The heading of the route at a point: the direction of the nearest node's first link, or 0 outside every node.
int32 AINodeGetNearestHeading(ptr32<SVECTOR> svPos)
{
    const sAINodeClass &ai = G(aiNodeVarG);
    ptr32<_sAINode> nearest = nullptr;
    if (ai.pNode)
    {
        const SVECTOR &pos = *svPos;
        uint32_t best = kFarAway;
        for (int32_t i = 0; i < ai.noNodesSingle; ++i)
        {
            const ptr32<_sAINode> node = ai.pNode + i;
            if (!insideNode(*node, pos))
                continue;
            const uint32_t d = distanceSquared(node->centre, pos);
            if (static_cast<int32_t>(d) < static_cast<int32_t>(best))
            {
                best = d;
                nearest = node;
            }
        }
    }
    if (!nearest)
        return 0;
    const SVECTOR &forward = nearest->link.svForwardDir[0];
    return ArcTan(forward.vz, forward.vx);
}

// The point on the racing line iDist units ahead of the car, following this player's branch at forks. Returns null
// if the car has no node or the point would be behind it. Also moves the car's looking node to the node it lands on.
ptr32<SVECTOR> AINodeUpdateRacingLineAhead(ptr32<_sObject> pObj, int32 iDist)
{
    _sCar_AI &carAI = pObj->carAI;
    ptr32<_sAINode> node = carAI.pCurNode;
    int32_t remaining = iDist + carAI.iAlongNodeDist;
    if (!node || remaining < 1)
        return nullptr;
    const ptr32<_sAINode> lookingBefore = carAI.pLookingNode;
    const uint8 overtaking = carAI.bOvertaking;
    int32_t branch;
    for (;;)
    {
        // A null node here reads guest address 0, as the original does.
        branch = !node || node->next[1] ? forkBranch(*node, pObj->playerNo) : 0;
        remaining -= node->link.dist[branch];
        if (remaining < 0)
            break;
        node = node->next[branch];
    }
    const int16_t along = static_cast<int16_t>(remaining + node->link.dist[branch]);
    carAI.pPrevLookingNode = lookingBefore;
    carAI.pLookingNode = node;
    sVec16 point;
    const SVECTOR *centre;
    if (overtaking == 0)
    {
        vec16MulScaler(point, asVec16(node->link.svForwardDir[branch]), along);
        centre = &node->centre;
    }
    else
    {
        vec16MulScaler(point, asVec16(node->link.svForwardOverDir[branch]), along);
        centre = &node->overCentre;
    }
    SVECTOR &out = G(svGlob);
    out.vx = static_cast<int16>(point.x + centre->vx);
    out.vy = static_cast<int16>(point.y + centre->vy);
    out.vz = static_cast<int16>(point.z + centre->vz);
    // The original copies its whole local vector, whose fourth short it never wrote (sp+6 in a 0x40-byte frame).
    out.pad = stale_stack<int16>(0x40, 6);
    return ptr32<SVECTOR>(guest_addr(&out));
}

// Fills possibleAIRoutes[count..31] with points 200 units apart along the route ahead of the car, always taking the
// branch nearer the finish. The first points are blended from the car's own position towards the route.
void AINodeRouteFinder(ptr32<_sObject> pObj, int32 iDistance, int32 count)
{
    while (count != 0x20)
    {
        const SVECTOR player = carPosition(*pObj);
        ptr32<_sAINode> node = pObj->carAI.pCurNode;
        int32_t remaining = iDistance + pObj->carAI.iAlongNodeDist;
        if (remaining < 0)
            remaining = 0;
        if (!node)
            return;
        int32_t branch;
        int32_t dist;
        for (;;)
        {
            branch = node->next[1] ? (node->next[0]->iFinishDist < node->next[1]->iFinishDist ? 1 : 0) : 0;
            dist = node->link.dist[branch];
            remaining -= dist;
            if (remaining < 0)
                break;
            node = node->next[branch];
        }
        sVec16 point;
        vec16MulScaler(point, asVec16(node->link.svForwardDir[branch]), static_cast<int16_t>(remaining + dist));
        point.x = static_cast<int16>(point.x + node->centre.vx);
        point.y = static_cast<int16>(point.y + node->centre.vy);
        point.z = static_cast<int16>(point.z + node->centre.vz);

        sVec16 result;
        const int32_t blend = iDistance - 200;
        if (blend < 0x1000)
        {
            const int32_t weight = blend < 0 ? 0 : blend;
            sVec16 scaledPoint, scaledPlayer;
            vec16MulScaler(scaledPoint, point, static_cast<int16_t>(weight));
            vec16MulScaler(scaledPlayer, asVec16(player), static_cast<int16_t>(0x1000 - weight));
            result.x = static_cast<int16>(scaledPlayer.x + scaledPoint.x);
            result.y = static_cast<int16>(scaledPlayer.y + scaledPoint.y);
            result.z = static_cast<int16>(scaledPlayer.z + scaledPoint.z);
        }
        else
        {
            result = point;
        }
        SVECTOR &route = G(aiNodeVarG).possibleAIRoutes[count];
        route.vx = result.x;
        route.vy = result.y;
        route.vz = result.z;
        // The original recurses for the next point; a loop does the same.
        iDistance += 200;
        ++count;
    }
}

void AINodeTerm()
{
    ResTypeUnSetConstructDaemon(kAINodeType);
}

void AINodeInit()
{
    ResTypeSetConstructDaemon(kAINodeType, kAINodeConstructDaemon, 1);
    G(aiNodeVarG).pRecurseSCR = ptr32<_sRecurseSCR>(guest_addr(&G(scratchPadG)));
}

// Called by the loader when a level's node file arrives. The file holds a count and the nodes, whose links are
// indices (-1 for none); they become pointers. Each node is then marked as land or water: by level type for the
// water levels, otherwise by what surface lies under its centre.
void AINodeConstructDaemon(ptr32<memHandle> pHandle)
{
    if (!pHandle)
        return;
    const uint32_t file = pHandle->address;
    const int16_t count = *static_cast<int16_t *>(guest_host(file));
    const ptr32<_sAINode> nodes(file + 4u);
    const auto link = [&](ptr32<_sAINode> &ref)
    {
        const int32_t index = static_cast<int32_t>(ref.address);
        ref = index == -1 ? ptr32<_sAINode>(nullptr) : nodes + index;
    };
    for (int16_t i = 0; i < count; i = static_cast<int16_t>(i + 1))
    {
        _sAINode &node = nodes[i];
        link(node.prev[0]);
        link(node.next[0]);
        link(node.prev[1]);
        link(node.next[1]);
        const uint8 level = G(mainVarG).level;
        if (level == 'a')
            continue;
        const uint32_t kind = level % 12u;
        if (kind == 5 || kind == 0 || kind == 8)
        {
            node.link.onwater = -1;
            continue;
        }
        // AColDownCast takes guest pointers: the original's locals, at their places in its 0x100-byte frame.
        const ptr32<_sColCellVecPSX> pos = frame_local<_sColCellVecPSX>(0x100, 0x40);
        const ptr32<_sColCellVecPSX> poi = frame_local<_sColCellVecPSX>(0x100, 0x30);
        const ptr32<_sColNormalPSX> normal = frame_local<_sColNormalPSX>(0x100, 0x20);
        const ptr32<uint8> material = frame_local<uint8>(0x100, 0x60);
        _sColCellVecPSX start;
        start.x = ee_itof(node.centre.vx);
        start.y = ee_itof(static_cast<int16_t>(node.centre.vy + 0x32));
        start.z = ee_itof(node.centre.vz);
        start.pad = ee_float(0.0f);
        *pos = start;
        if (AColDownCast(pos, poi, normal, material))
            node.link.onwater = G(mtlG).mtl[*material].iWheelMark == 2 ? -1 : 0;
        else
            guest_call(kPrintf, 0x322628u, static_cast<int32_t>(i)); // "Warning - AINode Downcast failed (node %d)!"
    }
}

// For each node and each of its links: the direction and distance to the next node, for the racing line and the
// overtaking line, and the node's box on X and Z, which covers both ends of the link. Also the box around the whole
// route. link.dir gets a random bit per player, choosing each player's branch at forks; a branch to a node of
// priority 5 is always taken.
void AINodeCreateLinkInfo()
{
    sAINodeClass &ai = G(aiNodeVarG);
    srand(G(sysTimerVarG).tickTime);
    ai.iMinZ = 0x7fff;
    ai.iMaxZ = -0x7fff;
    ai.iMinX = 0x7fff;
    ai.iMaxX = -0x7fff;
    for (int32_t i = 0; i < ai.noNodesSingle; ++i)
    {
        _sAINode &node = ai.pNode[i];
        node.link.dir = static_cast<int8>(rand() + i);
        for (int32_t branch = 0; branch < 2; ++branch)
        {
            const ptr32<_sAINode> nextPtr = node.next[branch];
            if (!nextPtr)
            {
                node.link.dist[branch] = 0;
                continue;
            }
            const _sAINode &next = *nextPtr;
            if (next.priority == 5)
                node.link.dir = branch == 1 ? 0 : -1;

            int16_t minX = 0x7fff, maxX = -0x7fff, minZ = 0x7fff, maxZ = -0x7fff;
            node.boundsMin[0] = node.boundsMin[1] = minX;
            node.boundsMax[0] = node.boundsMax[1] = maxX;
            for (int side = 0; side < 2; ++side)
            {
                for (const SVECTOR *corner : std::initializer_list<const SVECTOR *>{&node.node[side], &next.node[side]})
                {
                    if (corner->vx < minX)
                        node.boundsMin[0] = minX = corner->vx;
                    if (corner->vx > maxX)
                        node.boundsMax[0] = maxX = corner->vx;
                    if (corner->vz < minZ)
                        node.boundsMin[1] = minZ = corner->vz;
                    if (corner->vz > maxZ)
                        node.boundsMax[1] = maxZ = corner->vz;
                }
            }
            if (minX < ai.iMinX)
                ai.iMinX = minX;
            if (minZ < ai.iMinZ)
                ai.iMinZ = minZ;
            if (ai.iMaxX < maxX)
                ai.iMaxX = maxX;
            if (ai.iMaxZ < maxZ)
                ai.iMaxZ = maxZ;

            // The distance kept is the overtaking line's: the racing line's is overwritten.
            SVECTOR &forward = node.link.svForwardDir[branch];
            forward.vx = static_cast<int16>(next.centre.vx - node.centre.vx);
            forward.vy = static_cast<int16>(next.centre.vy - node.centre.vy);
            forward.vz = static_cast<int16>(next.centre.vz - node.centre.vz);
            node.link.dist[branch] = static_cast<int32>(VecLength(ptr32<_sVec16>(guest_addr(&forward))));
            vecNormalise(forward, forward);
            SVECTOR &over = node.link.svForwardOverDir[branch];
            over.vx = static_cast<int16>(next.overCentre.vx - node.overCentre.vx);
            over.vy = static_cast<int16>(next.overCentre.vy - node.overCentre.vy);
            over.vz = static_cast<int16>(next.overCentre.vz - node.overCentre.vz);
            node.link.dist[branch] = static_cast<int32>(VecLength(ptr32<_sVec16>(guest_addr(&over))));
            vecNormalise(over, over);
        }
    }
}

// Picks the level's node list, records the start node and the lap length, and builds the links.
void AINodeSetupClass()
{
    const ptr32<uint32> file = G(pLvlG)->pAINode;
    if (!file)
        return;
    sAINodeClass &ai = G(aiNodeVarG);
    ai.noNodesSingle = static_cast<int16>(*file);
    ai.pNode = ptr32<_sAINode>(file.address + 4u);
    // After the last node comes one more record: its first four bytes are the start node, then the lap length.
    const _sAINode &trailer = ai.pNode[ai.noNodesSingle];
    std::memcpy(&ai.startNode, &trailer, 4);
    ai.nodeTotalDist = trailer.iFinishDist;
    AINodeCreateLinkInfo();
    ai.iNodeSearcher = 0;
}

int32 AINodeChooseNodeRoute(ptr32<_sObject> pObj, ptr32<_sAINode> pNode);

// The node search shared by AINodeUpdateRecursive and the functions that walk the route to call it.
//
// A node is a candidate if the car is strictly inside its box and within 400 units of its height; the nearest
// candidate's centre (the overtaking centre when overtaking) wins.
ptr32<_sAINode> AINodeUpdateRecursive(ptr32<_sAINode> pNode, int32 count, int32 iDir)
{
    _sRecurseSCR &scr = *G(aiNodeVarG).pRecurseSCR;
    _sAINode &node = *pNode;
    if (usableNode(node, scr.iType != 0))
    {
        const SVECTOR &pos = *scr.pPos;
        if (node.boundsMin[0] < pos.vx && pos.vx < node.boundsMax[0] && node.boundsMin[1] < pos.vz && pos.vz < node.boundsMax[1] &&
            node.centre.vy < pos.vy + 400 && pos.vy - 400 < node.centre.vy)
        {
            const SVECTOR &target = scr.pObj->carAI.bOvertaking == 0 ? node.centre : node.overCentre;
            scr.svLen.vx = static_cast<int16>(target.vx - pos.vx);
            scr.svLen.vy = static_cast<int16>(target.vy - pos.vy);
            scr.svLen.vz = static_cast<int16>(target.vz - pos.vz);
            const uint32_t d = vec16LengthSquared(asVec16(scr.svLen));
            scr.n = static_cast<int32>(d);
            if (static_cast<int32_t>(d) < scr.iDistance)
            {
                scr.iDistance = static_cast<int32>(d);
                scr.pNearestNode = pNode;
            }
        }
    }

    const int32_t remaining = count - 1;
    if (remaining == 0)
    {
        // At the far end of a forward search, the computer drivers choose their branch at the next fork.
        if (computerDriven(*scr.pObj) && iDir == 1 && node.next[1])
        {
            const int32_t bit = scr.iObjBit;
            if (AINodeChooseNodeRoute(scr.pObj, pNode) == 0)
                node.link.dir = static_cast<int8>(node.link.dir & ~bit);
            else
                node.link.dir = static_cast<int8>(node.link.dir | bit);
        }
        return pNode;
    }
    if (iDir == 1)
    {
        // Forwards. A computer driver only searches the branch it has chosen; anyone else searches both.
        ptr32<_sAINode> onward;
        if (!node.next[1])
        {
            onward = node.next[0];
            if (!onward)
                return pNode;
        }
        else
        {
            if ((static_cast<int32_t>(node.link.dir) & scr.iObjBit) == 0 || !computerDriven(*scr.pObj))
                AINodeUpdateRecursive(node.next[0], remaining, 1);
            if ((static_cast<int32_t>(node.link.dir) & scr.iObjBit) == 0 && computerDriven(*scr.pObj))
                return pNode;
            onward = node.next[1];
        }
        AINodeUpdateRecursive(onward, remaining, 1);
    }
    else
    {
        if (node.prev[0])
            AINodeUpdateRecursive(node.prev[0], remaining, 2);
        if (node.prev[1])
            AINodeUpdateRecursive(node.prev[1], remaining, 2);
    }
    return pNode;
}

// Walks back count-1 nodes along every route into pNode, then searches forwards from each node it reaches.
void AINodeBackTrackRecursive(ptr32<_sAINode> pNode, int32 count)
{
    do
    {
        if (--count == 0)
        {
            AINodeUpdateRecursive(pNode, G(aiNodeVarG).pRecurseSCR->iRange, 1);
            return;
        }
        if (!pNode->prev[0])
            AINodeUpdateRecursive(pNode, G(aiNodeVarG).pRecurseSCR->iRange, 1);
        else
            AINodeBackTrackRecursive(pNode->prev[0], count);
        pNode = pNode->prev[1];
    } while (pNode);
}

// Walks forward count-1 nodes along every route from pNode, then searches backwards from each node it reaches.
void AINodeForwardTrackRecursive(ptr32<_sAINode> pNode, int32 count)
{
    do
    {
        if (--count == 0)
        {
            AINodeUpdateRecursive(pNode, G(aiNodeVarG).pRecurseSCR->iRange, 2);
            return;
        }
        if (pNode->next[0])
            AINodeForwardTrackRecursive(pNode->next[0], count);
        pNode = pNode->next[1];
    } while (pNode);
}

// Which branch to take at a fork, from the priorities of the two nodes it leads to: 1 to 5 mark a shortcut kept for
// cars carrying weapons (1), for the back half of the field (3), for the front half and some cups (4), or always (5).
// Otherwise a coin toss. 0 if the node has no fork.
int32 AINodeChooseNodeRoute(ptr32<_sObject> pObj, ptr32<_sAINode> pNode)
{
    if (pNode && !pNode->next[1])
        return 0;
    const _sObject &obj = *pObj;
    const auto below = [](int64_t a, int64_t b) { return static_cast<uint64_t>(a) < static_cast<uint64_t>(b); };
    const int64_t racePos = obj.iRacePos;
    const int32_t half = static_cast<int32_t>(G(objectVarG).noObjects >> 1);
    const int32_t cup = G(frontEndVarG).cup;
    int32_t choice = rand() & 1;
    for (int32_t k = 0; k < 2; ++k)
    {
        const int8 priority = pNode->next[k]->priority;
        if (priority == 3)
            choice = below(racePos, half - 1) ? 1 - k : k;
        else if (priority == 1)
        {
            if (obj.weaponsHeld != 0)
                choice = 1 - k;
        }
        else if (priority == 4)
        {
            int32_t frontHalf = k;
            if (below(racePos, half))
                frontHalf = choice = 1 - k;
            if (static_cast<uint32_t>(cup) - 5u < 2u)
                choice = frontHalf;
            if (6 < cup)
                choice = k;
        }
        else if (priority == 5)
            choice = 1 - k;
    }
    return choice;
}

// Finds the node the car is in now, searching the route around its current node. A computer driver searches 8 nodes
// back, then 8 forward; anyone else searches both ways, then the nodes near its route position, then all nodes.
// Returns null if none contains the car.
ptr32<_sAINode> AINodeUpdateCurrent(ptr32<_sObject> pObj)
{
    _sCar_AI &carAI = pObj->carAI;
    if (!carAI.pCurNode)
        return nullptr;
    _sRecurseSCR &scr = *G(aiNodeVarG).pRecurseSCR;
    scr.pPos = ptr32<SVECTOR>(guest_addr(&G(svGlob)));
    scr.pNearestNode = nullptr;
    carPosition(*pObj);
    scr.iDistance = 0x10000000;
    scr.pObj = pObj;
    scr.iObjBit = 1 << (pObj->playerNo & 31);
    scr.iType = VfxAreWeInCar(pObj);
    if (computerDriven(*pObj))
    {
        scr.iRange = 0x10;
        AINodeBackTrackRecursive(carAI.pCurNode, 8);
        if (scr.pNearestNode)
            return scr.pNearestNode;
        scr.iRange = 8;
        AINodeForwardTrackRecursive(carAI.pCurNode, 8);
        return scr.pNearestNode;
    }
    scr.iRange = 0x10;
    AINodeBackTrackRecursive(carAI.pCurNode, 8);
    scr.iRange = 0x10;
    AINodeForwardTrackRecursive(carAI.pCurNode, 0x10);
    if (scr.pNearestNode)
    {
        carAI.iGettingLost = 0;
        return scr.pNearestNode;
    }
    AINodeNearestInRange(pObj);
    if (scr.pNearestNode)
    {
        carAI.iGettingLost = 0;
        return scr.pNearestNode;
    }
    carAI.iGoingWrongWay = 0;
    if (AINodeInAnyNode(pObj) != 0)
        carAI.iGettingLost = carAI.iGettingLost + 1;
    return scr.pNearestNode;
}

// The cosine of the angle between where the car points and the direction to its looking point, both flattened.
void AINodeUpdateAngleToRacingLine(ptr32<_sObject> pObj)
{
    const SVECTOR &pos = carPosition(*pObj);
    const SVECTOR &looking = pObj->carAI.svLookingPoint;
    SVECTOR toLooking{};
    toLooking.vx = static_cast<int16>(looking.vx - pos.vx);
    toLooking.vy = 0;
    toLooking.vz = static_cast<int16>(looking.vz - pos.vz);
    vecNormalise(toLooking, toLooking);
    sMat &gte = G(gteMat);
    gte.Xx = static_cast<int16>(-toLooking.vz);
    gte.Xy = 0;
    gte.Xz = toLooking.vx;
    std::memcpy(&G(matGlob), &pObj->pRendObj->matrix, sizeof(MATRIX));
    SVECTOR heading{};
    heading.vx = G(matGlob).m[0][2];
    heading.vy = 0;
    heading.vz = G(matGlob).m[2][2];
    vecNormalise(heading, heading);
    gteMulVec16(asVec16(heading));
    pObj->carAI.cosAngleToRacingLine = static_cast<int16>(G(gteOut).x);
}

// AI.c

const uint32_t kAINodeUpdateFinishDistance = 0x102fc8; // Not native yet: lap counting, sound, music and effects.

void AIInit(int32 noAI)
{
    G(aiVarG).noAIPlayers = noAI;
}

// Steering correction away from the car just ahead, if it is within 1000 units: +32 or -32 depending on which side
// of this car it is.
int32 AIAdjustForTooCloseToCarAhead(ptr32<_sObject> pObj, int32 i)
{
    _sCar_AI &carAI = pObj->carAI;
    const ptr32<_sObject> ahead = carAI.pNearestObjInFront;
    if (carAI.iTimeFinishedRace != 0 || !ahead || carAI.distNearestCarInFront >= 1000)
        return 0;
    SVECTOR here, there;
    const uint64_t herePacked = AIN_CarPositionVal(pObj);
    std::memcpy(&here, &herePacked, 8);
    const uint64_t therePacked = AIN_CarPositionVal(ahead);
    std::memcpy(&there, &therePacked, 8);
    const MATRIX &m = *AIN_CarMatrixPoi(pObj);
    sMat &gte = G(gteMat);
    gte.Xx = static_cast<int16>(there.vx - here.vx);
    gte.Xy = static_cast<int16>(there.vy - here.vy);
    gte.Xz = static_cast<int16>(there.vz - here.vz);
    sVec16 side;
    side.x = m.m[0][0];
    side.y = m.m[1][0];
    side.z = m.m[2][0];
    gteMulVec16(side);
    return G(gteOut).x > 0 ? -0x20 : 0x20;
}

// Rubber-banding: a computer car ahead of the human players eases off. i is its throttle.
int32 AIPlayCatchup(ptr32<_sObject> pObj, int32 i)
{
    if (VfxAreWeInCar(pObj) == 0)
        return i;
    const sObjectClass &objects = G(objectVarG);
    if (computerDriven(*objects.list[0]))
        return i;
    // The human to measure against: player one, or in a two-player race whichever is further ahead.
    ptr32<_sObject> human = objects.list[0];
    if (!computerDriven(*objects.list[1]) && objects.list[0]->iRacePos < objects.list[1]->iRacePos)
        human = objects.list[1];
    int32_t lead = pObj->carAI.iTotalRacedDist - human->carAI.iTotalRacedDist;
    if (lead < 0)
        return i;
    lead = (lead - 8000) >> 8;
    if (lead < 4)
        lead = 4;
    const int32_t cup = G(frontEndVarG).cup;
    if (4 < cup)
        lead >>= 1;
    if (6 < cup)
        lead >>= 1;
    if (0x23 < lead)
        lead = 0x23;
    i -= lead;
    if (0x7f < i)
        i = 0x7f;
    if (i < 0x10)
        i = 0x10;
    return i;
}

// Moves the car's current node along the route and keeps its last valid node, which only ever moves forward. A car
// whose search fails falls back to its last valid node. Then updates its distance to the finish.
void AIUpdateObjectNode(ptr32<_sObject> pObj)
{
    _sCar_AI &carAI = pObj->carAI;
    ptr32<_sAINode> current = carAI.pCurNode;
    if (!current)
    {
        carAI.pCurNode = AINodeNearest(pObj);
        current = carAI.pCurNode;
    }
    else
    {
        if (G(hudVarG).iReadySteadyGo == 0)
        {
            carAI.sStuckOnNodeCount = static_cast<int16>(carAI.sStuckOnNodeCount - 1);
            if (carAI.sStuckOnNodeCount < 0)
                carAI.sStuckOnNodeCount = 0;
        }
        const ptr32<_sAINode> previous = carAI.pPrevValidNode;
        // Moving back to an earlier node adds to the count of frames the car has been stuck.
        if (previous && previous != current && current->iFinishDist < previous->iFinishDist)
        {
            if (0x6e < carAI.sStuckOnNodeCount)
                carAI.sStuckOnNodeCount = 0x6e;
            carAI.sStuckOnNodeCount = static_cast<int16>(carAI.sStuckOnNodeCount + 0x14);
        }
        carAI.pPrevValidNode = current;
        carAI.pCurNode = AINodeUpdateCurrent(pObj);
        current = carAI.pCurNode;
        if (!current && !computerDriven(*pObj))
        {
            carAI.pCurNode = carAI.pPrevValidNode;
            current = carAI.pCurNode;
        }
    }
    if (!current)
    {
        carAI.pCurNode = carAI.pPrevValidNode;
        return;
    }
    if (current->priority == 10)
        carAI.iHalfLapDone = 1;
    guest_call(kAINodeUpdateFinishDistance, pObj);
}

// Resets every car's race state before the start.
void AISetupCars()
{
    sObjectClass &objects = G(objectVarG);
    sAIClass &ai = G(aiVarG);
    for (uint32_t index = 0; index < objects.noObjects; ++index)
    {
        const ptr32<_sObject> pObj = objects.list[index];
        _sObject &obj = *pObj;
        _sCar_AI &carAI = obj.carAI;
        _sFPhysLibCar &physics = *obj.pPhysics;
        carAI.iLapsCompleted = -1;
        carAI.iHalfLapDone = 1;
        carAI.pCurNode = nullptr;
        carAI.pPrevValidNode = nullptr;
        carAI.iGettingLost = 0;
        carAI.iGoingWrongWay = 0;
        carAI.iFinishDist = 0;
        carAI.iTotalRacedDist = 0;
        carAI.iLineClockOver = 0;
        carAI.bOvertaking = 0;
        for (int lap = 8; lap >= 0; --lap)
            obj.iLapTimes[lap] = 0;
        physics.inputEngineThrottle = 0;
        physics.inputSteerAngle = 0;
        carAI.bAIState = 0;
        carAI.pNearestObjInFront = nullptr;
        carAI.pNearestObjBehind = nullptr;
        obj.iBestLapTime = 0x10000000;
        carAI.sStuckOnNodeCount = 0x6e;
        ai.raceOrder[index].pObject = pObj;
        carAI.iTimeFinishedRace = 0;
        carAI.sAIStateCount = 0;
        carAI.iSlowSpeedAccum = 0;
        carAI.pStuckOnNode = nullptr;
        ai.raceOrder[index].modelType = 0;
        AIUpdateObjectNode(pObj);
    }
    ai.player2CameIn = 0;
    ai.player1CameIn = 0;
}

// Steering correction from the track's edges: positive towards the right edge when the car is near the left one,
// and the other way. Only on nodes without a fork. The edge directions are recomputed when the car reaches a new node.
int32 AIAdjustForTrackEdges(ptr32<_sObject> pObj)
{
    _sCar_AI &carAI = pObj->carAI;
    const ptr32<_sAINode> nodePtr = carAI.pCurNode;
    const _sAINode &node = *nodePtr;
    if (node.next[1])
        return 0;
    if (nodePtr != carAI.pPrevValidNode)
    {
        const _sAINode &next = *node.next[0];
        carAI.svRightEdge.vy = 0;
        carAI.svRightEdge.vx = static_cast<int16>(next.node[0].vx - node.node[0].vx);
        carAI.svRightEdge.vz = static_cast<int16>(next.node[0].vz - node.node[0].vz);
        carAI.svLeftEdge.vy = 0;
        carAI.svLeftEdge.vx = static_cast<int16>(next.node[1].vx - node.node[1].vx);
        carAI.svLeftEdge.vz = static_cast<int16>(next.node[1].vz - node.node[1].vz);
        vecNormalise(carAI.svRightEdge, carAI.svRightEdge);
        vecNormalise(carAI.svLeftEdge, carAI.svLeftEdge);
    }
    const SVECTOR &pos = *AIN_CarPositionPoi(pObj);
    const int16_t along = static_cast<int16_t>(carAI.iAlongNodeDist);
    // How close the car is to each edge, at its distance along the node: 240 at the edge, 0 from 240*16 units away.
    const auto nearness = [&](const SVECTOR &edgeDir, const SVECTOR &edgeStart)
    {
        sVec16 toEdge;
        vec16MulScaler(toEdge, asVec16(edgeDir), along);
        toEdge.x = static_cast<int16>(toEdge.x + (edgeStart.vx - pos.vx));
        toEdge.y = static_cast<int16>(toEdge.y + (edgeStart.vy - pos.vy));
        toEdge.z = static_cast<int16>(toEdge.z + (edgeStart.vz - pos.vz));
        const uint32_t d = vec16LengthSquared(toEdge) >> 8;
        return d < 0xf0 ? static_cast<int32_t>(0xf0 - d) : 0;
    };
    const int32_t left = nearness(carAI.svLeftEdge, node.node[1]);
    const int32_t right = nearness(carAI.svRightEdge, node.node[0]);
    return (left - right) >> 2;
}

// PLAYER.c

// Brake lights: on while braking, reversing against the car's motion or throttling hard against it, and when
// slowing from speed with little throttle.
void PlayerDetectBraking(ptr32<_sObject> pObject)
{
    _sObject &obj = *pObject;
    const _sFPhysLibCar &car = *obj.pPhysics;
    bool on = false;
    if (car.mode == 1)
    {
        const int16_t throttle = car.inputEngineThrottle;
        const ee_float forward = (car.mat.Xz * car.vel.x + car.mat.Yz * car.vel.y + car.mat.Zz * car.vel.z) * ee_float(0.00024414062f);
        const ee_float speed = float_from_bits(float_bits(forward) & 0x7FFFFFFFu);
        const ee_float threshold = G(speedyThreshold);
        if (car.inputBrake != 0)
            on = true;
        else if (throttle < -1 && 1.0f < forward.v)
            on = true;
        else if (1 < throttle && forward.v < -1.0f)
            on = true;
        else if (threshold.v < speed.v && throttle < 0x50)
            on = true;
        else if (speed.v < (-threshold).v)
            on = -0x50 < throttle;
    }
    obj.lightsOn = static_cast<int8>(on ? obj.lightsOn | 1 : obj.lightsOn & 0xfe);
}

namespace {

// Raises a pad's motors to these levels if they are lower, and marks the pad to be sent the new values. The player
// number is not checked, so the entries are addressed as the original does: past the two pads, the writes land in
// the neighbouring fields, and the compiler must not assume they don't overlap.
void raiseVibration(uint32_t player, uint8 left, uint8 right)
{
    const uint32_t input = guest_addr(&G(inputVarG));
    uint8 *motors = static_cast<uint8 *>(guest_host(input + offsetof(inputClass, userShockVals) + player * sizeof(sMotors)));
    const uint32_t signal = input + offsetof(inputClass, signalSent) + player * 4u;
    const auto clearSignal = [&] { std::memset(guest_host(signal), 0, 4); };
    if (motors[0] < right)
    {
        motors[0] = right;
        clearSignal();
    }
    if (motors[1] < left)
    {
        motors[1] = left;
        clearSignal();
        if (0x20 < motors[1]) // Read back: clearing the signal may have overwritten it.
            motors[1] = 0x20;
    }
}

} // namespace

// Rumble from the forces on the car: big impulses shake both motors, smaller ones the left; wheels in the air
// (flags 0xe0) add a little. Levels fade each frame, and the rumble stops after 150 frames of continuous shaking
// until the car has been still for 25.
void PlayerPhysicsVibrations(ptr32<_sObject> pObject)
{
    const uint32_t player = pObject->playerNo;
    const _sFPhysLibCar &car = *pObject->pPhysics;
    // Per-player counters, addressed as the original does (see raiseVibration).
    const auto counter = [player](int32_t (&array)[2]) -> int32_t & { return *static_cast<int32_t *>(guest_host(guest_addr(&array) + player * 4u)); };
    int32_t &left = counter(G(vibLeft));
    int32_t &right = counter(G(vibRight));
    left = left < 3 ? 0 : left - 3;
    right = right < 1 ? 0 : right - 1;
    const ee_float x = car.impulse.x, y = car.impulse.y, z = car.impulse.z;
    const int32_t force = ee_ftoi(ee_sqrt(x * x + y * y + z * z));
    if (force < 0xfa1)
    {
        if (400 < force)
            left += force / 100;
    }
    else
    {
        const int32_t oldRight = right;
        left += 0x28;
        right = oldRight + 5;
    }
    if ((car.flags & 0xe0) != 0)
        left += 3;
    int32_t level = left;
    if (0x28 < level)
    {
        level = 0x28;
        left = 0x28;
    }
    if (0xe < right)
        right = 0xe;
    if (level == 0 && right == 0)
    {
        if (0x19 < ++counter(G(noVibCount)))
            counter(G(vibCount)) = 0;
    }
    else
    {
        ++counter(G(vibCount));
        counter(G(noVibCount)) = 0;
    }
    if (counter(G(vibCount)) < 0x96)
        raiseVibration(player, static_cast<uint8>(left), static_cast<uint8>(right));
}

void PlayerSetVibrate(uint8 vibLeft, uint8 vibRight, uint8 playerNum)
{
    raiseVibration(playerNum, vibLeft, vibRight);
}

// Turns a stick or button into a steering value from -127 to 127. A stick pushed past 60 switches the control to
// analogue (*data = 1), which reads the stick through a response curve until a button is pressed; buttons move the
// value 15 a frame towards their side (10 once past it) and let it return to the centre by 15 a frame.
int32_t ProcessControlInput(ptr32<int16> inputInOut, int32_t analogueIn, int32_t digitalIn, ptr32<int8> data)
{
    int32_t analogue = *data;
    int32_t value = 0;
    if (analogue != 0)
    {
        if (digitalIn == 0)
        {
            // Indexed as the original does, with no bounds: the address wraps like any guest address.
            const uint32_t magnitude = analogueIn < 0 ? 0u - static_cast<uint32_t>(analogueIn) : static_cast<uint32_t>(analogueIn);
            const int32_t curved = *static_cast<int8_t *>(guest_host(guest_addr(&G(curvedata)) + magnitude));
            value = analogueIn < 0 ? -curved : curved;
        }
        else
        {
            *data = 0;
            analogue = 0;
        }
    }
    if (analogue == 0)
    {
        const int32_t current = *inputInOut;
        if (digitalIn < 0)
            value = current < -0xe ? current - 10 : current - 0xf;
        else if (digitalIn == 0)
            value = current >= 0x10 ? current - 0xf : current > -0x10 ? 0 : current + 0xf;
        else
            value = current < 0xf ? current + 0xf : current + 10;
        if (0x3c < (analogueIn < 0 ? static_cast<int32_t>(0u - static_cast<uint32_t>(analogueIn)) : analogueIn))
        {
            analogue = 1;
            *data = 1;
        }
    }
    if (0x7f < value)
        value = 0x7f;
    if (value < -0x7f)
        value = -0x7f;
    *inputInOut = static_cast<int16>(value);
    return analogue;
}

// In a two-player race the player in front loses up to 4/64 of their throttle, by how far they lead.
int32 PlayerCatchup(ptr32<_sObject> pObj, int32 i)
{
    const sObjectClass &objects = G(objectVarG);
    const ptr32<_sObject> other = objects.list[0] == pObj ? objects.list[1] : objects.list[0];
    int32_t lead = (pObj->carAI.iTotalRacedDist - other->carAI.iTotalRacedDist) >> 10;
    if (4 < lead)
        lead = 4;
    if (0 < lead && 100 < i)
        i = (i * (0x40 - lead)) >> 6;
    return i;
}

void PlayerTerm()
{
    InputSetVibrationFlags(INPUT_MULTITAP_1, 0);
    InputSetVibrationFlags(INPUT_MULTITAP_2, 0);
}

// The pad buttons for each control, by the controller layout chosen in the front end (0xff: not used).
void PlayerInit(int32 noPlayers)
{
    static const uint8 kPlayer1[4][12] = {
        {7, 5, 14, 15, 11, 13, 12, 10, 0, 4, 6, 8},
        {0xff, 0xff, 0xff, 0xff, 11, 9, 10, 8, 0, 4, 6, 12},
        {7, 5, 14, 15, 11, 9, 12, 10, 0, 4, 6, 8},
        {0xff, 0xff, 0xff, 0xff, 12, 14, 15, 10, 0, 4, 6, 11},
    };
    static const uint8 kPlayer2[4][12] = {
        {0x17, 0x15, 0x1e, 0x1f, 0x1b, 0x1d, 0x1c, 0x1a, 0x10, 0x14, 0x16, 0x18},
        {0xff, 0xff, 0xff, 0xff, 0x1b, 0x19, 0x1a, 0x18, 0x10, 0x14, 0x16, 0x1c},
        {0x17, 0x15, 0x1e, 0x1f, 0x1b, 0x19, 0x1c, 0x1a, 0x10, 0x14, 0x16, 0x18},
        {0xff, 0xff, 0xff, 0xff, 0x1c, 0x1e, 0x1f, 0x1a, 0x10, 0x14, 0x16, 0x1b},
    };
    sPlayerClass &players = G(playerVarG);
    const sFrontEndClass &frontEnd = G(frontEndVarG);
    for (int p = 0; p < 2; ++p)
    {
        // Layout 4 is the same as layout 0; any other value leaves the keys alone.
        const uint32_t layout = static_cast<uint32_t>(frontEnd.gameStateData.playerConfig[p].controllerConfig);
        if (layout < 5)
            std::memcpy(players.player[p].keys, (p == 0 ? kPlayer1 : kPlayer2)[layout == 4 ? 0 : layout], 12);
    }
    players.noPlayers = noPlayers;
    InputSetVibrationFlags(INPUT_MULTITAP_1, frontEnd.gameStateData.playerConfig[0].vibrationOnOff == 0 ? 1 : 0);
    if (noPlayers == 2)
        InputSetVibrationFlags(INPUT_MULTITAP_2, frontEnd.gameStateData.playerConfig[1].vibrationOnOff == 0 ? 1 : 0);
}

} // namespace rc

RC_DECOMP_FN_SETUP(VfxAreWeInCar, 0x16a330, rc::fuzz::aiCar)
RC_DECOMP_FN_SETUP(VfxCarPositionPoi, 0x169ff8, rc::fuzz::aiCar)
RC_DECOMP_FN_SETUP(VfxCarMatrixPoi, 0x16a198, rc::fuzz::aiCar)
RC_DECOMP_FN_SETUP(VfxCarVelocityPoi, 0x16a298, rc::fuzz::aiCar)
RC_DECOMP_FN(InputSetVibrationFlags, 0x191c20)
RC_DECOMP_FN(InputHeld, 0x1917f0)
RC_DECOMP_FN(InputPressureHit, 0x1915e8)
RC_DECOMP_FN_SETUP(AIN_CarPositionPoi, 0x103c20, rc::fuzz::aiCar)
RC_DECOMP_FN_SETUP(AIN_CarPositionVal, 0x103c58, rc::fuzz::aiCar)
RC_DECOMP_FN_SETUP(AIN_CarMatrixPoi, 0x103cf8, rc::fuzz::aiCar)
RC_DECOMP_FN_SETUP(AIN_CarMatrixVal, 0x103d50, rc::fuzz::aiMatrixAndCar)
RC_DECOMP_FN_SETUP(AINodeNearest, 0x102280, rc::fuzz::aiCar)
RC_DECOMP_FN_SETUP(AINodeNearestInRange, 0x102428, rc::fuzz::aiCar)
RC_DECOMP_FN_SETUP(AINodeInAnyNode, 0x102750, rc::fuzz::aiCar)
RC_DECOMP_FN_SETUP(AINodeDistanceAlong, 0x102e48, rc::fuzz::aiCar)
RC_DECOMP_FN_SETUP(AINodeGetReSpawnNode, 0x1036c0, rc::fuzz::aiCar)
RC_DECOMP_FN_SETUP(AINodeGetNearestHeading, 0x1025f8, rc::fuzz::aiPoint)
RC_DECOMP_FN_SETUP(AINodeUpdateRacingLineAhead, 0x103430, rc::fuzz::aiCar)
RC_DECOMP_FN_SETUP(AINodeRouteFinder, 0x1038b8, rc::fuzz::aiCar)
RC_DECOMP_FN(AINodeTerm, 0x1016b8)
RC_DECOMP_FN(srand, 0x1b8808)
RC_DECOMP_FN(rand, 0x1b8818)
RC_DECOMP_FN(VecNormalise, 0x1a0390)
RC_DECOMP_FN(AINodeInit, 0x103b78)
RC_DECOMP_FN_NOFUZZ(AINodeConstructDaemon, 0x1016c8)
RC_DECOMP_FN_NOFUZZ(AINodeCreateLinkInfo, 0x101960)
RC_DECOMP_FN_NOFUZZ(AINodeSetupClass, 0x103bb8)
RC_DECOMP_FN_SETUP_HEAP(AINodeUpdateRecursive, 0x102b80, rc::fuzz::aiSearch)
RC_DECOMP_FN_SETUP_HEAP(AINodeBackTrackRecursive, 0x102a40, rc::fuzz::aiSearch)
RC_DECOMP_FN_SETUP_HEAP(AINodeForwardTrackRecursive, 0x102ae8, rc::fuzz::aiSearch)
RC_DECOMP_FN_SETUP(AINodeChooseNodeRoute, 0x103598, rc::fuzz::aiCarAndFork)
RC_DECOMP_FN_SETUP_HEAP(AINodeUpdateCurrent, 0x1028e0, rc::fuzz::aiCar)
RC_DECOMP_FN_SETUP(AINodeUpdateAngleToRacingLine, 0x103de8, rc::fuzz::aiCar)
RC_DECOMP_FN(AIInit, 0x1014b0)
RC_DECOMP_FN_SETUP(AIAdjustForTooCloseToCarAhead, 0x101380, rc::fuzz::aiCar)
RC_DECOMP_FN_SETUP(AIPlayCatchup, 0x1015c0, rc::fuzz::aiCar)
RC_DECOMP_FN_SETUP_HEAP(AIUpdateObjectNode, 0x100a40, rc::fuzz::aiCar)
RC_DECOMP_FN_NOFUZZ(AISetupCars, 0x1004a0)
RC_DECOMP_FN_SETUP(AIAdjustForTrackEdges, 0x101198, rc::fuzz::aiCar)
RC_DECOMP_FN_SETUP(PlayerDetectBraking, 0x13f940, rc::fuzz::aiCar)
RC_DECOMP_FN_SETUP(PlayerPhysicsVibrations, 0x140348, rc::fuzz::aiCar)
RC_DECOMP_FN(PlayerSetVibrate, 0x140588)
RC_DECOMP_FN(ProcessControlInput, 0x140608)
RC_DECOMP_FN_SETUP(PlayerCatchup, 0x1407f8, rc::fuzz::aiCar)
RC_DECOMP_FN(PlayerTerm, 0x13f918)
RC_DECOMP_FN(PlayerInit, 0x13f628)

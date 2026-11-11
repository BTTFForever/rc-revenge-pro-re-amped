// Inputs for the random test of the route node code (RC_SHADOW_FUZZ). The node functions follow pointers from a car
// to its physics, task and nodes, so random bytes would be meaningless. These copy one of the race's real cars into
// the test's scratch area, place it near a random node of the loaded level (sometimes far from any), and point its
// route fields at real nodes. They do nothing until a level with nodes is loaded.

#include "ai_fuzz.h"

namespace rc::fuzz {

namespace {

constexpr uint32_t kObject = 0x0000u, kPhysics = 0x1000u, kInteger = 0x2000u, kTask = 0x2100u, kPoint = 0x2200u;
static_assert(sizeof(_sObject) <= kPhysics - kObject);
static_assert(sizeof(_sFPhysLibCar) <= kInteger - kPhysics);
static_assert(sizeof(_sOPhysLibIntegerCarState) <= kTask - kInteger);
static_assert(sizeof(_sTask) <= kPoint - kTask);

constexpr uint32_t kAIControl = 0x1000c0u;

struct Rng
{
    uint64_t &state;

    uint32_t next() { return nextFuzz(state); }
    int below(int count) { return static_cast<int>(next() % static_cast<uint32_t>(count)); }
    bool chance(int percent) { return below(100) < percent; }
    int between(int low, int high) { return low + below(high - low + 1); }
};

void setArg(R5900Context *ctx, int index, uint32_t value)
{
    SET_GPR_S32(ctx, 4 + index, static_cast<int32_t>(value));
}

bool levelReady()
{
    const sAINodeClass &ai = G(aiNodeVarG);
    if (!ai.pNode || ai.noNodesSingle <= 0 || !ai.pRecurseSCR)
        return false;
    for (uint32_t i = 0; i < G(objectVarG).noObjects; ++i)
        if (G(objectVarG).list[i])
            return true;
    return false;
}

ptr32<_sAINode> randomNode(Rng &rng)
{
    const sAINodeClass &ai = G(aiNodeVarG);
    return ai.pNode + rng.below(ai.noNodesSingle);
}

// A node with a fork, if the level has one within a few tries.
ptr32<_sAINode> randomFork(Rng &rng)
{
    ptr32<_sAINode> node = randomNode(rng);
    for (int tries = 0; tries < 64 && !node->next[1]; ++tries)
        node = randomNode(rng);
    return node;
}

// A point in or near a node: usually within its box, sometimes anywhere near it, now and then far away.
SVECTOR pointNear(Rng &rng, const _sAINode &node)
{
    SVECTOR p{};
    const int kind = rng.below(10);
    if (kind < 6)
    {
        p.vx = static_cast<int16>(rng.between(node.boundsMin[0], node.boundsMax[0]));
        p.vz = static_cast<int16>(rng.between(node.boundsMin[1], node.boundsMax[1]));
    }
    else if (kind < 9)
    {
        p.vx = static_cast<int16>(node.centre.vx + rng.between(-3000, 3000));
        p.vz = static_cast<int16>(node.centre.vz + rng.between(-3000, 3000));
    }
    else
    {
        p.vx = static_cast<int16>(rng.next());
        p.vz = static_cast<int16>(rng.next());
    }
    p.vy = static_cast<int16>(node.centre.vy + rng.between(-500, 500));
    p.pad = static_cast<int16>(rng.next());
    return p;
}

// One of the race's cars, copied into the scratch area with its physics and task, placed near a random node.
uint32_t buildCar(Rng &rng, uint32_t scratch)
{
    const sObjectClass &objects = G(objectVarG);
    ptr32<_sObject> source = nullptr;
    while (!source)
        source = objects.list[rng.below(static_cast<int>(objects.noObjects))];

    _sObject &car = *static_cast<_sObject *>(guest_host(scratch + kObject));
    car = *source;
    _sFPhysLibCar &physics = *static_cast<_sFPhysLibCar *>(guest_host(scratch + kPhysics));
    physics = *source->pPhysics;
    _sOPhysLibIntegerCarState &state = *static_cast<_sOPhysLibIntegerCarState *>(guest_host(scratch + kInteger));
    state = *physics.pInteger;
    _sTask &task = *static_cast<_sTask *>(guest_host(scratch + kTask));
    task = *source->pTask;
    car.pPhysics = ptr32<_sFPhysLibCar>(scratch + kPhysics);
    physics.pInteger = ptr32<_sOPhysLibIntegerCarState>(scratch + kInteger);
    car.pTask = ptr32<_sTask>(scratch + kTask);
    if (rng.chance(50))
        task.pFnCallback.address = kAIControl;

    const ptr32<_sAINode> home = randomNode(rng);
    const SVECTOR at = pointNear(rng, *home);
    state.pos.x = (at.vx << 8) + rng.below(256);
    state.pos.y = (at.vy << 8) + rng.below(256);
    state.pos.z = (at.vz << 8) + rng.below(256);

    _sCar_AI &ai = car.carAI;
    ai.pCurNode = rng.chance(10) ? ptr32<_sAINode>(nullptr) : rng.chance(70) ? home : randomNode(rng);
    ai.pPrevValidNode = rng.chance(70) ? home : randomNode(rng);
    ai.pStuckOnNode = rng.chance(25) ? randomNode(rng) : ptr32<_sAINode>(nullptr);
    ai.bOvertaking = static_cast<uint8>(rng.below(2));
    ai.iAlongNodeDist = rng.between(-3000, 6000);
    ai.iGettingLost = rng.between(0, 5);
    ai.svLookingPoint = pointNear(rng, *home);
    car.playerNo = static_cast<uint8>(rng.below(8));
    car.iRacePos = static_cast<int16>(rng.below(8));
    car.weaponsHeld = static_cast<int8>(rng.below(3));
    static const uint8 kWeapons[] = {0, 1, 2, 4, 5, 8, 11};
    car.weaponType = kWeapons[rng.below(7)];
    car.weaponFiring = rng.chance(80) ? 0 : 1;
    car.respawn = rng.chance(90) ? 0 : 1;
    car.aiFire = rng.below(2);
    car.weaponLock = static_cast<uint8>(rng.below(2));
    G(aiNodeVarG).iNodeSearcher = rng.below(G(aiNodeVarG).noNodesSingle);
    return scratch + kObject;
}

} // namespace

void aiCar(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!levelReady())
        return;
    Rng rng{state};
    setArg(ctx, 0, buildCar(rng, scratch));
    setArg(ctx, 1, static_cast<uint32_t>(rng.between(-2000, 9000))); // A distance ahead, for the functions that take one.
    setArg(ctx, 2, static_cast<uint32_t>(rng.between(0, 32)));       // AINodeRouteFinder's starting count.
}

void aiCarAndFork(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!levelReady())
        return;
    Rng rng{state};
    setArg(ctx, 0, buildCar(rng, scratch));
    G(frontEndVarG).cup = rng.between(0, 9);
    setArg(ctx, 1, randomFork(rng).address);
}

void aiMatrixAndCar(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!levelReady())
        return;
    Rng rng{state};
    setArg(ctx, 1, buildCar(rng, scratch));
    setArg(ctx, 0, scratch + kPoint);
}

void aiPoint(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!levelReady())
        return;
    Rng rng{state};
    *static_cast<SVECTOR *>(guest_host(scratch + kPoint)) = pointNear(rng, *randomNode(rng));
    setArg(ctx, 0, scratch + kPoint);
}

// The search state AINodeUpdateCurrent sets up before walking the route, for testing the walkers on their own.
void aiSearch(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!levelReady())
        return;
    Rng rng{state};
    const uint32_t car = buildCar(rng, scratch);
    const _sObject &object = *static_cast<_sObject *>(guest_host(car));
    _sRecurseSCR &scr = *G(aiNodeVarG).pRecurseSCR;
    const ptr32<_sAINode> start = object.carAI.pCurNode ? object.carAI.pCurNode : randomNode(rng);
    *static_cast<SVECTOR *>(guest_host(scratch + kPoint)) = pointNear(rng, *start);
    scr.pPos = ptr32<SVECTOR>(scratch + kPoint);
    scr.pNearestNode = rng.chance(20) ? randomNode(rng) : ptr32<_sAINode>(nullptr);
    scr.iDistance = rng.chance(80) ? 0x10000000 : rng.between(0, 4000000);
    scr.pObj = ptr32<_sObject>(car);
    scr.iObjBit = 1 << object.playerNo;
    scr.iType = rng.below(2);
    scr.iRange = rng.between(1, 16);
    setArg(ctx, 0, start.address);
    setArg(ctx, 1, static_cast<uint32_t>(rng.between(1, 16)));
    setArg(ctx, 2, static_cast<uint32_t>(rng.between(1, 2)));
}

} // namespace rc::fuzz

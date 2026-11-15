// Builds the pickups' random-test inputs. See pickup_fuzz.h.

#include "pickup_fuzz.h"

#include <vector>

namespace rc::fuzz {

namespace {

constexpr uint32_t kPickupVars = 0x1ee738u, kPickups = kPickupVars + 0x40u, kPickupPos = kPickupVars + 0x44u;
constexpr uint32_t kNumPickups = kPickupVars + 0x48u, kLastUpdated = kPickupVars + 0x4cu, kUltimates = kPickupVars + 0x50u;
constexpr uint32_t kMainVars = 0x1ed900u, kMainTime = kMainVars + 0x1cu, kMainLevel = kMainVars + 0x34au;
constexpr uint32_t kPauseMenu = kMainVars + 0x366u, kPickupsOnOff = kMainVars + 0x3a8u;
constexpr uint32_t kVisMask0 = 0x1f48d8u, kVisMask1 = 0x1f48dcu, kCameraPtr = 0x1eab20u;
constexpr uint32_t kObjectList = 0x1ee390u, kNoObjects = 0x1ee500u, kRaceOrder = 0x1e9404u, kLevelPtr = 0x327e80u;
constexpr uint32_t kRandom = 0x328318u, kSfxInitOk = 0x31e488u;

// Scratch layout.
constexpr uint32_t kPositions = 0x0000u, kPickup = 0x1000u, kTables = 0x1200u, kLevelTable = 0x1800u;

template <typename T>
T &mem(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

struct Rng
{
    uint64_t &state;
    uint32_t next() { return nextFuzz(state); }
    uint32_t below(uint32_t n) { return n ? next() % n : 0u; }
    bool chance(uint32_t percent) { return below(100) < percent; }
    float range(float low, float high) { return low + (high - low) * (static_cast<float>(next() >> 8) / 16777216.0f); }
};

void setArg(R5900Context *ctx, int index, uint32_t value) { SET_GPR_S32(ctx, 4 + index, static_cast<int32_t>(value)); }

bool inRam(uint32_t address) { return address != 0 && address < 0x2000000u; }

// Object types: the sixteen with a special weapon often, any other now and then.
uint32_t objectType(Rng &rng)
{
    static const uint8_t kSpecial[] = {2, 0x15, 3, 0x16, 0xb, 0x1e, 0xe, 0x21, 0x10, 0x23, 0x11, 0x24, 0x12, 0x25, 0x13, 0x26};
    if (rng.chance(60))
        return kSpecial[rng.below(sizeof(kSpecial))];
    return rng.chance(90) ? rng.below(0x2a) : rng.next();
}

void seed(Rng &rng)
{
    mem<uint32_t>(kRandom) = rng.next();
    mem<uint32_t>(kRandom + 4u) = rng.next();
    mem<uint8_t>(kSfxInitOk) = 0; // an original that plays a sound waits for the IOP offline
}

// The race's pickup list, as far as numPickups and the links go.
std::vector<uint32_t> pickupList()
{
    std::vector<uint32_t> list;
    uint32_t p = mem<uint32_t>(kPickups);
    const uint32_t count = mem<uint32_t>(kNumPickups);
    while (inRam(p) && list.size() < count && list.size() < 200)
    {
        list.push_back(p);
        p = mem<uint32_t>(p);
    }
    return list;
}

int32_t timeNear(Rng &rng) { return static_cast<int32_t>(mem<uint32_t>(kMainTime)) + static_cast<int32_t>(rng.below(0x3000)) - 0x800; }

uint32_t usableCar(Rng &rng)
{
    const uint32_t count = mem<uint32_t>(kNoObjects);
    for (int tries = 0; tries < 30 && count; ++tries)
    {
        const uint32_t car = mem<uint32_t>(kObjectList + 4u * rng.below(count));
        if (inRam(car) && inRam(mem<uint32_t>(car + 0xcu)) && inRam(mem<uint32_t>(car + 0x10u)))
            return car;
    }
    return mem<uint32_t>(kObjectList);
}

// The weapon tables: the level's, or scratch ones with random counts and entries (9, the ultimate, often; signed bytes).
void varyTables(Rng &rng, uint32_t scratch)
{
    for (uint32_t k = 0; k < 16; ++k)
    {
        const uint32_t slot = kPickupVars + k * 4u;
        if (!inRam(mem<uint32_t>(slot)) || rng.chance(70))
        {
            const uint32_t table = scratch + kTables + k * 0x20u;
            const uint32_t n = 1 + rng.below(12);
            mem<uint8_t>(table) = static_cast<uint8_t>(n);
            for (uint32_t i = 1; i < 0x20; ++i)
                mem<uint8_t>(table + i) = static_cast<uint8_t>(rng.chance(20) ? 9u : rng.chance(50) ? (0x80u | rng.next()) : 1u + rng.below(19));
            mem<uint32_t>(slot) = table;
        }
        else if (mem<uint8_t>(mem<uint32_t>(slot)) == 0)
            mem<uint8_t>(mem<uint32_t>(slot)) = static_cast<uint8_t>(1 + rng.below(5)); // a count of 0 traps
    }
}

void varyPickup(Rng &rng, uint32_t p)
{
    if (rng.chance(60))
        mem<uint8_t>(p + 0xa8u) = static_cast<uint8_t>(rng.chance(80) ? 1u + rng.below(3) : rng.below(256));
    if (rng.chance(50))
        mem<uint8_t>(p + 0xa9u) = static_cast<uint8_t>(rng.chance(80) ? rng.below(2) : rng.next());
    if (rng.chance(40))
        mem<int32_t>(p + 0xacu) = timeNear(rng);
    if (rng.chance(30))
        mem<uint16_t>(p + 0xaau) = static_cast<uint16_t>(rng.chance(60) ? 0u : rng.below(20));
    if (rng.chance(30))
        mem<uint32_t>(p + 4u) = rng.chance(50) ? 0u : rng.next();
    if (rng.chance(30))
        mem<uint32_t>(p + 8u) = rng.chance(50) ? 0u : rng.next();
}

} // namespace

void pickupNone(R5900Context *, uint64_t &state, uint32_t)
{
    Rng rng{state};
    seed(rng);
}

void pickupSpawn(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    seed(rng);
    const uint32_t real = mem<uint32_t>(kPickupPos);
    const uint32_t realCount = mem<uint32_t>(kNumPickups);
    if (!inRam(real) || rng.chance(50))
    {
        // Positions in scratch: copies of the level's, moved, or anywhere (so some are over no ground).
        const uint32_t count = rng.below(9);
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t at = scratch + kPositions + i * 0x10u;
            if (inRam(real) && realCount && rng.chance(70))
            {
                const uint32_t from = real + 0x10u * rng.below(realCount);
                for (uint32_t k = 0; k < 0x10; k += 4)
                    mem<uint32_t>(at + k) = mem<uint32_t>(from + k);
                mem<int16_t>(at + 8u) = static_cast<int16_t>(mem<int16_t>(at + 8u) + static_cast<int16_t>(rng.below(200)) - 100);
                mem<int16_t>(at + 0xcu) = static_cast<int16_t>(mem<int16_t>(at + 0xcu) + static_cast<int16_t>(rng.below(200)) - 100);
            }
            else
            {
                for (uint32_t k = 0; k < 0x10; k += 4)
                    mem<uint32_t>(at + k) = rng.next();
            }
        }
        mem<uint32_t>(kPickupPos) = scratch + kPositions;
        mem<uint32_t>(kNumPickups) = count;
    }
    else
        mem<uint32_t>(kNumPickups) = rng.below(realCount + 1);
    if (rng.chance(30))
        mem<uint8_t>(kMainLevel) = 0x61;
    setArg(ctx, 0, static_cast<uint32_t>(timeNear(rng)));
}

void pickupOne(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    seed(rng);
    const std::vector<uint32_t> list = pickupList();
    uint32_t p = scratch + kPickup;
    if (!list.empty())
        p = list[rng.below(static_cast<uint32_t>(list.size()))];
    varyPickup(rng, p);
    const int32_t respawn = mem<int32_t>(p + 0xacu);
    setArg(ctx, 0, p);
    setArg(ctx, 1, static_cast<uint32_t>(rng.chance(60) ? respawn + static_cast<int32_t>(rng.below(3)) - 1 : timeNear(rng)));
}

void pickupRender(R5900Context *ctx, uint64_t &state, uint32_t)
{
    Rng rng{state};
    seed(rng);
    const std::vector<uint32_t> list = pickupList();
    for (const uint32_t p : list)
        varyPickup(rng, p);
    mem<uint32_t>(kNumPickups) = rng.below(static_cast<uint32_t>(list.size()) + 1);
    if (rng.chance(20))
        mem<int32_t>(kPickupsOnOff) = 0;
    else if (rng.chance(30))
        mem<int32_t>(kPickupsOnOff) = 1;
    if (rng.chance(30))
        mem<uint16_t>(kPauseMenu) = static_cast<uint16_t>(rng.below(2));
    if (rng.chance(40))
        mem<uint32_t>(kVisMask0) = rng.chance(30) ? 0u : rng.next();
    if (rng.chance(40))
        mem<uint32_t>(kVisMask1) = rng.chance(30) ? 0u : rng.next();
    // The camera brought near a pickup, so the distance test passes and fails on each axis.
    const uint32_t camera = mem<uint32_t>(kCameraPtr);
    if (inRam(camera) && !list.empty() && rng.chance(60))
    {
        const uint32_t p = list[rng.below(static_cast<uint32_t>(list.size()))];
        const float span = rng.chance(50) ? 15000.0f : 3000.0f;
        mem<int32_t>(camera + 0xcu) = static_cast<int32_t>((mem<float>(p + 0x78u) + rng.range(-span, span)) * 256.0f);
        mem<int32_t>(camera + 0x10u) = static_cast<int32_t>((mem<float>(p + 0x7cu) + rng.range(-span, span)) * 256.0f);
        mem<int32_t>(camera + 0x14u) = static_cast<int32_t>((mem<float>(p + 0x80u) + rng.range(-span, span)) * 256.0f);
    }
    if (rng.chance(30))
        mem<int32_t>(kLastUpdated) = timeNear(rng);
    setArg(ctx, 0, static_cast<uint32_t>(rng.chance(80) ? mem<int32_t>(kLastUpdated) + static_cast<int32_t>(rng.below(100)) : timeNear(rng)));
}

void pickupUltimate(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    seed(rng);
    varyTables(rng, scratch);
    const uint32_t car = mem<uint32_t>(kObjectList);
    if (inRam(car) && rng.chance(50))
        mem<int32_t>(kUltimates) = mem<int16_t>(car + 0x104u) + static_cast<int32_t>(rng.below(3)) - 1;
    setArg(ctx, 0, rng.chance(80) ? 9u : rng.below(20));
    setArg(ctx, 1, rng.chance(50) ? 0u : rng.chance(80) ? 1u : rng.next());
}

void pickupSpecial(R5900Context *ctx, uint64_t &state, uint32_t)
{
    Rng rng{state};
    seed(rng);
    setArg(ctx, 0, objectType(rng));
}

void pickupGive(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    seed(rng);
    varyTables(rng, scratch);
    const std::vector<uint32_t> list = pickupList();
    uint32_t p = scratch + kPickup;
    if (!list.empty() && rng.chance(50))
        p = list[rng.below(static_cast<uint32_t>(list.size()))];
    mem<uint16_t>(p + 0xaau) = static_cast<uint16_t>(rng.chance(85) ? 0u : rng.below(20));
    const uint32_t count = mem<uint32_t>(kNoObjects);
    if (count >= 2 && rng.chance(30))
        mem<uint32_t>(kNoObjects) = 1 + rng.below(count); // a smaller race scales the position to 8 rows
    const uint32_t objects = mem<uint32_t>(kNoObjects);
    // The race order's cars with their types varied (a definition copy in scratch with any type).
    const uint32_t position = rng.below(objects ? objects : 1);
    if (rng.chance(70)) // the car's type varied (a definition copy in scratch)
    {
        const uint32_t car = mem<uint32_t>(kRaceOrder + 12u * position);
        if (inRam(car) && inRam(mem<uint32_t>(car + 0xcu)))
        {
            const uint32_t def = scratch + 0x1700u;
            for (uint32_t k = 0; k < 0x40; k += 4)
                mem<uint32_t>(def + k) = mem<uint32_t>(mem<uint32_t>(car + 0xcu) + k);
            mem<uint32_t>(def + 4u) = objectType(rng);
            mem<uint32_t>(car + 0xcu) = def;
        }
    }
    const uint32_t car = mem<uint32_t>(kObjectList);
    if (inRam(car) && rng.chance(50))
        mem<int32_t>(kUltimates) = mem<int16_t>(car + 0x104u);
    setArg(ctx, 0, p);
    setArg(ctx, 1, rng.chance(90) ? rng.below(4) : rng.next());
    setArg(ctx, 2, position);
}

void pickupRace(R5900Context *, uint64_t &state, uint32_t)
{
    Rng rng{state};
    seed(rng);
    const std::vector<uint32_t> list = pickupList();
    mem<uint32_t>(kNumPickups) = rng.below(static_cast<uint32_t>(list.size()) + 1);
    if (rng.chance(25))
        mem<int32_t>(kPickupsOnOff) = 0;
    else if (rng.chance(30))
        mem<int32_t>(kPickupsOnOff) = 1;
}

void pickupHit(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    seed(rng);
    varyTables(rng, scratch);
    const std::vector<uint32_t> list = pickupList();
    uint32_t p = scratch + kPickup;
    if (!list.empty())
        p = list[rng.below(static_cast<uint32_t>(list.size()))];
    mem<uint16_t>(p + 0xaau) = static_cast<uint16_t>(rng.chance(80) ? 0u : rng.below(20));
    const uint32_t car = usableCar(rng);
    if (rng.chance(50))
        mem<uint32_t>(car + 0x194u) = rng.chance(50) ? 0u : (1u << (8 * rng.below(4)));
    if (rng.chance(40))
        mem<uint8_t>(car + 0x44u) = static_cast<uint8_t>(rng.below(4));
    if (rng.chance(15))
        mem<int32_t>(kMainVars) = 0x20;
    setArg(ctx, 0, car);
    setArg(ctx, 1, p);
}

void pickupLevel(R5900Context *, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    seed(rng);
    const uint32_t level = mem<uint32_t>(kLevelPtr);
    if (inRam(level) && rng.chance(50))
    {
        for (uint32_t k = 0; k < 0x80; k += 4)
            mem<uint32_t>(scratch + kLevelTable + k) = rng.next();
        mem<uint32_t>(level + 0x34u) = scratch + kLevelTable;
    }
}

} // namespace rc::fuzz

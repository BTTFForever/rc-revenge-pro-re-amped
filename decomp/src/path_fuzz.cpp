// Builds the camera paths' random-test inputs. See path_fuzz.h.

#include "path_fuzz.h"

#include <algorithm>
#include <vector>

namespace rc::fuzz {

namespace {

constexpr uint32_t kDetails = sym::pathDetailsG, kBuilt = kDetails + 0x24u, kBuiltTags = kDetails + 0x2cu;
constexpr uint32_t kFrontEnd = sym::frontEndVarG, kPathLinks = sym::pathLinks, kCameraPtr = (sym::cameraVarG + 0xb70u), kLevelPtr = sym::pLvlG;

// Scratch layout: the front-end level data, its camera list, path list and paths, a camera, a handle.
constexpr uint32_t kFELevel = 0x0000u, kCams = 0x0100u, kList = 0x0500u, kPaths = 0x0600u, kPathStride = 0x60u;
constexpr uint32_t kCamera = 0x2000u, kHandle = 0x2800u, kTagWords = 0x2900u;
constexpr int kMaxPaths = 40, kMaxCams = 40;

template <typename T>
T &mem(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

struct Rng
{
    uint64_t &state;
    uint32_t next() { return nextFuzz(state); }
    uint32_t below(uint32_t n) { return n ? next() % n : 0u; }
    bool chance(uint32_t percent) { return below(100) < percent; }
};

void setArg(R5900Context *ctx, int index, uint32_t value) { SET_GPR_S32(ctx, 4 + index, static_cast<int32_t>(value)); }

// The tags of this input: a few links of the game's table (from, to and both vias each), now and then a stray tag.
struct World
{
    std::vector<uint32_t> tags;
    uint32_t scratch = 0;
    int paths = 0;
    bool earlyZeros = true; // off for the path builder: a path cut short ends at another tag and the chain can cycle

    uint32_t tag(Rng &rng) const
    {
        if (rng.chance(4))
            return rng.chance(50) ? 0u : 0x10000000u + rng.below(0x300);
        return tags[rng.below(static_cast<uint32_t>(tags.size()))];
    }
    uint32_t path(int i) const { return scratch + kPaths + static_cast<uint32_t>(i) * kPathStride; }
};

uint32_t linkCount()
{
    uint32_t n = 0;
    while (mem<uint32_t>(kPathLinks + n * 16u) != 0)
        ++n;
    return n;
}

void writePath(Rng &rng, uint32_t at, uint32_t first, uint32_t last, const World &world)
{
    const uint32_t length = rng.chance(5) ? 0u : 1u + rng.below(6);
    mem<uint8_t>(at) = static_cast<uint8_t>(length);
    mem<uint8_t>(at + 1u) = static_cast<uint8_t>(rng.next());
    mem<uint16_t>(at + 2u) = static_cast<uint16_t>(rng.chance(20) ? 0u : rng.next());
    mem<uint32_t>(at + 4u) = rng.next();
    for (uint32_t i = 0; i < length; ++i)
        mem<uint32_t>(at + 8u + i * 4u) = i == 0 ? first : i + 1 == length ? last : world.tag(rng);
    mem<uint32_t>(at + 8u + length * 4u) = 0;
    if (length > 2 && world.earlyZeros && rng.chance(5))
        mem<uint32_t>(at + 8u + rng.below(length) * 4u) = 0; // an early end for the copies that stop at 0
}

World buildWorld(Rng &rng, uint32_t scratch, bool earlyZeros = true)
{
    World world;
    world.scratch = scratch;
    world.earlyZeros = earlyZeros;
    const uint32_t links = linkCount();
    const uint32_t groups = 1 + rng.below(4);
    for (uint32_t g = 0; g < groups && links; ++g)
    {
        const uint32_t link = kPathLinks + rng.below(links) * 16u;
        for (uint32_t k = 0; k < 4; ++k)
            if (mem<uint32_t>(link + k * 4u) != 0)
                world.tags.push_back(mem<uint32_t>(link + k * 4u));
    }
    if (world.tags.empty())
        world.tags.push_back(0x100000e2u);
    world.tags.push_back(0x100000adu);
    world.tags.push_back(0x100000dau);

    // The cameras: one per tag mostly, in random order, with random positions and angles.
    const int cams = rng.chance(5) ? static_cast<int>(rng.below(3)) - 2 : static_cast<int>(rng.below(kMaxCams));
    mem<int32_t>(scratch + kCams) = cams;
    for (int i = 0; i < std::max(cams, 0); ++i)
    {
        const uint32_t c = scratch + kCams + 4u + static_cast<uint32_t>(i) * 0x14u;
        mem<uint32_t>(c) = world.tag(rng);
        for (uint32_t k = 4; k < 0x14; k += 4)
            mem<uint32_t>(c + k) = rng.next();
    }

    world.paths = rng.chance(5) ? 0 : static_cast<int>(rng.below(kMaxPaths));
    for (int i = 0; i < world.paths; ++i)
    {
        uint32_t first = world.tag(rng), last = world.tag(rng);
        if (rng.chance(50) && links) // the leg of a link
        {
            const uint32_t link = kPathLinks + rng.below(links) * 16u;
            first = mem<uint32_t>(link + (rng.chance(50) ? 0u : 4u));
            last = mem<uint32_t>(link + (rng.chance(50) ? 8u : 12u));
            if (rng.chance(50))
                std::swap(first, last);
        }
        writePath(rng, world.path(i), first, last, world);
        mem<uint32_t>(scratch + kList + static_cast<uint32_t>(i) * 4u) = world.path(i);
    }
    mem<uint32_t>(scratch + kList + static_cast<uint32_t>(world.paths) * 4u) = 0;

    mem<uint32_t>(scratch + kFELevel + 0x30u) = scratch + kCams;
    mem<uint32_t>(scratch + kFELevel + 0x34u) = scratch + kList;
    mem<uint32_t>(kFrontEnd) = scratch + kFELevel;
    return world;
}

// The constructed path: random tags of the world (repeats common), its length byte and the byte above it.
void varyBuilt(Rng &rng, const World &world, uint32_t maxLength)
{
    const uint32_t length = rng.below(maxLength + 1);
    mem<uint8_t>(kBuilt) = static_cast<uint8_t>(length);
    mem<uint8_t>(kBuilt + 1u) = static_cast<uint8_t>(rng.chance(70) ? 0u : rng.below(3));
    mem<uint16_t>(kBuilt + 2u) = static_cast<uint16_t>(rng.chance(30) ? 0u : rng.next());
    mem<uint32_t>(kBuilt + 4u) = rng.next();
    const uint32_t few = 1 + rng.below(5); // a small alphabet makes loops
    for (uint32_t i = 0; i < length + 2; ++i)
    {
        uint32_t tag = rng.chance(50) ? world.tags[rng.below(std::min<uint32_t>(few, static_cast<uint32_t>(world.tags.size())))]
                                      : world.tag(rng);
        if (rng.chance(3))
            tag = 0xffffffffu;
        mem<uint32_t>(kBuiltTags + i * 4u) = i == length ? 0u : tag;
    }
}

void varyDetails(Rng &rng, const World &world)
{
    mem<uint32_t>(kDetails + 0x0u) = world.tag(rng);
    mem<uint32_t>(kDetails + 0x8u) = world.tag(rng);
    const uint32_t kind = rng.below(10);
    mem<uint32_t>(kDetails + 0x4u) = kind < 6 ? kBuilt : kind < 9 && world.paths ? world.path(static_cast<int>(rng.below(static_cast<uint32_t>(world.paths)))) : 0u;
}

} // namespace

void pathNone(R5900Context *, uint64_t &, uint32_t) {}

void pathCameras(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    const World world = buildWorld(rng, scratch);
    if (rng.chance(50))
        mem<uint32_t>(kCameraPtr) = scratch + kCamera;
    setArg(ctx, 0, rng.chance(90) ? world.tag(rng) : rng.next());
    setArg(ctx, 1, scratch + kCams);
}

void pathPitch(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    buildWorld(rng, scratch);
    setArg(ctx, 0, scratch + kCams);
}

void pathNode(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    const World world = buildWorld(rng, scratch);
    varyBuilt(rng, world, 30);
    varyDetails(rng, world);
    if (mem<uint32_t>(kDetails + 4u) == 0)
        mem<uint32_t>(kDetails + 4u) = kBuilt;
    setArg(ctx, 0, rng.next());
    setArg(ctx, 1, rng.below(rng.chance(90) ? 12u : 100u));
}

void pathCopy(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World world = buildWorld(rng, scratch);
    if (world.paths == 0)
    {
        writePath(rng, world.path(0), world.tag(rng), world.tag(rng), world);
        world.paths = 1;
    }
    varyBuilt(rng, world, 10);
    setArg(ctx, 0, world.path(static_cast<int>(rng.below(static_cast<uint32_t>(world.paths)))));
    setArg(ctx, 1, rng.chance(45) ? 3u : rng.chance(80) ? 4u : rng.below(6));
}

void pathBuilt(R5900Context *, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    const World world = buildWorld(rng, scratch);
    varyBuilt(rng, world, 40);
}

void pathSmoother(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    World world = buildWorld(rng, scratch);
    if (rng.chance(50))
        world.tags = {0x100000e4u, 0x100000dbu, 0x100000dau};
    varyBuilt(rng, world, 20);
    const uint32_t a = world.tag(rng), b = rng.chance(20) ? a : world.tag(rng);
    setArg(ctx, 0, a);
    setArg(ctx, 1, b);
    setArg(ctx, 2, world.tag(rng));
    setArg(ctx, 3, rng.next());
}

void pathTags(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    const World world = buildWorld(rng, scratch);
    varyBuilt(rng, world, 10);
    uint32_t start = world.tag(rng), end = world.tag(rng);
    if (world.paths && rng.chance(40)) // the two ends of a path
    {
        const uint32_t p = world.path(static_cast<int>(rng.below(static_cast<uint32_t>(world.paths))));
        const uint32_t length = mem<uint8_t>(p);
        start = mem<uint32_t>(p + 8u);
        end = mem<uint32_t>(p + 4u + length * 4u);
        if (rng.chance(50))
            std::swap(start, end);
    }
    else if (rng.chance(40)) // a pair of the link table
    {
        const uint32_t links = linkCount();
        const uint32_t link = kPathLinks + rng.below(links) * 16u;
        start = mem<uint32_t>(link);
        end = mem<uint32_t>(link + 4u);
        if (rng.chance(50))
            std::swap(start, end);
    }
    setArg(ctx, 0, start);
    setArg(ctx, 1, end);
}

void pathDirection(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    const World world = buildWorld(rng, scratch);
    const uint32_t p = world.path(0);
    writePath(rng, p, world.tag(rng), world.tag(rng), world);
    const uint32_t length = mem<uint8_t>(p);
    const uint32_t first = mem<uint32_t>(p + 8u), last = mem<uint32_t>(p + 4u + length * 4u);
    setArg(ctx, 0, p);
    setArg(ctx, 1, rng.chance(60) ? last : world.tag(rng));
    setArg(ctx, 2, rng.chance(60) ? first : world.tag(rng));
}

void pathDaemon(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    const uint32_t count = rng.below(rng.chance(80) ? 20u : 60u);
    for (uint32_t i = 0; i < count + 2; ++i)
        mem<uint32_t>(scratch + kTagWords + 8u + i * 4u) = i == count ? 0u : rng.next() | 1u;
    mem<uint32_t>(scratch + kTagWords) = rng.next();
    mem<uint32_t>(scratch + kHandle) = scratch + kTagWords;
    setArg(ctx, 0, scratch + kHandle);
}

void pathBuilder(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    const World world = buildWorld(rng, scratch, false);
    varyBuilt(rng, world, 8);
    varyDetails(rng, world);
    const uint32_t dest = rng.chance(10) ? mem<uint32_t>(kDetails) : world.tag(rng);
    setArg(ctx, 0, dest);
}

void pathEnd(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    const World world = buildWorld(rng, scratch, false);
    varyBuilt(rng, world, 8);
    varyDetails(rng, world);
    // The camera nodes look their tags up in the level's camera list: this input's.
    const uint32_t level = mem<uint32_t>(kLevelPtr);
    if (level != 0 && level < 0x2000000u && rng.chance(80))
        mem<uint32_t>(level + 0x1cu) = scratch + kCams;
    setArg(ctx, 0, rng.next());
}

} // namespace rc::fuzz

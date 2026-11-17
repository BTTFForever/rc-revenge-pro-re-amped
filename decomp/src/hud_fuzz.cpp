// Builds the HUD's random-test inputs. See hud_fuzz.h.

#include "hud_fuzz.h"

#include <cstring>

namespace rc::fuzz {

namespace {

constexpr uint32_t kHud = sym::hudVarG, kMainVars = sym::mainVarG, kFrontEnd = sym::frontEndVarG, kLevelPtr = sym::pLvlG;
constexpr uint32_t kObjectList = sym::objectVarG, kNoObjects = (sym::objectVarG + 0x170u), kGfx = sym::gGfxP2, kVsyncInc = (sym::displayVarG + 0x64u);
constexpr uint32_t kCameraRSG = (sym::cameraVarG + 0xb74u), kVfxSprite = (sym::visFxVarG + 0x2720u), kVfxRes = (sym::visFxVarG + 0x6cu), kSfxInitOk = sym::sfxVarG;
constexpr uint32_t kRandom = sym::randomVarG;

// Scratch layout.
constexpr uint32_t kSprite = 0x0000u, kText = 0x0100u, kFELevel = 0x0200u, kCams = 0x0300u, kRes = 0x0800u, kHandle = 0x0a00u;

template <typename T>
T &mem(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

struct Rng
{
    uint64_t &state;
    uint32_t next() { return nextFuzz(state); }
    uint32_t below(uint32_t n) { return n ? next() % n : 0u; }
    int32_t between(int32_t low, int32_t high) { return low + static_cast<int32_t>(below(static_cast<uint32_t>(high - low + 1))); }
    bool chance(uint32_t percent) { return below(100) < percent; }
    float range(float low, float high) { return low + (high - low) * (static_cast<float>(next() >> 8) / 16777216.0f); }
};

void setArg(R5900Context *ctx, int index, uint32_t value) { SET_GPR_S32(ctx, 4 + index, static_cast<int32_t>(value)); }
void setFloatArg(R5900Context *ctx, int index, float value) { std::memcpy(&ctx->f[12 + index], &value, 4); }

bool inRam(uint32_t address) { return address != 0 && address < 0x2000000u; }

uint32_t hudRes()
{
    const uint32_t level = mem<uint32_t>(kLevelPtr);
    return inRam(level) ? mem<uint32_t>(level + 0x40u) : 0u;
}

// Sound off (an original that plays one waits for the IOP offline); the game's random seeds from the input; the
// stack below $sp random (the rotated sprite's corner vectors have stale z and w lanes).
void common(R5900Context *ctx, Rng &rng)
{
    mem<uint8_t>(kSfxInitOk) = 0;
    mem<uint32_t>(kRandom) = rng.next();
    mem<uint32_t>(kRandom + 4u) = rng.next();
    const uint32_t sp = getRegU32(ctx, 29);
    for (uint32_t offset = 0x800; offset >= 4; offset -= 4)
        mem<uint32_t>(sp - offset) = rng.chance(70) ? rng.next() & 0x3fffffffu : rng.next();
}

// The packet buffer's open tag: kept, or none (the first vertex opens one).
void varyPacket(Rng &rng)
{
    if (rng.chance(40))
        mem<int32_t>(kGfx + 0x18u) = -1;
}

uint32_t realCar(Rng &rng)
{
    const uint32_t count = mem<uint32_t>(kNoObjects);
    for (int tries = 0; tries < 30 && count; ++tries)
    {
        const uint32_t car = mem<uint32_t>(kObjectList + 4u * rng.below(count));
        if (inRam(car) && inRam(mem<uint32_t>(car + 0x20u)) && inRam(mem<uint32_t>(mem<uint32_t>(car + 0x20u) + 0x1e8u)))
            return car;
    }
    return mem<uint32_t>(kObjectList);
}

// A HUD sprite: one of the level's, or a scratch copy with another size.
uint32_t sprite(Rng &rng, uint32_t scratch)
{
    const uint32_t res = hudRes();
    static const uint8_t kOffsets[] = {0x08, 0x24, 0x28, 0x2c, 0x30, 0x44, 0x48, 0x54, 0x58, 0x5c, 0x84, 0xa0, 0xa4, 0xc8, 0xcc};
    uint32_t spr = 0;
    if (inRam(res))
        spr = mem<uint32_t>(res + kOffsets[rng.below(sizeof(kOffsets))]);
    if (!inRam(spr) || rng.chance(40))
    {
        const uint32_t copy = scratch + kSprite;
        if (inRam(spr))
            std::memcpy(guest_host(copy), guest_host(spr), 8);
        else
            mem<uint32_t>(copy) = 0;
        mem<uint8_t>(copy + 6u) = static_cast<uint8_t>(rng.next());
        mem<uint8_t>(copy + 7u) = static_cast<uint8_t>(rng.next());
        spr = copy;
    }
    return spr;
}

void varyHud(Rng &rng)
{
    if (rng.chance(50))
        mem<uint8_t>(kHud + 0x1eu) = static_cast<uint8_t>(rng.next());
    if (rng.chance(40))
        mem<int16_t>(kHud + 0x1cu) = static_cast<int16_t>(rng.chance(50) ? 0 : rng.between(-0x40, 0x200));
    for (uint32_t k = 0; k < 2; ++k)
    {
        if (rng.chance(40))
        {
            static const int32_t kSplat[] = {-1, 0x80, 0x81, 0x82, 0xff, 0x100};
            mem<int32_t>(kHud + 0x7cu + k * 4u) = rng.chance(60) ? kSplat[rng.below(6)] : rng.between(-1, 0x120);
        }
        if (rng.chance(30))
            mem<int32_t>(kHud + 0x84u + k * 4u) = rng.between(0, 640);
        if (rng.chance(30))
            mem<int32_t>(kHud + 0x8cu + k * 4u) = rng.between(0, 480);
    }
}

void varyMain(Rng &rng)
{
    if (rng.chance(40))
        mem<uint32_t>(kMainVars + 0x34cu) = rng.chance(60) ? 0x100001feu : rng.next() & 0x100001ffu;
    if (rng.chance(20))
        mem<int32_t>(kMainVars) = rng.chance(50) ? 0x20 : 0x10;
    if (rng.chance(30))
        mem<uint8_t>(kMainVars + 0x34au) = static_cast<uint8_t>(rng.chance(30) ? 0x61 : 1 + rng.below(0x30));
    if (rng.chance(30))
        mem<int32_t>(kMainVars + 0x398u) = static_cast<int32_t>(rng.below(2));
    if (rng.chance(40))
        mem<int32_t>(kMainVars + 0x378u) = static_cast<int32_t>(rng.below(2));
    if (rng.chance(30))
        mem<uint8_t>(kMainVars + 0x36bu) = static_cast<uint8_t>(rng.below(2));
    if (rng.chance(20))
        mem<uint32_t>(kFrontEnd + 0x29cu) = 0x1000006bu;
}

void varyCar(Rng &rng, uint32_t car)
{
    if (rng.chance(50))
        mem<int16_t>(car + 0x13cu) = static_cast<int16_t>(rng.below(8));
    if (rng.chance(50))
        mem<int16_t>(car + 0x13eu) = static_cast<int16_t>(rng.below(8));
    if (rng.chance(40))
        mem<int32_t>(car + 0xe8u) = rng.chance(60) ? 0 : static_cast<int32_t>(rng.next() & 0xfffff);
    if (rng.chance(50))
        mem<int16_t>(car + 0x104u) = static_cast<int16_t>(rng.between(-2, 6));
    if (rng.chance(50))
        mem<int32_t>(car + 0x140u) = static_cast<int32_t>(rng.chance(10) ? rng.next() : rng.below(7000000));
    if (rng.chance(50))
        mem<int32_t>(car + 0x16cu) = rng.chance(40) ? 0x10000000 : static_cast<int32_t>(rng.below(7000000));
    if (rng.chance(50))
        mem<int32_t>(car + 0xf0u) = rng.chance(50) ? rng.between(0x3b5, 0x3b8) : rng.between(0, 0x800);
    if (rng.chance(50))
        mem<int32_t>(car + 0xecu) = rng.chance(50) ? rng.between(0x13, 0x16) : rng.between(0, 0x40);
    if (rng.chance(60))
        mem<int8_t>(car + 0x194u) = static_cast<int8_t>(rng.chance(30) ? 0 : rng.between(1, 3));
    if (rng.chance(60))
        mem<int8_t>(car + 0x195u) = static_cast<int8_t>(rng.between(1, 14));
    if (rng.chance(60))
        mem<int8_t>(car + 0x1afu) = static_cast<int8_t>(rng.chance(50) ? 0 : rng.between(1, 14));
    if (rng.chance(50))
        mem<float>(mem<uint32_t>(car + 0x20u) + 0x38u) = rng.chance(10) ? rng.range(-2000.0f, 0.0f) : rng.range(0.0f, 27000.0f);
    // The other cars' blips.
    const uint32_t count = mem<uint32_t>(kNoObjects);
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t other = mem<uint32_t>(kObjectList + 4u * i);
        if (!inRam(other))
            continue;
        if (rng.chance(30))
            mem<uint8_t>(other + 0x44u) = static_cast<uint8_t>(rng.below(4));
        if (rng.chance(30))
            mem<int32_t>(other + 0x48u) = rng.between(0, 0x90);
    }
}

} // namespace

void hudNone(R5900Context *ctx, uint64_t &state, uint32_t)
{
    Rng rng{state};
    common(ctx, rng);
    if (rng.chance(30))
        mem<int32_t>(kMainVars) = 0x10;
    if (rng.chance(40))
        mem<uint8_t>(kMainVars + 0x368u) = static_cast<uint8_t>(rng.below(2));
}

void hudTime(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    const int32_t time = rng.chance(10) ? static_cast<int32_t>(rng.next()) : rng.chance(20) ? rng.between(5990000, 6010000)
                                                                                               : rng.between(0, 7000000);
    setArg(ctx, 0, scratch + kText);
    setArg(ctx, 1, static_cast<uint32_t>(time));
}

void hudCar(R5900Context *ctx, uint64_t &state, uint32_t)
{
    Rng rng{state};
    common(ctx, rng);
    varyPacket(rng);
    varyHud(rng);
    varyMain(rng);
    const uint32_t car = realCar(rng);
    varyCar(rng, car);
    setArg(ctx, 0, car);
    setArg(ctx, 1, rng.chance(90) ? 1u + rng.below(2) : rng.below(4));
    setArg(ctx, 2, rng.chance(90) ? rng.below(2) : rng.below(4));
}

void hudPanel(R5900Context *ctx, uint64_t &state, uint32_t)
{
    Rng rng{state};
    common(ctx, rng);
    varyPacket(rng);
    varyHud(rng);
    varyMain(rng);
    if (rng.chance(30))
        mem<uint16_t>(kMainVars + 0x366u) = static_cast<uint16_t>(rng.below(2));
    if (rng.chance(40))
        mem<uint8_t>(kMainVars + 0x368u) = static_cast<uint8_t>(rng.below(2));
    if (rng.chance(40))
        mem<int32_t>(kHud + 0x78u) = static_cast<int32_t>(rng.below(2));
    if (rng.chance(40))
        mem<uint32_t>(kCameraRSG) = rng.below(2);
    const uint32_t car = realCar(rng);
    varyCar(rng, car);
    setArg(ctx, 0, rng.chance(10) ? 0u : car);
    setArg(ctx, 1, rng.chance(90) ? rng.below(2) : rng.below(4));
}

void hudTask(R5900Context *ctx, uint64_t &state, uint32_t)
{
    Rng rng{state};
    common(ctx, rng);
    mem<int32_t>(kHud + 0x70u) = rng.chance(20) ? 0 : rng.between(-1, 6);
    // mainVarG's timer, paused (TimerGet then returns its time as it is) or running from the dump's tick.
    if (rng.chance(70))
        mem<uint32_t>((sym::mainVarG + 0x10u)) |= 1u;
    const uint32_t now = mem<uint32_t>((sym::mainVarG + 0x18u));
    if (rng.chance(50))
        mem<int32_t>(kHud + 0x74u) = 0;
    else
        mem<uint32_t>(kHud + 0x74u) = now - (rng.chance(50) ? 4900u + rng.below(1200) : rng.below(7000)); // the last light at 5 s
    if (rng.chance(50))
        mem<int32_t>(kHud + 0x78u) = static_cast<int32_t>(rng.below(2));
    if (rng.chance(80))
        mem<uint32_t>(kCameraRSG) = rng.chance(80) ? 1u : 0u;
    if (rng.chance(30))
        mem<int32_t>(kMainVars) = 0x20;
    if (rng.chance(40))
        mem<uint8_t>(kHud + 0x1fu) = static_cast<uint8_t>(rng.below(3));
    if (rng.chance(40))
        mem<int32_t>(kHud + 0x18u) = static_cast<int32_t>(rng.next() >> rng.below(20));
    setArg(ctx, 0, rng.next());
    setArg(ctx, 1, static_cast<uint32_t>(mem<int32_t>(kMainVars + 0x1cu) + rng.between(0, 100)));
}

void hudSprite(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    common(ctx, rng);
    varyPacket(rng);
    setArg(ctx, 0, sprite(rng, scratch));
    setArg(ctx, 1, rng.next());
    setFloatArg(ctx, 0, rng.chance(5) ? rng.range(-5000.0f, 5000.0f) : rng.range(-64.0f, 700.0f));
    setFloatArg(ctx, 1, rng.chance(5) ? rng.range(-5000.0f, 5000.0f) : rng.range(-64.0f, 520.0f));
    setFloatArg(ctx, 2, rng.chance(10) ? rng.range(-2.0f, 2.0f) : rng.range(0.0f, 1.0f));
    setFloatArg(ctx, 3, rng.chance(20) ? -3.2f - rng.range(0.0f, 0.01f) : rng.range(-4.0f, 4.0f));
}

void hudSign(R5900Context *ctx, uint64_t &state, uint32_t)
{
    Rng rng{state};
    common(ctx, rng);
    varyPacket(rng);
    setArg(ctx, 0, static_cast<uint32_t>(rng.chance(5) ? static_cast<int32_t>(rng.next()) : rng.between(-100, 700)));
    setArg(ctx, 1, static_cast<uint32_t>(rng.between(-100, 600)));
    setArg(ctx, 2, static_cast<uint32_t>(rng.chance(5) ? static_cast<int32_t>(rng.next()) : rng.between(-8, 64)));
}

void hudBorder(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    common(ctx, rng);
    varyPacket(rng);
    if (rng.chance(60))
        mem<uint32_t>(kVsyncInc) = rng.chance(80) ? rng.below(100000) : rng.next();
    setArg(ctx, 0, sprite(rng, scratch));
    setArg(ctx, 1, static_cast<uint32_t>(rng.between(-100, 700)));
    setArg(ctx, 2, static_cast<uint32_t>(rng.between(-100, 600)));
    setArg(ctx, 3, rng.next());
}

void hudLocked(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    common(ctx, rng);
    // The front end's cameras in scratch; the locked tracks' tags among them (or not, or none).
    const uint32_t cams = scratch + kCams;
    const int32_t count = static_cast<int32_t>(rng.below(12));
    mem<int32_t>(cams) = count;
    for (int32_t i = 0; i < count; ++i)
    {
        const uint32_t c = cams + 4u + static_cast<uint32_t>(i) * 0x14u;
        mem<uint32_t>(c) = 0x10000000u + static_cast<uint32_t>(i);
        mem<uint32_t>(c + 4u) = rng.next();
        for (uint32_t k = 8; k < 0x14; k += 4)
            mem<int32_t>(c + k) = rng.between(-0x400000, 0x400000);
    }
    mem<uint32_t>(scratch + kFELevel + 0x30u) = cams;
    mem<uint32_t>(kFrontEnd) = scratch + kFELevel;
    for (uint32_t k = 0; k < 3; ++k)
        mem<uint32_t>(kFrontEnd + 0x12cu + k * 4u) = rng.chance(30) ? 0u : rng.chance(85) ? 0x10000000u + rng.below(static_cast<uint32_t>(count) + 1) : rng.next();
    const uint32_t spr = mem<uint32_t>(kVfxSprite), res = mem<uint32_t>(kVfxRes);
    const bool scratchpad = spr >= 0x70000000u && spr < 0x70004000u - 0x30u; // the effects' sprite record lives there
    if (!(inRam(spr) || scratchpad) || !inRam(res) || rng.chance(5))
        mem<uint32_t>(kObjectList) = 0;
}

void hudDaemon(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    common(ctx, rng);
    // Ids whose handle is already a pointer (low bits clear), which ResLoad returns as it is; one with the low bits set
    // would be read from the disc. In the dumps handlesHandle is 0, so the handles are low RAM's words.
    const resClass &res = G(resVarG);
    const uint32_t maxId = res.lookupHeader.maxId;
    const uint32_t handles = res.handlesHandle.address;
    const bool ok = handles < 0x02000000u && maxId >= 2 && maxId <= 0x10000;
    for (uint32_t offset = 0; offset < 0xe4; offset += 4)
    {
        uint32_t id = 0;
        for (int tries = 0; ok && tries < 200 && rng.chance(80); ++tries)
        {
            const uint32_t candidate = 1 + rng.below(maxId - 1);
            if ((mem<uint32_t>(handles + candidate * 4u) & 3u) == 0)
            {
                id = candidate;
                break;
            }
        }
        mem<uint32_t>(scratch + kRes + offset) = id;
    }
    mem<uint32_t>(scratch + kHandle) = scratch + kRes;
    setArg(ctx, 0, scratch + kHandle);
}

} // namespace rc::fuzz

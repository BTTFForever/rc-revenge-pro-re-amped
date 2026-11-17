// Builds the random-test inputs of AMENU.c. See amenu_fuzz.h.

#include "amenu_fuzz.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace rc::fuzz {

namespace {

// Globals.
constexpr uint32_t kMenuSystem = sym::menuSystemG, kMenuSystemPtr = sym::pMenuSystemG, kMainVars = sym::mainVarG, kFrontEnd = sym::frontEndVarG;
constexpr uint32_t kLevelPtr = sym::pLvlG, kTEditGeneralText = (sym::tEditVarG + 0x6ecu), kTEditCarNames = (sym::tEditVarG + 0x6f0u), kTEditHelp = (sym::tEditVarG + 0x24cu);
constexpr uint32_t kCameraPtr = (sym::cameraVarG + 0xb70u), kInput = sym::inputVarG, kGfx = sym::gGfxP2, kFastScroll = sym::fastScrollPoints;
constexpr uint32_t kMusicUserVol = (sym::rcmusicVarG + 0x20u), kSfxUserVol = (sym::revSfxVarG + 0x1144u), kMusicVolG = sym::musicVolG, kSfxVolG = sym::sfxVolG;
constexpr uint32_t kBox = kFrontEnd + 0xa8u;

// The functions replaced offline.
constexpr uint32_t kSfxPlay = sym::SfxPlay, kRCMusicModifyVolume = sym::RCMusicModifyVolume, kRevSfxModifyVolume = sym::RevSfxModifyVolume;
constexpr uint32_t kHideEmptyMenu = sym::HideEmptyMenu, kSetRewards = sym::SetRewards, kGetTimeTrialTime = sym::GetTimeTrialTime, kSelectionTest = sym::SelectionTest;
constexpr uint32_t kPadInfoMode = sym::scePadInfoMode;

// Scratch layout.
constexpr uint32_t kHeaders = 0x0000u, kColours = 0x0200u, kBorders = 0x0220u, kItems = 0x0300u, kVars = 0x0440u;
constexpr uint32_t kGroups = 0x0600u, kGroupCount = 64u, kStrings = 0x0a00u, kStringSize = 0x30u, kStringCount = 64u;
constexpr uint32_t kFeedback = 0x1600u, kSelection = 0x1640u, kGeneral = 0x1700u, kCars = 0x1820u, kLevel = 0x1900u;
constexpr uint32_t kFELevel = 0x1a00u, kExtra = 0x1b00u, kCamera = 0x1d00u, kList = 0x1e00u, kHelp = 0x2000u, kBar = 0x2080u;
constexpr uint32_t kSpriteParams = 0x2140u, kBuffer = 0x2200u, kGameState = 0x2800u, kCounters = 0x2a00u;
constexpr uint32_t kEnv = kFuzzScratch + 0x7b00u, kLog = kFuzzScratch + 0x7c00u, kLogMax = 48u;
enum : uint32_t { eSeed = 0, eCalls = 4, eHash = 8, ePad0 = 12, ePad1 = 16, eSelectPercent = 20 };

template <typename T>
T &mem(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

struct Rng
{
    uint64_t &state;
    uint32_t next() { return nextFuzz(state); }
    uint32_t below(uint32_t n) { return n ? next() % n : 0u; }
    int32_t between(int32_t low, int32_t high) { return low + static_cast<int32_t>(below(static_cast<uint32_t>(high - low + 1))); }
    bool chance(uint32_t percent) { return below(100) < percent; }
    template <typename T, size_t N>
    T pick(const T (&values)[N]) { return values[below(static_cast<uint32_t>(N))]; }
    // Each bit of mask set with the given chance.
    uint32_t bits(uint32_t mask, uint32_t percent)
    {
        uint32_t out = 0;
        for (uint32_t bit = 1; bit != 0 && bit <= mask; bit <<= 1)
            if ((mask & bit) != 0 && chance(percent))
                out |= bit;
        return out;
    }
};

void setArg(R5900Context *ctx, int index, uint32_t value) { SET_GPR_S32(ctx, 4 + index, static_cast<int32_t>(value)); }
uint32_t arg(R5900Context *ctx, int index) { return getRegU32(ctx, 4 + index); }
bool inRam(uint32_t address) { return address >= 0x100000u && address < 0x2000000u; }

uint32_t mix(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

uint32_t draw()
{
    const uint32_t k = mem<uint32_t>(kEnv + eCalls);
    mem<uint32_t>(kEnv + eCalls) = k + 1u;
    return mix(mem<uint32_t>(kEnv + eSeed) + k * 0x9e3779b9u);
}

void logEntry(uint32_t what, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    static const bool trace = std::getenv("RC_AMENUFUZZ_TRACE") != nullptr; // each logged call on stderr
    if (trace)
        std::fprintf(stderr, "[amenu_fuzz] %x %x %x %x %x\n", what, a, b, c, d);
    uint32_t hash = mem<uint32_t>(kEnv + eHash);
    for (const uint32_t word : {what, a, b, c, d})
        hash = (hash ^ word) * 16777619u;
    mem<uint32_t>(kEnv + eHash) = hash;
    const uint32_t count = mem<uint32_t>(kLog);
    if (count < kLogMax)
    {
        const uint32_t at = kLog + 4u + count * 20u;
        mem<uint32_t>(at) = what;
        mem<uint32_t>(at + 4u) = a;
        mem<uint32_t>(at + 8u) = b;
        mem<uint32_t>(at + 12u) = c;
        mem<uint32_t>(at + 16u) = d;
    }
    mem<uint32_t>(kLog) = count + 1u;
}

void returnWith(R5900Context *ctx, uint32_t value)
{
    SET_GPR_S32(ctx, 2, static_cast<int32_t>(value));
    ctx->pc = getRegU32(ctx, 31);
}

template <uint32_t Address, int Args, uint32_t Answer>
void loggedCall(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const auto a = [ctx](int i) { return i < Args ? arg(ctx, i) : 0u; };
    logEntry(Address, a(0), a(1), a(2), a(3));
    returnWith(ctx, Answer);
}

// SfxPlay(sample, volume, pan, pan, pitch, flags, chain).
void fakeSfxPlay(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    logEntry(kSfxPlay, arg(ctx, 0), arg(ctx, 1), arg(ctx, 2) | arg(ctx, 3) << 8, arg(ctx, 4) | arg(ctx, 5) << 12 | arg(ctx, 6) << 20);
    returnWith(ctx, 0);
}

// SetRewards(table, game state, cup): the table's bytes from the seed, mostly 0 to 3.
void fakeSetRewards(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t table = arg(ctx, 0);
    logEntry(kSetRewards, table, arg(ctx, 1), arg(ctx, 2), 0);
    const uint32_t r = draw();
    for (uint32_t i = 0; i < 0x28u; ++i)
    {
        const uint32_t v = mix(r + i);
        mem<uint8_t>(table + i) = static_cast<uint8_t>((v & 0x300u) == 0 ? v >> 16 : v & 3u);
    }
    returnWith(ctx, 0);
}

// GetTimeTrialTime(buffer, game state, level, reversed): a time in the buffer; mostly 0 or 1.
void fakeGetTimeTrialTime(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t dest = arg(ctx, 0);
    logEntry(kGetTimeTrialTime, dest, arg(ctx, 1), arg(ctx, 2), arg(ctx, 3));
    const uint32_t r = draw();
    if (dest != 0)
    {
        char text[16];
        std::snprintf(text, sizeof(text), "%u:%02u.%u", r % 10u, (r >> 8) % 60u, (r >> 16) % 10u);
        std::memcpy(guest_host(dest), text, std::strlen(text) + 1u);
    }
    static const uint32_t kAnswers[] = {0, 0, 0, 1, 1, 1, 2, 0xffffffffu};
    returnWith(ctx, kAnswers[(r >> 20) % 8u]);
}

void fakeSelectionTest(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t answer = draw() % 100u < mem<uint32_t>(kEnv + eSelectPercent) ? 1u : (draw() & 1u) != 0 ? 0u : 2u;
    logEntry(kSelectionTest, answer, 0, 0, 0);
    returnWith(ctx, answer);
}

// scePadInfoMode(port, slot, term, offset): the mode the input chose for the port.
void fakePadInfoMode(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    logEntry(kPadInfoMode, arg(ctx, 0), arg(ctx, 1), arg(ctx, 2), arg(ctx, 3));
    returnWith(ctx, mem<uint32_t>(kEnv + ((arg(ctx, 0) & 1u) != 0 ? ePad1 : ePad0)));
}

void quietMenu(Rng &rng)
{
    static const bool trace = std::getenv("RC_AMENUFUZZ_TRACE") != nullptr;
    static int input = 0;
    if (trace)
        std::fprintf(stderr, "[amenu_fuzz] input %d\n", input++);
    for (uint32_t offset = 0; offset < 0x40u; offset += 4u)
        mem<uint32_t>(kEnv + offset) = 0;
    mem<uint32_t>(kLog) = 0;
    mem<uint32_t>(kEnv + eSeed) = rng.next();
    static const uint32_t kModes[] = {0, 0, 4, 7, 1};
    mem<uint32_t>(kEnv + ePad0) = rng.pick(kModes);
    mem<uint32_t>(kEnv + ePad1) = rng.pick(kModes);
    mem<uint32_t>(kEnv + eSelectPercent) = rng.chance(60) ? 0u : 60u;
    if (!g_offline)
        return;
    static bool installed = false;
    if (installed)
        return;
    installed = true;
    PS2Runtime &rt = *g_runtime;
    rt.replaceFunction(kSfxPlay, &fakeSfxPlay);
    rt.replaceFunction(kRCMusicModifyVolume, &loggedCall<kRCMusicModifyVolume, 2, 0u>);
    rt.replaceFunction(kRevSfxModifyVolume, &loggedCall<kRevSfxModifyVolume, 1, 0u>);
    rt.replaceFunction(kHideEmptyMenu, &loggedCall<kHideEmptyMenu, 1, 1u>);
    rt.replaceFunction(kSetRewards, &fakeSetRewards);
    rt.replaceFunction(kGetTimeTrialTime, &fakeGetTimeTrialTime);
    rt.replaceFunction(kSelectionTest, &fakeSelectionTest);
    rt.replaceFunction(kPadInfoMode, &fakePadInfoMode);
}

// What the dump's own menus use, read before the setups write over menuSystemG.
struct Real
{
    uint32_t borderSet = 0, colourSet = 0, keys = 0, tick = 0, slider = 0;
};

const Real &real()
{
    static Real r;
    static bool read = false;
    if (!read)
    {
        read = true;
        uint32_t menu = mem<uint32_t>(kMenuSystem);
        if (!inRam(menu))
            menu = mem<uint32_t>(kMenuSystem + 0x14u);
        if (inRam(menu))
        {
            r.borderSet = mem<uint32_t>(menu + 8u);
            r.colourSet = mem<uint32_t>(menu + 0xcu);
        }
        r.keys = mem<uint32_t>(kMenuSystem + 0x24u);
        r.tick = mem<uint32_t>(kMenuSystem + 0x28u);
        r.slider = mem<uint32_t>(kMenuSystem + 0x2cu);
    }
    return r;
}

constexpr uint32_t kItemTags[] = {0, 0, 0, 0x10000200u, 0x10000223u, 0x10000224u, 0x10000225u, 0x10000285u, 0x10000286u, 0x10000287u,
                                  0x10000288u, 0x10000289u, 0x1000028bu, 0x1000028du, 0x1000028eu, 0x10000078u, 0x10000054u, 0x1000053fu};
constexpr uint32_t kMenuTags[] = {0, 0, 0, 0, 0x10000282u, 0x10000284u, 0x1000028fu, 0x10000290u, 0x1000020du, 0x10000217u};

// How a menu is made.
struct Options
{
    int items = -1;     // -1: random
    bool drawn = false; // sizes and places on the screen; no wild values
    bool tame = false;  // no flags that swap keys or change the kind of menu
};

// The world of one input.
class World
{
public:
    World(R5900Context *ctx, Rng &rng, uint32_t scratch) : ctx(ctx), rng(rng), s(scratch)
    {
        quietMenu(rng);
        // Null pointers are followed in places: low memory reads as zero, as in the game.
        std::memset(guest_host(0), 0, 0x100);
        // Stale stack words show through partly built sprite and rectangle parameters.
        const uint32_t sp = getRegU32(ctx, 29);
        for (uint32_t offset = 0x1000; offset >= 4; offset -= 4)
            mem<uint32_t>(sp - offset) = rng.chance(70) ? rng.next() & 0x3f3f3f3fu : rng.next();
        mem<int32_t>(kMainVars + 0x170u) = language = rng.between(0, 3);
        globals();
    }

    uint32_t header(int slot) const { return s + kHeaders + static_cast<uint32_t>(slot) * 0x80u; }
    uint32_t item(int index) const { return s + kItems + static_cast<uint32_t>(index) * 0x18u; }
    uint32_t var(int index) const { return s + kVars + static_cast<uint32_t>(index) * 0x1cu; }
    uint32_t colourSet() const { return s + kColours; }

    // A string in scratch.
    uint32_t string(const std::string &text)
    {
        const uint32_t at = s + kStrings + (m_strings++ % kStringCount) * kStringSize;
        const size_t length = std::min<size_t>(text.size(), kStringSize - 1u);
        std::memcpy(guest_host(at), text.data(), length);
        mem<char>(at + static_cast<uint32_t>(length)) = 0;
        return at;
    }

    std::string word(int low, int high)
    {
        static const char kChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789:.-";
        std::string out;
        for (int n = rng.between(low, high); n > 0; --n)
            out += kChars[rng.below(sizeof(kChars) - 1u)];
        return out;
    }

    // Words with spaces, now and then a key picture or a colour change.
    std::string phrase(int words, bool plain = false)
    {
        std::string out;
        for (int i = 0; i < words; ++i)
        {
            if (i != 0)
                out += ' ';
            if (!plain && rng.chance(8))
                out += rng.chance(60) ? "}A" : rng.chance(50) ? "}3" : "}b";
            else
                out += word(1, 7);
        }
        return out;
    }

    // A text group: the same kind of text in each language (a different one for the working language now and then).
    template <typename Make>
    uint32_t group(Make make)
    {
        const uint32_t at = s + kGroups + (m_groups++ % kGroupCount) * 0x10u;
        const uint32_t base = string(make());
        for (uint32_t l = 0; l < 4; ++l)
            mem<uint32_t>(at + l * 4u) = base;
        if (rng.chance(50))
            mem<uint32_t>(at + static_cast<uint32_t>(language) * 4u) = string(make());
        return at;
    }

    uint32_t textGroup(int words = 2) { return group([&] { return phrase(rng.between(1, words)); }); }

    // An ordinary menu at a slot: items, variables and the header.
    uint32_t menu(int slot, const Options &options = Options())
    {
        const uint32_t h = header(slot);
        std::memset(guest_host(h), 0, 0x80);
        auto &hd = mem<sAMenuHeader>(h);
        const int count = options.items >= 0 ? options.items : rng.chance(8) ? 0 : rng.between(1, 6);
        const int first = m_items;
        // Disabled items only where the selection's search for an enabled one must end: see amenu_fuzz.h's notes.
        const bool disabling = count >= 4 && rng.chance(35);
        const bool unselectable = !disabling && rng.chance(30);
        for (int i = 0; i < count; ++i)
        {
            const int index = m_items++;
            const uint32_t it = item(index);
            std::memset(guest_host(it), 0, 0x18);
            auto &mi = mem<sMenuItemStd>(it);
            const int kind = rng.chance(45) ? 0 : rng.between(1, 4); // plain, choice, number, slider, tabbed
            uint16_t flags = static_cast<uint16_t>(rng.bits(0x1 | 0x4 | 0x10 | 0x20, 8) | rng.bits(0x300, 10));
            if (unselectable && i < 2 && rng.chance(50))
                flags |= 0x40;
            uint8_t status = static_cast<uint8_t>(rng.bits(0x2 | 0x4 | 0x8 | 0x10 | 0x20, 8));
            if (disabling && i >= 2 && i <= count - 2 && rng.chance(50))
                status |= 1;
            if (kind == 4)
            {
                flags |= 2;
                if (rng.chance(40))
                    flags |= 8;
                const bool stops = (flags & 8) != 0;
                mi.pTextGroup.address = group([&] {
                    std::string out;
                    for (int n = rng.between(1, 4); n > 0; --n)
                    {
                        out += word(1, 5);
                        if (n > 1)
                            out += stops ? '*' : static_cast<char>(0xaa);
                    }
                    return out;
                });
            }
            else if (kind == 1 || kind == 2)
                mi.pTextGroup.address = group([&] { return rng.chance(85) ? phrase(1, true) + ": %s" : phrase(2, true); });
            else if (kind == 0 && rng.chance(6))
                mi.pTextGroup.address = 0;
            else
                mi.pTextGroup.address = textGroup(3);
            if (kind != 0 && (kind != 4 || rng.chance(30)))
            {
                const uint32_t v = var(index);
                std::memset(guest_host(v), 0, 0x1c);
                auto &mv = mem<sMenuItemVar>(v);
                uint16_t vf = static_cast<uint16_t>(rng.bits(0x10, 35) | rng.bits(0x8 | 0x100 | 0x200, 10));
                if (kind == 2)
                    vf |= 2;
                else if (kind == 3)
                    vf |= 4;
                mv.flags = vf;
                if (kind == 1 || kind == 4)
                {
                    const int choices = rng.between(1, 4);
                    mv.pTextGroup.address = rng.chance(4) ? 0u : group([&] {
                        std::string out;
                        for (int n = 0; n < choices; ++n)
                        {
                            if (n != 0)
                                out += '*';
                            out += word(rng.chance(10) ? 0 : 1, 6);
                        }
                        if (rng.chance(15))
                            out += '*';
                        return out;
                    });
                    mv.minValue = 0;
                    mv.maxValue = rng.chance(40) ? 1 : choices - 1 + rng.between(0, 1);
                    mv.value = rng.chance(85) ? rng.between(0, choices) : rng.between(-2, 8);
                }
                else if (kind == 2)
                {
                    mv.pTextGroup.address = (vf & 8) != 0 ? static_cast<uint32_t>(rng.between(1, 9)) : 0u;
                    mv.minValue = rng.between(-5, 3);
                    mv.maxValue = mv.minValue + rng.between(1, 300);
                    mv.value = rng.chance(85) ? rng.between(mv.minValue, mv.maxValue) : rng.between(-999, 9999);
                }
                else
                {
                    static const int32_t kMax[] = {255, 16383, 100, 64, 2, 31};
                    mv.minValue = 0;
                    mv.maxValue = rng.pick(kMax);
                    mv.value = rng.between(0, mv.maxValue);
                }
                static const uint32_t kVarTags[] = {0x1000001fu, 0x10000026u, 0x10000032u, 0x10000033u, 0x1000053eu, 0};
                mv.tagMVar = static_cast<int32>(rng.pick(kVarTags));
                if (rng.chance(50))
                {
                    mv.pFeedbackVar.address = s + kFeedback + static_cast<uint32_t>(index) * 4u;
                    mem<int32_t>(mv.pFeedbackVar.address) = rng.chance(70) ? mv.value : rng.between(mv.minValue, mv.maxValue);
                }
                mi.pMenuVar.address = v;
            }
            mi.flags = flags;
            mi.statusFlags = status;
            mi.selectionTag = rng.chance(55) ? rng.pick(kItemTags) : 0x10000000u + rng.below(0x300);
            mi.linkType = static_cast<uint8>(rng.chance(60) ? 0 : rng.chance(75) ? 1 : rng.between(2, 3));
            if (mi.linkType == 0)
                mi.pLinkMenu.address = rng.chance(45) ? header(rng.between(1, 3)) : 0u;
            else
                mi.pLinkMenu.address = rng.chance(35) ? 0x100001f9u : rng.chance(30) ? 0u : 0x10000000u + rng.below(0x300);
            mi.pNext.address = i + 1 < count ? item(index + 1) : 0u;
        }
        hd.pMenuStart.address = count != 0 ? item(first) : 0u;
        hd.menuItems = static_cast<uint8>(disabling || unselectable || rng.chance(92) ? count : rng.between(0, 9));
        hd.flags = static_cast<uint16>(rng.bits(0x1 | 0x4, 40) | rng.bits(0x2 | 0x80 | 0x800 | 0x1000 | 0x2000, 15));
        hd.moreFlags = static_cast<uint16>(rng.bits(0x2 | 0x4 | 0x8 | 0x10 | 0x100 | 0x400 | 0x800 | 0x1000 | 0x2000 | 0x8000, 9) | rng.bits(1, 15));
        hd.evenMoreFlags = static_cast<uint16>(rng.bits(0x1 | 0x4 | 0x8 | 0x20 | 0x80 | 0x100 | 0x200, 9));
        if (options.tame)
        {
            hd.flags &= static_cast<uint16>(~0x80u);
            hd.moreFlags &= 0x0c1e;
            hd.evenMoreFlags &= 0x0288;
        }
        if (count == 0)
            hd.evenMoreFlags &= static_cast<uint16>(~4u); // would clear flags of the item at address 0
        hd.selectionTag = rng.pick(kMenuTags);
        hd.pTitleText.address = rng.chance(65) ? textGroup(3) : 0u;
        hd.pDialogTxt.address = rng.chance(30) ? textGroup(8) : 0u;
        hd.pOptionsList.address = rng.chance(60) ? textGroup(4) : 0u;
        hd.pOptionsList2.address = rng.chance(40) ? textGroup(4) : 0u;
        hd.pOptionsList3.address = rng.chance(40) ? textGroup(4) : 0u;
        hd.pAdditionalText.address = rng.chance(50) ? textGroup(4) : 0u;
        hd.pColourSet.address = rng.chance(60) ? colourSet() : 0u;
        hd.pBorderSet.address = rng.chance(60) ? s + kBorders : 0u;
        hd.pLinkMenuReturn.address = rng.chance(50) ? header(3) : 0u;
        hd.pGlobalDestMenu.address = rng.chance(30) ? header(2) : 0u;
        hd.pSpecialMenu.address = rng.chance(30) ? header(1) : 0u;
        hd.specialStore = rng.chance(80) ? static_cast<uint32_t>(rng.between(1, 6)) : rng.next();
        hd.x = static_cast<int16>(rng.between(20, 300));
        hd.y = static_cast<int16>(rng.between(8, 120));
        hd.width = static_cast<uint16>(rng.chance(30) ? 0 : rng.between(20, 300));
        hd.height = static_cast<uint16>(rng.chance(30) ? 0 : rng.between(15, 160));
        hd.changeX = static_cast<uint8>(rng.chance(70) ? 0 : rng.between(1, 12));
        hd.changeY = static_cast<uint8>(rng.between(6, 20));
        hd.dlgLines = static_cast<int8>(rng.chance(50) ? -1 : rng.between(0, 3));
        hd.spare = static_cast<int16>(rng.chance(80) ? 0 : rng.between(-12, 12));
        const bool scrolling = !disabling && !unselectable && count > 2 && rng.chance(35);
        hd.scrollItems = static_cast<int8>(scrolling ? rng.between(1, count) : 0);
        hd.scrollStart = static_cast<int8>(scrolling && rng.chance(60) ? rng.between(1, 2) : 0);
        hd.selection = static_cast<int8>(count == 0 ? 0 : rng.chance(92) ? rng.between(0, count - 1) : rng.between(-1, count + 1));
        for (int8 *t = &hd.tofspEnglish; t <= &hd.tofsnSpanish; ++t)
            *t = static_cast<int8>(rng.chance(60) ? 0 : rng.between(-9, 9));
        if (!options.drawn)
        {
            if (rng.chance(8))
                hd.x = static_cast<int16>(rng.next());
            if (rng.chance(8))
                hd.width = static_cast<uint16>(rng.next());
            if (rng.chance(8))
                hd.height = static_cast<uint16>(rng.next());
        }
        // A stored selection: one of the two players' (1, 2) or a byte of its own.
        if (rng.chance(15))
        {
            const uint32_t stored = s + kSelection + static_cast<uint32_t>(slot) * 4u;
            mem<uint8_t>(stored) = static_cast<uint8_t>(count == 0 ? 0 : rng.between(0, count - 1));
            hd.pSelection.address = rng.chance(60) ? stored : static_cast<uint32_t>(rng.between(1, 2));
            hd.pValueStore.address = stored + 1u;
        }
        return h;
    }

    // A scrolling list at a slot: text groups ended by an empty one.
    uint32_t scrollList(int slot)
    {
        const uint32_t h = menu(slot, {0, true, true});
        auto &hd = mem<sAMenuHeader>(h);
        const int lines = rng.between(0, 12);
        const uint32_t list = s + kList;
        for (int i = 0; i < lines; ++i)
        {
            const std::string text = rng.chance(15) ? "-" + phrase(1) : phrase(rng.between(1, 3));
            const uint32_t at = string(text);
            for (uint32_t l = 0; l < 4; ++l)
                mem<uint32_t>(list + static_cast<uint32_t>(i) * 0x10u + l * 4u) = at;
        }
        std::memset(guest_host(list + static_cast<uint32_t>(lines) * 0x10u), 0, 0x10);
        hd.pMenuStart.address = list;
        hd.evenMoreFlags = static_cast<uint16>(hd.evenMoreFlags | 0x40u);
        hd.menuItems = static_cast<uint8>(rng.chance(90) ? lines : rng.between(0, 12));
        hd.selection = static_cast<int8>(lines == 0 ? 0 : rng.between(0, lines - 1));
        hd.height = static_cast<uint16>(rng.between(40, 160));
        hd.scrollStart = static_cast<int8>(rng.between(0, 30));
        hd.pSelection.address = 0;
        if (rng.chance(10))
            hd.x = 0xf5;
        return h;
    }

    // A name-entry menu at a slot: a grid of characters, then OK and cancel, 0xaa between. The last row is neither
    // empty nor one character long (the cursor's search for a character would not end).
    uint32_t areaMenu(int slot, bool cached)
    {
        const uint32_t h = menu(slot, {1, true, true});
        auto &hd = mem<sAMenuHeader>(h);
        const int cols = rng.between(3, 9), rows = rng.between(1, 4), endCol = rng.between(2, cols - 1);
        const int chars = rows * cols + endCol;
        std::string text;
        for (int i = 0; i < chars; ++i)
            text += rng.chance(15) ? static_cast<char>(0xc6) : static_cast<char>('A' + rng.below(26));
        text += static_cast<char>(0xaa);
        text += word(1, 6);
        text += static_cast<char>(0xaa);
        text += word(1, 8);
        const uint32_t at = s + kBuffer;
        std::memcpy(guest_host(at), text.c_str(), text.size() + 1u);
        const uint32_t tg = s + kGroups + (m_groups++ % kGroupCount) * 0x10u;
        for (uint32_t l = 0; l < 4; ++l)
            mem<uint32_t>(tg + l * 4u) = at;
        hd.pCharList.address = tg;
        hd.flags = static_cast<uint16>(hd.flags | 0x4000u);
        hd.scrollItems = static_cast<int8>(cols);
        hd.scrollStart = static_cast<int8>(rng.between(0, 6));
        hd.rowSelect = static_cast<int8>(rng.between(0, rows + 2));
        hd.colSelect = static_cast<int8>(rng.between(0, cols - 1));
        const uint32_t info = kMenuSystem + 0x38u;
        std::memset(guest_host(info), 0, 0x30);
        if (cached)
        {
            // As AMenuAreaMenuSetup leaves it.
            mem<uint32_t>(info) = h;
            mem<uint32_t>(info + 4u) = at + static_cast<uint32_t>(text.rfind(static_cast<char>(0xaa))) + 1u;
            const size_t ok = text.find(static_cast<char>(0xaa)) + 1u;
            const std::string okText = text.substr(ok, text.rfind(static_cast<char>(0xaa)) - ok);
            std::memcpy(guest_host(info + 8u), okText.c_str(), okText.size() + 1u);
            mem<uint16_t>(info + 0x28u) = static_cast<uint16_t>(cols);
            mem<uint16_t>(info + 0x2au) = static_cast<uint16_t>(rows);
            mem<uint16_t>(info + 0x2cu) = static_cast<uint16_t>(endCol);
            mem<uint16_t>(info + 0x2eu) = static_cast<uint16_t>(rows);
            hd.scrollStart = static_cast<int8>(rows);
        }
        else
        {
            // Left from another menu: the counts the handler reads before the grid is laid out again.
            mem<uint16_t>(info + 0x28u) = static_cast<uint16_t>(rng.chance(70) ? cols : rng.between(2, 9));
            mem<uint16_t>(info + 0x2au) = static_cast<uint16_t>(rng.chance(70) ? rows : rng.between(1, 5));
        }
        mem<uint16_t>(kMenuSystem + 0x96u) = static_cast<uint16_t>(rng.between(0, 12));
        return h;
    }

    // Keys: mostly none; else one menu key hit and held on a pad, or a stick pushed.
    void keys(int percentPressed = 55)
    {
        auto &in = mem<inputClass>(kInput);
        for (int i = 0; i < 32; ++i)
        {
            in.inactiveBuffer[i] = static_cast<uint8>(rng.chance(4) ? rng.next() & 0x23u : 0u);
            in.activeBuffer[i] = static_cast<uint8>(rng.chance(4) ? rng.next() & 0x23u : 0u);
        }
        in.analog1A = in.analog1B = in.analog2A = in.analog2B = 0;
        if (rng.chance(static_cast<uint32_t>(percentPressed)))
        {
            static const uint8_t kKeys[] = {4, 6, 7, 5, 0xc, 0xe, 0xe, 0xe, 0xf, 0xd, 3, 4, 6};
            const int presses = rng.chance(85) ? 1 : 2;
            for (int n = 0; n < presses; ++n)
            {
                const uint32_t key = rng.pick(kKeys) + (rng.chance(25) ? 0x10u : 0u);
                const uint8 bits = static_cast<uint8>(rng.chance(75) ? 0x21 : rng.chance(50) ? 0x20 : 0x01);
                in.inactiveBuffer[key] = bits;
                in.activeBuffer[key] = bits;
            }
        }
        else if (rng.chance(30))
        {
            static const int8_t kPush[] = {-128, -100, -81, -80, 80, 81, 100, 127, 0};
            (rng.chance(50) ? in.analog1A : in.analog2A) = rng.pick(kPush);
            (rng.chance(50) ? in.analog1B : in.analog2B) = rng.pick(kPush);
        }
        static const int32_t kStates[] = {0, 1, 2, 6, 6, 6, 2};
        in.padState[0] = rng.pick(kStates);
        in.padState[1] = rng.pick(kStates);
        static const uint8_t kIds[] = {0x41, 0x73, 0x79, 0x53, 0x73, 0x53};
        in.padBuffer1[0] = static_cast<uint8>(rng.chance(85) ? 0 : 0xff);
        in.padBuffer1[1] = rng.pick(kIds);
        in.padBuffer2[0] = static_cast<uint8>(rng.chance(85) ? 0 : 0xff);
        in.padBuffer2[1] = rng.pick(kIds);
        mem<uint8_t>(sym::lastHitPadG) = static_cast<uint8_t>(rng.chance(80) ? rng.between(0, 1) : 0xff); // lastHitPadG
        mem<uint8_t>(sym::lastHeldPadG) = static_cast<uint8_t>(rng.between(0, 1));
    }

    R5900Context *ctx;
    Rng &rng;
    const uint32_t s;
    int language = 0;

private:
    // Everything outside scratch the unit reads, set for every input.
    void globals()
    {
        const Real &r = real();
        // Colours and borders.
        for (uint32_t i = 0; i < 0x10u; ++i)
            mem<uint8_t>(s + kColours + i) = static_cast<uint8_t>(rng.next() & 0x7fu);
        if (inRam(r.borderSet))
            std::memcpy(guest_host(s + kBorders), guest_host(r.borderSet), 0x24);
        if (rng.chance(40))
        {
            mem<uint8_t>(s + kBorders + 0x20u) = static_cast<uint8_t>(rng.between(0, 16));
            mem<uint8_t>(s + kBorders + 0x21u) = static_cast<uint8_t>(rng.between(0, 16));
        }
        // The menu system.
        const uint32_t ms = kMenuSystem;
        mem<uint32_t>(kMenuSystemPtr) = ms;
        mem<uint32_t>(ms + 0x0u) = header(0);
        mem<uint32_t>(ms + 0x4u) = rng.chance(50) ? header(3) : 0u;
        mem<uint32_t>(ms + 0x8u) = colourSet();
        mem<uint32_t>(ms + 0xcu) = s + kBorders;
        mem<uint32_t>(ms + 0x18u) = rng.chance(30) ? s + kBorders : 0u;
        mem<uint32_t>(ms + 0x1cu) = rng.chance(30) ? colourSet() : 0u;
        mem<uint32_t>(ms + 0x24u) = r.keys;
        mem<uint32_t>(ms + 0x28u) = r.tick;
        mem<uint32_t>(ms + 0x2cu) = r.slider;
        mem<uint32_t>(ms + 0x34u) = rng.chance(35) ? kSelectionTest : 0u;
        std::memset(guest_host(ms + 0x38u), 0, 0x30);
        static const uint32_t kCodes[] = {0x11000000u, 0x11000001u, 0x11000002u, 0x11000003u, 0x11000004u, 0};
        mem<uint32_t>(ms + 0x68u) = rng.chance(70) ? 0u : rng.next();
        mem<uint32_t>(ms + 0x6cu) = rng.pick(kCodes);
        mem<int16_t>(ms + 0x70u) = static_cast<int16_t>(rng.between(0, 5));
        mem<int16_t>(ms + 0x72u) = static_cast<int16_t>(rng.between(0, 5));
        mem<int16_t>(ms + 0x80u) = 1;
        for (uint32_t tab = 0; tab < 10; ++tab)
            mem<int16_t>(ms + 0x82u + tab * 2u) = static_cast<int16_t>(tab * 40u + rng.below(30));
        mem<uint16_t>(ms + 0x96u) = static_cast<uint16_t>(rng.between(0, 12));
        mem<int16_t>(ms + 0x98u) = static_cast<int16_t>(rng.between(0, 40));
        for (uint32_t offset = 0x9c; offset < 0xc4; ++offset)
            mem<uint8_t>(ms + offset) = static_cast<uint8_t>(rng.next());
        const std::string typed = word(0, 10);
        std::memset(guest_host(ms + 0xc4u), 0, 40);
        std::memcpy(guest_host(ms + 0xc4u), typed.c_str(), typed.size());
        mem<int8_t>(ms + 0xecu) = static_cast<int8_t>(rng.between(-1, 6));
        static const int8_t kSmooth[] = {0, 0, 0, 0, 0, 1, -1, 2, -2, 7, -7, 12, -12};
        mem<int8_t>(ms + 0xedu) = rng.pick(kSmooth);
        mem<int8_t>(ms + 0xeeu) = static_cast<int8_t>(rng.chance(80) ? 0 : rng.between(-1, 3));
        mem<int8_t>(ms + 0xefu) = static_cast<int8_t>(rng.between(0, 1));
        mem<int8_t>(ms + 0xf0u) = 0;
        mem<int8_t>(ms + 0xf1u) = static_cast<int8_t>(rng.chance(80) ? 0 : rng.between(1, 2));
        for (uint32_t offset = 0xf2; offset < 0xf6; ++offset)
            mem<uint8_t>(ms + offset) = static_cast<uint8_t>(rng.between(0, 4));
        mem<uint8_t>(ms + 0xf6u) = static_cast<uint8_t>(rng.chance(75) ? 0 : rng.between(1, 3));
        // The camera whose movement hides menus and resets the keys.
        mem<uint32_t>(kCameraPtr) = s + kCamera;
        mem<int32_t>(s + kCamera + 0xd8u) = rng.chance(70) ? -1 : rng.between(0, 5);
        // Text tables: through the level, or the Track Editor's when there is none.
        for (uint32_t i = 0; i < 0x44u; ++i)
            mem<uint32_t>(s + kGeneral + i * 4u) = rng.chance(3) ? 0u : group([&] { return i == 0x17u && rng.chance(50) ? phrase(1) + "  " : phrase(rng.between(1, 2)); });
        for (uint32_t i = 0; i < 0x30u; ++i)
            mem<uint32_t>(s + kCars + i * 4u) = s + kStrings + (i % kStringCount) * kStringSize;
        mem<uint32_t>(s + kLevel + 0x2cu) = s + kGeneral;
        mem<uint32_t>(s + kLevel + 0x58u) = s + kCars;
        mem<uint32_t>(kTEditGeneralText) = s + kGeneral;
        mem<uint32_t>(kTEditCarNames) = s + kCars;
        mem<uint32_t>(kLevelPtr) = rng.chance(80) ? s + kLevel : 0xffffffffu;
        // The front end's variables.
        const uint32_t fe = kFrontEnd;
        mem<uint32_t>(fe) = s + kFELevel;
        // The tracks' names and pictures: levels 1 to 24, and a few entries either side (the earlier page builders
        // start from the jump position, which can be -1).
        mem<uint32_t>(s + kFELevel + 0xfcu) = s + kExtra + 0x40u;
        for (uint32_t level = 0; level < 30; ++level)
        {
            mem<uint32_t>(s + kExtra + level * 0x10u) = rng.chance(3) ? 0u : group([&] { return phrase(rng.between(1, 4), true); });
            mem<uint32_t>(s + kExtra + level * 0x10u + 4u) = rng.chance(50) ? r.keys : r.tick;
        }
        // The box and border fiddle factors.
        for (uint32_t address = sym::ffRectLeftG; address <= sym::ffBordBottomG; address += 4u)
            mem<int32_t>(address) = rng.chance(50) ? 0 : rng.between(0, 6);
        for (uint32_t offset = 0x188; offset <= 0x194; offset += 4)
            mem<int32_t>(fe + offset) = rng.between(0, 3);
        mem<int16_t>(fe + 0x264u) = static_cast<int16_t>(rng.between(-12, 32));
        mem<int16_t>(fe + 0x266u) = static_cast<int16_t>(rng.between(-12, 32));
        mem<uint32_t>(fe + 0x2a8u) = rng.below(600);
        mem<int16_t>(fe + 0x2c0u) = static_cast<int16_t>(rng.between(-10, 30));
        mem<int16_t>(fe + 0x2c4u) = static_cast<int16_t>(rng.between(-10, 30));
        mem<int16_t>(fe + 0x2c8u) = static_cast<int16_t>(rng.chance(40) ? 0 : rng.between(-30, 30));
        mem<int16_t>(fe + 0x2cau) = static_cast<int16_t>(rng.chance(40) ? 0 : rng.between(-600, 40));
        mem<int16_t>(fe + 0x2ccu) = static_cast<int16_t>(rng.chance(50) ? 0 : rng.between(-1, 900));
        mem<int16_t>(fe + 0x2ceu) = static_cast<int16_t>(rng.between(0, 20));
        mem<int8_t>(fe + 0x2d0u) = 1;
        mem<int8_t>(fe + 0x2d1u) = static_cast<int8_t>(rng.chance(85) ? 0 : rng.between(-1, 2));
        mem<int8_t>(fe + 0x2d2u) = static_cast<int8_t>(rng.chance(50) ? -1 : rng.between(20, 110));
        mem<int8_t>(fe + 0x2d3u) = static_cast<int8_t>(rng.chance(50) ? -1 : rng.between(20, 100));
        mem<int8_t>(fe + 0x2d4u) = static_cast<int8_t>(rng.chance(25) ? 1 : 0);
        mem<int8_t>(fe + 0x2d5u) = static_cast<int8_t>(rng.chance(25) ? 1 : 0);
        mem<uint8_t>(fe + 0x2dau) = static_cast<uint8_t>(rng.chance(30) ? 1 : 0);
        static const int32_t kHeights[] = {-1, -1, 30, 64, 90, 140};
        mem<int32_t>(sym::amenuScrollTTHeightG) = rng.pick(kHeights); // the entry heights measured on a page's first pass
        mem<int32_t>(sym::amenuScrollProgHeaderHeightG) = rng.pick(kHeights);
        mem<int32_t>(sym::amenuScrollProgEntryHeightG) = rng.pick(kHeights);
        mem<int32_t>(sym::bottomOfScrollG) = rng.between(0, 1);  // bottomOfScrollG
        mem<int32_t>(sym::scrollLinesG) = rng.chance(60) ? 0 : rng.between(0, 30); // scrollLinesG
        const int points = rng.chance(70) ? rng.between(0, 8) : rng.between(20, 40);
        for (int i = 0; i < 50; ++i)
            mem<int32_t>(kFastScroll + static_cast<uint32_t>(i) * 4u) = i < points ? -rng.between(0, 900) : -1;
        mem<int32_t>(sym::scrollJumpPosG) = points == 0 ? 0 : rng.between(0, points - 1); // scrollJumpPosG
        mem<int32_t>(sym::fixIDSPdelay1G) = rng.chance(70) ? 0 : 0x1000028b;              // the delayed vibration checks
        mem<int32_t>(sym::fixIDSPdelay2G) = rng.chance(70) ? 0 : 0x10000078;
        mem<int32_t>(sym::areaMenuPauseG) = rng.chance(70) ? 0 : rng.between(0, 3);        // areaMenuPauseG
        mem<uint32_t>(sym::pLastHeader_630) = rng.chance(50) ? header(0) : 0u;              // pLastHeader
        // Settings the option menus show.
        mem<int32_t>(kMusicUserVol) = rng.between(0, 16383);
        mem<int32_t>(kSfxUserVol) = rng.between(0, 255);
        mem<int32_t>(kMusicVolG) = rng.between(0, 16383);
        mem<int32_t>(kSfxVolG) = rng.between(0, 255);
        mem<int32_t>((sym::sfxVarG + 0x8u)) = rng.between(0, 2);
        for (uint32_t offset = 0x3df; offset <= 0x3e4; ++offset)
            mem<uint8_t>(kMainVars + offset) = static_cast<uint8_t>(rng.between(0, 5));
        mem<int32_t>(s + kGameState + 0x124u) = rng.chance(85) ? rng.between(0, 8) : rng.between(-2, 12);
        // The packet buffer's open tag: kept, or none.
        if (rng.chance(40))
            mem<int32_t>(kGfx + 0x18u) = -1;
        keys();
    }

    int m_items = 0;
    uint32_t m_strings = 0x30u, m_groups = 0;
};

uint32_t screenX(Rng &rng) { return static_cast<uint32_t>(rng.between(0, 500)); }
uint32_t screenY(Rng &rng) { return static_cast<uint32_t>(rng.between(0, 200)); }

// The world with the four menus, the first of any kind the options allow.
struct Menus
{
    World w;
    uint32_t h0, h1, h2, h3;
    Menus(R5900Context *ctx, Rng &rng, uint32_t scratch, const Options &options = Options()) : w(ctx, rng, scratch)
    {
        h3 = w.menu(3, {rng.between(0, 2), options.drawn, true});
        h2 = w.menu(2, {rng.between(0, 3), options.drawn, true});
        h1 = w.menu(1, {rng.between(0, 2), options.drawn, true});
        h0 = w.menu(0, options);
    }
};

} // namespace

void amenuHeader(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch);
    setArg(ctx, 0, m.h0);
    static const uint32_t kValues[] = {0, 1, 2, 4, 0x20, 0x800, 0x1000, 0x2000, 0x4000, 0xffff, 0xfffffffcu, 3, 60, 200};
    setArg(ctx, 1, rng.chance(60) ? rng.pick(kValues) : rng.next() >> rng.below(28));
}

void amenuItemIndex(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch);
    setArg(ctx, 0, m.h0);
    setArg(ctx, 1, static_cast<uint32_t>(rng.between(-2, 8)));
}

void amenuAddItem(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch);
    setArg(ctx, 0, m.h0);
    setArg(ctx, 1, m.w.item(11));
    std::memset(guest_host(m.w.item(11)), 0, 0x18);
}

void amenuLink(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    const int items = rng.between(0, 6);
    Menus m(ctx, rng, scratch, {items, false, false});
    setArg(ctx, 0, m.h0);
    setArg(ctx, 1, static_cast<uint32_t>(items == 0 || rng.chance(20) ? -1 : rng.between(0, items - 1)));
    setArg(ctx, 2, rng.chance(80) ? m.h1 : 0u);
    setArg(ctx, 3, rng.below(4));
    setArg(ctx, 4, rng.chance(50) ? rng.pick(kItemTags) : rng.next());
}

void amenuMinSel(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch);
    setArg(ctx, 0, m.h0);
    for (int i = 0; i < 3; ++i)
    {
        const uint32_t counter = scratch + kCounters + static_cast<uint32_t>(i) * 4u;
        mem<int32_t>(counter) = rng.chance(70) ? 0 : rng.between(-3, 12);
        setArg(ctx, 1 + i, counter);
    }
}

void amenuItem(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    const int items = rng.between(1, 6);
    Menus m(ctx, rng, scratch, {items, false, false});
    const int index = rng.between(0, items - 1);
    setArg(ctx, 0, m.h0);
    setArg(ctx, 1, m.w.item(index));
    setArg(ctx, 2, m.w.colourSet());
    setArg(ctx, 3, static_cast<uint32_t>(rng.chance(70) ? index : rng.between(0, 6)));
    // The vibration items and their delayed checks, more often than tags come up by themselves.
    if (rng.chance(40))
    {
        const uint32_t tag = rng.chance(50) ? 0x1000028bu : 0x10000078u;
        mem<uint32_t>(m.w.item(index) + 0x10u) = tag;
        if (rng.chance(60))
            mem<uint32_t>(tag == 0x1000028bu ? sym::fixIDSPdelay1G : sym::fixIDSPdelay2G) = tag;
    }
}

void amenuChange(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch, {rng.between(1, 6), false, false});
    setArg(ctx, 0, m.h0);
    setArg(ctx, 1, static_cast<uint32_t>(rng.chance(90) ? rng.between(1, 2) : rng.between(-1, 4)));
    setArg(ctx, 2, rng.below(2));
}

void amenuPadTest(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch);
    static const uint32_t kPads[] = {0, 5, 0, 5, 1, 4};
    setArg(ctx, 0, rng.pick(kPads));
    setArg(ctx, 1, rng.chance(40) ? mem<uint32_t>(m.h0 + 0x14u) : rng.chance(50) ? 0u : rng.pick(kMenuTags));
    setArg(ctx, 2, m.h0);
}

void amenuNone(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch);
    if (rng.chance(50))
        mem<uint16_t>(m.h0 + 0x30u) |= 0x2000u;
}

void amenuTextGroup(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch);
    setArg(ctx, 0, rng.chance(15) ? 0u : m.w.textGroup(3));
    setArg(ctx, 1, m.w.string(m.w.phrase(2)));
}

void amenuMessage(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch);
    setArg(ctx, 0, static_cast<uint32_t>(rng.between(0, 0x2f)));
}

void amenuStrings(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch);
    static const char *const kTexts[] = {"Laps: %s", "%s", "%", "ab%", "%sx", "x%d%s", "sss", "ass%s s", "", "Music volume:", "a%%s"};
    setArg(ctx, 0, scratch + kBuffer);
    setArg(ctx, 1, m.w.string(rng.chance(70) ? std::string(rng.pick(kTexts)) : m.w.phrase(2) + " %s"));
}

void amenuCentre(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch);
    setArg(ctx, 0, m.h0);
    setArg(ctx, 1, m.w.string(m.w.phrase(rng.between(1, 3))));
    setArg(ctx, 2, rng.chance(50) ? 0xffffffffu : rng.below(400));
}

void amenuInput(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch);
    m.w.keys(80);
    setArg(ctx, 0, static_cast<uint32_t>(rng.chance(92) ? rng.between(-1, 1) : rng.between(-3, 3)));
    setArg(ctx, 1, static_cast<uint32_t>(rng.chance(92) ? rng.between(2, 3) : rng.between(0, 4)));
    setArg(ctx, 2, rng.chance(85) ? m.h0 : 0u);
}

void amenuProcess(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch);
    m.w.keys(85);
    if (rng.chance(15))
        mem<uint32_t>(kMenuSystem) = rng.chance(50) ? m.h1 : m.h3;
    // An item that only has a tag: what select does then depends on the item's and the menu's tags.
    if (rng.chance(30))
    {
        auto &hd = mem<sAMenuHeader>(m.h0);
        const uint32_t it = hd.pMenuStart.address;
        if (it != 0)
        {
            uint32_t at = it;
            for (int i = 0; i < hd.selection && mem<uint32_t>(at + 8u) != 0; ++i)
                at = mem<uint32_t>(at + 8u);
            static const uint32_t kTags[] = {0x10000223u, 0x10000224u, 0x10000222u, 0x10000225u, 0x10000040u};
            mem<uint32_t>(at + 0x10u) = rng.pick(kTags);
            mem<uint8_t>(at + 0x16u) = static_cast<uint8_t>(rng.chance(50) ? 2 : 0);
            mem<uint32_t>(at + 0xcu) = 0;
            mem<uint16_t>(at + 0x14u) &= static_cast<uint16_t>(~0x15u);
            if (rng.chance(70))
                hd.pGlobalDestMenu.address = 0;
            static const uint32_t kMenuTagsHere[] = {0, 0x10000282u, 0x10000284u, 0x1000028fu, 0x10000290u, 0x10000217u};
            hd.selectionTag = rng.pick(kMenuTagsHere);
            // Select, on the pad the menu listens to.
            auto &in = mem<inputClass>(kInput);
            if (rng.chance(70))
                in.inactiveBuffer[0xe] = in.activeBuffer[0xe] = 0x21;
            mem<int8_t>(kMenuSystem + 0xf1u) = 0;
        }
    }
    setArg(ctx, 0, m.h0);
    setArg(ctx, 1, static_cast<uint32_t>(rng.chance(85) ? 0 : rng.between(1, 2)));
}

void amenuProcessScroll(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch);
    const uint32_t h = m.w.scrollList(0);
    m.w.keys(85);
    setArg(ctx, 0, h);
    setArg(ctx, 1, 0);
}

void amenuProcessArea(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch);
    const uint32_t h = m.w.areaMenu(0, rng.chance(60));
    m.w.keys(90);
    setArg(ctx, 0, h);
}

void amenuSizeItem(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    const int items = rng.between(1, 6);
    Menus m(ctx, rng, scratch, {items, true, false});
    setArg(ctx, 0, m.w.item(rng.between(0, items - 1)));
    setArg(ctx, 1, static_cast<uint32_t>(rng.chance(40) ? 0 : rng.between(0, 300)));
}

// a0 a header of any kind; now and then a scrolling one whose height gives 256 lines (the count is a byte).
void amenuSize(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch, {-1, true, false});
    const int kind = rng.chance(60) ? 0 : rng.between(1, 2);
    const uint32_t h = kind == 1 ? m.w.scrollList(0) : kind == 2 ? m.w.areaMenu(0, rng.chance(40)) : m.h0;
    auto &hd = mem<sAMenuHeader>(h);
    if (rng.chance(25))
    {
        hd.flags = static_cast<uint16>(hd.flags | 0x2000u);
        const int lines = rng.chance(50) ? 256 : rng.pick({1, 2, 255, 257, 512});
        hd.height = static_cast<uint16>(std::min(0xffff, (lines + 1) * hd.changeY + rng.between(0, hd.changeY - 1)));
    }
    setArg(ctx, 0, h);
}

void amenuAreaHeader(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch, {-1, true, false});
    setArg(ctx, 0, m.w.areaMenu(0, rng.chance(40)));
}

namespace {

// A header to draw: ordinary, a scrolling list or a name-entry menu by kind (0, 1, 2), with its parent ordinary.
uint32_t drawable(Menus &m, int kind)
{
    Rng &rng = m.w.rng;
    uint32_t h = m.h0;
    if (kind == 1)
        h = m.w.scrollList(0);
    else if (kind == 2)
        h = m.w.areaMenu(0, rng.chance(40));
    // The parent is drawn behind a menu with flag 2: never a list (which has no items to walk).
    return h;
}

} // namespace

void amenuDraw(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch, {-1, true, false});
    const uint32_t h = drawable(m, rng.chance(60) ? 0 : rng.between(1, 2));
    if (rng.chance(25))
        mem<int8_t>(kMenuSystem + 0xf0u) = 1; // the screen-adjust mode
    setArg(ctx, 0, h);
    setArg(ctx, 1, rng.below(4));
}

void amenuDrawOrdinary(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch, {-1, true, false});
    setArg(ctx, 0, m.h0);
    setArg(ctx, 1, rng.below(4));
}

void amenuDrawScroll(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch, {-1, true, false});
    setArg(ctx, 0, m.w.scrollList(0));
    // The list's lines step by the active menu's line height, which is another menu's now and then.
    if (rng.chance(30))
        mem<uint32_t>(kMenuSystem) = m.h1;
}

void amenuDrawTitle(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch, {-1, true, false});
    setArg(ctx, 0, m.h0);
    setArg(ctx, 1, m.w.colourSet());
    setArg(ctx, 2, rng.chance(40) ? 0u : screenY(rng));
}

void amenuDrawItem(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    const int items = rng.between(1, 6);
    Menus m(ctx, rng, scratch, {items, true, false});
    setArg(ctx, 0, m.h0);
    setArg(ctx, 1, m.w.item(rng.between(0, items - 1)));
    setArg(ctx, 2, m.w.colourSet());
    setArg(ctx, 3, screenX(rng));
    setArg(ctx, 4, screenY(rng));
    setArg(ctx, 5, 0);
}

void amenuDrawDialog(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch, {-1, true, false});
    setArg(ctx, 0, screenX(rng));
    setArg(ctx, 1, screenY(rng));
    setArg(ctx, 2, m.h0);
    setArg(ctx, 3, m.w.colourSet());
}

void amenuDrawTabs(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch);
    std::string text;
    for (int n = rng.between(0, 5); n > 0; --n)
    {
        text += m.w.word(0, 5);
        if (n > 1 || rng.chance(20))
            text += static_cast<char>(0xaa);
    }
    setArg(ctx, 0, screenX(rng));
    setArg(ctx, 1, screenY(rng));
    setArg(ctx, 2, scratch + kBuffer);
    setArg(ctx, 3, m.w.string(text));
}

void amenuBar(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch);
    const uint32_t bar = scratch + kBar;
    auto &b = mem<sBarDetails>(bar);
    b.x = static_cast<int16>(screenX(rng));
    b.y = static_cast<int16>(screenY(rng));
    b.flags = static_cast<uint16>(rng.next());
    b.maxValue = static_cast<int16>(rng.chance(85) ? rng.between(1, 300) : rng.between(-300, -1));
    b.minValue = 0;
    b.value = static_cast<int16>(rng.chance(85) ? rng.between(0, 300) : rng.between(-300, 600));
    b.width = static_cast<uint16>(rng.between(0, 300));
    b.height = static_cast<uint16>(rng.between(0, 40));
    setArg(ctx, 0, bar);
}

void amenuClip(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch, {-1, true, false});
    setArg(ctx, 0, m.h0);
    setArg(ctx, 1, static_cast<uint32_t>(rng.between(-10, 40)));
    setArg(ctx, 2, static_cast<uint32_t>(rng.between(-10, 40)));
}

void amenuTick(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch);
    setArg(ctx, 0, screenX(rng));
    setArg(ctx, 1, screenY(rng));
    setArg(ctx, 2, static_cast<uint32_t>(rng.chance(70) ? rng.between(0, 2) : rng.between(-1, 15)));
}

void amenuTickText(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch, {-1, true, false});
    setArg(ctx, 0, m.h0);
    setArg(ctx, 1, screenX(rng));
    setArg(ctx, 2, screenY(rng));
    setArg(ctx, 3, static_cast<uint32_t>(rng.between(-1, 3)));
    setArg(ctx, 4, m.w.string(m.w.phrase(rng.between(1, 2))));
    setArg(ctx, 5, static_cast<uint32_t>(rng.chance(90) ? rng.between(1, 4) : rng.between(0, 6)));
}

void amenuTrack(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch);
    static const int32_t kScales[] = {0x100, 0x100, 0x80, 0x200, 0x101, -1, -4, 0, 0x7f};
    setArg(ctx, 0, screenX(rng));
    setArg(ctx, 1, screenY(rng));
    setArg(ctx, 2, static_cast<uint32_t>(rng.pick(kScales)));
    setArg(ctx, 3, static_cast<uint32_t>(rng.between(80, 480)));
    setArg(ctx, 4, static_cast<uint32_t>(rng.between(1, 24)));
}

namespace {

// A page's box: an ordinary header made as the pages make theirs.
uint32_t pageBox(Menus &m, uint32_t at)
{
    Rng &rng = m.w.rng;
    std::memcpy(guest_host(at), guest_host(m.h0), 0x80);
    auto &hd = mem<sAMenuHeader>(at);
    hd.pMenuStart.address = 0;
    hd.pTitleText.address = 0;
    hd.pDialogTxt.address = 0;
    hd.flags = 0x820;
    hd.x = static_cast<int16>(rng.between(20, 100));
    hd.y = static_cast<int16>(rng.chance(70) ? 0x14 : rng.between(0, 60));
    hd.width = static_cast<uint16>(rng.chance(70) ? 0x1e0 : rng.between(100, 500));
    hd.height = static_cast<uint16>(rng.chance(70) ? 0x96 : rng.between(40, 200));
    hd.changeY = static_cast<uint8>(rng.between(6, 16));
    return at;
}

uint32_t pageY(Rng &rng) { return static_cast<uint32_t>(rng.chance(75) ? rng.between(-150, 260) : rng.between(-2000, 2000)); }

} // namespace

void amenuProgHeader(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch, {-1, true, false});
    const uint32_t box = pageBox(m, kBox);
    setArg(ctx, 0, box);
    setArg(ctx, 1, scratch + kGameState);
    setArg(ctx, 2, static_cast<uint32_t>(mem<int16_t>(box + 0x64u)));
    setArg(ctx, 3, pageY(rng));
}

void amenuProgEntry(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch, {-1, true, false});
    const uint32_t box = pageBox(m, kBox);
    setArg(ctx, 0, box);
    setArg(ctx, 1, scratch + kGameState);
    setArg(ctx, 2, static_cast<uint32_t>(mem<int16_t>(box + 0x64u)));
    setArg(ctx, 3, pageY(rng));
    setArg(ctx, 4, 0x1e0);
    setArg(ctx, 5, static_cast<uint32_t>(rng.between(1, 24)));
}

void amenuTTEntry(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch, {-1, true, false});
    const uint32_t box = pageBox(m, kBox);
    setArg(ctx, 0, box);
    setArg(ctx, 1, static_cast<uint32_t>(mem<int16_t>(box + 0x64u)));
    setArg(ctx, 2, pageY(rng));
    setArg(ctx, 3, 0x1e0);
    setArg(ctx, 4, static_cast<uint32_t>(rng.between(1, 24)));
}

void amenuPageSub(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch, {-1, true, false});
    m.w.keys(70);
    // The box as an earlier frame left it, or (now and then) the source itself.
    const uint32_t box = pageBox(m, rng.chance(90) ? kBox : m.h1);
    setArg(ctx, 0, m.h0);
    setArg(ctx, 1, box);
    setArg(ctx, 2, static_cast<uint32_t>(rng.chance(60) ? 0x1e0 : rng.between(100, 500)));
    setArg(ctx, 3, rng.chance(20) ? 0u : m.w.string(m.w.phrase(rng.between(1, 6))));
}

void amenuPageScroll(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch, {-1, true, false});
    m.w.keys(85);
    setArg(ctx, 0, pageBox(m, kBox));
    setArg(ctx, 1, static_cast<uint32_t>(rng.chance(85) ? rng.between(1, 8) : rng.between(-2, 12)));
}

void amenuPage(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch, {-1, true, false});
    m.w.keys(70);
    pageBox(m, kBox);
    // The Track Editor's help: headings and paragraphs, their heights known or not.
    const uint32_t help = scratch + kHelp;
    const int paragraphs = rng.between(0, 8);
    const bool measured = rng.chance(50);
    for (int i = 0; i < paragraphs; ++i)
    {
        const bool heading = rng.chance(30);
        mem<uint16_t>(help + static_cast<uint32_t>(i) * 8u) = static_cast<uint16_t>(heading ? 1 : rng.chance(50) ? 0 : 2);
        mem<uint16_t>(help + static_cast<uint32_t>(i) * 8u + 2u) = static_cast<uint16_t>(measured ? rng.between(8, 60) : 0);
        mem<uint32_t>(help + static_cast<uint32_t>(i) * 8u + 4u) = rng.chance(4) ? 0u : m.w.textGroup(heading ? 2 : 8);
    }
    mem<uint16_t>(help + static_cast<uint32_t>(paragraphs) * 8u) = 0xffff;
    mem<uint16_t>(help + static_cast<uint32_t>(paragraphs) * 8u + 2u) = 0;
    mem<uint32_t>(kTEditHelp) = help;
    setArg(ctx, 0, m.h0);
    setArg(ctx, 1, scratch + kGameState);
    static const int32_t kCards[] = {-1, -1, 1, 2, 0, 3};
    setArg(ctx, 2, static_cast<uint32_t>(rng.pick(kCards)));
    // BuildProgress takes the page's offset from $s2 for a card that is none of its three.
    SET_GPR_S32(ctx, 18, rng.between(-40, 60));
}

void amenuHelpEntry(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch, {-1, true, false});
    const uint32_t box = pageBox(m, kBox);
    const uint32_t help = scratch + kHelp;
    mem<uint16_t>(help) = static_cast<uint16_t>(rng.chance(40) ? 1 : rng.below(3));
    mem<uint16_t>(help + 2u) = static_cast<uint16_t>(rng.between(0, 80));
    mem<uint32_t>(help + 4u) = rng.chance(8) ? 0u : m.w.textGroup(8);
    setArg(ctx, 0, box);
    setArg(ctx, 1, help);
    setArg(ctx, 2, static_cast<uint32_t>(mem<int16_t>(box + 0x64u)));
    setArg(ctx, 3, pageY(rng));
    setArg(ctx, 4, 0x168);
    setArg(ctx, 5, rng.below(8));
}

void amenuLine(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch, {-1, true, false});
    setArg(ctx, 0, m.h0);
    setArg(ctx, 1, screenY(rng));
}

void amenuSpriteBorder(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch);
    const uint32_t params = scratch + kSpriteParams;
    auto &p = mem<_sSpriteParams>(params);
    p.x = static_cast<int16>(screenX(rng));
    p.y = static_cast<int16>(screenY(rng));
    p.width = static_cast<uint16>(rng.between(0, 200));
    p.height = static_cast<uint16>(rng.between(0, 120));
    setArg(ctx, 0, static_cast<uint32_t>(rng.between(-4, 12)));
    setArg(ctx, 1, params);
}

void amenuRaceTick(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch);
    setArg(ctx, 0, scratch + kGameState);
    setArg(ctx, 1, static_cast<uint32_t>(rng.chance(90) ? rng.between(1, 48) : rng.between(-30, 200)));
    setArg(ctx, 2, static_cast<uint32_t>(rng.chance(80) ? rng.between(1, 3) : rng.between(0, 0xff)));
}

void amenuInit(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    Rng rng{state};
    Menus m(ctx, rng, scratch);
    for (uint32_t offset = 0; offset < 0xf8u; ++offset)
        mem<uint8_t>(kMenuSystem + offset) = static_cast<uint8_t>(rng.next());
    mem<uint32_t>(kMenuSystemPtr) = rng.chance(50) ? 0u : kMenuSystem;
    mem<uint32_t>(kMainVars + 0x16cu) = rng.next();
}

} // namespace rc::fuzz

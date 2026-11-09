// Installs decompiled functions into the running game and checks them against the recompiled ones.
//
//   RC_DECOMP=0         keep every recompiled function
//   RC_SHADOW=1         compare sampled calls against the recompiled originals
//   RC_SHADOW_FUZZ=n    also run each function on n random inputs, once the game has started
//   RC_SHADOW_LOG=file  the running totals, rewritten every 3 seconds (default: decomp_shadow.txt)

#include "rc_decomp.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace rc {

uint8_t *g_rdram = nullptr;
PS2Runtime *g_runtime = nullptr;

uint32_t hw_read32(uint32_t address)
{
    return g_runtime->memory().read32(address);
}

namespace {

struct Registered
{
    Entry *entry;
    RecompFn shadow;
};

std::vector<Registered> &registry()
{
    static std::vector<Registered> entries;
    return entries;
}

constexpr uint32_t kStackScratch = 0x2000u; // The dead area below $sp that the recompiled code may use.
constexpr uint64_t kAlwaysCompare = 64u;    // Every function's first calls are always compared.
constexpr int kSamplesPerSecond = 15;       // Each comparison scans 32 MB several times.

bool g_comparing = false; // Only the guest executor thread comes here.
std::unique_ptr<uint8_t[]> g_before;
std::unique_ptr<uint8_t[]> g_expected;

bool sampleThisCall(uint64_t callIndex)
{
    if (callIndex < kAlwaysCompare)
        return true;
    using Clock = std::chrono::steady_clock;
    static Clock::time_point windowStart = Clock::now();
    static int windowCount = 0;
    const Clock::time_point now = Clock::now();
    if (now - windowStart >= std::chrono::seconds(1))
    {
        windowStart = now;
        windowCount = 0;
    }
    if (windowCount >= kSamplesPerSecond)
        return false;
    ++windowCount;
    return true;
}

// First differing byte in [begin, end), or end.
uint32_t firstDifference(const uint8_t *a, const uint8_t *b, uint32_t begin, uint32_t end)
{
    constexpr uint32_t kBlock = 4096u;
    for (uint32_t pos = begin; pos < end;)
    {
        const uint32_t size = std::min(kBlock, end - pos);
        if (std::memcmp(a + pos, b + pos, size) != 0)
        {
            for (uint32_t i = 0; i < size; ++i)
            {
                if (a[pos + i] != b[pos + i])
                    return pos + i;
            }
        }
        pos += size;
    }
    return end;
}

void revertWrites(uint8_t *ram, const uint8_t *before, uint32_t begin, uint32_t end)
{
    constexpr uint32_t kBlock = 4096u;
    for (uint32_t pos = begin; pos < end; pos += kBlock)
    {
        const uint32_t size = std::min(kBlock, end - pos);
        if (std::memcmp(ram + pos, before + pos, size) != 0)
            std::memcpy(ram + pos, before + pos, size);
    }
}

void reportMismatch(const Entry &entry, const R5900Context &in, const char *what, uint64_t original, uint64_t decompiled,
                    uint32_t address)
{
    if (entry.mismatches.load(std::memory_order_relaxed) > 8u)
        return;
    std::fprintf(stderr,
                 "[decomp] MISMATCH %s (0x%x): %s original=0x%llx decompiled=0x%llx at 0x%x | "
                 "a0=%x a1=%x a2=%x a3=%x f12=%g f13=%g ra=%x\n",
                 entry.name, entry.address, what, (unsigned long long)original, (unsigned long long)decompiled, address,
                 getRegU32(&in, 4), getRegU32(&in, 5), getRegU32(&in, 6), getRegU32(&in, 7), in.f[12], in.f[13],
                 getRegU32(&in, 31));
}

void writeSummary(const std::string &path)
{
    std::ofstream out(path, std::ios::trunc);
    out << "calls compared mismatches address name\n";
    uint64_t totalCompared = 0u, totalMismatches = 0u;
    for (const Registered &item : registry())
    {
        const Entry &entry = *item.entry;
        const uint64_t compared = entry.compared.load(std::memory_order_relaxed);
        const uint64_t mismatches = entry.mismatches.load(std::memory_order_relaxed);
        totalCompared += compared;
        totalMismatches += mismatches;
        out << entry.calls.load(std::memory_order_relaxed) << ' ' << compared << ' ' << mismatches << " 0x" << std::hex
            << entry.address << std::dec << ' ' << entry.name << '\n';
    }
    out << "total compared " << totalCompared << " mismatches " << totalMismatches << '\n';
}

// Random inputs reach the code that a demo race does not. Each input runs through the original and the
// decompiled function from identical state, comparing the result, the scratch area and the game's globals.
void fuzzAll(uint8_t *rdram, const R5900Context &live, PS2Runtime *runtime, int iterations)
{
    constexpr uint32_t kScratch = 0x01F00000u, kScratchSize = 0x8000u;
    constexpr uint32_t kDataStart = 0x001E9400u, kDataEnd = 0x004C7190u;
    std::unique_ptr<uint8_t[]> backup(new uint8_t[PS2_RAM_SIZE]);
    std::memcpy(backup.get(), rdram, PS2_RAM_SIZE);
    std::unique_ptr<uint8_t[]> scratchStart(new uint8_t[kScratchSize]), globalsStart(new uint8_t[kDataEnd - kDataStart]);
    std::unique_ptr<uint8_t[]> scratchAfter(new uint8_t[kScratchSize]), globalsAfter(new uint8_t[kDataEnd - kDataStart]);

    uint64_t failures = 0u;
    for (const Registered &item : registry())
    {
        Entry &entry = *item.entry;
        if (entry.randomiseArgs == nullptr)
            continue;
        uint64_t state = 0x5EEDu + entry.address;
        uint64_t mismatches = 0u;
        for (int i = 0; i < iterations; ++i)
        {
            for (uint32_t pos = 0u; pos < kScratchSize; pos += 4u)
            {
                // Mostly small values: fixed-point vectors are 16-bit, and huge ones only overflow.
                const uint32_t word = (nextFuzz(state) & 1u) != 0u ? nextFuzz(state) : (nextFuzz(state) & 0x0FFF0FFFu);
                std::memcpy(rdram + kScratch + pos, &word, 4u);
            }
            std::memcpy(scratchStart.get(), rdram + kScratch, kScratchSize);
            std::memcpy(globalsStart.get(), rdram + kDataStart, kDataEnd - kDataStart);

            R5900Context inputs = live;
            entry.randomiseArgs(&inputs, state, kScratch);

            R5900Context originalCtx = inputs;
            entry.original(rdram, &originalCtx, runtime);
            if (originalCtx.pc != getRegU32(&inputs, 31))
            {
                std::memcpy(rdram + kScratch, scratchStart.get(), kScratchSize);
                std::memcpy(rdram + kDataStart, globalsStart.get(), kDataEnd - kDataStart);
                continue; // Cut short by a scheduling point.
            }
            std::memcpy(scratchAfter.get(), rdram + kScratch, kScratchSize);
            std::memcpy(globalsAfter.get(), rdram + kDataStart, kDataEnd - kDataStart);
            std::memcpy(rdram + kScratch, scratchStart.get(), kScratchSize);
            std::memcpy(rdram + kDataStart, globalsStart.get(), kDataEnd - kDataStart);

            R5900Context nativeCtx = inputs;
            entry.native(rdram, &nativeCtx, runtime);

            const bool regsSame = entry.returns == ReturnKind::None ||
                                  (entry.returns == ReturnKind::Gpr ? GPR_U64((&nativeCtx), 2) == GPR_U64((&originalCtx), 2)
                                                                    : std::memcmp(&nativeCtx.f[0], &originalCtx.f[0], 4u) == 0);
            const bool scratchSame = std::memcmp(rdram + kScratch, scratchAfter.get(), kScratchSize) == 0;
            const bool globalsSame = std::memcmp(rdram + kDataStart, globalsAfter.get(), kDataEnd - kDataStart) == 0;
            if (!regsSame || !scratchSame || !globalsSame)
            {
                if (++mismatches <= 4u)
                    std::fprintf(stderr, "[decomp] FUZZ MISMATCH %s iteration %d: return %s scratch %s globals %s | a0=%x a1=%x a2=%x f12=%g f13=%g | v0 %llx vs %llx\n",
                                 entry.name, i, regsSame ? "same" : "DIFF", scratchSame ? "same" : "DIFF", globalsSame ? "same" : "DIFF",
                                 getRegU32(&inputs, 4), getRegU32(&inputs, 5), getRegU32(&inputs, 6), inputs.f[12], inputs.f[13],
                                 (unsigned long long)GPR_U64((&originalCtx), 2), (unsigned long long)GPR_U64((&nativeCtx), 2));
            }
        }
        std::fprintf(stderr, "[decomp] fuzz %s: %d inputs, %llu mismatches\n", entry.name, iterations, (unsigned long long)mismatches);
        failures += mismatches;
    }
    std::memcpy(rdram, backup.get(), PS2_RAM_SIZE);
    std::fprintf(stderr, "[decomp] fuzz finished: %llu mismatches\n", (unsigned long long)failures);
}

} // namespace

void registerEntry(Entry &entry, RecompFn shadow)
{
    registry().push_back({&entry, shadow});
}

void shadowCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, Entry &entry)
{
    // An original that calls another decompiled function reaches this again. The reference must be
    // purely the recompiled code, so anything nested inside a comparison runs the original directly.
    if (g_comparing)
    {
        entry.original(rdram, ctx, runtime);
        return;
    }

    static bool fuzzed = false;
    if (!fuzzed)
    {
        fuzzed = true;
        if (const char *count = std::getenv("RC_SHADOW_FUZZ"))
        {
            g_comparing = true;
            fuzzAll(rdram, *ctx, runtime, std::atoi(count));
            g_comparing = false;
        }
    }

    const uint64_t callIndex = entry.calls.fetch_add(1u, std::memory_order_relaxed);
    if (!sampleThisCall(callIndex))
    {
        entry.native(rdram, ctx, runtime);
        return;
    }

    constexpr uint32_t kRamSize = PS2_RAM_SIZE;
    const uint32_t stackTop = getRegU32(ctx, 29) & PS2_RAM_MASK;
    const uint32_t stackLow = stackTop > kStackScratch ? stackTop - kStackScratch : 0u;

    std::memcpy(g_before.get(), rdram, kRamSize);

    // The original runs on a copy of the registers, then its writes to RAM are undone.
    R5900Context originalCtx = *ctx;
    g_comparing = true;
    entry.original(rdram, &originalCtx, runtime);
    g_comparing = false;
    std::memcpy(g_expected.get(), rdram, kRamSize);
    revertWrites(rdram, g_before.get(), 0u, kRamSize);

    // A scheduling point can cut the original short; such a run proves nothing.
    if (originalCtx.pc != getRegU32(ctx, 31))
    {
        entry.native(rdram, ctx, runtime);
        return;
    }

    const R5900Context input = *ctx;
    entry.native(rdram, ctx, runtime);
    entry.compared.fetch_add(1u, std::memory_order_relaxed);

    bool same = true;
    const auto fail = [&](const char *what, uint64_t original, uint64_t decompiled, uint32_t address)
    {
        if (same)
            entry.mismatches.fetch_add(1u, std::memory_order_relaxed);
        same = false;
        reportMismatch(entry, input, what, original, decompiled, address);
    };

    if (entry.returns == ReturnKind::Gpr && GPR_U64(ctx, 2) != GPR_U64((&originalCtx), 2))
        fail("v0", GPR_U64((&originalCtx), 2), GPR_U64(ctx, 2), 0u);
    if (entry.returns == ReturnKind::Float && std::memcmp(&ctx->f[0], &originalCtx.f[0], sizeof(float)) != 0)
    {
        uint32_t original, decompiled;
        std::memcpy(&original, &originalCtx.f[0], 4);
        std::memcpy(&decompiled, &ctx->f[0], 4);
        fail("f0", original, decompiled, 0u);
    }

    // The dead stack area below $sp is the one place the two are expected to differ.
    for (const auto &[begin, end] : {std::pair<uint32_t, uint32_t>{0u, stackLow}, {stackTop, kRamSize}})
    {
        const uint32_t difference = firstDifference(rdram, g_expected.get(), begin, end);
        if (difference != end)
            fail("ram", g_expected[difference], rdram[difference], difference);
    }
}

RecompFn decompOr(uint32_t address, RecompFn fallback)
{
    const char *enabled = std::getenv("RC_DECOMP");
    if (enabled && enabled[0] == '0')
        return fallback;
    for (const Registered &item : registry())
    {
        if (item.entry->address == address)
            return item.entry->native;
    }
    return fallback;
}

void installDecomp(PS2Runtime &runtime)
{
    g_rdram = runtime.memory().getRDRAM();
    g_runtime = &runtime;
    const char *enabled = std::getenv("RC_DECOMP");
    if (enabled && enabled[0] == '0')
    {
        std::fprintf(stderr, "[decomp] off, using %zu recompiled functions\n", registry().size());
        return;
    }
    const char *shadowSetting = std::getenv("RC_SHADOW");
    const bool shadow = shadowSetting && shadowSetting[0] == '1';
    if (shadow)
    {
        g_before.reset(new uint8_t[PS2_RAM_SIZE]);
        g_expected.reset(new uint8_t[PS2_RAM_SIZE]);
        const char *log = std::getenv("RC_SHADOW_LOG");
        std::thread(
            [path = std::string(log ? log : "decomp_shadow.txt")]
            {
                for (;;)
                {
                    std::this_thread::sleep_for(std::chrono::seconds(3));
                    writeSummary(path);
                }
            })
            .detach();
    }
    size_t installed = 0u;
    for (const Registered &item : registry())
        installed += runtime.replaceFunction(item.entry->address, shadow ? item.shadow : item.entry->native) ? 1u : 0u;
    std::fprintf(stderr, "[decomp] %zu of %zu functions installed%s\n", installed, registry().size(),
                 shadow ? ", comparing against the recompiled originals" : "");
}

} // namespace rc

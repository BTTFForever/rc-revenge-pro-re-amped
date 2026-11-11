// Installs decompiled functions into the running game and checks them against the recompiled ones.
//
//   RC_DECOMP=0         keep every recompiled function
//   RC_SHADOW=1         compare sampled calls against the recompiled originals
//   RC_SHADOW_FUZZ=n    also run each function on n random inputs, in steps between the game's own calls
//   RC_SHADOW_LOG=file  the running totals, rewritten every 3 seconds (default: decomp_shadow.txt)

#include "rc_decomp.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
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
    static const int samplesPerSecond = []
    {
        const char *rate = std::getenv("RC_SHADOW_RATE"); // Comparisons per second, across all functions.
        return rate ? std::atoi(rate) : static_cast<int>(kSamplesPerSecond);
    }();
    if (windowCount >= static_cast<decltype(windowCount)>(samplesPerSecond))
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
//
// Recompiled code stops at its next call or loop once the scheduler has an interrupt or a thread switch due, and
// nothing can service that from in here. So the test runs in steps: when an original is cut short, the step ends,
// the game carries on, and the next compared call resumes the test from the same input.
struct FuzzProgress
{
    size_t index = 0;          // Which registered function.
    int iteration = 0;
    uint64_t state = 0;        // The random generator, at the start of the current input.
    bool entryStarted = false;
    int retries = 0;           // Times the current input was cut short with a whole step to itself.
    uint64_t mismatches = 0, returnedNonZero = 0, wroteMemory = 0, cutShort = 0; // For the current function.
    uint64_t failures = 0, steps = 0;
};

// Returns true once every function has been tested.
bool fuzzStep(uint8_t *rdram, const R5900Context &live, PS2Runtime *runtime, int iterations)
{
    constexpr uint32_t kScratch = 0x01F00000u, kScratchSize = 0x8000u;
    constexpr uint32_t kDataStart = 0x001E9400u, kDataEnd = 0x004C7190u;
    static FuzzProgress progress;
    static std::unique_ptr<uint8_t[]> backup(new uint8_t[PS2_RAM_SIZE]);
    static std::unique_ptr<uint8_t[]> scratchStart(new uint8_t[kScratchSize]), globalsStart(new uint8_t[kDataEnd - kDataStart]);
    static std::unique_ptr<uint8_t[]> scratchAfter(new uint8_t[kScratchSize]), globalsAfter(new uint8_t[kDataEnd - kDataStart]);
    static const std::string only = [] // RC_SHADOW_ONLY=name,name limits the test to those functions.
    {
        const char *names = std::getenv("RC_SHADOW_ONLY");
        return names ? "," + std::string(names) + "," : std::string();
    }();
    constexpr int kWholeRamInputs = 40, kTries = 2, kLongTries = 80;

    std::memcpy(backup.get(), rdram, PS2_RAM_SIZE);
    ++progress.steps;
    bool firstOfStep = true;
    bool paused = false;
    while (!paused && progress.index < registry().size())
    {
        Entry &entry = *registry()[progress.index].entry;
        if (entry.randomiseArgs == nullptr || (!only.empty() && only.find("," + std::string(entry.name) + ",") == std::string::npos))
        {
            ++progress.index;
            continue;
        }
        if (!progress.entryStarted)
        {
            progress.entryStarted = true;
            progress.iteration = 0;
            progress.state = 0x5EEDu + entry.address;
            progress.mismatches = progress.returnedNonZero = progress.wroteMemory = progress.cutShort = 0u;
        }
        const int inputs = entry.fuzzWholeRam ? std::min(iterations, kWholeRamInputs) : iterations;
        for (; progress.iteration < inputs; ++progress.iteration, firstOfStep = false, progress.retries = 0)
        {
            uint64_t state = progress.state;
            for (uint32_t pos = 0u; pos < kScratchSize; pos += 4u)
            {
                // Mostly small values: fixed-point vectors are 16-bit, and huge ones only overflow.
                const uint32_t word = (nextFuzz(state) & 1u) != 0u ? nextFuzz(state) : (nextFuzz(state) & 0x0FFF0FFFu);
                std::memcpy(rdram + kScratch + pos, &word, 4u);
            }
            // A function registered with its own setup builds valid data in the scratch area and the globals here.
            R5900Context inputs = live;
            entry.randomiseArgs(&inputs, state, kScratch);
            std::memcpy(scratchStart.get(), rdram + kScratch, kScratchSize);
            std::memcpy(globalsStart.get(), rdram + kDataStart, kDataEnd - kDataStart);
            if (entry.fuzzWholeRam)
                std::memcpy(g_before.get(), rdram, PS2_RAM_SIZE);

            R5900Context originalCtx = inputs;
            entry.original(rdram, &originalCtx, runtime);
            if (entry.fuzzWholeRam)
            {
                std::memcpy(g_expected.get(), rdram, PS2_RAM_SIZE);
                revertWrites(rdram, g_before.get(), 0u, PS2_RAM_SIZE);
            }
            if (originalCtx.pc != getRegU32(&inputs, 31))
            {
                // Cut short by a scheduling point. Try the input again in the next step, unless it has had several
                // whole steps to itself: then it is something the function cannot finish, and it is counted and skipped.
                // A function that runs for longer than the usual gap between interrupts needs many tries.
                std::memcpy(rdram + kScratch, scratchStart.get(), kScratchSize);
                std::memcpy(rdram + kDataStart, globalsStart.get(), kDataEnd - kDataStart);
                paused = true;
                if (firstOfStep && ++progress.retries >= (entry.fuzzWholeRam ? kLongTries : kTries))
                {
                    ++progress.cutShort;
                    progress.state = state;
                    progress.retries = 0;
                    ++progress.iteration;
                }
                break;
            }
            progress.state = state;
            const uint8_t *after = entry.fuzzWholeRam ? g_expected.get() : rdram;
            std::memcpy(scratchAfter.get(), after + kScratch, kScratchSize);
            std::memcpy(globalsAfter.get(), after + kDataStart, kDataEnd - kDataStart);
            progress.returnedNonZero += entry.returns == ReturnKind::Gpr && GPR_U64((&originalCtx), 2) != 0u ? 1u : 0u;
            progress.wroteMemory += std::memcmp(scratchAfter.get(), scratchStart.get(), kScratchSize) != 0 ||
                                            std::memcmp(globalsAfter.get(), globalsStart.get(), kDataEnd - kDataStart) != 0
                                        ? 1u
                                        : 0u;
            std::memcpy(rdram + kScratch, scratchStart.get(), kScratchSize);
            std::memcpy(rdram + kDataStart, globalsStart.get(), kDataEnd - kDataStart);

            R5900Context nativeCtx = inputs;
            entry.native(rdram, &nativeCtx, runtime);

            const bool regsSame = entry.returns == ReturnKind::None ||
                                  (entry.returns == ReturnKind::Gpr ? GPR_U64((&nativeCtx), 2) == GPR_U64((&originalCtx), 2)
                                                                    : std::memcmp(&nativeCtx.f[0], &originalCtx.f[0], 4u) == 0);
            const bool scratchSame = std::memcmp(rdram + kScratch, scratchAfter.get(), kScratchSize) == 0;
            const bool globalsSame = std::memcmp(rdram + kDataStart, globalsAfter.get(), kDataEnd - kDataStart) == 0;
            bool ramSame = true;
            if (entry.fuzzWholeRam)
            {
                // As for the game's own calls: everything but the dead stack below $sp.
                const uint32_t stackTop = getRegU32(&inputs, 29) & PS2_RAM_MASK;
                const uint32_t stackLow = stackTop > kStackScratch ? stackTop - kStackScratch : 0u;
                for (const auto &[begin, end] : {std::pair<uint32_t, uint32_t>{0u, stackLow}, {stackTop, static_cast<uint32_t>(PS2_RAM_SIZE)}})
                {
                    const uint32_t difference = firstDifference(rdram, g_expected.get(), begin, end);
                    if (difference != end && ramSame && progress.mismatches < 4u)
                        std::fprintf(stderr, "[decomp] FUZZ DETAIL %s: ram 0x%x original=%02x decompiled=%02x\n", entry.name, difference,
                                     g_expected[difference], rdram[difference]);
                    ramSame = ramSame && difference == end;
                }
                revertWrites(rdram, backup.get(), 0u, PS2_RAM_SIZE); // The heap as it was, for the next input.
            }
            if (!regsSame || !scratchSame || !globalsSame || !ramSame)
            {
                if (progress.mismatches < 4u && !scratchSame)
                {
                    const uint32_t at = firstDifference(rdram + kScratch, scratchAfter.get(), 0u, kScratchSize) & ~3u;
                    uint32_t native, original;
                    std::memcpy(&native, rdram + kScratch + at, 4u);
                    std::memcpy(&original, scratchAfter.get() + at, 4u);
                    std::fprintf(stderr, "[decomp] FUZZ DETAIL %s: scratch+0x%x original=%08x decompiled=%08x\n", entry.name, at, original, native);
                }
                if (progress.mismatches < 4u && !globalsSame)
                {
                    const uint32_t at = firstDifference(rdram + kDataStart, globalsAfter.get(), 0u, kDataEnd - kDataStart) & ~3u;
                    uint32_t native, original;
                    std::memcpy(&native, rdram + kDataStart + at, 4u);
                    std::memcpy(&original, globalsAfter.get() + at, 4u);
                    std::fprintf(stderr, "[decomp] FUZZ DETAIL %s: global 0x%x original=%08x decompiled=%08x\n", entry.name, kDataStart + at, original, native);
                }
                if (++progress.mismatches <= 4u)
                    std::fprintf(stderr, "[decomp] FUZZ MISMATCH %s iteration %d: return %s scratch %s globals %s | a0=%x a1=%x a2=%x f12=%g f13=%g | v0 %llx vs %llx\n",
                                 entry.name, progress.iteration, regsSame ? "same" : "DIFF", scratchSame ? "same" : "DIFF", globalsSame ? "same" : "DIFF",
                                 getRegU32(&inputs, 4), getRegU32(&inputs, 5), getRegU32(&inputs, 6), inputs.f[12], inputs.f[13],
                                 (unsigned long long)GPR_U64((&originalCtx), 2), (unsigned long long)GPR_U64((&nativeCtx), 2));
            }
        }
        if (progress.iteration >= inputs)
        {
            // The counts say how much of the function the inputs reached.
            std::fprintf(stderr, "[decomp] fuzz %s: %d inputs, %llu mismatches (%llu returned non-zero, %llu wrote memory, %llu cut short)\n",
                         entry.name, inputs, (unsigned long long)progress.mismatches, (unsigned long long)progress.returnedNonZero,
                         (unsigned long long)progress.wroteMemory, (unsigned long long)progress.cutShort);
            progress.failures += progress.mismatches;
            progress.entryStarted = false;
            ++progress.index;
        }
    }
    revertWrites(rdram, backup.get(), 0u, PS2_RAM_SIZE);
    if (paused)
        return false;
    std::fprintf(stderr, "[decomp] fuzz finished: %llu mismatches, in %llu steps\n", (unsigned long long)progress.failures,
                 (unsigned long long)progress.steps);
    return true;
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

    // The random test waits for the game's heap, which some of the functions allocate from.
    static bool fuzzed = std::getenv("RC_SHADOW_FUZZ") == nullptr;
    if (!fuzzed && G(memVarG).pHeader)
    {
        g_comparing = true;
        fuzzed = fuzzStep(rdram, *ctx, runtime, std::atoi(std::getenv("RC_SHADOW_FUZZ")));
        g_comparing = false;
    }

    const uint64_t callIndex = entry.calls.fetch_add(1u, std::memory_order_relaxed);
    // RC_SHADOW_ALWAYS=name,name: compare every call of these, to reach rare paths. Slow.
    static const std::string always = []
    {
        const char *names = std::getenv("RC_SHADOW_ALWAYS");
        return "," + std::string(names ? names : "") + ",";
    }();
    const bool compareAlways = always.find("," + std::string(entry.name) + ",") != std::string::npos;
    if (!compareAlways && !sampleThisCall(callIndex))
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

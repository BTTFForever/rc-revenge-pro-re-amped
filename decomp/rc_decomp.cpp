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

uint32_t g_nativeExtraCycles = []
{
    const char *value = std::getenv("RC_DECOMP_CHARGE");
    return value ? static_cast<uint32_t>(std::atoi(value)) : 0u;
}();


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
// Set while a compared native function runs. Native code that calls guest code (guest_call) can reach other
// decompiled functions; comparing those there would reuse the RAM snapshots the outer comparison still needs.
bool g_inComparedNative = false;

//
// The random test runs natives on a copy of the registers, but guest_call works on the scheduler's running context,
// which is the game's own: its registers are put back afterwards, or the game would carry on with the test's.
void runComparedNative(Entry &entry, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    R5900Context &running = runtime->eeScheduler().runningContext();
    const bool copy = ctx != &running;
    const R5900Context saved = copy ? running : R5900Context{};
    // Interrupts wait, as for the original, so none runs inside the native's guest calls.
    runtime->eeScheduler().holdCheckpoints(true);
    g_inComparedNative = true;
    // RC_SHADOW_SELFTEST=1 runs the original in place of the native: any difference left is noise from outside the
    // function (another thread writing guest RAM between the two runs).
    static const bool selfTest = std::getenv("RC_SHADOW_SELFTEST") != nullptr;
    if (selfTest)
    {
        const uint32_t returnAddress = getRegU32(ctx, 31);
        entry.original(rdram, ctx, runtime);
        for (int resumes = 0; ctx->pc != returnAddress && resumes < 1000000; ++resumes)
            runtime->lookupFunction(ctx->pc)(rdram, ctx, runtime);
    }
    else
        entry.native(rdram, ctx, runtime);
    g_inComparedNative = false;
    runtime->eeScheduler().holdCheckpoints(false);
    if (copy)
        running = saved;
}

// Runs a recompiled original to its end. Interrupts wait until it returns: a run cut short by one proves nothing,
// and long functions (deep searches, big clears) would otherwise rarely finish between two interrupts.
//
// A recompiled function can also return before the guest function has: a call to itself becomes a jump inside the
// same host function, so the inner return leaves the host function with pc at the outer call's return address, and
// the game's dispatcher re-enters it there. This keeps dispatching the same way until pc reaches the caller.
void runOriginal(Entry &entry, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    EeScheduler &scheduler = runtime->eeScheduler();
    const uint32_t returnAddress = getRegU32(ctx, 31);
    scheduler.holdCheckpoints(true);
    entry.original(rdram, ctx, runtime);
    for (int resumes = 0; ctx->pc != returnAddress && resumes < 1000000; ++resumes)
        runtime->lookupFunction(ctx->pc)(rdram, ctx, runtime);
    scheduler.holdCheckpoints(false);
}
std::unique_ptr<uint8_t[]> g_before;
std::unique_ptr<uint8_t[]> g_expected;

bool sampleThisCall(uint64_t callIndex)
{
    // RC_SHADOW_FIRST=<n> changes how many of each function's first calls are always compared (0 for play testing).
    static const uint64_t first = []
    {
        const char *value = std::getenv("RC_SHADOW_FIRST");
        return value ? static_cast<uint64_t>(std::atoll(value)) : kAlwaysCompare;
    }();
    if (callIndex < first)
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

// The 16 KB scratchpad (0x70000000) is not part of main RAM, and the game keeps live data there (the effects' sprite
// record and quad, visFxVarG.sprite and pGT4). A comparison must give the native run the scratchpad the original
// started with and compare what both leave there, like RAM.
struct Scratchpad
{
    uint8_t before[PS2_SCRATCHPAD_SIZE];
    uint8_t expected[PS2_SCRATCHPAD_SIZE];

    void save() { std::memcpy(before, ps2GetScratchpadHostPtr(), PS2_SCRATCHPAD_SIZE); }
    // After the original's run: keeps its result and puts the starting contents back.
    void keepAndRevert()
    {
        std::memcpy(expected, ps2GetScratchpadHostPtr(), PS2_SCRATCHPAD_SIZE);
        std::memcpy(ps2GetScratchpadHostPtr(), before, PS2_SCRATCHPAD_SIZE);
    }
    // After the native run: the first differing offset, or PS2_SCRATCHPAD_SIZE.
    // Not compared: the w lanes of the effects' quad (pGT4 at 0x70000030: four vertices of 16 bytes). The sprite
    // drawing adds a stack word nobody wrote to them, which is often a register a recompiled function saved in an
    // earlier call; native functions save none, so the lanes cannot match across calls. Nothing reads them: RdrPoly
    // and RdrTri send x, y, z and a constant 1.
    uint32_t firstDifference() const
    {
        const uint8_t *now = ps2GetScratchpadHostPtr();
        for (uint32_t i = 0; i < PS2_SCRATCHPAD_SIZE; ++i)
        {
            const bool quadW = i >= 0x30u && i < 0x70u && (i & 0xcu) == 0xcu;
            if (now[i] != expected[i] && !quadW)
                return i;
        }
        return PS2_SCRATCHPAD_SIZE;
    }
};
Scratchpad g_pad; // Comparisons do not nest.

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

} // namespace

// One random input of the random test: builds it (the function's setup, or random arguments), runs the original and
// the decompiled function from identical state and compares the result, the scratch area, the game's globals, the
// scratchpad and, for a function that uses the heap, all of RAM. Shared by the live test (fuzzStep) and the offline
// oracle (decomp/oracle). ramBackup is RAM as the step began; a whole-RAM input puts it back afterwards. Detail lines go
// to stderr while mismatchesSoFar < 4. On a cut-short original the scratch area and globals are put back and nothing
// is compared.
FuzzCaseResult fuzzCase(Entry &entry, uint8_t *rdram, const R5900Context &live, PS2Runtime *runtime, uint64_t &state,
                        const uint8_t *ramBackup, int iteration, uint64_t mismatchesSoFar)
{
    constexpr uint32_t kScratch = kFuzzScratch, kScratchSize = kFuzzScratchSize;
    constexpr uint32_t kDataStart = kFuzzDataStart, kDataEnd = kFuzzDataEnd;
    static std::unique_ptr<uint8_t[]> scratchStart(new uint8_t[kScratchSize]), globalsStart(new uint8_t[kDataEnd - kDataStart]);
    static std::unique_ptr<uint8_t[]> scratchAfter(new uint8_t[kScratchSize]), globalsAfter(new uint8_t[kDataEnd - kDataStart]);
    static std::unique_ptr<uint8_t[]> stackStart(new uint8_t[kStackScratch]);
    const bool wasComparing = g_comparing;
    g_comparing = true; // Calls nested in either run reach the recompiled originals, as in shadowCall.
    FuzzCaseResult result;

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

    // The dead stack below $sp as the original finds it: some functions read stack words they never wrote, and the
    // native run must find the same ones, not what the original's run left there.
    const uint32_t stackBase = (getRegU32(&inputs, 29) & PS2_RAM_MASK) - kStackScratch;
    const bool stackInRam = (getRegU32(&inputs, 29) & PS2_RAM_MASK) >= kStackScratch;
    if (stackInRam)
        std::memcpy(stackStart.get(), rdram + stackBase, kStackScratch);
    g_pad.save();
    R5900Context originalCtx = inputs;
    runOriginal(entry, rdram, &originalCtx, runtime);
    g_pad.keepAndRevert();
    if (entry.fuzzWholeRam)
    {
        std::memcpy(g_expected.get(), rdram, PS2_RAM_SIZE);
        revertWrites(rdram, g_before.get(), 0u, PS2_RAM_SIZE);
    }
    if (originalCtx.pc != getRegU32(&inputs, 31))
    {
        // Cut short by a scheduling point: the caller decides whether to try the input again.
        std::memcpy(rdram + kScratch, scratchStart.get(), kScratchSize);
        std::memcpy(rdram + kDataStart, globalsStart.get(), kDataEnd - kDataStart);
        result.cutShort = true;
        g_comparing = wasComparing;
        return result;
    }
    const uint8_t *after = entry.fuzzWholeRam ? g_expected.get() : rdram;
    std::memcpy(scratchAfter.get(), after + kScratch, kScratchSize);
    // The globals (3 MB) by 4 KB blocks, since a function changes few of them: globalsAfter holds the original's result
    // in the blocks it changed, and the start elsewhere once completeAfter() has run.
    constexpr uint32_t kDataSize = kDataEnd - kDataStart, kBlock = 4096u, kBlocks = (kDataSize + kBlock - 1u) / kBlock;
    static bool changed[kBlocks];
    bool globalsWritten = false;
    for (uint32_t block = 0; block < kBlocks; ++block)
    {
        const uint32_t offset = block * kBlock, size = std::min(kBlock, kDataSize - offset);
        changed[block] = std::memcmp(after + kDataStart + offset, globalsStart.get() + offset, size) != 0;
        if (changed[block])
        {
            std::memcpy(globalsAfter.get() + offset, after + kDataStart + offset, size);
            std::memcpy(rdram + kDataStart + offset, globalsStart.get() + offset, size);
            globalsWritten = true;
        }
    }
    const auto completeAfter = [&]
    {
        for (uint32_t block = 0; block < kBlocks; ++block)
            if (!changed[block])
            {
                const uint32_t offset = block * kBlock;
                std::memcpy(globalsAfter.get() + offset, globalsStart.get() + offset, std::min(kBlock, kDataSize - offset));
            }
    };
    result.returnedNonZero = entry.returns == ReturnKind::Gpr && GPR_U64((&originalCtx), 2) != 0u;
    result.wroteMemory = std::memcmp(scratchAfter.get(), scratchStart.get(), kScratchSize) != 0 || globalsWritten;
    std::memcpy(rdram + kScratch, scratchStart.get(), kScratchSize);

    if (stackInRam)
        std::memcpy(rdram + stackBase, stackStart.get(), kStackScratch);
    R5900Context nativeCtx = inputs;
    runComparedNative(entry, rdram, &nativeCtx, runtime);

    const bool regsSame = entry.returns == ReturnKind::None ||
                          (entry.returns == ReturnKind::Gpr ? GPR_U64((&nativeCtx), 2) == GPR_U64((&originalCtx), 2)
                                                            : std::memcmp(&nativeCtx.f[0], &originalCtx.f[0], 4u) == 0);
    const bool scratchSame = std::memcmp(rdram + kScratch, scratchAfter.get(), kScratchSize) == 0;
    bool globalsSame = true;
    for (uint32_t block = 0; block < kBlocks && globalsSame; ++block)
    {
        const uint32_t offset = block * kBlock;
        const uint8_t *expected = (changed[block] ? globalsAfter.get() : globalsStart.get()) + offset;
        globalsSame = std::memcmp(rdram + kDataStart + offset, expected, std::min(kBlock, kDataSize - offset)) == 0;
    }
    if (!globalsSame)
        completeAfter(); // for the reports below
    const uint32_t padDifference = g_pad.firstDifference();
    const bool padSame = padDifference == PS2_SCRATCHPAD_SIZE;
    if (!padSame && mismatchesSoFar < 4u)
        std::fprintf(stderr, "[decomp] FUZZ DETAIL %s: scratchpad+0x%x original=%02x decompiled=%02x\n", entry.name, padDifference,
                     g_pad.expected[padDifference], ps2GetScratchpadHostPtr()[padDifference]);
    if (!scratchSame && std::getenv("RC_SHADOW_DIFFS") != nullptr)
    {
        int shown = 0;
        for (uint32_t i = 0; i < kScratchSize && shown < 8; ++i)
            if (rdram[kScratch + i] != scratchAfter[i])
            {
                std::fprintf(stderr, "[decomp] FUZZ SCRATCH %s +0x%x original=%02x decompiled=%02x\n", entry.name, i, scratchAfter[i],
                             rdram[kScratch + i]);
                ++shown;
            }
    }
    if (!globalsSame && std::getenv("RC_SHADOW_DIFFS") != nullptr)
    {
        // RC_SHADOW_DIFFS=1: the first differing globals of a random input.
        int shown = 0;
        for (uint32_t i = 0; i < kDataEnd - kDataStart && shown < 8; ++i)
            if (rdram[kDataStart + i] != globalsAfter[i])
            {
                std::fprintf(stderr, "[decomp] FUZZ GLOBAL %s 0x%x original=%02x decompiled=%02x\n", entry.name, kDataStart + i,
                             globalsAfter[i], rdram[kDataStart + i]);
                ++shown;
            }
    }
    bool ramSame = true;
    if (entry.fuzzWholeRam)
    {
        // As for the game's own calls: everything but the dead stack below $sp.
        const uint32_t stackTop = getRegU32(&inputs, 29) & PS2_RAM_MASK;
        const uint32_t stackLow = stackTop > kStackScratch ? stackTop - kStackScratch : 0u;
        for (const auto &[begin, end] : {std::pair<uint32_t, uint32_t>{0u, stackLow}, {stackTop, static_cast<uint32_t>(PS2_RAM_SIZE)}})
        {
            const uint32_t difference = firstDifference(rdram, g_expected.get(), begin, end);
            if (difference != end && ramSame && mismatchesSoFar < 4u)
                std::fprintf(stderr, "[decomp] FUZZ DETAIL %s: ram 0x%x original=%02x decompiled=%02x\n", entry.name, difference,
                             g_expected[difference], rdram[difference]);
            ramSame = ramSame && difference == end;
        }
        revertWrites(rdram, ramBackup, 0u, PS2_RAM_SIZE); // The heap as it was, for the next input.
    }
    result.same = regsSame && scratchSame && globalsSame && ramSame && padSame;
    if (!result.same)
    {
        if (mismatchesSoFar < 4u && !scratchSame)
        {
            const uint32_t at = firstDifference(rdram + kScratch, scratchAfter.get(), 0u, kScratchSize) & ~3u;
            uint32_t native, original;
            std::memcpy(&native, rdram + kScratch + at, 4u);
            std::memcpy(&original, scratchAfter.get() + at, 4u);
            std::fprintf(stderr, "[decomp] FUZZ DETAIL %s: scratch+0x%x original=%08x decompiled=%08x\n", entry.name, at, original, native);
        }
        if (mismatchesSoFar < 4u && !globalsSame)
        {
            const uint32_t at = firstDifference(rdram + kDataStart, globalsAfter.get(), 0u, kDataEnd - kDataStart) & ~3u;
            uint32_t native, original;
            std::memcpy(&native, rdram + kDataStart + at, 4u);
            std::memcpy(&original, globalsAfter.get() + at, 4u);
            std::fprintf(stderr, "[decomp] FUZZ DETAIL %s: global 0x%x original=%08x decompiled=%08x\n", entry.name, kDataStart + at, original, native);
        }
        if (mismatchesSoFar < 4u)
            std::fprintf(stderr, "[decomp] FUZZ MISMATCH %s iteration %d: return %s scratch %s globals %s | a0=%x a1=%x a2=%x f12=%g f13=%g sp=%x | v0 %llx vs %llx\n",
                         entry.name, iteration, regsSame ? "same" : "DIFF", scratchSame ? "same" : "DIFF", globalsSame ? "same" : "DIFF",
                         getRegU32(&inputs, 4), getRegU32(&inputs, 5), getRegU32(&inputs, 6), inputs.f[12], inputs.f[13], getRegU32(&inputs, 29),
                         (unsigned long long)GPR_U64((&originalCtx), 2), (unsigned long long)GPR_U64((&nativeCtx), 2));
    }
    g_comparing = wasComparing;
    return result;
}

namespace {

// Random inputs reach the code that a demo race does not. Each input runs through the original and the
// decompiled function from identical state (fuzzCase).
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
    static FuzzProgress progress;
    static std::unique_ptr<uint8_t[]> backup(new uint8_t[PS2_RAM_SIZE]);
    static const std::string only = [] // RC_SHADOW_ONLY=name,name limits the test to those functions.
    {
        const char *names = std::getenv("RC_SHADOW_ONLY");
        return names ? "," + std::string(names) + "," : std::string();
    }();
    constexpr int kTries = 2, kLongTries = 80;

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
        const int inputs = entry.fuzzWholeRam ? std::min(iterations, kFuzzWholeRamInputs) : iterations;
        for (; progress.iteration < inputs; ++progress.iteration, firstOfStep = false, progress.retries = 0)
        {
            uint64_t state = progress.state;
            const FuzzCaseResult result = fuzzCase(entry, rdram, live, runtime, state, backup.get(), progress.iteration, progress.mismatches);
            if (result.cutShort)
            {
                // Try the input again in the next step, unless it has had several whole steps to itself: then it is
                // something the function cannot finish, and it is counted and skipped. A function that runs for longer
                // than the usual gap between interrupts needs many tries.
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
            progress.returnedNonZero += result.returnedNonZero ? 1u : 0u;
            progress.wroteMemory += result.wroteMemory ? 1u : 0u;
            progress.mismatches += result.same ? 0u : 1u;
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

// Functions whose writes are confined to known memory can be compared cheaply: only those ranges are snapshot,
// restored and compared, instead of all of RAM. Each provider returns the ranges at the time of the call.
std::vector<std::pair<std::string, RegionProvider>> &regionProviders()
{
    static std::vector<std::pair<std::string, RegionProvider>> providers;
    return providers;
}

void registerRegions(const char *name, RegionProvider provider)
{
    regionProviders().emplace_back(name, provider);
}

namespace {

RegionProvider regionsFor(const Entry &entry)
{
    for (const auto &[name, provider] : regionProviders())
        if (name == entry.name)
            return provider;
    return nullptr;
}

// The light comparison: as shadowCall below, over the provider's ranges only.
void shadowCallRegions(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, Entry &entry, RegionProvider provider)
{
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    provider(ranges);
    std::vector<std::vector<uint8_t>> before, expected;
    for (const auto &[begin, end] : ranges)
        before.emplace_back(rdram + begin, rdram + end);
    g_pad.save();
    R5900Context originalCtx = *ctx;
    g_comparing = true;
    runOriginal(entry, rdram, &originalCtx, runtime);
    g_comparing = false;
    g_pad.keepAndRevert();
    for (size_t i = 0; i < ranges.size(); ++i)
    {
        expected.emplace_back(rdram + ranges[i].first, rdram + ranges[i].second);
        std::memcpy(rdram + ranges[i].first, before[i].data(), before[i].size());
    }
    if (originalCtx.pc != getRegU32(ctx, 31))
    {
        entry.native(rdram, ctx, runtime);
        return;
    }
    const R5900Context input = *ctx;
    runComparedNative(entry, rdram, ctx, runtime);
    entry.compared.fetch_add(1u, std::memory_order_relaxed);
    for (size_t i = 0; i < ranges.size(); ++i)
    {
        const uint8_t *now = rdram + ranges[i].first;
        for (uint32_t k = 0; k < expected[i].size(); ++k)
        {
            if (now[k] != expected[i][k])
            {
                entry.mismatches.fetch_add(1u, std::memory_order_relaxed);
                reportMismatch(entry, input, "ram", expected[i][k], now[k], ranges[i].first + k);
                // Show the whole differing stretch, up to 64 bytes.
                std::fprintf(stderr, "[decomp]   region 0x%x+0x%x:", ranges[i].first, k);
                for (uint32_t j = k; j < expected[i].size() && j < k + 64u; ++j)
                    std::fprintf(stderr, " %02x/%02x", expected[i][j], now[j]);
                std::fprintf(stderr, "\n");
                return;
            }
        }
    }
}

} // namespace

void shadowCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, Entry &entry)
{
    // An original that calls another decompiled function reaches this again. The reference must be
    // purely the recompiled code, so anything nested inside a comparison runs the original directly.
    if (g_comparing)
    {
        entry.original(rdram, ctx, runtime);
        return;
    }
    if (g_inComparedNative)
    {
        entry.native(rdram, ctx, runtime);
        return;
    }

    // The random test waits for the game's heap, which some of the functions allocate from.
    // RC_SHADOW_FUZZ_DELAY=<seconds> waits longer, for setups that need a level loaded.
    static bool fuzzed = std::getenv("RC_SHADOW_FUZZ") == nullptr;
    static const auto fuzzFrom = std::chrono::steady_clock::now() +
                                 std::chrono::seconds(std::atoi(std::getenv("RC_SHADOW_FUZZ_DELAY") ? std::getenv("RC_SHADOW_FUZZ_DELAY") : "0"));
    if (!fuzzed && G(memVarG).pHeader && std::chrono::steady_clock::now() >= fuzzFrom)
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

    if (const RegionProvider provider = regionsFor(entry))
    {
        shadowCallRegions(rdram, ctx, runtime, entry, provider);
        return;
    }

    constexpr uint32_t kRamSize = PS2_RAM_SIZE;
    const uint32_t stackTop = getRegU32(ctx, 29) & PS2_RAM_MASK;
    const uint32_t stackLow = stackTop > kStackScratch ? stackTop - kStackScratch : 0u;

    std::memcpy(g_before.get(), rdram, kRamSize);

    // The original runs on a copy of the registers, then its writes to RAM are undone.
    g_pad.save();
    R5900Context originalCtx = *ctx;
    g_comparing = true;
    runOriginal(entry, rdram, &originalCtx, runtime);
    g_comparing = false;
    g_pad.keepAndRevert();
    std::memcpy(g_expected.get(), rdram, kRamSize);
    revertWrites(rdram, g_before.get(), 0u, kRamSize);

    // A scheduling point can cut the original short; such a run proves nothing.
    if (originalCtx.pc != getRegU32(ctx, 31))
    {
        entry.native(rdram, ctx, runtime);
        return;
    }

    const R5900Context input = *ctx;
    runComparedNative(entry, rdram, ctx, runtime);
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

    if (const uint32_t at = g_pad.firstDifference(); at != PS2_SCRATCHPAD_SIZE)
        fail("scratchpad", g_pad.expected[at], ps2GetScratchpadHostPtr()[at], PS2_SCRATCHPAD_BASE + at);

    // The dead stack area below $sp is the one place the two are expected to differ.
    for (const auto &[begin, end] : {std::pair<uint32_t, uint32_t>{0u, stackLow}, {stackTop, kRamSize}})
    {
        const uint32_t difference = firstDifference(rdram, g_expected.get(), begin, end);
        if (difference != end)
        {
            fail("ram", g_expected[difference], rdram[difference], difference);
            // RC_SHADOW_DIFFS=1: list every differing range, to see what kind of data it is.
            static const bool listAll = std::getenv("RC_SHADOW_DIFFS") != nullptr;
            for (uint32_t at = difference, shown = 0; listAll && at < end && shown < 24; ++shown)
            {
                uint32_t stop = at;
                while (stop < end && rdram[stop] != g_expected[stop])
                    ++stop;
                std::fprintf(stderr, "[decomp]   differs 0x%x..0x%x:", at, stop);
                for (uint32_t i = at; i < stop && i < at + 16u; ++i)
                    std::fprintf(stderr, " %02x/%02x", g_expected[i], rdram[i]);
                std::fprintf(stderr, "\n");
                at = firstDifference(rdram, g_expected.get(), stop, end);
            }
        }
    }
}

// Runs a function's recompiled original from native code on a copy of the running context, with interrupts held,
// to its real return; for rare paths not ported yet (rc_decomp.h).
ee_float callOriginalFloat(uint32_t address, ee_float argument)
{
    for (const Registered &item : registry())
    {
        if (item.entry->address != address)
            continue;
        PS2Runtime *runtime = g_runtime;
        R5900Context ctx = runtime->eeScheduler().runningContext();
        ctx.f[12] = argument.v;
        constexpr uint32_t kReturn = 0xFFFFFFE0u; // No guest code has this address.
        SET_GPR_U32((&ctx), 31, kReturn);
        const bool comparing = g_comparing;
        g_comparing = true; // Nested decompiled functions run their originals too.
        runtime->eeScheduler().holdCheckpoints(true);
        item.entry->original(g_rdram, &ctx, runtime);
        for (int resumes = 0; ctx.pc != kReturn && resumes < 1000000; ++resumes)
            runtime->lookupFunction(ctx.pc)(g_rdram, &ctx, runtime);
        runtime->eeScheduler().holdCheckpoints(false);
        g_comparing = comparing;
        return ee_float(ctx.f[0]);
    }
    std::fprintf(stderr, "[decomp] callOriginalFloat: nothing registered at 0x%x\n", address);
    std::abort();
}

// RC_DECOMP_SKIP=name,name leaves those functions recompiled. A native caller still calls them natively, so this is
// for exposing a native function's callees to the comparison: with the caller recompiled, its calls go through the
// table and are compared.
bool skipped(const char *name)
{
    static const std::string skip = []
    {
        const char *names = std::getenv("RC_DECOMP_SKIP");
        return "," + std::string(names ? names : "") + ",";
    }();
    return skip.find("," + std::string(name) + ",") != std::string::npos;
}

RecompFn decompOr(uint32_t address, RecompFn fallback)
{
    const char *enabled = std::getenv("RC_DECOMP");
    if (enabled && enabled[0] == '0')
        return fallback;
    for (const Registered &item : registry())
    {
        if (item.entry->address == address)
            return skipped(item.entry->name) ? fallback : item.entry->native;
    }
    return fallback;
}

std::vector<Entry *> registeredEntries()
{
    std::vector<Entry *> entries;
    for (const Registered &item : registry())
        entries.push_back(item.entry);
    return entries;
}

void installForOracle(PS2Runtime &runtime)
{
    g_rdram = runtime.memory().getRDRAM();
    g_runtime = &runtime;
    g_before.reset(new uint8_t[PS2_RAM_SIZE]);
    g_expected.reset(new uint8_t[PS2_RAM_SIZE]);
    for (const Registered &item : registry())
        runtime.replaceFunction(item.entry->address, item.shadow);
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
        if (!skipped(item.entry->name))
            installed += runtime.replaceFunction(item.entry->address, shadow ? item.shadow : item.entry->native) ? 1u : 0u;
    std::fprintf(stderr, "[decomp] %zu of %zu functions installed%s\n", installed, registry().size(),
                 shadow ? ", comparing against the recompiled originals" : "");
}

} // namespace rc

// Runs a C++ VU1 program beside the original and compares the GS packets and final VU memory.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "ps2_vu1_impl.inl"
#include "vu1_ref.h"

#define RC_VU1_IMAGES(X) \
    X(0x6EC8B9FF8FA44221ull, "Rdr_VUShapeProg") \
    X(0xBAC581270F262987ull, "TERdr_VUShapeProg") \
    X(0xAB0E7743A9BA7393ull, "EnvRdr_VUShapeProg") \
    X(0xEC08B22243D4DC8Cull, "Rdr_VUSkinProg") \
    X(0xE96770E4143D2DA7ull, "Rdr_VUWaterProg") \
    X(0xC3952C0FAEB52EADull, "TRdr_VUShapeProg")
#define RC_DECLARE_IMAGE(hash, name) \
    template <> VU1InterpreterAccess::StepResult VU1InterpreterAccess::runImage<hash>(VU1Interpreter &, const Vu1RunArgs &);
RC_VU1_IMAGES(RC_DECLARE_IMAGE)

namespace rc::vu1 {

namespace {

std::vector<std::vector<uint8_t>> g_expected;
size_t g_next = 0u;
bool g_active = false;
std::unique_ptr<uint8_t[]> g_memory;
::FILE *g_capture = nullptr; // RC_VU1_CAPTURE=<file>: real runs for tools/shader_check.cpp.
int g_captureLeft = 0;

std::atomic<uint64_t> g_runs{0}, g_packets{0}, g_packetMismatches{0}, g_memoryMismatches{0}, g_countMismatches{0};

void report(const char *what, uint64_t value, uint32_t where, uint32_t a, uint32_t b)
{
    static std::atomic<int> printed{0};
    if (printed.fetch_add(1) < 24)
        std::fprintf(stderr, "[vu1 ref] MISMATCH %s #%llu at 0x%x: original 0x%08x, C++ 0x%08x\n", what,
                     (unsigned long long)value, where, a, b);
}

void observeKick(const uint8_t *data, size_t bytes)
{
    if (!g_active)
        return;
    if (g_next >= g_expected.size())
        return; // A packet from another program, once this one has finished.
    const std::vector<uint8_t> &expected = g_expected[g_next++];
    g_packets.fetch_add(1u);
    if (expected.size() != bytes)
    {
        g_packetMismatches.fetch_add(1u);
        report("packet size", g_next, 0u, static_cast<uint32_t>(bytes), static_cast<uint32_t>(expected.size()));
        return;
    }
    for (size_t i = 0u; i + 4u <= bytes; i += 4u)
    {
        uint32_t a, b;
        std::memcpy(&a, data + i, 4u);
        std::memcpy(&b, expected.data() + i, 4u);
        if (a != b)
        {
            g_packetMismatches.fetch_add(1u);
            report("packet", g_next, static_cast<uint32_t>(i), a, b);
            return;
        }
    }
}

// RC_VU1_STATS=1: how many runs of each program, and how much thread time they take.
template <uint64_t Hash>
struct ImageStats
{
    static inline std::atomic<uint64_t> runs{0}, nanoseconds{0};
    static VU1InterpreterAccess::StepResult run(VU1Interpreter &vu, const Vu1RunArgs &args)
    {
        const auto start = std::chrono::steady_clock::now();
        const auto result = VU1InterpreterAccess::runImage<Hash>(vu, args);
        nanoseconds.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
        runs.fetch_add(1u);
        return result;
    }
};

void checkedWater(uint8_t *memory, const float registers[32][4], uint32_t pc, uint32_t top, std::vector<std::vector<uint8_t>> &kicks)
{
    runWaterShape(memory, registers, pc, top, kicks);
}

using RunFn = void (*)(uint8_t *, const float[32][4], uint32_t, uint32_t, std::vector<std::vector<uint8_t>> &);

uint64_t g_captureHash = 0xC3952C0FAEB52EADull;
std::atomic<uint64_t> g_cutShort{0}; // Runs the runtime stopped at its cycle limit and never resumed.
std::atomic<uint64_t> g_trackRuns{0}, g_envRuns{0}, g_envMappedRuns{0}, g_waterRuns{0}; // What the comparison has actually covered.

// `Entries` are the program counters at which the C++ version can start. Anything else continues a run already begun.
template <uint64_t Hash, RunFn Run, uint32_t SecondEntry, uint32_t DirectEntry>
VU1InterpreterAccess::StepResult checked(VU1Interpreter &vu, const Vu1RunArgs &args)
{
    const VU1State state = VU1InterpreterAccess::effectiveState(vu);
    if (state.pc == 0u || state.pc == SecondEntry || state.pc == DirectEntry)
    {
        if (g_active && g_next != g_expected.size())
        {
            g_countMismatches.fetch_add(1u);
            report("packet count", g_runs.load(), state.pc, static_cast<uint32_t>(g_next), static_cast<uint32_t>(g_expected.size()));
        }
        std::memcpy(g_memory.get(), args.vuData, args.dataSize);
        if (g_capture != nullptr && Hash == g_captureHash && g_captureLeft > 0 && state.pc == 0u && g_runs.load() % 61u == 0u)
        {
            std::fwrite(args.vuData, 1u, 0x4000u, g_capture);
            std::fwrite(state.vf, sizeof(state.vf), 1u, g_capture);
            std::fwrite(&state.top, sizeof(state.top), 1u, g_capture);
            std::fflush(g_capture);
            --g_captureLeft;
        }
        g_expected.clear();
        g_next = 0u;
        Run(g_memory.get(), state.vf, state.pc, state.top, g_expected);
        g_active = true;
        g_runs.fetch_add(1u);
        if (Hash == 0xAB0E7743A9BA7393ull)
        {
            g_envRuns.fetch_add(1u);
            int16_t flag;
            std::memcpy(&flag, args.vuData + 10 * 16 + 8, 2);
            if (flag > 0)
                g_envMappedRuns.fetch_add(1u);
        }
        else if (Hash == 0xE96770E4143D2DA7ull)
            g_waterRuns.fetch_add(1u);
        else
            g_trackRuns.fetch_add(1u);
    }
    else
    {
        static std::atomic<int> printed{0};
        if (printed.fetch_add(1) < 8)
            std::fprintf(stderr, "[vu1 ref] %016llx started at pc 0x%x\n", (unsigned long long)Hash, state.pc);
    }

    const auto result = VU1InterpreterAccess::runImage<Hash>(vu, args);

    if (result == VU1InterpreterAccess::StepResult::Budget && g_active)
    {
        // The original stopped early and nothing resumes it, so the C++ version, which finishes, can't be compared
        // past this point. The packets up to here were compared as usual.
        g_cutShort.fetch_add(1u);
        g_active = false;
    }
    if (result == VU1InterpreterAccess::StepResult::Ended && g_active)
    {
        if (g_next != g_expected.size())
        {
            g_countMismatches.fetch_add(1u);
            report("packet count at end", Hash >> 48, static_cast<uint32_t>(g_runs.load()), static_cast<uint32_t>(g_next), static_cast<uint32_t>(g_expected.size()));
        }
        for (uint32_t i = 0u; i + 4u <= args.dataSize; i += 4u)
        {
            uint32_t a, b;
            std::memcpy(&a, args.vuData + i, 4u);
            std::memcpy(&b, g_memory.get() + i, 4u);
            if (a != b)
            {
                g_memoryMismatches.fetch_add(1u);
                report("memory", g_runs.load(), i, a, b);
                break;
            }
        }
        g_active = false;
    }
    return result;
}

} // namespace

void installChecks()
{
    if (const char *stats = std::getenv("RC_VU1_STATS"); stats && stats[0] == '1')
    {
#define RC_REGISTER_STATS(hash, name) registerVu1CompiledProgram(hash, &ImageStats<hash>::run);
        RC_VU1_IMAGES(RC_REGISTER_STATS)
        std::thread(
            []
            {
                for (;;)
                {
                    std::this_thread::sleep_for(std::chrono::seconds(10));
#define RC_PRINT_STATS(hash, name) std::fprintf(stderr, "[vu1 stats] %-20s runs %10llu  time %8.2f s\n", name, (unsigned long long)ImageStats<hash>::runs.load(), ImageStats<hash>::nanoseconds.load() / 1e9);
                    RC_VU1_IMAGES(RC_PRINT_STATS)
                }
            })
            .detach();
        return;
    }
    const char *enabled = std::getenv("RC_VU1_REF");
    if (!enabled || enabled[0] != '1')
        return;
    g_memory.reset(new uint8_t[0x4000]);
    if (const char *capture = std::getenv("RC_VU1_CAPTURE"))
    {
        g_capture = std::fopen(capture, "wb");
        g_captureLeft = 1500;
    }
    g_vu1KickObserver = &observeKick;
    if (const char *hash = std::getenv("RC_VU1_CAPTURE_HASH"))
        g_captureHash = std::strtoull(hash, nullptr, 16);
    registerVu1CompiledProgram(0xC3952C0FAEB52EADull, &checked<0xC3952C0FAEB52EADull, &runTrdrShape, 0xFFFFFFFFu, 0x2050u>);
    registerVu1CompiledProgram(0xE96770E4143D2DA7ull, &checked<0xE96770E4143D2DA7ull, &checkedWater, 0xFFFFFFFFu, 0xFFFFFFFFu>);
    registerVu1CompiledProgram(0xAB0E7743A9BA7393ull, &checked<0xAB0E7743A9BA7393ull, &runEnvShape, 0x78u, 0x2370u>);
    std::thread(
        []
        {
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                std::fprintf(stderr, "[vu1 ref] runs %llu (track %llu, env %llu, env-mapped %llu, water %llu) packets %llu cut short %llu | mismatches: packets %llu, memory %llu, counts %llu\n",
                             (unsigned long long)g_runs.load(), (unsigned long long)g_trackRuns.load(), (unsigned long long)g_envRuns.load(),
                             (unsigned long long)g_envMappedRuns.load(), (unsigned long long)g_waterRuns.load(), (unsigned long long)g_packets.load(), (unsigned long long)g_cutShort.load(),
                             (unsigned long long)g_packetMismatches.load(), (unsigned long long)g_memoryMismatches.load(),
                             (unsigned long long)g_countMismatches.load());
            }
        })
        .detach();
}

} // namespace rc::vu1

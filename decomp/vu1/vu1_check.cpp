// Runs a C++ VU1 program beside the original and compares the GS packets and final VU memory.
#include <algorithm>
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

using RunFn = void (*)(uint8_t *, const float[32][4], uint32_t, uint32_t, std::vector<std::vector<uint8_t>> &);

// RC_VU1_SYNTH=<directory of program images>: for programs the demos never run. Real track data from the game is rearranged
// into each program's input format and run through both the original program, on a separate interpreter that sends
// nothing to the GS, and the C++ version. Lighting values, normals and the second matrix are generated.
struct Synth
{
    const char *name;
    std::vector<uint8_t> image;
    RunFn run;
    void (*build)(const uint8_t *track, uint8_t *memory, uint32_t top, uint32_t &newTop, uint32_t &seed);
    std::atomic<uint64_t> runs{0}, packets{0}, packetMismatches{0}, memoryMismatches{0}, countMismatches{0};
};

float randomFloat(uint32_t &seed, float low, float high)
{
    seed = seed * 1664525u + 1013904223u;
    return low + (high - low) * static_cast<float>(seed >> 8) / 16777216.0f;
}

void copyQuads(uint8_t *memory, int to, const uint8_t *from, int at, int count = 1)
{
    std::memcpy(memory + to * 16, from + at * 16, static_cast<size_t>(count) * 16u);
}

int16_t countAt(const uint8_t *memory, int quad)
{
    int16_t value;
    std::memcpy(&value, memory + (quad & 0x3FF) * 16 + 8, 2);
    return value;
}

// Replaces each triangle's colours with random normals.
void randomiseColours(uint8_t *memory, int reader, uint32_t &seed)
{
    for (int guard = 0; guard < 64; ++guard)
    {
        const int16_t count = countAt(memory, reader);
        if (count < 0)
        {
            reader += 1 - count;
            continue;
        }
        if (count == 0 || reader + 4 + count * 10 > 1024)
            return;
        for (int t = 0; t < count; ++t)
            for (int c = 0; c < 3; ++c)
                for (int lane = 0; lane < 3; ++lane)
                {
                    const float value = randomFloat(seed, -1.0f, 1.0f);
                    std::memcpy(memory + (reader + 4 + t * 10 + 7 + c) * 16 + lane * 4, &value, 4);
                }
        reader += 4 + count * 10;
    }
}

void buildTerdr(const uint8_t *track, uint8_t *memory, uint32_t top, uint32_t &newTop, uint32_t &seed)
{
    std::memcpy(memory, track, 0x4000u);
    copyQuads(memory, 14, track, 18, 4);
    copyQuads(memory, 12, track, 16, 2);
    copyQuads(memory, 10, track, 14, 2);
    copyQuads(memory, 8, track, 12);
    for (int quad = 18; quad < 25; ++quad)
        for (int lane = 0; lane < 4; ++lane)
        {
            const float value = randomFloat(seed, 0.0f, 80.0f);
            std::memcpy(memory + quad * 16 + lane * 4, &value, 4);
        }
    copyQuads(memory, 26, track, 25);
    copyQuads(memory, 36, track, 35);
    copyQuads(memory, 248, track, 247, 4);
    randomiseColours(memory, static_cast<int>(top & 0x3FFu), seed);
    newTop = top;
}

void buildRdr(const uint8_t *track, uint8_t *memory, uint32_t top, uint32_t &newTop, uint32_t &seed)
{
    std::memset(memory, 0, 0x4000u);
    copyQuads(memory, 22, track, 18, 4);
    for (int row = 0; row < 4; ++row)
    {
        for (int lane = 0; lane < 4; ++lane)
        {
            const float value = (lane == row ? 1.0f : 0.0f) + randomFloat(seed, -0.002f, 0.002f);
            std::memcpy(memory + (26 + row) * 16 + lane * 4, &value, 4);
        }
    }
    copyQuads(memory, 20, track, 16);
    copyQuads(memory, 21, track, 17);
    copyQuads(memory, 19, track, 15);
    copyQuads(memory, 18, track, 14);
    copyQuads(memory, 16, track, 12);
    const uint64_t tag[2] = {10u | (1ull << 15) | (1ull << 60), 0xEu};
    std::memcpy(memory, tag, 16);
    copyQuads(memory, 30, track, 25);
    copyQuads(memory, 40, track, 35);

    int reader = static_cast<int>(top & 0x3FFu);
    while (countAt(track, reader) < 0)
        reader += 1 - countAt(track, reader);
    int count = countAt(track, reader);
    if (count <= 0 || reader + 4 + count * 10 > 1024)
        count = 0;
    count = std::min(count, (1024 - 256 - 5) / 10);
    newTop = 256u;
    if (count == 0)
        return;
    copyQuads(memory, 256, track, reader, 4 + count * 10);
    const int16_t shortened = static_cast<int16_t>(count);
    std::memcpy(memory + 256 * 16 + 8, &shortened, 2);
    for (int t = 0; t < count; ++t)
    {
        // Half the triangles get the backface test.
        const int32_t skip = randomFloat(seed, 0.0f, 1.0f) < 0.5f ? 0 : 1;
        std::memcpy(memory + (256 + 4 + t * 10) * 16 + 12, &skip, 4);
    }
}

Synth g_synth[2] = {{"TERdr", {}, &runTerdrShape, &buildTerdr}, {"Rdr_VUShape", {}, &runRdrShape, &buildRdr}};
std::vector<std::vector<uint8_t>> *g_synthCapture = nullptr;
std::atomic<uint64_t> g_trackSeen{0};

void observeSynth(const uint8_t *data, size_t bytes)
{
    if (g_synthCapture != nullptr)
        g_synthCapture->emplace_back(data, data + bytes);
}

void runSynth(const uint8_t *track, const VU1State &state, GS &gs)
{
    static VU1Interpreter interpreter;
    static uint32_t seed = 12345u;
    static std::vector<uint8_t> original(0x4000u), native(0x4000u);
    for (Synth &synth : g_synth)
    {
        if (synth.image.empty())
            continue;
        uint32_t top = 0u;
        synth.build(track, original.data(), state.top, top, seed);
        std::memcpy(native.data(), original.data(), 0x4000u);

        interpreter.reset();
        float registers[32][4];
        std::memcpy(registers, state.vf, sizeof(registers));
        registers[0][0] = registers[0][1] = registers[0][2] = 0.0f;
        registers[0][3] = 1.0f;
        std::memcpy(interpreter.state().vf, registers, sizeof(registers));

        std::vector<std::vector<uint8_t>> fromOriginal, fromNative;
        auto *const saved = g_vu1KickObserver;
        g_synthCapture = &fromOriginal;
        g_vu1KickObserver = &observeSynth;
        interpreter.execute(synth.image.data(), 0x4000u, original.data(), 0x4000u, gs, nullptr, 0u, top, 0u, 1u << 26);
        g_vu1KickObserver = saved;
        g_synthCapture = nullptr;

        synth.run(native.data(), registers, 0u, top, fromNative);

        synth.runs.fetch_add(1u);
        synth.packets.fetch_add(fromOriginal.size());
        if (fromOriginal.size() != fromNative.size())
        {
            synth.countMismatches.fetch_add(1u);
            report(synth.name, synth.runs.load(), 0xC0u, static_cast<uint32_t>(fromOriginal.size()), static_cast<uint32_t>(fromNative.size()));
        }
        for (size_t i = 0u; i < std::min(fromOriginal.size(), fromNative.size()); ++i)
        {
            if (fromOriginal[i] == fromNative[i])
                continue;
            synth.packetMismatches.fetch_add(1u);
            size_t at = 0u;
            while (at < std::min(fromOriginal[i].size(), fromNative[i].size()) && fromOriginal[i][at] == fromNative[i][at])
                ++at;
            at &= ~size_t{3};
            uint32_t a = 0u, b = 0u;
            if (at + 4u <= fromOriginal[i].size())
                std::memcpy(&a, fromOriginal[i].data() + at, 4);
            if (at + 4u <= fromNative[i].size())
                std::memcpy(&b, fromNative[i].data() + at, 4);
            report(synth.name, i, static_cast<uint32_t>(at), a, b);
            break;
        }
        if (std::memcmp(original.data(), native.data(), 0x4000u) != 0)
        {
            synth.memoryMismatches.fetch_add(1u);
            size_t at = 0u;
            while (at < 0x4000u && original[at] == native[at])
                ++at;
            at &= ~size_t{3};
            uint32_t a, b;
            std::memcpy(&a, original.data() + at, 4);
            std::memcpy(&b, native.data() + at, 4);
            report("synthetic memory", synth.runs.load(), static_cast<uint32_t>(at), a, b);
        }
    }
}

void checkedWater(uint8_t *memory, const float registers[32][4], uint32_t pc, uint32_t top, std::vector<std::vector<uint8_t>> &kicks)
{
    runWaterShape(memory, registers, pc, top, kicks);
}


uint64_t g_captureHash = 0xC3952C0FAEB52EADull;
std::atomic<uint64_t> g_cutShort{0}; // Runs the runtime stopped at its cycle limit and never resumed.
std::atomic<uint64_t> g_trackRuns{0}, g_envRuns{0}, g_envMappedRuns{0}, g_waterRuns{0}, g_skinRuns{0}; // What the comparison has actually covered.

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
        else if (Hash == 0xEC08B22243D4DC8Cull)
            g_skinRuns.fetch_add(1u);
        else
            g_trackRuns.fetch_add(1u);
    }
    else
    {
        static std::atomic<int> printed{0};
        if (printed.fetch_add(1) < 8)
            std::fprintf(stderr, "[vu1 ref] %016llx started at pc 0x%x\n", (unsigned long long)Hash, state.pc);
    }

    if (Hash == 0xC3952C0FAEB52EADull && state.pc == 0u && !g_synth[0].image.empty() && g_trackSeen.fetch_add(1u) % 197u == 0u)
    {
        static std::vector<uint8_t> copy(0x4000u);
        std::memcpy(copy.data(), args.vuData, 0x4000u);
        runSynth(copy.data(), state, *args.gs);
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
    if (const char *directory = std::getenv("RC_VU1_SYNTH"))
    {
        const char *const files[2] = {"TERdr_VUShapeProg.bin", "Rdr_VUShapeProg.bin"};
        for (int i = 0; i < 2; ++i)
        {
            const std::string path = std::string(directory) + "/" + files[i];
            if (::FILE *file = std::fopen(path.c_str(), "rb"))
            {
                g_synth[i].image.resize(0x4000u);
                if (std::fread(g_synth[i].image.data(), 1u, 0x4000u, file) != 0x4000u)
                    g_synth[i].image.clear();
                std::fclose(file);
            }
        }
    }
    if (const char *hash = std::getenv("RC_VU1_CAPTURE_HASH"))
        g_captureHash = std::strtoull(hash, nullptr, 16);
    registerVu1CompiledProgram(0xC3952C0FAEB52EADull, &checked<0xC3952C0FAEB52EADull, &runTrdrShape, 0xFFFFFFFFu, 0x2050u>);
    registerVu1CompiledProgram(0xE96770E4143D2DA7ull, &checked<0xE96770E4143D2DA7ull, &checkedWater, 0xFFFFFFFFu, 0xFFFFFFFFu>);
    registerVu1CompiledProgram(0xEC08B22243D4DC8Cull, &checked<0xEC08B22243D4DC8Cull, &runSkinShape, 0xFFFFFFFFu, 0xFFFFFFFFu>);
    // These two have only been seen through RC_VU1_SYNTH; a real run is compared the moment one happens.
    registerVu1CompiledProgram(0x6EC8B9FF8FA44221ull, &checked<0x6EC8B9FF8FA44221ull, &runRdrShape, 0xFFFFFFFFu, 0xFFFFFFFFu>);
    registerVu1CompiledProgram(0xBAC581270F262987ull, &checked<0xBAC581270F262987ull, &runTerdrShape, 0xFFFFFFFFu, 0x2198u>);
    registerVu1CompiledProgram(0xAB0E7743A9BA7393ull, &checked<0xAB0E7743A9BA7393ull, &runEnvShape, 0x78u, 0x2370u>);
    std::thread(
        []
        {
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                for (const Synth &synth : g_synth)
                    if (!synth.image.empty())
                        std::fprintf(stderr, "[vu1 synth] %s runs %llu packets %llu | mismatches: packets %llu, memory %llu, counts %llu\n", synth.name,
                                     (unsigned long long)synth.runs.load(), (unsigned long long)synth.packets.load(), (unsigned long long)synth.packetMismatches.load(),
                                     (unsigned long long)synth.memoryMismatches.load(), (unsigned long long)synth.countMismatches.load());
                std::fprintf(stderr, "[vu1 ref] runs %llu (track %llu, env %llu, env-mapped %llu, water %llu, skin %llu) packets %llu cut short %llu | mismatches: packets %llu, memory %llu, counts %llu\n",
                             (unsigned long long)g_runs.load(), (unsigned long long)g_trackRuns.load(), (unsigned long long)g_envRuns.load(),
                             (unsigned long long)g_envMappedRuns.load(), (unsigned long long)g_waterRuns.load(), (unsigned long long)g_skinRuns.load(), (unsigned long long)g_packets.load(), (unsigned long long)g_cutShort.load(),
                             (unsigned long long)g_packetMismatches.load(), (unsigned long long)g_memoryMismatches.load(),
                             (unsigned long long)g_countMismatches.load());
            }
        })
        .detach();
}

} // namespace rc::vu1

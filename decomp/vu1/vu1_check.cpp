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

template <>
VU1InterpreterAccess::StepResult VU1InterpreterAccess::runImage<0xC3952C0FAEB52EADull>(VU1Interpreter &, const Vu1RunArgs &);

namespace rc::vu1 {

namespace {

std::vector<std::vector<uint8_t>> g_expected;
size_t g_next = 0u;
bool g_active = false;
std::unique_ptr<uint8_t[]> g_memory;

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

VU1InterpreterAccess::StepResult checkedTrdr(VU1Interpreter &vu, const Vu1RunArgs &args)
{
    const VU1State state = VU1InterpreterAccess::effectiveState(vu);
    if (state.pc == 0u || state.pc == 0x2050u)
    {
        if (g_active && g_next != g_expected.size())
            g_countMismatches.fetch_add(1u);
        std::memcpy(g_memory.get(), args.vuData, args.dataSize);
        g_expected.clear();
        g_next = 0u;
        runTrdrShape(g_memory.get(), state.vf, state.pc, state.top, g_expected);
        g_active = true;
        g_runs.fetch_add(1u);
    }

    const auto result = VU1InterpreterAccess::runImage<0xC3952C0FAEB52EADull>(vu, args);

    if (result == VU1InterpreterAccess::StepResult::Ended && g_active)
    {
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
    }
    return result;
}

} // namespace

void installChecks()
{
    const char *enabled = std::getenv("RC_VU1_REF");
    if (!enabled || enabled[0] != '1')
        return;
    g_memory.reset(new uint8_t[0x4000]);
    g_vu1KickObserver = &observeKick;
    registerVu1CompiledProgram(0xC3952C0FAEB52EADull, &checkedTrdr);
    std::thread(
        []
        {
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                std::fprintf(stderr, "[vu1 ref] runs %llu packets %llu | mismatches: packets %llu, memory %llu, counts %llu\n",
                             (unsigned long long)g_runs.load(), (unsigned long long)g_packets.load(),
                             (unsigned long long)g_packetMismatches.load(), (unsigned long long)g_memoryMismatches.load(),
                             (unsigned long long)g_countMismatches.load());
            }
        })
        .detach();
}

} // namespace rc::vu1

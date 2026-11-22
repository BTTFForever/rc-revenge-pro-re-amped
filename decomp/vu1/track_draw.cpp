// Draws track geometry and the environment-mapped pass with a vertex shader instead of running TRdr_VUShapeProg,
// EnvRdr_VUShapeProg and Rdr_VUSkinProg.
//
// Enable with RC_TRACK_SHADER=1 and PS2X_GS_HW=1. The program's non-triangle packets still go to the GS as they did,
// and each texture block's triangles become one batch in the GS command stream, drawn with the state in force at that
// point. decomp/shaders/trdr_shape.vert is the same file tools/shader_check.cpp checks against the VU1 program.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "ps2_vu1_impl.inl"
#include "runtime/gs/gs_track_batch.h"
#include "vu1_ref.h"

namespace {

constexpr uint64_t kOriginalTrack = 0xC3952C0FAEB52EADull;
constexpr uint64_t kShaderTrack = 0x5243545241434B31ull; // Not a real image hash: only a template argument.
constexpr uint64_t kOriginalWater = 0xE96770E4143D2DA7ull;
constexpr uint64_t kNativeWater = 0x524357415445524Eull; // Likewise.
constexpr uint64_t kOriginalSkin = 0xEC08B22243D4DC8Cull;
constexpr uint64_t kShaderSkin = 0x5243534B494E5331ull; // Likewise.
constexpr uint64_t kOriginalEnv = 0xAB0E7743A9BA7393ull;
constexpr uint64_t kShaderEnv = 0x5243454E56534831ull; // Likewise.

const char *const kTrackShader =
#include "trdr_shape_glsl.h"
    ;

int16_t loadCount(const uint8_t *memory, int quad)
{
    int16_t value;
    std::memcpy(&value, memory + (quad & 0x3FF) * 16 + 8, 2);
    return value;
}

void sendPacket(PS2Memory *memory, const uint8_t *data, size_t bytes)
{
    memory->submitGifPacket(GifPathId::Path1, data, static_cast<uint32_t>(bytes));
}

void kickPacket(PS2Memory *memory, const uint8_t *vu, int quad)
{
    std::vector<uint8_t> packet;
    rc::vu1::copyGifPacket(vu, quad, packet);
    sendPacket(memory, packet.data(), packet.size());
}

// One texture block's triangles, as a GS packet writing PRIM and then the batch's pointer.
// Skin (shapeMode 3) reads all of VU memory. `skinRun` numbers the program's runs; after a run's first block, which
// carries the memory, the others (`skinShared`) carry none: the backend keeps the run's copy (gs_track_batch.h).
void sendTriangles(PS2Memory *memory, const uint8_t *vu, int first, int triangles, int32_t shapeMode = 0, const void *block = nullptr,
                   uint32_t skinRun = 0u, bool skinShared = false)
{
    const size_t quadBytes = shapeMode == 3 ? 0x4000u : triangles * 160u;
    const size_t carried = skinShared ? 0u : quadBytes;
    auto *batch = static_cast<GSTrackBatch *>(std::malloc(offsetof(GSTrackBatch, quads) + std::max<size_t>(carried, sizeof(float))));
    batch->triangles = static_cast<uint32_t>(triangles);
    batch->quadBytes = shapeMode == 3 ? 0x4000u : 0u;
    std::memset(batch->draw, 0, sizeof(batch->draw));
    if (shapeMode == 3)
    {
        std::memcpy(batch->draw, vu + 21 * 16, 64);
        std::memcpy(batch->draw + 16, vu + 15 * 16, 16);
        std::memcpy(batch->draw + 20, vu + 16 * 16, 16);
        std::memcpy(batch->draw + 24, vu + 14 * 16, 16);
        std::memcpy(batch->draw + 28, vu + 10 * 16, 16);
        batch->draw[35] = 1.0f; // No tint.
        // The original overwrites the last matrix row's position lanes with the negated near plane.
        const float nearPlane = batch->draw[30];
        batch->draw[12] = batch->draw[13] = batch->draw[14] = 0.0f - nearPlane;
    }
    else
    {
        std::memcpy(batch->draw, vu + 18 * 16, 64);
        std::memcpy(batch->draw + 16, vu + 16 * 16, 16);
        std::memcpy(batch->draw + 20, vu + 17 * 16, 16);
        std::memcpy(batch->draw + 24, vu + 15 * 16, 16);
        std::memcpy(batch->draw + 28, vu + 12 * 16, 16);
        std::memcpy(batch->draw + 32, vu + 11 * 16, 16);
    }
    std::memcpy(batch->draw + 41, &shapeMode, 4);
    if (shapeMode == 3)
    {
        std::memcpy(batch->draw + 44, vu + 12 * 16, 16);
        batch->draw[60] = static_cast<float>(first);
        std::memcpy(batch->draw + kGSTrackSkinRun, &skinRun, 4);
        batch->draw[kGSTrackSkinShared] = skinShared ? 1.0f : 0.0f;
    }
    else if (shapeMode != 0)
    {
        std::memcpy(batch->draw + 44, vu + 7 * 16, 48);
        std::memcpy(batch->draw + 56, vu + 10 * 16, 16);
        std::memcpy(batch->draw + 60, block, 16);
    }
    if (shapeMode == 3)
    {
        if (!skinShared)
            std::memcpy(batch->quads, vu, 0x4000u);
    }
    else
        for (int i = 0; i < triangles * 10; ++i)
            std::memcpy(batch->quads + i * 4, vu + ((first + i) & 0x3FF) * 16, 16);

    // The per-triangle GIF tag the original would have used sets PRIM only if its PRE bit is on.
    uint64_t triangleTag;
    std::memcpy(&triangleTag, vu + 25 * 16, 8);
    const bool setPrim = ((triangleTag >> 46) & 1u) != 0u;
    const uint64_t prim = (triangleTag >> 47) & 0x7FFu;
    const uint64_t pointer = reinterpret_cast<uint64_t>(batch);
    if (setPrim)
    {
        const uint64_t packet[6] = {2u | (1ull << 15) | (1ull << 60), 0xEu, prim, 0x00u, pointer, kGSTrackBatchRegister};
        sendPacket(memory, reinterpret_cast<const uint8_t *>(packet), sizeof(packet));
    }
    else
    {
        const uint64_t packet[4] = {1u | (1ull << 15) | (1ull << 60), 0xEu, pointer, kGSTrackBatchRegister};
        sendPacket(memory, reinterpret_cast<const uint8_t *>(packet), sizeof(packet));
    }
}

} // namespace

template <>
VU1InterpreterAccess::StepResult VU1InterpreterAccess::runImage<kOriginalTrack>(VU1Interpreter &, const Vu1RunArgs &);

template <>
VU1InterpreterAccess::StepResult VU1InterpreterAccess::runImage<kShaderTrack>(VU1Interpreter &vu, const Vu1RunArgs &args)
{
    if (vu.m_state.pc != 0u || args.memory == nullptr)
        return runImage<kOriginalTrack>(vu, args); // Another entry point, or a run already in progress.

    uint8_t *const memory = args.vuData;
    kickPacket(args.memory, memory, 247);

    int reader = static_cast<int>(vu.m_state.top & 0x3FFu);
    int16_t count = loadCount(memory, reader);
    for (;;)
    {
        while (count < 0)
        {
            kickPacket(args.memory, memory, reader + 1);
            reader += 1 - count;
            count = loadCount(memory, reader);
        }
        if (count == 0)
            break;
        // Four quadwords of texture state go out through a packet at the start of VU memory.
        std::memcpy(memory + 3 * 16, memory + reader * 16, 64);
        reader += 4;
        kickPacket(args.memory, memory, 0);
        sendTriangles(args.memory, memory, reader, count);
        reader += count * 10;
        count = loadCount(memory, reader);
    }
    return StepResult::Ended;
}

template <>
VU1InterpreterAccess::StepResult VU1InterpreterAccess::runImage<kOriginalEnv>(VU1Interpreter &, const Vu1RunArgs &);

template <>
VU1InterpreterAccess::StepResult VU1InterpreterAccess::runImage<kShaderEnv>(VU1Interpreter &vu, const Vu1RunArgs &args)
{
    if (vu.m_state.pc != 0u || args.memory == nullptr)
        return runImage<kOriginalEnv>(vu, args); // Another entry point, or a run already in progress.

    uint8_t *const memory = args.vuData;
    int16_t flag;
    std::memcpy(&flag, memory + 10 * 16 + 8, 2);
    const bool environment = flag > 0;

    int reader = static_cast<int>(vu.m_state.top & 0x3FFu);
    int16_t count = loadCount(memory, reader);
    for (;;)
    {
        uint8_t block[16];
        std::memcpy(block, memory + reader * 16, 16);
        std::memcpy(memory + 3 * 16, memory + reader * 16, 64);
        if (environment) // The block's texture is replaced by the environment map.
            std::memcpy(memory + 3 * 16, memory + 10 * 16, 8);
        reader += 4;
        kickPacket(args.memory, memory, 0);
        sendTriangles(args.memory, memory, reader, count, environment ? 2 : 1, block);
        reader += count * 10;
        count = loadCount(memory, reader);
        while (count < 0)
        {
            kickPacket(args.memory, memory, reader + 1);
            reader += 1 - count;
            count = loadCount(memory, reader);
        }
        if (count == 0)
            break;
    }
    return StepResult::Ended;
}

// Water draws each triangle three times with GS state changes in between, so its order can't be batched for a shader
// without changing how overlapping water blends. For now the exact C++ version runs in place of the VU1 program.
template <>
VU1InterpreterAccess::StepResult VU1InterpreterAccess::runImage<kOriginalWater>(VU1Interpreter &, const Vu1RunArgs &);

template <>
VU1InterpreterAccess::StepResult VU1InterpreterAccess::runImage<kNativeWater>(VU1Interpreter &vu, const Vu1RunArgs &args)
{
    if (vu.m_state.pc != 0u || args.memory == nullptr)
        return runImage<kOriginalWater>(vu, args);

    std::vector<std::vector<uint8_t>> kicks;
    float after[32][4];
    rc::vu1::runWaterShape(args.vuData, vu.m_state.vf, 0u, vu.m_state.top, kicks, after);
    std::memcpy(vu.m_state.vf, after, sizeof(after));
    for (const std::vector<uint8_t> &packet : kicks)
        sendPacket(args.memory, packet.data(), packet.size());
    return StepResult::Ended;
}

template <>
VU1InterpreterAccess::StepResult VU1InterpreterAccess::runImage<kOriginalSkin>(VU1Interpreter &, const Vu1RunArgs &);

template <>
VU1InterpreterAccess::StepResult VU1InterpreterAccess::runImage<kShaderSkin>(VU1Interpreter &vu, const Vu1RunArgs &args)
{
    if (vu.m_state.pc != 0u || args.memory == nullptr)
        return runImage<kOriginalSkin>(vu, args);

    uint8_t *const memory = args.vuData;
    int reader = 512;
    int16_t count = loadCount(memory, reader);
    // The loop changes only quadword 4 (the texture state it sends), which the shader never reads: the run's first
    // block carries VU memory for all of them. A model of one-triangle blocks sent ~9,000 copies of 16 KB a frame.
    static uint32_t runs = 0u;
    const uint32_t run = ++runs;
    bool shared = false;
    for (;;)
    {
        std::memcpy(memory + 4 * 16, memory + reader * 16, 16);
        kickPacket(args.memory, memory, 0);
        sendTriangles(args.memory, memory, reader + 1, count, 3, nullptr, run, shared);
        shared = true;
        reader += 1 + count * 16;
        count = loadCount(memory, reader);
        if (count == 0)
            break;
    }
    return StepResult::Ended;
}

namespace {

// The GS backend builds its shaders during start-up, before the game's overrides run.
const bool g_shaderProvided = []
{
    const char *enabled = std::getenv("RC_TRACK_SHADER");
    if (enabled && enabled[0] == '1')
        g_gsTrackVertexShader = kTrackShader;
    return true;
}();

} // namespace

namespace rc::vu1 {

void installTrackDraw()
{
    const char *enabled = std::getenv("RC_TRACK_SHADER");
    if (!enabled || enabled[0] != '1')
        return;
    registerVu1CompiledProgram(kOriginalTrack, &VU1InterpreterAccess::runImage<kShaderTrack>);
    registerVu1CompiledProgram(kOriginalEnv, &VU1InterpreterAccess::runImage<kShaderEnv>);
    registerVu1CompiledProgram(kOriginalSkin, &VU1InterpreterAccess::runImage<kShaderSkin>);
    registerVu1CompiledProgram(kOriginalWater, &VU1InterpreterAccess::runImage<kNativeWater>);
    std::fprintf(stderr, "[decomp] drawing track geometry with the vertex shader\n");
}

} // namespace rc::vu1

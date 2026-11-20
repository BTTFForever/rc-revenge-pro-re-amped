// C++ versions of the game's VU1 microprograms.
#pragma once

#include <cstdint>
#include <vector>

namespace rc::vu1 {

// Runs TRdr_VUShapeProg on a copy of VU data memory. `registers` are the vector registers at the start,
// because the output packets carry components the program never writes. Appends each packet the program
// sends to the GS.
void runTrdrShape(uint8_t *memory, const float registers[32][4], uint32_t startPc, uint32_t top,
                  std::vector<std::vector<uint8_t>> &kicks);

// The same for EnvRdr_VUShapeProg, the environment-mapped pass.
void runEnvShape(uint8_t *memory, const float registers[32][4], uint32_t startPc, uint32_t top,
                 std::vector<std::vector<uint8_t>> &kicks);

// The same for Rdr_VUWaterProg. `after`, if given, receives the vector registers as the program leaves them.
void runWaterShape(uint8_t *memory, const float registers[32][4], uint32_t startPc, uint32_t top,
                   std::vector<std::vector<uint8_t>> &kicks, float (*after)[4] = nullptr);

// The same for Rdr_VUSkinProg.
void runSkinShape(uint8_t *memory, const float registers[32][4], uint32_t startPc, uint32_t top,
                  std::vector<std::vector<uint8_t>> &kicks);

// The same for TERdr_VUShapeProg (Track Editor pieces) and Rdr_VUShapeProg.
void runTerdrShape(uint8_t *memory, const float registers[32][4], uint32_t startPc, uint32_t top,
                   std::vector<std::vector<uint8_t>> &kicks);
void runRdrShape(uint8_t *memory, const float registers[32][4], uint32_t startPc, uint32_t top,
                 std::vector<std::vector<uint8_t>> &kicks);

// The entry the renderer's VIF1 reader uses (RENDERER_3D_DESIGN.md 5.2): VU data memory and the vector registers are
// changed in place, as the program leaves them, and each XGKICK is handed to `packet` as it happens. The functions
// above are wrappers kept for RC_VU1_REF, RC_VU1_SYNTH and tools/shader_check: they copy the registers and collect
// the packets. The host FPU must round toward zero (MXCSR bits 13-14 set) during a run, as for the VU1.
struct Io
{
    uint8_t *memory;  // 16 KB of VU data memory, changed in place
    float (*vf)[4];   // 32 registers in, and out as the program leaves them
    uint32_t startPc, top;
    void (*packet)(void *user, const uint8_t *data, uint32_t bytes); // one XGKICK
    void *user;
};
enum Program { kTrdr, kEnv, kWater, kSkin, kTerdr, kRdr, kProgramCount };
const char *programName(Program);
// Whether `startPc` (a byte address) is an entry the reference reproduces.
bool isEntry(Program, uint32_t startPc);
// False (and nothing run) when the start address is not an entry of that program.
bool run(Program, Io &);
void runTrdrShapeIo(Io &);
void runEnvShapeIo(Io &);
void runWaterShapeIo(Io &);
void runSkinShapeIo(Io &);
void runTerdrShapeIo(Io &);
void runRdrShapeIo(Io &);

// Copies one GIF packet, starting at a VU memory quadword: tags and data up to the end-of-packet tag.
void copyGifPacket(const uint8_t *memory, int quad, std::vector<uint8_t> &packet);

// With RC_TRACK_SHADER=1 on the render-target GS backend, draws track geometry with the vertex shader instead of VU1.
void installTrackDraw();

// With RC_VU1_REF=1, compares the C++ version against the original on every run. Call after the
// generated programs are registered.
void installChecks();

} // namespace rc::vu1

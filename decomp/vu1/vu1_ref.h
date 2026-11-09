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

// Copies one GIF packet, starting at a VU memory quadword: tags and data up to the end-of-packet tag.
void copyGifPacket(const uint8_t *memory, int quad, std::vector<uint8_t> &packet);

// With RC_TRACK_SHADER=1 on the render-target GS backend, draws track geometry with the vertex shader instead of VU1.
void installTrackDraw();

// With RC_VU1_REF=1, compares the C++ version against the original on every run. Call after the
// generated programs are registered.
void installChecks();

} // namespace rc::vu1

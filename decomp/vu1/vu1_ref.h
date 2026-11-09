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

// With RC_VU1_REF=1, compares the C++ version against the original on every run. Call after the
// generated programs are registered.
void installChecks();

} // namespace rc::vu1

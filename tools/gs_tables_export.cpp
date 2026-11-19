// Writes the GS memory layout tables from the runtime for tools/bbk_export.py.
// Build: g++ -std=c++20 -I<runtime>/include gs_tables_export.cpp <runtime>/src/lib/gs/ps2_gs_memory.cpp
#include <cstdint>
#include <cstdio>

#include "runtime/gs/ps2_gs_memory.h"

int main(int, char **argv)
{
    GSMem::InitLookupTables();
    FILE *out = std::fopen(argv[1], "wb");
    const struct { GSMem::PixelStorageMode mode; uint32_t psm, shiftX, shiftY; } formats[] = {
        {GSMem::PixelStorageMode::C32, 0x00, 6, 5},
        {GSMem::PixelStorageMode::C16, 0x02, 6, 6},
        {GSMem::PixelStorageMode::P8, 0x13, 7, 6},
        {GSMem::PixelStorageMode::P4, 0x14, 7, 7},
        // (renderer step 7 batch 3) for rc_tex_test: CT16S and the Z buffer formats (Z24 shares Z32's table).
        {GSMem::PixelStorageMode::C16S, 0x0A, 6, 6},
        {GSMem::PixelStorageMode::Z32, 0x30, 6, 5},
        {GSMem::PixelStorageMode::Z16, 0x32, 6, 6},
        {GSMem::PixelStorageMode::Z16S, 0x3A, 6, 6},
    };
    for (const auto &f : formats)
    {
        const uint32_t count = 32u << (f.shiftX + f.shiftY);
        const uint32_t header[4] = {f.psm, f.shiftX, f.shiftY, count};
        std::fwrite(header, sizeof(header), 1, out);
        std::fwrite(GSMem::PageTableData(f.mode), sizeof(uint16_t), count, out);
    }
    std::fclose(out);
}

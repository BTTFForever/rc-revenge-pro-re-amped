#pragma once

// A minimal PNG writer (8-bit RGB, one IDAT through zlib) for rc_replay's pictures.

#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace rc::replay
{
    inline void pngChunk(std::FILE *f, const char *type, const uint8_t *data, uint32_t bytes)
    {
        const uint8_t len[4] = {static_cast<uint8_t>(bytes >> 24), static_cast<uint8_t>(bytes >> 16), static_cast<uint8_t>(bytes >> 8),
                                static_cast<uint8_t>(bytes)};
        std::fwrite(len, 1, 4, f);
        std::fwrite(type, 1, 4, f);
        if (bytes)
            std::fwrite(data, 1, bytes, f);
        uLong crc = crc32(0L, reinterpret_cast<const Bytef *>(type), 4);
        if (bytes)
            crc = crc32(crc, data, bytes);
        const uint8_t c[4] = {static_cast<uint8_t>(crc >> 24), static_cast<uint8_t>(crc >> 16), static_cast<uint8_t>(crc >> 8),
                              static_cast<uint8_t>(crc)};
        std::fwrite(c, 1, 4, f);
    }

    // rgba: width * height * 4 bytes; the alpha byte is dropped (the GS stores 0x80 for opaque).
    inline bool writePng(const std::string &path, const uint8_t *rgba, uint32_t width, uint32_t height)
    {
        std::vector<uint8_t> raw;
        raw.reserve(static_cast<size_t>(height) * (width * 3u + 1u));
        for (uint32_t y = 0; y < height; ++y)
        {
            raw.push_back(0);
            const uint8_t *row = rgba + static_cast<size_t>(y) * width * 4u;
            for (uint32_t x = 0; x < width; ++x)
                raw.insert(raw.end(), row + x * 4u, row + x * 4u + 3u);
        }
        uLongf packedBytes = compressBound(static_cast<uLong>(raw.size()));
        std::vector<uint8_t> packed(packedBytes);
        if (compress2(packed.data(), &packedBytes, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK)
            return false;
        std::FILE *f = std::fopen(path.c_str(), "wb");
        if (!f)
            return false;
        static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        std::fwrite(sig, 1, 8, f);
        const uint8_t ihdr[13] = {static_cast<uint8_t>(width >> 24), static_cast<uint8_t>(width >> 16), static_cast<uint8_t>(width >> 8),
                                  static_cast<uint8_t>(width), static_cast<uint8_t>(height >> 24), static_cast<uint8_t>(height >> 16),
                                  static_cast<uint8_t>(height >> 8), static_cast<uint8_t>(height), 8, 2, 0, 0, 0};
        pngChunk(f, "IHDR", ihdr, 13);
        pngChunk(f, "IDAT", packed.data(), static_cast<uint32_t>(packedBytes));
        pngChunk(f, "IEND", nullptr, 0);
        return std::fclose(f) == 0;
    }
}

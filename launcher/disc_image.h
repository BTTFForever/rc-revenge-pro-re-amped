// Reads a PS2 disc image without mounting it (the C++ counterpart of tools/iso_extract.py): a 2048-byte-sector .iso or
// a raw 2352-byte-sector .bin (Mode 1 or Mode 2 Form 1), a .cue followed to its .bin.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace launcher {

class DiscImage
{
public:
    // Opens the image (a .cue: its first FILE entry). On failure, error() says why.
    explicit DiscImage(const std::filesystem::path &path);
    ~DiscImage();
    DiscImage(const DiscImage &) = delete;
    DiscImage &operator=(const DiscImage &) = delete;

    bool ok() const { return m_file != nullptr && m_error.empty(); }
    const std::string &error() const { return m_error; }
    const std::filesystem::path &path() const { return m_path; } // the data file (.cue resolved)
    bool raw() const { return m_raw; }
    uint64_t sectors() const { return m_sectors; }

    // A file of the root directory or below ("SYSTEM.CNF", "IRX/X.IRX"), case-insensitive; nullopt if missing.
    std::optional<std::vector<uint8_t>> readFile(const std::string &name);
    // The boot executable's name from SYSTEM.CNF's BOOT2 line ("SLUS_201.53"); empty with error() set if none.
    std::string bootName();

    // Writes the 2048-byte-sector ISO of a raw image; progress goes 0..1. False with error() set on failure.
    bool writeIso(const std::filesystem::path &out, std::atomic<float> &progress);

private:
    bool sector(uint64_t lba, uint32_t count, uint8_t *out);
    std::optional<std::pair<uint32_t, uint32_t>> find(const std::string &name); // extent, size

    std::filesystem::path m_path;
    std::FILE *m_file = nullptr;
    std::string m_error;
    bool m_raw = false;
    uint32_t m_offset = 0;
    uint64_t m_sectors = 0;
};

// The SHA-1 of some bytes, as lowercase hex.
std::string sha1Hex(const uint8_t *data, size_t size);

} // namespace launcher

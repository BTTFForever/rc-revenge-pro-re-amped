#include "disc_image.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>

namespace launcher {
namespace {

constexpr uint32_t kSector = 2048, kRaw = 2352;
constexpr uint8_t kSync[12] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};

std::string upper(std::string s)
{
    for (char &c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Paths may hold non-ASCII characters: open them as wide strings on Windows, and seek with 64-bit offsets.
std::FILE *openFile(const std::filesystem::path &path, const char *mode)
{
#ifdef _WIN32
    const std::wstring wmode(mode, mode + std::strlen(mode));
    return _wfopen(path.c_str(), wmode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

bool seekTo(std::FILE *f, uint64_t offset)
{
#ifdef _WIN32
    return _fseeki64(f, static_cast<long long>(offset), SEEK_SET) == 0;
#else
    return fseeko(f, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }

// A .cue's first FILE entry, else the path itself.
std::filesystem::path resolveCue(const std::filesystem::path &path, std::string &error)
{
    if (upper(path.extension().string()) != ".CUE")
        return path;
    std::ifstream in(path);
    if (!in)
    {
        error = "cannot read " + path.string();
        return {};
    }
    std::stringstream text;
    text << in.rdbuf();
    const std::string s = text.str();
    std::smatch m;
    if (std::regex_search(s, m, std::regex(R"(FILE\s+\"([^\"]+)\")")) || std::regex_search(s, m, std::regex(R"(FILE\s+(\S+))")))
        return path.parent_path() / m[1].str();
    error = path.filename().string() + " has no FILE line";
    return {};
}

} // namespace

DiscImage::DiscImage(const std::filesystem::path &path)
{
    m_path = resolveCue(std::filesystem::absolute(path), m_error);
    if (!m_error.empty())
        return;
    m_file = openFile(m_path, "rb");
    if (!m_file)
    {
        m_error = "cannot open " + m_path.string();
        return;
    }
    std::error_code ec;
    const uint64_t size = std::filesystem::file_size(m_path, ec);
    uint8_t head[16] = {};
    if (ec || std::fread(head, 1, sizeof head, m_file) != sizeof head)
    {
        m_error = m_path.filename().string() + " is too small to be a disc image";
        return;
    }
    m_raw = size % kRaw == 0 && std::memcmp(head, kSync, sizeof kSync) == 0;
    if (m_raw)
    {
        if (head[15] == 1)
            m_offset = 16;
        else if (head[15] == 2)
            m_offset = 24; // Mode 2 Form 1: the sub-header, then 2048 bytes of user data
        else
        {
            m_error = m_path.filename().string() + ": unknown sector mode";
            return;
        }
        m_sectors = size / kRaw;
    }
    else
    {
        if (size % kSector)
        {
            m_error = m_path.filename().string() + " is neither an ISO (2048-byte sectors) nor a raw BIN (2352-byte sectors)";
            return;
        }
        m_sectors = size / kSector;
    }
    uint8_t pvd[kSector];
    if (!sector(16, 1, pvd) || pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0)
        m_error = m_path.filename().string() + " has no ISO 9660 file system";
}

DiscImage::~DiscImage()
{
    if (m_file)
        std::fclose(m_file);
}

bool DiscImage::sector(uint64_t lba, uint32_t count, uint8_t *out)
{
    if (lba + count > m_sectors)
        return false;
    if (!m_raw)
        return seekTo(m_file, lba * kSector) &&
               std::fread(out, kSector, count, m_file) == count;
    for (uint32_t i = 0; i < count; ++i)
        if (!seekTo(m_file, (lba + i) * kRaw + m_offset) ||
            std::fread(out + size_t(i) * kSector, kSector, 1, m_file) != 1)
            return false;
    return true;
}

std::optional<std::pair<uint32_t, uint32_t>> DiscImage::find(const std::string &name)
{
    uint8_t pvd[kSector];
    if (!sector(16, 1, pvd))
        return std::nullopt;
    uint32_t extent = le32(pvd + 156 + 2), size = le32(pvd + 156 + 10);
    std::string rest = upper(name);
    while (!rest.empty())
    {
        const size_t slash = rest.find('/');
        const std::string part = rest.substr(0, slash);
        rest = slash == std::string::npos ? "" : rest.substr(slash + 1);
        if (size > 1u << 20)
            return std::nullopt;
        std::vector<uint8_t> dir((size + kSector - 1) / kSector * kSector);
        if (!sector(extent, static_cast<uint32_t>(dir.size() / kSector), dir.data()))
            return std::nullopt;
        bool found = false;
        for (size_t pos = 0; pos < size && !found;)
        {
            const uint8_t n = dir[pos];
            if (n == 0) // records do not cross sectors
            {
                pos = (pos / kSector + 1) * kSector;
                continue;
            }
            if (pos + n > dir.size() || n < 34)
                return std::nullopt;
            const uint8_t *rec = dir.data() + pos;
            std::string entry(reinterpret_cast<const char *>(rec + 33), rec[32]);
            entry = upper(entry.substr(0, entry.find(';')));
            if (entry == part)
            {
                extent = le32(rec + 2);
                size = le32(rec + 10);
                found = true;
            }
            pos += n;
        }
        if (!found)
            return std::nullopt;
    }
    return std::make_pair(extent, size);
}

std::optional<std::vector<uint8_t>> DiscImage::readFile(const std::string &name)
{
    const auto where = find(name);
    if (!where)
        return std::nullopt;
    std::vector<uint8_t> data((where->second + kSector - 1) / kSector * kSector);
    if (!data.empty() && !sector(where->first, static_cast<uint32_t>(data.size() / kSector), data.data()))
        return std::nullopt;
    data.resize(where->second);
    return data;
}

std::string DiscImage::bootName()
{
    const auto cnf = readFile("SYSTEM.CNF");
    if (cnf)
    {
        const std::string text(cnf->begin(), cnf->end());
        std::smatch m;
        if (std::regex_search(text, m, std::regex(R"(BOOT2\s*=\s*cdrom0:\\?([^;\s]+))", std::regex::icase)))
            return m[1].str();
    }
    m_error = m_path.filename().string() + " is not a PS2 disc (no SYSTEM.CNF with a BOOT2 line)";
    return {};
}

bool DiscImage::writeIso(const std::filesystem::path &out, std::atomic<float> &progress)
{
    std::FILE *f = openFile(out, "wb");
    if (!f)
    {
        m_error = "cannot write " + out.string();
        return false;
    }
    std::vector<uint8_t> buffer(256 * kSector);
    for (uint64_t lba = 0; lba < m_sectors; lba += 256)
    {
        const uint32_t count = static_cast<uint32_t>(std::min<uint64_t>(256, m_sectors - lba));
        if (!sector(lba, count, buffer.data()) || std::fwrite(buffer.data(), kSector, count, f) != count)
        {
            std::fclose(f);
            m_error = "failed while converting the image (disk full?)";
            return false;
        }
        progress = float(lba + count) / float(m_sectors);
    }
    if (std::fclose(f) != 0)
    {
        m_error = "failed while converting the image (disk full?)";
        return false;
    }
    return true;
}

// ---- SHA-1 (FIPS 180-1) ----
std::string sha1Hex(const uint8_t *data, size_t size)
{
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    auto rol = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
    std::vector<uint8_t> msg(data, data + size);
    msg.push_back(0x80);
    while (msg.size() % 64 != 56)
        msg.push_back(0);
    const uint64_t bits = uint64_t(size) * 8;
    for (int i = 7; i >= 0; --i)
        msg.push_back(static_cast<uint8_t>(bits >> (i * 8)));
    for (size_t block = 0; block < msg.size(); block += 64)
    {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(msg[block + 4 * i]) << 24) | (msg[block + 4 * i + 1] << 16) | (msg[block + 4 * i + 2] << 8) | msg[block + 4 * i + 3];
        for (int i = 16; i < 80; ++i)
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i)
        {
            uint32_t f, k;
            if (i < 20)
                f = (b & c) | (~b & d), k = 0x5A827999u;
            else if (i < 40)
                f = b ^ c ^ d, k = 0x6ED9EBA1u;
            else if (i < 60)
                f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDCu;
            else
                f = b ^ c ^ d, k = 0xCA62C1D6u;
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    char hex[41];
    for (int i = 0; i < 5; ++i)
        std::snprintf(hex + 8 * i, 9, "%08x", h[i]);
    return std::string(hex, 40);
}

} // namespace launcher

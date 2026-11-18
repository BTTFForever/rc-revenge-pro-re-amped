// Builds the random-test inputs of RES.c, LOADING.c, SYSTIMER.c, TIMER.c and BACKDROP.c. See res_fuzz.h.

#include "res_fuzz.h"

#include <algorithm>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <vector>

// The runtime's hook on stores to hardware registers (ps2_runtime.cpp), for the oracle only.
extern bool (*g_ps2HardwareStoreHook)(uint32_t address, uint32_t value);

namespace rc {

bool_ MemBlockAllocLo(ptr32<memHandle> pHandle, uint32 size); // mem.cpp
void MemBlockFree(ptr32<memHandle> pHandle);

namespace fuzz {

namespace {

constexpr uint32_t kRes = sym::resVarG, kFmFiles = sym::fmVarG, kSysTimer = sym::sysTimerVarG;
constexpr uint32_t kFMFileOpen = sym::FMFileOpen, kFMFileRead = sym::FMFileRead;
constexpr uint32_t kLoadErrorCallback = sym::LoadErrorCallback, kResReAlloc = sym::ResReAlloc;
constexpr uint32_t kDisplayInitHiRes = sym::DisplayInitHiRes, kSwapBuffers = sym::GfxP2_SwapBuffers, kSyncV = sym::sceGsSyncV, kFlushCache = sym::FlushCache,
                   kSyncPath = sym::sceGsSyncPath, kExecLoadImage = sym::sceGsExecLoadImage;
constexpr uint32_t kSifAllocIopHeap = sym::sceSifAllocIopHeap, kSifFreeIopHeap = sym::sceSifFreeIopHeap, kSdTransToIOP = sym::sceSdTransToIOP, kSifCallRpc = sym::sceSifCallRpc;
constexpr uint32_t kMpegLoad = sym::mpegOnInterruptLOAD, kMpegPlay = sym::mpegOnInterruptPLAY, kMpegTerm = sym::mpegOnInterruptTERM, kMpegFinished = sym::mpegOnInterruptFINISHED,
                   kRCMusicStop = sym::RCMusicStop, kInputSnapshot = sym::InputSnapshot;
constexpr uint32_t kAddIntcHandler2 = sym::AddIntcHandler2, kRemoveIntcHandler = sym::RemoveIntcHandler;
constexpr uint32_t kInputKeys = sym::inputVarG + 0x5cu; // inputVarG's hit flags (bit 0x20), by key
constexpr uint32_t kLoadArea = 0x200000u; // a load area, more than any setup's loads need

// Control words of the fakes (all in scratch, so each run starts from the same values and they are compared).
constexpr uint32_t kEnv = kFuzzScratch + 0x7a00u;
enum : uint32_t
{
    eFailMask = 0x0,       // bit k: the k-th FMFileRead fails
    eReadCalls = 0x4,
    eFinishedAfter = 0x8,  // mpegOnInterruptFINISHED answers 1 from this call on
    eFinishedCalls = 0xc,
    eInputCalls = 0x10,
    eHitAt = 0x14,         // the InputSnapshot call that presses eHitKey
    eHitKey = 0x18,
    eIopHeap = 0x1c,       // sceSifAllocIopHeap's answer
    eIntcId = 0x20,        // AddIntcHandler2's answer
    eFailKind = 0x24,      // two bits per failing read: short, corrupt, empty, long
    eHash = 0x28,          // a hash of every logged call, so calls past the log's end are compared too
    eOpenFailMask = 0x2c,  // bit k: the k-th FMFileOpen fails
    eOpenCalls = 0x30,
};
constexpr uint32_t kLog = kFuzzScratch + 0x7c00u, kLogMax = 50u; // count word, then 5 words an entry
constexpr uint32_t kHardwareStore = 1u;

template <typename T>
T load(uint32_t address)
{
    T value;
    std::memcpy(&value, guest_host(address), sizeof(T));
    return value;
}

template <typename T>
void store(uint32_t address, T value)
{
    std::memcpy(guest_host(address), &value, sizeof(T));
}

uint32_t rnd(uint64_t &state, uint32_t n) { return n == 0 ? 0u : nextFuzz(state) % n; }
bool chance(uint64_t &state, uint32_t percent) { return rnd(state, 100) < percent; }
void setArg(R5900Context *ctx, int index, uint32_t value) { SET_GPR_S32(ctx, 4 + index, static_cast<int32_t>(value)); }
uint32_t env(uint32_t offset) { return load<uint32_t>(kEnv + offset); }
void setEnv(uint32_t offset, uint32_t value) { store<uint32_t>(kEnv + offset, value); }

void putString(uint32_t at, const std::string &text)
{
    std::memcpy(guest_host(at), text.c_str(), text.size() + 1u);
}

// --- The disc image, read directly (ISO 9660). ---

struct Iso
{
    int fd = -1;
    std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> files; // "CAT/RDS.CAT" -> (sector, size)
};

std::string normalize(std::string path)
{
    if (const auto colon = path.find(':'); colon != std::string::npos)
        path.erase(0, colon + 1);
    if (const auto version = path.rfind(';'); version != std::string::npos)
        path.erase(version);
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path.front() == '/')
        path.erase(0, 1);
    for (char &c : path)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return path;
}

void readIso(const Iso &iso, uint64_t offset, void *out, size_t size)
{
    std::memset(out, 0, size);
    if (iso.fd >= 0 && size != 0)
    {
        const ssize_t got = pread(iso.fd, out, size, static_cast<off_t>(offset));
        (void)got;
    }
}

uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

void walk(Iso &iso, uint32_t lsn, uint32_t size, const std::string &prefix, int depth)
{
    if (depth > 8)
        return;
    std::vector<uint8_t> dir(size);
    readIso(iso, static_cast<uint64_t>(lsn) * 2048u, dir.data(), size);
    for (uint32_t pos = 0; pos + 33 < size;)
    {
        const uint8_t length = dir[pos];
        if (length == 0)
        {
            pos = (pos / 2048u + 1u) * 2048u;
            continue;
        }
        const uint8_t *record = dir.data() + pos;
        const std::string name(reinterpret_cast<const char *>(record + 33), record[32]);
        pos += length;
        if (name.size() == 1 && (name[0] == '\0' || name[0] == '\1'))
            continue;
        if ((record[25] & 2u) != 0)
            walk(iso, le32(record + 2), le32(record + 10), prefix + name + "/", depth + 1);
        else
            iso.files[normalize(prefix + name)] = {le32(record + 2), le32(record + 10)};
    }
}

const Iso &iso()
{
    static const Iso image = []
    {
        Iso result;
        const char *path = std::getenv("RC_ISO");
#if RC_VERSION_EU
        constexpr const char *kDefault = "game/eu/RC Revenge Pro (Europe).iso"; // tools/setup.sh; run from the repository
#else
        constexpr const char *kDefault = "game/usa/RC Revenge Pro (USA).iso";
#endif
        result.fd = open(path ? path : kDefault, O_RDONLY);
        if (result.fd < 0)
        {
            std::fprintf(stderr, "[res_fuzz] no disc image (set RC_ISO)\n");
            return result;
        }
        uint8_t pvd[2048];
        readIso(result, 16u * 2048u, pvd, sizeof(pvd));
        walk(result, le32(pvd + 156 + 2), le32(pvd + 156 + 10), "", 0);
        return result;
    }();
    return image;
}

std::pair<uint32_t, uint32_t> isoFile(const char *name)
{
    const auto found = iso().files.find(name);
    return found == iso().files.end() ? std::pair<uint32_t, uint32_t>{0, 0} : found->second;
}

// The database as the game reads it: each id's lookup flags, the CAT's offsets, and per CAT resource its header.
struct Resource
{
    uint32_t id, offset, typeVal, fixups, swizzles, length, footprint;
    uint32_t closure = 0; // its footprint and those of every resource its swizzles load, each once (16-aligned)
};
struct Database
{
    uint32_t maxId = 0;
    std::vector<uint32_t> flags, offsets;
    std::vector<Resource> all, small, withFixups, withSwizzles;
};

const Database &database()
{
    static const Database db = []
    {
        Database result;
        const auto [lufSector, lufSize] = isoFile("RC2.LUF");
        const auto [catSector, catSize] = isoFile("CAT/RDS.CAT");
        if (lufSize < 4u || catSize < 8u)
            return result;
        std::vector<uint8_t> luf(lufSize);
        readIso(iso(), static_cast<uint64_t>(lufSector) * 2048u, luf.data(), lufSize);
        result.maxId = le32(luf.data());
        result.flags.resize(result.maxId);
        result.offsets.resize(result.maxId);
        std::vector<uint8_t> offsets(result.maxId * 4u);
        readIso(iso(), static_cast<uint64_t>(catSector) * 2048u + 8u, offsets.data(), offsets.size());
        for (uint32_t i = 0; i < result.maxId; ++i)
        {
            result.flags[i] = 4u + i * 4u + 4u <= lufSize ? le32(luf.data() + 4u + i * 4u) : 0u;
            result.offsets[i] = le32(offsets.data() + i * 4u);
            if ((result.flags[i] & 0x400u) == 0u || i == 0)
                continue;
            uint8_t header[0x1c];
            readIso(iso(), static_cast<uint64_t>(catSector) * 2048u + result.offsets[i], header, sizeof(header));
            Resource r{i, result.offsets[i], le32(header + 12), static_cast<uint32_t>(header[20] | header[21] << 8),
                       static_cast<uint32_t>(header[22] | header[23] << 8), le32(header + 24), 0};
            r.footprint = (r.fixups + r.swizzles) * 4u + 8u + r.length;
            result.all.push_back(r);
        }
        // What each resource loads with it (its swizzled ids), for the room a load needs.
        std::unordered_map<uint32_t, size_t> index;
        std::vector<std::vector<uint32_t>> deps(result.all.size());
        for (size_t k = 0; k < result.all.size(); ++k)
        {
            const Resource &r = result.all[k];
            index[r.id] = k;
            if (r.swizzles == 0u)
                continue;
            std::vector<uint8_t> bytes(0x1cu + r.footprint);
            readIso(iso(), static_cast<uint64_t>(catSector) * 2048u + r.offset, bytes.data(), bytes.size());
            const uint32_t base = 0x1cu + (r.fixups + r.swizzles) * 4u + 8u;
            for (uint32_t i = 0; i < r.swizzles; ++i)
            {
                const uint32_t at = base + le32(bytes.data() + 0x1cu + (r.fixups + i) * 4u);
                if (at + 4u <= bytes.size())
                    deps[k].push_back(le32(bytes.data() + at));
            }
        }
        for (size_t k = 0; k < result.all.size(); ++k)
        {
            std::vector<uint32_t> todo{result.all[k].id};
            std::unordered_map<uint32_t, bool> seen;
            uint64_t total = 0;
            while (!todo.empty())
            {
                const uint32_t id = todo.back();
                todo.pop_back();
                const auto found = index.find(id);
                if (seen[id] || found == index.end())
                    continue;
                seen[id] = true;
                total += result.all[found->second].footprint + 0x20u;
                for (const uint32_t dep : deps[found->second])
                    todo.push_back(dep);
            }
            Resource &r = result.all[k];
            r.closure = static_cast<uint32_t>(std::min<uint64_t>(total, 0xffffffffu));
            if (r.closure <= 0x20000u)
                result.small.push_back(r);
            if (r.fixups != 0u && r.footprint <= 0x5000u)
                result.withFixups.push_back(r);
            if (r.swizzles != 0u && r.closure <= 0x100000u)
                result.withSwizzles.push_back(r);
        }
        return result;
    }();
    return db;
}

// --- The logged fakes. ---

void logEntry(uint32_t what, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    static const bool trace = std::getenv("RC_RESFUZZ_TRACE") != nullptr; // each logged call on stderr
    if (trace)
        std::fprintf(stderr, "[res_fuzz] %x %x %x %x %x\n", what, a, b, c, d);
    uint32_t hash = env(eHash);
    for (const uint32_t word : {what, a, b, c, d})
        hash = (hash ^ word) * 16777619u;
    setEnv(eHash, hash);
    const uint32_t count = load<uint32_t>(kLog);
    if (count < kLogMax)
    {
        const uint32_t at = kLog + 4u + count * 20u;
        store<uint32_t>(at, what);
        store<uint32_t>(at + 4u, a);
        store<uint32_t>(at + 8u, b);
        store<uint32_t>(at + 12u, c);
        store<uint32_t>(at + 16u, d);
    }
    store<uint32_t>(kLog, count + 1u);
}

void returnWith(R5900Context *ctx, uint32_t value)
{
    SET_GPR_S32(ctx, 2, static_cast<int32_t>(value));
    ctx->pc = getRegU32(ctx, 31);
}

uint32_t arg(R5900Context *ctx, int i) { return getRegU32(ctx, 4 + i); }

// A call logged with the argument registers the function reads.
template <uint32_t Address, int Args>
void loggedCall(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const auto a = [ctx](int i) { return i < Args ? arg(ctx, i) : 0u; };
    logEntry(Address, a(0), a(1), a(2), a(3));
    returnWith(ctx, 0);
}

void fakeOpen(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t name = arg(ctx, 0);
    uint32_t text = name;
    if (load<int8_t>(text) == '_')
        ++text;
    std::string key;
    for (uint32_t i = 0; i < 255u; ++i)
    {
        uint8_t c = load<uint8_t>(text + i);
        if (c == 0)
            break;
        if (c >= 'a' && c <= 'z')
        {
            c = static_cast<uint8_t>(c - 0x20);
            store<uint8_t>(text + i, c); // the real one upper-cases the name in place
        }
        key.push_back(static_cast<char>(c));
    }
    const uint32_t call = env(eOpenCalls);
    setEnv(eOpenCalls, call + 1u);
    int32_t slot = -1;
    uint32_t hash = 2166136261u;
    for (const char c : key)
        hash = (hash ^ static_cast<uint8_t>(c)) * 16777619u;
    const auto found = iso().files.find(normalize(key));
    if (!(call < 32u && ((env(eOpenFailMask) >> call) & 1u) != 0u) && found != iso().files.end())
        for (int32_t i = 0; i < 6; ++i)
            if (load<uint32_t>(kFmFiles + i * 16u) == 0u)
            {
                slot = i;
                const uint32_t entry = kFmFiles + i * 16u;
                store<uint32_t>(entry, 1);
                store<uint32_t>(entry + 4u, 0);
                store<uint32_t>(entry + 8u, found->second.first);
                store<uint32_t>(entry + 12u, found->second.second);
                break;
            }
    logEntry(kFMFileOpen, name, hash, static_cast<uint32_t>(slot), 0);
    returnWith(ctx, static_cast<uint32_t>(slot));
}

void fakeRead(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t data = arg(ctx, 0), size = arg(ctx, 1), fd = arg(ctx, 2);
    const uint32_t entry = kFmFiles + (fd << 4);
    const uint32_t offset = load<uint32_t>(entry + 4u), sector = load<uint32_t>(entry + 8u);
    const uint32_t call = env(eReadCalls);
    setEnv(eReadCalls, call + 1u);
    const bool fail = call < 32u && ((env(eFailMask) >> call) & 1u) != 0u;
    const uint32_t kind = (env(eFailKind) >> ((call % 16u) * 2u)) & 3u;
    uint32_t answer = size, copy = size;
    if (fail)
    {
        if (kind == 0u)
            answer = copy = size >> 1;
        else if (kind == 2u)
            answer = copy = 0;
        else if (kind == 3u)
            answer = size + 1u;
    }
    copy = std::min(copy, 0x1000000u);
    std::vector<uint8_t> bytes(copy);
    readIso(iso(), static_cast<uint64_t>(sector) * 2048u + offset, bytes.data(), copy);
    for (uint32_t i = 0; i < copy; ++i)
        store<uint8_t>(data + i, bytes[i]);
    if (fail && kind == 1u && size != 0u)
    {
        const uint32_t at = data + (call * 7u) % size;
        store<uint8_t>(at, static_cast<uint8_t>(load<uint8_t>(at) ^ 0x5au));
    }
    store<uint32_t>(entry + 4u, offset + (fail ? 0u : answer)); // a failed read leaves the position, so a retry reads the same bytes
    logEntry(kFMFileRead, data, size, fd, answer);
    returnWith(ctx, answer);
}

void fakeFinished(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t calls = env(eFinishedCalls) + 1u;
    setEnv(eFinishedCalls, calls);
    logEntry(kMpegFinished, calls, 0, 0, 0);
    returnWith(ctx, calls >= env(eFinishedAfter) ? 1u : 0u);
}

void fakeSnapshot(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t calls = env(eInputCalls) + 1u;
    setEnv(eInputCalls, calls);
    for (const uint32_t key : {0x03u, 0x13u, 0x0eu, 0x1eu})
        store<uint8_t>(kInputKeys + key, static_cast<uint8_t>(load<uint8_t>(kInputKeys + key) & ~0x20u));
    if (calls == env(eHitAt))
    {
        const uint32_t key = env(eHitKey) & 0x1fu;
        store<uint8_t>(kInputKeys + key, static_cast<uint8_t>(load<uint8_t>(kInputKeys + key) | 0x20u));
    }
    logEntry(kInputSnapshot, calls, 0, 0, 0);
    returnWith(ctx, 0);
}

void fakeAllocIop(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    logEntry(kSifAllocIopHeap, arg(ctx, 0), 0, 0, 0);
    returnWith(ctx, env(eIopHeap));
}

void fakeAddIntc(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    logEntry(kAddIntcHandler2, arg(ctx, 0), arg(ctx, 1), arg(ctx, 2), arg(ctx, 3));
    returnWith(ctx, env(eIntcId));
}

// sceSdRemote's RPC to the IOP: the command and the words it sends. sceSdRemote copies six argument registers
// ($a2-$t3) after a header word whatever the command; only those the command uses are logged ($a2, $a3, and
// $t0-$t2 for the transfer 0x80d0): the rest are stale registers, which differ between any two callers. (The fakes
// replace only functions the originals reach through the dispatcher: a tail jump in recompiled code calls its target
// directly, so sceSdRemote, EnableIntc, DisableIntc and VSyncCallback stay real, and the RPC under sceSdRemote is
// logged instead.)
void fakeCallRpc(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t command = arg(ctx, 1), send = arg(ctx, 3);
    const uint32_t words = command == 0x80d0u ? 6u : 3u;
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < words; ++i)
        hash = (hash ^ load<uint32_t>(send + i * 4u)) * 16777619u;
    logEntry(kSifCallRpc, command, arg(ctx, 2), arg(ctx, 4), hash);
    returnWith(ctx, 0);
}

bool loggedStore(uint32_t address, uint32_t value)
{
    logEntry(kHardwareStore, address, value, 0, 0);
    return true; // dropped
}

// Offline: installs the fakes (once) and resets their control words; reads are good, the pad untouched, the movie
// finishes at once. Without fakeIopFree, sceSifFreeIopHeap stays real (BBTransferSRAM reaches it by a tail jump, which
// recompiled code makes past any replacement).
void quietLoader(bool fakeIopFree = true)
{
    if (g_offline)
    {
        static bool installed = false;
        if (!installed)
        {
            installed = true;
            PS2Runtime &rt = *g_runtime;
            rt.replaceFunction(kFMFileOpen, &fakeOpen);
            rt.replaceFunction(kFMFileRead, &fakeRead);
            rt.replaceFunction(kMpegFinished, &fakeFinished);
            rt.replaceFunction(kInputSnapshot, &fakeSnapshot);
            rt.replaceFunction(kSifAllocIopHeap, &fakeAllocIop);
            rt.replaceFunction(kAddIntcHandler2, &fakeAddIntc);
            rt.replaceFunction(kSifCallRpc, &fakeCallRpc);
            rt.replaceFunction(kLoadErrorCallback, &loggedCall<kLoadErrorCallback, 0>);
            rt.replaceFunction(kDisplayInitHiRes, &loggedCall<kDisplayInitHiRes, 0>);
            rt.replaceFunction(kSwapBuffers, &loggedCall<kSwapBuffers, 0>);
            rt.replaceFunction(kSyncV, &loggedCall<kSyncV, 1>);
            rt.replaceFunction(kFlushCache, &loggedCall<kFlushCache, 1>);
            rt.replaceFunction(kSyncPath, &loggedCall<kSyncPath, 2>);
            rt.replaceFunction(kExecLoadImage, &loggedCall<kExecLoadImage, 2>);
            if (fakeIopFree)
                rt.replaceFunction(kSifFreeIopHeap, &loggedCall<kSifFreeIopHeap, 1>);
            rt.replaceFunction(kSdTransToIOP, &loggedCall<kSdTransToIOP, 4>);
            rt.replaceFunction(kMpegLoad, &loggedCall<kMpegLoad, 4>);
            rt.replaceFunction(kMpegPlay, &loggedCall<kMpegPlay, 2>);
            rt.replaceFunction(kMpegTerm, &loggedCall<kMpegTerm, 1>);
            rt.replaceFunction(kRCMusicStop, &loggedCall<kRCMusicStop, 0>);
            rt.replaceFunction(kRemoveIntcHandler, &loggedCall<kRemoveIntcHandler, 2>);
            g_ps2HardwareStoreHook = &loggedStore;
        }
    }
    for (uint32_t offset = 0; offset < 0x40u; offset += 4u)
        setEnv(offset, 0);
    setEnv(eFinishedAfter, 1);
    setEnv(eHitAt, 0xffffffffu);
    store<uint32_t>(kLog, 0);
}

// Some early reads fail, so the retries and the error function run. A corrupt read with the full count (kind 1) is
// only given where the data is checksummed (the resource loads): elsewhere the game would use the bad data, as a
// table size read wrong that sends the next read over the stack.
void someFailures(uint64_t &state, uint32_t percent, bool corrupt = false)
{
    if (!chance(state, percent))
        return;
    uint32_t mask = nextFuzz(state) & nextFuzz(state);
    if (chance(state, 50))
        mask &= 0xffu;
    if (chance(state, 20))
        mask = 0x3fu << rnd(state, 4); // six in a row: the error function
    setEnv(eFailMask, mask);
    uint32_t kinds = nextFuzz(state);
    if (!corrupt)
        for (uint32_t k = 0; k < 16u; ++k)
            if (((kinds >> (2u * k)) & 3u) == 1u)
                kinds ^= 3u << (2u * k); // corrupt -> long
    setEnv(eFailKind, kinds);
}

void errorFunction(uint64_t &state)
{
    G(resVarG).pfLoadErrorFunction.address = chance(state, 80) ? kLoadErrorCallback : 0u;
}

// The timer 0 counter, stopped (mode 0) at a value, so both runs read the same count.
void stopCounter(uint64_t &state)
{
    if (!g_offline)
        return;
    g_runtime->memory().write32(0x10000010u, 0);
    static const uint32_t kCounts[] = {0u, 1u, 0x3d85u, 0x3d86u, 0xffffu};
    g_runtime->memory().write32(0x10000000u, chance(state, 30) ? kCounts[rnd(state, 5)] : rnd(state, 0x10000));
}

// File table slots: those named stay, the others are freed or kept at random.
void shuffleSlots(uint64_t &state, std::initializer_list<int32_t> keep)
{
    for (int32_t i = 0; i < 6; ++i)
    {
        if (std::find(keep.begin(), keep.end(), i) != keep.end())
            continue;
        if (chance(state, 40))
            store<uint32_t>(kFmFiles + i * 16u, 0);
        else if (chance(state, 20))
            store<uint32_t>(kFmFiles + i * 16u, 1);
    }
}

// A file in a slot, as FMFileOpen leaves it.
void openSlot(int32_t slot, const char *name, uint32_t offset)
{
    const auto [sector, size] = isoFile(name);
    const uint32_t entry = kFmFiles + static_cast<uint32_t>(slot) * 16u;
    store<uint32_t>(entry, 1);
    store<uint32_t>(entry + 4u, offset);
    store<uint32_t>(entry + 8u, sector);
    store<uint32_t>(entry + 12u, size);
}

// --- Room on the heap. ---

uint32_t largestFree()
{
    const uint32_t header = G(memVarG).pHeader.address;
    uint32_t largest = 0;
    for (uint32_t block = load<uint32_t>(header + 0xcu), n = 0; block != header && n < 100000; block = load<uint32_t>(block + 0xcu), ++n)
        if (load<uint32_t>(block) == MEM_BLOCK_TYPE_FREE)
            largest = std::max(largest, load<uint32_t>(block + 4u));
    return largest;
}

// Frees the level's largest blocks until a free block of this size exists (some dumps' heaps have little room left,
// and a failed allocation sends these functions through null pointers, which recompiled code stores to as hardware
// registers past the end of RAM and native code wraps into RAM). Whole RAM is restored after each input.
void makeRoom(uint32_t bytes, uint32_t spare)
{
    for (int tries = 0; tries < 64 && largestFree() < bytes; ++tries)
    {
        const uint32_t header = G(memVarG).pHeader.address;
        uint32_t biggest = 0, biggestSize = 0;
        for (uint32_t block = load<uint32_t>(header + 0xcu), n = 0; block != header && n < 100000; block = load<uint32_t>(block + 0xcu), ++n)
            if (load<uint32_t>(block) == MEM_BLOCK_TYPE_USED && load<uint32_t>(block + 4u) > biggestSize)
            {
                biggest = block;
                biggestSize = load<uint32_t>(block + 4u);
            }
        if (biggest == 0u)
            return;
        store<uint32_t>(spare, biggest + 0x40u);
        MemBlockFree(ptr32<memHandle>(spare));
    }
}

// --- The loader's state for loads. ---

const Resource &pick(uint64_t &state, const std::vector<Resource> &from) { return from[rnd(state, static_cast<uint32_t>(from.size()))]; }

// A handle table built as ResLoadTemporaryHandles builds it (some ids already loaded), a daemon table with each type's
// options and a logged daemon (or none), the CAT open in its slot. Returns false without a disc.
bool loadEnvironment(uint64_t &state, uint32_t scratch)
{
    quietLoader();
    const Database &db = database();
    if (db.small.empty())
        return false;
    resClass &r = G(resVarG);
    errorFunction(state);
    someFailures(state, 30, true);
    r.lookupHeader.maxId = db.maxId;
    if (r.handlesHandle.address == 0u)
        MemBlockAllocLo(ptr32<memHandle>(kRes + 0x3a4u), db.maxId * 4u);
    const uint32_t handles = r.handlesHandle.address;
    for (uint32_t i = 0; i < db.maxId; ++i)
    {
        uint32_t value = (db.flags[i] & 0x400u) != 0u ? db.offsets[i] | 3u : db.flags[i] | 2u;
        if ((value & 3u) == 3u && chance(state, 3))
            value = scratch + 0x10u * rnd(state, 0x100); // loaded already
        store<uint32_t>(handles + i * 4u, value);
    }
    store<uint32_t>(handles, 0);
    const uint32_t daemons = scratch + 0x6000u;
    r.pConstructDaemonsList = ptr32<resConstruct>(daemons);
    static const uint16_t kOptions[] = {0, 1, 2, 4, 6, 6, 4, 2};
    for (uint32_t type = 0; type < 0x60u; ++type)
    {
        const uint32_t entry = daemons + type * 8u;
        store<uint32_t>(entry, chance(state, 60) ? kLoadErrorCallback : 0u); // a logged daemon
        store<uint16_t>(entry + 4u, chance(state, 90) ? kOptions[rnd(state, 8)] : static_cast<uint16_t>(nextFuzz(state)));
        store<uint16_t>(entry + 6u, static_cast<uint16_t>(nextFuzz(state)));
    }
    if (r.catHandle < 0 || r.catHandle > 5)
        r.catHandle = 2;
    openSlot(r.catHandle, "CAT/RDS.CAT", rnd(state, 0x100000));
    // The load area: in the game, the area ResInit was given was freed back to the heap afterwards and the heap
    // reuses it, so a load (which never checks pTop) must get a block of its own here, or it overwrites the heap.
    const uint32_t area = scratch + 0x7900u;
    makeRoom(kLoadArea + 0x100000u, scratch + 0x7904u);
    store<uint32_t>(area, 0);
    MemBlockAllocLo(ptr32<memHandle>(area), kLoadArea);
    r.pTop = ptr32<uint8>(load<uint32_t>(area));
    r.pBot = r.pCurr = ptr32<uint8>(load<uint32_t>(area) + kLoadArea);
    r.pPrevCurr = r.pCurr;
    r.loadFlag = 0;
    r.loadFlagStore = static_cast<int32>(nextFuzz(state));
    return true;
}

uint32_t pickId(uint64_t &state)
{
    const Database &db = database();
    if (chance(state, 5))
    {
        // An id already loaded (or made so).
        const uint32_t id = pick(state, db.small).id;
        store<uint32_t>(G(resVarG).handlesHandle.address + id * 4u, kFuzzScratch + 0x10u * rnd(state, 0x100));
        return id;
    }
    return pick(state, db.small).id;
}

// A resource's bytes after its header (tables, 8 bytes, data), as ResLoadImage reads them, and its header in resVarG.
void readResource(const Resource &resource, uint32_t at)
{
    const auto [catSector, catSize] = isoFile("CAT/RDS.CAT");
    const uint64_t start = static_cast<uint64_t>(catSector) * 2048u + resource.offset;
    std::vector<uint8_t> bytes(0x1cu + resource.footprint);
    readIso(iso(), start, bytes.data(), bytes.size());
    std::memcpy(guest_host(kRes + 0xa8u), bytes.data(), 0x1cu);
    for (uint32_t i = 0; i < resource.footprint; ++i)
        store<uint8_t>(at + i, bytes[0x1cu + i]);
}

} // namespace

bool resLoadEnvironment(uint64_t &state, uint32_t scratch) { return loadEnvironment(state, scratch); }
uint32_t resPickId(uint64_t &state) { return pickId(state); }

// --- RES ---

void resInit(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    quietLoader();
    errorFunction(state);
    someFailures(state, 25);
    shuffleSlots(state, {});
    static const char *const kNames[] = {"\\RC2", "\\RC2", "\\RC2", "\\rc2", "\\RC3", "\\CAT\\RC2", "\\\\RC2"};
    putString(scratch, kNames[rnd(state, 7)]);
    if (chance(state, 10))
        setEnv(eOpenFailMask, 1u << rnd(state, 3));
    setArg(ctx, 0, scratch);
    setArg(ctx, 1, chance(state, 50) ? G(resVarG).pTop.address : nextFuzz(state) & 0x1fffff0u);
    setArg(ctx, 2, chance(state, 50) ? G(resVarG).size : nextFuzz(state) >> rnd(state, 16));
}

void resHandles(R5900Context *, uint64_t &state, uint32_t)
{
    resClass &r = G(resVarG);
    r.lookupHeader.maxId = chance(state, 50) ? r.lookupHeader.maxId : rnd(state, 0x2000);
    r.pBot = ptr32<uint8>(chance(state, 70) ? r.pBot.address : nextFuzz(state));
    r.handlesHandle = nullptr;
    if (chance(state, 60))
        MemBlockAllocLo(ptr32<memHandle>(kRes + 0x3a4u), 4u + rnd(state, 0x4000));
}

void resTemporaryHandles(R5900Context *, uint64_t &state, uint32_t)
{
    quietLoader();
    errorFunction(state);
    someFailures(state, 30);
    resClass &r = G(resVarG);
    const Database &db = database();
    r.lookupHeader.maxId = chance(state, 30) ? db.maxId : 1u + rnd(state, chance(state, 50) ? 0x400u : db.maxId);
    r.handlesHandle = nullptr;
    MemBlockAllocLo(ptr32<memHandle>(kRes + 0x3a4u), r.lookupHeader.maxId * 4u);
    if (r.lufHandle < 0 || r.lufHandle > 5)
        r.lufHandle = 0;
    openSlot(r.lufHandle, "RC2.LUF", rnd(state, 0x100));
    r.pConcatOffsets = nullptr;
    if (chance(state, 25) && !db.offsets.empty())
    {
        MemBlockAllocLo(ptr32<memHandle>(kRes + 0x90u), (db.maxId + 1u) * 4u);
        for (uint32_t i = 0; i < db.maxId; ++i)
            store<uint32_t>(r.pConcatOffsets.address + i * 4u, db.offsets[i]);
    }
    // The CAT must open (a free slot, the right path): reading from descriptor -1 gives a garbage count of offsets.
    putString(kRes + 0x308u, "");
    if (chance(state, 30))
    {
        r.catHandle = -1;
        shuffleSlots(state, {r.lufHandle});
        store<uint32_t>(kFmFiles + (r.lufHandle == 5 ? 4u : 5u) * 16u, 0);
    }
    else
    {
        if (r.catHandle < 0 || r.catHandle > 5)
            r.catHandle = 2;
        openSlot(r.catHandle, "CAT/RDS.CAT", rnd(state, 0x1000));
    }
}

void resTerm(R5900Context *, uint64_t &state, uint32_t)
{
    resClass &r = G(resVarG);
    for (const uint32_t field : {0x39cu, 0x398u, 0x3a0u})
    {
        store<uint32_t>(kRes + field, 0);
        MemBlockAllocLo(ptr32<memHandle>(kRes + field), 4u + rnd(state, 0x800));
    }
    r.handlesHandle = nullptr;
    if (chance(state, 60))
        MemBlockAllocLo(ptr32<memHandle>(kRes + 0x3a4u), 4u + rnd(state, 0x800));
    static const int32_t kFiles[] = {-1, 0, 1, 2, 3, 5};
    r.lufHandle = kFiles[rnd(state, 6)];
    r.tlfHandle = kFiles[rnd(state, 6)];
    r.catHandle = kFiles[rnd(state, 6)];
    r.initFlag = static_cast<int32>(nextFuzz(state));
}

void resTypeList(R5900Context *, uint64_t &state, uint32_t)
{
    quietLoader();
    errorFunction(state);
    someFailures(state, 35);
    resClass &r = G(resVarG);
    if (r.tlfHandle < 0 || r.tlfHandle > 5)
        r.tlfHandle = 1;
    openSlot(r.tlfHandle, "RC2.TLF", rnd(state, 0x400));
    if (chance(state, 10))
        store<uint32_t>(kFmFiles + static_cast<uint32_t>(r.tlfHandle) * 16u + 12u, 992u - 12u * rnd(state, 8));
    r.pTypeList = nullptr;
    r.pTypePtrs = nullptr;
    r.pConstructDaemonsList = nullptr;
}

void resOpen(R5900Context *, uint64_t &state, uint32_t scratch)
{
    quietLoader();
    shuffleSlots(state, {});
    resClass &r = G(resVarG);
    static const char *const kPaths[] = {"", "", "", "\\CAT", "\\X"};
    static const char *const kNames[] = {"rc2", "rc2", "RC2", "rc3", "rds"};
    putString(scratch, kPaths[rnd(state, 5)]);
    putString(scratch + 0x40u, kNames[rnd(state, 5)]);
    r.pPath = ptr32<char>(scratch);
    r.pName = ptr32<char>(scratch + 0x40u);
    if (chance(state, 10))
        setEnv(eOpenFailMask, 1u << rnd(state, 2));
}

void resLoad(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!loadEnvironment(state, scratch))
        return;
    setArg(ctx, 0, pickId(state));
}

void resLoadBlock(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!loadEnvironment(state, scratch))
        return;
    setArg(ctx, 0, pickId(state));
    setArg(ctx, 1, scratch + 0x40u + 4u * rnd(state, 16));
}

void resLoadImage(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!loadEnvironment(state, scratch))
        return;
    resClass &r = G(resVarG);
    if (chance(state, 30))
    {
        // Six or more failures in a row: the error function, then the count starts again.
        setEnv(eFailMask, (chance(state, 50) ? 0x3fu : 0xfffu) << rnd(state, 3));
        setEnv(eFailKind, nextFuzz(state));
    }
    r.loadFlag = chance(state, 30) ? 1 : chance(state, 90) ? 0 : 2;
    // A handle not in the CAT reads the lookup file (descriptor 0) as a resource and retries for ever.
    const uint32_t id = chance(state, 90) ? pick(state, database().small).id : pick(state, database().withSwizzles).id;
    store<uint32_t>(r.handlesHandle.address + id * 4u, database().offsets[id] | 3u);
    setArg(ctx, 0, id);
    setArg(ctx, 1, r.pCurr.address - (chance(state, 50) ? 0u : rnd(state, 0x10000)));
}

void resFreeBlock(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t handle = scratch + 4u * rnd(state, 64);
    store<uint32_t>(handle, 0);
    MemBlockAllocLo(ptr32<memHandle>(handle), 4u + rnd(state, 0x2000));
    setArg(ctx, 0, handle);
}

void resCrc(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    static const uint32_t kCrcs[] = {0u, 0xffffffffu, 1u, 0x80000000u};
    setArg(ctx, 0, chance(state, 30) ? kCrcs[rnd(state, 4)] : nextFuzz(state));
    const uint32_t kind = rnd(state, 10);
    if (kind == 0)
    {
        setArg(ctx, 1, 0);
        setArg(ctx, 2, nextFuzz(state) >> rnd(state, 32));
        return;
    }
    const uint32_t buffer = kind == 1 ? 0x100000u + rnd(state, 0x3c0000) : scratch + rnd(state, 0x100);
    setArg(ctx, 1, buffer);
    static const uint32_t kLengths[] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33};
    setArg(ctx, 2, chance(state, 40) ? kLengths[rnd(state, 15)] : rnd(state, kind == 1 ? 0x10000u : 0x7800u));
}

void resChecksum(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    setArg(ctx, 0, chance(state, 30) ? 0u : nextFuzz(state));
    setArg(ctx, 1, chance(state, 10) ? 0x100000u + 4u * rnd(state, 0xf0000) : scratch + 4u * rnd(state, 0x40));
    static const uint32_t kLengths[] = {0, 1, 3, 4, 5, 8, 12, 16, 20, 0x14, 0x18, 0x1c};
    setArg(ctx, 2, chance(state, 50) ? kLengths[rnd(state, 12)] : rnd(state, 0x7800));
}

void resArea(R5900Context *ctx, uint64_t &state, uint32_t)
{
    resClass &r = G(resVarG);
    const uint32_t current = chance(state, 70) ? r.pCurr.address : nextFuzz(state);
    r.pCurr = ptr32<uint8>(current);
    if (chance(state, 30))
        r.pTop = ptr32<uint8>(current - rnd(state, 0x1000));
    r.pPrevCurr = ptr32<uint8>(nextFuzz(state));
    r.ppBlockListSave = ptr32<ptr32<resMemBinaryHeader>>(nextFuzz(state));
    r.ppBlockList = ptr32<ptr32<resMemBinaryHeader>>(nextFuzz(state));
    r.binaryHeader.fixups = static_cast<uint16>(chance(state, 50) ? rnd(state, 64) : nextFuzz(state));
    r.binaryHeader.swizzles = static_cast<uint16>(chance(state, 50) ? rnd(state, 64) : nextFuzz(state));
    const uint32_t room = current - r.pTop.address;
    setArg(ctx, 0, chance(state, 40) ? room + rnd(state, 3) - 1u : nextFuzz(state) >> rnd(state, 32));
}

void resGetHandle(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    G(resVarG).handlesHandle = ptr32<uint32>(scratch + 4u * rnd(state, 0x100));
    setArg(ctx, 0, rnd(state, 0x1000));
}

void resClose(R5900Context *, uint64_t &state, uint32_t)
{
    resClass &r = G(resVarG);
    static const int32_t kFiles[] = {-2, -1, 0, 1, 2, 3, 4, 5, 7};
    r.lufHandle = kFiles[rnd(state, 9)];
    r.tlfHandle = kFiles[rnd(state, 9)];
    r.catHandle = kFiles[rnd(state, 9)];
}

void resNames(R5900Context *, uint64_t &state, uint32_t scratch)
{
    std::string name;
    for (uint32_t i = 0, n = rnd(state, 10); i < n; ++i)
        name.push_back(static_cast<char>('a' + rnd(state, 26)));
    putString(scratch, name);
    G(resVarG).pName = ptr32<char>(scratch);
    // Set every time: a setup's changes to the globals outlive the input (the test reverts only the function's).
    static const uint32_t kExtensions[] = {(sym::sec_sdata + 0x7b8u), (sym::sec_sdata + 0x7b0u), (sym::sec_sdata + 0x7a8u)}; // ".luf", ".tlf", ".cfg"
    for (uint32_t i = 0; i < 3u; ++i)
        G(resExtG)[i] = ptr32<char>(kExtensions[i]);
    if (chance(state, 30))
        for (uint32_t i = 0; i < 3u; ++i)
        {
            std::string ext = ".";
            for (uint32_t k = 0, n = rnd(state, 4); k < n; ++k)
                ext.push_back(static_cast<char>('A' + rnd(state, 26)));
            putString(scratch + 0x40u + i * 0x10u, ext);
            G(resExtG)[i] = ptr32<char>(scratch + 0x40u + i * 0x10u);
        }
}

void resCompType(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    static const char *const kTypes[] = {"TEX", "ATX", "SPR", "TEX", "TEXT", "TE", "", "STB", "RSO", "RSA", "\x80X"};
    const auto type = [&](uint32_t at)
    {
        putString(at, chance(state, 85) ? kTypes[rnd(state, 11)] : std::string(1, static_cast<char>(1 + rnd(state, 255))));
        return at;
    };
    store<uint32_t>(scratch, type(scratch + 0x20u));
    store<uint32_t>(scratch + 4u, chance(state, 15) ? load<uint32_t>(scratch) : type(scratch + 0x40u));
    setArg(ctx, 0, scratch);
    setArg(ctx, 1, scratch + 4u);
}

void resSTB(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!loadEnvironment(state, scratch))
        return;
    const uint32_t list = scratch + 0x200u, count = rnd(state, 12);
    for (uint32_t i = 0; i < count; ++i)
        store<uint32_t>(list + i * 4u, pickId(state)); // small: each loads at most 128 KB with what it pulls in
    G(resVarG).binaryHeader.binaryLength = static_cast<int32>(count * 4u + (chance(state, 20) ? rnd(state, 4) : 0u));
    store<uint32_t>(scratch + 0x40u, list);
    setArg(ctx, 0, scratch + 0x40u);

}

void resInitLoading(R5900Context *, uint64_t &state, uint32_t)
{
    quietLoader();
    errorFunction(state);
    someFailures(state, 40);
    resClass &r = G(resVarG);
    if (r.lufHandle < 0 || r.lufHandle > 5)
        r.lufHandle = 0;
    openSlot(r.lufHandle, "RC2.LUF", chance(state, 50) ? 0u : rnd(state, 0x100));
}

void resRelocate(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    resClass &r = G(resVarG);
    const uint32_t block = scratch + 0x400u;
    if (!database().withFixups.empty() && chance(state, 70))
    {
        readResource(pick(state, database().withFixups), block);
    }
    else
    {
        // Made up: fix-ups pointing anywhere in the block, the tables included, some twice.
        const uint32_t fixups = rnd(state, 40), swizzles = rnd(state, 8), length = 4u * rnd(state, 0x400);
        r.binaryHeader.fixups = static_cast<uint16>(fixups);
        r.binaryHeader.swizzles = static_cast<uint16>(swizzles);
        const int32_t base = static_cast<int32_t>((fixups + swizzles) * 4u + 8u);
        for (uint32_t i = 0; i < fixups; ++i)
        {
            const int32_t target = chance(state, 15) ? -base + 4 * static_cast<int32_t>(rnd(state, fixups + 1u))
                                                      : 4 * static_cast<int32_t>(rnd(state, length / 4u + 1u));
            store<int32_t>(block + i * 4u, target);
        }
    }
    setArg(ctx, 0, block);
    setArg(ctx, 1, nextFuzz(state));
}

void resAutoLoad(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!loadEnvironment(state, scratch) || database().withSwizzles.empty())
        return;
    const Resource &resource = pick(state, database().withSwizzles);
    const uint32_t at = (G(resVarG).pCurr.address - resource.footprint - 0x100u) & ~0xfu;
    readResource(resource, at);
    G(resVarG).pCurr = ptr32<uint8>(at); // as ResLoad leaves it: later loads go below the resource
    setArg(ctx, 0, at);
    setArg(ctx, 1, resource.id);
}

// --- LOADING ---

void loadingInit(R5900Context *ctx, uint64_t &state, uint32_t)
{
    quietLoader();
    G(bLoadingG) = chance(state, 70) ? 0 : chance(state, 50) ? 1 : static_cast<int32_t>(nextFuzz(state));
    store<uint32_t>((sym::mpegBuffer + 0x4u), chance(state, 70) ? 0u : nextFuzz(state));
    putString(sym::fileUsed, chance(state, 40) ? "" : chance(state, 50) ? "sbot1.m2v" : "abc");
    store<uint8_t>((sym::mainVarG + 0x34au), static_cast<uint8_t>(nextFuzz(state)));
    store<uint8_t>((sym::mainVarG + 0x369u), chance(state, 80) ? static_cast<uint8_t>(rnd(state, 3)) : static_cast<uint8_t>(nextFuzz(state)));
    static const int32_t kTypes[] = {-1, 0, 1, 2, 3, 4, 5, 6};
    setArg(ctx, 0, chance(state, 90) ? static_cast<uint32_t>(kTypes[rnd(state, 8)]) : nextFuzz(state));
}

void loadingFlags(R5900Context *, uint64_t &state, uint32_t)
{
    quietLoader();
    G(bLoadingG) = static_cast<int32_t>(chance(state, 50) ? rnd(state, 2) : nextFuzz(state));
    static const uint32_t kFrames[] = {0u, 1u, 0xffffffffu, 0x7fffffffu, 0x80000000u};
    store<uint32_t>((sym::frontEndVarG + 0x2a4u), chance(state, 60) ? kFrames[rnd(state, 5)] : nextFuzz(state));
}

// --- SYSTIMER and TIMER ---

namespace {

// The clock near its edges: tickTime near the carry at 0x200000, lastTicks a period or less.
void randomClock(uint64_t &state)
{
    sysTimerClass &t = G(sysTimerVarG);
    static const uint32_t kTicks[] = {0u, 0x1fc27au, 0x1fffffu, 0x200000u, 0x200001u, 0xffffffffu};
    t.tickTime = chance(state, 40) ? kTicks[rnd(state, 6)] : nextFuzz(state) >> rnd(state, 12);
    t.tickTimeCarry = chance(state, 50) ? rnd(state, 4) : nextFuzz(state);
    t.lastTicks = chance(state, 50) ? 0x3d86 : static_cast<int32>(chance(state, 80) ? rnd(state, 0x10000) : nextFuzz(state));
    t.eventHandle = static_cast<int32_t>(nextFuzz(state));
    t.bInitialised = nextFuzz(state);
    stopCounter(state);
}

// A list of timers in scratch, due around now; the callbacks are logged, or ResReAlloc (which stores its argument).
uint32_t timerList(uint64_t &state, uint32_t scratch, uint32_t count)
{
    uint32_t head = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t timer = scratch + 0x100u + i * 0x10u;
        store<uint32_t>(timer, head);
        store<int32_t>(timer + 4u, static_cast<int32_t>(1u + rnd(state, 0x3d86)));
        store<int32_t>(timer + 8u, G(sysTimerVarG).lastTicks + static_cast<int32_t>(rnd(state, 0x200)) - 0x100);
        store<uint32_t>(timer + 0xcu, chance(state, 50) ? kLoadErrorCallback : kResReAlloc);
        head = timer;
    }
    return head;
}

} // namespace

void sysTimer(R5900Context *, uint64_t &state, uint32_t scratch)
{
    quietLoader();
    randomClock(state);
    G(sysTimerVarG).pSysTimerList = ptr32<_sSysTimer>(chance(state, 50) ? 0u : timerList(state, scratch, rnd(state, 4)));
    setEnv(eIntcId, nextFuzz(state));
}

void sysTimerAdd(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    quietLoader();
    randomClock(state);
    G(sysTimerVarG).pSysTimerList = ptr32<_sSysTimer>(timerList(state, scratch, rnd(state, 5)));
    setArg(ctx, 0, scratch + 0x80u);
    setArg(ctx, 1, chance(state, 50) ? kLoadErrorCallback : kResReAlloc);
    static const uint32_t kFrequencies[] = {1u, 2u, 36u, 0x3d86u, 0x3d87u, 0xffffffffu};
    setArg(ctx, 2, chance(state, 40) ? kFrequencies[rnd(state, 6)] : 1u + rnd(state, 0x8000));
}

void sysTimerRemove(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    quietLoader();
    randomClock(state);
    const uint32_t head = chance(state, 40) ? 0u : timerList(state, scratch, 1u + rnd(state, 4));
    G(sysTimerVarG).pSysTimerList = ptr32<_sSysTimer>(head);
    const uint32_t timer = chance(state, 10) ? 0u : head != 0u && chance(state, 50) ? head : scratch + 0x100u + 0x10u * rnd(state, 4);
    if (head == 0u && timer != 0u)
        store<uint32_t>(0, nextFuzz(state) | 1u); // else the original's loop on address 0 never ends
    setArg(ctx, 0, timer);
}

void sysTimerHandler(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    quietLoader();
    randomClock(state);
    G(sysTimerVarG).pSysTimerList = ptr32<_sSysTimer>(chance(state, 30) ? 0u : timerList(state, scratch, rnd(state, 6)));
    setArg(ctx, 0, 9);
    setArg(ctx, 1, scratch + 4u * rnd(state, 16));
    setArg(ctx, 2, nextFuzz(state));
}

void timerArgs(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    quietLoader();
    randomClock(state);
    const uint32_t timer = scratch + 4u * rnd(state, 64);
    store<uint32_t>(timer, chance(state, 80) ? rnd(state, 4) : nextFuzz(state));
    store<uint32_t>(timer + 4u, nextFuzz(state) >> rnd(state, 32));
    store<uint32_t>(timer + 8u, nextFuzz(state) >> rnd(state, 32));
    setArg(ctx, 0, timer);
    setArg(ctx, 1, nextFuzz(state) >> rnd(state, 32));
    setArg(ctx, 2, chance(state, 80) ? rnd(state, 4) : nextFuzz(state));
}

// --- BACKDROP ---

namespace {

void backdropCommon(uint64_t &state)
{
    quietLoader();
    makeRoom(0x300000u, kFuzzScratch + 0x7904u); // the screens' files, the converted image and the file cache
    errorFunction(state);
    someFailures(state, 30);
    shuffleSlots(state, {0, 1, 2});
    setEnv(eIopHeap, chance(state, 80) ? 0x100000u + 0x40u * rnd(state, 0x1000) : nextFuzz(state));
    G(dwAddr) = chance(state, 60) ? 0x5000 : static_cast<int32_t>(0x5000u + 0x10u * rnd(state, 0x8000));
    G(leftVAGAddr) = nextFuzz(state);
    G(rightVAGAddr) = nextFuzz(state);
    G(gRdr).frameNumber = nextFuzz(state);
}

} // namespace

void backdropScreens(R5900Context *, uint64_t &state, uint32_t)
{
    backdropCommon(state);
    setEnv(eFinishedAfter, 1u + rnd(state, 20));
    static const uint32_t kKeys[] = {0x03u, 0x13u, 0x0eu, 0x1eu, 0x05u};
    setEnv(eHitKey, kKeys[rnd(state, 5)]);
    // The press: in the first screen's wait, a later one, or never (each wait then runs its 300 frames).
    setEnv(eHitAt, chance(state, 20) ? 0xffffffffu : 1u + rnd(state, chance(state, 50) ? 20u : 900u));
}

void backdropBmp(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    backdropCommon(state);
    static const char *const kNames[] = {"LEG_N.BMP", "asc_n.BMP", "dby_N.BMP", "lgo_N.BMP", "_lgo_n.bmp"};
    putString(scratch, kNames[rnd(state, 5)]);
    setArg(ctx, 0, scratch);
}

void backdropVag(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    backdropCommon(state);
    putString(scratch, chance(state, 50) ? "LEFT.VAG" : "right.vag");
    setArg(ctx, 0, scratch);
}

void backdropDisplay(R5900Context *ctx, uint64_t &state, uint32_t)
{
    backdropCommon(state);
    setArg(ctx, 0, chance(state, 80) ? 0x100000u + rnd(state, 0x1700000) : nextFuzz(state) & 0x1f00000u);
    setArg(ctx, 1, chance(state, 90) ? rnd(state, 2) : nextFuzz(state));
}

void backdropPlay(R5900Context *, uint64_t &state, uint32_t)
{
    backdropCommon(state);
}

// --- BB.c ---

namespace {

// A BBK on the disc (\BB\<n / 10>\<n % 10>.BBK): its sections' sizes, whether it has the sound bank (the level and front
// end files; the track editor's (97, 100, 101) have only the GS image, the game data and the handles, and are loaded with
// flag 1), and where its game data goes.
struct BbFile
{
    uint32_t level = 0;
    bool sound = false;
    uint32_t compressed[4] = {}; // the sections' size words, in file order
    uint32_t sections = 0;
    uint32_t soundSize = 0, gsSize = 0, dataAt = 0, dataSize = 0;
};

const std::vector<BbFile> &bbFiles()
{
    static const std::vector<BbFile> files = []
    {
        std::vector<BbFile> found;
        for (uint32_t n = 0; n < 120; ++n)
        {
            const auto [sector, size] = isoFile(("BB/" + std::to_string(n / 10) + "/" + std::to_string(n % 10) + ".BBK").c_str());
            if (size < 0x14ecu)
                continue;
            std::vector<uint8_t> bytes(size);
            readIso(iso(), static_cast<uint64_t>(sector) * 2048u, bytes.data(), size);
            BbFile f;
            f.level = n;
            f.soundSize = le32(bytes.data() + 0x24u);
            f.gsSize = le32(bytes.data() + 0x11c0u);
            f.dataSize = le32(bytes.data() + 0x14d4u);
            f.dataAt = le32(bytes.data() + 0x14dcu);
            for (const uint32_t k : {4u, 3u})
            {
                uint32_t at = 0x14ecu, i = 0;
                for (; i < k && at + 4u <= size; ++i)
                {
                    f.compressed[i] = le32(bytes.data() + at);
                    at += 4u + f.compressed[i];
                }
                if (i == k && at == size)
                {
                    f.sections = k;
                    f.sound = k == 4u;
                    break;
                }
            }
            if (f.sections != 0)
                found.push_back(f);
        }
        return found;
    }();
    return files;
}

bool overlaps(uint32_t a, uint32_t aSize, uint32_t b, uint32_t bSize) { return a < b + bSize && b < a + aSize; }

// Where a file's load can go in the dump's heap (a level is loaded there, so the game's own places are in use): each
// section's buffer in the first free block it fits (MemBlockAllocLo's choice), and the resource area's top (where the
// sound bank and the GS image inflate) and the handles inside a used block, all clear of the game data's place and of
// every block header. False if the dump has no such room for this file.
struct BbPlaces
{
    uint32_t top = 0, handles = 0;
};

bool bbFits(const BbFile &f, BbPlaces *places = nullptr)
{
    const uint32_t header = G(memVarG).pHeader.address;
    uint32_t largest = 0;
    for (uint32_t i = 0; i < f.sections; ++i)
        largest = std::max(largest, f.compressed[i]);
    const uint32_t topSize = (std::max({f.soundSize, f.gsSize, f.sound ? f.compressed[0] : 0u}) + 0xffu) & ~0xffu;
    constexpr uint32_t kHandlesRoom = 0x20000u;
    bool buffered = false;
    uint32_t area = 0;
    for (uint32_t block = load<uint32_t>(header + 0xcu), n = 0; block != header && n < 100000; block = load<uint32_t>(block + 0xcu), ++n)
    {
        const uint32_t type = load<uint32_t>(block), size = load<uint32_t>(block + 4u);
        if (type == MEM_BLOCK_TYPE_FREE && !buffered && size >= largest + 0x80u)
        {
            if (overlaps(block, size, f.dataAt, f.dataSize))
                return false;
            buffered = true;
        }
        else if (type == MEM_BLOCK_TYPE_USED && area == 0 && size >= 0x80u + topSize + kHandlesRoom && !overlaps(block, size, f.dataAt, f.dataSize))
            area = (block + 0x40u + 0xffu) & ~0xffu;
    }
    if (!buffered || area == 0)
        return false;
    if (places)
    {
        places->top = area;
        places->handles = area + topSize;
    }
    return true;
}

} // namespace

// a0 a level whose file fits the dump's heap (or one with no file; the resource area's top and the handles placed by
// bbFits), a1 flags as the game loads that file: the sound
// bank's kinds (0x10, 0x20, both, raw) with or without the handles (8), or flag 1 for the editor's files (no sound
// section); the first header reads fail now and then (the retries and the error function), the generic effects loaded or
// not. (Flag 2, no GS image, is used by no file: with any file on the disc it would read the next section's size as the
// GS image's.)
void bbLoad(R5900Context *ctx, uint64_t &state, uint32_t)
{
    quietLoader(false);
    std::vector<const BbFile *> fitting;
    for (const BbFile &f : bbFiles())
        if (bbFits(f))
            fitting.push_back(&f);
    uint32_t level, flag;
    if (fitting.empty() || chance(state, 4))
    {
        level = chance(state, 50) ? 49u + rnd(state, 40) : nextFuzz(state); // no such file
        flag = 0x10;
    }
    else
    {
        // Every other input the editor's small file (101, 0.8 MB): each input inflates its file twice, and the
        // oracle gives a function 20 s for all its inputs on a dump.
        const BbFile *small = nullptr;
        for (const BbFile *candidate : fitting)
            if (candidate->level == 101u)
                small = candidate;
        const BbFile &f = small && chance(state, 50) ? *small : *fitting[rnd(state, static_cast<uint32_t>(fitting.size()))];
        level = f.level;
        BbPlaces places;
        bbFits(f, &places);
        G(resVarG).pTop.address = places.top;
        G(resVarG).handlesHandle.address = places.handles;
        static const uint32_t kSound[] = {0x10, 0x20, 0x30, 0, 0x18, 0x28, 0x38, 8};
        flag = f.sound ? kSound[rnd(state, 8)] : (chance(state, 70) ? 1u : 9u);
        if (chance(state, 20))
            flag |= nextFuzz(state) & 0xc0u; // bits nothing reads
    }
    if (chance(state, 30))
    {
        // Up to seven header reads fail in a row (short, empty or long; never corrupt with the full count, which the
        // game would take): six or more run the error function.
        setEnv(eFailMask, (1u << (1u + rnd(state, 7))) - 1u);
        uint32_t kinds = nextFuzz(state);
        for (uint32_t k = 0; k < 16u; ++k)
            if (((kinds >> (2u * k)) & 3u) == 1u)
                kinds ^= 3u << (2u * k);
        setEnv(eFailKind, kinds);
    }
    if (chance(state, 5))
        setEnv(eOpenFailMask, 1);
    errorFunction(state);
    G(genericLoaded) = static_cast<int32>(rnd(state, 2));
    setEnv(eIopHeap, chance(state, 85) ? 0xa0000u + 0x40u * rnd(state, 0x800) : 0u);
    shuffleSlots(state, {});
    setArg(ctx, 0, level);
    setArg(ctx, 1, flag);
}

// a0 the generic-then-level kind or not, a1 a size, a2 a sound RAM address, a3 a header in scratch, t0 the bank.
void bbTransfer(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    quietLoader(false);
    const uint32_t header = scratch + 0x100u;
    for (uint32_t i = 0; i < 0x40u; i += 4)
        store<uint32_t>(header + i, nextFuzz(state));
    const uint32_t start = 0x5010u + 0x10u * rnd(state, 0x100);
    const uint32_t end = start + 0x19000u + 0x10u * rnd(state, 0x4000);
    store<uint32_t>(header + 0x24u, chance(state, 85) ? (end - start) + 0x10u * rnd(state, 0x8000) : nextFuzz(state)); // dataSize
    store<uint32_t>(header + 0x28u, chance(state, 85) ? start : nextFuzz(state));                                     // startOfGenericAddr
    store<uint32_t>(header + 0x2cu, chance(state, 85) ? end : nextFuzz(state));                                       // endOfGenericAddr
    setEnv(eIopHeap, chance(state, 85) ? 0xa0000u + 0x40u * rnd(state, 0x800) : 0u);
    setArg(ctx, 0, chance(state, 50) ? 0u : chance(state, 80) ? 1u : nextFuzz(state));
    setArg(ctx, 1, chance(state, 80) ? 0x10u * rnd(state, 0x20000) : nextFuzz(state));
    setArg(ctx, 2, chance(state, 80) ? 0x10u * rnd(state, 0x20000) : nextFuzz(state));
    setArg(ctx, 3, header);
    setArg(ctx, 4, scratch + 0x1000u + 0x10u * rnd(state, 0x100));
}

} // namespace fuzz
} // namespace rc

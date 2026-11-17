// Decompiled from FM.c and PCFILE.c.
//
// FM.c is the game's disc file layer. A file is a slot of fmVarG.files (six: in use, read position, first sector,
// length); FMFileOpen looks a name up on the disc with sceCdSearchFile (upper-cased in place, a leading '_' dropped,
// "\\" put in front unless it starts with one, ";1" after) and keeps the last ten names in a directory cache. The
// cache never hits: it stores the name without "\\" and ";1" and the start sector -1, and compares it with the
// decorated name. FMFileRead reads through a sector cache of six entries over one buffer (FMPrepare allocates it):
// cache mode 0 uses one entry of six times the chunk, mode 1 six entries of one chunk each. A read goes chunk by chunk
// (chunks aligned to multiples of the chunk size, so they cover sectors outside the file); a missing chunk replaces
// the entry with the oldest time stamp and is fetched with sceCdRead, retried after a vertical blank until accepted,
// then waited for with sceCdSync. The read position always advances by the size asked for and the size is returned,
// whatever the disc did.
//
// PCFILE.c is the debugging host file access ("host:" names through sceOpen and friends). Reads and writes go to the
// buffer directly from its first 16-byte boundary; the unaligned head and the tail go through a 16-byte buffer.
// Never called by the game: PCcreat, PClseek, PCread, PCwrite, PCclose, and the empty FMUnPrepareEx.

#include "rc_decomp.h"
#include "stream_fuzz.h"

namespace rc {

bool_ MemBlockAllocLo(ptr32<memHandle> pHandle, uint32 size); // mem.cpp
bool_ MemBlockAllocHi(ptr32<memHandle> pHandle, uint32 size);
void MemBlockFree(ptr32<memHandle> pHandle);
void MemBlockCopy32(memHandle dstHandle, memHandle srcHandle, uint32 size);

void FMSetCacheMode(eFMCacheMode mode);
void FMInvalidateCache();
int32_t FMFileFetchSectors(uint32 sectorNum, ptr32<uint8> pBuffer, uint32 count);

namespace {

constexpr uint32_t kFm = 0x29d8c8u; // fmVarG
constexpr uint32_t kDirCache = kFm + 0x60u, kCache = kFm + 0x178u;
constexpr uint32_t kStrcat = 0x1b8af0u, kStrcmp = 0x1b8c1cu, kStrcpy = 0x1b8d60u, kMemcpy = 0x1b7c60u;
constexpr uint32_t kCdDiskReady = 0x1cd708u, kCdSearchFile = 0x1cc110u, kCdSeek = 0x1ccea8u, kCdRead = 0x1cc920u,
                   kCdSync = 0x1cd1d8u, kGsSyncV = 0x1c3d90u;
constexpr uint32_t kSceOpen = 0x1c1170u, kSceClose = 0x1c12c8u, kSceLseek = 0x1c1380u, kSceRead = 0x1c14f0u,
                   kSceWrite = 0x1c1620u;
// "\\", "" and ";1" (FM), "host:" and the two 16-byte buffers (PCFILE).
constexpr uint32_t kBackslash = 0x328300u, kEmpty = 0x328308u, kVersion = 0x328310u;
constexpr uint32_t kHost = 0x327f50u, kReadBuffer = 0x3285c0u, kWriteBuffer = 0x3285d0u;

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

sFMClass &fm() { return G(fmVarG); }
// A slot by index, as the original addresses it (fd << 4 from the table).
uint32_t fileAt(int32_t fd) { return kFm + (static_cast<uint32_t>(fd) << 4); }
uint32_t dirAt(uint32_t index) { return kDirCache + index * 0x1cu; }
uint32_t cacheAt(uint32_t index) { return kCache + (index << 4); }

uint32_t guestStrlen(uint32_t text)
{
    uint32_t length = 0;
    while (load<uint8_t>(text + length) != 0)
        ++length;
    return length;
}

} // namespace

// --- FM.c ---

// Splits a buffer into the six cache entries (one chunk each: the buffer size / 0x3000 sectors), then cache mode 0.
void FMPrepareEx(ptr32<void> pBuffer, uint32 bufferSize)
{
    sFMClass &f = fm();
    const uint32_t sectors = bufferSize / 0x3000u;
    const uint32_t bytes = sectors << 11;
    f.sectorsPerChunk = sectors;
    f.realSectorsPerChunk = sectors;
    uint32_t data = pBuffer.address;
    for (int i = 0; i < 6; ++i, data += bytes)
        f.cache[i].pData = ptr32<void>(data);
    FMSetCacheMode(static_cast<eFMCacheMode>(0));
}

// Mode 0: one cache entry of six chunks; otherwise six entries of one chunk. Empties the cache.
void FMSetCacheMode(eFMCacheMode mode)
{
    sFMClass &f = fm();
    if (mode == 0)
    {
        f.usedCacheEntries = 1;
        f.sectorsPerChunk = f.realSectorsPerChunk * 6u;
    }
    else
    {
        f.usedCacheEntries = 6;
        f.sectorsPerChunk = f.realSectorsPerChunk;
    }
    FMInvalidateCache();
}

void FMUnPrepare()
{
    MemBlockFree(ptr32<memHandle>(cacheAt(0) + 0xcu));
}

anFMFile FMFileOpen(ptr32<char> pFilename)
{
    GuestFrame frame(0xd0);
    const ptr32<char> temp = frame.local<char>(0);         // tempFilename[20]
    const uint32_t fp = frame.local<sceCdlFILE>(0x20).address; // lsn, size, name, date
    sFMClass &f = fm();
    const uint32_t name = load<int8_t>(pFilename.address) == '_' ? pFilename.address + 1u : pFilename.address;
    for (uint32_t i = 0; i < guestStrlen(name); ++i)
    {
        const uint8_t c = load<uint8_t>(name + i);
        if (static_cast<uint32_t>(c) - 0x61u < 0x1au)
            store<uint8_t>(name + i, static_cast<uint8_t>(c - 0x20u));
    }
    int32_t slot = -1;
    for (int32_t i = 0; i < 6; ++i)
        if (load<uint32_t>(fileAt(i)) == 0u)
        {
            slot = i;
            break;
        }
    if (slot == -1)
        return -1;

    if (load<int8_t>(name) == '\\')
        store<uint8_t>(temp.address, load<uint8_t>(kEmpty));
    else
        store<uint16_t>(temp.address, load<uint16_t>(kBackslash));
    guest_call(kStrcat, temp, name);
    guest_call(kStrcat, temp, kVersion);

    int32_t startSector = -1;
    uint32_t length = 0;
    for (uint32_t i = 0; i < 10u; ++i)
        if (guest_call<int32_t>(kStrcmp, dirAt(i), temp) == 0)
        {
            startSector = load<int32_t>(dirAt(i) + 0x14u);
            length = load<uint32_t>(dirAt(i) + 0x18u);
            break;
        }
    guest_call(kCdDiskReady, 0);
    if (startSector == -1)
    {
        bool found = false;
        for (uint32_t tries = 0; tries < 1000u && !found; ++tries)
            found = guest_call<int32_t>(kCdSearchFile, fp, temp) != 0;
        length = load<uint32_t>(fp + 4u);
        if (!found)
            return -1;
    }
    // The file's first sector comes from the search, also on a directory cache hit (then the stack word is stale).
    const uint32_t entry = fileAt(slot);
    store<uint32_t>(entry, 1u);
    store<uint32_t>(entry + 0xcu, length);
    store<uint32_t>(entry + 8u, load<uint32_t>(fp));
    store<uint32_t>(entry + 4u, 0u);
    guest_call(kStrcpy, dirAt(f.dirCacheIdx), name);
    store<int32_t>(dirAt(f.dirCacheIdx) + 0x14u, startSector);
    store<uint32_t>(dirAt(f.dirCacheIdx) + 0x18u, length);
    f.dirCacheIdx = (f.dirCacheIdx + 1u) % 10u;
    return slot;
}

uint32 FMFileRead(ptr32<void> pData, size_t size, anFMFile fd)
{
    GuestFrame frame(0xc0);
    sFMClass &f = fm();
    const uint32_t entry = fileAt(fd);
    const uint32_t offset = load<uint32_t>(entry + 4u), start = load<uint32_t>(entry + 8u);
    const uint32_t bytes = static_cast<uint32_t>(size);
    uint32_t spc = f.sectorsPerChunk;
    uint32_t chunk = ((offset >> 11) + start) / spc;
    const uint32_t lines = (((offset - 1u + bytes) >> 11) + start) / spc - chunk + 1u;
    uint32_t relative = offset + (start << 11) - (chunk << 11) * spc;
    uint32_t left = bytes, dest = pData.address;
    for (uint32_t i = 0; i < lines; ++i)
    {
        spc = f.sectorsPerChunk;
        const uint32_t used = f.usedCacheEntries;
        const uint32_t end = left + relative;
        uint32_t k = 0;
        for (; k < used; ++k)
            if ((load<uint32_t>(cacheAt(k)) & 1u) != 0u && load<uint32_t>(cacheAt(k) + 4u) == chunk)
                break;
        if (k == used)
        {
            // The oldest entry (the first of equal ones), used or not.
            uint32_t oldest = 0;
            k = 0;
            for (uint32_t j = 0; j < used; ++j)
                if (j == 0u || load<uint32_t>(cacheAt(j) + 8u) < oldest)
                {
                    k = j;
                    oldest = load<uint32_t>(cacheAt(j) + 8u);
                }
            FMFileFetchSectors(chunk * spc, ptr32<uint8>(load<uint32_t>(cacheAt(k) + 0xcu)), spc);
            store<uint32_t>(cacheAt(k) + 4u, chunk);
            store<uint32_t>(cacheAt(k), 1u);
            store<uint32_t>(cacheAt(k) + 8u, static_cast<uint32_t>(f.timeStamp));
            f.timeStamp = f.timeStamp + 1;
            spc = f.sectorsPerChunk;
        }
        const uint32_t chunkBytes = spc << 11;
        const uint32_t copy = chunkBytes < end ? chunkBytes - relative : left;
        MemBlockCopy32(memHandle(dest), memHandle(load<uint32_t>(cacheAt(k) + 0xcu) + relative), copy);
        const uint32_t tail = copy & 3u;
        if (tail != 0u)
            guest_call(kMemcpy, dest + (copy - tail), load<uint32_t>(cacheAt(k) + 0xcu) + relative + (copy - tail), tail);
        dest += copy;
        left -= copy;
        if (left == 0u)
            break;
        relative = 0;
        ++chunk;
    }
    store<uint32_t>(entry + 4u, load<uint32_t>(entry + 4u) + bytes);
    return bytes;
}

int32 FMInit()
{
    sFMClass &f = fm();
    for (int i = 0; i < 6; ++i)
        f.files[i].flags = 0;
    for (int i = 0; i < 10; ++i)
        f.dirCache[i].filename[0] = 0;
    f.dirCacheIdx = 0;
    f.timeStamp = 0;
    return 0;
}

void FMPrepare(uint32 cacheSize)
{
    GuestFrame frame(0x30);
    const ptr32<memHandle> handle = frame.local<memHandle>(0);
    *handle = nullptr;
    if (MemBlockAllocLo(handle, cacheSize))
        FMPrepareEx(*handle, cacheSize);
}

void FMPrepareHi(uint32 cacheSize)
{
    GuestFrame frame(0x30);
    const ptr32<memHandle> handle = frame.local<memHandle>(0);
    *handle = nullptr;
    if (MemBlockAllocHi(handle, cacheSize))
        FMPrepareEx(*handle, cacheSize);
}

int32 FMFileClose(anFMFile fd)
{
    store<uint32_t>(fileAt(fd), 0u);
    return 0;
}

// Mode 0 from the start, 1 from the position, 2 from the end; any other mode only returns the position.
uint32 FMFileSeek(anFMFile fd, int32 offset, int32_t seekMode)
{
    const uint32_t entry = fileAt(fd);
    if (seekMode == 1)
        store<uint32_t>(entry + 4u, load<uint32_t>(entry + 4u) + static_cast<uint32_t>(offset));
    else if (seekMode == 0)
        store<uint32_t>(entry + 4u, static_cast<uint32_t>(offset));
    else if (seekMode == 2)
        store<uint32_t>(entry + 4u, load<uint32_t>(entry + 0xcu) + static_cast<uint32_t>(offset));
    return load<uint32_t>(entry + 4u);
}

uint32 FMFileLength(int32_t fd)
{
    return load<uint32_t>(fileAt(fd) + 0xcu);
}

void FMInvalidateCache()
{
    for (uint32_t i = 0; i < 6u; ++i)
        store<uint32_t>(cacheAt(i), 0u);
}

// Seeks, then asks for the read until the drive accepts it and waits for it, a vertical blank between tries. Returns
// sceCdSync's last answer.
int32_t FMFileFetchSectors(uint32 sectorNum, ptr32<uint8> pBuffer, uint32 count)
{
    GuestFrame frame(0x50);
    guest_call(kCdSeek, sectorNum);
    const uint32_t mode = frame.local<sceCdRMode>(0).address;
    store<uint8_t>(mode, 0x20);    // trycount
    store<uint8_t>(mode + 1u, 1);  // spindlctrl
    store<uint8_t>(mode + 2u, 0);  // datapattern
    while (guest_call<int32_t>(kCdRead, sectorNum, count, pBuffer, mode) == 0)
        guest_call(kGsSyncV, 0);
    int32_t result;
    while ((result = guest_call<int32_t>(kCdSync, 1)) > 0)
        guest_call(kGsSyncV, 0);
    return result;
}

void FMUnPrepareEx()
{
}

// --- PCFILE.c ---

int32_t PCcreat(ptr32<char> name, int32_t perms)
{
    (void)perms;
    GuestFrame frame(0x120);
    const uint32_t newName = frame.local<char>(0).address;
    store<uint32_t>(newName, load<uint32_t>(kHost));
    store<uint16_t>(newName + 4u, load<uint16_t>(kHost + 4u));
    guest_call(kStrcat, newName, name);
    return guest_call<int32_t>(kSceOpen, newName, 0x602);
}

// Modes 0-2 pass through; any other passes the caller's $a3 (mode2 is never set for it).
int32_t PClseek(int32_t fd, int32_t offset, int32_t mode)
{
    uint32_t mode2 = getRegU32(&g_runtime->eeScheduler().runningContext(), 7);
    if (mode == 0)
        mode2 = 0;
    if (mode == 1)
        mode2 = 1;
    GuestFrame frame(0x10);
    return guest_call<int32_t>(kSceLseek, fd, offset, mode == 2 ? 2u : mode2);
}

int32_t PCread(int32_t fd, ptr32<char> buff, int32_t len)
{
    GuestFrame frame(0x80);
    const uint32_t total = static_cast<uint32_t>(len);
    uint32_t at = buff.address, done = 0, got = 0;
    if ((at & 0xfu) != 0u)
    {
        uint32_t head = 0x10u - (at & 0xfu);
        if (total < head)
            head = total;
        done = head;
        got = guest_call<uint32_t>(kSceRead, fd, kReadBuffer, head);
        guest_call(kMemcpy, at, kReadBuffer, head);
        at += done;
    }
    const uint32_t rest = total - done;
    if (rest >= 0x11u)
    {
        const uint32_t middle = rest & 0xfffffff0u;
        const uint32_t from = at;
        at += middle;
        got += guest_call<uint32_t>(kSceRead, fd, from, middle);
        done += middle;
    }
    if (total != done)
    {
        const uint32_t last = total - done;
        got += guest_call<uint32_t>(kSceRead, fd, kReadBuffer, last);
        guest_call(kMemcpy, at, kReadBuffer, last);
    }
    return static_cast<int32_t>(got);
}

int32_t PCwrite(int32_t fd, ptr32<char> buff, int32_t len)
{
    GuestFrame frame(0x80);
    const uint32_t total = static_cast<uint32_t>(len);
    uint32_t at = buff.address, done = 0, put = 0;
    if ((at & 0xfu) != 0u)
    {
        uint32_t head = 0x10u - (at & 0xfu);
        if (total < head)
            head = total;
        done = head;
        guest_call(kMemcpy, kWriteBuffer, at, head);
        at += done;
        put = guest_call<uint32_t>(kSceWrite, fd, kWriteBuffer, head);
    }
    const uint32_t rest = total - done;
    if (rest >= 0x11u)
    {
        const uint32_t middle = rest & 0xfffffff0u;
        const uint32_t from = at;
        at += middle;
        put += guest_call<uint32_t>(kSceWrite, fd, from, middle);
        done += middle;
    }
    if (total != done)
    {
        const uint32_t last = total - done;
        guest_call(kMemcpy, kWriteBuffer, at, last);
        put += guest_call<uint32_t>(kSceWrite, fd, kWriteBuffer, last);
    }
    return static_cast<int32_t>(put);
}

int32_t PCclose(int32_t fd)
{
    GuestFrame frame(0x10);
    return guest_call<int32_t>(kSceClose, fd);
}

} // namespace rc

RC_DECOMP_FN_SETUP(FMPrepareEx, 0x1a04a0, rc::fuzz::fmPrepareEx)
RC_DECOMP_FN_SETUP(FMSetCacheMode, 0x1a0500, rc::fuzz::fmCacheMode)
RC_DECOMP_FN_SETUP_HEAP(FMUnPrepare, 0x1a0540, rc::fuzz::fmUnPrepare)
RC_DECOMP_FN_BLOCKING_SETUP(FMFileOpen, 0x1a0550, rc::fuzz::fmFileOpen)
RC_DECOMP_FN_BLOCKING_SETUP(FMFileRead, 0x1a0838, rc::fuzz::fmFileRead)
RC_DECOMP_FN_SETUP(FMInit, 0x1a0b18, rc::fuzz::fmState)
RC_DECOMP_FN_SETUP_HEAP(FMPrepare, 0x1a0b70, rc::fuzz::fmPrepare)
RC_DECOMP_FN_SETUP_HEAP(FMPrepareHi, 0x1a0bb0, rc::fuzz::fmPrepare)
RC_DECOMP_FN_SETUP(FMFileClose, 0x1a0bf0, rc::fuzz::fmFileArgs)
RC_DECOMP_FN_SETUP(FMFileSeek, 0x1a0c10, rc::fuzz::fmFileArgs)
RC_DECOMP_FN_SETUP(FMFileLength, 0x1a0ca0, rc::fuzz::fmFileArgs)
RC_DECOMP_FN_SETUP(FMInvalidateCache, 0x1a0cb8, rc::fuzz::fmState)
RC_DECOMP_FN_BLOCKING_SETUP(FMFileFetchSectors, 0x1a0ce0, rc::fuzz::fmFetch)
RC_DECOMP_FN(FMUnPrepareEx, 0x1a0f20)
RC_DECOMP_FN_BLOCKING_SETUP(PCcreat, 0x13cf10, rc::fuzz::pcCreat)
RC_DECOMP_FN_BLOCKING_SETUP(PClseek, 0x13cf58, rc::fuzz::pcLseek)
RC_DECOMP_FN_BLOCKING_SETUP(PCread, 0x13cf90, rc::fuzz::pcReadWrite)
RC_DECOMP_FN_BLOCKING_SETUP(PCwrite, 0x13d0a8, rc::fuzz::pcReadWrite)
RC_DECOMP_FN_BLOCKING_SETUP(PCclose, 0x13d1c0, rc::fuzz::pcClose)

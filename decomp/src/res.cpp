// Decompiled from RES.c (the resource loader) and TSO.c (scenery objects).
//
// The loader opens the game's resource database at boot (ResInit: the .luf lookup file, the .tlf type list and the
// CAT\RDS.CAT concatenation of every resource), then loads resources by id: each one is a binary header (two checksums,
// the type, fix-up and swizzle counts and the data length), its fix-up and swizzle tables, eight bytes and the data.
// Fix-ups are offsets of words to make absolute (ResAutoRelocateR); swizzles are offsets of words holding resource ids
// to load and replace by their handles (ResAutoLoadR). After a load the type's construct daemon runs. Every read retries
// up to six times and then calls the read error function (LoadErrorCallback, which does nothing), and tries again.
//
// Never called by the game (no jal, no address taken, no data pointer): ResFreeBlock, ResGetHandle, ResLoadBlock and
// ResLoadTemporaryHandles. ResCompType is reached through qsort and bsearch, ResSTBConstructDaemon through the
// daemon table.

#include "rc_decomp.h"

namespace rc {

bool_ MemBlockAllocLo(ptr32<memHandle> pHandle, uint32 size); // mem.cpp
void MemBlockFree(ptr32<memHandle> pHandle);

uint32 ResBinaryChecksum(uint32 checkSum, ptr32<uint32> pCs, uint32 len);
uint32 ResCalcResourceFootPrint(uint32 binarySize);
uint32 ResLoadImage(uint32 id, ptr32<void> pMem);
void ResAutoRelocateR(ptr32<uint8> pLoadedData, uint32 id);
void ResAutoLoadR(ptr32<uint8> pLoadedData, uint32 id);
ptr32<void> ResLoad(uint32 id);
void ResGetNames();
int32 ResOpenDB();
void ResReadTypeList();
void ResInitLoading();
void ResFreeTypeList();
void ResFreeTemporaryHandles();
int32 ResCloseDB();
ptr32<rdsTypeListRecord> ResFindType(ptr32<char> pType);
void ResTypeSetConstructDaemon(ptr32<char> pType, uint32 constructDaemon, int16 options);

namespace {

constexpr uint32_t kRes = 0x31e070u; // resVarG
// Offsets in resVarG used by address.
constexpr uint32_t kNamePathCopy = 0x8u, kResNames = 0x48u, kCatHeader = 0x88u, kConcatOffsets = 0x90u,
                   kBinaryHeader = 0xa8u, kSelectionPath = 0x308u, kLookupHeader = 0x388u, kTypePtrs = 0x398u,
                   kTypeList = 0x39cu, kDaemons = 0x3a0u, kHandles = 0x3a4u, kBlockListHeader = 0x3a8u;
// Strings: "\" (between path and name), "STB", "\CAT\rds.cat", "%s\CAT\RDS.CAT".
constexpr uint32_t kBackslash = 0x3283c8u, kStbType = 0x3283c0u, kCatName = 0x324d10u, kCatFormat = 0x324d20u;
constexpr uint32_t kStrcpy = 0x1b8d60u, kStrcat = 0x1b8af0u, kStrcmp = 0x1b8c1cu, kStrrchr = 0x1b9328u, kSprintf = 0x1b8910u,
                   kQsort = 0x1b7de0u;
constexpr uint32_t kFMFileOpen = 0x1a0550u, kFMFileRead = 0x1a0838u, kFMFileClose = 0x1a0bf0u, kFMFileSeek = 0x1a0c10u,
                   kFMFileLength = 0x1a0ca0u;
constexpr uint32_t kResCompType = 0x1abc80u, kResSTBConstructDaemon = 0x1abca0u;

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

resClass &res() { return G(resVarG); }

uint32_t fileRead(uint32_t data, uint32_t size, int32_t fd) { return guest_call<uint32_t>(kFMFileRead, data, size, fd); }

// The rule every read loop of the loader follows after a failed attempt: retry at once until six attempts have failed,
// then count once more and call the read error function, which also restarts the count.
void readFailed(uint32_t &retries)
{
    if (++retries < 6u)
        return;
    ++retries;
    const uint32_t errorFn = res().pfLoadErrorFunction.address;
    if (errorFn != 0u)
    {
        retries = 0;
        guest_call(errorFn);
    }
}

// A read loop that counts each attempt, success included (as most of the loader's do), and fails if fewer bytes came.
template <typename Attempt>
void readCounted(uint32_t &retries, Attempt attempt)
{
    for (;;)
    {
        const bool done = attempt();
        ++retries;
        if (done)
            return;
        if (retries < 6u)
            continue;
        ++retries;
        const uint32_t errorFn = res().pfLoadErrorFunction.address;
        if (errorFn != 0u)
        {
            retries = 0;
            guest_call(errorFn);
        }
    }
}

// After a load: the type's daemon entry may relocate the fix-ups (option 2), load the swizzled ids (option 4) and call
// the daemon with the handle's address.
void constructLoaded(uint32_t entry, uint32_t id, uint32_t handle)
{
    if (entry == 0u)
        return;
    uint16_t options = load<uint16_t>(entry + 4u);
    if (options == 0u)
        return;
    if ((options & 2u) != 0u)
    {
        ResAutoRelocateR(ptr32<uint8>(res().pFixupData.address), id);
        options = load<uint16_t>(entry + 4u);
    }
    if ((options & 4u) != 0u)
        ResAutoLoadR(ptr32<uint8>(res().pFixupData.address), id);
    const uint32_t daemon = load<uint32_t>(entry);
    if (daemon != 0u)
        guest_call(daemon, handle);
}

} // namespace

// Opens the database named by namePath ("\RC2": its folder and base name) and sets the loader up with buffer as the
// area resources load into, top down. Returns ResOpenDB's result if a file is missing, else 0.
int32 ResInit(ptr32<char> namePath, ptr32<uint8> buffer, uint32 bufSize)
{
    GuestFrame frame(0x170);
    resClass &r = res();
    const uint32_t errorFn = r.pfLoadErrorFunction.address; // the only field kept
    std::memset(&r, 0, 0x3d4u);
    r.pfLoadErrorFunction.address = errorFn;
    r.initFlag = 1;
    guest_call(kStrcpy, kRes + kNamePathCopy, namePath);
    const uint32_t slash = guest_call<uint32_t>(kStrrchr, kRes + kNamePathCopy, 0x5c);
    store<uint8_t>(slash, 0);
    const uint32_t end = buffer.address + bufSize;
    r.pPath = ptr32<char>(kRes + kNamePathCopy);
    r.pName = ptr32<char>(slash + 1u);
    r.size = bufSize;
    r.pTop = buffer;
    r.pCurr = ptr32<uint8>(end);
    r.catHandle = -1;
    r.ppBlockList = ptr32<ptr32<resMemBinaryHeader>>(kRes + kBlockListHeader);
    r.resLoadedCount = 0;
    r.resSTB = 0;
    r.pBot = ptr32<uint8>(end);
    r.types = 0;
    r.pTypePtrs = nullptr;
    r.pTypeList = nullptr;
    r.lufHandle = r.tlfHandle = r.cfgHandle = -1;
    r.pBlockListHeader = nullptr;
    r.pFixupData = r.pResourceData = r.pMemHeaderData = nullptr;
    r.loadFlag = 0;
    const int32 opened = ResOpenDB();
    if (opened != 0)
        return opened;
    ResReadTypeList();
    ResInitLoading();
    std::memcpy(guest_host(frame.local<char>(0).address), guest_host(kCatName), 13u);
    r.catHandle = guest_call<int32_t>(kFMFileOpen, frame.local<char>(0));
    const ptr32<rdsTypeListRecord> stb = ResFindType(ptr32<char>(kStbType));
    r.resSTB = load<int16_t>(stb.address + 8u);
    ResTypeSetConstructDaemon(ptr32<char>(kStbType), kResSTBConstructDaemon, 1);
    return 0;
}

// Ready for a level's loads: the area empty, no blocks, and the handle table (one word per id) allocated if needed.
void ResLoadInit()
{
    resClass &r = res();
    const uint32_t bottom = r.pBot.address;
    const uint32_t maxId = r.lookupHeader.maxId;
    const uint32_t handles = r.handlesHandle.address;
    r.initFlag = 1;
    r.pCurr = ptr32<uint8>(bottom);
    r.ppBlockList = ptr32<ptr32<resMemBinaryHeader>>(kRes + kBlockListHeader);
    r.resLoadedCount = 0;
    r.pBlockListHeader = nullptr;
    r.pFixupData = nullptr;
    r.pResourceData = nullptr;
    r.pMemHeaderData = nullptr;
    r.loadFlag = 0;
    if (handles == 0u)
        MemBlockAllocLo(ptr32<memHandle>(kRes + kHandles), maxId << 2);
}

// Fills the handle table from the lookup file: entry 0 empty, ids in the concatenation (flag 0x400) get their offset
// in it with both low bits set, the others their flags with bit 1 set. Reads the concatenation's offsets if needed.
void ResLoadTemporaryHandles()
{
    GuestFrame frame(0x1a0);
    resClass &r = res();
    bool offsetsRead = false;
    uint32_t retries = 0;
    const uint32_t tableSize = r.lookupHeader.maxId << 2;
    guest_call(kFMFileSeek, r.lufHandle, 4, 0);
    readCounted(retries, [&] { return fileRead(r.handlesHandle.address, tableSize, r.lufHandle) == tableSize; });
    uint32_t entry = r.handlesHandle.address;
    store<uint32_t>(entry, 0);
    entry += 4u;
    for (uint32_t id = 1; id < r.lookupHeader.maxId; ++id, entry += 4u)
    {
        const uint32_t flags = load<uint32_t>(entry);
        if ((flags & 0x400u) == 0u)
        {
            store<uint32_t>(entry, flags | 2u);
            continue;
        }
        if (r.pConcatOffsets.address == 0u)
        {
            const ptr32<char> name = frame.local<char>(0);
            guest_call(kSprintf, name, kCatFormat, kRes + kSelectionPath);
            if (r.catHandle == -1)
                r.catHandle = guest_call<int32_t>(kFMFileOpen, name);
            else
                guest_call(kFMFileSeek, r.catHandle, 0, 0);
            retries = 0;
            readCounted(retries, [&] { return fileRead(kRes + kCatHeader, 8, r.catHandle) == 8u; });
            retries = 0;
            MemBlockAllocLo(ptr32<memHandle>(kRes + kConcatOffsets), (r.catHeader.maxId + 1u) << 2);
            readCounted(retries, [&]
                        {
                            const uint32_t size = r.catHeader.maxId << 2;
                            const uint32_t read = fileRead(r.pConcatOffsets.address, size, r.catHandle);
                            return read == (r.catHeader.maxId << 2);
                        });
            offsetsRead = true;
        }
        store<uint32_t>(entry, load<uint32_t>(r.pConcatOffsets.address + id * 4u) | 3u);
    }
    if (offsetsRead)
        MemBlockFree(ptr32<memHandle>(kRes + kConcatOffsets));
}

void ResFreeTemporaryHandles()
{
    if (res().handlesHandle.address != 0u)
        MemBlockFree(ptr32<memHandle>(kRes + kHandles));
}

void ResTerm()
{
    {
        GuestFrame frame(0x10);
        res().initFlag = 0;
        ResFreeTypeList();
        ResFreeTemporaryHandles();
    }
    ResCloseDB();
}

void ResLoadTerm() { ResFreeTemporaryHandles(); }

// Reads the type list file: a count, then 12-byte records (an 8-byte name, the type's index, flags). Allocates the
// list, a table of pointers to its records sorted by name, and the daemon table, cleared.
void ResReadTypeList()
{
    GuestFrame frame(0xa0);
    resClass &r = res();
    uint32_t retries = 0;
    const uint32_t length = guest_call<uint32_t>(kFMFileLength, r.tlfHandle);
    const uint32_t listSize = length - 8u;
    guest_call(kFMFileSeek, r.tlfHandle, 0, 0);
    const uint32_t header = frame.local<uint32_t>(0).address;
    while (fileRead(header, 8, r.tlfHandle) != 8u)
        readFailed(retries);
    r.types = load<int32_t>(header);
    MemBlockAllocLo(ptr32<memHandle>(kRes + kTypeList), length + 4u);
    MemBlockAllocLo(ptr32<memHandle>(kRes + kTypePtrs), static_cast<uint32_t>(r.types) << 2);
    MemBlockAllocLo(ptr32<memHandle>(kRes + kDaemons), static_cast<uint32_t>(r.types) << 3);
    const int32_t types = r.types;
    const uint32_t daemons = r.pConstructDaemonsList.address;
    for (int32_t i = 0; i < types; ++i)
    {
        const uint32_t entry = daemons + static_cast<uint32_t>(i) * 8u;
        store<uint16_t>(entry + 6u, 0);
        store<uint32_t>(entry, 0);
        store<uint16_t>(entry + 4u, 0);
    }
    readCounted(retries, [&] { return fileRead(r.pTypeList.address, listSize, r.tlfHandle) == listSize; });
    const int32_t count = r.types;
    const uint32_t pointers = r.pTypePtrs.address;
    const uint32_t list = r.pTypeList.address;
    for (int32_t i = 0; i < count; ++i)
        store<uint32_t>(pointers + static_cast<uint32_t>(i) * 4u, list + static_cast<uint32_t>(i) * 12u);
    guest_call(kQsort, r.pTypePtrs, r.types, 4, kResCompType);
}

void ResFreeTypeList()
{
    GuestFrame frame(0x20);
    MemBlockFree(ptr32<memHandle>(kRes + kTypeList));
    MemBlockFree(ptr32<memHandle>(kRes + kTypePtrs));
    MemBlockFree(ptr32<memHandle>(kRes + kDaemons));
}

// Opens <path>\<name>.luf and .tlf. Returns 1 if either is missing.
int32 ResOpenDB()
{
    GuestFrame frame(0x130);
    resClass &r = res();
    const ptr32<char> name = frame.local<char>(0);
    ResGetNames();
    guest_call(kStrcpy, name, r.pPath);
    guest_call(kStrcat, name, kBackslash);
    guest_call(kStrcat, name, kRes + kResNames);
    r.lufHandle = guest_call<int32_t>(kFMFileOpen, name);
    guest_call(kStrcpy, name, r.pPath);
    guest_call(kStrcat, name, kBackslash);
    guest_call(kStrcat, name, kRes + kResNames + 0x10u);
    const int32_t tlf = guest_call<int32_t>(kFMFileOpen, name);
    r.tlfHandle = tlf;
    return r.lufHandle == -1 || tlf == -1 ? 1 : 0;
}

// Loads a resource into the area below pCurr, unless its handle is already a pointer (low bits clear), and returns
// the handle.
ptr32<void> ResLoad(uint32 id)
{
    GuestFrame frame(0x70);
    resClass &r = res();
    const uint32_t handles = r.handlesHandle.address;
    r.loadFlagStore = 0;
    const uint32_t slot = handles + (id << 2);
    const uint32_t handle = load<uint32_t>(slot);
    if ((handle & 3u) == 0u)
        return ptr32<void>(handle);
    ResLoadImage(id, ptr32<void>(r.pCurr.address));
    const uint32_t resource = r.pResourceData.address;
    const uint32_t current = r.pCurr.address;
    store<uint32_t>(slot, resource);
    const uint32_t daemons = r.pConstructDaemonsList.address;
    const uint32_t length = static_cast<uint32_t>(r.binaryHeader.binaryLength);
    const uint32_t type = r.binaryHeader.typeVal;
    r.pPrevCurr = ptr32<uint8>(current);
    const uint32_t used = length + ((current - length) & 0xfu);
    const uint32_t entry = daemons + type * 8u;
    r.pCurr = ptr32<uint8>(current - used);
    constructLoaded(entry, id, slot);
    return ptr32<void>(load<uint32_t>(handles + (id << 2)));
}

void ResFreeBlock(ptr32<ptr32<void>> handle) { MemBlockFree(ptr32<memHandle>(handle.address)); }

// Reads a resource: its header (checked against its own checksum), then the tables and data, checked against the
// header's checksum, retrying until both match. Block loads (loadFlag 1) go to a heap block in the handle's slot,
// others end just below pMem (the data 16-byte aligned to pMem - length). Returns the bytes read.
uint32 ResLoadImage(uint32 id, ptr32<void> pMem)
{
    GuestFrame frame(0x200);
    resClass &r = res();
    const uint32_t index = id << 2;
    int32_t fd = 0;
    uint32_t footprint = 0;
    const uint32_t handles = r.handlesHandle.address;
    r.resLoadedCount += 1;
    bool fromCat = false;
    uint32_t offset = 0;
    bool again = true;
    const uint32_t slot = index + handles;
    const uint32_t handle = load<uint32_t>(slot);
    int32_t retries = 0;
    if ((handle & 3u) == 3u)
    {
        fd = r.catHandle;
        fromCat = true;
        offset = handle & ~3u;
    }
    if (r.loadFlag == 1)
        store<uint32_t>(slot, 0);
    do
    {
        guest_call(kFMFileSeek, fd, offset, 0);
        uint32_t read = fileRead(kRes + kBinaryHeader, 0x1c, fd);
        uint32_t sum = ResBinaryChecksum(0, ptr32<uint32>(kRes + kBinaryHeader + 8u), 0x14);
        if (read != 0x1cu || sum != r.binaryHeader.magic)
        {
            if (++retries < 6)
                continue;
        }
        else
        {
            sum = ResBinaryChecksum(0, ptr32<uint32>(kRes + kBinaryHeader + 4u), 0x18);
            const uint32_t length = static_cast<uint32_t>(r.binaryHeader.binaryLength);
            footprint = ResCalcResourceFootPrint(length);
            uint32_t start, data;
            if (r.loadFlag == 1)
            {
                const uint32_t blockHandle = index + handles;
                start = load<uint32_t>(blockHandle);
                if (start == 0u)
                {
                    MemBlockAllocLo(ptr32<memHandle>(blockHandle), footprint);
                    start = load<uint32_t>(blockHandle);
                }
                read = fileRead(start, footprint, fd);
                sum = ResBinaryChecksum(sum, ptr32<uint32>(start), footprint);
                data = start + footprint - length;
            }
            else
            {
                const uint32_t end = pMem.address - length;
                const uint32_t pad = end & 0xfu;
                start = pMem.address - footprint - pad;
                read = fileRead(start, footprint, fd);
                sum = ResBinaryChecksum(sum, ptr32<uint32>(start), footprint);
                data = end - pad;
            }
            // A good read ends the loop; a bad one is retried, the error function called after the sixth.
            if (read == footprint && sum == r.binaryHeader.checksum)
                again = false;
            else if (++retries >= 6)
                goto readError;
            r.pFixupData = ptr32<uint8>(start);
            r.pResourceData = ptr32<uint8>(data);
            r.pMemHeaderData = ptr32<uint8>(data - 8u);
            continue;
        }
    readError:
        const uint32_t errorFn = r.pfLoadErrorFunction.address;
        if (errorFn != 0u)
        {
            retries = 0;
            guest_call(errorFn);
        }
    } while (again);
    if (!fromCat)
        guest_call(kFMFileClose, fd);
    return footprint;
}

// A pure function of its arguments: the CRC-32 (reflected, the table in crc_tableG) of len bytes, 0 for a null buffer.
uint32 ResBinaryCrc(uint32 crc, ptr32<uint8> buf, uint32 len)
{
    if (buf.address == 0u)
        return 0;
    const auto &table = G(crc_tableG);
    uint32_t c = ~crc;
    for (uint32_t at = buf.address; len != 0u; --len, ++at)
        c = table[(c ^ load<uint8_t>(at)) & 0xffu] ^ (c >> 8);
    return ~c;
}

// Takes blockSize bytes off the bottom of the free area; null if that passes the top.
ptr32<void> ResAlloc(uint32 blockSize)
{
    resClass &r = res();
    const uint32_t current = r.pCurr.address;
    const uint32_t top = r.pTop.address;
    const uint32_t block = current - blockSize;
    r.pPrevCurr = ptr32<uint8>(current);
    r.pCurr = ptr32<uint8>(block);
    return ptr32<void>(block < top ? 0u : block);
}

void ResReAlloc(ptr32<void> p) { res().pCurr = ptr32<uint8>(p.address); }

ptr32<void> ResReadAlloc()
{
    resClass &r = res();
    const ptr32<ptr32<resMemBinaryHeader>> saved = r.ppBlockListSave;
    const uint32_t previous = r.pPrevCurr.address;
    r.ppBlockList = saved;
    return ptr32<void>(previous);
}

// Finds a resource type by its name, such as "RCAF". The loader keeps pointers to the types sorted by name.
ptr32<rdsTypeListRecord> ResFindType(ptr32<char> pType)
{
    const resClass &r = G(resVarG);
    uint32_t low = 0, count = static_cast<uint32_t>(r.types);
    while (low < count)
    {
        const uint32_t middle = (low + count) >> 1;
        const ptr32<rdsTypeListRecord> record = r.pTypePtrs[middle];
        const int order = std::strcmp(pType.get(), record->type);
        if (order == 0)
            return record;
        if (order > 0)
            low = middle + 1;
        else
            count = middle;
    }
    return nullptr;
}

// Sets the function the loader calls when a resource of this type has been loaded.
void ResTypeSetConstructDaemon(ptr32<char> pType, uint32 constructDaemon, int16 options)
{
    const ptr32<rdsTypeListRecord> type = ResFindType(pType);
    if (!type)
        return;
    resConstruct &construct = G(resVarG).pConstructDaemonsList[type->typeVal];
    construct.options = static_cast<uint16>(options);
    construct.constructDaemon.address = constructDaemon;
}

void ResTypeUnSetConstructDaemon(ptr32<char> pType)
{
    const ptr32<rdsTypeListRecord> type = ResFindType(pType);
    if (!type)
        return;
    resConstruct &construct = G(resVarG).pConstructDaemonsList[type->typeVal];
    construct.version = 0;
    construct.constructDaemon.address = 0;
    construct.options = 0;
}

// As ResLoad, into a heap block whose old slot value is stored at handle; the free area is not used.
ptr32<void> ResLoadBlock(uint32 id, ptr32<ptr32<void>> handle)
{
    GuestFrame frame(0x70);
    resClass &r = res();
    const uint32_t handles = r.handlesHandle.address;
    r.loadFlagStore = 0;
    const uint32_t slot = handles + (id << 2);
    const uint32_t value = load<uint32_t>(slot);
    if ((value & 3u) == 0u)
        return ptr32<void>(value);
    const uint32_t current = r.pCurr.address;
    r.loadFlag = 1;
    r.loadFlagStore = 1;
    ResLoadImage(id, ptr32<void>(current));
    r.loadFlag = 0;
    const uint32_t resource = r.pResourceData.address;
    const uint32_t block = load<uint32_t>(slot);
    store<uint32_t>(slot, resource);
    const uint32_t daemons = r.pConstructDaemonsList.address;
    const uint32_t type = r.binaryHeader.typeVal;
    store<uint32_t>(handle.address, block);
    constructLoaded(daemons + type * 8u, id, slot);
    return ptr32<void>(load<uint32_t>(handles + (id << 2)));
}

ptr32<void> ResGetHandle(uint32 id) { return ptr32<void>(load<uint32_t>(res().handlesHandle.address + (id << 2))); }

void ResSetReadErrorFn(uint32 pfLoadErrorFunction) { res().pfLoadErrorFunction.address = pfLoadErrorFunction; }

// A pure function of its arguments: the words of the buffer (len / 4 of them) XORed into checkSum.
uint32 ResBinaryChecksum(uint32 checkSum, ptr32<uint32> pCs, uint32 len)
{
    for (uint32_t words = len >> 2, at = pCs.address; words != 0u; --words, at += 4u)
        checkSum ^= load<uint32_t>(at);
    return checkSum;
}

int32 ResCloseDB()
{
    GuestFrame frame(0x20);
    resClass &r = res();
    guest_call(kFMFileClose, r.lufHandle);
    guest_call(kFMFileClose, r.tlfHandle);
    const int32_t cat = r.catHandle;
    if (cat != 0)
    {
        guest_call(kFMFileClose, cat);
        r.catHandle = -1;
    }
    return 0;
}

// The three file names: the base name with each of resExtG's extensions (".luf", ".tlf", ".cfg").
void ResGetNames()
{
    GuestFrame frame(0x70);
    resClass &r = res();
    const ptr32<char> name = frame.local<char>(0);
    for (uint32_t i = 0; i < 3u; ++i)
    {
        guest_call(kStrcpy, name, r.pName);
        guest_call(kStrcat, name, G(resExtG)[i]);
        guest_call(kStrcpy, kRes + kResNames + i * 0x10u, name);
    }
}

// The comparison qsort and bsearch use on the type pointers: strcmp of the two records' names.
int32_t ResCompType(ptr32<void> a, ptr32<void> b)
{
    return guest_call<int32_t>(kStrcmp, load<uint32_t>(a.address), load<uint32_t>(b.address));
}

// An STB resource is a list of resource ids (its length / 4 of them), all loaded with it.
void ResSTBConstructDaemon(ptr32<ptr32<void>> handle)
{
    GuestFrame frame(0x40);
    const uint32_t count = static_cast<uint32_t>(res().binaryHeader.binaryLength) >> 2;
    const uint32_t list = load<uint32_t>(handle.address);
    for (uint32_t i = 0; i < count; ++i)
        ResLoad(load<uint32_t>(list + i * 4u));
}

// Reads the lookup file's header (the number of ids).
void ResInitLoading()
{
    GuestFrame frame(0x50);
    resClass &r = res();
    uint32_t retries = 0;
    readCounted(retries, [&] { return fileRead(kRes + kLookupHeader, 4, r.lufHandle) == 4u; });
}

// Makes the loaded data's fix-ups absolute: each fix-up is the offset from the data start (after the tables and 8
// bytes) of a word that holds an offset from there too.
void ResAutoRelocateR(ptr32<uint8> pLoadedData, uint32)
{
    const resClass &r = res();
    const uint32_t fixups = r.binaryHeader.fixups;
    const uint32_t base = pLoadedData.address + fixups * 4u + static_cast<uint32_t>(r.binaryHeader.swizzles) * 4u + 8u;
    for (uint32_t i = 0; i < fixups; ++i)
    {
        const uint32_t word = base + load<uint32_t>(pLoadedData.address + i * 4u);
        store<uint32_t>(word, base + load<uint32_t>(word));
    }
}

// Loads the resources the data's swizzled words name and replaces each id by its handle. The swizzle table is copied
// to a heap block first, since the loads can reuse the area it lies in.
void ResAutoLoadR(ptr32<uint8> pLoadedData, uint32)
{
    GuestFrame frame(0x70);
    const resClass &r = res();
    const uint32_t swizzles = r.binaryHeader.swizzles;
    const uint32_t table = pLoadedData.address + static_cast<uint32_t>(r.binaryHeader.fixups) * 4u;
    const uint32_t base = table + swizzles * 4u + 8u;
    if (swizzles == 0u)
        return;
    const uint32_t handle = frame.local<uint32_t>(0).address;
    store<uint32_t>(handle, 0);
    MemBlockAllocLo(ptr32<memHandle>(handle), swizzles * 4u);
    const uint32_t copy = load<uint32_t>(handle);
    for (uint32_t i = 0; i < swizzles; ++i)
        store<uint32_t>(copy + i * 4u, load<uint32_t>(table + i * 4u));
    for (uint32_t i = 0; i < swizzles; ++i)
    {
        const uint32_t word = base + load<uint32_t>(copy + i * 4u);
        const uint32_t loaded = ResLoad(load<uint32_t>(word)).address;
        store<uint32_t>(word, loaded);
    }
    MemBlockFree(ptr32<memHandle>(handle));
}

uint32 ResCalcResourceFootPrint(uint32 binarySize)
{
    const resClass &r = res();
    return (static_cast<uint32_t>(r.binaryHeader.swizzles) + r.binaryHeader.fixups) * 4u + 8u + binarySize;
}

// TSO.c: the level's scenery object with this tag, or null.
ptr32<_sTSO> TSOFindByTag(uint32 tag)
{
    const ptr32<_sTSOHeader> header = G(tsoVarG).pTSOHeader;
    const ptr32<_sTSO> objects(header.address + 4u);
    for (uint32_t i = 0; i < header->numObj; ++i)
    {
        if (objects[i].tag == tag)
            return objects + static_cast<int32_t>(i);
    }
    return nullptr;
}

} // namespace rc

#include "res_fuzz.h"

RC_DECOMP_FN_BLOCKING_SETUP_HEAP(ResInit, 0x1aa708, rc::fuzz::resInit)
RC_DECOMP_FN_SETUP_HEAP(ResLoadInit, 0x1aa8b0, rc::fuzz::resHandles)
RC_DECOMP_FN_BLOCKING_SETUP_HEAP(ResLoadTemporaryHandles, 0x1aa910, rc::fuzz::resTemporaryHandles)
RC_DECOMP_FN_SETUP_HEAP(ResFreeTemporaryHandles, 0x1aad90, rc::fuzz::resHandles)
RC_DECOMP_FN_SETUP_HEAP(ResTerm, 0x1aadb8, rc::fuzz::resTerm)
RC_DECOMP_FN_SETUP_HEAP(ResLoadTerm, 0x1aade0, rc::fuzz::resHandles)
RC_DECOMP_FN_BLOCKING_SETUP_HEAP(ResReadTypeList, 0x1aade8, rc::fuzz::resTypeList)
RC_DECOMP_FN_SETUP_HEAP(ResFreeTypeList, 0x1ab0c0, rc::fuzz::resTerm)
RC_DECOMP_FN_BLOCKING_SETUP(ResOpenDB, 0x1ab0f8, rc::fuzz::resOpen)
RC_DECOMP_FN_BLOCKING_SETUP_HEAP(ResLoad, 0x1ab1b0, rc::fuzz::resLoad)
RC_DECOMP_FN_SETUP_HEAP(ResFreeBlock, 0x1ab2c8, rc::fuzz::resFreeBlock)
RC_DECOMP_FN_BLOCKING_SETUP_HEAP(ResLoadImage, 0x1ab2d0, rc::fuzz::resLoadImage)
RC_DECOMP_FN_SETUP(ResBinaryCrc, 0x1ab590, rc::fuzz::resCrc)
RC_DECOMP_FN_SETUP(ResAlloc, 0x1ab828, rc::fuzz::resArea)
RC_DECOMP_FN_SETUP(ResReAlloc, 0x1ab850, rc::fuzz::resArea)
RC_DECOMP_FN_SETUP(ResReadAlloc, 0x1ab860, rc::fuzz::resArea)
RC_DECOMP_FN_NOFUZZ(ResTypeSetConstructDaemon, 0x1ab878)
RC_DECOMP_FN_NOFUZZ(ResTypeUnSetConstructDaemon, 0x1ab8d8)
RC_DECOMP_FN_NOFUZZ(ResFindType, 0x1ab920)
RC_DECOMP_FN_BLOCKING_SETUP_HEAP(ResLoadBlock, 0x1ab980, rc::fuzz::resLoadBlock)
RC_DECOMP_FN_SETUP(ResGetHandle, 0x1aba98, rc::fuzz::resGetHandle)
RC_DECOMP_FN(ResSetReadErrorFn, 0x1abab0)
RC_DECOMP_FN_SETUP(ResBinaryChecksum, 0x1abac0, rc::fuzz::resChecksum)
RC_DECOMP_FN_SETUP(ResCloseDB, 0x1abb60, rc::fuzz::resClose)
RC_DECOMP_FN_SETUP(ResGetNames, 0x1abbb8, rc::fuzz::resNames)
RC_DECOMP_FN_SETUP(ResCompType, 0x1abc80, rc::fuzz::resCompType)
RC_DECOMP_FN_BLOCKING_SETUP_HEAP(ResSTBConstructDaemon, 0x1abca0, rc::fuzz::resSTB)
RC_DECOMP_FN_BLOCKING_SETUP(ResInitLoading, 0x1abd70, rc::fuzz::resInitLoading)
RC_DECOMP_FN_SETUP(ResAutoRelocateR, 0x1abea8, rc::fuzz::resRelocate)
RC_DECOMP_FN_BLOCKING_SETUP_HEAP(ResAutoLoadR, 0x1abfa8, rc::fuzz::resAutoLoad)
RC_DECOMP_FN_SETUP(ResCalcResourceFootPrint, 0x1ac190, rc::fuzz::resArea)
RC_DECOMP_FN_NOFUZZ(TSOFindByTag, 0x18cb78)

// Builds the random-test inputs of MEM.c, MEMFRAG.c, HEAP.c and TASK.c. See mem_fuzz.h.

#include "mem_fuzz.h"

#include <algorithm>
#include <vector>

namespace rc {

// The native functions, used to put the heap, pools and task list into varied states.
bool_ MemBlockAllocLo(ptr32<memHandle> pHandle, uint32 size);
bool_ MemBlockAllocHi(ptr32<memHandle> pHandle, uint32 size);
void MemBlockFree(ptr32<memHandle> pHandle);
bool_ MemFragPoolCreate(ptr32<ptr32<_sMemFragPool>> ppPool, uint32 fragsPerBlock, uint32 maxFrags, uint32 fragSize, uint32 flags);
memFragHandle MemFragCreate(ptr32<_sMemFragPool> pPool);
void MemFragDelete(ptr32<_sMemFragPool> pPool, memFragHandle fragHandle);
void TaskInit();
void TaskInsert(ptr32<_sTask> pTask);
void TaskCachedInsert(ptr32<_sTask> pTask);

namespace fuzz {

namespace {

constexpr uint32_t kMemVars = 0x1fa940u, kHeapVars = 0x1f4c50u, kTaskVars = 0x31ff58u, kWeaponVars = 0x1f3a80u;
constexpr uint32_t kSfxInitOk = 0x31e488u;
// Callbacks for scratch tasks.
constexpr uint32_t kHeapFree = 0x1908c8u, kVSyncCallback = 0x19f260u, kMemOutOfMemory = 0x194a38u, kTaskDelete = 0x1afc48u,
                   kTaskCachedInsert = 0x1afb68u, kTaskInsert = 0x1afaf0u;
// Scratch layout: the handle under test, a spare handle, a pool handle, fragment handles, tasks, a small heap, copies.
constexpr uint32_t kHandle = 0x0u, kSpare = 0x10u, kPoolHandle = 0x20u, kTasks = 0x400u, kTaskSize = 0x18u, kFakeHeap = 0x1000u,
                   kCopy = 0x2000u;
constexpr uint32_t kHeader = 0x40u;

template <typename T>
T &mem(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

uint32_t rnd(uint64_t &state, uint32_t n) { return n == 0 ? 0u : nextFuzz(state) % n; }
bool chance(uint64_t &state, uint32_t percent) { return rnd(state, 100) < percent; }
void setArg(R5900Context *ctx, int index, uint32_t value) { SET_GPR_S32(ctx, 4 + index, static_cast<int32_t>(value)); }

memClass &memVars() { return mem<memClass>(kMemVars); }
uint32_t type(uint32_t block) { return mem<uint32_t>(block); }
uint32_t size(uint32_t block) { return mem<uint32_t>(block + 4u); }
uint32_t prev(uint32_t block) { return mem<uint32_t>(block + 8u); }
uint32_t next(uint32_t block) { return mem<uint32_t>(block + 0xcu); }

// The heap's blocks of one type, in address order.
std::vector<uint32_t> blocks(uint32_t wanted)
{
    std::vector<uint32_t> found;
    const uint32_t header = memVars().pHeader.address;
    for (uint32_t block = next(header), n = 0; block != header && n < 100000; block = next(block), ++n)
        if (type(block) == wanted)
            found.push_back(block);
    return found;
}

uint32_t largestFree()
{
    uint32_t largest = 0;
    for (uint32_t block : blocks(MEM_BLOCK_TYPE_FREE))
        largest = std::max(largest, size(block));
    return largest;
}

ptr32<memHandle> spare(uint32_t scratch) { return ptr32<memHandle>(scratch + kSpare); }

// Allocates, keeping nothing: blocks that stay in use.
void allocate(uint64_t &state, uint32_t scratch, uint32_t bytes)
{
    if (chance(state, 50))
        MemBlockAllocLo(spare(scratch), bytes);
    else
        MemBlockAllocHi(spare(scratch), bytes);
}

// Fills the heap: the largest free blocks are allocated whole or with up to three headers' worth left, then (half the
// time) everything that fits a 64 KB block, so little or nothing is left.
void fill(uint64_t &state, uint32_t scratch)
{
    for (int i = 0, n = 1 + static_cast<int>(rnd(state, 3)); i < n; ++i)
    {
        const uint32_t largest = largestFree();
        if (largest < 2 * kHeader)
            break;
        const uint32_t left = kHeader * rnd(state, 4);
        if (largest > left + kHeader)
            allocate(state, scratch, largest - kHeader - left);
    }
    if (chance(state, 50))
        for (int i = 0; i < 4000 && MemBlockAllocLo(spare(scratch), 0x10000u); ++i)
        {
        }
}

// Frees some real blocks, allocates some small ones, sometimes fills the heap, sometimes makes a free block's size
// not a multiple of 64 (so a remainder below 0x1C, the split threshold, can occur).
void fragment(uint64_t &state, uint32_t scratch)
{
    if (chance(state, 65))
    {
        std::vector<uint32_t> used = blocks(MEM_BLOCK_TYPE_USED);
        for (int i = 0, n = 1 + static_cast<int>(rnd(state, 16)); i < n && !used.empty(); ++i)
        {
            const uint32_t at = rnd(state, static_cast<uint32_t>(used.size()));
            mem<uint32_t>(scratch + kSpare) = used[at] + kHeader;
            MemBlockFree(spare(scratch));
            used.erase(used.begin() + at);
        }
    }
    if (chance(state, 40))
        for (int i = 0, n = 1 + static_cast<int>(rnd(state, 6)); i < n; ++i)
            allocate(state, scratch, chance(state, 50) ? rnd(state, 0x400) : rnd(state, 0x8000));
    if (chance(state, 15))
        fill(state, scratch);
    if (chance(state, 20))
    {
        const std::vector<uint32_t> free = blocks(MEM_BLOCK_TYPE_FREE);
        if (!free.empty())
        {
            const uint32_t block = free[rnd(state, static_cast<uint32_t>(free.size()))];
            if (size(block) > 2 * kHeader)
                mem<uint32_t>(block + 4u) = size(block) - 4u * (1 + rnd(state, 15));
        }
    }
}

// A used block: mostly one next to a free block (after it, before it, or between two), else any.
uint32_t pickUsed(uint64_t &state)
{
    const std::vector<uint32_t> used = blocks(MEM_BLOCK_TYPE_USED);
    if (used.empty())
        return 0;
    std::vector<uint32_t> nextFree, prevFree, both;
    for (uint32_t block : used)
    {
        const bool n = type(next(block)) == MEM_BLOCK_TYPE_FREE, p = type(prev(block)) == MEM_BLOCK_TYPE_FREE;
        if (n)
            nextFree.push_back(block);
        if (p)
            prevFree.push_back(block);
        if (n && p)
            both.push_back(block);
    }
    const uint32_t kind = rnd(state, 10);
    const std::vector<uint32_t> &from = kind < 3 && !nextFree.empty() ? nextFree
                                        : kind < 6 && !prevFree.empty() ? prevFree
                                        : kind < 7 && !both.empty()     ? both
                                                                        : used;
    return from[rnd(state, static_cast<uint32_t>(from.size()))];
}

// A request size of a few bytes more or less than a multiple of 64, to reach the rounding.
uint32_t ragged(uint64_t &state, uint32_t bytes)
{
    const uint32_t kind = rnd(state, 4);
    return kind == 0 ? bytes : kind == 1 ? bytes - rnd(state, 0x3f) : kind == 2 ? bytes + 1u : bytes + rnd(state, 0x40);
}

// The handle under test, holding the block's memory address.
uint32_t handleTo(uint32_t scratch, uint32_t block)
{
    mem<uint32_t>(scratch + kHandle) = block + kHeader;
    return scratch + kHandle;
}

// Makes sizes not multiples of 64 around the block under test (the next free block's, the block's own), so that a
// remainder or shrinkage below the 0x1C threshold, or a new header overlapping the old one, occurs; and sometimes makes
// the block after it the heap's current block.
void raggedAround(uint64_t &state, uint32_t block)
{
    const uint32_t after = next(block);
    if (chance(state, 30) && type(after) == MEM_BLOCK_TYPE_FREE && size(after) > 2 * kHeader)
        mem<uint32_t>(after + 4u) = size(after) - 4u * (1 + rnd(state, 15));
    if (chance(state, 15) && size(block) > 2 * kHeader)
        mem<uint32_t>(block + 4u) = size(block) - 4u * (1 + rnd(state, 15));
    if (chance(state, 25))
        memVars().pCurrent = ptr32<_sMemFreeBlock>(after);
}

uint32_t growSize(uint64_t &state, uint32_t block)
{
    const uint32_t data = size(block) - kHeader;
    const uint32_t after = next(block);
    const uint32_t kind = rnd(state, 100);
    if (kind < 10)
        return rnd(state, data + 1u);
    if (kind < 20)
        return data + rnd(state, 0x40);
    if (type(after) == MEM_BLOCK_TYPE_FREE)
    {
        const uint32_t room = size(after);
        if (kind < 45)
            return ragged(state, data + room);
        if (kind < 70 && room > 2 * kHeader)
            return data + room - kHeader * (1 + rnd(state, std::min(4u, room / kHeader - 1)));
        if (kind < 85)
            return data + room + kHeader * (1 + rnd(state, 2));
        return data + rnd(state, room);
    }
    if (kind < 60)
        return data + kHeader * (1 + rnd(state, 64));
    if (kind < 75)
        return data + largestFree() + kHeader * rnd(state, 3);
    return data + rnd(state, 0x40000);
}

uint32_t shrinkSizeAny(uint64_t &state, uint32_t data);

// Never more than the block holds once rounded: with a ragged size, rounding up would make the shrinkage negative
// and the new free block land on the next block's header (a list that loops for ever).
uint32_t shrinkSize(uint64_t &state, uint32_t block)
{
    const uint32_t data = (size(block) - kHeader) & ~0x3fu;
    return std::min(shrinkSizeAny(state, data), data);
}

uint32_t shrinkSizeAny(uint64_t &state, uint32_t data)
{
    const uint32_t kind = rnd(state, 100);
    if (kind < 15)
        return data;
    if (kind < 25)
        return data - rnd(state, std::min(data, 0x3fu) + 1u);
    if (kind < 35)
        return 0;
    if (kind < 70 && data >= kHeader)
        return ragged(state, data - kHeader * (1 + rnd(state, std::min(8u, data / kHeader))));
    return rnd(state, data + 1u);
}

// A small heap in scratch whose header is a free-typed block and whose chain ends in a block of type 0, so that
// MemBlockFreeAll (which starts at the header) frees the used blocks between and stops.
void fakeHeap(uint64_t &state, uint32_t scratch)
{
    const uint32_t base = scratch + kFakeHeap;
    const uint32_t count = 1 + rnd(state, 8);
    std::vector<uint32_t> chain{base};
    for (uint32_t i = 0; i < count; ++i)
        chain.push_back(base + kHeader * (i + 1));
    const uint32_t end = base + kHeader * (count + 1);
    chain.push_back(end);
    std::vector<uint32_t> ring{base};
    for (size_t i = 0; i < chain.size(); ++i)
    {
        const uint32_t block = chain[i];
        const bool middle = i > 0 && i + 1 < chain.size();
        const uint32_t t = !middle ? (i == 0 ? MEM_BLOCK_TYPE_FREE : MEM_BLOCK_TYPE_SENTINEL)
                                   : (chance(state, 35) ? MEM_BLOCK_TYPE_FREE : MEM_BLOCK_TYPE_USED);
        mem<uint32_t>(block) = t;
        mem<uint32_t>(block + 4u) = kHeader * (1 + rnd(state, 4));
        mem<uint32_t>(block + 8u) = chain[(i + chain.size() - 1) % chain.size()];
        mem<uint32_t>(block + 0xcu) = chain[(i + 1) % chain.size()];
        if (middle && t == MEM_BLOCK_TYPE_FREE)
            ring.push_back(block);
    }
    ring.push_back(end);
    for (size_t i = 0; i < ring.size(); ++i)
    {
        mem<uint32_t>(ring[i] + 0x10u) = ring[(i + ring.size() - 1) % ring.size()];
        mem<uint32_t>(ring[i] + 0x14u) = ring[(i + 1) % ring.size()];
    }
    memClass &vars = memVars();
    vars.pHeader = ptr32<_sMemFreeBlock>(base);
    vars.pCurrent = ptr32<_sMemFreeBlock>(base);
    vars.numFree = rnd(state, 10);
    vars.numUsed = rnd(state, 10);
}

// A fragment pool on the heap (or the weapons' own), with fragments handed out and some given back.
struct Pool
{
    uint32_t handle = 0, pool = 0;
    std::vector<uint32_t> live, dead;
};

uint32_t blockBytes(uint32_t pool) { return mem<uint32_t>(pool + 0x10u) * mem<uint32_t>(pool + 8u) + 0x10u; }

Pool buildPool(uint64_t &state, uint32_t scratch)
{
    Pool p;
    if (chance(state, 15) && mem<uint32_t>(kWeaponVars) != 0u)
        p.handle = kWeaponVars; // Its fragments in use are not known: deletes then pick block slots.
    else
    {
        p.handle = scratch + kPoolHandle;
        const uint32_t perBlock = chance(state, 10) ? 1u : 1u + rnd(state, 12);
        const uint32_t fragSize = chance(state, 70) ? 4u * (1u + rnd(state, 0x50)) : 1u + rnd(state, 0x140);
        const uint32_t maxFrags = chance(state, 10) ? 0u : 1u + rnd(state, 50);
        const uint32_t flags = chance(state, 80) ? rnd(state, 2) : rnd(state, 16);
        mem<uint32_t>(p.handle) = 0;
        // A pool whose first block could not be made (flag 1, limit 0) is still made: it is used as it is.
        if (!MemFragPoolCreate(ptr32<ptr32<_sMemFragPool>>(p.handle), perBlock, maxFrags, fragSize, flags) && mem<uint32_t>(p.handle) == 0u)
            return p;
    }
    p.pool = mem<uint32_t>(p.handle);
    const ptr32<_sMemFragPool> pool(p.pool);
    const uint32_t perBlock = mem<uint32_t>(p.pool + 0x10u);
    for (uint32_t i = 0, n = rnd(state, 3 * std::min(perBlock, 16u) + 4); i < n; ++i)
        if (const uint32_t frag = MemFragCreate(pool).address)
            p.live.push_back(frag);
    const uint32_t keep = rnd(state, 100);
    for (size_t i = 0; i < p.live.size();)
        if (rnd(state, 100) >= keep)
        {
            MemFragDelete(pool, memFragHandle(p.live[i]));
            p.dead.push_back(p.live[i]);
            p.live.erase(p.live.begin() + static_cast<ptrdiff_t>(i));
        }
        else
            ++i;
    for (uint32_t i = 0, n = rnd(state, 4); i < n; ++i)
        if (const uint32_t frag = MemFragCreate(pool).address)
            p.live.push_back(frag);
    return p;
}

std::vector<uint32_t> poolBlocks(uint32_t pool)
{
    std::vector<uint32_t> found;
    for (uint32_t block = mem<uint32_t>(pool + 0x14u); block != 0u && found.size() < 1000; block = mem<uint32_t>(block + 0xcu))
        found.push_back(block);
    return found;
}

// A pool with no blocks, for when none could be made.
uint32_t emptyPool(uint32_t scratch)
{
    const uint32_t pool = scratch + kPoolHandle + 0x40u;
    for (uint32_t i = 0; i < 0x18u; i += 4u)
        mem<uint32_t>(pool + i) = 0;
    return pool;
}

ptr32<_sTask> taskIn(uint32_t scratch, uint32_t index) { return ptr32<_sTask>(scratch + kTasks + index * kTaskSize); }

std::vector<uint32_t> listed()
{
    std::vector<uint32_t> found;
    const uint32_t footer = kTaskVars + 0x24u;
    for (uint32_t task = kTaskVars + 0xcu; found.size() < 200; task = mem<uint32_t>(task + 0x14u))
    {
        found.push_back(task);
        if (task == footer)
            break;
    }
    return found;
}

int32_t taskTime(uint32_t task) { return mem<int32_t>(task + 4u); }

// A time near the list's: equal to a task's (the searches stop at equal times), one either side, or in its range.
int32_t timeNear(uint64_t &state, const std::vector<uint32_t> &tasks)
{
    const uint32_t task = tasks[1 + rnd(state, static_cast<uint32_t>(tasks.size() - 1))];
    const int32_t time = taskTime(task);
    const uint32_t kind = rnd(state, 10);
    if (kind < 4 || time == 0x7fffffff)
        return time == 0x7fffffff && kind < 2 ? time : std::max(0, time - static_cast<int32_t>(kind & 1));
    if (kind < 6)
        return time + 1;
    if (kind < 7)
        return 0;
    // Times stay in 0..0x7fffffff: a negative one would search past the header (time -1) round the list for ever.
    const int64_t near = static_cast<int64_t>(taskTime(tasks[1])) - 50 + rnd(state, 400);
    return static_cast<int32_t>(std::clamp<int64_t>(near, 0, 0x7fffffff));
}

// The dump's list with a few scratch tasks inserted, the search point anywhere in it, or (rarely) an empty list.
std::vector<uint32_t> varyList(uint64_t &state, uint32_t scratch, uint32_t firstFree)
{
    if (chance(state, 8))
        TaskInit();
    for (uint32_t i = 0, n = rnd(state, 6); i < n; ++i)
    {
        const std::vector<uint32_t> tasks = listed();
        const ptr32<_sTask> task = taskIn(scratch, firstFree + i);
        task->flags = rnd(state, 4) & 2u;
        task->time = timeNear(state, tasks);
        task->pFnCallback = ee_fnptr{kVSyncCallback};
        task->pData = nullptr;
        if (chance(state, 50))
            TaskInsert(task);
        else
            TaskCachedInsert(task);
    }
    std::vector<uint32_t> tasks = listed();
    mem<uint32_t>(kTaskVars + 8u) = tasks[rnd(state, static_cast<uint32_t>(tasks.size()))];
    return tasks;
}

} // namespace

// MEM

void memAlloc(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    fragment(state, scratch);
    const std::vector<uint32_t> free = blocks(MEM_BLOCK_TYPE_FREE);
    uint32_t bytes;
    const uint32_t kind = rnd(state, 100);
    if (kind < 12)
        bytes = 0;
    else if (kind < 32)
        bytes = rnd(state, 0x400);
    else if (kind < 47)
        bytes = rnd(state, 0x40000);
    else if (kind < 72 && !free.empty())
    {
        const uint32_t block = free[rnd(state, static_cast<uint32_t>(free.size()))];
        bytes = ragged(state, size(block) - kHeader - kHeader * rnd(state, 2));
    }
    else if (kind < 88)
        bytes = largestFree() - kHeader + kHeader * rnd(state, 3) - (chance(state, 50) ? kHeader : 0u);
    else
        bytes = nextFuzz(state);
    mem<uint32_t>(scratch + kHandle) = nextFuzz(state);
    setArg(ctx, 0, scratch + kHandle);
    setArg(ctx, 1, bytes);
}

void memFree(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    fragment(state, scratch);
    setArg(ctx, 0, handleTo(scratch, pickUsed(state)));
}

void memSet(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    setArg(ctx, 0, scratch + kCopy + rnd(state, 0x800) * 4u + (chance(state, 10) ? 1u + rnd(state, 3) : 0u));
    setArg(ctx, 1, nextFuzz(state));
    setArg(ctx, 2, chance(state, 10) ? rnd(state, 8) : rnd(state, 0x1000));
}

void memCopy(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t bytes = chance(state, 80) ? 4u * rnd(state, 0x400) : rnd(state, 0x1000);
    const uint32_t src = scratch + kCopy + rnd(state, 0x400) * 4u + (chance(state, 10) ? 1u + rnd(state, 3) : 0u);
    uint32_t dst;
    const uint32_t kind = rnd(state, 4);
    if (kind == 0)
        dst = src;
    else if (kind == 1)
        dst = src + 4u * rnd(state, 0x40);
    else if (kind == 2)
        dst = src - 4u * rnd(state, 0x40);
    else
        dst = scratch + kCopy + rnd(state, 0x1000) * 4u;
    setArg(ctx, 0, dst);
    setArg(ctx, 1, src);
    setArg(ctx, 2, bytes);
}

void memGrow(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    fragment(state, scratch);
    const uint32_t block = pickUsed(state);
    raggedAround(state, block);
    setArg(ctx, 0, handleTo(scratch, block));
    setArg(ctx, 1, growSize(state, block));
}

void memShrink(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    fragment(state, scratch);
    const uint32_t block = pickUsed(state);
    raggedAround(state, block);
    setArg(ctx, 0, handleTo(scratch, block));
    setArg(ctx, 1, shrinkSize(state, block));
}

void memResize(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    fragment(state, scratch);
    const uint32_t block = pickUsed(state);
    raggedAround(state, block);
    setArg(ctx, 0, handleTo(scratch, block));
    setArg(ctx, 1, chance(state, 50) ? growSize(state, block) : shrinkSize(state, block));
}

void memInit(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    setArg(ctx, 0, scratch + kFakeHeap + rnd(state, 0x400) * 4u);
    setArg(ctx, 1, chance(state, 50) ? kHeader * rnd(state, 0x1000) : nextFuzz(state));
}

void memCounters(R5900Context *, uint64_t &state, uint32_t)
{
    memClass &vars = memVars();
    if (chance(state, 30))
        vars.pPool = nullptr;
    if (chance(state, 50))
    {
        vars.numFree = rnd(state, 100);
        vars.numUsed = rnd(state, 1000);
        vars.memUsed = nextFuzz(state);
        vars.memFree = chance(state, 50) ? rnd(state, 0x2000) : nextFuzz(state);
    }
}

void memFreeAll(R5900Context *, uint64_t &state, uint32_t scratch)
{
    if (chance(state, 25))
        fragment(state, scratch);
    else
        fakeHeap(state, scratch);
}

void memFragmented(R5900Context *, uint64_t &state, uint32_t scratch) { fragment(state, scratch); }

// MEMFRAG

void fragPool(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const Pool p = buildPool(state, scratch);
    if (chance(state, 15))
        fill(state, scratch);
    setArg(ctx, 0, p.pool);
}

void fragPoolHandle(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const Pool p = buildPool(state, scratch);
    setArg(ctx, 0, p.handle);
}

void fragPoolCreate(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (chance(state, 30))
        fragment(state, scratch);
    if (chance(state, 10))
        fill(state, scratch);
    setArg(ctx, 0, scratch + kPoolHandle);
    setArg(ctx, 1, chance(state, 10) ? 0u : 1u + rnd(state, 16));
    setArg(ctx, 2, chance(state, 10) ? 0u : rnd(state, 100));
    setArg(ctx, 3, chance(state, 10) ? nextFuzz(state) % 0x100000u : rnd(state, 0x400));
    setArg(ctx, 4, chance(state, 70) ? rnd(state, 2) : nextFuzz(state));
}

void fragDelete(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const Pool p = buildPool(state, scratch);
    const std::vector<uint32_t> blocksOf = p.pool ? poolBlocks(p.pool) : std::vector<uint32_t>{};
    uint32_t frag = scratch + kCopy + rnd(state, 0x100) * 4u;
    const uint32_t kind = rnd(state, 100);
    if (kind < 70 && !p.live.empty())
        frag = p.live[rnd(state, static_cast<uint32_t>(p.live.size()))];
    else if (kind < 76 && !p.dead.empty())
        frag = p.dead[rnd(state, static_cast<uint32_t>(p.dead.size()))];
    else if (kind < 92 && !blocksOf.empty())
    {
        // A block slot (the weapons' pool), the header, or exactly the end.
        const uint32_t block = blocksOf[rnd(state, static_cast<uint32_t>(blocksOf.size()))];
        const uint32_t slots = mem<uint32_t>(block + 4u);
        const uint32_t which = rnd(state, 4);
        frag = which == 0 ? block : which == 1 ? block + blockBytes(p.pool) - (chance(state, 50) ? 0u : 1u)
                                               : block + 0x10u + mem<uint32_t>(p.pool + 8u) * rnd(state, std::max(slots, 1u));
    }
    else if (kind < 96)
        frag = nextFuzz(state) & 0x01fffffcu;
    setArg(ctx, 0, p.pool ? p.pool : emptyPool(scratch));
    setArg(ctx, 1, frag);
}

void fragBlock(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const Pool p = buildPool(state, scratch);
    const std::vector<uint32_t> blocksOf = p.pool ? poolBlocks(p.pool) : std::vector<uint32_t>{};
    uint32_t block;
    if (chance(state, 75) && !blocksOf.empty())
        block = blocksOf[rnd(state, static_cast<uint32_t>(blocksOf.size()))];
    else
    {
        // A heap block of the pool's shape that is not in its list.
        MemBlockAllocLo(spare(scratch), p.pool ? blockBytes(p.pool) : 0x40u);
        block = mem<uint32_t>(scratch + kSpare);
    }
    if (!block)
        block = pickUsed(state) + kHeader;
    setArg(ctx, 0, p.pool ? p.pool : emptyPool(scratch));
    setArg(ctx, 1, block);
}

// HEAP

void heapInit(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    fragment(state, scratch);
    const uint32_t kind = rnd(state, 10);
    setArg(ctx, 0, kind < 2 ? rnd(state, 0x40) : kind < 6 ? rnd(state, 0x10000) : kind < 9 ? largestFree() - kHeader - rnd(state, 0x80) + rnd(state, 0x80) : nextFuzz(state));
}

void heapTerm(R5900Context *, uint64_t &state, uint32_t scratch)
{
    fragment(state, scratch);
    if (chance(state, 40))
        MemBlockAllocLo(ptr32<memHandle>(kHeapVars), rnd(state, 0x4000));
}

void heapVars(R5900Context *ctx, uint64_t &state, uint32_t)
{
    _sHeapVars &heap = mem<_sHeapVars>(kHeapVars);
    if (chance(state, 60))
        heap.pCurrent = ptr32<uint8>(heap.pStart.address + rnd(state, heap.pEnd.address - heap.pStart.address + 0x10u));
    if (chance(state, 15))
        heap.pEnd = ptr32<uint8>(chance(state, 50) ? heap.pCurrent.address : nextFuzz(state));
    if (chance(state, 15))
        heap.pStart = ptr32<uint8>(nextFuzz(state));
    const uint32_t room = heap.pEnd.address - heap.pCurrent.address;
    const uint32_t kind = rnd(state, 10);
    setArg(ctx, 0, kind < 3 ? rnd(state, 0x200) : kind < 5 ? room - 1u + rnd(state, 3) : kind < 6 ? room - rnd(state, 4) : kind < 8 ? rnd(state, room + 1u) : nextFuzz(state));
}

// TASK

void taskSet(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    setArg(ctx, 0, taskIn(scratch, rnd(state, 30)).address);
    for (int i = 1; i < 4; ++i)
        setArg(ctx, i, nextFuzz(state));
}

void taskList(R5900Context *, uint64_t &state, uint32_t scratch) { varyList(state, scratch, 0); }

void taskInsert(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const std::vector<uint32_t> tasks = varyList(state, scratch, 1);
    const ptr32<_sTask> task = taskIn(scratch, 0);
    task->flags = chance(state, 70) ? 0u : nextFuzz(state);
    task->time = chance(state, 5) ? 0x7fffffff : timeNear(state, tasks);
    setArg(ctx, 0, task.address);
}

void taskDelete(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const std::vector<uint32_t> tasks = varyList(state, scratch, 1);
    uint32_t task;
    const uint32_t kind = rnd(state, 100);
    if (kind < 75 && tasks.size() > 2)
        task = tasks[1 + rnd(state, static_cast<uint32_t>(tasks.size() - 2))];
    else if (kind < 92)
    {
        task = taskIn(scratch, 0).address; // Not in the list: flags clear, or set with stale links into it.
        mem<uint32_t>(task) = rnd(state, 2) * 2u;
    }
    else
        task = tasks[chance(state, 50) ? 0 : tasks.size() - 1];
    setArg(ctx, 0, task);
}

void taskProcess(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (chance(state, 40))
    {
        // The dump's own tasks (the race's handlers), due over the next few frames. Sound off: offline it would wait.
        mem<uint8_t>(kSfxInitOk) = 0;
        const int32_t first = taskTime(mem<uint32_t>(kTaskVars + 0x20u));
        setArg(ctx, 0, static_cast<uint32_t>(chance(state, 10) ? first - 1 : first + static_cast<int32_t>(rnd(state, 80))));
        return;
    }
    // Scratch tasks over six time steps; callbacks that free the scratch heap, do nothing, delete another task, or
    // insert a further task (each its own, which may fall due in the same call).
    TaskInit();
    const int32_t start = static_cast<int32_t>(rnd(state, 1000));
    const uint32_t count = 1 + rnd(state, 10);
    uint32_t extra = count;
    for (uint32_t i = 0; i < count; ++i)
    {
        const ptr32<_sTask> task = taskIn(scratch, i);
        task->flags = 0;
        task->time = start + static_cast<int32_t>(rnd(state, 6));
        task->pData = ptr32<void>(nextFuzz(state));
        const uint32_t kind = rnd(state, 6);
        uint32_t callback = kind == 0 ? kHeapFree : kind == 1 ? kVSyncCallback : kind == 2 ? kMemOutOfMemory : kTaskDelete;
        if (kind == 3)
            task->pData = taskIn(scratch, rnd(state, count)).cast<void>();
        else if (kind >= 4 && extra < 30)
        {
            const ptr32<_sTask> later = taskIn(scratch, extra++);
            later->flags = 0;
            later->time = start + static_cast<int32_t>(rnd(state, 8));
            later->pFnCallback = ee_fnptr{chance(state, 50) ? kHeapFree : kVSyncCallback};
            later->pData = ptr32<void>(nextFuzz(state));
            callback = kind == 4 ? kTaskCachedInsert : kTaskInsert;
            task->pData = later.cast<void>();
        }
        task->pFnCallback = ee_fnptr{callback};
        if (chance(state, 50))
            TaskInsert(task);
        else
            TaskCachedInsert(task);
    }
    setArg(ctx, 0, static_cast<uint32_t>(start - 1 + static_cast<int32_t>(rnd(state, 8))));
}

} // namespace fuzz

} // namespace rc

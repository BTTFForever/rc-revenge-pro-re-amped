// Decompiled from MEM.c: the game's heap.
//
// The heap is one pool of blocks, each with a 64-byte header in front of its memory. Every block is linked to its
// neighbours in address order (pPrev, pNext), and the free ones also form a ring in address order through the
// sentinel memVarG.pHeader (pPrevFree, pNextFree). A handle is the variable that holds a block's address: freeing
// clears it. Block sizes include the header and are multiples of 64, so the "is anything left over" tests below
// (against 0x1C) only tell an exact fit from a remainder of at least 64.
//
// The functions are written from the instructions in their order of loads and stores, since a block header made at a
// new place can overlap the one it replaces.

#include <cstring>

#include "mem_fuzz.h"

namespace rc {

namespace {

constexpr uint32_t kHeaderBytes = 0x40u;
constexpr uint32_t kSmallestSplit = 0x1Cu; // A block is split only if the rest can hold more than its own header.

ptr32<_sMemFreeBlock> asFree(ptr32<_sMemBlock> block) { return block.cast<_sMemFreeBlock>(); }
ptr32<_sMemBlock> asBlock(ptr32<_sMemFreeBlock> block) { return block.cast<_sMemBlock>(); }

} // namespace

// What the game does when the heap is full: nothing, and the allocation reports failure.
bool_ MemOutOfMemory()
{
    return 0;
}

// Allocates from the first free block that is large enough, which is the lowest in memory.
bool_ MemBlockAllocLo(ptr32<memHandle> pHandle, uint32 size)
{
    memClass &mem = G(memVarG);
    const uint32_t rounded = (size + 0x3Fu) & ~0x3Fu;
    const uint32_t needed = rounded + kHeaderBytes;

    ptr32<_sMemFreeBlock> block = mem.pHeader->pNextFree;
    while (block->size < needed)
    {
        block = block->pNextFree;
        if (block == mem.pHeader)
            return MemOutOfMemory();
    }

    if (block->size < needed + kSmallestSplit)
    {
        // Too little would be left over: the whole block is used.
        block->pPrevFree->pNextFree = block->pNextFree;
        mem.numFree = mem.numFree - 1;
        block->type = MEM_BLOCK_TYPE_USED;
        block->pNextFree->pPrevFree = block->pPrevFree;
        mem.pCurrent = block->pNextFree;
    }
    else
    {
        // The rest of the block becomes a new free block, taking this one's place in the free ring.
        const ptr32<_sMemFreeBlock> rest(block.address + needed);
        mem.pCurrent = rest;
        rest->pPrev = asBlock(block);
        rest->pPrevFree = block->pPrevFree;
        rest->pNext = block->pNext;
        rest->pNextFree = block->pNextFree;
        const ptr32<_sMemFreeBlock> nextFree = block->pNextFree;
        block->pNext->pPrev = asBlock(rest);
        nextFree->pPrevFree = rest;
        rest->size = block->size - needed;
        block->type = MEM_BLOCK_TYPE_USED;
        block->pPrevFree->pNextFree = rest;
        block->size = needed;
        rest->type = MEM_BLOCK_TYPE_FREE;
        block->pNext = asBlock(rest);
    }

    mem.numUsed = mem.numUsed + 1;
    *pHandle = memHandle(block.address + kHeaderBytes);
    mem.memUsed = mem.memUsed + block->size;
    mem.memFree = mem.memFree - block->size;
    return 1;
}

// Frees a block, joining it to a free neighbour on either side, and clears the handle.
void MemBlockFree(ptr32<memHandle> pHandle)
{
    memClass &mem = G(memVarG);
    const ptr32<_sMemFreeBlock> block(pHandle->address - kHeaderBytes);
    const ptr32<_sMemFreeBlock> prev = asFree(block->pPrev);
    const ptr32<_sMemFreeBlock> next = asFree(block->pNext);
    mem.memUsed = mem.memUsed - block->size;
    mem.memFree = mem.memFree + block->size;

    ptr32<_sMemFreeBlock> freed = block;
    if (prev->type == MEM_BLOCK_TYPE_FREE)
    {
        freed = prev;
        if (next->type == MEM_BLOCK_TYPE_FREE)
        {
            // Free on both sides: all three become the block before.
            prev->size = prev->size + block->size + next->size;
            prev->pNextFree = next->pNextFree;
            prev->pNext = next->pNext;
            mem.numFree = mem.numFree - 1;
            next->pNext->pPrev = asBlock(prev);
            next->pNextFree->pPrevFree = prev;
            mem.pCurrent = prev;
        }
        else
        {
            prev->pNext = asBlock(next);
            next->pPrev = asBlock(prev);
            prev->size = prev->size + block->size;
        }
    }
    else if (next->type == MEM_BLOCK_TYPE_FREE)
    {
        // The block takes over the free block after it, and its place in the free ring.
        block->pNextFree = next->pNextFree;
        block->pPrevFree = next->pPrevFree;
        block->pNext = next->pNext;
        block->size = block->size + next->size;
        next->pPrevFree->pNextFree = block;
        next->pNext->pPrev = asBlock(block);
        next->pNextFree->pPrevFree = block;
        mem.pCurrent = block;
    }
    else
    {
        // Used on both sides: find the nearest free blocks, or the sentinel, to link between.
        ptr32<_sMemFreeBlock> before = prev;
        while (before->type == MEM_BLOCK_TYPE_USED)
            before = asFree(before->pPrev);
        ptr32<_sMemFreeBlock> after = next;
        while (after->type == MEM_BLOCK_TYPE_USED)
            after = asFree(after->pNext);
        block->pNextFree = after;
        mem.numFree = mem.numFree + 1;
        block->pPrevFree = before;
        before->pNextFree = block;
        after->pPrevFree = block;
    }

    mem.numUsed = mem.numUsed - 1;
    freed->type = MEM_BLOCK_TYPE_FREE;
    *pHandle = nullptr;
}

// Fills memory with a 32-bit value. size is in bytes.
void MemBlockSet32(memHandle handle, uint32 value, uint32 size)
{
    const ptr32<uint32> words = handle.cast<uint32>();
    for (uint32_t i = 0; i < size / 4u; ++i)
        words[i] = value;
}

namespace {

// Word copies go through memcpy: the copy functions take any address, and an unaligned word reads as the
// recompiled code reads it.
uint32_t load32(uint32_t address)
{
    uint32_t value;
    std::memcpy(&value, guest_host(address), 4u);
    return value;
}

void store32(uint32_t address, uint32_t value) { std::memcpy(guest_host(address), &value, 4u); }

uint32_t wantedSize(uint32_t size) { return ((size + 0x3Fu) & ~0x3Fu) + kHeaderBytes; }

} // namespace

// Copies size / 4 words upwards.
void MemBlockCopy32(memHandle dstHandle, memHandle srcHandle, uint32 size)
{
    uint32_t dst = dstHandle.address, src = srcHandle.address;
    for (uint32_t words = size >> 2; words != 0u; --words, dst += 4u, src += 4u)
        store32(dst, load32(src));
}

// A copy that may overlap: upwards when the destination is lower, otherwise downwards from the end. The downward copy
// starts at the word just past the end and ends at the first word, so it copies one word more than size / 4.
void MemBlockMove32(memHandle dstHandle, memHandle srcHandle, uint32 size)
{
    if (dstHandle.address < srcHandle.address)
    {
        MemBlockCopy32(dstHandle, srcHandle, size);
        return;
    }
    uint32_t src = srcHandle.address + size, dst = dstHandle.address + size;
    for (uint32_t words = size >> 2; words != 0u; --words, dst -= 4u, src -= 4u)
        store32(dst, load32(src));
    store32(dst, load32(src));
}

// Grows a block in place into the free block after it, or moves it to a new block when the one after is in use.
// A free block after it that is too small leaves the block as it is, and still reports success.
bool_ MemBlockGrow(ptr32<memHandle> pHandle, uint32 size)
{
    memClass &mem = G(memVarG);
    const uint32_t rounded = (size + 0x3Fu) & ~0x3Fu;
    const uint32_t wanted = wantedSize(size);
    const memHandle handle = *pHandle;
    const ptr32<_sMemFreeBlock> block(handle.address - kHeaderBytes);
    if (block->size >= wanted)
        return 1;
    const ptr32<_sMemFreeBlock> next = asFree(block->pNext);
    const uint32_t growth = wanted - block->size;
    if (next->type != MEM_BLOCK_TYPE_FREE)
    {
        // The original's two locals: the new handle at sp+0, the old one at sp+4 (freed through its address).
        GuestFrame frame(0x50);
        const ptr32<memHandle> newHandle = frame.local<memHandle>(0), oldHandle = frame.local<memHandle>(4);
        *oldHandle = handle;
        *newHandle = nullptr;
        const bool_ allocated = MemBlockAllocLo(newHandle, rounded);
        if (allocated != 1)
            return allocated;
        MemBlockCopy32(*newHandle, *oldHandle, block->size - kHeaderBytes);
        MemBlockFree(oldHandle);
        *pHandle = *newHandle;
        return 1;
    }
    const uint32_t nextSize = next->size;
    if (nextSize < growth)
        return 1;
    if (nextSize < growth + kSmallestSplit)
    {
        // The whole free block is taken.
        const ptr32<_sMemFreeBlock> prevFree = next->pPrevFree;
        const uint32_t newSize = wanted + (nextSize - growth);
        const ptr32<_sMemFreeBlock> nextFree = next->pNextFree;
        const ptr32<_sMemBlock> after = next->pNext;
        prevFree->pNextFree = nextFree;
        block->pNext = after;
        const ptr32<_sMemFreeBlock> ringNext = next->pNextFree;
        const ptr32<_sMemFreeBlock> ringPrev = next->pPrevFree;
        const ptr32<_sMemFreeBlock> current = mem.pCurrent;
        ringNext->pPrevFree = ringPrev;
        after->pPrev = asBlock(block);
        block->size = newSize;
        if (current == next)
            mem.pCurrent = nextFree;
        mem.numFree = mem.numFree - 1;
        mem.memUsed = mem.memUsed + next->size;
        mem.memFree = mem.memFree - next->size;
        return 1;
    }
    // The rest of the free block becomes a new free block after the grown one.
    const ptr32<_sMemFreeBlock> rest(block.address + wanted);
    const uint32_t restSize = nextSize - growth;
    rest->pPrev = asBlock(block);
    const ptr32<_sMemFreeBlock> nextFree = next->pNextFree;
    const ptr32<_sMemBlock> following = block->pNext;
    const ptr32<_sMemFreeBlock> prevFree = next->pPrevFree;
    const ptr32<_sMemBlock> after = following->pNext;
    rest->pPrevFree = prevFree;
    rest->pNext = after;
    after->pPrev = asBlock(rest);
    rest->pNextFree = nextFree;
    const ptr32<_sMemBlock> restPrev = rest->pPrev;
    rest->size = restSize;
    prevFree->pNextFree = rest;
    const ptr32<_sMemFreeBlock> current = mem.pCurrent;
    block->size = wanted;
    rest->type = MEM_BLOCK_TYPE_FREE;
    restPrev->pNext = asBlock(rest);
    nextFree->pPrevFree = rest;
    if (current == next)
        mem.pCurrent = rest;
    mem.memUsed = mem.memUsed + growth;
    mem.memFree = mem.memFree - growth;
    return 1;
}

// Gives the end of a block back: to the free block after it, or as a new free block if at least a header's worth is
// left. The totals change even when nothing is given back.
void MemBlockShrink(ptr32<memHandle> pHandle, uint32 size)
{
    memClass &mem = G(memVarG);
    const uint32_t wanted = wantedSize(size);
    const ptr32<_sMemFreeBlock> block(pHandle->address - kHeaderBytes);
    const uint32_t shrinkage = block->size - wanted;
    const uint32_t memUsed = mem.memUsed, memFree = mem.memFree;
    const ptr32<_sMemFreeBlock> next = asFree(block->pNext);
    mem.memUsed = memUsed - shrinkage;
    const eMemBlockType nextType = next->type;
    mem.memFree = memFree + shrinkage;
    const ptr32<_sMemFreeBlock> freed(block.address + wanted);

    if (nextType == MEM_BLOCK_TYPE_FREE)
    {
        // The free block after it moves down to start where this one now ends.
        const ptr32<_sMemBlock> nextPrev = next->pPrev;
        const ptr32<_sMemFreeBlock> nextPrevFree = next->pPrevFree;
        freed->pPrev = nextPrev;
        freed->pPrevFree = nextPrevFree;
        const ptr32<_sMemBlock> after = next->pNext;
        const ptr32<_sMemFreeBlock> nextFree = next->pNextFree;
        const uint32_t blockSize = block->size;
        const uint32_t nextSize = next->size;
        freed->pNext = after;
        freed->pNextFree = nextFree;
        after->pPrev = asBlock(freed);
        nextFree->pPrevFree = freed;
        const ptr32<_sMemBlock> freedPrev = freed->pPrev;
        const ptr32<_sMemFreeBlock> freedPrevFree = freed->pPrevFree;
        freed->size = nextSize + blockSize - wanted;
        const ptr32<_sMemFreeBlock> current = mem.pCurrent;
        freedPrevFree->pNextFree = freed;
        block->size = wanted;
        freed->type = nextType;
        freedPrev->pNext = asBlock(freed);
        if (current == next)
            mem.pCurrent = next->pNextFree; // Read again: the new header may lie over the old one.
        return;
    }
    if (shrinkage < kSmallestSplit)
        return;

    // A new free block, linked into the free ring between the nearest free blocks (or the sentinel) on either side.
    ptr32<_sMemFreeBlock> before = asFree(block->pPrev);
    freed->pPrev = asBlock(block);
    ptr32<_sMemFreeBlock> after = next;
    const uint32_t freedSize = block->size - wanted;
    const ptr32<_sMemBlock> blockNext = block->pNext;
    const eMemBlockType beforeType = before->type;
    freed->pNext = blockNext;
    block->pNext = asBlock(freed);
    freed->size = freedSize;
    const ptr32<_sMemBlock> freedNext = freed->pNext;
    block->size = wanted;
    freedNext->pPrev = asBlock(freed);
    if (beforeType == MEM_BLOCK_TYPE_USED)
        do
            before = asFree(before->pPrev);
        while (before->type == MEM_BLOCK_TYPE_USED);
    const uint32_t numFree = mem.numFree;
    if (nextType == MEM_BLOCK_TYPE_USED)
        do
            after = asFree(after->pNext);
        while (after->type == MEM_BLOCK_TYPE_USED);
    freed->pNextFree = after;
    freed->pPrevFree = before;
    before->pNextFree = freed;
    mem.numFree = numFree + 1;
    freed->type = MEM_BLOCK_TYPE_FREE;
    after->pPrevFree = freed;
}

// Gives the start of a block back: the header moves up and the memory before it joins the free block before, or
// becomes a new free block if at least a header's worth is freed. The handle moves with the block; the contents do not.
void MemBlockCompress(ptr32<memHandle> pHandle, uint32 size)
{
    memClass &mem = G(memVarG);
    const uint32_t wanted = wantedSize(size);
    const uint32_t handle = pHandle->address;
    const ptr32<_sMemFreeBlock> block(handle - kHeaderBytes);
    ptr32<_sMemFreeBlock> prev = asFree(block->pPrev);
    const uint32_t compression = block->size - wanted;
    const ptr32<_sMemFreeBlock> moved(block.address + compression);

    if (prev->type == MEM_BLOCK_TYPE_FREE)
    {
        MemBlockMove32(moved.cast<void>(), block.cast<void>(), kHeaderBytes);
        const uint32_t prevSize = prev->size;
        prev->pNext = asBlock(moved);
        prev->size = prevSize + compression;
        *pHandle = memHandle(handle + compression);
        const uint32_t movedSize = moved->size;
        const ptr32<_sMemBlock> after = moved->pNext;
        moved->size = movedSize - compression;
        after->pPrev = asBlock(moved);
        const uint32_t memUsed = mem.memUsed, memFree = mem.memFree;
        mem.memUsed = memUsed - compression;
        mem.memFree = memFree + compression;
        return;
    }
    if (compression < kSmallestSplit)
        return;

    ptr32<_sMemFreeBlock> after = asFree(block->pNext);
    MemBlockMove32(moved.cast<void>(), block.cast<void>(), kHeaderBytes);
    const uint32_t freedSize = block->size - wanted;
    const ptr32<_sMemBlock> movedNext = moved->pNext;
    block->type = MEM_BLOCK_TYPE_FREE;
    moved->pPrev = asBlock(block);
    block->size = freedSize;
    movedNext->pPrev = asBlock(moved);
    const eMemBlockType prevType = prev->type;
    moved->size = wanted;
    block->pNext = asBlock(moved);
    const uint32_t memUsed = mem.memUsed, memFree = mem.memFree;
    const eMemBlockType afterType = after->type;
    const uint32_t numFree = mem.numFree;
    if (prevType == MEM_BLOCK_TYPE_USED)
        do
            prev = asFree(prev->pPrev);
        while (prev->type == MEM_BLOCK_TYPE_USED);
    if (afterType == MEM_BLOCK_TYPE_USED)
        do
            after = asFree(after->pNext);
        while (after->type == MEM_BLOCK_TYPE_USED);
    block->pNextFree = after;
    block->pPrevFree = prev;
    after->pPrevFree = block;
    prev->pNextFree = block;
    mem.numFree = numFree + 1;
    mem.memUsed = memUsed - compression;
    *pHandle = memHandle(handle + compression);
    mem.memFree = memFree + compression;
}

// Makes the pool at addr one free block, ringed with the sentinel.
void MemBlockInit(uint32 addr, uint32 size)
{
    memClass &mem = G(memVarG);
    _sMemFreeBlock &sentinel = G(memSentinelG);
    const ptr32<_sMemFreeBlock> sentinelAddress(guest_addr(&sentinel));
    const ptr32<_sMemFreeBlock> pool(addr);
    mem.size = size;
    mem.numFree = 1;
    mem.numUsed = 0;
    mem.memUsed = 0;
    mem.memFree = size;
    sentinel.pPrev = pool.cast<_sMemBlock>();
    sentinel.pNext = pool.cast<_sMemBlock>();
    sentinel.pPrevFree = pool;
    sentinel.pNextFree = pool;
    sentinel.type = MEM_BLOCK_TYPE_SENTINEL;
    sentinel.size = 0;
    mem.pCurrent = sentinelAddress;
    mem.pPool = pool.cast<void>();
    pool->type = MEM_BLOCK_TYPE_FREE;
    pool->size = size;
    pool->pNextFree = sentinelAddress;
    mem.pHeader = sentinelAddress;
    pool->pPrev = asBlock(sentinelAddress);
    pool->pNext = asBlock(sentinelAddress);
    pool->pPrevFree = sentinelAddress;
}

// Forgets the pool (the used and free totals are left).
void MemBlockTerm()
{
    memClass &mem = G(memVarG);
    if (!mem.pPool)
        return;
    mem.pPool = nullptr;
    mem.size = 0;
    mem.pHeader = nullptr;
    mem.pCurrent = nullptr;
    mem.numFree = 0;
    mem.numUsed = 0;
}

// Allocates from the end of the highest free block that is large enough.
bool_ MemBlockAllocHi(ptr32<memHandle> pHandle, uint32 size)
{
    memClass &mem = G(memVarG);
    const ptr32<_sMemFreeBlock> header = mem.pHeader;
    const uint32_t needed = wantedSize(size);
    ptr32<_sMemFreeBlock> block = header->pPrevFree;
    uint32_t blockSize;
    while ((blockSize = block->size) < needed)
    {
        block = block->pPrevFree;
        if (block == header)
            return MemOutOfMemory();
    }

    ptr32<_sMemFreeBlock> used = block;
    if (blockSize < needed + kSmallestSplit)
    {
        // The whole block is used.
        const uint32_t numFree = mem.numFree;
        const ptr32<_sMemFreeBlock> nextFree = block->pNextFree;
        const ptr32<_sMemFreeBlock> prevFree = block->pPrevFree;
        mem.numFree = numFree - 1;
        prevFree->pNextFree = nextFree;
        block->type = MEM_BLOCK_TYPE_USED;
        const ptr32<_sMemFreeBlock> ringNext = block->pNextFree;
        ringNext->pPrevFree = block->pPrevFree;
        mem.pCurrent = block->pNextFree;
    }
    else
    {
        // The end of the block is split off; the free block keeps its place.
        used = ptr32<_sMemFreeBlock>(block.address + blockSize - needed);
        const ptr32<_sMemBlock> blockNext = block->pNext;
        used->pPrev = asBlock(block);
        used->pNext = blockNext;
        used->size = needed;
        const ptr32<_sMemBlock> after = block->pNext;
        const uint32_t freeSize = block->size;
        used->type = MEM_BLOCK_TYPE_USED;
        after->pPrev = asBlock(used);
        block->size = freeSize - needed;
        block->type = MEM_BLOCK_TYPE_FREE;
        mem.pCurrent = block;
        block->pNext = asBlock(used);
    }

    const uint32_t numUsed = mem.numUsed, memUsed = mem.memUsed, memFree = mem.memFree;
    mem.numUsed = numUsed + 1;
    *pHandle = memHandle(used.address + kHeaderBytes);
    mem.memUsed = memUsed + used->size;
    mem.memFree = memFree - used->size;
    return 1;
}

// Grows or shrinks a block to the size, rounded to 64 bytes.
bool_ MemBlockResize(ptr32<memHandle> pHandle, uint32 size)
{
    const uint32_t rounded = (size + 0x3Fu) & ~0x3Fu;
    const ptr32<_sMemBlock> block(pHandle->address - kHeaderBytes);
    if (block->size - kHeaderBytes < rounded)
    {
        const bool_ grown = MemBlockGrow(pHandle, rounded);
        return grown == 1 ? 1 : grown;
    }
    MemBlockShrink(pHandle, rounded);
    return 1;
}

// Frees every used block. It walks from the sentinel itself, whose type is neither free nor used, so in the game it
// returns at once.
void MemBlockFreeAll()
{
    memClass &mem = G(memVarG);
    GuestFrame frame(0x50);
    const ptr32<memHandle> handle = frame.local<memHandle>(0);
    for (;;)
    {
        ptr32<_sMemFreeBlock> block = mem.pHeader;
        eMemBlockType type = block->type;
        while (type == MEM_BLOCK_TYPE_FREE)
        {
            block = asFree(block->pNext);
            type = block->type;
        }
        if (type != MEM_BLOCK_TYPE_USED)
            return;
        *handle = memHandle(block.address + kHeaderBytes);
        MemBlockFree(handle);
    }
}

uint32 MemGetUsed() { return G(memVarG).memUsed; }

// Free memory less a header for each free block.
uint32 MemGetFree()
{
    const memClass &mem = G(memVarG);
    return mem.memFree - (mem.numFree << 6);
}

// The largest free block's size less its header. The ring is walked from the sentinel's next, the sentinel included
// when the ring is empty.
uint32 MemGetLargestFree()
{
    const ptr32<_sMemFreeBlock> header = G(memVarG).pHeader;
    ptr32<_sMemFreeBlock> block = header->pNextFree;
    uint32_t largest = 0;
    do
    {
        const uint32_t size = block->size;
        block = block->pNextFree;
        if (largest < size)
            largest = size;
    } while (block != header);
    return largest - kHeaderBytes;
}

} // namespace rc

RC_DECOMP_FN(MemOutOfMemory, 0x194a38)
RC_DECOMP_FN_SETUP_HEAP(MemBlockAllocLo, 0x1937f8, rc::fuzz::memAlloc)
RC_DECOMP_FN_SETUP_HEAP(MemBlockFree, 0x193a00, rc::fuzz::memFree)
RC_DECOMP_FN_SETUP(MemBlockSet32, 0x1946d0, rc::fuzz::memSet)
RC_DECOMP_FN_SETUP(MemBlockCopy32, 0x194748, rc::fuzz::memCopy)
RC_DECOMP_FN_SETUP(MemBlockMove32, 0x1947e8, rc::fuzz::memCopy)
RC_DECOMP_FN_SETUP_HEAP(MemBlockGrow, 0x193c20, rc::fuzz::memGrow)
RC_DECOMP_FN_SETUP_HEAP(MemBlockShrink, 0x193dd8, rc::fuzz::memShrink)
RC_DECOMP_FN_SETUP_HEAP(MemBlockCompress, 0x193fd0, rc::fuzz::memShrink)
RC_DECOMP_FN_SETUP_HEAP(MemBlockResize, 0x1944c8, rc::fuzz::memResize)
RC_DECOMP_FN_SETUP(MemBlockInit, 0x194238, rc::fuzz::memInit)
RC_DECOMP_FN_SETUP(MemBlockTerm, 0x1942a0, rc::fuzz::memCounters)
RC_DECOMP_FN_SETUP_HEAP(MemBlockAllocHi, 0x1942d0, rc::fuzz::memAlloc)
RC_DECOMP_FN_SETUP_HEAP(MemBlockFreeAll, 0x194538, rc::fuzz::memFreeAll)
RC_DECOMP_FN_SETUP(MemGetUsed, 0x194950, rc::fuzz::memCounters)
RC_DECOMP_FN_SETUP(MemGetFree, 0x194960, rc::fuzz::memCounters)
RC_DECOMP_FN_SETUP_HEAP(MemGetLargestFree, 0x194980, rc::fuzz::memFragmented)

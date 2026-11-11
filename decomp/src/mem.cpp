// Decompiled from MEM.c: the game's heap. Only what the collision code needs so far.
//
// The heap is one pool of blocks, each with a 64-byte header in front of its memory. Every block is linked to its
// neighbours in address order (pPrev, pNext), and the free ones also form a ring in address order through the
// sentinel memVarG.pHeader (pPrevFree, pNextFree). A handle is the variable that holds a block's address: freeing
// clears it.

#include "rc_decomp.h"

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

} // namespace rc

RC_DECOMP_FN(MemOutOfMemory, 0x194a38)
RC_DECOMP_FN_NOFUZZ(MemBlockAllocLo, 0x1937f8)
RC_DECOMP_FN_NOFUZZ(MemBlockFree, 0x193a00)
RC_DECOMP_FN_NOFUZZ(MemBlockSet32, 0x1946d0)

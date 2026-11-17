// Decompiled from MEMFRAG.c: pools of equal-sized fragments (the weapons use one). A pool owns a list of blocks
// allocated from the heap, each a 16-byte header followed by fragsPerBlock fragments. A block hands out fragments
// in order (insertPoint) and then reuses freed ones through a free list threaded through the fragments themselves.
// The pool's own fragsUsed is never counted up, so maxFrags only stops a pool whose limit is 0.

#include "mem_fuzz.h"

namespace rc {

bool_ MemBlockAllocLo(ptr32<memHandle> pHandle, uint32 size); // mem.cpp
void MemBlockFree(ptr32<memHandle> pHandle);

namespace {

constexpr uint32_t kBlockHeader = 0x10u;

uint32_t blockBytes(const _sMemFragPool &pool) { return pool.fragsPerBlock * pool.fragSize + kBlockHeader; }

// A new block at the head of the pool's list, from the heap through the handle given (the caller's stack local).
ptr32<_sMemFragBlock> newBlock(ptr32<_sMemFragPool> pPool, ptr32<ptr32<_sMemFragBlock>> local)
{
    *local = nullptr;
    if (!(pPool->fragsUsed < pPool->maxFrags))
        return nullptr;
    if (!MemBlockAllocLo(local.cast<memHandle>(), blockBytes(*pPool)))
        return nullptr;
    const ptr32<_sMemFragBlock> block = *local;
    const ptr32<_sMemFragBlock> first = pPool->pFirstBlock;
    pPool->pFirstBlock = block;
    block->pNextBlock = first;
    block->fragsUsed = 0;
    block->insertPoint = 0;
    block->pFreeFrag = nullptr;
    return block;
}

// Takes the block out of the pool's list, if it is there.
void unlinkBlock(ptr32<_sMemFragPool> pPool, ptr32<_sMemFragBlock> pBlock)
{
    ptr32<_sMemFragBlock> prev = pPool->pFirstBlock;
    if (!prev)
        return;
    if (prev == pBlock)
    {
        pPool->pFirstBlock = pBlock->pNextBlock;
        return;
    }
    for (;;)
    {
        const ptr32<_sMemFragBlock> next = prev->pNextBlock;
        if (!next)
            return;
        if (next == pBlock)
        {
            prev->pNextBlock = pBlock->pNextBlock;
            return;
        }
        prev = next;
    }
}

} // namespace

ptr32<_sMemFragBlock> MemFragBlockCreate(ptr32<_sMemFragPool> pPool)
{
    GuestFrame frame(0x30);
    return newBlock(pPool, frame.local<ptr32<_sMemFragBlock>>(0));
}

// Unlinks the block and frees it; a block not in the list is freed all the same.
void MemFragBlockDelete(ptr32<_sMemFragPool> pPool, ptr32<_sMemFragBlock> pBlock)
{
    GuestFrame frame(0x20);
    const ptr32<ptr32<_sMemFragBlock>> local = frame.local<ptr32<_sMemFragBlock>>(0);
    *local = pBlock;
    unlinkBlock(pPool, pBlock);
    MemBlockFree(local.cast<memHandle>());
}

void MemFragPoolDelete(ptr32<ptr32<_sMemFragPool>> ppPool)
{
    const ptr32<_sMemFragPool> pool = *ppPool;
    {
        GuestFrame frame(0x30);
        while (pool->pFirstBlock)
            MemFragBlockDelete(pool, pool->pFirstBlock);
    }
    MemBlockFree(ppPool.cast<memHandle>()); // A tail call, after the frame is gone.
}

// A fragment from the first block with a freed one or one never handed out, else from a new block.
memFragHandle MemFragCreate(ptr32<_sMemFragPool> pPool)
{
    for (ptr32<_sMemFragBlock> block = pPool->pFirstBlock; block; block = block->pNextBlock)
    {
        const ptr32<_sMemFragFree> frag = block->pFreeFrag;
        if (frag)
        {
            const uint32_t used = block->fragsUsed;
            const ptr32<_sMemFragFree> next = frag->pNextFrag;
            block->fragsUsed = used + 1;
            block->pFreeFrag = next;
            return frag.cast<void>();
        }
        const uint32_t used = block->fragsUsed;
        if (used < pPool->fragsPerBlock)
        {
            const uint32_t fragSize = pPool->fragSize;
            const uint32_t insert = block->insertPoint;
            block->fragsUsed = used + 1;
            block->insertPoint = insert + 1;
            return memFragHandle(block.address + kBlockHeader + fragSize * insert);
        }
    }
    GuestFrame frame(0x30);
    const ptr32<_sMemFragBlock> block = newBlock(pPool, frame.local<ptr32<_sMemFragBlock>>(0));
    if (!block)
        return nullptr;
    const uint32_t used = block->fragsUsed, insert = block->insertPoint;
    block->fragsUsed = used + 1;
    block->insertPoint = insert + 1;
    return memFragHandle(block.address + kBlockHeader);
}

bool_ MemFragPoolCreate(ptr32<ptr32<_sMemFragPool>> ppPool, uint32 fragsPerBlock, uint32 maxFrags, uint32 fragSize, uint32 flags)
{
    GuestFrame frame(0x60);
    if (!MemBlockAllocLo(ppPool.cast<memHandle>(), 0x18))
        return 0;
    const ptr32<_sMemFragPool> pool = *ppPool;
    pool->maxFrags = maxFrags;
    pool->fragSize = (fragSize + 3u) & ~3u;
    pool->fragsPerBlock = fragsPerBlock;
    pool->flags = flags;
    pool->fragsUsed = 0;
    pool->pFirstBlock = nullptr;
    // Flag 1: the pool keeps one block from the start (and never gives its last block back).
    if ((flags & 1u) != 0 && !MemFragBlockCreate(pool))
        return 0;
    return 1;
}

// Returns a fragment to the block it lies in. A block left empty is freed, unless the pool has flag 1 and that is its
// only block (judged by the first block having no next), which is reset instead.
void MemFragDelete(ptr32<_sMemFragPool> pPool, memFragHandle fragHandle)
{
    const uint32_t bytes = blockBytes(*pPool);
    const ptr32<_sMemFragBlock> first = pPool->pFirstBlock;
    const uint32_t frag = fragHandle.address;
    ptr32<_sMemFragBlock> block = first;
    for (;; block = block->pNextBlock)
    {
        if (!block)
            return;
        if (frag < block.address || frag >= block.address + bytes)
            continue;
        break;
    }
    const uint32_t used = block->fragsUsed - 1;
    block->fragsUsed = used;
    if (used != 0)
    {
        const ptr32<_sMemFragFree> freeFrag = block->pFreeFrag;
        block->pFreeFrag = fragHandle.cast<_sMemFragFree>();
        fragHandle.cast<_sMemFragFree>()->pNextFrag = freeFrag;
        return;
    }
    if ((pPool->flags & 1u) != 0 && !first->pNextBlock)
    {
        block->insertPoint = 0;
        block->pFreeFrag = nullptr;
        return;
    }
    GuestFrame frame(0x20);
    const ptr32<ptr32<_sMemFragBlock>> local = frame.local<ptr32<_sMemFragBlock>>(0);
    *local = block;
    if (first)
        unlinkBlock(pPool, block);
    MemBlockFree(local.cast<memHandle>());
}

// Walks every block's free list and checks nothing (the checks are compiled out).
void MemFragDEBUGValidate(ptr32<_sMemFragPool> pPool)
{
    for (ptr32<_sMemFragBlock> block = pPool->pFirstBlock; block;)
    {
        const ptr32<_sMemFragBlock> next = block->pNextBlock;
        for (ptr32<_sMemFragFree> frag = block->pFreeFrag; frag; frag = frag->pNextFrag)
        {
        }
        block = next;
    }
}

} // namespace rc

RC_DECOMP_FN_SETUP_HEAP(MemFragPoolDelete, rc::fuzz::fragPoolHandle)
RC_DECOMP_FN_SETUP_HEAP(MemFragCreate, rc::fuzz::fragPool)
RC_DECOMP_FN_SETUP_HEAP(MemFragPoolCreate, rc::fuzz::fragPoolCreate)
RC_DECOMP_FN_SETUP_HEAP(MemFragDelete, rc::fuzz::fragDelete)
RC_DECOMP_FN_SETUP_HEAP(MemFragBlockCreate, rc::fuzz::fragPool)
RC_DECOMP_FN_SETUP_HEAP(MemFragBlockDelete, rc::fuzz::fragBlock)
RC_DECOMP_FN_SETUP_HEAP(MemFragDEBUGValidate, rc::fuzz::fragPool)

// Decompiled from HEAP.c: a stack-like scratch heap carved from one block of the main heap. Allocation moves a
// pointer up; freeing moves it back to the address given.

#include "mem_fuzz.h"

namespace rc {

bool_ MemBlockAllocLo(ptr32<memHandle> pHandle, uint32 size); // mem.cpp
void MemBlockFree(ptr32<memHandle> pHandle);

namespace {

ptr32<memHandle> heapHandle() { return ptr32<memHandle>(guest_addr(&G(heapVarsG).handle)); }

} // namespace

bool_ HeapInit(uint32 heapSize)
{
    _sHeapVars &heap = G(heapVarsG);
    const uint32_t size = (heapSize + 3u) & ~3u;
    heap.handle = nullptr;
    if (!MemBlockAllocLo(heapHandle(), size))
        return 0;
    const ptr32<uint8> start = heap.handle.cast<uint8>();
    heap.pStart = start;
    heap.pEnd = ptr32<uint8>(start.address + size);
    heap.pCurrent = start;
    return 1;
}

void HeapTerm()
{
    _sHeapVars &heap = G(heapVarsG);
    MemBlockFree(heapHandle());
    heap.pEnd = nullptr;
    heap.pStart = nullptr;
    heap.pCurrent = nullptr;
}

// Null if the request does not fit (the test is on the size as given; the pointer then moves by it rounded to 4).
ptr32<uint8> HeapAlloc(uint32 size)
{
    _sHeapVars &heap = G(heapVarsG);
    const ptr32<uint8> current = heap.pCurrent;
    if (heap.pEnd.address < current.address + size)
        return nullptr;
    heap.pCurrent = ptr32<uint8>(current.address + ((size + 3u) & ~3u));
    return current;
}

void HeapFree(ptr32<uint8> pHeap) { G(heapVarsG).pCurrent = pHeap; }

uint32 HeapGetFree()
{
    const _sHeapVars &heap = G(heapVarsG);
    return heap.pEnd.address - heap.pCurrent.address;
}

void HeapReset()
{
    _sHeapVars &heap = G(heapVarsG);
    heap.pCurrent = heap.pStart;
}

} // namespace rc

RC_DECOMP_FN_SETUP_HEAP(HeapInit, 0x1907d0, rc::fuzz::heapInit)
RC_DECOMP_FN_SETUP_HEAP(HeapTerm, 0x190848, rc::fuzz::heapTerm)
RC_DECOMP_FN_SETUP(HeapAlloc, 0x190880, rc::fuzz::heapVars)
RC_DECOMP_FN_SETUP(HeapFree, 0x1908c8, rc::fuzz::heapVars)
RC_DECOMP_FN_SETUP(HeapGetFree, 0x1908d8, rc::fuzz::heapVars)
RC_DECOMP_FN_SETUP(HeapReset, 0x1908f0, rc::fuzz::heapVars)

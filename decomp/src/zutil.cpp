// Decompiled from ADLER32.c, ZUTIL.c and UNCOMPR.c: the game's zlib 1.1.3 checksum, allocator hooks and uncompress, the one
// call the game makes into zlib (BBLoad, for every level section).
//
// zcalloc and zcfree hand out and free blocks of the game's own heap (MemBlockAllocLo / MemBlockFree, with the handle in the
// function's frame). uncompress is the 1.1.4 form: Z_OK from inflate (the input ran out) is a Z_BUF_ERROR, and an error is
// returned only after inflateEnd; its stream (56 bytes) lives in its frame with zalloc and zfree zero, so inflateInit2_ makes
// them zcalloc and zcfree, and opaque and the fields inflateInit2_ and inflate do not write hold whatever the stack had.

#include "zlib.h"

namespace rc {

bool_ MemBlockAllocLo(ptr32<memHandle> pHandle, uint32 size); // mem.cpp
void MemBlockFree(ptr32<memHandle> pHandle);

// The sums are kept below 65521 after every block of at most 5552 bytes, as the original does (the same result as one sum
// at the end, since nothing overflows 32 bits within a block).
uLong adler32(uLong adler, ptr32<Bytef> buf, uInt len)
{
    uint32_t s1 = adler & 0xffffu, s2 = (adler >> 16) & 0xffffu;
    if (buf.address == 0)
        return 1;
    uint32_t at = buf.address;
    while (len > 0)
    {
        uint32_t k = len < 5552u ? len : 5552u;
        len -= k;
        while (k-- > 0)
        {
            s1 += zl::byteAt(at++);
            s2 += s1;
        }
        s1 %= 65521u;
        s2 %= 65521u;
    }
    return s1 | (s2 << 16);
}

ptr32<char> zlibVersion()
{
    return ptr32<char>(sym::mark_21 + 0x10u);
}

ptr32<char> zError(int32_t err)
{
    // z_errmsg[Z_NEED_DICT - err]
    const uint32_t entry = sym::z_errmsg + static_cast<uint32_t>(zl::Z_NEED_DICT - err) * 4u;
    return ptr32<char>(*static_cast<uint32_t *>(guest_host(entry)));
}

voidpf zcalloc(voidpf, uint32_t items, uint32_t size)
{
    GuestFrame frame(0x20);
    const ptr32<memHandle> mem = frame.local<memHandle>(0);
    *mem = memHandle(0u);
    MemBlockAllocLo(mem, items * size);
    return *mem;
}

void zcfree(voidpf, voidpf ptr)
{
    GuestFrame frame(0x20);
    const ptr32<memHandle> mem = frame.local<memHandle>(0);
    *mem = ptr;
    MemBlockFree(mem);
}

int32_t uncompress(ptr32<Bytef> dest, ptr32<uLongf> destLen, ptr32<Bytef> source, uLong sourceLen)
{
    using namespace zl;
    GuestFrame frame(0x70);
    const ptr32<z_stream_s> stream = frame.local<z_stream_s>(0);
    stream->next_in = source;
    stream->avail_in = sourceLen;
    stream->next_out = dest;
    stream->avail_out = *destLen;
    stream->zalloc = ee_fnptr{0};
    stream->zfree = ee_fnptr{0};
    int32_t err = inflateInit_(stream, ptr32<char>(sym::interruptPC + 8u), 0x38);
    if (err != Z_OK)
        return err;
    err = inflate(stream, 4);
    if (err != Z_STREAM_END)
    {
        inflateEnd(stream);
        return err != 0 ? err : Z_BUF_ERROR;
    }
    *destLen = stream->total_out;
    return inflateEnd(stream);
}

RC_DECOMP_FN_SETUP(adler32, rc::fuzz::zAdler)
RC_DECOMP_FN(zlibVersion)
RC_DECOMP_FN_SETUP(zError, rc::fuzz::zErrorArg)
RC_DECOMP_FN_SETUP_HEAP(zcalloc, rc::fuzz::zAlloc)
RC_DECOMP_FN_SETUP_HEAP(zcfree, rc::fuzz::zFree)
RC_DECOMP_FN_SETUP_HEAP(uncompress, rc::fuzz::zUncompress)

} // namespace rc

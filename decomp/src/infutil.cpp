// Decompiled from INFUTIL.c: copies the window's decoded bytes to the caller's output and runs the checksum over them.

#include "zlib.h"

namespace rc {

using namespace zl;

// Copies what the decoder has written to the window (from s->read up to s->write, or to the window's end) to the stream's
// output, as much as fits; the read pointer moves on, wrapping at the end of the window. The checksum function (a guest
// function pointer, adler32 or none) is run over each piece before it is copied. A Z_BUF_ERROR that did make progress turns
// into Z_OK.
int32_t inflate_flush(ptr32<inflate_blocks_state__INFUTIL> sp, ptr32<z_stream_s> z, int32_t r)
{
    GuestFrame frame(0x70);
    const ptr32<inflate_blocks_state__INFBLOCK> s(sp.address);
    uint32_t p = z->next_out.address;
    uint32_t q = s->read.address;
    uint32_t n = (s->write.address < q ? s->end.address : s->write.address) - q;
    if (n > z->avail_out)
        n = z->avail_out;
    if (n != 0 && r == Z_BUF_ERROR)
        r = Z_OK;
    z->avail_out = z->avail_out - n;
    z->total_out = z->total_out + n;
    if (s->checkfn.address != 0)
    {
        const uint32_t sum = guest_call<uint32_t>(s->checkfn.address, s->check, ptr32<Bytef>(q), n);
        s->check = sum;
        z->adler = sum;
    }
    guest_call<void>(sym::memcpy, ptr32<void>(p), ptr32<void>(q), n);
    q += n;
    p += n;
    if (q == s->end.address)
    {
        if (s->write.address == s->end.address)
            s->write = s->window;
        q = s->window.address;
        n = s->write.address - q;
        if (n > z->avail_out)
            n = z->avail_out;
        if (n != 0 && r == Z_BUF_ERROR)
            r = Z_OK;
        z->avail_out = z->avail_out - n;
        z->total_out = z->total_out + n;
        if (s->checkfn.address != 0)
        {
            const uint32_t sum = guest_call<uint32_t>(s->checkfn.address, s->check, ptr32<Bytef>(q), n);
            s->check = sum;
            z->adler = sum;
        }
        guest_call<void>(sym::memcpy, ptr32<void>(p), ptr32<void>(q), n);
        q += n;
        p += n;
    }
    z->next_out = ptr32<Bytef>(p);
    s->read = ptr32<Bytef>(q);
    return r;
}

RC_DECOMP_FN_SETUP(inflate_flush, rc::fuzz::zFlush)

} // namespace rc

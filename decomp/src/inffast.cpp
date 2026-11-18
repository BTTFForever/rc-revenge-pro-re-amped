// Decompiled from INFFAST.c: the game's zlib 1.1.3 fast inflate loop, used while the window has 258 bytes of room and the
// input 10 bytes left (so that a literal/length code, its extra bits, a distance code and its extra bits can all be read
// without checking). It loads the stream and block state, decodes until either runs short, and writes the state back; the
// bit buffer's whole bytes that were taken but not needed are given back to the input (UNGRAB) before it returns.

#include "zlib.h"

namespace rc {

using namespace zl;

namespace {

constexpr uint32_t kMsgDistanceCode = sym::border + 0x168u;  // "invalid distance code"
constexpr uint32_t kMsgLengthCode = sym::border + 0x180u;    // "invalid literal/length code"

} // namespace

int32_t inflate_fast(uInt bl, uInt bd, ptr32<inflate_huft_s> tl, ptr32<inflate_huft_s> td,
                     ptr32<inflate_blocks_state__INFBLOCK> s, ptr32<z_stream_s> z)
{
    GuestFrame frame(0x70);
    uint32_t e, b, k, p, n, q, m, c, d, r;
    uint32_t t;

    // LOAD
    p = z->next_in.address;
    n = z->avail_in;
    b = s->bitb;
    k = s->bitk;
    q = s->write.address;
    m = q < s->read.address ? s->read.address - q - 1 : s->end.address - q;
    const uint32_t ml = inflateMask(bl), md = inflateMask(bd);

    // UNGRAB, UPDATE
    const auto leave = [&]
    {
        c = z->avail_in - n;
        c = (k >> 3) < c ? (k >> 3) : c;
        n += c;
        p -= c;
        k -= c << 3;
        s->bitb = b;
        s->bitk = k;
        z->avail_in = n;
        z->total_in = z->total_in + (p - z->next_in.address);
        z->next_in = ptr32<Bytef>(p);
        s->write = ptr32<Bytef>(q);
    };
    const auto grab = [&](uint32_t bits)
    {
        while (k < bits)
        {
            n--;
            b |= static_cast<uint32_t>(byteAt(p++)) << (k & 31u);
            k += 8;
        }
    };
    const auto dump = [&](uint32_t bits)
    {
        b >>= (bits & 31u);
        k -= bits;
    };

    do
    {
        grab(20);
        t = tl.address + (b & ml) * 8u;
        e = byteAt(t);
        if (e == 0)
        {
            dump(byteAt(t + 1));
            byteAt(q++) = static_cast<uint8_t>(wordAt(t + 4));
            m--;
            continue;
        }
        do
        {
            dump(byteAt(t + 1));
            if ((e & 16) != 0)
            {
                // the length
                e &= 15;
                c = wordAt(t + 4) + (b & inflateMask(e));
                dump(e);
                // the distance
                grab(15);
                t = td.address + (b & md) * 8u;
                e = byteAt(t);
                do
                {
                    dump(byteAt(t + 1));
                    if ((e & 16) != 0)
                    {
                        e &= 15;
                        grab(e);
                        d = wordAt(t + 4) + (b & inflateMask(e));
                        dump(e);
                        // the copy
                        m -= c;
                        if (q - s->window.address >= d)
                        {
                            r = q - d;
                            byteAt(q++) = byteAt(r++);
                            c--;
                            byteAt(q++) = byteAt(r++);
                            c--;
                        }
                        else
                        {
                            e = d - (q - s->window.address);
                            r = s->end.address - e;
                            if (c > e)
                            {
                                c -= e;
                                do
                                {
                                    byteAt(q++) = byteAt(r++);
                                } while (--e != 0);
                                r = s->window.address;
                            }
                        }
                        do
                        {
                            byteAt(q++) = byteAt(r++);
                        } while (--c != 0);
                        break;
                    }
                    else if ((e & 64) == 0)
                    {
                        t += wordAt(t + 4) * 8u;
                        t += (b & inflateMask(e)) * 8u;
                        e = byteAt(t);
                    }
                    else
                    {
                        z->msg = ptr32<char>(kMsgDistanceCode);
                        leave();
                        return Z_DATA_ERROR;
                    }
                } while (true);
                break;
            }
            if ((e & 64) == 0)
            {
                t += wordAt(t + 4) * 8u;
                t += (b & inflateMask(e)) * 8u;
                e = byteAt(t);
                if (e == 0)
                {
                    dump(byteAt(t + 1));
                    byteAt(q++) = static_cast<uint8_t>(wordAt(t + 4));
                    m--;
                    break;
                }
            }
            else if ((e & 32) != 0)
            {
                leave();
                return Z_STREAM_END;
            }
            else
            {
                z->msg = ptr32<char>(kMsgLengthCode);
                leave();
                return Z_DATA_ERROR;
            }
        } while (true);
    } while (m >= 258 && n >= 10);
    leave();
    return Z_OK;
}

RC_DECOMP_FN_SETUP(inflate_fast, rc::fuzz::zFast)

} // namespace rc

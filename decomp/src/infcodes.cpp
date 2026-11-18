// Decompiled from INFCODES.c: the game's zlib 1.1.3 literal/length and distance decoder. A codes state (0x1c bytes) holds
// the mode, the length being copied, a 8-byte union (the table being walked and the bits it needs; a literal; or the extra
// bits wanted and the distance), the table index widths (lbits, dbits: bytes) and the two tables.
//
// inflate_codes works on the stream and the block state through locals, as inflate_blocks does (see infblock.cpp): LOAD and
// UPDATE move the input pointers and the bit buffer, FLUSH writes only the window's write pointer. Runs with m >= 258 and
// n >= 10 (output room and input left) go through inflate_fast. A copy that starts before the window's start reads from its
// end (the 1.1.3 form: one test, not a loop).

#include "zlib.h"

namespace rc {

using namespace zl;

namespace {

constexpr uint32_t kMsgLengthCode = sym::border + 0x100u;    // "invalid literal/length code"
constexpr uint32_t kMsgDistanceCode = sym::border + 0x120u;  // "invalid distance code"

} // namespace

ptr32<inflate_codes_state__INFCODES> inflate_codes_new(uInt bl, uInt bd, ptr32<inflate_huft_s> tl, ptr32<inflate_huft_s> td,
                                                       ptr32<z_stream_s> z)
{
    GuestFrame frame(0x50);
    const ptr32<inflate_codes_state__INFCODES> c(zalloc(z, 1, 0x1c));
    if (c.address != 0)
    {
        c->lbits = static_cast<Byte>(bl);
        c->dbits = static_cast<Byte>(bd);
        c->ltree = tl;
        c->dtree = td;
        c->mode = static_cast<inflate_codes_mode>(START);
    }
    return c;
}

void inflate_codes_free(ptr32<inflate_codes_state__INFCODES> c, ptr32<z_stream_s> z)
{
    GuestFrame frame(0x10);
    zfree(z, c.address);
}

int32_t inflate_codes(ptr32<inflate_blocks_state__INFBLOCK> s, ptr32<z_stream_s> z, int32_t r)
{
    GuestFrame frame(0x90);
    uint32_t j, e, b, k, p, n, q, m, f;
    const ptr32<inflate_codes_state__INFCODES> c(s->sub.decode.codes.address);

    const auto wavail = [&]() -> uint32_t { return q < s->read.address ? s->read.address - q - 1 : s->end.address - q; };
    const auto loadIn = [&] { p = z->next_in.address; n = z->avail_in; b = s->bitb; k = s->bitk; };
    const auto loadOut = [&] { q = s->write.address; m = wavail(); };
    const auto update = [&]
    {
        s->bitb = b;
        s->bitk = k;
        z->avail_in = n;
        z->total_in = z->total_in + (p - z->next_in.address);
        z->next_in = ptr32<Bytef>(p);
        s->write = ptr32<Bytef>(q);
    };
    const auto leave = [&]() -> int32_t
    {
        update();
        return inflate_flush(ptr32<inflate_blocks_state__INFUTIL>(s.address), z, r);
    };
    const auto flush = [&]
    {
        s->write = ptr32<Bytef>(q);
        r = inflate_flush(ptr32<inflate_blocks_state__INFUTIL>(s.address), z, r);
        q = s->write.address;
        m = wavail();
    };
    const auto wrap = [&]
    {
        if (q == s->end.address && s->read.address != s->window.address)
        {
            q = s->window.address;
            m = wavail();
        }
    };
    const auto dump = [&](uint32_t bits)
    {
        b >>= (bits & 31u);
        k -= bits;
    };
#define NEEDBITS(j)                                    \
    while (k < (j))                                    \
    {                                                  \
        if (n != 0)                                    \
            r = Z_OK;                                  \
        else                                           \
            return leave();                            \
        n--;                                           \
        b |= static_cast<uint32_t>(byteAt(p++)) << (k & 31u); \
        k += 8;                                        \
    }
#define NEEDOUT                      \
    if (m == 0)                      \
    {                                \
        wrap();                      \
        if (m == 0)                  \
        {                            \
            flush();                 \
            wrap();                  \
            if (m == 0)              \
                return leave();      \
        }                            \
    }                                \
    r = Z_OK;

    loadIn();
    loadOut();
    while (true)
    {
        switch (c->mode)
        {
        case START:
            if (m >= 258 && n >= 10)
            {
                update();
                r = inflate_fast(c->lbits, c->dbits, c->ltree, c->dtree, s, z);
                loadIn();
                loadOut();
                if (r != Z_OK)
                {
                    c->mode = static_cast<inflate_codes_mode>(r == Z_STREAM_END ? WASH : BADCODE);
                    break;
                }
            }
            c->sub.code.need = c->lbits;
            c->sub.code.tree = c->ltree;
            c->mode = static_cast<inflate_codes_mode>(LEN);
            [[fallthrough]];
        case LEN:
        {
            j = c->sub.code.need;
            NEEDBITS(j)
            const uint32_t t = c->sub.code.tree.address + (b & inflateMask(j)) * 8u;
            dump(byteAt(t + 1));
            e = byteAt(t);
            if (e == 0)
            {
                c->sub.lit = wordAt(t + 4);
                c->mode = static_cast<inflate_codes_mode>(LIT);
                break;
            }
            if ((e & 16) != 0)
            {
                c->sub.copy.get = e & 15;
                c->len = wordAt(t + 4);
                c->mode = static_cast<inflate_codes_mode>(LENEXT);
                break;
            }
            if ((e & 64) == 0)
            {
                c->sub.code.need = e;
                c->sub.code.tree = ptr32<inflate_huft_s>(t + wordAt(t + 4) * 8u);
                break;
            }
            if ((e & 32) != 0)
            {
                c->mode = static_cast<inflate_codes_mode>(WASH);
                break;
            }
            c->mode = static_cast<inflate_codes_mode>(BADCODE);
            z->msg = ptr32<char>(kMsgLengthCode);
            r = Z_DATA_ERROR;
            return leave();
        }
        case LENEXT:
        {
            j = c->sub.copy.get;
            NEEDBITS(j)
            c->len = c->len + (b & inflateMask(j));
            dump(j);
            c->sub.code.need = c->dbits;
            c->sub.code.tree = c->dtree;
            c->mode = static_cast<inflate_codes_mode>(DIST);
        }
            [[fallthrough]];
        case DIST:
        {
            j = c->sub.code.need;
            NEEDBITS(j)
            const uint32_t t = c->sub.code.tree.address + (b & inflateMask(j)) * 8u;
            dump(byteAt(t + 1));
            e = byteAt(t);
            if ((e & 16) != 0)
            {
                c->sub.copy.get = e & 15;
                c->sub.copy.dist = wordAt(t + 4);
                c->mode = static_cast<inflate_codes_mode>(DISTEXT);
                break;
            }
            if ((e & 64) == 0)
            {
                c->sub.code.need = e;
                c->sub.code.tree = ptr32<inflate_huft_s>(t + wordAt(t + 4) * 8u);
                break;
            }
            c->mode = static_cast<inflate_codes_mode>(BADCODE);
            z->msg = ptr32<char>(kMsgDistanceCode);
            r = Z_DATA_ERROR;
            return leave();
        }
        case DISTEXT:
        {
            j = c->sub.copy.get;
            NEEDBITS(j)
            c->sub.copy.dist = c->sub.copy.dist + (b & inflateMask(j));
            dump(j);
            c->mode = static_cast<inflate_codes_mode>(COPY);
        }
            [[fallthrough]];
        case COPY:
        {
            const uint32_t window = s->window.address;
            const uint32_t dist = c->sub.copy.dist;
            f = (q - window) < dist ? s->end.address - (dist - (q - window)) : q - dist;
            while (c->len != 0)
            {
                NEEDOUT
                byteAt(q) = byteAt(f);
                q++;
                f++;
                m--;
                if (f == s->end.address)
                    f = s->window.address;
                c->len = c->len - 1;
            }
            c->mode = static_cast<inflate_codes_mode>(START);
            break;
        }
        case LIT:
            NEEDOUT
            byteAt(q) = static_cast<uint8_t>(c->sub.lit);
            q++;
            m--;
            c->mode = static_cast<inflate_codes_mode>(START);
            break;
        case WASH:
            if (k > 7)
            {
                k -= 8;
                n++;
                p--;
            }
            flush();
            if (s->read.address != s->write.address)
                return leave();
            c->mode = static_cast<inflate_codes_mode>(END);
            [[fallthrough]];
        case END:
            r = Z_STREAM_END;
            return leave();
        case BADCODE:
            r = Z_DATA_ERROR;
            return leave();
        default:
            r = Z_STREAM_ERROR;
            return leave();
        }
    }
#undef NEEDBITS
#undef NEEDOUT
}

RC_DECOMP_FN_SETUP(inflate_codes, rc::fuzz::zCodes)
RC_DECOMP_FN_SETUP(inflate_codes_new, rc::fuzz::zCodesNew)
RC_DECOMP_FN_SETUP(inflate_codes_free, rc::fuzz::zCodesFree)

} // namespace rc

// Decompiled from INFBLOCK.c: the game's zlib 1.1.3 block decoder. A block state (0x40 bytes) holds the decoder's mode, a
// 20-byte union (stored length; the dynamic table's header, code lengths and tables; or the codes state), the last-block
// flag, the bit buffer (bitb, bitk), the huffman table area (hufts: MANY entries of 8 bytes), the sliding window (window,
// end) with its read and write pointers, and the checksum function and value.
//
// The functions are written from zlib's macros as the compiler expanded them: LOAD / UPDATE move the stream's input
// pointers and the bit buffer between the state and locals, FLUSH writes only the window's write pointer and reloads only
// q and m (UPDATEOUT / LOADOUT of zlib 1.1.3), NEEDBITS sets r to Z_OK for each input byte it takes, and a decoding step
// that cannot go on leaves through inflate_flush. Calls to the stream's zalloc and zfree, the checksum function and memcpy
// are guest calls; the other zlib functions are called directly. inflate_blocks keeps its 0xe0-byte frame, where the
// locals inflate_trees_fixed and inflate_trees_dynamic fill (bl, bd, tl, td) and inflate_trees_dynamic's ninth argument
// (z, at $sp + 0) live.

#include "zlib.h"

namespace rc {

using namespace zl;

namespace {

constexpr uint32_t kMsgBlockType = sym::border + 0x50u;      // "invalid block type"
constexpr uint32_t kMsgStoredLengths = sym::border + 0x68u;  // "invalid stored block lengths"
constexpr uint32_t kMsgTooMany = sym::border + 0x88u;        // "too many length or distance symbols"
constexpr uint32_t kMsgBitRepeat = sym::border + 0xb0u;      // "invalid bit length repeat"

ptr32<inflate_blocks_state__INFUTIL> asUtil(ptr32<inflate_blocks_state__INFBLOCK> s)
{
    return ptr32<inflate_blocks_state__INFUTIL>(s.address);
}

uint32_t borderAt(uint32_t index) { return wordAt(sym::border + index * 4u); }

} // namespace

ptr32<inflate_blocks_state__INFBLOCK> inflate_blocks_new(ptr32<z_stream_s> z, check_func c, uInt w)
{
    GuestFrame frame(0x50);
    const ptr32<inflate_blocks_state__INFBLOCK> s(zalloc(z, 1, 0x40));
    if (s.address == 0)
        return ptr32<inflate_blocks_state__INFBLOCK>(0u);
    const uint32_t hufts = zalloc(z, 8, 0x5a0);
    s->hufts = ptr32<inflate_huft_s>(hufts);
    if (hufts == 0)
    {
        zfree(z, s.address);
        return ptr32<inflate_blocks_state__INFBLOCK>(0u);
    }
    const uint32_t window = zalloc(z, 1, w);
    s->window = ptr32<Bytef>(window);
    if (window == 0)
    {
        zfree(z, s->hufts.address);
        zfree(z, s.address);
        return ptr32<inflate_blocks_state__INFBLOCK>(0u);
    }
    s->checkfn = c;
    s->end = ptr32<Bytef>(window + w);
    s->read = ptr32<Bytef>(window);
    s->mode = static_cast<inflate_block_mode>(TYPE);
    s->bitk = 0;
    s->bitb = 0;
    s->write = ptr32<Bytef>(window);
    if (c.address != 0)
    {
        const uint32_t sum = guest_call<uint32_t>(c.address, 0u, ptr32<Bytef>(0u), 0u);
        s->check = sum;
        z->adler = sum;
    }
    return s;
}

void inflate_blocks_reset(ptr32<inflate_blocks_state__INFBLOCK> s, ptr32<z_stream_s> z, ptr32<uLongf> c)
{
    GuestFrame frame(0x30);
    if (c.address != 0)
        *c = s->check;
    if (s->mode == BTREE || s->mode == DTREE)
        zfree(z, s->sub.trees.blens.address);
    if (s->mode == CODES)
        inflate_codes_free(ptr32<inflate_codes_state__INFCODES>(s->sub.decode.codes.address), z);
    const uint32_t window = s->window.address;
    s->read = ptr32<Bytef>(window);
    s->mode = static_cast<inflate_block_mode>(TYPE);
    s->bitk = 0;
    s->bitb = 0;
    s->write = ptr32<Bytef>(window);
    if (s->checkfn.address != 0)
    {
        const uint32_t sum = guest_call<uint32_t>(s->checkfn.address, 0u, ptr32<Bytef>(0u), 0u);
        s->check = sum;
        z->adler = sum;
    }
}

int32_t inflate_blocks_free(ptr32<inflate_blocks_state__INFBLOCK> s, ptr32<z_stream_s> z)
{
    GuestFrame frame(0x30);
    if (s->mode == BTREE || s->mode == DTREE)
        zfree(z, s->sub.trees.blens.address);
    if (s->mode == CODES)
        inflate_codes_free(ptr32<inflate_codes_state__INFCODES>(s->sub.decode.codes.address), z);
    const uint32_t window = s->window.address;
    s->read = ptr32<Bytef>(window);
    s->mode = static_cast<inflate_block_mode>(TYPE);
    s->bitk = 0;
    s->bitb = 0;
    s->write = ptr32<Bytef>(window);
    if (s->checkfn.address != 0)
    {
        const uint32_t sum = guest_call<uint32_t>(s->checkfn.address, 0u, ptr32<Bytef>(0u), 0u);
        s->check = sum;
        z->adler = sum;
    }
    zfree(z, s->window.address);
    zfree(z, s->hufts.address);
    zfree(z, s.address);
    return Z_OK;
}

void inflate_set_dictionary(ptr32<inflate_blocks_state__INFBLOCK> s, ptr32<Bytef> d, uInt n)
{
    GuestFrame frame(0x30);
    guest_call<void>(sym::memcpy, ptr32<void>(s->window.address), ptr32<void>(d.address), n);
    const uint32_t end = s->window.address + n;
    s->read = ptr32<Bytef>(end);
    s->write = ptr32<Bytef>(end);
}

int32_t inflate_blocks_sync_point(ptr32<inflate_blocks_state__INFBLOCK> s)
{
    return s->mode == LENS ? 1 : 0;
}

int32_t inflate_blocks(ptr32<inflate_blocks_state__INFBLOCK> s, ptr32<z_stream_s> z, int32_t r)
{
    GuestFrame frame(0xe0);
    uint32_t t;
    uint32_t b, k, p, n, q, m;

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
        return inflate_flush(asUtil(s), z, r);
    };
    const auto flush = [&]
    {
        s->write = ptr32<Bytef>(q);
        r = inflate_flush(asUtil(s), z, r);
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
// NEEDBITS(j): takes input bytes until k >= j; with no input left, leaves.
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
// NEEDOUT: room for one output byte in the window, flushing and wrapping as needed.
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
        switch (s->mode)
        {
        case TYPE:
        {
            NEEDBITS(3)
            t = b & 7;
            s->last = t & 1;
            switch (t >> 1)
            {
            case 0:
                dump(3);
                t = k & 7;
                dump(t);
                s->mode = static_cast<inflate_block_mode>(LENS);
                break;
            case 1:
            {
                const ptr32<uInt> bl = frame.local<uInt>(0x10), bd = frame.local<uInt>(0x14);
                const ptr32<ptr32<inflate_huft_s>> tl = frame.local<ptr32<inflate_huft_s>>(0x18), td = frame.local<ptr32<inflate_huft_s>>(0x1c);
                inflate_trees_fixed(bl, bd, tl, td, z);
                const ptr32<inflate_codes_state__INFCODES> codes = inflate_codes_new(*bl, *bd, *tl, *td, z);
                s->sub.decode.codes = ptr32<inflate_codes_state>(codes.address);
                if (codes.address == 0)
                {
                    r = Z_MEM_ERROR;
                    return leave();
                }
                dump(3);
                s->mode = static_cast<inflate_block_mode>(CODES);
                break;
            }
            case 2:
                dump(3);
                s->mode = static_cast<inflate_block_mode>(TABLE);
                break;
            case 3:
                dump(3);
                s->mode = static_cast<inflate_block_mode>(B_BAD);
                z->msg = ptr32<char>(kMsgBlockType);
                r = Z_DATA_ERROR;
                return leave();
            }
            break;
        }
        case LENS:
        {
            NEEDBITS(32)
            if ((((~b) >> 16) & 0xffffu) != (b & 0xffffu))
            {
                s->mode = static_cast<inflate_block_mode>(B_BAD);
                z->msg = ptr32<char>(kMsgStoredLengths);
                r = Z_DATA_ERROR;
                return leave();
            }
            s->sub.left = b & 0xffffu;
            b = 0;
            k = 0;
            s->mode = static_cast<inflate_block_mode>(s->sub.left != 0 ? STORED : (s->last != 0 ? DRY : TYPE));
            break;
        }
        case STORED:
        {
            if (n == 0)
                return leave();
            NEEDOUT
            t = s->sub.left;
            if (t > n)
                t = n;
            if (t > m)
                t = m;
            guest_call<void>(sym::memcpy, ptr32<void>(q), ptr32<void>(p), t);
            p += t;
            n -= t;
            q += t;
            m -= t;
            s->sub.left = s->sub.left - t;
            if (s->sub.left != 0)
                break;
            s->mode = static_cast<inflate_block_mode>(s->last != 0 ? DRY : TYPE);
            break;
        }
        case TABLE:
        {
            NEEDBITS(14)
            t = b & 0x3fffu;
            s->sub.trees.table = t;
            if ((t & 0x1f) > 29 || ((t >> 5) & 0x1f) > 29)
            {
                s->mode = static_cast<inflate_block_mode>(B_BAD);
                z->msg = ptr32<char>(kMsgTooMany);
                r = Z_DATA_ERROR;
                return leave();
            }
            t = 258 + (t & 0x1f) + ((t >> 5) & 0x1f);
            const uint32_t blens = zalloc(z, t, 4);
            s->sub.trees.blens = ptr32<uIntf>(blens);
            if (blens == 0)
            {
                r = Z_MEM_ERROR;
                return leave();
            }
            dump(14);
            s->sub.trees.index = 0;
            s->mode = static_cast<inflate_block_mode>(BTREE);
        }
            [[fallthrough]];
        case BTREE:
        {
            while (s->sub.trees.index < 4 + (s->sub.trees.table >> 10))
            {
                NEEDBITS(3)
                const uint32_t index = s->sub.trees.index;
                s->sub.trees.index = index + 1;
                wordAt(s->sub.trees.blens.address + borderAt(index) * 4u) = b & 7;
                dump(3);
            }
            while (s->sub.trees.index < 19)
            {
                const uint32_t index = s->sub.trees.index;
                s->sub.trees.index = index + 1;
                wordAt(s->sub.trees.blens.address + borderAt(index) * 4u) = 0;
            }
            s->sub.trees.bb = 7;
            t = static_cast<uint32_t>(inflate_trees_bits(s->sub.trees.blens, ptr32<uIntf>(s.address + 0x10u),
                                                         ptr32<ptr32<inflate_huft_s>>(s.address + 0x14u), s->hufts, z));
            if (t != 0)
            {
                zfree(z, s->sub.trees.blens.address);
                r = static_cast<int32_t>(t);
                if (r == Z_DATA_ERROR)
                    s->mode = static_cast<inflate_block_mode>(B_BAD);
                return leave();
            }
            s->sub.trees.index = 0;
            s->mode = static_cast<inflate_block_mode>(DTREE);
        }
            [[fallthrough]];
        case DTREE:
        {
            while (true)
            {
                t = s->sub.trees.index;
                if (!(t < 258 + (s->sub.trees.table & 0x1f) + ((s->sub.trees.table >> 5) & 0x1f)))
                    break;
                t = s->sub.trees.bb;
                NEEDBITS(t)
                const uint32_t h = s->sub.trees.tb.address + (b & inflateMask(t)) * 8u;
                t = byteAt(h + 1);
                uint32_t c = wordAt(h + 4);
                if (c < 16)
                {
                    dump(t);
                    const uint32_t index = s->sub.trees.index;
                    s->sub.trees.index = index + 1;
                    wordAt(s->sub.trees.blens.address + index * 4u) = c;
                }
                else
                {
                    uint32_t i = c == 18 ? 7 : c - 14;
                    uint32_t j = c == 18 ? 11 : 3;
                    NEEDBITS(t + i)
                    dump(t);
                    j += b & inflateMask(i);
                    dump(i);
                    i = s->sub.trees.index;
                    t = s->sub.trees.table;
                    if (i + j > 258 + (t & 0x1f) + ((t >> 5) & 0x1f) || (c == 16 && i < 1))
                    {
                        zfree(z, s->sub.trees.blens.address);
                        s->mode = static_cast<inflate_block_mode>(B_BAD);
                        z->msg = ptr32<char>(kMsgBitRepeat);
                        r = Z_DATA_ERROR;
                        return leave();
                    }
                    c = c == 16 ? wordAt(s->sub.trees.blens.address + (i - 1) * 4u) : 0;
                    do
                    {
                        wordAt(s->sub.trees.blens.address + i * 4u) = c;
                        i++;
                    } while (--j != 0);
                    s->sub.trees.index = i;
                }
            }
            s->sub.trees.tb = ptr32<inflate_huft_s>(0u);
            {
                const ptr32<uInt> bl = frame.local<uInt>(0x20), bd = frame.local<uInt>(0x24);
                const ptr32<ptr32<inflate_huft_s>> tl = frame.local<ptr32<inflate_huft_s>>(0x28), td = frame.local<ptr32<inflate_huft_s>>(0x2c);
                *bl = 9;
                *bd = 6;
                t = s->sub.trees.table;
                wordAt(frame.local<uint32_t>(0).address) = z.address; // the ninth argument
                t = static_cast<uint32_t>(inflate_trees_dynamic(257 + (t & 0x1f), 1 + ((t >> 5) & 0x1f), s->sub.trees.blens, bl, bd, tl, td,
                                                                s->hufts));
                zfree(z, s->sub.trees.blens.address);
                if (t != 0)
                {
                    if (t == static_cast<uint32_t>(Z_DATA_ERROR))
                        s->mode = static_cast<inflate_block_mode>(B_BAD);
                    r = static_cast<int32_t>(t);
                    return leave();
                }
                const ptr32<inflate_codes_state__INFCODES> codes = inflate_codes_new(*bl, *bd, *tl, *td, z);
                if (codes.address == 0)
                {
                    r = Z_MEM_ERROR;
                    return leave();
                }
                s->sub.decode.codes = ptr32<inflate_codes_state>(codes.address);
            }
            s->mode = static_cast<inflate_block_mode>(CODES);
        }
            [[fallthrough]];
        case CODES:
        {
            update();
            r = inflate_codes(s, z, r);
            if (r != Z_STREAM_END)
                return inflate_flush(asUtil(s), z, r);
            r = Z_OK;
            inflate_codes_free(ptr32<inflate_codes_state__INFCODES>(s->sub.decode.codes.address), z);
            loadIn();
            loadOut();
            if (s->last == 0)
            {
                s->mode = static_cast<inflate_block_mode>(TYPE);
                break;
            }
            s->mode = static_cast<inflate_block_mode>(DRY);
        }
            [[fallthrough]];
        case DRY:
        {
            flush();
            if (s->read.address != s->write.address)
                return leave();
            s->mode = static_cast<inflate_block_mode>(B_DONE);
        }
            [[fallthrough]];
        case B_DONE:
            r = Z_STREAM_END;
            return leave();
        case B_BAD:
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

RC_DECOMP_FN_SETUP_HEAP(inflate_blocks_new, rc::fuzz::zBlocksNew)
RC_DECOMP_FN_SETUP(inflate_blocks, rc::fuzz::zBlocks)
RC_DECOMP_FN_SETUP(inflate_blocks_reset, rc::fuzz::zBlocksReset)
RC_DECOMP_FN_SETUP_HEAP(inflate_blocks_free, rc::fuzz::zBlocksFree)
RC_DECOMP_FN_SETUP(inflate_set_dictionary, rc::fuzz::zBlocksSetDictionary)
RC_DECOMP_FN_SETUP(inflate_blocks_sync_point, rc::fuzz::zBlocksSyncPoint)

} // namespace rc

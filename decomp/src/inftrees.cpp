// Decompiled from INFTREES.c: the game's zlib 1.1.3 huffman table builder and the three table makers that use it.
//
// huft_build turns a list of code lengths into a table of 8-byte entries (inflate_huft_s: exop, bits, then the base value or
// the offset of a sub-table) placed in the hufts area hp, counting what it has used in *hn. Its ninth and tenth arguments
// (hn and v, the work space of code values) arrive on the stack, at $sp + 0 and $sp + 8 of the caller's frame, where a native
// caller stores them in its GuestFrame. The entry it builds is a local struct whose two padding bytes are never written, and
// the compiler copies the whole 8 bytes: the table's padding holds what the original's stack held at that place (read once
// with stale_stack). An entry for "out of values" leaves the base the entry before had.
//
// inflate_trees_bits and inflate_trees_dynamic take a work array from the stream's zalloc and free it before they return;
// each error sets the stream's message to the game's string. inflate_trees_fixed hands out the game's precomputed fixed
// tables (fixed_tl, fixed_td, in the data, and the index widths fixed_bl and fixed_bd).

#include "zlib.h"

namespace rc {

using namespace zl;

namespace {

// zlib's BMAX
constexpr int kBmax = 15;

constexpr uint32_t kMsgOversubBits = sym::cpdext + 0x78u;       // "oversubscribed dynamic bit lengths tree"
constexpr uint32_t kMsgIncompleteBits = sym::cpdext + 0xa0u;    // "incomplete dynamic bit lengths tree"
constexpr uint32_t kMsgOversubLit = sym::cpdext + 0xc8u;        // "oversubscribed literal/length tree"
constexpr uint32_t kMsgIncompleteLit = sym::cpdext + 0xf0u;     // "incomplete literal/length tree"
constexpr uint32_t kMsgOversubDist = sym::cpdext + 0x110u;      // "oversubscribed distance tree"
constexpr uint32_t kMsgIncompleteDist = sym::cpdext + 0x130u;   // "incomplete distance tree"
constexpr uint32_t kMsgEmptyDist = sym::cpdext + 0x150u;        // "empty distance tree with lengths"

} // namespace

int32_t huft_build(ptr32<uIntf> b, uInt n, uInt s, ptr32<uIntf> d, ptr32<uIntf> e, ptr32<ptr32<inflate_huft_s>> t,
                   ptr32<uIntf> m, ptr32<inflate_huft_s> hp)
{
    const ptr32<uInt> hn = stack_arg<ptr32<uInt>>(0);
    const uint32_t v = stack_arg<uint32_t>(1);
    // The original's 0x190-byte frame, where its locals live: c[] at 0, the entry being built (r) at 0x40, u[] at 0x50, x[]
    // at 0x90, and the arguments it saves at 0xd0. The native keeps them there too (below $sp, as the original does): the
    // two padding bytes of r (0x42) are never written but are copied into every entry, so they hold what the stack held
    // there, and a later call from another frame (inflate_trees_dynamic after inflate_trees_bits, 0x30 bytes lower) finds
    // there what this call left of its counts (c[4]) or table pointers (u[8]).
    const uint32_t frameAt = getRegU32(&g_runtime->eeScheduler().runningContext(), 29) - 0x190u;
    uint8_t *const frame = static_cast<uint8_t *>(guest_host(frameAt));
    uint32_t *const c = reinterpret_cast<uint32_t *>(frame);
    uint32_t *const u = reinterpret_cast<uint32_t *>(frame + 0x50);
    uint32_t *const x = reinterpret_cast<uint32_t *>(frame + 0x90);
    uint8_t &rExop = frame[0x40];
    uint8_t &rBits = frame[0x41];
    uint32_t &rBase = *reinterpret_cast<uint32_t *>(frame + 0x44);
    const uint16_t rPad = *reinterpret_cast<uint16_t *>(frame + 0x42); // as the stack held it

    uint32_t a, f, i, j, mask, z;
    int32_t g, h, k, l, w, y;
    uint32_t q;
    *reinterpret_cast<uint32_t *>(frame + 0xd0) = n;
    *reinterpret_cast<uint32_t *>(frame + 0xd4) = s;
    *reinterpret_cast<uint32_t *>(frame + 0xd8) = d.address;
    *reinterpret_cast<uint32_t *>(frame + 0xdc) = e.address;
    *reinterpret_cast<uint32_t *>(frame + 0xe0) = t.address;
    *reinterpret_cast<uint32_t *>(frame + 0xe4) = hp.address;

    std::memset(c, 0, 16 * sizeof(uint32_t));
    uint32_t p = b.address;
    i = n;
    do
    {
        c[wordAt(p) & 15u]++; // (lengths are 0 to 15; the original's c[] would overrun the stack on others)
        p += 4;
    } while (--i != 0);
    if (c[0] == n)
    {
        *t = ptr32<inflate_huft_s>(0u);
        *m = 0;
        return Z_OK;
    }

    l = static_cast<int32_t>(*m);
    for (j = 1; j <= static_cast<uint32_t>(kBmax); j++)
        if (c[j] != 0)
            break;
    k = static_cast<int32_t>(j);
    if (static_cast<uint32_t>(l) < j)
        l = static_cast<int32_t>(j);
    for (i = kBmax; i != 0; i--)
        if (c[i] != 0)
            break;
    g = static_cast<int32_t>(i);
    if (static_cast<uint32_t>(l) > i)
        l = static_cast<int32_t>(i);
    *m = static_cast<uint32_t>(l);

    for (y = static_cast<int32_t>(1u << (j & 31)); j < i; j++, y <<= 1)
        if ((y -= static_cast<int32_t>(c[j])) < 0)
            return Z_DATA_ERROR;
    if ((y -= static_cast<int32_t>(c[i])) < 0)
        return Z_DATA_ERROR;
    c[i] += static_cast<uint32_t>(y);

    x[1] = j = 0;
    uint32_t cp = 1;
    uint32_t xi = 2;
    while (--i != 0)
        x[xi++] = (j += c[cp++]);

    p = b.address;
    i = 0;
    do
    {
        if ((j = wordAt(p) & 15u) != 0)
            wordAt(v + x[j]++ * 4u) = i;
        p += 4;
    } while (++i < n);
    n = x[g];
    *reinterpret_cast<uint32_t *>(frame + 0xd0) = n;

    x[0] = i = 0;
    uint32_t vp = 0; // index into v
    h = -1;
    w = -l;
    u[0] = 0;
    q = 0;
    z = 0;

    for (; k <= g; k++)
    {
        a = c[k];
        while (a-- != 0)
        {
            while (k > w + l)
            {
                h++;
                w += l;
                z = static_cast<uint32_t>(g - w);
                z = z > static_cast<uint32_t>(l) ? static_cast<uint32_t>(l) : z;
                if ((f = 1u << ((j = static_cast<uint32_t>(k - w)) & 31)) > a + 1)
                {
                    f -= a + 1;
                    xi = static_cast<uint32_t>(k);
                    if (j < z)
                        while (++j < z)
                        {
                            if ((f <<= 1) <= c[++xi])
                                break;
                            f -= c[xi];
                        }
                }
                z = 1u << (j & 31);
                if (*hn + z > kMany)
                {
                    return Z_MEM_ERROR;
                }
                u[h] = q = hp.address + *hn * 8u;
                *hn = *hn + z;
                if (h != 0)
                {
                    x[h] = i;
                    rBits = static_cast<uint8_t>(l);
                    rExop = static_cast<uint8_t>(j);
                    j = i >> ((w - l) & 31);
                    rBase = (q - u[h - 1]) / 8u - j;
                    const uint32_t at = u[h - 1] + j * 8u;
                    byteAt(at) = rExop;
                    byteAt(at + 1) = rBits;
                    *static_cast<uint16_t *>(guest_host(at + 2)) = static_cast<uint16_t>(rPad);
                    wordAt(at + 4) = rBase;
                }
                else
                    *t = ptr32<inflate_huft_s>(q);
            }
            rBits = static_cast<uint8_t>(k - w);
            if (vp >= n)
                rExop = 128 + 64;
            else if (wordAt(v + vp * 4u) < s)
            {
                rExop = static_cast<uint8_t>(wordAt(v + vp * 4u) < 256 ? 0 : 32 + 64);
                rBase = wordAt(v + vp * 4u);
                vp++;
            }
            else
            {
                const uint32_t code = wordAt(v + vp * 4u);
                rExop = static_cast<uint8_t>(wordAt(e.address + (code - s) * 4u) + 16 + 64);
                rBase = wordAt(d.address + (code - s) * 4u);
                vp++;
            }
            f = 1u << ((k - w) & 31);
            for (j = i >> (w & 31); j < z; j += f)
            {
                const uint32_t at = q + j * 8u;
                byteAt(at) = rExop;
                byteAt(at + 1) = rBits;
                *static_cast<uint16_t *>(guest_host(at + 2)) = static_cast<uint16_t>(rPad);
                wordAt(at + 4) = rBase;
            }
            for (j = 1u << ((k - 1) & 31); (i & j) != 0; j >>= 1)
                i ^= j;
            i ^= j;
            mask = (1u << (w & 31)) - 1;
            while ((i & mask) != x[h])
            {
                h--;
                w -= l;
                mask = (1u << (w & 31)) - 1;
            }
        }
    }
    return y != 0 && g != 1 ? Z_BUF_ERROR : Z_OK;
}

int32_t inflate_trees_bits(ptr32<uIntf> c, ptr32<uIntf> bb, ptr32<ptr32<inflate_huft_s>> tb, ptr32<inflate_huft_s> hp,
                           ptr32<z_stream_s> z)
{
    GuestFrame frame(0x90);
    const ptr32<uInt> hn = frame.local<uInt>(0x10);
    *hn = 0;
    const uint32_t v = zalloc(z, 19, 4);
    if (v == 0)
        return Z_MEM_ERROR;
    wordAt(frame.local<uint32_t>(0).address) = hn.address;
    wordAt(frame.local<uint32_t>(8).address) = v;
    int32_t r = huft_build(c, 19, 19, ptr32<uIntf>(0u), ptr32<uIntf>(0u), tb, bb, hp);
    if (r == Z_DATA_ERROR)
        z->msg = ptr32<char>(kMsgOversubBits);
    else if (r == Z_BUF_ERROR || *bb == 0)
    {
        z->msg = ptr32<char>(kMsgIncompleteBits);
        r = Z_DATA_ERROR;
    }
    zfree(z, v);
    return r;
}

int32_t inflate_trees_dynamic(uInt nl, uInt nd, ptr32<uIntf> c, ptr32<uIntf> bl, ptr32<uIntf> bd,
                              ptr32<ptr32<inflate_huft_s>> tl, ptr32<ptr32<inflate_huft_s>> td, ptr32<inflate_huft_s> hp)
{
    const ptr32<z_stream_s> z = stack_arg<ptr32<z_stream_s>>(0);
    GuestFrame frame(0xc0);
    const ptr32<uInt> hn = frame.local<uInt>(0x10);
    *hn = 0;
    const uint32_t v = zalloc(z, 288, 4);
    if (v == 0)
        return Z_MEM_ERROR;
    wordAt(frame.local<uint32_t>(0).address) = hn.address;
    wordAt(frame.local<uint32_t>(8).address) = v;
    int32_t r = huft_build(c, nl, 257, ptr32<uIntf>(sym::cplens), ptr32<uIntf>(sym::cplext), tl, bl, hp);
    if (r != Z_OK || *bl == 0)
    {
        if (r == Z_DATA_ERROR)
            z->msg = ptr32<char>(kMsgOversubLit);
        else if (r != Z_MEM_ERROR)
        {
            z->msg = ptr32<char>(kMsgIncompleteLit);
            r = Z_DATA_ERROR;
        }
        zfree(z, v);
        return r;
    }
    wordAt(frame.local<uint32_t>(0).address) = hn.address;
    wordAt(frame.local<uint32_t>(8).address) = v;
    r = huft_build(ptr32<uIntf>(c.address + nl * 4u), nd, 0, ptr32<uIntf>(sym::cpdist), ptr32<uIntf>(sym::cpdext), td, bd, hp);
    if (r != Z_OK || (*bd == 0 && nl > 257))
    {
        if (r == Z_DATA_ERROR)
            z->msg = ptr32<char>(kMsgOversubDist);
        else if (r == Z_BUF_ERROR)
        {
            z->msg = ptr32<char>(kMsgIncompleteDist);
            r = Z_DATA_ERROR;
        }
        else if (r != Z_MEM_ERROR)
        {
            z->msg = ptr32<char>(kMsgEmptyDist);
            r = Z_DATA_ERROR;
        }
        zfree(z, v);
        return r;
    }
    zfree(z, v);
    return Z_OK;
}

int32_t inflate_trees_fixed(ptr32<uIntf> bl, ptr32<uIntf> bd, ptr32<ptr32<inflate_huft_s>> tl, ptr32<ptr32<inflate_huft_s>> td,
                            ptr32<z_stream_s>)
{
    *bl = wordAt(sym::fixed_bl);
    *tl = ptr32<inflate_huft_s>(sym::fixed_tl);
    *td = ptr32<inflate_huft_s>(sym::fixed_td);
    *bd = wordAt(sym::fixed_bd);
    return Z_OK;
}

RC_DECOMP_FN_SETUP(huft_build, rc::fuzz::zHuftBuild)
RC_DECOMP_FN_SETUP(inflate_trees_bits, rc::fuzz::zTreesBits)
RC_DECOMP_FN_SETUP(inflate_trees_dynamic, rc::fuzz::zTreesDynamic)
RC_DECOMP_FN_SETUP(inflate_trees_fixed, rc::fuzz::zTreesFixed)

} // namespace rc

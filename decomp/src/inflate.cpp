// Decompiled from INFLATE.c: the game's zlib 1.1.3 inflate driver (the zlib header, the dictionary and checksum words, and
// the hand-off to the block decoder).
//
// The stream's state (0x18 bytes) is allocated with the stream's zalloc and holds the mode, a 4-byte union (method, the
// two check words, or the sync marker), the nowrap flag, the window bits and the block state. inflateInit2_ has inflateEnd
// and inflateReset compiled into it, so its failure paths are written out here as they run: the window-bits check frees the
// state through the stream's zfree (and the blocks, which a fresh state does not have yet), a failed inflate_blocks_new
// frees only the state and returns Z_MEM_ERROR.
//
// The messages are the game's strings (read-only data after the CRC table); the "1.1.3" the version check reads is the
// caller's (uncompress passes its own copy).

#include "zlib.h"

namespace rc {

using namespace zl;

namespace {

ptr32<internal_state__INFLATE> stateOf(ptr32<z_stream_s> z) { return ptr32<internal_state__INFLATE>(z->state.address); }

constexpr uint32_t kMsgMethod = sym::crc_tableG + 0x460u;      // "unknown compression method"
constexpr uint32_t kMsgWindow = sym::crc_tableG + 0x480u;      // "invalid window size"
constexpr uint32_t kMsgHeader = sym::crc_tableG + 0x498u;      // "incorrect header check"
constexpr uint32_t kMsgDictionary = sym::crc_tableG + 0x4b0u;  // "need dictionary"
constexpr uint32_t kMsgData = sym::crc_tableG + 0x4c0u;        // "incorrect data check"

// A checksum word: the next input byte (the caller has checked avail_in).
uint32_t nextByte(ptr32<z_stream_s> z)
{
    z->avail_in = z->avail_in - 1;
    z->total_in = z->total_in + 1;
    const uint32_t at = z->next_in.address;
    z->next_in = ptr32<Bytef>(at + 1);
    return byteAt(at);
}

} // namespace

int32_t inflateInit2_(ptr32<z_stream_s> z, int32_t w, ptr32<char> version, int32_t stream_size)
{
    GuestFrame frame(0x30);
    if (version.address == 0 || static_cast<int8_t>(byteAt(version.address)) != '1' || stream_size != 0x38)
        return Z_VERSION_ERROR;
    if (z.address == 0)
        return Z_STREAM_ERROR;
    z->msg = ptr32<char>(0u);
    if (z->zalloc.address == 0)
    {
        z->opaque = ptr32<void>(0u);
        z->zalloc = ee_fnptr{sym::zcalloc};
    }
    if (z->zfree.address == 0)
        z->zfree = ee_fnptr{sym::zcfree};
    const uint32_t state = zalloc(z, 1, 0x18);
    z->state = ptr32<internal_state>(state);
    if (state == 0)
        return Z_MEM_ERROR;
    const ptr32<internal_state__INFLATE> st(state);
    st->blocks = ptr32<inflate_blocks_state>(0u);
    st->nowrap = 0;
    if (w < 0)
    {
        w = -w;
        st->nowrap = 1;
    }
    if (static_cast<uint32_t>(w - 8) >= 8u)
    {
        // inflateEnd(z): the state, and its blocks if it had any (it has none yet).
        const uint32_t current = z->state.address;
        if (current == 0 || z->zfree.address == 0)
            return Z_STREAM_ERROR;
        const uint32_t blocks = ptr32<internal_state__INFLATE>(current)->blocks.address;
        if (blocks != 0)
            inflate_blocks_free(ptr32<inflate_blocks_state__INFBLOCK>(blocks), z);
        zfree(z, z->state.address);
        z->state = ptr32<internal_state>(0u);
        return Z_STREAM_ERROR;
    }
    st->wbits = static_cast<uint32_t>(w);
    const uint32_t blocks = inflate_blocks_new(z, st->nowrap != 0 ? check_func{0} : check_func{sym::adler32}, 1u << (w & 31)).address;
    const uint32_t stateNow = z->state.address;
    ptr32<internal_state__INFLATE>(stateNow)->blocks = ptr32<inflate_blocks_state>(blocks);
    if (blocks == 0)
    {
        if (z.address == 0 || stateNow == 0 || z->zfree.address == 0)
            return Z_MEM_ERROR;
        zfree(z, stateNow);
        z->state = ptr32<internal_state>(0u);
        return Z_MEM_ERROR;
    }
    // inflateReset(z), with its checks (z and the state are known here)
    ptr32<internal_state__INFLATE>(stateNow)->blocks = ptr32<inflate_blocks_state>(blocks);
    z->total_out = 0;
    z->total_in = 0;
    z->msg = ptr32<char>(0u);
    const ptr32<internal_state__INFLATE> reset(stateNow);
    reset->mode = reset->nowrap != 0 ? BLOCKS : METHOD;
    inflate_blocks_reset(ptr32<inflate_blocks_state__INFBLOCK>(blocks), z, ptr32<uLongf>(0u));
    return Z_OK;
}

int32_t inflateInit_(ptr32<z_stream_s> z, ptr32<char> version, int32_t stream_size)
{
    GuestFrame frame(0x10);
    return inflateInit2_(z, 15, version, stream_size);
}

int32_t inflateEnd(ptr32<z_stream_s> z)
{
    GuestFrame frame(0x20);
    if (z.address == 0 || z->state.address == 0 || z->zfree.address == 0)
        return Z_STREAM_ERROR;
    const uint32_t blocks = stateOf(z)->blocks.address;
    if (blocks != 0)
        inflate_blocks_free(ptr32<inflate_blocks_state__INFBLOCK>(blocks), z);
    zfree(z, z->state.address);
    z->state = ptr32<internal_state>(0u);
    return Z_OK;
}

int32_t inflateReset(ptr32<z_stream_s> z)
{
    GuestFrame frame(0x10);
    if (z.address == 0 || z->state.address == 0)
        return Z_STREAM_ERROR;
    const ptr32<internal_state__INFLATE> st = stateOf(z);
    z->total_out = 0;
    z->total_in = 0;
    z->msg = ptr32<char>(0u);
    st->mode = st->nowrap != 0 ? BLOCKS : METHOD;
    inflate_blocks_reset(ptr32<inflate_blocks_state__INFBLOCK>(st->blocks.address), z, ptr32<uLongf>(0u));
    return Z_OK;
}

int32_t inflateSyncPoint(ptr32<z_stream_s> z)
{
    GuestFrame frame(0x10);
    if (z.address == 0 || z->state.address == 0)
        return Z_STREAM_ERROR;
    const uint32_t blocks = stateOf(z)->blocks.address;
    if (blocks == 0)
        return Z_STREAM_ERROR;
    return inflate_blocks_sync_point(ptr32<inflate_blocks_state__INFBLOCK>(blocks));
}

int32_t inflateSetDictionary(ptr32<z_stream_s> z, ptr32<Bytef> dictionary, uInt dictLength)
{
    GuestFrame frame(0x50);
    uInt length = dictLength;
    if (z.address == 0 || z->state.address == 0 || stateOf(z)->mode != DICT0)
        return Z_STREAM_ERROR;
    if (adler32(1, dictionary, dictLength) != z->adler)
        return Z_DATA_ERROR;
    z->adler = 1;
    const ptr32<internal_state__INFLATE> st = stateOf(z);
    if ((length >> (st->wbits & 31u)) != 0)
    {
        length = (1u << (st->wbits & 31u)) - 1;
        dictionary = ptr32<Bytef>(dictionary.address + (dictLength - length));
    }
    inflate_set_dictionary(ptr32<inflate_blocks_state__INFBLOCK>(st->blocks.address), dictionary, length);
    stateOf(z)->mode = BLOCKS;
    return Z_OK;
}

int32_t inflateSync(ptr32<z_stream_s> z)
{
    GuestFrame frame(0x40);
    if (z.address == 0 || z->state.address == 0)
        return Z_STREAM_ERROR;
    ptr32<internal_state__INFLATE> st = stateOf(z);
    if (st->mode != BAD)
    {
        st->mode = BAD;
        st->sub.marker = 0;
    }
    uInt n = z->avail_in;
    if (n == 0)
        return Z_BUF_ERROR;
    uint32_t p = z->next_in.address;
    const uint32_t start = p;
    st = ptr32<internal_state__INFLATE>(z->state.address);
    uInt m = st->sub.marker;
    uint32_t total = z->total_in;
    while (n != 0 && m < 4)
    {
        // static Byte mark[4] = {0, 0, 0xff, 0xff}
        if (byteAt(p) == byteAt(sym::mark_21 + m))
            m++;
        else if (byteAt(p) != 0)
            m = 0;
        else
            m = 4 - m;
        p++;
        n--;
    }
    z->total_in = total + (p - start);
    z->next_in = ptr32<Bytef>(p);
    z->avail_in = n;
    st->sub.marker = m;
    if (m != 4)
        return Z_DATA_ERROR;
    const uint32_t r = z->total_in, w = z->total_out;
    // inflateReset(z), compiled in
    const ptr32<internal_state__INFLATE> now(z->state.address);
    if (now.address != 0)
    {
        z->total_out = 0;
        z->total_in = 0;
        z->msg = ptr32<char>(0u);
        now->mode = now->nowrap != 0 ? BLOCKS : METHOD;
        inflate_blocks_reset(ptr32<inflate_blocks_state__INFBLOCK>(now->blocks.address), z, ptr32<uLongf>(0u));
    }
    z->total_in = r;
    z->total_out = w;
    ptr32<internal_state__INFLATE>(z->state.address)->mode = BLOCKS;
    return Z_OK;
}

int32_t inflate(ptr32<z_stream_s> z, int32_t f)
{
    GuestFrame frame(0x40);
    if (z.address == 0 || z->state.address == 0 || z->next_in.address == 0)
        return Z_STREAM_ERROR;
    // f = f == Z_FINISH ? Z_BUF_ERROR : Z_OK; r = Z_BUF_ERROR
    f = f == 4 ? Z_BUF_ERROR : Z_OK;
    int32_t r = Z_BUF_ERROR;
    while (true)
    {
        const ptr32<internal_state__INFLATE> st(z->state.address);
        switch (st->mode)
        {
        case METHOD:
        {
            if (z->avail_in == 0)
                return r;
            r = f;
            const uint32_t method = nextByte(z);
            st->sub.method = method;
            if ((method & 0xf) != 8)
            {
                const ptr32<internal_state__INFLATE> bad(z->state.address);
                z->msg = ptr32<char>(kMsgMethod);
                bad->sub.marker = 5;
                bad->mode = BAD;
                break;
            }
            {
                const ptr32<internal_state__INFLATE> cur(z->state.address);
                if (cur->wbits < (cur->sub.method >> 4) + 8)
                {
                    cur->mode = BAD;
                    z->msg = ptr32<char>(kMsgWindow);
                    cur->sub.marker = 5;
                    break;
                }
                cur->mode = FLAG;
            }
        }
            [[fallthrough]];
        case FLAG:
        {
            if (z->avail_in == 0)
                return r;
            r = f;
            const uint32_t b = nextByte(z);
            const ptr32<internal_state__INFLATE> cur(z->state.address);
            if (((cur->sub.method << 8) + b) % 31 != 0)
            {
                z->msg = ptr32<char>(kMsgHeader);
                cur->sub.marker = 5;
                cur->mode = BAD;
                break;
            }
            if ((b & 0x20) == 0)
            {
                cur->mode = BLOCKS;
                break;
            }
            cur->mode = DICT4;
        }
            [[fallthrough]];
        case DICT4:
        {
            if (z->avail_in == 0)
                return r;
            r = f;
            const uint32_t byte = nextByte(z);
            const ptr32<internal_state__INFLATE> cur(z->state.address);
            cur->sub.check.need = byte << 24;
            cur->mode = DICT3;
        }
            [[fallthrough]];
        case DICT3:
        {
            if (z->avail_in == 0)
                return r;
            r = f;
            const uint32_t byte = nextByte(z);
            const ptr32<internal_state__INFLATE> cur(z->state.address);
            cur->sub.check.need = cur->sub.check.need + (byte << 16);
            cur->mode = DICT2;
        }
            [[fallthrough]];
        case DICT2:
        {
            if (z->avail_in == 0)
                return r;
            r = f;
            const uint32_t byte = nextByte(z);
            const ptr32<internal_state__INFLATE> cur(z->state.address);
            cur->sub.check.need = cur->sub.check.need + (byte << 8);
            cur->mode = DICT1;
        }
            [[fallthrough]];
        case DICT1:
        {
            if (z->avail_in == 0)
                return r;
            r = f;
            const uint32_t byte = nextByte(z);
            const ptr32<internal_state__INFLATE> cur(z->state.address);
            cur->sub.check.need = cur->sub.check.need + byte;
            const uint32_t need = ptr32<internal_state__INFLATE>(z->state.address)->sub.check.need;
            ptr32<internal_state__INFLATE>(z->state.address)->mode = DICT0;
            z->adler = need;
            return Z_NEED_DICT;
        }
        case DICT0:
        {
            st->mode = BAD;
            z->msg = ptr32<char>(kMsgDictionary);
            st->sub.marker = 0;
            return Z_STREAM_ERROR;
        }
        case BLOCKS:
        {
            r = inflate_blocks(ptr32<inflate_blocks_state__INFBLOCK>(st->blocks.address), z, r);
            if (r == Z_DATA_ERROR)
            {
                const ptr32<internal_state__INFLATE> cur(z->state.address);
                cur->sub.marker = 0;
                cur->mode = BAD;
                break;
            }
            if (r == Z_OK)
                r = f;
            if (r != Z_STREAM_END)
                return r;
            r = f;
            const ptr32<internal_state__INFLATE> cur(z->state.address);
            inflate_blocks_reset(ptr32<inflate_blocks_state__INFBLOCK>(cur->blocks.address), z,
                                 ptr32<uLongf>(z->state.address + 4u));
            const ptr32<internal_state__INFLATE> after(z->state.address);
            if (after->nowrap != 0)
            {
                after->mode = DONE;
                break;
            }
            after->mode = CHECK4;
        }
            [[fallthrough]];
        case CHECK4:
        {
            if (z->avail_in == 0)
                return r;
            r = f;
            const uint32_t byte = nextByte(z);
            const ptr32<internal_state__INFLATE> cur(z->state.address);
            cur->sub.check.need = byte << 24;
            cur->mode = CHECK3;
        }
            [[fallthrough]];
        case CHECK3:
        {
            if (z->avail_in == 0)
                return r;
            r = f;
            const uint32_t byte = nextByte(z);
            const ptr32<internal_state__INFLATE> cur(z->state.address);
            cur->sub.check.need = cur->sub.check.need + (byte << 16);
            cur->mode = CHECK2;
        }
            [[fallthrough]];
        case CHECK2:
        {
            if (z->avail_in == 0)
                return r;
            r = f;
            const uint32_t byte = nextByte(z);
            const ptr32<internal_state__INFLATE> cur(z->state.address);
            cur->sub.check.need = cur->sub.check.need + (byte << 8);
            cur->mode = CHECK1;
        }
            [[fallthrough]];
        case CHECK1:
        {
            if (z->avail_in == 0)
                return r;
            r = f;
            const uint32_t byte = nextByte(z);
            const ptr32<internal_state__INFLATE> cur(z->state.address);
            cur->sub.check.need = cur->sub.check.need + byte;
            const ptr32<internal_state__INFLATE> now(z->state.address);
            if (now->sub.check.was != now->sub.check.need)
            {
                z->msg = ptr32<char>(kMsgData);
                now->sub.marker = 5;
                now->mode = BAD;
                break;
            }
            now->mode = DONE;
        }
            [[fallthrough]];
        case DONE:
            return Z_STREAM_END;
        case BAD:
            return Z_DATA_ERROR;
        default:
            return Z_STREAM_ERROR;
        }
    }
}

RC_DECOMP_FN_SETUP(inflateInit2_, rc::fuzz::zInit2)
RC_DECOMP_FN_SETUP(inflate, rc::fuzz::zInflate)
RC_DECOMP_FN_SETUP(inflateSync, rc::fuzz::zSync)
RC_DECOMP_FN_SETUP_HEAP(inflateEnd, rc::fuzz::zEnd)
RC_DECOMP_FN_SETUP(inflateSetDictionary, rc::fuzz::zSetDict)
RC_DECOMP_FN_SETUP(inflateReset, rc::fuzz::zReset)
RC_DECOMP_FN_SETUP_HEAP(inflateInit_, rc::fuzz::zInit)
RC_DECOMP_FN_SETUP(inflateSyncPoint, rc::fuzz::zSyncPoint)

} // namespace rc

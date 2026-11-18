// The game's copy of zlib 1.1.3 (inflate only): shared declarations. See inflate.cpp, infblock.cpp, infcodes.cpp,
// inffast.cpp, inftrees.cpp, infutil.cpp and zutil.cpp.
//
// Layouts are the PS2's (uLong and pointers are 32 bits): z_stream 0x38 bytes, the inflate state 0x18, a block state 0x40,
// a codes state 0x1c, a huffman table entry 8. Everything is allocated in guest memory through the stream's zalloc and
// zfree, which are guest function pointers (called with guest_call: they are zcalloc and zcfree unless the caller set
// its own). The tables (inflate_mask, border, cplens, ...) and the error messages are the game's, read through sym::.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "rc_decomp.h"
#include "zlib_fuzz.h"

namespace rc {

namespace zl {


constexpr int32_t Z_OK = 0, Z_STREAM_END = 1, Z_NEED_DICT = 2, Z_STREAM_ERROR = -2, Z_DATA_ERROR = -3, Z_MEM_ERROR = -4,
                  Z_BUF_ERROR = -5, Z_VERSION_ERROR = -6;

// The mode enumerators are the generated ones (rc_types.h: inflate_mode METHOD..BAD, inflate_block_mode TYPE..DRY with
// DONE__823caa04 and BAD__823caa04, inflate_codes_mode START..BADCODE).
constexpr int32_t B_DONE = DONE__823caa04, B_BAD = BAD__823caa04;

constexpr uint32_t kMany = 1440; // the hufts of a block state: MANY entries of 8 bytes

// The game's tables and messages.
constexpr uint32_t kInflateMask = sym::inflate_mask;

inline uint32_t inflateMask(uint32_t bits) { return *static_cast<uint32_t *>(guest_host(kInflateMask + bits * 4u)); }

inline uint8_t &byteAt(uint32_t address) { return *static_cast<uint8_t *>(guest_host(address)); }
inline uint32_t &wordAt(uint32_t address) { return *static_cast<uint32_t *>(guest_host(address)); }

// ZALLOC(z, items, size) and ZFREE(z, ptr): the stream's guest functions.
inline uint32_t zalloc(ptr32<z_stream_s> z, uint32_t items, uint32_t size)
{
    return guest_call<ptr32<void>>(z->zalloc.address, z->opaque, items, size).address;
}
inline void zfree(ptr32<z_stream_s> z, uint32_t pointer)
{
    guest_call<void>(z->zfree.address, z->opaque, ptr32<void>(pointer));
}

} // namespace zl

// INFLATE.c
int32_t inflateInit2_(ptr32<z_stream_s> z, int32_t w, ptr32<char> version, int32_t stream_size);
int32_t inflate(ptr32<z_stream_s> z, int32_t f);
int32_t inflateSync(ptr32<z_stream_s> z);
int32_t inflateEnd(ptr32<z_stream_s> z);
int32_t inflateSetDictionary(ptr32<z_stream_s> z, ptr32<Bytef> dictionary, uInt dictLength);
int32_t inflateReset(ptr32<z_stream_s> z);
int32_t inflateInit_(ptr32<z_stream_s> z, ptr32<char> version, int32_t stream_size);
int32_t inflateSyncPoint(ptr32<z_stream_s> z);

// INFBLOCK.c
ptr32<inflate_blocks_state__INFBLOCK> inflate_blocks_new(ptr32<z_stream_s> z, check_func c, uInt w);
int32_t inflate_blocks(ptr32<inflate_blocks_state__INFBLOCK> s, ptr32<z_stream_s> z, int32_t r);
void inflate_blocks_reset(ptr32<inflate_blocks_state__INFBLOCK> s, ptr32<z_stream_s> z, ptr32<uLongf> c);
int32_t inflate_blocks_free(ptr32<inflate_blocks_state__INFBLOCK> s, ptr32<z_stream_s> z);
void inflate_set_dictionary(ptr32<inflate_blocks_state__INFBLOCK> s, ptr32<Bytef> d, uInt n);
int32_t inflate_blocks_sync_point(ptr32<inflate_blocks_state__INFBLOCK> s);

// INFCODES.c
int32_t inflate_codes(ptr32<inflate_blocks_state__INFBLOCK> s, ptr32<z_stream_s> z, int32_t r);
ptr32<inflate_codes_state__INFCODES> inflate_codes_new(uInt bl, uInt bd, ptr32<inflate_huft_s> tl, ptr32<inflate_huft_s> td,
                                                       ptr32<z_stream_s> z);
void inflate_codes_free(ptr32<inflate_codes_state__INFCODES> c, ptr32<z_stream_s> z);

// INFFAST.c
int32_t inflate_fast(uInt bl, uInt bd, ptr32<inflate_huft_s> tl, ptr32<inflate_huft_s> td,
                     ptr32<inflate_blocks_state__INFBLOCK> s, ptr32<z_stream_s> z);

// INFTREES.c. huft_build takes hn and v as its ninth and tenth arguments, on the stack ($sp + 0 and $sp + 8 of the caller,
// which a native caller stores in its GuestFrame); inflate_trees_dynamic takes z as its ninth.
int32_t huft_build(ptr32<uIntf> b, uInt n, uInt s, ptr32<uIntf> d, ptr32<uIntf> e, ptr32<ptr32<inflate_huft_s>> t,
                   ptr32<uIntf> m, ptr32<inflate_huft_s> hp);
int32_t inflate_trees_bits(ptr32<uIntf> c, ptr32<uIntf> bb, ptr32<ptr32<inflate_huft_s>> tb, ptr32<inflate_huft_s> hp,
                           ptr32<z_stream_s> z);
int32_t inflate_trees_dynamic(uInt nl, uInt nd, ptr32<uIntf> c, ptr32<uIntf> bl, ptr32<uIntf> bd,
                              ptr32<ptr32<inflate_huft_s>> tl, ptr32<ptr32<inflate_huft_s>> td, ptr32<inflate_huft_s> hp);
int32_t inflate_trees_fixed(ptr32<uIntf> bl, ptr32<uIntf> bd, ptr32<ptr32<inflate_huft_s>> tl, ptr32<ptr32<inflate_huft_s>> td,
                            ptr32<z_stream_s> z);

// INFUTIL.c
int32_t inflate_flush(ptr32<inflate_blocks_state__INFUTIL> s, ptr32<z_stream_s> z, int32_t r);

// ADLER32.c, ZUTIL.c, UNCOMPR.c
uLong adler32(uLong adler, ptr32<Bytef> buf, uInt len);
ptr32<char> zlibVersion();
ptr32<char> zError(int32_t err);
voidpf zcalloc(voidpf opaque, uint32_t items, uint32_t size);
void zcfree(voidpf opaque, voidpf ptr);
int32_t uncompress(ptr32<Bytef> dest, ptr32<uLongf> destLen, ptr32<Bytef> source, uLong sourceLen);

} // namespace rc

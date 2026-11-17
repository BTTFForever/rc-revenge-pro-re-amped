// The SDK's VIF1 packet builder (libvifpk) and the VU0 vector helpers RDRPOLY uses (libvu0). Their output is data the
// renderer reads, so they are reproduced exactly rather than replaced.
//
// A packet is a cursor into a buffer of 32-bit words, plus the open DMA tag and the open UNPACK code, whose counts are
// filled in when they are closed.

#include "vifpk.h"

namespace rc {

namespace {

ptr32<u_int> advance(ptr32<u_int> p, int32_t words) { return ptr32<u_int>(p.address + static_cast<uint32_t>(words) * 4u); }

uint32_t &word(uint32_t address) { return *static_cast<uint32_t *>(guest_host(address)); }

__m128 load4(uint32_t address) { return _mm_loadu_ps(static_cast<const float *>(guest_host(address & ~0xFu))); }
void store4(uint32_t address, __m128 v) { _mm_storeu_ps(static_cast<float *>(guest_host(address & ~0xFu)), v); }

} // namespace

void sceVif1PkInit(ptr32<sceVif1Packet> p, ptr32<ee_uint128> base)
{
    p->pCurrent = base.cast<u_int>();
    p->pDmaTag = nullptr;
    p->pBase = base;
}

void sceVif1PkReset(ptr32<sceVif1Packet> p)
{
    p->pDmaTag = nullptr;
    p->pCurrent = p->pBase.cast<u_int>();
}

// Pads to a quadword with zeros and closes the open DMA tag, adding the quadwords written since it to its count.
ptr32<u_int> sceVif1PkTerminate(ptr32<sceVif1Packet> p)
{
    uint32_t at = p->pCurrent.address;
    while ((at & 0xC) != 0)
    {
        word(at) = 0;
        at += 4;
    }
    const uint32_t tag = p->pDmaTag.address;
    if (tag != 0)
        word(tag) += static_cast<uint32_t>((static_cast<int32_t>(at - tag) >> 4) - 1);
    p->pDmaTag = nullptr;
    p->pCurrent = ptr32<u_int>(at);
    return p->pCurrent;
}

// Ends the packet with a DMA "ret" tag, which becomes the open tag.
ptr32<u_int> sceVif1PkRet(ptr32<sceVif1Packet> p, u_int data)
{
    const ptr32<u_int> tag = sceVif1PkTerminate(p);
    p->pDmaTag = tag.cast<ee_uint128>();
    word(tag.address) = data | 0x60000000u;
    p->pVifCode = nullptr;
    p->pCurrent = advance(tag, 2);
    word(tag.address + 4) = 0;
    return p->pCurrent;
}

void sceVif1PkAddCode(ptr32<sceVif1Packet> p, u_int code)
{
    *p->pCurrent = code;
    p->pCurrent = advance(p->pCurrent, 1);
}

void sceVif1PkAddUpkData32(ptr32<sceVif1Packet> p, u_int data)
{
    sceVif1PkAddCode(p, data);
}

void sceVif1PkAddUpkData32N(ptr32<sceVif1Packet> p, ptr32<u_int> data, u_int count)
{
    uint32_t to = p->pCurrent.address, from = data.address;
    for (u_int i = 0; i < count; ++i, to += 4, from += 4)
        word(to) = word(from);
    p->pCurrent = ptr32<u_int>(to);
}

void sceVif1PkAddUpkData64(ptr32<sceVif1Packet> p, uint64 data)
{
    const uint32_t at = p->pCurrent.address;
    word(at) = static_cast<uint32_t>(data);
    word(at + 4) = static_cast<uint32_t>(data >> 32);
    p->pCurrent = ptr32<u_int>(at + 8);
}

void pkAddUpkData128(sceVif1Packet &p, uint64_t low, uint64_t high)
{
    const uint32_t at = p.pCurrent.address;
    word(at) = static_cast<uint32_t>(low);
    word(at + 4) = static_cast<uint32_t>(low >> 32);
    word(at + 8) = static_cast<uint32_t>(high);
    word(at + 12) = static_cast<uint32_t>(high >> 32);
    p.pCurrent = ptr32<u_int>(at + 16);
}

// Opens an UNPACK: STCYCL cl/wl, then the UNPACK code with no count yet. numlen keeps what CloseUpkCode needs: the
// size of one element in bits (low half), and the write cycle (high half) when it is longer than the read cycle.
void sceVif1PkOpenUpkCode(ptr32<sceVif1Packet> p, u_short vuAddr, u_int upkCmd, u_int cl, u_int wl)
{
    const uint32_t at = p->pCurrent.address;
    word(at) = (wl << 8) | cl | 0x01000000u;
    word(at + 4) = (upkCmd << 24) | vuAddr;
    p->pCurrent = ptr32<u_int>(at + 8);
    p->pVifCode = ptr32<u_int>(at + 4);
    const uint32_t elementBits = (0x20u >> (upkCmd & 3u)) * (((upkCmd >> 2) & 3u) + 1u);
    const uint32_t readCycle = cl != 0 ? cl : 0x100u;
    const uint32_t writeCycle = wl != 0 ? wl : 0x100u;
    p->numlen = readCycle < writeCycle ? (writeCycle << 16) | (elementBits * readCycle) : 0x10000u | elementBits;
}

// Closes the UNPACK: its NUM field gets the number of elements written since it opened.
void sceVif1PkCloseUpkCode(ptr32<sceVif1Packet> p)
{
    const uint32_t code = p->pVifCode.address;
    const int32_t words = static_cast<int32_t>(p->pCurrent.address - 4u - code) >> 2;
    const uint32_t unit = p->numlen & 0xFFFFu;
    const uint32_t cycle = p->numlen & 0xFFFF0000u;
    const uint32_t elements = ee_divu(static_cast<uint32_t>(words) * 32u + unit - 1u, unit);
    p->pVifCode = nullptr;
    word(code) += cycle * elements;
}

void sceVu0CopyVector(ptr32<ee_float> dst, ptr32<ee_float> src)
{
    std::memcpy(guest_host(dst.address & ~0xFu), guest_host(src.address & ~0xFu), 16);
}

void sceVu0ScaleVector(ptr32<ee_float> dst, ptr32<ee_float> src, ee_float scale)
{
    store4(dst.address, PS2_VMUL(load4(src.address), _mm_set1_ps(scale.v)));
}

// dst = m * v, as VU0 does it: each column times one component, accumulated with rounding at each step.
void vu0ApplyMatrix(float *dst, const float *m, const float *v)
{
    __m128 acc = PS2_VMUL(_mm_loadu_ps(m), _mm_set1_ps(v[0]));
    acc = PS2_VADD(acc, PS2_VMUL(_mm_loadu_ps(m + 4), _mm_set1_ps(v[1])));
    acc = PS2_VADD(acc, PS2_VMUL(_mm_loadu_ps(m + 8), _mm_set1_ps(v[2])));
    _mm_storeu_ps(dst, PS2_VADD(acc, PS2_VMUL(_mm_loadu_ps(m + 12), _mm_set1_ps(v[3]))));
}

void sceVu0ApplyMatrix(ptr32<ee_float> dst, ptr32<ee_float> m, ptr32<ee_float> v)
{
    alignas(16) float matrix[16], vector[4], out[4];
    std::memcpy(matrix, guest_host(m.address & ~0xFu), 64);
    std::memcpy(vector, guest_host(v.address & ~0xFu), 16);
    vu0ApplyMatrix(out, matrix, vector);
    std::memcpy(guest_host(dst.address & ~0xFu), out, 16);
}

} // namespace rc

RC_DECOMP_FN(sceVif1PkInit)
RC_DECOMP_FN(sceVif1PkReset)
RC_DECOMP_FN(sceVif1PkTerminate)
RC_DECOMP_FN(sceVif1PkRet)
RC_DECOMP_FN(sceVif1PkAddCode)
RC_DECOMP_FN(sceVif1PkAddUpkData32)
RC_DECOMP_FN(sceVif1PkAddUpkData32N)
RC_DECOMP_FN(sceVif1PkAddUpkData64)
RC_DECOMP_FN(sceVif1PkOpenUpkCode)
RC_DECOMP_FN_NOFUZZ(sceVif1PkCloseUpkCode) // Divides by numlen: a zero unit traps.
RC_DECOMP_FN(sceVu0CopyVector)
RC_DECOMP_FN(sceVu0ScaleVector)
RC_DECOMP_FN(sceVu0ApplyMatrix)

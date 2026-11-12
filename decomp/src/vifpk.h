// libvifpk and libvu0 (vifpk.cpp).
#pragma once

#include "ee_math.h"

namespace rc {

void sceVif1PkInit(ptr32<sceVif1Packet> p, ptr32<ee_uint128> base);
void sceVif1PkReset(ptr32<sceVif1Packet> p);
ptr32<u_int> sceVif1PkTerminate(ptr32<sceVif1Packet> p);
ptr32<u_int> sceVif1PkRet(ptr32<sceVif1Packet> p, u_int data);
void sceVif1PkAddCode(ptr32<sceVif1Packet> p, u_int code);
void sceVif1PkAddUpkData32(ptr32<sceVif1Packet> p, u_int data);
void sceVif1PkAddUpkData32N(ptr32<sceVif1Packet> p, ptr32<u_int> data, u_int count);
void sceVif1PkAddUpkData64(ptr32<sceVif1Packet> p, uint64 data);
void pkAddUpkData128(sceVif1Packet &p, uint64_t low, uint64_t high); // The SDK's takes a 128-bit register.
void sceVif1PkOpenUpkCode(ptr32<sceVif1Packet> p, u_short vuAddr, u_int upkCmd, u_int cl, u_int wl);
void sceVif1PkCloseUpkCode(ptr32<sceVif1Packet> p);
void vu0ApplyMatrix(float *dst, const float *m, const float *v);

} // namespace rc

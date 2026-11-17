// Decompiled from TSO.c: the level's scenery objects (TSOs). tsoVarG.pTSOHeader is a count and then 0x3c-byte objects:
// a tag, two visibility words and a TOB instance (its shape or animated model at +0xc, position, angles, flags at +0x22,
// matrix, an animation angle at +0x36). Flag 0x40 marks a vertex-animated object, 0x80 a boned (skinned) one.
//
// Written from the instructions.

#include "anim_fuzz.h"

namespace rc {

void AngleAxisToMatrix(ptr32<_matPsx> pMat, ptr32<vec16_> pAxis, angle ang); // maths.cpp

namespace {

constexpr uint32_t kTsoVars = sym::tsoVarG, kTsoCallBack = sym::TSOCallBack, kRdrCamera = sym::rdrCameraG;
constexpr uint32_t kDistanceThreshold = sym::distanceThreshold_468, kViewConeScale = sym::lit4_3279ac;
constexpr uint32_t kReInitDelta = sym::dx_473; // TSOReInit's static dx, dy, dz
constexpr uint32_t kTobInit = sym::TOBInit, kTobTerm = sym::TOBTerm, kMat3nBuild = sym::Mat3nBuild;

template <typename T>
T &mem(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

ee_float fixed12(uint32_t address) { return ee_itof(mem<int16>(address)) * ee_float(1.0f / 4096.0f); }
ee_float fixed8(uint32_t address) { return ee_itof(mem<int32_t>(address)) * ee_float(1.0f / 256.0f); }

} // namespace

void TSOTerm() { guest_call(kTobTerm); }

void TSOInit()
{
    GuestFrame frame(0x10);
    guest_call(kTobInit);
    mem<uint32_t>(kTsoVars) = 0;
}

void TSOSetSFXCallBack(ee_fnptr CallBack) { mem<uint32_t>(kTsoCallBack) = CallBack.address; }

// Calls back for each object with a tag: a0 its TOB instance, a1 the tag.
void TSOPreProcess(ee_fnptr pCallback)
{
    GuestFrame frame(0x50);
    const uint32_t header = mem<uint32_t>(kTsoVars);
    if (header == 0)
        return;
    uint32_t tso = header + 4u;
    for (uint32_t n = mem<uint32_t>(header); n != 0; --n, tso += 0x3cu)
    {
        const uint32_t tag = mem<uint32_t>(tso);
        if (tag != 0)
            guest_call(pCallback.address, tso + 0xcu, tag);
    }
}

void TSO_UpdateMatrix(ptr32<_sTSO> pTSO)
{
    const uint32_t tso = pTSO.address;
    if ((mem<uint32_t>(tso + 0x20u) & 0xc00000u) != 0)
        return;
    uint16 flags = mem<uint16>(tso + 0x22u);
    if ((flags & 4) != 0)
    {
        AngleAxisToMatrix(ptr32<_matPsx>(tso + 0x24u), ptr32<vec16_>(tso + 0x1cu), mem<int16>(tso + 0x36u));
        flags = mem<uint16>(tso + 0x22u);
    }
    else if ((flags & 8) != 0)
    {
        GuestFrame frame(0x20);
        guest_call(kMat3nBuild, tso + 0x24u, tso + 0x1cu);
        flags = mem<uint16>(tso + 0x22u);
    }
    mem<uint16>(tso + 0x22u) = static_cast<uint16>(flags & 0xfffd);
}

// How far in front of the camera a sphere is, or -1 if it is outside the view (behind, past a side plane, or too far).
ee_float IsInViewCone(ptr32<_sVec3> pPos, f32 radius)
{
    const uint32_t cam = kRdrCamera;
    const ee_float r = radius;
    const ee_float dx = mem<ee_float>(pPos.address) - mem<ee_float>(cam + 0xbcu);
    const ee_float dy = mem<ee_float>(pPos.address + 4u) - mem<ee_float>(cam + 0xc0u);
    const ee_float dz = mem<ee_float>(pPos.address + 8u) - mem<ee_float>(cam + 0xc4u);
    const auto row = [&](uint32_t at) { return dx * mem<ee_float>(at) + dy * mem<ee_float>(at + 4u) + dz * mem<ee_float>(at + 8u); };
    ee_float x = row(cam + 0x98u);
    ee_float y = row(cam + 0xa4u);
    const ee_float z = row(cam + 0xb0u);
    if (z < -r)
        return -1.0f;
    if (ee_float(0.0f) < x)
        x = -x;
    if (ee_float(0.0f) < y)
        y = -y;
    const auto plane = [&](uint32_t at) { return x * mem<ee_float>(at) + y * mem<ee_float>(at + 4u) + z * mem<ee_float>(at + 8u); };
    if (r < plane(cam + 0x28u))
        return -1.0f;
    if (r < plane(cam + 0x34u))
        return -1.0f;
    const ee_float limit = mem<ee_float>(kDistanceThreshold) * mem<ee_float>(cam + 4u) * r * mem<ee_float>(kViewConeScale);
    if (limit < z)
        return -1.0f;
    return z;
}

// Puts the animated scenery back to its first frame: a vertex-animated object's vertices to frame 0 and its shape to
// the object's place (only a version 3 shape: anything else ends the whole pass), a boned one's model to the object.
void TSOReInit()
{
    const uint32_t header = mem<uint32_t>(kTsoVars);
    if (header == 0)
        return;
    uint32_t tso = header + 4u;
    for (uint32_t n = mem<uint32_t>(header); n != 0; --n, tso += 0x3cu)
    {
        const uint32_t ref = mem<uint32_t>(tso + 0xcu);
        if (ref == 0)
            continue;
        const uint16 flags = mem<uint16>(tso + 0x22u);
        if ((flags & 0x40) != 0)
        {
            const uint32_t inst = mem<uint32_t>(ref);
            mem<uint32_t>(ref + 0x10u) = 0;
            mem<uint32_t>(inst + 0x78u) = 0;
            const uint32_t bits = mem<uint32_t>(inst + 0x7cu) & 0xffffff7fu & 0xfffffffdu & 0xffffffefu;
            mem<uint32_t>(ref + 8u) = mem<uint32_t>(inst + 0x78u);
            const uint32_t playing = bits | 0x400u;
            mem<uint32_t>(inst + 0x7cu) = playing;
            mem<uint32_t>(inst + 0x7cu) = (playing & 0x100u) != 0 ? bits | 0x4c0u : playing & 0xffffffbfu;
            const uint32_t anim = mem<uint32_t>(ref);
            mem<uint32_t>(ref + 4u) = mem<uint32_t>(anim + 0x7cu);
            for (uint32_t k = 0; k < mem<uint8>(anim + 0x5du) * 3u; ++k)
            {
                const uint32_t frame0 = mem<uint32_t>(mem<uint32_t>(anim + 0x80u));
                const uint32_t src = mem<uint32_t>(mem<uint32_t>(frame0 + k * 4u));
                const uint32_t dst = mem<uint32_t>(mem<uint32_t>(mem<uint32_t>(anim + 0x84u) + k * 4u));
                const ee_float v[3] = {mem<ee_float>(src), mem<ee_float>(src + 4u), mem<ee_float>(src + 8u)};
                for (uint32_t i = 0; i < 3; ++i)
                    mem<ee_float>(kReInitDelta + i * 4u) = v[i];
                mem<ee_float>(dst + 8u) = v[2];
                mem<ee_float>(dst) = v[0];
                mem<ee_float>(dst + 4u) = v[1];
            }
            if (!(mem<ee_float>(anim) == ee_float(3.0f)))
                return;
            const ee_float pos[3] = {fixed8(tso + 0x10u), fixed8(tso + 0x14u), fixed8(tso + 0x18u)};
            for (uint32_t i = 0; i < 9; ++i)
                mem<ee_float>(anim + 8u + i * 4u) = fixed12(tso + 0x24u + i * 2u);
            mem<ee_float>(anim + 0x34u) = pos[2];
            mem<ee_float>(anim + 0x2cu) = pos[0];
            mem<ee_float>(anim + 0x30u) = pos[1];
            mem<uint32_t>(ref + 0x10u) = 0;
        }
        else if ((flags & 0x80) != 0)
        {
            const uint32_t ctrl = mem<uint32_t>(ref + 0x38u);
            const ee_float pos[3] = {fixed8(tso + 0x10u), fixed8(tso + 0x14u), fixed8(tso + 0x18u)};
            mem<uint32_t>(ctrl + 0x18u) = 0x32;
            mem<uint32_t>(ctrl + 4u) = 0;
            const uint32_t bits = mem<uint32_t>(ref + 0x40u);
            mem<ee_float>(ref + 0x24u) = pos[0];
            mem<ee_float>(ref + 0x28u) = pos[1];
            mem<ee_float>(ref + 0x2cu) = pos[2];
            const uint32_t cleared = bits & 0xffffff7fu & 0xfffffffdu & 0xffffffefu & 0xfffffbffu;
            mem<uint32_t>(ref + 0x30u) = 0;
            mem<uint32_t>(ref + 0x40u) = cleared;
            mem<uint32_t>(ref + 0x40u) = (bits & 0x100u) != 0 ? cleared | 0xc0u : cleared & 0xffffffbfu;
            for (uint32_t i = 0; i < 9; ++i)
                mem<ee_float>(ref + i * 4u) = fixed12(tso + 0x24u + i * 2u);
        }
    }
}

} // namespace rc

using rc::fuzz::tsoCallback;
using rc::fuzz::tsoLevel;
using rc::fuzz::tsoObject;
using rc::fuzz::tsoViewCone;

RC_DECOMP_FN_SETUP(TSOTerm, tsoLevel)
RC_DECOMP_FN_SETUP(TSOInit, tsoLevel)
RC_DECOMP_FN_SETUP(TSOSetSFXCallBack, tsoCallback)
RC_DECOMP_FN_SETUP_HEAP(TSOPreProcess, tsoCallback)
RC_DECOMP_FN_SETUP_HEAP(TSO_UpdateMatrix, tsoObject)
RC_DECOMP_FN_SETUP(IsInViewCone, tsoViewCone)
// Vertex-animated and boned objects on the level's heap.
RC_DECOMP_FN_SETUP_HEAP(TSOReInit, tsoLevel)

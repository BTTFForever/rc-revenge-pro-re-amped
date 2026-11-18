// Decompiled from TSO.c: the level's scenery objects (TSOs). tsoVarG.pTSOHeader is a count and then 0x3c-byte objects:
// a tag, two visibility words and a TOB instance (its shape or animated model at +0xc, position, angles, flags at +0x22,
// matrix, an animation angle at +0x36). Flag 0x40 marks a vertex-animated object, 0x80 a boned (skinned) one.
//
// Written from the instructions. The sound helpers call back through TSOCallBack (RevSfxTSO in the game).

#include "anim_fuzz.h"

namespace rc {

void AngleAxisToMatrix(ptr32<_matPsx> pMat, ptr32<vec16_> pAxis, angle ang); // maths.cpp

namespace {

constexpr uint32_t kTsoVars = sym::tsoVarG, kTsoCallBack = sym::TSOCallBack, kRdrCamera = sym::rdrCameraG;
constexpr uint32_t kDistanceThreshold = sym::distanceThreshold_468, kViewConeScale = sym::lit4_3279ac;
constexpr uint32_t kReInitDelta = sym::dx_473; // TSOReInit's static dx, dy, dz
constexpr uint32_t kRenderDelta = sym::dx_480; // TSORender's
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

namespace {

// An unsigned word as a float, as the compiler converts one: halved (keeping the low bit) and doubled when the top bit
// is set.
ee_float utof(uint32_t value)
{
    if (static_cast<int32_t>(value) >= 0)
        return ee_itof(static_cast<int32_t>(value));
    const ee_float half = ee_itof(static_cast<int32_t>((value >> 1) | (value & 1u)));
    return half + half;
}

// The sound callback (RevSfxTSO in the game): a0 a sound, a1 the object's position.
void callBack(int32_t sound, uint32_t pos) { guest_call(mem<uint32_t>(kTsoCallBack), sound, pos); }

// One of a boned object's two sounds (at +0x44 or +0x48: the sound, a frame, its state halfword at +2). The state's bits
// 0x200 and 0x400 play the sound 0x200 or 0x400 above it once the animation passes the frame (or the state), 0x800
// moves the state back; otherwise the sound plays when the animation passes the frame and the state is 0, and the state
// becomes the frame the animation has reached. The state is cleared when the animation is behind it.
void bonedSound(uint32_t ref, uint32_t at, int32_t sound, uint32_t pos)
{
    const uint16 state = mem<uint16>(at + 2u);
    const uint32_t ctrl = mem<uint32_t>(ref + 0x38u);
    if ((state & 0x200) != 0)
    {
        if (static_cast<int32_t>(mem<uint8>(at + 1u)) < mem<int32_t>(ctrl + 0x18u))
        {
            callBack(sound + 0x200, pos);
            mem<uint16>(at + 2u) = static_cast<uint16>(mem<uint16>(at + 2u) + 0x200);
        }
        return;
    }
    if ((state & 0x400) != 0)
    {
        if (static_cast<int16_t>(state) - 0x400 < mem<int32_t>(ctrl + 0x18u))
        {
            callBack(sound + 0x400, pos);
            mem<uint16>(at + 2u) = static_cast<uint16>(mem<uint16>(at + 2u) + 0x400);
        }
        return;
    }
    if ((state & 0x800) != 0)
    {
        if (static_cast<int16_t>(state) - 0x800 > mem<int32_t>(ctrl + 0x18u))
            mem<uint16>(at + 2u) = static_cast<uint16>(state - 0x600);
        return;
    }
    uint32_t anim = ctrl;
    int32_t now = static_cast<int16_t>(state);
    if (static_cast<int32_t>(mem<uint8>(at + 1u)) < mem<int32_t>(ctrl + 0x18u))
    {
        if (now == 0)
        {
            callBack(sound, pos);
            anim = mem<uint32_t>(ref + 0x38u);
            const uint16 frame = mem<uint16>(anim + 0x18u);
            mem<uint16>(at + 2u) = frame;
            now = static_cast<int16_t>(frame);
        }
        else if (mem<int32_t>(ctrl + 0x18u) < now)
        {
            mem<uint16>(at + 2u) = 0;
            return;
        }
        else
            return;
    }
    if (mem<int32_t>(anim + 0x18u) < now)
        mem<uint16>(at + 2u) = 0;
}

} // namespace

// The sounds of a boned (skinned) object: the first (+0x44) and, unless the first's state is 0x4000, the second (+0x48).
// With the first's state 0x4000, the second's frame counts how many times the first sound has played: it plays when
// the animation passes the first's frame, each time a sound one higher (from +0x49, wrapping back to the first's
// sound past +0x48's limit).
void TSOPlaySfxBoned(ptr32<_sRSRef> pRef, ptr32<vec32_> pPos)
{
    GuestFrame frame(0x40);
    const uint32_t ref = pRef.address, pos = pPos.address;
    bool special = false;
    const int32_t first = mem<int8>(ref + 0x44u);
    if (first != -1)
    {
        if (mem<int16>(ref + 0x4au) == 0x4000)
        {
            special = true;
            uint32_t ctrl = mem<uint32_t>(ref + 0x38u);
            int32_t now;
            if (static_cast<int32_t>(mem<uint8>(ref + 0x45u)) < mem<int32_t>(ctrl + 0x18u))
            {
                now = mem<int16>(ref + 0x46u);
                if (now == 0)
                {
                    callBack(mem<uint8>(ref + 0x49u), pos);
                    const uint8 next = static_cast<uint8>(mem<uint8>(ref + 0x49u) + 1);
                    ctrl = mem<uint32_t>(ref + 0x38u);
                    const int32_t limit = mem<int8>(ref + 0x48u);
                    const uint16 frameNow = mem<uint16>(ctrl + 0x18u);
                    mem<uint8>(ref + 0x49u) = next;
                    mem<uint16>(ref + 0x46u) = frameNow;
                    if (limit < static_cast<int32_t>(next))
                        mem<uint8>(ref + 0x49u) = mem<uint8>(ref + 0x44u);
                    now = static_cast<int16_t>(frameNow);
                }
            }
            else
                now = static_cast<int16_t>(mem<uint16>(ref + 0x46u));
            if (mem<int32_t>(ctrl + 0x18u) < now)
                mem<uint16>(ref + 0x46u) = 0;
        }
        else
            bonedSound(ref, ref + 0x44u, first, pos);
    }
    if (special)
        return;
    const int32_t second = mem<int8>(ref + 0x48u);
    if (second != -1)
        bonedSound(ref, ref + 0x48u, second, pos);
}

// The sounds of a vertex-animated object: the first (+0x18: the sound, a frame, its state at +0x1a) plays when the
// animation (the float frame at +8) passes the frame and the state is 0, the state then the whole frame reached; it is
// cleared when the animation is behind it. The second (+0x1c) does the same with its own fields when the animation runs
// backwards (flag 0x10) and is not at 0; running forwards, it is the first sound's fields that are tested and the first
// sound that plays (a game bug, kept).
void TSOPlaySfxVert(ptr32<_sVertAnimInstance> pRef, ptr32<vec32_> pPos)
{
    GuestFrame frame(0x30);
    const uint32_t ref = pRef.address, pos = pPos.address;
    const auto wholeFrame = [&] { return static_cast<uint32_t>(ee_ftoi(mem<ee_float>(ref + 8u))) & 0xffu; };
    const int32_t first = mem<int8>(ref + 0x18u);
    if (first != -1)
    {
        ee_float at = mem<ee_float>(ref + 8u);
        uint16 state = mem<uint16>(ref + 0x1au);
        if (ee_itof(mem<int8>(ref + 0x19u)) < at && mem<int16>(ref + 0x1au) == 0)
        {
            callBack(first, pos);
            at = mem<ee_float>(ref + 8u);
            state = static_cast<uint16>(wholeFrame());
            mem<uint16>(ref + 0x1au) = state;
        }
        if (at < ee_itof(static_cast<int16_t>(state)))
            mem<uint16>(ref + 0x1au) = 0;
    }
    const int32_t second = mem<int8>(ref + 0x1cu);
    if (second == -1)
        return;
    if ((mem<uint32_t>(mem<uint32_t>(ref) + 0x7cu) & 0x10u) != 0)
    {
        const ee_float at = mem<ee_float>(ref + 8u);
        uint16 state = mem<uint16>(ref + 0x1eu);
        if (!(at == ee_float(0.0f)) && at < ee_itof(mem<int8>(ref + 0x1du)) && mem<int16>(ref + 0x1eu) == 0)
        {
            callBack(second, pos);
            state = static_cast<uint16>(wholeFrame());
            mem<uint16>(ref + 0x1eu) = state;
        }
        if (static_cast<int32_t>(static_cast<int16_t>(state)) < static_cast<int32_t>(wholeFrame()))
            mem<uint16>(ref + 0x1eu) = 0;
        return;
    }
    const ee_float at = mem<ee_float>(ref + 8u);
    uint16 state = mem<uint16>(ref + 0x1au);
    if (ee_itof(mem<int8>(ref + 0x19u)) < at && mem<int16>(ref + 0x1au) == 0)
    {
        callBack(mem<int8>(ref + 0x18u), pos);
        state = static_cast<uint16>(wholeFrame());
        mem<uint16>(ref + 0x1au) = state;
    }
    if (static_cast<int32_t>(wholeFrame()) < static_cast<int32_t>(static_cast<int16_t>(state)))
        mem<uint16>(ref + 0x1au) = 0;
}

namespace {

// The object's matrix (fixed point 4.12) as floats, at m.
void matrixToFloats(uint32_t tso, uint32_t m)
{
    for (uint32_t i = 0; i < 9; ++i)
        mem<ee_float>(m + i * 4u) = fixed12(tso + 0x24u + i * 2u);
}

// The shape's matrix and position for drawing (TRdr_VUShape, Rdr_Water): rows from +8, the position at +0x2c. The
// last matrix entry is stored after the position.
void shapePlace(uint32_t tso, uint32_t shape)
{
    const ee_float x = fixed8(tso + 0x10u), y = fixed8(tso + 0x14u), z = fixed8(tso + 0x18u);
    for (uint32_t i = 0; i < 8; ++i)
        mem<ee_float>(shape + 8u + i * 4u) = fixed12(tso + 0x24u + i * 2u);
    mem<ee_float>(shape + 0x2cu) = x;
    mem<ee_float>(shape + 0x30u) = y;
    mem<ee_float>(shape + 0x34u) = z;
    mem<ee_float>(shape + 0x28u) = fixed12(tso + 0x34u);
}

} // namespace

// Draws the level's scenery objects: each one's matrix rebuilt when it changed, then by kind: vertex-animated (the
// frame advanced, the vertices blended between key frames, then drawn as a track shape), boned (the animation
// advanced, posed when its frame changed, drawn skinned), water, or a plain render object. A vertex-animated or water
// shape that is not version 3 ends the whole pass (the objects after it are not drawn and the clearing pass is
// skipped). Objects with flag 0x20 are skipped; each drawn one gets 0x10. Last, every vertex-animated object's "blended"
// bit (0x400) is cleared for the next frame.
void TSORender(tTime time)
{
    GuestFrame frame(0xc0);
    const uint32_t header = mem<uint32_t>(kTsoVars);
    if (header == 0)
        return;
    const uint32_t now = static_cast<uint32_t>(time);
    const ee_float zero = 0.0f, hundred = 100.0f, two = 2.0f;
    uint32_t tso = header + 4u;
    for (uint32_t n = mem<uint32_t>(header); n != 0; --n, tso += 0x3cu)
    {
        const uint16 flags = mem<uint16>(tso + 0x22u);
        if ((flags & 0x20) != 0)
        {
            mem<uint16>(tso + 0x22u) = static_cast<uint16>(flags & 0xffee);
            continue;
        }
        if ((flags & 2) != 0 && (mem<uint32_t>(tso + 0x20u) & 0xc00000u) == 0)
        {
            uint16 updated;
            if ((flags & 4) != 0)
            {
                AngleAxisToMatrix(ptr32<_matPsx>(tso + 0x24u), ptr32<vec16_>(tso + 0x1cu), mem<int16>(tso + 0x36u));
                updated = mem<uint16>(tso + 0x22u);
            }
            else if ((flags & 8) != 0)
            {
                guest_call(kMat3nBuild, tso + 0x24u, tso + 0x1cu);
                updated = mem<uint16>(tso + 0x22u);
            }
            else
                updated = mem<uint16>(tso + 0x22u);
            mem<uint16>(tso + 0x22u) = static_cast<uint16>(updated & 0xfffd);
        }
        const uint32_t ref = mem<uint32_t>(tso + 0xcu);
        if (ref == 0)
        {
            mem<uint16>(tso + 0x22u) = static_cast<uint16>(mem<uint16>(tso + 0x22u) | 0x10);
            continue;
        }
        if ((flags & 0x40) != 0)
        {
            // Vertex animation: ref is the instance, *ref the shape with its animation fields.
            if (hundred < utof(now - mem<uint32_t>(ref + 0x10u)))
                mem<uint32_t>(ref + 0x10u) = now;
            TSOPlaySfxVert(ptr32<_sVertAnimInstance>(ref), ptr32<vec32_>(tso + 0x10u));
            const uint32_t shape = mem<uint32_t>(ref);
            const uint32_t shapeFlags = mem<uint32_t>(shape + 0x7cu);
            const ee_float shapeFrame = mem<ee_float>(shape + 0x78u);
            mem<uint32_t>(ref + 4u) = shapeFlags;
            mem<ee_float>(ref + 8u) = shapeFrame;
            uint32_t bits = mem<uint32_t>(shape + 0x7cu);
            if ((bits & 0x80u) != 0)
            {
                mem<uint32_t>(shape + 0x78u) = 0;
                mem<uint32_t>(shape + 0x7cu) = bits & 0xffffff7fu;
                bits = mem<uint32_t>(shape + 0x7cu);
            }
            if ((bits & 0x400u) == 0 && (bits & 0x40u) != 0)
            {
                ee_float at;
                const ee_float step = utof(now - mem<uint32_t>(ref + 0x10u)) / utof(mem<uint32_t>(ref + 0xcu));
                if ((bits & 0x10u) != 0)
                {
                    // Backwards: past the start it bounces (0xc), wraps (4) or stops (flag 2).
                    mem<ee_float>(shape + 0x78u) = mem<ee_float>(shape + 0x78u) - step;
                    at = mem<ee_float>(shape + 0x78u);
                    if (at < zero)
                    {
                        const uint32_t b = mem<uint32_t>(shape + 0x7cu);
                        if ((b & 0xcu) == 0xcu)
                        {
                            mem<uint32_t>(shape + 0x7cu) = b & 0xffffffefu;
                            mem<ee_float>(shape + 0x78u) = -at;
                        }
                        else if ((b & 4u) != 0)
                            mem<ee_float>(shape + 0x78u) = mem<ee_float>(shape + 0x78u) + utof(mem<uint32_t>(shape + 0x74u));
                        else
                        {
                            mem<uint32_t>(shape + 0x78u) = 0;
                            mem<uint32_t>(shape + 0x7cu) = (b & 0xffffffbfu) | 2u;
                        }
                        at = mem<ee_float>(shape + 0x78u);
                    }
                }
                else
                {
                    // Forwards: within two frames of the end it bounces (8), wraps (4) or stops (flag 2).
                    const uint32_t frames = mem<uint32_t>(shape + 0x74u);
                    mem<ee_float>(shape + 0x78u) = mem<ee_float>(shape + 0x78u) + step;
                    const ee_float last = utof(frames) - two;
                    if (last <= mem<ee_float>(shape + 0x78u))
                    {
                        const uint32_t b = mem<uint32_t>(shape + 0x7cu);
                        if ((b & 8u) != 0)
                        {
                            const uint32_t count = mem<uint32_t>(shape + 0x74u);
                            mem<uint32_t>(shape + 0x7cu) = b | 0x10u;
                            const ee_float end = utof(count) - two;
                            const ee_float current = mem<ee_float>(shape + 0x78u);
                            const ee_float over = current - end;
                            mem<ee_float>(shape + 0x78u) = current - over;
                        }
                        else if ((b & 4u) != 0)
                            mem<ee_float>(shape + 0x78u) = mem<ee_float>(shape + 0x78u) - utof(mem<uint32_t>(shape + 0x74u));
                        else
                        {
                            const ee_float end = utof(mem<uint32_t>(shape + 0x74u)) - two;
                            const uint32_t cleared = (mem<uint32_t>(shape + 0x7cu) & 0xffffffbfu) | 2u;
                            mem<ee_float>(shape + 0x78u) = end;
                            mem<uint32_t>(shape + 0x7cu) = cleared;
                        }
                    }
                    at = mem<ee_float>(shape + 0x78u);
                }
                mem<ee_float>(ref + 8u) = at;
                // The vertices: key frame `key` blended towards the next by the fraction (scaled by the frames since
                // the vertex's own key).
                const ee_float frameNow = mem<ee_float>(shape + 0x78u);
                const uint32_t key = guest_call<uint32_t>(sym::fptoui, frameNow);
                const ee_float frac = frameNow - utof(key);
                for (uint32_t k = 0; k < mem<uint8>(shape + 0x5du) * 3u; ++k)
                {
                    const uint32_t keyList = mem<uint32_t>(mem<uint32_t>(shape + 0x80u) + key * 4u);
                    const uint32_t vertex = mem<uint32_t>(keyList + k * 4u);
                    const uint32_t delta = mem<uint32_t>(vertex + 4u), since = mem<uint32_t>(vertex + 8u), base = mem<uint32_t>(vertex);
                    const ee_float scale = utof(key - since + 1u);
                    const ee_float x = mem<ee_float>(base) + mem<ee_float>(delta) * frac * scale;
                    mem<ee_float>(kRenderDelta) = x;
                    const ee_float y = mem<ee_float>(base + 4u) + mem<ee_float>(delta + 4u) * frac * scale;
                    mem<ee_float>(kRenderDelta + 4u) = y;
                    const ee_float zd = mem<ee_float>(delta + 8u) * frac * scale;
                    const uint32_t dst = mem<uint32_t>(mem<uint32_t>(mem<uint32_t>(shape + 0x84u) + k * 4u));
                    const ee_float z = mem<ee_float>(base + 8u) + zd;
                    const ee_float dx = mem<ee_float>(kRenderDelta), dy = mem<ee_float>(kRenderDelta + 4u);
                    mem<ee_float>(kRenderDelta + 8u) = z;
                    mem<ee_float>(dst + 8u) = z;
                    mem<ee_float>(dst) = dx;
                    mem<ee_float>(dst + 4u) = dy;
                }
                mem<uint32_t>(shape + 0x7cu) = mem<uint32_t>(shape + 0x7cu) | 0x400u;
            }
            if (!(mem<ee_float>(shape) == ee_float(3.0f)))
                return;
            shapePlace(tso, shape);
            guest_call(sym::TRdr_VUShape, shape, kRdrCamera, 0);
            mem<uint32_t>(ref + 0x10u) = now;
        }
        else if ((flags & 0x80) != 0)
        {
            // Boned: ref is the skinned instance; its animation control at +0x38, its output at +0x3c.
            const ee_float x = fixed8(tso + 0x10u), y = fixed8(tso + 0x14u), z = fixed8(tso + 0x18u);
            const uint32_t bits = mem<uint32_t>(ref + 0x40u);
            mem<ee_float>(ref + 0x24u) = x;
            mem<ee_float>(ref + 0x28u) = y;
            mem<ee_float>(ref + 0x2cu) = z;
            if ((bits & 0x80u) != 0)
            {
                mem<uint32_t>(ref + 0x30u) = 0;
                mem<uint32_t>(ref + 0x40u) = bits & 0xffffff7fu;
            }
            const uint32_t ctrl = mem<uint32_t>(ref + 0x38u);
            if (now != mem<uint32_t>(ctrl + 4u))
            {
                const ee_float since = ee_itof(static_cast<int32_t>(now - mem<uint32_t>(ctrl + 4u)));
                if (since < zero || hundred < since)
                    mem<uint32_t>(ctrl + 4u) = now;
                const uint32_t b = mem<uint32_t>(ref + 0x40u);
                if ((b & 0x40u) != 0)
                {
                    if ((b & 0x10u) != 0)
                    {
                        // Backwards: bounce (0xc), wrap (4) or stop (flag 2) before the start.
                        const ee_float step = ee_itof(static_cast<int32_t>(now - mem<uint32_t>(ctrl + 4u))) / mem<ee_float>(ctrl + 0x10u);
                        const ee_float at = mem<ee_float>(ref + 0x30u) - step;
                        mem<ee_float>(ref + 0x30u) = at;
                        if (at < zero)
                        {
                            if ((b & 0xcu) == 0xcu)
                            {
                                mem<uint32_t>(ref + 0x40u) = b & 0xffffffefu;
                                mem<ee_float>(ref + 0x30u) = -at;
                            }
                            else if ((b & 4u) != 0)
                                mem<ee_float>(ref + 0x30u) = mem<ee_float>(ref + 0x30u) + utof(mem<uint32_t>(mem<uint32_t>(ctrl) + 4u));
                            else
                            {
                                mem<uint32_t>(ref + 0x30u) = 0;
                                mem<uint32_t>(ref + 0x40u) = (b | 2u) & 0xffffffbfu;
                            }
                        }
                    }
                    else
                    {
                        // Forwards: bounce (8), wrap (4) or stop (flag 2) at the last frame.
                        const uint32_t since2 = now - mem<uint32_t>(ctrl + 4u);
                        const ee_float speed = mem<ee_float>(ctrl + 0x10u);
                        const ee_float current = mem<ee_float>(ref + 0x30u);
                        const uint32_t count = mem<uint32_t>(mem<uint32_t>(ctrl) + 4u);
                        mem<ee_float>(ref + 0x30u) = current + ee_itof(static_cast<int32_t>(since2)) / speed;
                        const ee_float last = utof(count - 1u);
                        const ee_float at = mem<ee_float>(ref + 0x30u);
                        if (last <= at)
                        {
                            if ((b & 8u) != 0)
                            {
                                mem<uint32_t>(ref + 0x40u) = b | 0x10u;
                                const ee_float back = at - utof(mem<uint32_t>(mem<uint32_t>(ctrl) + 4u) - 1u);
                                mem<ee_float>(ref + 0x30u) = at - back;
                            }
                            else if ((b & 4u) != 0)
                                mem<ee_float>(ref + 0x30u) = at - utof(mem<uint32_t>(mem<uint32_t>(ctrl) + 4u));
                            else
                            {
                                mem<ee_float>(ref + 0x30u) = utof(mem<uint32_t>(mem<uint32_t>(ctrl) + 4u) - 1u);
                                mem<uint32_t>(ref + 0x40u) = (b | 2u) & 0xffffffbfu;
                            }
                        }
                    }
                }
            }
            TSOPlaySfxBoned(ptr32<_sRSRef>(ref), ptr32<vec32_>(tso + 0x10u));
            const uint32_t place = ref + 0x24u;
            const ee_float radius = mem<ee_float>(mem<uint32_t>(mem<uint32_t>(ref + 0x34u) + 8u) + 0x40u);
            if (IsInViewCone(ptr32<_sVec3>(place), radius) == ee_float(-1.0f))
            {
                mem<uint32_t>(mem<uint32_t>(ref + 0x38u) + 4u) = now;
            }
            else
            {
                const uint32_t frameNow = guest_call<uint32_t>(sym::fptoui, mem<ee_float>(ref + 0x30u));
                const uint32_t control = mem<uint32_t>(ref + 0x38u);
                if (mem<uint32_t>(control + 0x18u) != frameNow)
                {
                    mem<uint32_t>(control + 0x18u) = frameNow;
                    const uint32_t output = mem<uint32_t>(ref + 0x3cu);
                    matrixToFloats(tso, ref);
                    guest_call(sym::MxAnim_OutputFlatFrame, output, control, now, place, ref);
                }
                guest_call(sym::SkinAnimRdr, ref, now);
                mem<uint32_t>(mem<uint32_t>(ref + 0x38u) + 4u) = now;
            }
        }
        else if ((flags & 0x200) != 0)
        {
            // Water: ref is the shape itself.
            if (!(mem<ee_float>(ref) == ee_float(3.0f)))
                return;
            shapePlace(tso, ref);
            guest_call(sym::Rdr_Water, ref, kRdrCamera);
        }
        else
            guest_call(sym::RendObjRender, tso + 0xcu, 0, 0, 0);
        mem<uint16>(tso + 0x22u) = static_cast<uint16>(mem<uint16>(tso + 0x22u) | 0x10);
    }
    tso = header + 4u;
    for (uint32_t n = mem<uint32_t>(header); n != 0; --n, tso += 0x3cu)
    {
        const uint32_t ref = mem<uint32_t>(tso + 0xcu);
        if (ref != 0 && (mem<uint16>(tso + 0x22u) & 0x40) != 0)
        {
            const uint32_t shape = mem<uint32_t>(ref);
            mem<uint32_t>(shape + 0x7cu) = mem<uint32_t>(shape + 0x7cu) & 0xfffffbffu;
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
// Drawing and the sounds (RevSfxTSO called back, with the IOP's answers scripted offline): the level's own objects.
RC_DECOMP_FN_SETUP_HEAP(TSORender, rc::fuzz::tsoRender)
RC_DECOMP_FN_SETUP(TSOPlaySfxBoned, rc::fuzz::tsoSfxBoned)
RC_DECOMP_FN_SETUP(TSOPlaySfxVert, rc::fuzz::tsoSfxVert)

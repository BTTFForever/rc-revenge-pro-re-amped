// Decompiled from TBOX.c: trigger boxes. A level's boxes (_sTBox, 0x18 bytes: interest mask, pitch, head, centre,
// half sizes, data type, data offset) are found through a grid of cells over the level, each a list of box indices.
// TBoxUpdate tests a point (a camera's or a car's position, in 1/256 units) against the boxes of its cell whose mask
// matches (any of the low 24 bits; all of the top 8), turned into the box's frame if it is rotated, and calls the box
// type's callback with the box and the object. A box's data sits at &dataOffset + dataOffset.
//
// Written from the instructions.

#include "physics_fuzz.h"
#include "weapon.h"
#include "../rc_cars.h"

namespace rc {

void PS2C_Vec16MulMat(ptr32<_sMat> pMat, ptr32<_sVec16> pIn, ptr32<_sVec32> pOut); // psxdefs.cpp
void CameraFollowPlayer(ptr32<_sCamera> pCamera, bool_ goStraightThere, bool_ doCollision); // camera.cpp
void CameraSetMode(ptr32<_sCamera> pCamera, int32 mode);
void CameraSetNear(ptr32<_sCamera> pCamera);
void CameraSetTunnelMode(ptr32<_sCamera> pCamera);

void TBoxUpdate(ptr32<vec32> pPos, uint32 mask, ptr32<void> pData);
void TBoxPreProcess();

namespace {

using weapon::at;

constexpr uint32_t kTBoxVars = sym::tBoxVarG, kFns = kTBoxVars + 4u, kScratch = sym::scratchPadG, kLsParent = sym::lsVarG;
constexpr uint32_t kCameras = sym::cameraVarG, kCameraSize = 0x5b8u, kMainVars = sym::mainVarG, kObjectList = sym::objectVarG;
constexpr uint32_t kRaceOrder = cars::kRaceOrder /* enhancement: aiVarG.raceOrder moved for more than 8 cars (rc_cars.h) */, kRevSfx = sym::revSfxVarG, kReadySteadyGo = (sym::hudVarG + 0x70u), kLevelPtr = sym::pLvlG;
constexpr uint32_t kSfxSetReverbDepth = sym::SfxSetReverbDepth, kRevSfxTrigger = sym::RevSfxTrigger;

uint32_t boxData(uint32_t box) { return box + 0x16u + at<uint16_t>(box + 0x16u); }

// The camera an object's trigger moves: player 2's if bit 3 of its first word is set.
uint32_t cameraFor(ptr32<void> pData) { return (at<uint32_t>(pData.address) & 8u) ? kCameras + kCameraSize : kCameras; }

int32_t absWord(int32_t v) { return v < 0 ? static_cast<int32_t>(0u - static_cast<uint32_t>(v)) : v; }

uint8_t clampByte(int32_t v) { return static_cast<uint8_t>(v < 0 ? 0 : v < 0x100 ? v : 0xff); }

} // namespace

// Reverb by position along the box (the scratch z offset from TBoxUpdate), between its two levels.
void TBoxReverbCallback(ptr32<_sTBox> pTBox, ptr32<void>)
{
    const uint32_t box = pTBox.address, data = boxData(box);
    if (at<uint8_t>(kMainVars + 0x368u) != 0 || at<int32_t>(kReadySteadyGo) != 0 || at<uint16_t>(kMainVars + 0x366u) != 0)
        return;
    if (at<int32_t>(kMainVars + 0x174u) != 0) // gameEndPos
    {
        if (at<int32_t>(kRevSfx + 0x1140u) == 0)
            return;
        at<int32_t>(kRevSfx + 0x1140u) = 0;
        guest_call(kSfxSetReverbDepth, 0); // a tail call
        return;
    }
    const int32_t z = at<int32_t>(kScratch + 0x18u);
    at<int32_t>(kRevSfx + 0x1140u) = z < 0 ? at<uint8_t>(data) : at<uint8_t>(data + 1u);
    const int32_t high = at<uint8_t>(data + 1u), low = at<uint8_t>(data);
    int32_t span = high - low;
    if (span <= 0)
        span = low - high;
    const int32_t maxZ = at<int16_t>(box + 0x12u);
    const int32_t step = ee_div(span << 8, maxZ << 1);
    const int32_t along = static_cast<int32_t>(static_cast<uint32_t>(step) * static_cast<uint32_t>(maxZ + z)) >> 8;
    const int32_t depth = at<uint8_t>(data + 1u) < at<uint8_t>(data) ? low - along : along + low;
    guest_call(kSfxSetReverbDepth, depth); // a tail call
}

void TBoxSfxCallback(ptr32<_sTBox> pTBox, ptr32<void>)
{
    const uint32_t box = pTBox.address;
    guest_call(kRevSfxTrigger, static_cast<int32_t>(at<int16_t>(boxData(box) + 2u)), box + 8u); // a tail call
}

// Camera boxes: back to following the car (flag 1); a tracking camera at the box's point (2, zoomed with 8); a fixed
// camera there looking the box's way (4, zoomed with 8). The point and angles are the data's first six halfwords.
void TBoxCamCallback(ptr32<_sTBox> pTBox, ptr32<void> pData)
{
    const uint32_t data = boxData(pTBox.address);
    const uint32_t cam = cameraFor(pData);
    const ptr32<_sCamera> camera(cam);
    const uint16_t flags = at<uint16_t>(data + 6u);
    const bool split = at<uint8_t>(kMainVars + 0x368u) != 0;
    if (flags & 1u)
    {
        if (at<int32_t>(cam) == 0) // already CAMERA_MODE_GAME_FOLLOW_PLAYER
            return;
        {
            GuestFrame frame(0x40);
            CameraSetMode(camera, 0);
            CameraFollowPlayer(camera, 1, 1);
            at<int32_t>(cam + 4u) = 1; // near
        }
        CameraSetNear(camera); // a tail call
        return;
    }
    if ((flags & 2u) && !split)
    {
        at<int32_t>(cam + 0xcu) = at<int16_t>(data + 0u) << 8;
        at<int32_t>(cam + 0x10u) = at<int16_t>(data + 2u) << 8;
        at<int32_t>(cam + 0x14u) = at<int16_t>(data + 4u) << 8;
        GuestFrame frame(0x40);
        CameraSetMode(camera, (flags & 8u) ? 8 : 3);
        at<uint32_t>(cam + 0xacu) = pData.address; // pTarget
        return;
    }
    if (!(flags & 4u) || split)
        return;
    at<int32_t>(cam + 0xcu) = at<int16_t>(data + 0u) << 8;
    at<int32_t>(cam + 0x10u) = at<int16_t>(data + 2u) << 8;
    at<int16_t>(cam + 0xa0u) = static_cast<int16_t>(0u - at<uint16_t>(data + 8u));
    at<int32_t>(cam + 0x14u) = at<int16_t>(data + 4u) << 8;
    at<int16_t>(cam + 0xa4u) = 0;
    at<int16_t>(cam + 0xa2u) = static_cast<int16_t>(0u - at<uint16_t>(data + 0xau));
    {
        GuestFrame frame(0x40);
        guest_call(weapon::kMat3nBuildXYZ, cam + 0x24u, cam + 0xa0u);
        if (at<uint16_t>(data + 6u) & 8u)
        {
            CameraSetMode(camera, 10);
            at<uint32_t>(cam + 0xacu) = pData.address;
            return;
        }
    }
    CameraSetMode(camera, 9); // a tail call
}

// Animation boxes: the data holds an animation (flags at +0x22, its state word found through +0xc) and an action: 0
// start, 1 stop, 2 to 4 other state changes; the animation's own flags (+8) are set for the first three.
void TBoxAnimCallback(ptr32<_sTBox> pTBox, ptr32<void>)
{
    GuestFrame frame(0x10);
    const uint32_t data = boxData(pTBox.address);
    const uint32_t anim = at<uint32_t>(data);
    if (anim == 0)
        return;
    if (at<uint16_t>(data + 6u) == 1)
    {
        const uint16_t animFlags = at<uint16_t>(anim + 0x22u);
        // Neither flag: a stack word the original never set (its "noFlags" local).
        uint32_t word = frame.local<uint8_t>(0).address;
        if (animFlags & 0x80u)
            word = at<uint32_t>(anim + 0xcu) + 0x40u;
        else if (animFlags & 0x40u)
            word = at<uint32_t>(at<uint32_t>(anim + 0xcu)) + 0x7cu;
        uint32_t &w = at<uint32_t>(word);
        switch (at<uint16_t>(data + 4u))
        {
        case 0:
            if (!(w & 0x40u) && (w & 3u) != 3u)
                w = w | 0xc0u;
            break;
        case 1:
            if (w & 0x40u)
                w = (w & 0xffffffbfu) | 0x200u;
            break;
        case 2: w = w | 0x40u; break;
        case 3: w = (w | 0x40u) & 0xffffffefu; break;
        case 4:
            if (!(w & 0x40u))
                w = w | 0x50u;
            break;
        default: break;
        }
    }
    if (at<uint32_t>(anim) == 0)
        return;
    uint16_t &f = at<uint16_t>(anim + 8u);
    switch (at<uint16_t>(data + 4u))
    {
    case 0:
        if (!(f & 0x40u))
            f = static_cast<uint16_t>(f | 0x80u);
        break;
    case 1:
        if (f & 0x40u)
            f = static_cast<uint16_t>(f | 0x200u);
        break;
    case 2:
        if (!(f & 0x40u))
            f = static_cast<uint16_t>(f | 0x40u);
        break;
    default: break;
    }
}

void TBoxTunnelCallback(ptr32<_sTBox>, ptr32<void> pData)
{
    CameraSetTunnelMode(ptr32<_sCamera>(cameraFor(pData))); // a tail call
}

void TBoxInit()
{
    at<uint32_t>(kFns + 0x28u) = sym::TBoxColCallback; // TBoxColCallback
    at<uint32_t>(kFns + 0x00u) = sym::TBoxReverbCallback; // TBoxReverbCallback
    at<uint32_t>(kFns + 0x04u) = sym::TBoxEnvCallback; // TBoxEnvCallback
    at<uint32_t>(kFns + 0x08u) = sym::TBoxSfxCallback; // TBoxSfxCallback
    at<uint32_t>(kFns + 0x0cu) = sym::TBoxVfxCallback; // TBoxVfxCallback
    at<uint32_t>(kFns + 0x10u) = sym::TBoxCamCallback; // TBoxCamCallback
    at<uint32_t>(kFns + 0x14u) = sym::TBoxAnimCallback; // TBoxAnimCallback
    at<uint32_t>(kFns + 0x18u) = sym::TBoxTintCallback; // TBoxTintCallback
    at<uint32_t>(kFns + 0x1cu) = sym::TBoxAmbCallback; // TBoxAmbCallback
    at<uint32_t>(kFns + 0x20u) = sym::TBoxXACallback; // TBoxXACallback
    at<uint32_t>(kFns + 0x24u) = sym::TBoxTunnelCallback; // TBoxTunnelCallback
}

void TBoxTerm() {}

// The level's boxes; each sound box (type 2) gets a trigger record, its data's second halfword becoming the record's
// index.
void TBoxSetupClass()
{
    const uint32_t grid = at<uint32_t>(at<uint32_t>(kLevelPtr) + 0x54u);
    at<uint32_t>(kTBoxVars) = grid;
    uint32_t &count = at<uint32_t>(kRevSfx + 0x1184u);
    count = 0;
    if (grid == 0)
        return;
    for (uint32_t i = 0; i < at<uint32_t>(grid); ++i)
    {
        const uint32_t box = at<uint32_t>(grid + 8u) + i * 0x18u;
        if (at<uint16_t>(box + 0x14u) != 2)
            continue;
        const uint32_t data = boxData(box);
        const uint32_t trigger = kRevSfx + count * 8u;
        at<uint16_t>(trigger + 0x1188u) = at<uint16_t>(data);
        at<uint32_t>(trigger + 0x118cu) = 0;
        at<uint16_t>(trigger + 0x118au) = at<uint16_t>(data + 2u);
        const uint16_t index = static_cast<uint16_t>(count);
        count = count + 1u;
        at<uint16_t>(data + 2u) = index;
    }
}

void TBoxColCallback(ptr32<_sTBox> pTBox, ptr32<void> pData)
{
    const int32_t n = at<int32_t>(pTBox.address + at<uint16_t>(pTBox.address + 0x16u) + 0x16u);
    at<int32_t>(pData.address + static_cast<uint32_t>(n - 1) * 4u + 0x1fcu) = 1; // inTrigger[n - 1]
}

void TBoxEnvCallback(ptr32<_sTBox> pTBox, ptr32<void> pData)
{
    const uint32_t target = at<uint32_t>(at<uint32_t>(pData.address + 0x18u));
    const uint32_t data = boxData(pTBox.address);
    at<uint32_t>(target + 0x60u) = at<uint32_t>(data);
    at<uint8_t>(target + 0x5eu) = at<uint8_t>(data + 4u);
    at<uint8_t>(target + 0x5fu) = at<uint8_t>(data + 5u);
}

void TBoxVfxCallback(ptr32<_sTBox>, ptr32<void>) {}

// Tint boxes add their four signed amounts to the object's tint bytes (+8 to +0xb).
void TBoxTintCallback(ptr32<_sTBox> pTBox, ptr32<void> pData)
{
    const uint32_t data = boxData(pTBox.address);
    for (uint32_t k = 0; k < 4; ++k)
        at<uint8_t>(pData.address + 8u + k) = clampByte(at<uint8_t>(pData.address + 8u + k) + at<int16_t>(data + 2u * k));
}

void TBoxAmbCallback(ptr32<_sTBox>, ptr32<void>) {}

void TBoxXACallback(ptr32<_sTBox>, ptr32<void>) {}

void TBoxPreProcess()
{
    int32_t i = 0;
    do
        at<uint32_t>(kCameras + static_cast<uint32_t>(i) * kCameraSize + 0x57cu) = 0; // inTunnel
    while (!(static_cast<int32_t>(at<uint8_t>(kMainVars + 0x368u)) < ++i));
}

// The cameras (masks 0xff000001 and 0xff000002), then every car in race order (its mask and bit 16 + its place), its
// trigger flags cleared first.
void TBoxProcess()
{
    GuestFrame frame(0x70);
    const uint32_t sp = frame.local<uint8_t>(0).address;
    TBoxPreProcess();
    TBoxUpdate(ptr32<vec32>(kCameras + 0xcu), 0xff000001u, ptr32<void>(kCameras));
    if (at<uint8_t>(kMainVars + 0x368u) != 0)
        TBoxUpdate(ptr32<vec32>(kCameras + kCameraSize + 0xcu), 0xff000002u, ptr32<void>(kCameras + kCameraSize));
    for (uint32_t i = 0; i < at<uint32_t>(kObjectList + 0x170u); ++i)
    {
        const uint32_t obj = at<uint32_t>(kRaceOrder + i * 12u);
        // Bits 16-23: the car's place (the top byte is the cameras'). Enhancement (rc_cars.h): from 9th on a car takes 8th's bit;
        // its own (bit 24 and up) made the trigger boxes take it for a camera (the race camera stuck at a trackside view).
        const uint32_t mask = at<uint32_t>(obj) | (0x10000u << (i < 8u ? i : 7u));
        for (uint32_t off = 0x1fcu; off <= 0x218u; off += 4u)
            at<uint32_t>(obj + off) = 0;
        for (uint32_t k = 0; k < 3; ++k)
            at<int32_t>(sp + 0x10u + 4u * k) = ee_ftoi(at<ee_float>(at<uint32_t>(obj + 0x20u) + 8u + 4u * k));
        std::memcpy(guest_host(sp), guest_host(sp + 0x10u), 12);
        TBoxUpdate(ptr32<vec32>(sp), mask, ptr32<void>(obj));
    }
}

void TBoxUpdate(ptr32<vec32> pPos, uint32 mask, ptr32<void> pData)
{
    GuestFrame frame(0xa0);
    const uint32_t grid = at<uint32_t>(kTBoxVars);
    const uint32_t parent = at<uint32_t>(kLsParent);
    if (grid == 0)
        return;
    const uint32_t s = kScratch;
    at<int16_t>(s + 4u) = static_cast<int16_t>(at<int32_t>(pPos.address + 8u) >> 8);
    at<int16_t>(s + 0u) = static_cast<int16_t>(at<int32_t>(pPos.address + 0u) >> 8);
    at<int16_t>(s + 2u) = static_cast<int16_t>(at<int32_t>(pPos.address + 4u) >> 8);
    const uint32_t shift = static_cast<uint32_t>(at<int16_t>(grid + 4u)) & 31u;
    const uint32_t widthShift = static_cast<uint32_t>(at<int16_t>(grid + 6u)) & 31u;
    const int32_t col = (at<int16_t>(s + 0u) - at<int32_t>(parent + 0x58u)) >> shift;
    const int32_t row = (at<int16_t>(s + 4u) - at<int32_t>(parent + 0x5cu)) >> shift;
    const uint16_t cell = at<uint16_t>(grid + static_cast<uint32_t>((static_cast<uint32_t>(col) << widthShift) + static_cast<uint32_t>(row)) * 2u + 0xcu);
    const uint32_t boxes = at<uint32_t>(grid + 8u);
    if (cell == 0)
        return;
    uint32_t list = grid + cell;
    const uint16_t count = at<uint16_t>(list);
    for (uint32_t j = 0; j < count; ++j)
    {
        list += 2u;
        const uint32_t box = at<uint16_t>(list) * 0x18u + boxes;
        const uint32_t interest = at<uint32_t>(box);
        if ((interest & 0xffffffu & mask) == 0)
            continue;
        const uint32_t top = interest & 0xff000000u;
        if ((top & mask) != top)
            continue;
        const uint16_t pitch = static_cast<uint16_t>(0u - at<uint16_t>(box + 4u));
        const uint16_t head = static_cast<uint16_t>(0u - at<uint16_t>(box + 6u));
        at<uint16_t>(s + 0x32u) = pitch;
        at<uint16_t>(s + 0x34u) = head;
        if (pitch == 0 && head == 0)
        {
            at<int32_t>(s + 0x10u) = at<int16_t>(s + 0u) - at<int16_t>(box + 8u);
            at<int32_t>(s + 0x14u) = at<int16_t>(s + 2u) - at<int16_t>(box + 0xau);
            at<int32_t>(s + 0x18u) = at<int16_t>(s + 4u) - at<int16_t>(box + 0xcu);
        }
        else
        {
            at<uint16_t>(s + 0x36u) = 0;
            guest_call(weapon::kMat3nBuildXYZ, s + 0x20u, s + 0x32u);
            at<uint16_t>(s + 8u) = static_cast<uint16_t>(at<uint16_t>(s + 0u) - at<uint16_t>(box + 8u));
            at<uint16_t>(s + 0xau) = static_cast<uint16_t>(at<uint16_t>(s + 2u) - at<uint16_t>(box + 0xau));
            at<uint16_t>(s + 0xcu) = static_cast<uint16_t>(at<uint16_t>(s + 4u) - at<uint16_t>(box + 0xcu));
            PS2C_Vec16MulMat(ptr32<_sMat>(s + 0x20u), ptr32<_sVec16>(s + 8u), ptr32<_sVec32>(s + 0x10u));
        }
        if (at<int16_t>(box + 0xeu) < absWord(at<int32_t>(s + 0x10u)))
            continue;
        if (at<int16_t>(box + 0x12u) < absWord(at<int32_t>(s + 0x18u)))
            continue;
        if (at<int16_t>(box + 0x10u) < absWord(at<int32_t>(s + 0x14u)))
            continue;
        guest_call(at<uint32_t>(kFns + 4u * at<uint16_t>(box + 0x14u)), box, pData.address);
    }
}

RC_DECOMP_FN_SETUP_HEAP(TBoxProcess, rc::fuzz::tboxRace)
RC_DECOMP_FN_SETUP_HEAP(TBoxUpdate, rc::fuzz::tboxUpdate)
RC_DECOMP_FN_SETUP_HEAP(TBoxReverbCallback, rc::fuzz::tboxReverb)
RC_DECOMP_FN_SETUP_HEAP(TBoxSfxCallback, rc::fuzz::tboxSfx)
RC_DECOMP_FN_SETUP_HEAP(TBoxCamCallback, rc::fuzz::tboxCam)
RC_DECOMP_FN_SETUP_HEAP(TBoxAnimCallback, rc::fuzz::tboxAnim)
RC_DECOMP_FN_SETUP_HEAP(TBoxTunnelCallback, rc::fuzz::tboxTunnel)
RC_DECOMP_FN(TBoxInit)
RC_DECOMP_FN(TBoxTerm)
RC_DECOMP_FN_SETUP_HEAP(TBoxSetupClass, rc::fuzz::tboxRace)
RC_DECOMP_FN_SETUP_HEAP(TBoxColCallback, rc::fuzz::tboxCol)
RC_DECOMP_FN_SETUP_HEAP(TBoxEnvCallback, rc::fuzz::tboxEnv)
RC_DECOMP_FN(TBoxVfxCallback)
RC_DECOMP_FN_SETUP_HEAP(TBoxTintCallback, rc::fuzz::tboxTint)
RC_DECOMP_FN(TBoxAmbCallback)
RC_DECOMP_FN(TBoxXACallback)
RC_DECOMP_FN_SETUP_HEAP(TBoxPreProcess, rc::fuzz::tboxRace)

} // namespace rc

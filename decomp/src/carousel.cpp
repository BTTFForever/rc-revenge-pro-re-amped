// Decompiled from CAROUSEL.c: the turntable of cars in the front end's car-selection room. Each of its 19 places
// holds a car object (a question mark while the car is locked); the turntable turns to the highlighted car, which
// spins and now and then hops. Animated scenery around it (frontTSOTagList, in groups) stops near the turntable.
//
// Written from the instructions (Ghidra's C as a first reading; CarouselTerm is a tail call to ObjectDeleteAll,
// which Ghidra shows inline).

#include "front.h"
#include "front_fuzz.h"
#include "ee_math.h"

namespace rc {

ee_float ee_sinf(ee_float x); // libm_float.cpp
ee_float ee_cosf(ee_float x);

using namespace front;

namespace {

constexpr uint32_t kTsoFlags = 0x22u;    // sTSO's obj.flags: 1 drawn, 0x20 animation stopped
constexpr uint32_t kTsoX = 0x10u, kTsoZ = 0x18u; // its position
constexpr int32_t kTagEntries = 29;

// The low word of a product, as mult leaves it.
int32_t mul32(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b)); }

uint16_t &tsoFlags(uint32_t tso) { return at<uint16_t>(tso + kTsoFlags); }

// Stops or restarts the animated scenery of a group.
void setAnimGroup(int32 group, bool stop)
{
    const uint32_t list = sym::frontTSOTagList;
    for (int32_t i = 0; i < kTagEntries; ++i)
        if (at<int32_t>(list + 12u * i + 4u) == group)
        {
            uint16_t &flags = tsoFlags(at<uint32_t>(list + 12u * i + 8u));
            flags = static_cast<uint16_t>(stop ? flags | 0x20u : flags & 0xffdfu);
        }
}

} // namespace

// The turntable's cars in their order, the first eleven (but the concept car and the jungle ranger) available.
void CarouselInit()
{
    GuestFrame frame(0x60);
    sCarouselClass &c = carousel();
    c.pTSO = guest_call<ptr32<_sTSO>>(sym::TSOFindByTag, 0x10000263u);
    static const int32_t kTypes[19] = {1, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 14, 15, 16, 17, 18, 19};
    for (int32_t i = 0; i < 19; ++i)
    {
        sCarousel &e = c.carousel[i];
        e.type = static_cast<eObjectType>(kTypes[i]);
        e.flag = (i <= 10 && i != 3 && i != 7) ? 1 : 0;
        e.flag2 = 0;
        e.bBoat = 0;
    }
    tsoFlags(c.pTSO.address) = static_cast<uint16_t>(tsoFlags(c.pTSO.address) | 0x20u);
    const uint32_t list = sym::frontTSOTagList;
    for (int32_t i = 0; i < kTagEntries; ++i)
        at<uint32_t>(list + 12u * i + 8u) = guest_call<uint32_t>(sym::TSOFindByTag, at<uint32_t>(list + 12u * i));
}

void CarouselTerm()
{
    guest_call<void>(sym::ObjectDeleteAll);
}

// Places a car object at each of the turntable's places, evenly round the turntable's centre, on the floor.
void CarouselSetUp(uint32 numSegments, tTime time)
{
    sCarouselClass &c = carousel();
    GuestFrame frame(0xc0);
    const uint32_t separation = ee_divu(0x10000u, numSegments);
    const uint32_t tso = c.pTSO.address;
    c.moveMode = 1;
    c.angle = static_cast<int32>(0xffff0000u);
    c.tgtAngle = static_cast<int32>(0xffff0000u);
    c.numSegments = numSegments;
    c.separation = static_cast<int32>(separation);
    const ptr32<ee_float> pos = frame.local<ee_float>(0);
    uint32_t angle = 0xffff0000u;
    for (uint32_t i = 0; i < numSegments; ++i)
    {
        sCarousel &e = c.carousel[i];
        const ptr32<_sObjectDef> pDef = feLevel()->carDef[e.type - 1];
        const ee_float a = ee_itof(static_cast<int32_t>((angle >> 4) & 0xfffu));
        pDef->pos.x = ee_ftoi(ee_sinf((a + a) * amenu::constant(sym::lit4_327604)) * ee_float(4096.0f));
        const ee_float b = ee_itof(static_cast<int32_t>((angle >> 4) & 0xfffu));
        const int32_t cosine = ee_ftoi(ee_cosf((b + b) * amenu::constant(sym::lit4_327608)) * ee_float(4096.0f));
        const int32_t radius = -G(carouselRadiusG);
        pos[1] = ee_itof(0x514);
        int32_t x = mul32(pDef->pos.x, radius) >> 12;
        int32_t z = mul32(cosine, radius) >> 12;
        pDef->pos.x = x;
        pDef->pos.z = z;
        pDef->pos.y = 0x514;
        x += at<int32_t>(tso + kTsoX) / 256;
        pDef->pos.x = x;
        pos[0] = ee_itof(x);
        z += at<int32_t>(tso + kTsoZ) / 256;
        pos[2] = ee_itof(z);
        pDef->pos.z = z;
        guest_call<int32_t>(sym::AColDownCast, pos, guest_addr(&e.poi), guest_addr(&e.normal), frame.local<uint8_t>(0x10));
        pDef->angles.pitch = 0;
        pDef->angles.roll = 0;
        pDef->angles.head = static_cast<int16>(angle - 0x4000u);
        const ptr32<_sObject> pObject = guest_call<ptr32<_sObject>>(sym::ObjectCreate, pDef, time, i);
        e.pObject = pObject;
        angle += separation;
        pObject->pDef = pDef;
    }
}

// Each frame: the scenery's animations by the camera's distance from the car-selection view (stopped close up, the
// turntable hidden far away), the turntable's turn towards its target, and each car's place, spin and hop.
uint32 CarouselRender(tTime time)
{
    GuestFrame frame(0xd0);
    sCarouselClass &c = carousel();
    const uint32_t tso = c.pTSO.address;
    const int32_t dt = time - c.lastUpdated;
    c.lastUpdated = time;
    const uint32_t pCamera = at<uint32_t>(sym::cameraVarG + 0xb70u);
    int32_t dz = at<int32_t>(pCamera + 0x14u) - G(camCarSel3zG);
    if (dz < 0)
        dz = -dz;
    int32_t dx = at<int32_t>(pCamera + 0xcu) - G(camCarSel3xG);
    if (dx < 0)
        dx = -dx;
    const int32_t distance = dz < dx ? dx : dz;
    int32_t mode;
    if (distance < 1000)
    {
        tsoFlags(tso) = static_cast<uint16_t>(tsoFlags(tso) | 1u);
        for (const int32 group : {1, 4, 3, 2})
            CarouselDisableAnimGroup(group);
        mode = c.moveMode;
    }
    else if (distance < 80000)
    {
        for (const int32 group : {1, 4, 3, 2})
            CarouselEnableAnimGroup(group);
        mode = c.moveMode;
        tsoFlags(tso) = static_cast<uint16_t>(tsoFlags(tso) | 1u);
        if (mode == 3) // back in view: the turntable turns in from six places away
        {
            mode = 1;
            c.moveMode = 1;
            c.angle = static_cast<int32>(0x10000u - (c.segment + 6u) * static_cast<uint32_t>(c.separation));
        }
        if (mode == 0)
        {
            c.moveMode = 2;
            CarouselMove(c.numSegments ? (c.segment + 6u) % c.numSegments : 0u);
            mode = c.moveMode;
        }
    }
    else
    {
        c.moveMode = 3;
        tsoFlags(tso) = static_cast<uint16_t>(tsoFlags(tso) & 0xfffeu);
        mode = 3;
    }
    uint32_t angle = static_cast<uint32_t>(c.angle);
    uint32_t target = static_cast<uint32_t>(c.tgtAngle);
    const uint32_t separation = static_cast<uint32_t>(c.separation);
    if (mode == 0)
    {
        target &= 0xffffu;
        angle &= 0xffffu;
        c.tgtAngle = static_cast<int32>(target);
    }
    if (mode == 1 && angle == target)
    {
        c.moveMode = 0;
        mode = 0;
    }
    int32_t diff = static_cast<int32_t>(angle - target);
    if (mode == 0)
        diff = static_cast<int16_t>(static_cast<int16_t>(angle) - static_cast<int16_t>(target));
    const int32_t step = static_cast<int32_t>(static_cast<uint32_t>(dt) << 4);
    if ((diff < 0 ? -diff : diff) < step)
        angle = target;
    else if (diff > 0)
        angle -= static_cast<uint32_t>(step);
    else if (diff < 0)
        angle += static_cast<uint32_t>(step);
    c.angle = static_cast<int32>(angle);
    guest_call<void>(sym::VfxSetupCameraMatrix);
    for (uint32_t i = 0; i < c.numSegments; ++i)
    {
        sCarousel &e = c.carousel[i];
        const ptr32<_sObject> pObject = e.pObject;
        const uint32_t phys = pObject->pPhysics.address;
        const ptr32<_sRendObj> pRend = pObject->pRendObj;
        // Visible from the front of the turntable only; while it turns, only the places near the target.
        e.flag2 = (angle - 0x3e9u > 0xabdeu && static_cast<uint16_t>(static_cast<int16_t>(angle) + 0x4e20) < 0x5209u) ? 1 : 0;
        if (c.moveMode == 1)
        {
            e.angle = 0;
            if (e.flag2 != 0)
            {
                const uint32_t segment = c.segment;
                if (segment < 7u)
                {
                    if (segment < i && i < segment + 10u)
                        e.flag2 = 0;
                }
                else if (segment < i || i < segment - 6u)
                    e.flag2 = 0;
            }
        }
        const ee_float a = ee_itof(static_cast<int32_t>(angle)) * amenu::constant(sym::lit4_32760c);
        at<uint8_t>(phys + 0x1cdu) = static_cast<uint8_t>(at<uint8_t>(phys + 0x1cdu) & 0xfdu);
        pRend->pos.x = ee_ftoi(ee_sinf(a) * ee_float(4096.0f));
        const int32_t cosine = ee_ftoi(ee_cosf(a) * ee_float(4096.0f));
        const int32_t radius = -G(carouselRadiusG);
        const int32_t x = mul32(pRend->pos.x, radius) >> 4;
        pRend->pos.x = x;
        const int32_t z = mul32(cosine, radius) >> 4;
        pRend->pos.z = z;
        pRend->pos.x = x + at<int32_t>(tso + kTsoX);
        pRend->pos.z = z + at<int32_t>(tso + kTsoZ);
        if (i == c.segment) // the highlighted car spins, and hops now and then when at rest
        {
            ee_float &velY = at<ee_float>(phys + 0x24u);
            if (ee_float(std::fabs(float(velY))) < ee_float(1.0f) && guest_call<int32_t>(sym::RandomGetRange, 0, 10) == 0)
                velY = velY + ee_float(800.0f);
            e.angle = static_cast<::rc::angle>(static_cast<uint16_t>(static_cast<uint16_t>(e.angle) + G(carouselRotateRateG) * dt));
        }
        pRend->angles.pitch = 0;
        pRend->angles.roll = 0;
        pRend->angles.head = static_cast<int16>(angle + static_cast<uint16_t>(e.angle) - 0x4000u);
        angle += separation;
        const uint32_t rend = pRend.address;
        guest_call<void>(sym::Mat3nBuildXYZ, rend + 0x18u, rend + 0x10u);
        // The physics' copy of the matrix: the integer one as it is, the float one transposed. The loads and stores
        // keep the original's order (the integer matrix pointer read again after the copy, the position read before
        // the transposed stores, the last of those after the position stores): it shows when the render object, the
        // physics and its matrix overlap.
        std::memmove(guest_host(at<uint32_t>(phys + 0x1e8u) + 0x28u), guest_host(rend + 0x18u), 0x14);
        static const uint32_t kTransposed[9] = {0x18, 0x1e, 0x24, 0x1a, 0x20, 0x26, 0x1c, 0x22, 0x28};
        const int16_t first = at<int16_t>(rend + kTransposed[0]);
        const ee_float posZ = ee_itof(at<int32_t>(rend + 0xcu));
        const int32_t posX = at<int32_t>(rend + 4u);
        const uint32_t integer = at<uint32_t>(phys + 0x1e8u);
        const ee_float posXf = ee_itof(posX);
        const ee_float renderY = at<ee_float>(phys + 0xcu);
        at<ee_float>(phys + 0x4cu) = ee_itof(first);
        for (int32_t k = 1; k < 8; ++k)
            at<ee_float>(phys + 0x4cu + 4u * k) = ee_itof(at<int16_t>(rend + kTransposed[k]));
        const int16_t last = at<int16_t>(rend + kTransposed[8]);
        at<int32_t>(integer + 4u) = posX;
        at<ee_float>(phys + 0x8u) = posXf;
        const int32_t posZi = at<int32_t>(rend + 0xcu);
        at<ee_float>(phys + 0x10u) = posZ;
        at<int32_t>(integer + 0xcu) = posZi;
        at<ee_float>(phys + 0x6cu) = ee_itof(last);
        const ee_float x2 = ee_itof(at<int32_t>(rend + 4u));
        const ee_float z2 = ee_itof(at<int32_t>(rend + 0xcu));
        at<ee_float>(phys + 0x14u) = x2;
        at<ee_float>(phys + 0x1cu) = z2;
        at<int32_t>(rend + 8u) = ee_ftoi(renderY); // pos.y
    }
    return 0;
}

// Draws the turntable's cars (when the turntable is in view): an available car with its wheels and lights (the
// highlighted one lit), a locked one as a bobbing question mark; the cars out of view are hidden.
uint32 CarouselDraw(tTime time)
{
    sCarouselClass &c = carousel();
    c.lastUpdated = time;
    if ((tsoFlags(c.pTSO.address) & 1u) == 0)
    {
        for (uint32_t i = 0; i < c.numSegments; ++i)
        {
            const ptr32<_sObject> pObject = c.carousel[i].pObject;
            pObject->zDrawnDist = 0x7fff;
            pObject->pRendObj->flags = static_cast<uint16>(pObject->pRendObj->flags & 0xfffeu);
            pObject->sDontRenderMe = 1;
        }
        return 0;
    }
    const amenu::Locals f(0x110);
    guest_call<void>(sym::VfxSetupCameraMatrix);
    guest_call<void>(sym::RdrZedsWriteOff, 0);
    const ptr32<_sRendObj> pTmp = f.ptr<_sRendObj>(0);
    for (uint32_t i = 0; i < c.numSegments; ++i)
    {
        sCarousel &e = c.carousel[i];
        const ptr32<_sObject> pObject = e.pObject;
        const uint32_t phys = pObject->pPhysics.address;
        const ptr32<_sWheels> pWheels = pObject->pWheels;
        ptr32<_sRendObj> pRend = pObject->pRendObj;
        // The ground under the car, a little above its position.
        const auto downCast = [&]()
        {
            const ee_float scale = float_from_bits(0x3b800000u); // 1/256
            f.at<ee_float>(0x40) = at<ee_float>(phys + 0x14u) * scale;
            f.at<ee_float>(0x44) = at<ee_float>(phys + 0x18u) * scale + ee_float(150.0f);
            f.at<ee_float>(0x48) = at<ee_float>(phys + 0x1cu) * scale;
            pObject->downCastWorked = guest_call<int32_t>(sym::AColDownCast, f.ptr<ee_float>(0x40), guest_addr(&pObject->poiUnderCentre),
                                                          guest_addr(&pObject->normUnderCentre), f.ptr<uint8_t>(0x60));
        };
        if (e.flag2 != 1)
        {
            pObject->zDrawnDist = 0x7fff;
            pRend->flags = static_cast<uint16>(pRend->flags & 0xfffeu);
            pObject->sDontRenderMe = 1;
        }
        else if (e.flag == 1)
        {
            at<uint32_t>(pRend->pRendObjHead.address + 0x64u) = 0; // no tint
            downCast();
            const uint32_t head = pRend->pRendObjHead.address;
            at<uint16_t>(head + 0x58u) = static_cast<uint16_t>(i == c.segment ? at<uint16_t>(head + 0x58u) & 0xbfffu : at<uint16_t>(head + 0x58u) | 0x4000u);
            at<uint32_t>(head + 0x60u) = at<uint32_t>(G(pLvlG).address + 0x60u); // the level's environment map
            const int16 depth = guest_call<int16>(sym::RendObjRender, pRend, 0, 0, 1);
            pObject->lightsOn = 3;
            pObject->zDrawnDist = static_cast<uint16>(depth);
            pObject->zShortestDrawnDist = 0x7fff;
            pObject->pushRenderDist = 0;
            pObject->sDontRenderMe = 0;
            if (i == c.segment)
                guest_call<void>(sym::ObjectRenderLights, pObject);
        }
        else
        {
            // A locked car: the question mark in its place.
            std::memmove(pTmp.get(), pRend.get(), 0x38);
            const uint32_t mark = feLevel()->pQuestionMarkROB.address;
            at<uint32_t>(mark + 0x64u) = 0x80000000u;
            f.sw(0x0, mark);
            downCast();
            const ee_float t = ee_itof(static_cast<int32_t>((G(mainVarG).time << 1) & 0xfffu));
            const int32_t bob = ee_ftoi(ee_sinf((t + t) * amenu::constant(sym::lit4_327610)) * ee_float(4096.0f));
            f.sw(0x8, static_cast<uint32_t>((bob >> 2) + 0x1389e));
            const int16 depth = guest_call<int16>(sym::RendObjRender, pTmp, 0, 0, 1);
            pObject->zDrawnDist = static_cast<uint16>(depth);
            pObject->zShortestDrawnDist = 0x7fff;
            pObject->sDontRenderMe = 1;
            pObject->pushRenderDist = 0;
        }
        if (e.flag != 1)
            continue;
        const int32_t type = pObject->pDef->type;
        if (type == 15 || static_cast<uint32_t>(type) >= 0x14u || !pWheels || e.flag2 != e.flag)
            continue;
        // The wheels: front left and right, rear left and right, and for the moon rover the two middle ones between.
        const int32_t wheels = type == 0x12 ? 6 : 4;
        const uint32_t rend = pRend.address;
        for (int32_t w = 0; w < wheels; ++w)
        {
            const ptr32<_sNode> nodes[4] = {pWheels->pFrontL, pWheels->pFrontR, pWheels->pRearL, pWheels->pRearR};
            const auto worldPos = [&](ptr32<_sNode> pNode, uint32_t out)
            { guest_call<void>(sym::NodeCalcWorldPos, pNode, rend + 0x18u, rend + 0x4u, f.sp + out); };
            if (w < 4)
                worldPos(nodes[w], 0x4);
            else
            {
                worldPos(nodes[w - 4], 0x4);
                worldPos(nodes[w - 2], 0x50);
                f.sw(0x4, static_cast<uint32_t>((f.at<int32_t>(0x4) + f.at<int32_t>(0x50)) / 2));
                f.sw(0xc, static_cast<uint32_t>((f.at<int32_t>(0xc) + f.at<int32_t>(0x58)) / 2));
            }
            std::memmove(&f.at<uint8_t>(0x18), guest_host(rend + 0x18u), 0x20);
            const uint32_t wheelHead = at<uint32_t>(at<uint32_t>(pWheels->pWheelRes.address + 4u * w) + 4u);
            f.sw(0x0, wheelHead);
            f.sw(0x8, static_cast<uint32_t>(ee_ftoi(at<ee_float>(phys + 0xb0u + static_cast<uint32_t>(w % 4) * 0x48u))));
            at<uint32_t>(wheelHead + 0x64u) = at<uint32_t>(pRend->pRendObjHead.address + 0x64u); // the car's tint
            if ((w & 1) != 0) // a right wheel: mirrored
                for (const uint32_t k : {0x18u, 0x1eu, 0x24u})
                    f.sh(k, static_cast<uint32_t>(-static_cast<int32_t>(f.lhu(k))));
            guest_call<int16>(sym::RendObjRender, pTmp, 0, 0, 1);
        }
    }
    return 0;
}

// Turns the turntable to the first place holding the car type (if any).
void CarouselMoveTo(eObjectType type)
{
    GuestFrame frame(0x10);
    sCarouselClass &c = carousel();
    for (uint32_t i = 0; i < c.numSegments; ++i)
        if (c.carousel[i].type == type)
        {
            CarouselMove(i);
            return;
        }
}

void CarouselMove(uint32 segment)
{
    sCarouselClass &c = carousel();
    c.segment = segment;
    c.tgtAngle = static_cast<int32>(0x10000u - segment * static_cast<uint32_t>(c.separation));
}

// The highlighted car turns into its boat or back (an effect does it).
void CarouselMode(bool_ bBoat)
{
    GuestFrame frame(0x30);
    sCarouselClass &c = carousel();
    sCarousel &e = c.carousel[c.segment];
    if (bBoat != static_cast<int32_t>(e.bBoat))
    {
        guest_call<int32_t>(sym::VfxAddNew, e.pObject, bBoat, 0, 0x1b);
        e.bBoat = static_cast<uint32>(bBoat);
    }
}

void CarouselDisableAnimGroup(int32 group)
{
    setAnimGroup(group, true);
}

void CarouselEnableAnimGroup(int32 group)
{
    setAnimGroup(group, false);
}

} // namespace rc

RC_DECOMP_FN_SETUP_HEAP(CarouselInit, rc::fuzz::frontCarouselInit)
RC_DECOMP_FN_SETUP_HEAP(CarouselTerm, rc::fuzz::frontStubbed)
RC_DECOMP_FN_SETUP_HEAP(CarouselSetUp, rc::fuzz::frontCarouselSetUp)
RC_DECOMP_FN_SETUP_HEAP(CarouselRender, rc::fuzz::frontCarousel)
RC_DECOMP_FN_SETUP_HEAP(CarouselDraw, rc::fuzz::frontCarousel)
RC_DECOMP_FN_SETUP(CarouselMoveTo, rc::fuzz::frontCarType)
RC_DECOMP_FN_SETUP(CarouselMove, rc::fuzz::frontCarousel)
RC_DECOMP_FN_SETUP(CarouselMode, rc::fuzz::frontCarousel)
RC_DECOMP_FN_SETUP(CarouselDisableAnimGroup, rc::fuzz::frontAnimGroup)
RC_DECOMP_FN_SETUP(CarouselEnableAnimGroup, rc::fuzz::frontAnimGroup)

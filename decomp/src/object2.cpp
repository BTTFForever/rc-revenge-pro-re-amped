// Decompiled from OBJECT.c: the per-frame sound updates, the lights and the drawing of the cars. See object.cpp.

#include "object.h"
#include "object_fuzz.h"

#include <algorithm>

namespace rc {

using namespace object;



// ---------------------------------------------------------------------------------------------------------------------
// Engine sound.

namespace {

const ee_float k128 = 128.0f, k30 = 30.0f, k70 = 70.0f, k40 = 40.0f;
inline ee_float constAt(uint32_t address) { return at<ee_float>(address); }
constexpr uint32_t kAiVolumeScale = 0x32768c, kVolumeScale = 0x327690, kBoatVolumeScale = 0x327694, kMaterialVolumeScale = 0x327698;
constexpr uint32_t kRocketVolumeScale = 0x327684;

// min.s: a plain comparison of the operands in the instruction's order.
ee_float ee_min(ee_float a, ee_float b) { return std::min(a.v, b.v); }

bool isUfoOrRocket(uint32_t type) { return type == 0xf || type == 0x22 || type == 0x13 || type == 0x26; }

// pSphere[3].liquidDepth < 0: the fourth sphere is under water.
bool fourthSphereUnder(ptr32<_sFPhysLibCar> physics) { return at<ee_float>(physics->pSphere.address + 0x528u) < ee_float(0.0f); }

} // namespace

// The engine's pitch and volume (and the material sound's sample) from the car's speed; in the air (mode 1) the
// pitch follows the throttle instead.
void ObjectEngineSFXParms(ptr32<_sObject> pObject, int32 flags)
{
    const uint32_t incomingS2 = callerS2();
    GuestFrame frame(0x40);
    uint32_t mode = 0;
    ptr32<_sAllObjSounds> sounds = pObject->pSounds;
    const ptr32<_sFPhysLibCar> physics = pObject->pPhysics;
    int32_t pitch;
    if (sounds->engineAccel == 0)
        pitch = sounds->enginePitchModify;
    else
        pitch = ee_ftoi(ee_itof(sounds->enginePitchModify) + (physics->speed * k128) / ee_itof(sounds->engineAccel));
    int32_t volume;
    if (pObject->playerType == 2)
    {
        volume = ee_ftoi(physics->speed * constAt(kAiVolumeScale) + k30);
        if (volume >= 0x3d)
            volume = 0x3c;
    }
    else
    {
        volume = ee_ftoi(physics->speed * constAt(kVolumeScale) + k70);
        if (volume >= 0x6f)
            volume = 0x6e;
    }
    const revsfxClass &rev = G(revSfxVarG);
    if (at<int32_t>(0x1ef3b0u + 0x1160u) != 0)
        volume = ee_div(volume * at<int32_t>(0x1ef3b0u + 0x1164u), 10000);
    (void)rev;
    const ptr32<_sObjectDef> def = pObject->pDef;
    if (at<int32_t>(0x1ece10u) == 0) // hudVarG.iReadySteadyGo
    {
        if (static_cast<uint32_t>(def->type) >= 0x14u)
        {
            if (fourthSphereUnder(physics))
                mode = 1;
        }
        else if ((physics->wheel[0].flags & 1u) == 0)
            mode = (physics->wheel[3].flags & 1u) ^ 1u;
    }
    if (isUfoOrRocket(static_cast<uint32_t>(def->type)))
        mode = 2;
    _sAllObjSounds &s = *sounds;
    if (mode == 1)
    {
        const int16_t throttle = physics->inputEngineThrottle;
        const int32_t held = throttle < 0 ? -static_cast<int32_t>(throttle) : throttle;
        if (s.engineMaxInAir - 0x50 < s.engine[0].pitch)
            s.enginePitchRate = static_cast<int16>(s.enginePitchRate + 1);
        else
            s.enginePitchRate = 0;
        if (held == 0 || held < s.engineOldPitch)
            s.engine[0].pitch = static_cast<int16>(s.engine[0].pitch - 5);
        else if (s.enginePitchRate == 0)
            s.engine[0].pitch = static_cast<int16>(s.engine[0].pitch + 0xc);
        else if (s.enginePitchRate % 3 == 1)
            s.engine[0].pitch = static_cast<int16>(s.engine[0].pitch + 1);
        const int16_t now = s.engine[0].pitch;
        if (s.engineMaxInAir < now)
            s.engine[0].pitch = s.engineMaxInAir;
        else if (now < s.enginePitchModify)
            s.engine[0].pitch = s.enginePitchModify;
        if (flags != 0)
            s.engine[1].pitch = s.engine[0].pitch;
        s.engineOldPitch = static_cast<int16>(held);
    }
    else
        s.engine[0].pitch = static_cast<int16>(pitch);
    s.engine[0].volume = static_cast<int16>(volume);
    if (flags != 0)
    {
        s.engine[1].volume = static_cast<int16>(volume);
        s.engine[1].pitch = s.engine[0].pitch;
    }
    if (pObject->playerType >= 2 || isUfoOrRocket(static_cast<uint32_t>(def->type)))
        return;
    if (static_cast<uint32_t>(def->type) >= 0x14u)
    {
        s.material.pitch = 0x100;
        const int32_t v = ee_ftoi(physics->speed * constAt(kBoatVolumeScale));
        s.material.volume = static_cast<int16>(v);
        if (static_cast<int16_t>(v) >= 0x5b)
            s.material.volume = 0x5a;
        s.material.sample = fourthSphereUnder(physics) ? 0xff : 0x39;
        s.material.oldMaterial = static_cast<int16>(incomingS2); // never set: the caller's $s2
        return;
    }
    int32_t material = physics->wheel[0].drivingOnMtl;
    if (mode != 0)
        material = 0x1d;
    const int32_t v = ee_ftoi(physics->speed * constAt(kMaterialVolumeScale));
    s.material.volume = static_cast<int16>(v);
    if (static_cast<int16_t>(v) >= 0x29)
        s.material.volume = 0x28;
    if (material != s.material.oldMaterial)
    {
        if (s.material.handle != -1)
        {
            sfxStop(s.material.handle);
            sounds = pObject->pSounds;
            sounds->material.handle = -1;
        }
        _sMaterialSound &m = sounds->material;
        m.pitch = 0x100;
        switch (material)
        {
        case 2: m.sample = 0x22; break;
        case 3: m.pitch = 0xdc; m.sample = 0x24; break;
        case 4: m.volume = 0x32; m.sample = 0x23; break;
        case 7: m.sample = 0x25; break;
        case 10: m.pitch = 0xc4; m.sample = 0x39; break;
        case 11: m.sample = 0x24; break;
        case 16: case 17: m.pitch = 0x180; m.sample = 0x22; break;
        default: m.sample = 0xff; break;
        }
    }
    sounds->material.oldMaterial = static_cast<int16>(material);
}

void ObjectUpdateEngine(ptr32<_sObject> pObject, int32 flags)
{
    GuestFrame frame(0x60);
    ObjectEngineSFXParms(pObject, flags);
    if (pObject->playerType < 2)
    {
        for (int32_t i = 0; i <= flags; ++i)
        {
            const uint32_t sound = pObject->pSounds.address + static_cast<uint32_t>(i) * 8u;
            const _sObjSound &e = at<_sObjSound>(sound);
            int32_t number, type;
            if (e.handle != -1)
                number = static_cast<int16_t>(e.handle), type = i + 0x18;
            else
                number = at<int16_t>(pObject->pDef.address + 0x34u), type = i + 4;
            const int32_t handle = sfx3D(number, e.volume, e.pitch, rendPos(pObject), type, 5000);
            at<int32_t>(pObject->pSounds.address + static_cast<uint32_t>(i) * 8u) = handle;
            const ptr32<_sObjectDef> def = pObject->pDef;
            const uint32_t t = static_cast<uint32_t>(def->type);
            if (t != 0x13 && t != 0x26)
                continue;
            const uint32_t sounds = pObject->pSounds.address;
            _sObjSound &r = at<_sObjSound>(sounds + 0x30u + static_cast<uint32_t>(i) * 8u);
            r.pitch = 0x100;
            const int32_t v = ee_ftoi(pObject->pPhysics->speed * constAt(kRocketVolumeScale) + k40);
            r.volume = static_cast<int16>(v);
            if (pObject->playerType == 2)
            {
                if (static_cast<int16_t>(v) >= 0x51)
                    r.volume = 0x50;
            }
            else if (static_cast<int16_t>(v) >= 0x92)
                r.volume = 0x91;
            if (r.handle != -1)
                number = static_cast<int16_t>(r.handle), type = i + 0x58;
            else
                number = at<int16_t>(def.address + 0x3cu), type = i + 0x44;
            const int32_t rocket = sfx3D(number, r.volume, r.pitch, rendPos(pObject), type, 5000);
            at<int32_t>(pObject->pSounds.address + 0x30u + static_cast<uint32_t>(i) * 8u) = rocket;
        }
        const _sMaterialSound &m = pObject->pSounds->material;
        if (m.handle == -1)
        {
            if (m.sample == 0xff)
                return;
            pObject->pSounds->material.handle = sfx3D(m.sample, m.volume, m.pitch, rendPos(pObject), pObject->playerType + 0x44, 5000);
        }
        else if (m.sample == 0xff)
        {
            sfxStop(m.handle);
            pObject->pSounds->material.handle = -1;
        }
        else
            pObject->pSounds->material.handle =
                sfx3D(static_cast<int16_t>(m.handle), m.volume, m.pitch, rendPos(pObject), pObject->playerType + 0x58, 5000);
        return;
    }
    const _sObjSound &e = pObject->pSounds->engine[0];
    int32_t number, type;
    if (e.handle != -1)
        number = static_cast<int16_t>(e.handle), type = 0x58;
    else
        number = at<int16_t>(pObject->pDef.address + 0x34u), type = 0x44;
    int32_t handle;
    if (flags == 0)
        handle = sfx3D(number, e.volume, e.pitch, rendPos(pObject), type, 5000);
    else
        handle = static_cast<int16_t>(guest_call<int32_t>(kRevSfxPlay3DCombine, number, static_cast<int32_t>(e.volume),
                                                          static_cast<int32_t>(e.pitch), ptr32<uint8>(rendPos(pObject)), type, 5000));
    pObject->pSounds->engine[0].handle = handle;
}


// ---------------------------------------------------------------------------------------------------------------------
// Skids: the tyre squeal when cornering hard with both sides down, the oily skid while skidOil runs.

namespace {

// The oil is gone: the skid marks lose their oily bit and the wheels their oily counters.
void endOil(ptr32<_sObject> pObject)
{
    at<uint8_t>(pObject->pVfx_SkidLeft.address + 0x18bu) &= 0xfeu;
    at<uint8_t>(pObject->pVfx_SkidRight.address + 0x18bu) &= 0xfeu;
    _sFPhysLibCar &p = *pObject->pPhysics;
    p.wheel[3].oilyCounter = 0;
    p.wheel[0].oilyCounter = 0;
    p.wheel[2].oilyCounter = 0;
    p.wheel[1].oilyCounter = 0;
}

} // namespace

void ObjectUpdateSkid(ptr32<_sObject> pObject, int32, tTime time)
{
    if (at<int32_t>(0x1ece10u) != 0) // hudVarG.iReadySteadyGo
        return;
    const uint32_t type = static_cast<uint32_t>(pObject->pDef->type);
    if (isUfoOrRocket(type))
        return;
    GuestFrame frame(0x40);
    if (pObject->playerType == 2)
    {
        if (pObject->skidOil == 0 || !(pObject->skidOil < time))
            return;
        pObject->skidOil = 0;
        endOil(pObject);
        return;
    }
    if (type >= 0x14u)
    {
        const int32_t handle = pObject->pSounds->skid[0].handle;
        if (handle == -1)
            return;
        sfxStop(handle);
        pObject->pSounds->skid[0].handle = -1;
        return;
    }
    const ptr32<_sFPhysLibCar> physics = pObject->pPhysics;
    const uint16_t front = physics->wheel[0].flags, back = physics->wheel[3].flags;
    if ((front & 2u) && (front & 1u) && (back & 1u) && (back & 2u) &&
        static_cast<uint16_t>(physics->inputSteerAngle + 100) >= 0xc9u)
    {
        const int32_t handle = pObject->pSounds->skid[0].handle;
        int32_t number, kind;
        if (handle != -1)
            number = static_cast<int16_t>(handle), kind = pObject->playerType + 0x18;
        else
            number = 0x27, kind = pObject->playerType + 4;
        const int32_t played = sfx3D(number, 0x30, 0x100, rendPos(pObject), kind, 5000);
        pObject->pSounds->skid[0].handle = played;
    }
    else if (pObject->pSounds->skid[0].handle != -1)
    {
        sfxStop(pObject->pSounds->skid[0].handle);
        pObject->pSounds->skid[0].handle = -1;
    }
    if (!(time < pObject->skidOil))
    {
        if (pObject->pSounds->skid[1].handle != -1)
        {
            sfxStop(pObject->pSounds->skid[1].handle);
            pObject->pSounds->skid[1].handle = -1;
        }
        if (pObject->skidOil == 0)
            return;
        pObject->skidOil = 0;
        endOil(pObject);
        return;
    }
    bool down = false;
    for (int w = 0; w < 4 && !down; ++w)
        down = (physics->wheel[w].flags & 1u) != 0;
    if (!down)
    {
        if (pObject->pSounds->skid[1].handle != -1)
        {
            sfxStop(pObject->pSounds->skid[1].handle);
            pObject->pSounds->skid[1].handle = -1;
        }
        pObject->skidOil = 0;
        at<uint8_t>(pObject->pVfx_SkidLeft.address + 0x18bu) &= 0xfeu;
        at<uint8_t>(pObject->pVfx_SkidRight.address + 0x18bu) &= 0xfeu;
        _sFPhysLibCar &p = *pObject->pPhysics;
        p.wheel[3].oilyCounter = 0;
        p.wheel[2].oilyCounter = 0;
        p.wheel[1].oilyCounter = 0;
        p.wheel[0].oilyCounter = 0;
    }
    const int32_t handle = pObject->pSounds->skid[1].handle;
    int32_t number, kind;
    if (handle != -1)
        number = static_cast<int16_t>(handle), kind = pObject->playerType + 0x18;
    else
    {
        if (static_cast<uint16_t>(pObject->pPhysics->inputSteerAngle + 0x40) < 0x81u)
            return;
        number = 0x27, kind = pObject->playerType + 4;
    }
    const int32_t played = sfx3D(number, 0x80, 0x100, rendPos(pObject), kind, 5000);
    pObject->pSounds->skid[1].handle = played;
}


// ---------------------------------------------------------------------------------------------------------------------
// Scrapes: the outer spheres (index 4 and up, last first) that touch a level poly (type 1) or a car (type 2) play the
// scrape and the material's sound and throw sparks, one contact per sphere.

void ObjectUpdateScrape(ptr32<_sObject> pObject, int32)
{
    GuestFrame frame(0xc0);
    const ptr32<vec32> vec = frame.local<vec32>(0);
    const ptr32<int32_t> collided = frame.local<int32_t>(0x10);
    const ee_float k2000 = 2000.0f, k255 = 255.0f;
    const uint32_t kSphere = 0x190u;
    int32_t second = -1, first = 0; // second keeps its value from sphere to sphere, as the original's register does
    *collided = 0;
    const ptr32<_sCarCol> carCol = pObject->pCarCol;
    int32_t index = static_cast<int32_t>(carCol->numSpheres) - 1;
    uint32_t sphere = carCol->pSphere.address + static_cast<uint32_t>(index) * kSphere;
    for (; index >= 4; --index, sphere -= kSphere)
    {
        for (int32_t c = at<int8_t>(sphere + 0x67u) - 1; c >= 0; --c)
        {
            const int8_t type = at<int8_t>(sphere + 0x94u + static_cast<uint32_t>(c));
            if (type == 1)
            {
                const uint32_t poly = at<uint32_t>(sphere + 0x160u + static_cast<uint32_t>(c) * 4u);
                if (at<uint8_t>(poly + 0x53u) == 0x20)
                    continue;
                if (k2000 < at<ee_float>(sphere + 0xa4u + static_cast<uint32_t>(c) * 16u) && k2000 < pObject->pPhysics->mat.Yy)
                    continue;
                first = ObjectMaterialSfx(1, at<uint8_t>(poly + 0x53u));
                second = ObjectMaterialSfx(0, at<uint8_t>(poly + 0x53u));
            }
            else if (type == 2)
            {
                first = RandomGetNDRange(0, 200) < 100 ? 0x54 : 0x55;
                if (pObject->playerType == 2)
                    pObject->bodgeRearLightOn = 0x14;
            }
            else
                continue;
            if (pObject->playerType != 2)
            {
                const ee_float depth = at<ee_float>(sphere + 0xacu + static_cast<uint32_t>(c) * 16u);
                const ee_float loud = ee_min(depth * at<ee_float>(0x327688u) + ee_float(128.0f), k255);
                const int32_t volume = static_cast<int16_t>(ee_ftoi(loud));
                _sObjSound &scrape = pObject->pSounds->scrape[0];
                int32_t number, kind;
                if (scrape.handle != -1)
                    number = static_cast<int16_t>(scrape.handle), kind = pObject->playerType + 0x18;
                else
                    number = static_cast<int16_t>(first), kind = pObject->playerType + 4;
                const int32_t played = sfx3D(number, volume, 0xff, rendPos(pObject), kind, 5000);
                pObject->pSounds->scrape[0].handle = played;
                if (second != -1)
                {
                    const _sObjSound &material = pObject->pSounds->scrape[1];
                    if (material.handle != -1)
                        number = static_cast<int16_t>(material.handle), kind = pObject->playerType + 0x18;
                    else
                        number = static_cast<int16_t>(second), kind = pObject->playerType + 4;
                    const int32_t also = sfx3D(number, volume, 0xff, rendPos(pObject), kind, 5000);
                    pObject->pSounds->scrape[1].handle = also;
                }
                *collided = 1;
            }
            vec->x = ee_ftoi(at<ee_float>(sphere));
            vec->y = ee_ftoi(at<ee_float>(sphere + 4u));
            vec->z = ee_ftoi(at<ee_float>(sphere + 8u));
            VfxCreate_Sparks(pObject->pVfx_SparkObj, vec);
            break;
        }
    }
    if (*collided != 0 || pObject->pSounds->scrape[0].handle == -1)
        return;
    sfxStop(pObject->pSounds->scrape[0].handle);
    const int32_t other = pObject->pSounds->scrape[1].handle;
    pObject->pSounds->scrape[0].handle = -1;
    if (other != -1)
    {
        sfxStop(other);
        pObject->pSounds->scrape[1].handle = -1;
    }
}


// ---------------------------------------------------------------------------------------------------------------------
// The front end's carousel: each car's engine idles, the chosen one louder; the dump tug and Yella also play their
// second engine at a fixed place, the rocket car its rocket.

void ObjectSfxFrontEnd(int32 which)
{
    GuestFrame frame(0x70);
    const ptr32<vec32> pos = frame.local<vec32>(0);
    constexpr uint32_t kCarousel = 0x1ebaf0u;
    const HeldS2 s2(kCarousel); // ObjectEngineSFXParms keeps it as a boat's material
    for (uint32_t i = 0; i < at<uint32_t>(kCarousel + 0x1cu); ++i)
    {
        const ptr32<_sObject> o(at<uint32_t>(kCarousel + 0x2cu + i * 0x34u));
        ObjectEngineSFXParms(o, 0);
        _sAllObjSounds &snd = *o->pSounds;
        snd.engine[0].volume = static_cast<int16>(snd.engine[0].volume + (static_cast<int32_t>(i) == which ? 100 : 0x1e));
        const uint32_t type = static_cast<uint32_t>(o->pDef->type);
        if (type == 0x11 || type == 0xe)
        {
            if (type == 0x11)
                *pos = vec32{static_cast<int32_t>(0xffeb06e3u), 0x174a4, static_cast<int32_t>(0xfff39c21u)};
            else
                *pos = vec32{static_cast<int32_t>(0xfff17ebeu), 0xa39a, static_cast<int32_t>(0xffed5b40u)};
            snd.engine[1].volume = 0x80;
            snd.engine[1].pitch = 0x100;
            int32_t number, kind;
            if (snd.engine[1].handle != -1)
                number = static_cast<int16_t>(snd.engine[1].handle), kind = 0x58;
            else
                number = at<int16_t>(o->pDef.address + 0x34u), kind = 0x44;
            const int32_t played = sfx3D(number, 0x80, 0x100, pos.address, kind, 4000);
            o->pSounds->engine[1].handle = played;
        }
        {
            const _sObjSound &e = o->pSounds->engine[0];
            int32_t number, kind;
            if (e.handle != -1)
                number = static_cast<int16_t>(e.handle), kind = 0x58;
            else
                number = at<int16_t>(o->pDef.address + 0x34u), kind = 0x44;
            const int32_t played = sfx3D(number, e.volume, e.pitch, rendPos(o), kind, 1000);
            o->pSounds->engine[0].handle = played;
        }
        const uint32_t now = static_cast<uint32_t>(o->pDef->type);
        if (now != 0x13 && now != 0x26)
            continue;
        _sObjSound &r = o->pSounds->rocket[0];
        r.pitch = 0x100;
        const int32_t v = ee_ftoi(o->pPhysics->speed * constAt(0x3276ccu) + k40);
        r.volume = static_cast<int16>(v);
        int32_t number, kind;
        if (r.handle != -1)
            number = static_cast<int16_t>(r.handle), kind = 0x58;
        else
            number = at<int16_t>(o->pDef.address + 0x3cu), kind = 0x44;
        const int32_t played = sfx3D(number, static_cast<int16_t>(v), 0x100, rendPos(o), kind, 1000);
        o->pSounds->rocket[0].handle = played;
    }
}


// ---------------------------------------------------------------------------------------------------------------------
// Overtaking: a car that moved up a place (close to the car it passed, for a player) plays its driver's taunt.

void ObjectUpdateOvertaking(ptr32<_sObject> pObject, int32, tTime time)
{
    const uint32_t incomingA3 = getRegU32(&g_runtime->eeScheduler().runningContext(), 7);
    if (at<int32_t>(0x1ece10u) != 0) // hudVarG.iReadySteadyGo
        return;
    GuestFrame frame(0x40);
    _sObject &o = *pObject;
    const auto place = [&]() { return o.playerType < 2 ? o.iRacePosAverage : o.iRacePos; };
    if (place() < o.overtakingPrev && o.overtakingTimerStart < time)
    {
        bool close = true;
        if (o.playerType < 2)
        {
            // The car now at our old place (the last one looked at if none is; with no cars, whatever $a3 held).
            uint32_t other = incomingA3;
            for (uint32_t i = 0; i < G(objectVarG).noObjects; i = static_cast<uint32_t>(static_cast<int16_t>(i + 1)))
            {
                other = objectAt(i).address;
                if (at<_sObject>(other).iRacePos == o.overtakingPrev)
                    break;
            }
            const _sObject &them = at<_sObject>(other);
            const ptr32<vec16_> delta = frame.local<vec16_>(0);
            const int32_t dz = ee_ftoi(o.pos16.z - them.pos16.z);
            const int32_t dx = ee_ftoi(o.pos16.x - them.pos16.x);
            const int32_t dy = ee_ftoi(o.pos16.y - them.pos16.y);
            delta->z = static_cast<int16>(dz);
            delta->x = static_cast<int16>(dx);
            delta->y = static_cast<int16>(dy);
            close = Vec16Magnitude(delta) < 2000u;
        }
        if (close)
        {
            o.overtakingMisc = 1;
            o.overtakingTimerStart = time + 0x9c4;
            o.overtakingTimer = time;
        }
    }
    o.overtakingPrev = static_cast<int8>(place());
    if (o.overtakingMisc == 0)
        return;
    const ptr32<_sObjectDef> def = o.pDef;
    const int32_t volume = o.playerType < 2 ? 0xdc : 0xb4;
    const auto play = [&](int32_t pitch)
    { return sfx3D(at<int16_t>(def.address + 0x38u), volume, pitch, rendPos(pObject), o.playerType + 4, 2000); };
    const auto stopNow = [&]()
    {
        guest_call(kSfxStopNow, static_cast<int32_t>(o.overtakingSfx));
        o.overtakingTimer = time + 0xfa;
        o.overtakingSfx = -1;
        o.overtakingMisc = 2;
    };
    const uint32_t type = static_cast<uint32_t>(def->type);
    const uint32_t kind = type - 1u < 0x26u ? (type - 1u) % 19u + 1u : 0u; // boats share their car's case
    if (kind == 0)
        return;
    int32_t startPitch = 0x100, wait = 0;
    switch (kind)
    {
    case 1: startPitch = 0xe2; wait = 0x50; break;  // Bad Dr RC
    case 5: wait = 0x3c; break;                     // Phat Trucker
    case 7: wait = 0x3c; break;                     // Jungle Ranger
    case 8: wait = 0x50; break;                     // Griffin
    case 9: startPitch = 0xe2; wait = 0xfa; break;  // F1
    case 10: wait = 0x50; break;                    // Dark Avenger
    case 12: case 13: wait = 0x14; break;           // Canary XL, Big Mumma
    case 3:                                         // Sarge
        if (o.weaponFiring != 0 && o.weaponsHeldType == 0xc)
        {
            if (o.overtakingSfx == -1)
                return;
            sfxStop(o.overtakingSfx);
            o.overtakingSfx = -1;
            o.overtakingTimer = 0;
            o.overtakingMisc = 0;
            return;
        }
        if (o.overtakingTimer == time && o.overtakingSfx == -1)
        {
            o.overtakingSfx = static_cast<int8>(play(0x100));
            o.overtakingTimer = time + 0x5dc;
            if (o.pVfxSarge)
            {
                VfxDelete(o.pVfxSarge);
                o.pVfxSarge = ptr32<_sVfx_Object>(0u);
            }
            o.pVfxSarge = VfxAddNew(pObject, 0, 0, 0x19);
            return;
        }
        if (!(o.overtakingTimer < time) || o.overtakingSfx == -1)
            return;
        sfxStop(o.overtakingSfx);
        o.overtakingSfx = -1;
        o.overtakingMisc = 0;
        o.overtakingTimer = 0;
        if (o.pVfxSarge)
        {
            VfxDelete(o.pVfxSarge);
            o.pVfxSarge = ptr32<_sVfx_Object>(0u);
        }
        return;
    case 4: case 6:                                 // RC Action, Nitro XLi: a run of short calls
    {
        if (o.overtakingTimer == time && o.overtakingSfx == -1)
        {
            o.overtakingSfx = static_cast<int8>(play(0x100));
            o.overtakingTimer = time + (kind == 4 ? 0x28 : 0x3c);
            return;
        }
        if (!(o.overtakingTimer < time))
            return;
        if (static_cast<uint8_t>(o.overtakingMisc) & 1u)
        {
            guest_call(kSfxStopNow, static_cast<int32_t>(o.overtakingSfx));
            o.overtakingTimer = time + 0x96;
            o.overtakingSfx = -1;
            o.overtakingMisc = static_cast<int8>(o.overtakingMisc + 1);
            return;
        }
        const bool last = kind == 4 ? o.overtakingMisc != 2 : o.overtakingMisc == 6;
        if (!last)
        {
            o.overtakingSfx = static_cast<int8>(play(kind == 4 ? 0xe2 : 0x100));
            o.overtakingTimer = time + 0x3c;
            o.overtakingMisc = static_cast<int8>(o.overtakingMisc + 1);
            return;
        }
        play(kind == 4 ? 0x11e : 0x100);
        o.overtakingTimer = 0;
        o.overtakingSfx = -1;
        o.overtakingMisc = 0;
        return;
    }
    default:                                        // the rest: one call
        if (o.overtakingTimer != time || o.overtakingSfx != -1)
            return;
        play(0x100);
        o.overtakingTimer = 0;
        o.overtakingMisc = 0;
        return;
    }
    if (o.overtakingTimer == time && o.overtakingSfx == -1)
    {
        o.overtakingSfx = static_cast<int8>(play(startPitch));
        o.overtakingTimer = time + wait;
        return;
    }
    if (!(o.overtakingTimer < time))
        return;
    if (o.overtakingMisc == 1)
    {
        stopNow();
        return;
    }
    play(0x100);
    o.overtakingTimer = 0;
    o.overtakingSfx = -1;
    o.overtakingMisc = 0;
}


// ---------------------------------------------------------------------------------------------------------------------
// Lights: up to four rear and four head lights, each a quad from a node (its place, its X axis scaled by radius2 and
// its Z axis by radius, in 1/64 units), with the quad's centre in the fifth entry and a flare size.

void ObjectInitLights(ptr32<_sObject> pObject, ptr32<_sObjectDef> pDef)
{
    const ee_float kAxis = 0.00012207031f, kQuarter = 0.25f, kHeadFlare = 1.5f;
    for (int side = 0; side < 2; ++side)
    {
        const uint32_t tag = side == 0 ? 0x10000636u : 0x10000637u;
        const uint8_t bit = side == 0 ? 1 : 2;
        const uint32_t lights = pObject.address + (side == 0 ? 0x224u : 0x314u);
        const uint32_t flares = pObject.address + (side == 0 ? 0x404u : 0x414u);
        ptr32<_sNode> node = NodeFindByTag(pDef->pNodeHead, tag);
        int32_t n = 0;
        bool full = false;
        while (node)
        {
            pObject->lightsPresent = static_cast<int8>(static_cast<uint8_t>(pObject->lightsPresent) | bit);
            const uint32_t nd = node.address;
            const ee_float pos[3] = {ee_itof(at<int32_t>(nd + 0x20u) << 6), ee_itof(at<int32_t>(nd + 0x24u) << 6),
                                     ee_itof(at<int32_t>(nd + 0x28u) << 6)};
            const ee_float w = ee_itof(static_cast<int32_t>(at<int16_t>(nd + 0xau)) << 6);
            const ee_float h = ee_itof(static_cast<int32_t>(at<int16_t>(nd + 8u)) << 6);
            const ee_float flare = w + h;
            ee_float left[3], up[3];
            for (int k = 0; k < 3; ++k)
            {
                left[k] = ee_itof(at<int16_t>(nd + 0xcu + 6u * static_cast<uint32_t>(k))) * w * kAxis;
                up[k] = ee_itof(at<int16_t>(nd + 0x10u + 6u * static_cast<uint32_t>(k))) * h * kAxis;
            }
            const uint32_t quad = lights + static_cast<uint32_t>(n) * 0x3cu;
            for (int c = 0; c < 4; ++c)
                for (int k = 0; k < 3; ++k)
                {
                    const ee_float a = (c & 1) ? pos[k] + up[k] : pos[k] - up[k];
                    at<ee_float>(quad + 0xcu * static_cast<uint32_t>(c) + 4u * static_cast<uint32_t>(k)) = c < 2 ? a - left[k] : a + left[k];
                }
            for (int k = 0; k < 3; ++k)
                at<uint32_t>(quad + 0x30u + 4u * static_cast<uint32_t>(k)) = 0;
            for (int c = 0; c < 4; ++c)
                for (int k = 0; k < 3; ++k)
                {
                    ee_float &acc = at<ee_float>(quad + 0x30u + 4u * static_cast<uint32_t>(k));
                    acc = acc + at<ee_float>(quad + 0xcu * static_cast<uint32_t>(c) + 4u * static_cast<uint32_t>(k));
                }
            for (int k = 0; k < 3; ++k)
            {
                ee_float &acc = at<ee_float>(quad + 0x30u + 4u * static_cast<uint32_t>(k));
                acc = acc * kQuarter;
            }
            at<ee_float>(flares + 4u * static_cast<uint32_t>(n)) = side == 0 ? flare : flare * kHeadFlare;
            node = NodeFindNextByTag(node, pDef->pNodeHead, tag);
            if (++n >= 4)
            {
                full = true;
                break;
            }
        }
        if (!full)
            for (; n < 4; ++n)
                at<uint32_t>(lights + static_cast<uint32_t>(n) * 0x3cu + 8u) = 0; // the first corner's z: no light
    }
}

} // namespace rc

RC_DECOMP_FN_SETUP_HEAP(ObjectEngineSFXParms, 0x136d38, rc::fuzz::objectCar)
RC_DECOMP_FN_SETUP_HEAP(ObjectUpdateEngine, 0x135f28, rc::fuzz::objectCar)
RC_DECOMP_FN_SETUP_HEAP(ObjectUpdateSkid, 0x136250, rc::fuzz::objectCar)
RC_DECOMP_FN_SETUP(ObjectInitLights, 0x133ef0, rc::fuzz::objectScratchDef)
RC_DECOMP_FN_SETUP_HEAP(ObjectUpdateOvertaking, 0x137c40, rc::fuzz::objectOvertaking)
RC_DECOMP_FN_SETUP_HEAP(ObjectSfxFrontEnd, 0x1390d0, rc::fuzz::objectFrontEnd)
RC_DECOMP_FN_SETUP_HEAP(ObjectUpdateScrape, 0x136578, rc::fuzz::objectScrape)

// Decompiled from REVSFX.c: the game's layer over SFX.c. RevSfxPlay3D and RevSfxPlay3DCombine place a sound against
// the camera (or both split-screen cameras): its distance sets the volume, the camera's axes the left/right and
// front/back pans, and the flags say what to do with it (4 start it, 8 update a playing one, 0x10 stop it when out of
// range, 0x80 when too quiet, 0x200 play it anyway, 0x40 Dolby rear). The level's ambient sounds (a resource of type
// "SFX": VAG ids and per sound a place, range, type, interval and pitch variance) are loaded by SFXConstructDaemon or
// SFXSetupClass and played each frame by RevSfxUpdateAmbient by type: 0 looping, 1 random intervals, 3 alternating
// pitch, 5 a descending pair, 6 inside a vehicle, 7 and 8 following animated objects, 9 updated by others, 10 rain,
// others at fixed intervals. RevSfxPauseFade fades all voices out for the pause menu and back in.
//
// Never called by the game (no jal, no address taken, no data pointer): RevSfxSetMode, RevsfxDebugInfo and
// RevSfxDolby; RevSfxCalcReverb's only caller (RevSfxStopAll) passes 0, so its split-screen branch is unreachable.
// SFXConstructDaemon is reached through the loader's daemon table and RevSfxTSO as the TSO sound callback (set by
// MainInit).

#include "rc_decomp.h"
#include "sfx_fuzz.h"

// Europe variant: RevSfxUpdateAmbient differs only in register use and instruction order (the same body;
// tools/asm_listing.py NAME --diff).
RC_VERSION_VARIANT(RevSfxUpdateAmbient)

namespace rc {

// sfx.cpp
int16 SfxPlay(int16 wSampleId, uint8 bVolume, uint8 bLRPan, uint8 bFBPan, int16 wPitch, int16 wFlags, int16 chainId);
void SfxStop(int16 wPlayId);
void SfxStopNow(int16 wPlayId);
void SfxKillAllChannels();
void SfxSetReverbDepth(uint8 bDepth);
void SfxSetVolume(uint8 bVolume);
void SfxSetMode(int32 mode);
void SfxPause(int32 flags);
void SfxEffectLRPan(int16 wPlayId, uint8 bLRPan);
void SfxEffectFBPan(int16 wPlayId, uint8 bFBPan);
void SfxEffectVol(int16 wPlayId, uint8 bVolume);
void SfxEffectPitch(int16 wPlayId, int16 wPitch);
void SfxStoreAddress(int32 noVags, int32 type);
void SfxClearSram(int32 no);
// Elsewhere.
uint16 Sqr32(uint32 n);                                         // math_core.cpp
uint32 RandomGetND();                                           // math_core.cpp
uint32 ps2__Vec32Normalise(ptr32<_sVec16> pOut, ptr32<_sVec32> pVec32); // psxdefs.cpp
void ObjectClearSfx(ptr32<_sObject> pObject);                   // object.cpp
void TimerSetup(ptr32<_sTimer> pTimer, tTime time, uint32 flags); // timer.cpp
tTime TimerGet(ptr32<_sTimer> pTimer);
ptr32<void> ResLoad(uint32 id); // res.cpp
ptr32<void> ResReadAlloc();
void ResReAlloc(ptr32<void> p);
void ResTypeSetConstructDaemon(ptr32<char> pType, uint32 constructDaemon, int16 options);

void RevSfxInitAmbient(int32 flags);
void RevSfxInitAmbientRestart();
void RevSfxCalcReverb(int32 split);
void RevSfxStopAll(int32 flags);
int16 RevSfxPlay3D(int16 num, int16 vol, int16 pitch, ptr32<vec32_> pos, int16 flags, int16 range);

namespace {

constexpr uint32_t kRev = sym::revSfxVarG, kSfx = sym::sfxVarG, kMain = sym::mainVarG, kCamera = sym::cameraVarG, kCamera2 = (sym::cameraVarG + 0x5b8u);
constexpr uint32_t kObjects = sym::objectVarG, kAnimData = sym::animVarG, kSifAllocIopHeap = sym::sceSifAllocIopHeap, kSdRemote = sym::sceSdRemote,
                   kSdTransToIOP = sym::sceSdTransToIOP, kFlushCache = sym::FlushCache;
constexpr uint32_t kSfxType = (sym::sec_sdata + 0x378u), kSFXConstructDaemon = sym::SFXConstructDaemon; // "SFX"
constexpr uint32_t kCameraStride = 0x5b8u;
// mainVarG fields.
constexpr uint32_t kStatus = kMain, kGameTimer = kMain + 0x10u, kTime = kMain + 0x1cu, kGameEndPos = kMain + 0x174u,
                   kLevel = kMain + 0x34au, kPauseMenu = kMain + 0x366u, kSplitScreen = kMain + 0x368u;

template <typename T>
T &at(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

revsfxClass &rev() { return G(revSfxVarG); }
uint32_t ambientAt(int32_t i) { return kRev + static_cast<uint32_t>(i) * 0x24u; }
sAmbientSound &ambient(int32_t i) { return at<sAmbientSound>(ambientAt(i)); }
int16_t &handleOf(int32_t i, int32_t slot) { return at<int16_t>(ambientAt(i) + 0xcu + static_cast<uint32_t>(slot) * 2u); }
EffectStruct &effect(int32_t e) { return at<EffectStruct>(kSfx + 0xe8u + static_cast<uint32_t>(e) * 0x20u); }
int32_t sfxMode() { return at<int32_t>(kSfx + 8u); }
int32_t now() { return at<int32_t>(kTime); }

int32_t mul(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b)); }
int32_t abs32(int32_t v) { return v < 0 ? static_cast<int32_t>(0u - static_cast<uint32_t>(v)) : v; }
int32_t add(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b)); }
// divu's remainder (the original breaks on a zero divisor; the setups never give one).
uint32_t remu(uint32_t a, uint32_t b) { return b == 0 ? a : a % b; }

// The game is paused, quitting or over: no sounds are placed.
bool soundsHeld()
{
    return at<uint16_t>(kPauseMenu) == 1 || at<int32_t>(kStatus) == 4 || at<uint32_t>(kGameEndPos) >= 2u;
}

// A pan from a dot product of 16-bit vectors: its top byte (after a shift of 4 or 5) plus 15, as a signed byte,
// clamped to 0..30.
int32_t pan(int32_t dot, uint32_t shift)
{
    int32_t value = static_cast<int8_t>((static_cast<int32_t>(static_cast<uint32_t>(dot) << shift) >> 24) + 0xf);
    if (value < 0)
        value = 0;
    if (value > 0x1e)
        value = 0x1e;
    return value;
}

// The position relative to a camera, in 1/256 units, and its distance (range when any axis is out of range).
struct Placed
{
    int32_t x, y, z, distance;
};

Placed place(uint32_t camera, ptr32<vec32_> pos, int32_t range)
{
    Placed p{};
    p.x = static_cast<int32_t>(static_cast<uint32_t>(pos->x) - static_cast<uint32_t>(at<int32_t>(camera + 0xcu))) >> 8;
    p.y = static_cast<int32_t>(static_cast<uint32_t>(pos->y) - static_cast<uint32_t>(at<int32_t>(camera + 0x10u))) >> 8;
    p.z = static_cast<int32_t>(static_cast<uint32_t>(pos->z) - static_cast<uint32_t>(at<int32_t>(camera + 0x14u))) >> 8;
    p.distance = range;
    if (!(range < abs32(p.x)) && !(range < abs32(p.y)) && !(range < abs32(p.z)))
        p.distance = Sqr32(static_cast<uint32_t>(mul(p.x, p.x)) + static_cast<uint32_t>(mul(p.y, p.y)) +
                           static_cast<uint32_t>(mul(p.z, p.z)));
    return p;
}

// The unit direction (written where the original keeps it) dotted with a camera axis: column 0 (x) at 0x24, 2 at 0x30.
int32_t dotAxis(const _sVec16 &unit, uint32_t camera, uint32_t axis)
{
    return mul(unit.x, at<int16_t>(camera + axis)) + mul(unit.y, at<int16_t>(camera + axis + 2u)) +
           mul(unit.z, at<int16_t>(camera + axis + 4u));
}

void clearObjectSounds()
{
    for (uint32_t i = 0; i < at<uint32_t>(kObjects + 0x170u); ++i)
        ObjectClearSfx(at<ptr32<_sObject>>(kObjects + i * 4u));
}

} // namespace

int16 RevSfxPlay3D(int16 num, int16 vol, int16 pitch, ptr32<vec32_> pos, int16 flags, int16 range)
{
    GuestFrame frame(0xc0);
    const uint32_t camera = (flags & 1) != 0 ? kCamera2 : kCamera;
    const Placed p = place(camera, pos, range);
    if (!(p.distance < range))
    {
        if ((flags & 0x10) != 0)
        {
            SfxStopNow(num);
            return -1;
        }
        if ((flags & 0x200) == 0)
            return -1;
    }
    const int16 volume = static_cast<int16>(ee_div(mul(vol, range - p.distance), range));
    if ((flags & 0x180) != 0 && volume < 0x18)
    {
        if ((flags & 0x80) != 0)
            SfxStopNow(num);
        return -1;
    }
    int32_t lr = 0xf, fb = 0xf;
    if (p.distance != 0)
    {
        const ptr32<_sVec32> delta = frame.local<_sVec32>(0);
        const ptr32<_sVec16> unit = frame.local<_sVec16>(0x40);
        delta->x = p.x;
        delta->y = p.y;
        delta->z = p.z;
        ps2__Vec32Normalise(unit, delta);
        lr = pan(dotAxis(*unit, camera, 0x24u), 4);
        if (sfxMode() == 2 && (flags & 0x40) != 0 && at<uint8_t>(kSplitScreen) == 0)
            fb = pan(mul(p.x, at<int16_t>(camera + 0x30u)) + mul(p.y, at<int16_t>(camera + 0x32u)) +
                         mul(p.z, at<int16_t>(camera + 0x34u)),
                     5);
    }
    if ((flags & 4) != 0)
        return SfxPlay(num, static_cast<uint8>(volume), static_cast<uint8>(lr), static_cast<uint8>(fb), pitch, flags, 0);
    if ((flags & 8) == 0)
        return -1;
    SfxEffectLRPan(num, static_cast<uint8>(lr));
    SfxEffectFBPan(num, static_cast<uint8>(fb));
    SfxEffectVol(num, static_cast<uint8>(volume));
    SfxEffectPitch(num, pitch);
    return num;
}

// As RevSfxPlay3D for both split-screen cameras: the louder volume, and the pans weighted by each camera's volume.
int16 RevSfxPlay3DCombine(int16 num, int16 vol, int16 pitch, ptr32<vec32_> pos, int16 flags, int16 range)
{
    GuestFrame frame(0x100);
    int32_t volumes[2], lrs[2], fbs[2];
    for (uint32_t c = 0; c < 2; ++c)
    {
        const uint32_t camera = kCamera + c * kCameraStride;
        const Placed p = place(camera, pos, range);
        int32_t scaled = mul(vol, range - p.distance);
        if (scaled < 0)
            scaled = 0;
        volumes[c] = static_cast<int16_t>(ee_div(scaled, range));
        lrs[c] = fbs[c] = 0xf;
        if (p.distance != 0 && p.distance != range)
        {
            const ptr32<_sVec32> delta = frame.local<_sVec32>(0);
            const ptr32<_sVec16> unit = frame.local<_sVec16>(0x40);
            delta->x = p.x;
            delta->y = p.y;
            delta->z = p.z;
            ps2__Vec32Normalise(unit, delta);
            lrs[c] = pan(dotAxis(*unit, camera, 0x24u), 4);
            if (sfxMode() == 2)
                fbs[c] = pan(dotAxis(*unit, camera, 0x30u), 4);
        }
    }
    if (volumes[1] == 0 && volumes[0] == 0)
    {
        if ((flags & 0x10) != 0)
            SfxStopNow(num);
        return -1;
    }
    const int32_t sum = volumes[0] + volumes[1];
    const int32_t louder = volumes[1] < volumes[0] ? volumes[0] : volumes[1];
    const int32_t weight1 = static_cast<int16_t>(ee_div(volumes[1] << 10, sum));
    const int32_t weight0 = static_cast<int16_t>(ee_div(volumes[0] << 10, sum));
    const auto part = [](int32_t v) { return (v < 0 ? v + 0x3ff : v) >> 10; };
    const int32_t lr = static_cast<int8_t>(part(mul(weight0, lrs[0])) + part(mul(weight1, lrs[1])));
    const int32_t fb = static_cast<int8_t>(part(mul(weight0, fbs[0])) + part(mul(weight1, fbs[1])));
    if ((flags & 4) != 0)
        return SfxPlay(num, static_cast<uint8>(louder), static_cast<uint8>(lr), static_cast<uint8>(fb), pitch, 0, 0);
    if ((flags & 8) == 0)
        return -1;
    SfxEffectLRPan(num, static_cast<uint8>(lr));
    SfxEffectFBPan(num, static_cast<uint8>(fb));
    SfxEffectVol(num, static_cast<uint8>(louder));
    SfxEffectPitch(num, pitch);
    return num;
}

// Stops every voice but 23 and 47, restarts the ambient sounds; with flags 99 clears the cars' sound handles, with
// other non-zero flags also forgets the ambient sounds and the triggers. Then keys every voice off and turns the
// reverb off.
void RevSfxStopAll(int32 flags)
{
    for (int32_t i = 0; i < 0x30; ++i)
        if (i != 0x17 && i != 0x2f)
            SfxStopNow(static_cast<int16>(i));
    RevSfxInitAmbientRestart();
    if (flags == 99)
    {
        clearObjectSounds();
    }
    else if (flags != 0)
    {
        RevSfxInitAmbient(1);
        clearObjectSounds();
    }
    SfxKillAllChannels();
    rev().rvbLevel = 0;
    RevSfxCalcReverb(0);
}

void RevSfxInit()
{
    revsfxClass &r = rev();
    ResTypeSetConstructDaemon(ptr32<char>(kSfxType), kSFXConstructDaemon, 1);
    r.userVol = 0xc4;
    r.oldUserVol = 0xc4;
    r.pauseSfxHandle3 = -1;
    G(sfxVarG).bMasterVol = static_cast<uint8>(r.userVol);
    r.ambientSoundCt = 0;
    r.genericSoundCt = 0;
    r.fadeCt = 0;
    r.fadeType = 0;
    r.rvbLevel = 0;
    r.pauseSfxHandle = -1;
    r.pauseSfxHandle2 = -1;
    r.ambientActualCt = 0;
    r.oldSfxMode = 0;
    r.startFade = 0;
    r.startFadeMisc = 0;
    r.startFadeRate = 0;
    r.rain = 0;
    r.playPauseSfx = 0;
    r.timerPauseSfx = 0;
    G(IOPBatchFadeBuffer) = guest_call<uint32_t>(kSifAllocIopHeap, 0x180);
    TimerSetup(ptr32<_sTimer>(kRev + 0x1178u), 0, 0);
}

void RevSfxUpdateAmbient(int32 split)
{
    GuestFrame frame(0xc0);
    revsfxClass &r = rev();
    int32_t pitch = 0x100, ambientVol = 0x100;
    if (soundsHeld())
        return;
    if (r.startFade != 0)
    {
        if (r.startFade != 0xff)
        {
            if (r.startFade < 0)
            {
                r.startFade = r.startFade + 1;
                r.startFadeMisc = r.startFadeMisc - r.startFadeRate;
                if (r.startFade == 0)
                    r.startFade = 0xff;
            }
            else
            {
                r.startFade = r.startFade - 1;
                r.startFadeMisc = r.startFadeMisc + r.startFadeRate;
            }
        }
        ambientVol = ee_div(static_cast<int32_t>(static_cast<uint32_t>(r.startFadeMisc) << 8), 10000);
    }
    const int16 volume = static_cast<int16>(ambientVol);
    // The play flags: the sound's Dolby flag with others, as a 16-bit value.
    const auto with = [](sAmbientSound &a, uint32_t bits) { return static_cast<int16>(static_cast<uint16_t>(a.dolby) | bits); };
    // The next random time: an unsigned remainder by the interval, plus the interval.
    const auto randomTime = [](sAmbientSound &a, uint32_t random)
    {
        const int32_t interval = a.randInterval;
        a.timer = static_cast<int32_t>(static_cast<uint32_t>(now()) + remu(random, static_cast<uint32_t>(interval)) +
                                       static_cast<uint32_t>(interval));
    };
    for (int32_t i = 0; i < r.ambientActualCt; ++i)
    {
        sAmbientSound &a = ambient(i);
        const ptr32<vec32_> pos(ambientAt(i));
        const int32_t type = a.type;
        if (type == 0)
        {
            if (a.randInterval == 0)
            {
                if (a.pitchVariance == 0)
                    a.pitchVariance = 0x100;
            }
            else if (a.timer < now())
            {
                randomTime(a, RandomGetND());
                const uint32_t random = RandomGetND();
                const int32_t variance = a.pitchVariance;
                a.pitchVariance = static_cast<int16>(remu(random, static_cast<uint32_t>(variance >> 1)) - ((variance >> 2) - 0x100));
            }
            for (int32_t s = 0; s <= split; ++s)
            {
                int16_t &handle = handleOf(i, s);
                const bool fresh = handle == -1;
                const int16 played = RevSfxPlay3D(fresh ? a.sample : handle, volume, a.pitchVariance, pos,
                                                  static_cast<int16>(s + with(a, fresh ? 4u : 0x18u)), a.range);
                handleOf(i, s) = played;
            }
        }
        else if (type == 1)
        {
            const int32_t variance = a.pitchVariance;
            if (variance == 0)
                pitch = 0x100;
            else if (variance >= 0x101)
                pitch = variance - 0x100;
            else
                pitch = static_cast<int32_t>(remu(RandomGetND(), static_cast<uint32_t>(variance)) + 0x100u) - (variance >> 1);
            if (a.timer < now())
            {
                if (a.timer != 0)
                {
                    RevSfxPlay3D(a.sample, volume, static_cast<int16>(pitch), pos, with(a, 0x104u), a.range);
                    if (split != 0)
                        RevSfxPlay3D(a.sample, volume, static_cast<int16>(pitch), pos, with(a, 5u), a.range);
                }
                randomTime(a, RandomGetND());
            }
        }
        else if (type == 2)
        {
        }
        else if (type == 3)
        {
            if (a.timer < now())
            {
                if (G(lowHigh) != 0)
                {
                    pitch = 0x100 - a.pitchVariance;
                    G(lowHigh) = 0;
                }
                else
                {
                    G(lowHigh) = 1;
                    pitch = 0x100;
                }
                int32_t timer = a.timer;
                if (timer != 0)
                {
                    RevSfxPlay3D(a.sample, volume, static_cast<int16>(pitch), pos, with(a, 0x104u), a.range);
                    if (split != 0)
                        RevSfxPlay3D(a.sample, volume, static_cast<int16>(pitch), pos, with(a, 5u), a.range);
                    timer = a.timer;
                }
                a.timer = timer + a.randInterval;
            }
        }
        else if (type == 5)
        {
            a.randInterval = 0x28a;
            if (a.timer < now())
            {
                const ptr32<vec32_> lower = frame.local<vec32_>(0);
                lower->x = a.pos.x;
                lower->y = a.pos.y;
                lower->z = add(a.pos.z, -1100000);
                for (int32_t s = 0; s <= split; ++s)
                {
                    const int16 flags = static_cast<int16>(s + 0x44);
                    RevSfxPlay3D(a.sample, volume, static_cast<int16>(pitch - static_cast<uint16_t>(a.pitchVariance)), pos,
                                 flags, a.range);
                    RevSfxPlay3D(a.sample, volume, static_cast<int16>(pitch - static_cast<uint16_t>(a.pitchVariance)), lower,
                                 flags, a.range);
                }
                const int16 variance = static_cast<int16>(static_cast<uint16_t>(a.pitchVariance) + 0x20u);
                a.timer = a.timer + a.randInterval;
                a.pitchVariance = variance;
                if (variance >= 0x81)
                    a.pitchVariance = 0;
            }
        }
        else if (type == 6)
        {
            // Inside a vehicle: started when the camera follows a boat or plane (type 0x14 on), stopped otherwise.
            const uint32_t target = at<uint32_t>(at<uint32_t>(kCamera + 0xb70u) + 0xacu);
            if (a.timer == 0)
            {
                if (target != 0 && at<uint32_t>(at<uint32_t>(target + 0xcu) + 4u) >= 0x14u)
                {
                    const int16 played = SfxPlay(a.sample, 0x3c, 0xf, 0xf, 0x100, 0, 0);
                    a.timer = 1;
                    a.pitchVariance = played;
                }
            }
            else if (target != 0 && at<uint32_t>(at<uint32_t>(target + 0xcu) + 4u) < 0x14u)
            {
                if (a.pitchVariance != -1)
                {
                    SfxStop(a.pitchVariance);
                    a.pitchVariance = -1;
                }
                a.timer = 0;
            }
        }
        else if (static_cast<uint32_t>(type - 7) < 2u)
        {
            // Follows the first animated object with the tag (7: 0x1000047b; 8: 0x10000501 or 0x100005d4); when none
            // is found the entry after the last is used.
            const uint32_t anim = at<uint32_t>(kAnimData);
            uint32_t entry = anim != 0 ? anim + 8u : 0u;
            if (entry == 0)
                continue;
            const int32_t count = static_cast<int16_t>(at<uint16_t>(anim));
            for (int32_t k = 0; k < count; ++k, entry += 0x64u)
            {
                const uint32_t tag = at<uint32_t>(at<uint32_t>(entry));
                if (type == 7 ? tag == 0x1000047bu : tag == 0x10000501u || tag == 0x100005d4u)
                    break;
            }
            if (entry == 0)
                continue;
            if ((at<uint16_t>(entry + 8u) & 0x40) == 0)
            {
                for (int32_t s = 0; s <= split; ++s)
                    if (handleOf(i, s) != -1)
                    {
                        SfxStop(handleOf(i, s));
                        handleOf(i, s) = -1;
                    }
            }
            else if (effect(a.sample).dwLoop == 0)
            {
                if (a.timer < now())
                {
                    const int16 sample = a.sample;
                    RevSfxPlay3D(sample, volume, 0x100, ptr32<vec32_>(at<uint32_t>(entry) + 0x10u), 0x44, a.range);
                    if (split != 0)
                        RevSfxPlay3D(a.sample, volume, 0x100, ptr32<vec32_>(at<uint32_t>(entry) + 0x10u), 0x45, a.range);
                    randomTime(a, RandomGetND());
                }
            }
            else
            {
                for (int32_t s = 0; s <= split; ++s)
                {
                    const int16 handle = handleOf(i, s);
                    const uint32_t object = at<uint32_t>(entry);
                    const bool fresh = handle == -1;
                    const int16 played = RevSfxPlay3D(fresh ? a.sample : handle, volume, 0x100, ptr32<vec32_>(object + 0x10u),
                                                      static_cast<int16>(s + (fresh ? 0x44 : 0x58)), a.range);
                    handleOf(i, s) = played;
                }
            }
        }
        else if (type == 9)
        {
            if (a.pitchVariance == 0)
                a.pitchVariance = 0x100;
            if (a.handle[0] != -1)
            {
                const int16 played = RevSfxPlay3D(a.handle[0], volume, a.pitchVariance, pos, with(a, 0x18u), a.range);
                a.handle[0] = played;
            }
        }
        else if (type == 10)
        {
            if (r.rain == 0)
            {
                if (a.handle[0] != -1)
                {
                    SfxStop(a.handle[0]);
                    a.handle[0] = -1;
                }
            }
            else
            {
                const int16 handle = a.handle[0];
                int32_t level = static_cast<int32_t>(static_cast<uint32_t>(r.rain) * 4u + 0x14u);
                const uint8 lrPan = a.randInterval == 0 ? 0 : 0x1e;
                if (!(level < 0x8d))
                    level = 0x8c;
                const int32_t scaled = mul(level, ambientVol);
                const uint8 rainVolume = static_cast<uint8>((scaled < 0 ? scaled + 0xff : scaled) >> 8);
                if (handle == -1)
                {
                    const int16 played = SfxPlay(a.sample, rainVolume, lrPan, 0xf, 0x100, 0, 0);
                    a.handle[0] = played;
                }
                else
                {
                    SfxEffectVol(handle, rainVolume);
                }
            }
        }
        else
        {
            // At a fixed interval: the type.
            if (a.timer < now())
            {
                int32_t timer = a.timer, interval = type;
                if (timer != 0)
                {
                    RevSfxPlay3D(a.sample, volume, 0x100, pos, with(a, 0x104u), a.range);
                    if (split != 0)
                        RevSfxPlay3D(a.sample, volume, 0x100, pos, with(a, 5u), a.range);
                    interval = a.type;
                    timer = a.timer;
                }
                a.timer = timer + interval;
            }
        }
    }
}

// Flags 1: save the volumes and start fading out (ten frames); 2: start fading back in. Otherwise one step of the
// fade under way, sent to the IOP as one batch.
void RevSfxPauseFade(int32 flags)
{
    revsfxClass &r = rev();
    sfxClass &s = G(sfxVarG);
    if ((flags & 1) != 0)
    {
        SfxPause(0);
        r.fadeType = 1;
        r.fadeCt = 10;
        return;
    }
    if ((flags & 2) != 0)
    {
        r.fadeCt = 0;
        r.fadeType = 2;
        return;
    }
    if (r.fadeType != 1 && r.fadeType != 2)
        return;
    GuestFrame frame(0x360);
    const bool fadingOut = r.fadeType == 1;
    const uint32_t batch = frame.local<sceSdBatch>(0).address;
    uint32_t count = 0;
    const auto add = [&](int32_t v, int32_t left, int32_t right)
    {
        const uint32_t entry = v < 0x18 ? static_cast<uint32_t>(v) << 1 : (static_cast<uint32_t>(v - 0x18) << 1) | 1u;
        const uint32_t at0 = batch + count * 8u;
        at<int32_t>(at0 + 4u) = left;
        at<uint16_t>(at0) = 1;
        at<uint16_t>(at0 + 2u) = static_cast<uint16_t>(entry);
        at<uint16_t>(at0 + 10u) = static_cast<uint16_t>(entry | 0x100u);
        at<int32_t>(at0 + 12u) = right;
        at<uint16_t>(at0 + 8u) = 1;
        count += 2;
    };
    if (fadingOut)
    {
        if (r.fadeCt == 0)
        {
            r.fadeType = 0;
            r.fadeCt = 0x7f;
        }
        for (int32_t v = 0; v < 0x30; ++v)
        {
            VoiceStruct &vc = at<VoiceStruct>(kSfx + 0x1268u + static_cast<uint32_t>(v) * 0x18u);
            if ((vc.wLeftVol == 0 && vc.wRightVol == 0) || vc.wPlayId == -1)
                continue;
            const int32_t left = vc.wLeftVol;
            vc.wLeftVol = static_cast<int16>(left - ee_div(left, r.fadeCt));
            vc.wRightVol = static_cast<int16>(vc.wRightVol - ee_div(vc.wRightVol, r.fadeCt));
            if (v == 0x17 || v == 0x2f)
                continue;
            if (r.fadeType == 0)
                add(v, 0, 0);
            else
                add(v, ee_div(mul(vc.wLeftVol, s.bMasterVol), 0xff), ee_div(mul(vc.wRightVol, s.bMasterVol), 0xff));
        }
    }
    else
    {
        if (r.fadeCt == 10)
        {
            r.fadeType = 0;
            r.fadeCt = 0x7f;
        }
        for (int32_t v = 0; v < 0x30; ++v)
        {
            VoiceStruct &vc = at<VoiceStruct>(kSfx + 0x1268u + static_cast<uint32_t>(v) * 0x18u);
            const int32_t savedLeft = s.pwVolumeStore[2 * v], savedRight = s.pwVolumeStore[2 * v + 1];
            if (vc.wLeftVol == savedLeft && vc.wRightVol == vc.wLeftVol + 1)
                continue;
            if (vc.wPlayId == -1 || v == 0x17 || v == 0x2f)
                continue;
            if (r.fadeType == 0)
            {
                add(v, ee_div(mul(savedLeft, s.bMasterVol), 0xff), ee_div(mul(savedRight, s.bMasterVol), 0xff));
            }
            else
            {
                const int32_t step = r.fadeCt;
                vc.wLeftVol = static_cast<int16>(ee_div(mul(savedLeft, step), 10));
                vc.wRightVol = static_cast<int16>(ee_div(mul(savedRight, step), 10));
                add(v, ee_div(mul(vc.wLeftVol, s.bMasterVol), 0xff), ee_div(mul(vc.wRightVol, s.bMasterVol), 0xff));
            }
        }
    }
    guest_call(kFlushCache, 0);
    if (count != 0)
    {
        guest_call(kSdTransToIOP, batch, G(IOPBatchFadeBuffer), count << 3, 1);
        guest_call(kSdRemote, 1, 0x80b0, G(IOPBatchFadeBuffer), 0, count);
    }
    r.fadeCt = fadingOut ? r.fadeCt - 1 : r.fadeCt + 1;
}

void RevSfxCalcReverb(int32 split)
{
    SfxSetReverbDepth(split == 0 ? static_cast<uint8>(rev().rvbLevel) : 0);
}

// Loads the level's VAGs (type 0: the generic sounds, once; otherwise the ambient sounds, replacing those loaded) and
// records the ambient sounds. A VAG listed twice is loaded once; its later entries take the sample of the last VAG
// loaded before them (the original keeps it in a register).
void SFXConstructDaemon(ptr32<memHandle> pHandle)
{
    GuestFrame frame(0xb0);
    revsfxClass &r = rev();
    const uint32_t header = at<uint32_t>(pHandle.address);
    const int32_t type = at<int32_t>(header + 4u), noVags = at<int32_t>(header);
    ptr32<void> mem = nullptr;
    int32_t unique = 0;
    if (type != 0)
    {
        SfxClearSram(r.genericSoundCt);
    }
    else
    {
        mem = ResReadAlloc();
        if (r.genericSoundCt != 0)
        {
            ResReAlloc(mem);
            return;
        }
        r.genericSoundCt = 0;
    }
    r.ambientSoundCt = 0;
    r.ambientActualCt = 0;
    uint32_t loaded = 0;
    for (int32_t i = 0; i < noVags; ++i)
    {
        const uint32_t vag = at<uint32_t>(pHandle.address) + static_cast<uint32_t>(i) * 0x1cu + 8u;
        const int32_t id = at<int32_t>(vag + 0x18u);
        r.vagResList[i].resId = id;
        int32_t same = 0;
        while (same < i && r.vagResList[same].resId != id)
            ++same;
        const bool repeat = same < i;
        if (repeat)
        {
            r.vagResList[i].sfxEffectNo = r.vagResList[same].sfxEffectNo;
        }
        else
        {
            ++unique;
            loaded = ResLoad(static_cast<uint32_t>(at<int32_t>(vag + 0x18u))).address;
            r.vagResList[i].sfxEffectNo = static_cast<int32_t>(loaded);
        }
        if (type == 0)
        {
            ++r.genericSoundCt;
            continue;
        }
        sAmbientSound &a = ambient(r.ambientActualCt);
        a.pos.x = at<int32_t>(vag);
        a.sample = static_cast<int16>(static_cast<int32_t>(loaded) >> 2);
        a.handle[1] = -1;
        a.pos.y = at<int32_t>(vag + 4u);
        a.handle[0] = -1;
        a.pos.z = at<int32_t>(vag + 8u);
        const int16 range = at<int16_t>(vag + 0xcu);
        a.range = range;
        a.type = at<int32_t>(vag + 0x10u);
        a.randInterval = at<int16_t>(vag + 0x14u);
        a.pitchVariance = at<int16_t>(vag + 0x16u);
        if (range < 0)
        {
            a.range = static_cast<int16>(-range);
            a.dolby = 0x40;
        }
        else
        {
            a.dolby = 0;
        }
        a.timer = 0;
        if (!repeat)
            ++r.ambientSoundCt;
        ++r.ambientActualCt;
    }
    if (type != 0)
    {
        SfxStoreAddress(unique + r.genericSoundCt, type);
        return;
    }
    SfxStoreAddress(noVags, 0);
    ResReAlloc(mem);
}

// As SFXConstructDaemon for VAGs already loaded: the level's SFX data, its samples numbered after the generic ones.
void SFXSetupClass()
{
    revsfxClass &r = rev();
    sfxClass &s = G(sfxVarG);
    const uint32_t data = at<uint32_t>(at<uint32_t>(sym::pLvlG) + 0x24u); // pLvlG->pSFXData
    if (s.effectList[0].dwAddr == s.endOfGenericAddr)
    {
        r.genericSoundCt = 0;
    }
    else
    {
        for (int32_t e = 1; e < 0x8c; ++e)
            if (s.effectList[e].dwAddr == s.endOfGenericAddr)
            {
                r.genericSoundCt = e;
                break;
            }
    }
    r.ambientSoundCt = 0;
    r.ambientActualCt = 0;
    int32_t next = 0;
    for (int32_t i = 0; i < at<int32_t>(data); ++i)
    {
        const uint32_t vag = data + 8u + static_cast<uint32_t>(i) * 0x1cu;
        sAmbientSound &a = ambient(r.ambientActualCt);
        const int32_t id = at<int32_t>(vag + 0x18u);
        r.vagResList[i].resId = id;
        int32_t same = 0;
        while (same < i && r.vagResList[same].resId != id)
            ++same;
        if (same < i)
        {
            a.sample = static_cast<int16>(r.vagResList[same].sfxEffectNo);
        }
        else
        {
            const int16 sample = static_cast<int16>(static_cast<uint16_t>(r.genericSoundCt) + next);
            a.sample = sample;
            ++next;
            r.vagResList[i].sfxEffectNo = sample;
            ++r.ambientSoundCt;
        }
        a.pos.x = at<int32_t>(vag);
        a.handle[1] = -1;
        a.handle[0] = -1;
        a.pos.y = at<int32_t>(vag + 4u);
        a.pos.z = at<int32_t>(vag + 8u);
        const int16 range = at<int16_t>(vag + 0xcu);
        a.range = range;
        a.type = at<int32_t>(vag + 0x10u);
        a.timer = 0;
        a.randInterval = at<int16_t>(vag + 0x14u);
        a.pitchVariance = at<int16_t>(vag + 0x16u);
        if (range < 0)
        {
            a.dolby = 0x40;
            a.range = static_cast<int16>(-range);
        }
        else
        {
            a.dolby = 0;
        }
        ++r.ambientActualCt;
    }
}

// A one-shot trigger sound (sample num + 0x5d): repeat 0 plays it now and marks it 2 (playing; it becomes playable
// again when not triggered for a second), 1 plays it again after the sample's length.
void RevSfxTrigger(int16 num, ptr32<SVECTOR> pPos)
{
    (void)pPos;
    sTriggerSfx &t = at<sTriggerSfx>(kRev + 0x1188u + static_cast<uint32_t>(num) * 8u);
    const int32_t sample = t.vagNo + 0x5d;
    const tTime time = TimerGet(ptr32<_sTimer>(kGameTimer));
    if (t.repeat == 2)
    {
        if (add(t.timer, 1000) < time)
            t.repeat = 0;
        t.timer = time;
    }
    if (t.repeat == 0)
    {
        SfxPlay(static_cast<int16>(sample), 0xff, 0xf, 0xf, 0x100, 0, 0);
        t.repeat = 2;
        t.timer = time;
    }
    else if (t.repeat == 1 && t.timer < time)
    {
        const int32_t late = add(time, -t.timer);
        const EffectStruct &ef = effect(sample);
        const int32_t length = ee_div(mul(ef.dwSize, 0x708), ef.dwMulti == 0x800 ? 0x5622 : 0x2b11);
        SfxPlay(static_cast<int16>(sample), 0xff, 0xf, 0xf, 0x100, 0, 0);
        t.timer = add(time, length);
        if (late < 0x32)
            t.timer = add(add(time, length), -late);
    }
}

void RevSfxSpecialTrigger(int16 num, ptr32<vec32_> pPos, tTime time)
{
    sAmbientSound &a = ambient(num);
    if (!(a.timer < time))
        return;
    const EffectStruct &ef = effect(a.sample);
    const int32_t length = ee_div(mul(ef.dwSize, 0x708), ef.dwMulti == 0x800 ? 0x5622 : 0x2b11);
    RevSfxPlay3D(a.sample, 0x100, 0x100, pPos ? pPos : ptr32<vec32_>(ambientAt(num)), 4, a.range);
    ambient(num).timer = add(time, length);
}

void RevSfxInitialReverb(int32 split)
{
    if (split != 0)
        return;
    const uint32_t level = at<uint8_t>(kLevel);
    int32_t depth = 0;
    if (level >= 2 && level < 0x4e)
        switch ((level - 2) % 12)
        {
        case 0: depth = 0x28; break; // levels 2, 14, ... 74
        case 2: depth = 0x50; break; // 4, 16, ... 76
        case 3: depth = 0x46; break; // 5, 17, ... 77
        default: break;
        }
    rev().rvbLevel = depth;
    SfxSetReverbDepth(static_cast<uint8>(depth));
}

void RevSfxAnim(ptr32<animHead> pAnimHead, int32 flags)
{
    (void)flags;
    if (soundsHeld())
        return;
    animHead &h = *pAnimHead;
    if (h.sfxSample == -1)
        return;
    GuestFrame frame(0x70);
    const ptr32<vec32_> pos = frame.local<vec32_>(0);
    const uint32_t tso = h.pTSOData.address;
    pos->x = at<int32_t>(tso + 0x10u);
    pos->y = at<int32_t>(tso + 0x14u);
    pos->z = at<int32_t>(tso + 0x18u);
    const int16 sample = static_cast<int16>(h.sfxSample + 0x5d);
    if (h.sfxMisc != 1)
    {
        const int16 pitch = h.sfxMisc != 0 ? h.sfxMisc : 0x100;
        if (h.sfxHandle == -1)
            h.sfxHandle = static_cast<int8>(RevSfxPlay3D(sample, 0x80, pitch, pos, 0x44, 0x1194));
        else
            h.sfxHandle = static_cast<int8>(RevSfxPlay3D(h.sfxHandle, 0x80, pitch, pos, 0x58, 0x1194));
        return;
    }
    // Once per pass of the animation: played when the frame passes sfxTimer, then the handle holds the frame.
    if (h.sfxTimer < h.lastFrame && !(h.sfxHandle > 0))
    {
        RevSfxPlay3D(sample, 0x80, 0x100, pos, 4, 0x1194);
        h.sfxHandle = static_cast<int8>(h.lastFrame);
    }
    if (h.sfxHandle != 0 && h.lastFrame < h.sfxHandle)
        h.sfxHandle = -1;
}

// A scenery object's sound (an ambient entry; 0x200: start if not playing, 0x400: stop, neither: restart).
void RevSfxTSO(int16 playId, ptr32<vec32_> pPos)
{
    (void)pPos;
    if (soundsHeld())
        return;
    int32_t id = playId, kind = 0;
    if ((id & 0x200) != 0)
    {
        kind = 0x200;
        id = static_cast<int16_t>(id - 0x200);
    }
    if ((id & 0x400) != 0)
    {
        kind = 0x400;
        id = static_cast<int16_t>(id - 0x400);
    }
    sAmbientSound &a = ambient(id);
    const ptr32<vec32_> pos(ambientAt(id));
    if (a.pitchVariance == 0)
        a.pitchVariance = 0x100;
    if (kind == 0)
    {
        if (a.type == 2)
        {
            RevSfxPlay3D(a.sample, 0x100, a.pitchVariance, pos, static_cast<int16>(static_cast<uint16_t>(a.dolby) | 4u), a.range);
            return;
        }
        if (a.handle[0] != -1)
        {
            SfxStopNow(a.handle[0]);
            a.handle[0] = -1;
        }
    }
    else if (kind == 0x200)
    {
        if (a.handle[0] != -1)
            return;
    }
    else
    {
        if (a.handle[0] != -1)
        {
            SfxStop(a.handle[0]);
            a.handle[0] = -1;
        }
        return;
    }
    const int16 played =
        RevSfxPlay3D(a.sample, 0x100, a.pitchVariance, pos, static_cast<int16>(static_cast<uint16_t>(a.dolby) | 4u), a.range);
    a.handle[0] = played;
}

void RevSfxSetMode(int32 mode)
{
    SfxSetMode(mode);
    RevSfxStopAll(99);
}

// Forgets the ambient sounds (their places stay), the VAG list and the triggers; flags 0 also the count of unique
// ambient VAGs.
void RevSfxInitAmbient(int32 flags)
{
    revsfxClass &r = rev();
    for (int32_t i = 0; i < 100; ++i)
    {
        r.vagResList[i].resId = 0;
        r.vagResList[i].sfxEffectNo = 0;
        sAmbientSound &a = ambient(i);
        a.type = -1;
        a.timer = 0;
        a.dolby = 0;
        a.handle[0] = -1;
        a.handle[1] = -1;
        a.sample = 0;
        a.range = 0;
        a.randInterval = 0;
        a.pitchVariance = 0;
    }
    for (int32_t i = 0; i < 24; ++i)
    {
        r.revSfxTriggers[i].vagNo = 0;
        r.revSfxTriggers[i].repeat = 0;
        r.revSfxTriggers[i].timer = 0;
    }
    r.noTriggerSfx = 0;
    if (flags == 0)
        r.ambientSoundCt = 0;
    r.ambientActualCt = 0;
    r.fadeCt = 0x7f;
}

void RevSfxInitAmbientRestart()
{
    revsfxClass &r = rev();
    for (int32_t i = 0; i < 100; ++i)
    {
        sAmbientSound &a = ambient(i);
        a.handle[0] = -1;
        a.handle[1] = -1;
        a.timer = 0;
    }
    r.timerPauseSfx = 0;
    r.startFade = 0;
    r.startFadeMisc = 0;
    r.startFadeRate = 0;
    r.playPauseSfx = 0;
}

void RevsfxDebugInfo() {}

void RevSfxReset()
{
    revsfxClass &r = rev();
    if (r.ambientSoundCt == 0)
        return;
    SfxClearSram(r.genericSoundCt);
    r.ambientSoundCt = 0;
    G(sfxVarG).lastAmbientIndex = 0;
    G(sfxVarG).firstAmbientIndex = 0;
}

// The user's volume, and the pause menu's tick (sample 0x45) every half second while it is shown.
void RevSfxModifyVolume(int32 flag)
{
    revsfxClass &r = rev();
    const tTime time = TimerGet(ptr32<_sTimer>(kRev + 0x1178u));
    if (r.oldUserVol != r.userVol)
        SfxSetVolume(static_cast<uint8>(r.userVol));
    if (flag != 0 && flag != 3)
        r.playPauseSfx = 1;
    if (r.playPauseSfx != 0 && r.timerPauseSfx < time)
    {
        SfxPlay(0x45, 0x60, 0xf, 0xf, 0x96, 0, 0);
        r.timerPauseSfx = time + 500;
    }
    r.oldUserVol = r.userVol;
}

void RevSfxStopPauseSfx()
{
    revsfxClass &r = rev();
    if (r.playPauseSfx == 0)
        return;
    r.timerPauseSfx = 0;
    r.playPauseSfx = 0;
}

// The start-of-race fade of the ambient sounds: over |noFrames| frames from percentage to 100% (noFrames > 0), or
// from 100% down (noFrames < 0), in units of 1/10000.
void RevSfxReadyGoFade(int32 noFrames, int32 percentage)
{
    revsfxClass &r = rev();
    const int32_t frames = abs32(noFrames);
    if (noFrames < 0)
    {
        r.startFadeMisc = 10000;
        r.startFade = noFrames;
    }
    else
    {
        r.startFade = noFrames;
        r.startFadeMisc = mul(percentage, 100);
    }
    r.startFadeRate = ee_div(mul(100 - percentage, 100), frames);
}

// flags 0: remember the sound mode and drop Dolby to stereo if mode is set; otherwise put the remembered mode back.
void RevSfxDolby(int32 mode, int32 flags)
{
    revsfxClass &r = rev();
    sfxClass &s = G(sfxVarG);
    if (flags != 0)
    {
        s.mode = r.oldSfxMode;
        return;
    }
    r.oldSfxMode = s.mode;
    if (s.mode == 2 && mode != 0)
        s.mode = 1;
}

} // namespace rc

RC_DECOMP_FN_BLOCKING_SETUP(RevSfxPlay3D, rc::fuzz::revPlay3D)
RC_DECOMP_FN_BLOCKING_SETUP(RevSfxPlay3DCombine, rc::fuzz::revPlay3D)
RC_DECOMP_FN_BLOCKING_SETUP_HEAP(RevSfxStopAll, rc::fuzz::revStopAll)
RC_DECOMP_FN_BLOCKING_SETUP_HEAP(RevSfxInit, rc::fuzz::revInit)
RC_DECOMP_FN_BLOCKING_SETUP(RevSfxUpdateAmbient, rc::fuzz::revAmbient)
RC_DECOMP_FN_BLOCKING_SETUP(RevSfxPauseFade, rc::fuzz::revPauseFade)
RC_DECOMP_FN_BLOCKING_SETUP(RevSfxCalcReverb, rc::fuzz::revReverb)
RC_DECOMP_FN_BLOCKING_SETUP_HEAP(SFXConstructDaemon, rc::fuzz::revSfxDaemon)
RC_DECOMP_FN_SETUP(SFXSetupClass, rc::fuzz::revSetupClass)
RC_DECOMP_FN_BLOCKING_SETUP(RevSfxTrigger, rc::fuzz::revTrigger)
RC_DECOMP_FN_BLOCKING_SETUP(RevSfxSpecialTrigger, rc::fuzz::revSpecialTrigger)
RC_DECOMP_FN_BLOCKING_SETUP(RevSfxInitialReverb, rc::fuzz::revReverb)
RC_DECOMP_FN_BLOCKING_SETUP(RevSfxAnim, rc::fuzz::revAnim)
RC_DECOMP_FN_BLOCKING_SETUP(RevSfxTSO, rc::fuzz::revTso)
RC_DECOMP_FN_BLOCKING_SETUP_HEAP(RevSfxSetMode, rc::fuzz::revStopAll)
RC_DECOMP_FN_SETUP(RevSfxInitAmbient, rc::fuzz::revAmbientList)
RC_DECOMP_FN_SETUP(RevSfxInitAmbientRestart, rc::fuzz::revAmbientList)
RC_DECOMP_FN(RevsfxDebugInfo)
RC_DECOMP_FN_SETUP(RevSfxReset, rc::fuzz::revAmbientList)
RC_DECOMP_FN_BLOCKING_SETUP(RevSfxModifyVolume, rc::fuzz::revModifyVolume)
RC_DECOMP_FN_SETUP(RevSfxStopPauseSfx, rc::fuzz::revAmbientList)
RC_DECOMP_FN_SETUP(RevSfxReadyGoFade, rc::fuzz::revReadyGo)
RC_DECOMP_FN_SETUP(RevSfxDolby, rc::fuzz::revDolby)

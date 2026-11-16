// Decompiled from SFX.c: the sound effect player on the IOP's SPU2, through the SDK's remote sound library
// (sceSdRemote, an RPC to the IOP's sdrdrv; each call sends a command and up to six argument words and reads one word
// back).
//
// A sound effect is a VAG sample in SPU2 memory (effectList, 140 entries: address, size, rate multiplier, loop flag,
// loop address). Playing one takes a play id (playList, 48: control flags, effect flags, pitch, volume, pans, its one
// or two voices and a chained play id) and a voice (voiceList, 48; voices 23 and 47 are never handed out), sets the
// voice's start address and pitch at once and marks it to key on at the next SfxDoSfx. SfxDoSfx, once a frame, keys
// on the voices whose delay ran out, frees voices whose sample ended (the ENDX switches read back from both cores),
// fades out stopped voices over five frames ("no click"), and sends the changed volumes and pitches to the IOP as one
// batch. Mode 0 is mono, 1 stereo, 2 Dolby: a Dolby sound (effect flag 0x40) uses a front voice and a rear voice
// whose volumes come from the front/back pan, and SetDolbyVolume starts or stops the second voice as the pan moves,
// at the address the first voice has reached.
//
// Never called by the game (no jal, no address taken, no data pointer): SfxReInitReverb, SfxOff, SfxSetCallBack,
// SfxEffectGetVol and the empty SfxUnInitReverb, SfxReverbOn, SfxReverbOff, SfxUnInit, SfxContinue, SfxFreeEffect,
// SfxDumpSram, SfxCdInit, SfxSetCdVol, SfxCdSetReverb and SFX_TVAG. VAGConstructDaemon is reached through the
// loader's daemon table.

#include "rc_decomp.h"
#include "sfx_fuzz.h"

namespace rc {

ptr32<void> ResReadAlloc(); // res.cpp
void ResReAlloc(ptr32<void> p);
void ResTypeSetConstructDaemon(ptr32<char> pType, uint32 constructDaemon, int16 options);

int16 GetFreePlayId();
int16 GetFreeVoice();
int32 Big2Little(int32 dwNum);
int32 SfxDetermineLoop(ptr32<void> pVagPtr, int32 dwSize);
void SfxGetLoopAddress(int32 wSampleId, ptr32<char> pMem);
void SfxStopVoice(int16 wVoiceId);
void SetEffectVolume(int16 wPlayId);
void SetMonoVolume(int16 wPlayId);
void SetStereoVolume(int16 wPlayId);
void SetDolbyVolume(int16 wPlayId);

namespace {

constexpr uint32_t kSfx = 0x31e488u; // sfxVarG
constexpr uint32_t kSdRemote = 0x1ca550u, kSdRemoteInit = 0x1ca408u, kSdTransToIOP = 0x1ca4d0u, kSifAllocIopHeap = 0x1c18c8u,
                   kSifFreeIopHeap = 0x1c1938u, kSifInitIopHeap = 0x1c1838u, kFlushCache = 0x1be9a0u, kPrintf = 0x12e9e8u;
constexpr uint32_t kVagType = 0x3283d8u, kVAGConstructDaemon = 0x1adc70u; // "VAG"
constexpr uint32_t kAllocatedFormat = 0x325130u, kSendingFormat = 0x325150u;
constexpr uint32_t kStereoLookup = 0x31e448u, kRearLookup = 0x31e468u;
// sdrdrv commands.
constexpr uint32_t kSetParam = 0x8010u, kSetSwitch = 0x8030u, kGetSwitch = 0x8040u, kSetAddr = 0x8050u, kGetAddr = 0x8060u,
                   kSetCoreAttr = 0x8070u, kProcBatch = 0x80b0u, kVoiceTrans = 0x80d0u, kVoiceTransStatus = 0x80f0u,
                   kSetEffectAttr = 0x8130u, kClearEffectWorkArea = 0x8150u, kInit = 0x8000u;

template <typename T>
T &at(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

sfxClass &sfx() { return G(sfxVarG); }
// Elements by index, as the original addresses them (an index of -1 reads the word before the array).
VoiceStruct &voice(int32_t v) { return at<VoiceStruct>(kSfx + 0x1268u + static_cast<uint32_t>(v) * 0x18u); }
PlayStruct &play(int32_t p) { return at<PlayStruct>(kSfx + 0x16e8u + static_cast<uint32_t>(p) * 0x14u); }
EffectStruct &effect(int32_t e) { return at<EffectStruct>(kSfx + 0xe8u + static_cast<uint32_t>(e) * 0x20u); }

int32_t mul(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b)); }

// A voice's SPU2 parameter address: the voice within its core, shifted, and the core in bit 0.
uint32_t voiceEntry(int32_t v) { return v < 0x18 ? static_cast<uint32_t>(v) << 1 : (static_cast<uint32_t>(v - 0x18) << 1) | 1u; }
uint32_t voiceCore(int32_t v) { return v < 0x18 ? 0u : 1u; }
uint32_t voiceBit(int32_t v) { return 1u << ((v < 0x18 ? v : v - 0x18) & 31); }

// The IOP call, blocking (mode 1). Argument words are passed as registers, sign-extended.
template <typename... A>
int32_t sdRemote(uint32_t command, A... args)
{
    return guest_call<int32_t>(kSdRemote, 1, command, static_cast<int32_t>(args)...);
}

// Keys a voice off and frees it.
void keyOffVoice(int32_t v)
{
    sdRemote(kSetSwitch, voiceCore(v) | 0x1600u, voiceBit(v));
}

void clearVoice(VoiceStruct &vc)
{
    vc.wFlags = 0;
    vc.wPlayId = -1;
    vc.wDelay = 0;
    vc.noClickCount = 0;
    vc.bVolChanged = 0;
}

// The reverb attributes SfxInitReverb and SfxReInitReverb send: mode 1, no depth, delay or feedback.
void reverbDefaults(sceSdEffectAttr &attr)
{
    attr.core = 0;
    attr.mode = 1;
    attr.depth_L = 0;
    attr.depth_R = 0;
    attr.delay = 0;
    attr.feedback = 0;
}

// The rest of both reverb set-ups: both cores' effect attributes from attr, work areas cleared, effects on, the
// depths from attr, and the four effect switches.
void reverbSetup(ptr32<sceSdEffectAttr> attr, uint32_t switches)
{
    sdRemote(kSetEffectAttr, 0, attr.address);
    sdRemote(kSetEffectAttr, 1, attr.address);
    sdRemote(kClearEffectWorkArea, 0, 0, 1);
    sdRemote(kClearEffectWorkArea, 1, 0, 1);
    sdRemote(kSetCoreAttr, 2, 1);
    sdRemote(kSetCoreAttr, 3, 1);
    sdRemote(kSetParam, 0xb80, attr->depth_L);
    sdRemote(kSetParam, 0xc80, attr->depth_R);
    sdRemote(kSetParam, 0xb81, attr->depth_L);
    sdRemote(kSetParam, 0xc81, attr->depth_R);
    sdRemote(kSetSwitch, 0x1900, switches);
    sdRemote(kSetSwitch, 0x1b00, switches);
    sdRemote(kSetSwitch, 0x1901, switches);
    sdRemote(kSetSwitch, 0x1b01, switches);
}

// The play id test of the effect setters: sound on, a real id below 48 (as an unsigned 16-bit value), playing.
bool playing(int16 wPlayId)
{
    return sfx().bInitOk != 0 && wPlayId != -1 && static_cast<uint16_t>(wPlayId) < 0x30u &&
           (play(wPlayId).wControlFlags & 2) != 0;
}

} // namespace

int32 SfxInit(int32 mode)
{
    sfxClass &s = sfx();
    G(IOPBatchBuffer) = guest_call<uint32_t>(kSifAllocIopHeap, 0x240);
    ResTypeSetConstructDaemon(ptr32<char>(kVagType), kVAGConstructDaemon, 1);
    for (int32_t e = 0; e < 0x8c; ++e)
    {
        EffectStruct &ef = effect(e);
        ef.dwAddr = 0;
        ef.dwSize = 0;
        ef.dwImgAddr = 0;
        ef.dwImgSize = 0;
        ef.dwLoop = 0;
        ef.dwLoopAddr = 0;
    }
    for (int32_t v = 0; v < 0x30; ++v)
    {
        VoiceStruct &vc = voice(v);
        vc.bVolChanged = 0;
        vc.bPitchChanged = 0;
        vc.noClickCount = 0;
        vc.bStopNow = 0;
        vc.dwStoreAddress = 0;
        vc.wFlags = 0;
        vc.wDelay = 0;
        vc.wPlayId = -1;
        vc.dwStoreLoopAddress = 0;
        PlayStruct &p = play(v);
        p.wControlFlags = 0;
        p.bVoice1 = -1;
        p.bVoice2 = -1;
    }
    s.SPUCurrentAddr = 0x5000;
    s.mode = mode;
    s.bInitOk = 1;
    s.bMasterVol = 0xff;
    s.dontStopSfxVoiceId = -1;
    s.bCallBackOn = 0;
    s.bReverbInit = 0;
    s.bSfxOn = 0;
    s.bReverbOn = 0;
    s.startOfGenericAddr = 0;
    s.endOfGenericAddr = 0;
    s.endOfAmbientAddr = 0;
    s.firstAmbientIndex = 0;
    s.lastAmbientIndex = 0;
    return 0;
}

int32 SfxInitFirst()
{
    if (guest_call<int32_t>(kSdRemoteInit) == -1 || sdRemote(kInit, 0) == -1)
        return -1;
    sdRemote(kSetCoreAttr, 0x800);
    sdRemote(kSetParam, 0x980, 0x3fff);
    sdRemote(kSetParam, 0xa80, 0x3fff);
    sdRemote(kSetParam, 0x981, 0x3fff);
    sdRemote(kSetParam, 0xa81, 0x3fff);
    return guest_call<int32_t>(kSifInitIopHeap) > -1 ? 0 : -1;
}

void SfxInitReverb()
{
    sfxClass &s = sfx();
    if (s.bInitOk == 0 || s.bReverbInit != 0)
        return;
    GuestFrame frame(0x40);
    sdRemote(kSetAddr, 0x1d00, 0x1dffff);
    sdRemote(kSetAddr, 0x1d01, 0x1fffff);
    s.bReverbDepth = 0;
    s.bReverbOn = 1;
    s.bReverbInit = 1;
    const ptr32<sceSdEffectAttr> attr = frame.local<sceSdEffectAttr>(0);
    reverbDefaults(*attr);
    reverbSetup(attr, 0x7fffff);
}

void SfxSetReverbDepth(uint8 bDepth)
{
    sfxClass &s = sfx();
    if (s.bInitOk == 0 || s.bReverbInit == 0)
        return;
    const uint8_t depth = static_cast<int8_t>(bDepth) > -1 ? bDepth : 0x7f;
    s.bReverbDepth = depth;
    if (s.bReverbOn == 0)
        return;
    const int32_t left = static_cast<int16_t>(depth << 8), right = static_cast<int16_t>(s.bReverbDepth << 8);
    sdRemote(kSetParam, 0xb80, left);
    sdRemote(kSetParam, 0xc80, right);
    sdRemote(kSetParam, 0xb81, left);
    sdRemote(kSetParam, 0xc81, right);
}

// Copies a VAG (its 0x30-byte header is big-endian) into the next free effect slot and SPU2 memory at
// SPUCurrentAddr. Returns the effect, -3 if all 140 are used, 0 if the IOP buffer or the copy to it failed, -1 if the
// transfer to SPU2 did.
int32 SfxTransEffect(ptr32<void> pVagPtr, ptr32<void> pImgPtr, uint32 resId)
{
    (void)pImgPtr;
    sfxClass &s = sfx();
    if (s.bInitOk == 0)
        return -1;
    const int32_t size = Big2Little(at<int32_t>(pVagPtr.address + 0xcu));
    const int32_t spuAddr = s.SPUCurrentAddr;
    int32_t id = 0;
    if (effect(0).dwSize != 0)
        for (id = 1;; ++id)
        {
            if (id == 0x8c)
                return -3;
            if (effect(id).dwSize == 0)
                break;
        }
    const int32_t rate = Big2Little(at<int32_t>(pVagPtr.address + 0x10u));
    EffectStruct &ef = effect(id);
    ef.dwAddr = spuAddr;
    ef.dwSize = size;
    ef.dwMulti = ee_div(static_cast<int32_t>(static_cast<uint32_t>(rate) << 12), 0xac44);
    ef.dwLoop = SfxDetermineLoop(pVagPtr, size);
    ef.dwResourceId = static_cast<int32_t>(resId);
    SfxGetLoopAddress(id, ptr32<char>(pVagPtr.address));
    s.SPUCurrentAddr += size;
    guest_call(kFlushCache, 0);
    const uint32_t iop = guest_call<uint32_t>(kSifAllocIopHeap, size);
    if (iop == 0)
        return 0;
    guest_call(kPrintf, kAllocatedFormat, iop);
    if (guest_call<int32_t>(kSdTransToIOP, pVagPtr.address + 0x30u, iop, size, 1) == -1)
        return 0;
    guest_call(kPrintf, kSendingFormat, size, spuAddr);
    if (sdRemote(kVoiceTrans, 1, 0, iop, spuAddr, size) == -1)
        return -1;
    sdRemote(kVoiceTransStatus, 1, 1);
    guest_call(kSifFreeIopHeap, iop);
    return id;
}

void SfxDoSfx()
{
    sfxClass &s = sfx();
    if (s.bInitOk == 0)
        return;
    GuestFrame frame(0x6b0);
    const uint32_t volBatch = frame.local<sceSdBatch>(0).address, pitchBatch = frame.local<sceSdBatch>(0x300).address;
    uint32_t core0Trigger = 0, core1Trigger = 0;
    const uint32_t core0Ended = static_cast<uint32_t>(sdRemote(kGetSwitch, 0x1700));
    const uint32_t core1Ended = static_cast<uint32_t>(sdRemote(kGetSwitch, 0x1701));

    // Key-ons due, ended samples, delays.
    for (int32_t v = 0; v < 0x30; ++v)
    {
        VoiceStruct &vc = voice(v);
        if (vc.wDelay == 1)
        {
            if (v < 0x18)
                core0Trigger += 1u << v;
            else
                core1Trigger += 1u << (v - 0x18);
        }
        if ((vc.wFlags & 1) != 0 && vc.wDelay == 0 &&
            ((v < 0x18 ? (1u << v) & core0Ended : (1u << (v - 0x18)) & core1Ended) != 0) && (vc.wFlags & 2) == 0)
        {
            const int32_t id = vc.wPlayId;
            vc.wFlags = 0;
            vc.wPlayId = -1;
            PlayStruct &p = play(id);
            if (p.wControlFlags == 0 || (p.wControlFlags & 2) != 0)
            {
                if ((p.wControlFlags & 4) != 0)
                {
                    const PlayStruct &chained = play(p.wChainId);
                    voice(chained.bVoice1).wFlags = static_cast<int16>(voice(chained.bVoice1).wFlags & ~4);
                    if (chained.bVoice2 >= 0)
                        voice(chained.bVoice2).wFlags = static_cast<int16>(voice(chained.bVoice2).wFlags & ~4);
                }
            }
            else if (s.mode != 2)
            {
                p.bVoice1 = -1;
                p.wControlFlags = 0;
            }
            else if ((p.wEffectFlags & 0x40) == 0)
            {
                p.bVoice2 = -1;
                p.wControlFlags = 0;
                p.bVoice1 = -1;
            }
            else
            {
                int8 first = p.bVoice1;
                if (first == v)
                {
                    p.bVoice1 = -1;
                    first = -1;
                }
                if (p.bVoice2 == v)
                    p.bVoice2 = -1;
                if (first == -1 && p.bVoice2 == -1)
                    p.wControlFlags = 0;
            }
        }
        if (vc.wDelay != 0 && (vc.wFlags & 4) == 0)
            vc.wDelay = static_cast<int16>(vc.wDelay - 1);
    }

    // Volumes: changed ones, all of them when the master volume changed, and the no-click fade.
    uint32_t batchCountVol = 0;
    for (int32_t v = 0; v < 0x30; ++v)
    {
        if (v == 0x17 || v == 0x2f)
            continue;
        VoiceStruct &vc = voice(v);
        if (vc.bVolChanged == 0 && s.masterVolChanged == 0 && vc.noClickCount == 0)
            continue;
        if (vc.noClickCount != 0)
        {
            vc.noClickCount = static_cast<uint8>(vc.noClickCount - 1);
            if (vc.noClickCount != 0)
            {
                const int32_t n = vc.noClickCount;
                vc.wLeftVol = static_cast<int16>(vc.wLeftVol - ee_div(vc.wLeftVol, n));
                vc.wRightVol = static_cast<int16>(vc.wRightVol - ee_div(vc.wRightVol, n));
            }
            else
            {
                vc.wLeftVol = 0;
                vc.wRightVol = 0;
                SfxStopVoice(static_cast<int16>(v));
            }
        }
        const int32_t left = vc.wLeftVol, right = vc.wRightVol;
        const uint32_t entry = voiceEntry(v);
        const uint32_t at0 = volBatch + batchCountVol * 8u;
        at<uint16_t>(at0) = 1;
        at<uint16_t>(at0 + 2u) = static_cast<uint16_t>(entry | 0x100u);
        int32_t rightValue = ee_div(mul(right, s.bMasterVol), 0xff);
        if (right < 0)
            rightValue = static_cast<int16_t>(rightValue + 0x7fff);
        at<int32_t>(at0 + 4u) = rightValue;
        G(volR) = static_cast<int16>(rightValue);
        at<uint16_t>(at0 + 8u) = 1;
        at<uint16_t>(at0 + 10u) = static_cast<uint16_t>(entry);
        at<int32_t>(at0 + 12u) = ee_div(mul(left, s.bMasterVol), 0xff);
        batchCountVol += 2;
        vc.bVolChanged = 0;
        G(volL) = static_cast<int16>(ee_div(mul(left, s.bMasterVol), 0xff));
    }
    if (s.masterVolChanged != 0)
        s.masterVolChanged = 0;

    // Pitches.
    uint32_t batchCountPitch = 0;
    for (int32_t v = 0; v < 0x30; ++v)
    {
        VoiceStruct &vc = voice(v);
        if (vc.bPitchChanged == 0)
            continue;
        const int32_t multiplier = vc.wPlayId == -1 ? 0x800 : play(vc.wPlayId).dwMulti;
        const uint32_t at0 = pitchBatch + batchCountPitch * 8u;
        const int32_t product = mul(vc.wPitch, multiplier);
        at<uint16_t>(at0) = 1;
        at<uint16_t>(at0 + 2u) = static_cast<uint16_t>(voiceEntry(v) | 0x200u);
        ++batchCountPitch;
        vc.bPitchChanged = 0;
        at<int32_t>(at0 + 4u) = (product < 0 ? product + 0xff : product) >> 8;
    }

    if (batchCountVol != 0 || batchCountPitch != 0)
    {
        guest_call(kFlushCache, 0);
        if (batchCountVol != 0)
            guest_call(kSdTransToIOP, volBatch, G(IOPBatchBuffer), batchCountVol << 3, 1);
        G(volL) = static_cast<int16>(batchCountVol);
        if (batchCountPitch != 0)
        {
            guest_call(kFlushCache, 0);
            guest_call(kSdTransToIOP, pitchBatch, G(IOPBatchBuffer) + batchCountVol * 8u, batchCountPitch << 3, 1);
        }
        G(volR) = static_cast<int16>(batchCountPitch);
        sdRemote(kProcBatch, G(IOPBatchBuffer), 0, batchCountVol + batchCountPitch);
    }
    if (core0Trigger != 0)
        sdRemote(kSetSwitch, 0x1500, core0Trigger);
    if (core1Trigger != 0)
        sdRemote(kSetSwitch, 0x1501, core1Trigger);
}

int16 SfxPlay(int16 wSampleId, uint8 bVolume, uint8 bLRPan, uint8 bFBPan, int16 wPitch, int16 wFlags, int16 chainId)
{
    sfxClass &s = sfx();
    if (s.bInitOk == 0 || s.bSfxOn == 0)
        return -1;
    const int16 id = GetFreePlayId();
    if (id == -1)
        return -1;
    const EffectStruct &ef = effect(wSampleId);
    const int32_t multiplier = ef.dwMulti;
    PlayStruct &p = play(id);
    p.wControlFlags = ef.dwLoop != 0 ? 3 : 1;
    p.wEffectFlags = wFlags;
    p.bLRPan = bLRPan;
    p.bFBPan = bFBPan;
    p.wPitch = wPitch;
    p.bVol = static_cast<uint8>(ee_div(bVolume * s.bMasterVol, 0xff));
    p.dwMulti = multiplier;
    if (chainId != 0)
    {
        PlayStruct &chained = play(chainId);
        chained.wChainId = id;
        chained.wControlFlags = static_cast<int16>(chained.wControlFlags | 4);
    }
    const int32_t product = mul(wPitch, multiplier);
    const int16 pitch = static_cast<int16>((product < 0 ? product + 0xff : product) >> 8);
    const int16 first = GetFreeVoice();
    if (first == -1)
        return -2;
    p.bVoice1 = static_cast<int8>(first);
    VoiceStruct &v1 = voice(first);
    v1.bStopNow = 0;
    v1.dwStoreAddress = ef.dwAddr;
    v1.wFlags = p.wControlFlags;
    v1.wDelay = 1;
    v1.dwStoreLoopAddress = ef.dwLoopAddr;
    v1.wPitch = pitch;
    v1.wPlayId = id;
    sdRemote(kSetAddr, voiceEntry(first) | 0x2040u, ef.dwAddr);
    sdRemote(kSetParam, voiceEntry(first) | 0x200u, v1.wPitch);
    if (s.mode == 2 && (wFlags & 0x40) != 0)
    {
        v1.wDelay = 1;
        const int16 second = GetFreeVoice();
        p.bVoice2 = static_cast<int8>(second);
        if (second != -1)
        {
            VoiceStruct &v2 = voice(second);
            v2.bStopNow = 0;
            v2.wFlags = p.wControlFlags;
            v2.dwStoreAddress = ef.dwAddr;
            v2.wDelay = 2;
            v2.wPitch = pitch;
            v2.wPlayId = id;
            v2.dwStoreLoopAddress = ef.dwLoopAddr;
            sdRemote(kSetAddr, voiceEntry(second) | 0x2040u, ef.dwAddr);
            sdRemote(kSetParam, voiceEntry(second) | 0x200u, v2.wPitch);
        }
    }
    else
    {
        p.bVoice2 = -1;
    }
    SetEffectVolume(id);
    return id;
}

void SfxStop(int16 wPlayId)
{
    sfxClass &s = sfx();
    if (s.bInitOk == 0 || wPlayId == -1 || static_cast<uint16_t>(wPlayId) >= 0x30u)
        return;
    PlayStruct &p = play(wPlayId);
    const int8 first = p.bVoice1;
    if ((p.wEffectFlags & 0x40) == 0 || first != -1)
    {
        VoiceStruct &vc = voice(first);
        if (vc.wPlayId == wPlayId && vc.noClickCount == 0)
        {
            vc.noClickCount = 5;
            p.bVoice1 = -1;
            vc.wPlayId = -1;
        }
    }
    const int8 second = p.bVoice2;
    if (((p.wEffectFlags & 0x40) == 0 || second != -1) && second >= 0)
    {
        VoiceStruct &vc = voice(second);
        if (vc.wPlayId == wPlayId && vc.noClickCount == 0)
        {
            vc.noClickCount = 5;
            p.bVoice2 = -1;
            vc.wPlayId = -1;
        }
    }
    if (s.mode == 2)
    {
        if ((p.wEffectFlags & 0x40) == 0 || (p.bVoice1 == -1 && p.bVoice2 == -1))
            p.wControlFlags = 0;
    }
    else
    {
        p.wControlFlags = 0;
    }
}

void SetEffectVolume(int16 wPlayId)
{
    const int32_t mode = sfx().mode;
    if (mode == 1)
        SetStereoVolume(wPlayId);
    else if (mode == 0)
        SetMonoVolume(wPlayId);
    else if (mode == 2)
        SetDolbyVolume(wPlayId);
}

namespace {

// Copies a playing voice's pitch and addresses to a new voice of the same play and starts it where the other has got
// to (the front voice 0x36 past it, the rear voice 0x36 before it but not before the sample's start), with its volume
// at 0 until SfxDoSfx sets it.
void startPartner(int16 wPlayId, int32_t fresh, int32_t partner, bool behind)
{
    PlayStruct &p = play(wPlayId);
    VoiceStruct &vc = voice(fresh);
    vc.wFlags = p.wControlFlags;
    vc.wDelay = 0;
    const int16 pitch = voice(partner).wPitch;
    vc.wPlayId = wPlayId;
    vc.wPitch = pitch;
    vc.dwStoreAddress = voice(partner).dwStoreAddress;
    vc.dwStoreLoopAddress = voice(partner).dwStoreLoopAddress;
    vc.bStopNow = 0;
    // The original asks for the partner's address with the new voice's core and the partner's unreduced index.
    const int32_t now = sdRemote(kGetAddr, voiceCore(fresh) | (static_cast<uint32_t>(partner) << 1) | 0x2240u);
    const uint32_t entry = voiceEntry(fresh);
    const int32_t ahead = static_cast<int32_t>(static_cast<uint32_t>(now) + 0x36u);
    const int32_t back = static_cast<int32_t>(static_cast<uint32_t>(now) - 0x36u);
    const int32_t start = !behind ? ahead : vc.dwStoreAddress <= back ? back : vc.dwStoreAddress;
    sdRemote(kSetAddr, entry | 0x2040u, start);
    sdRemote(kSetParam, entry, 0);
    sdRemote(kSetParam, entry | 0x100u, 0);
    sdRemote(kSetSwitch, voiceCore(fresh) | 0x1500u, voiceBit(fresh));
    sdRemote(kSetAddr, entry | 0x2140u, voice(fresh).dwStoreLoopAddress);
    sdRemote(kSetParam, entry | 0x200u, voice(fresh).wPitch);
}

// Stops one voice of a Dolby sound (it is keyed off at once unless already stopping) and marks it stopping.
void stopDolbyVoice(int8 &slot)
{
    const int32_t v = slot;
    VoiceStruct &vc = voice(v);
    if (vc.bStopNow == 0)
    {
        clearVoice(vc);
        keyOffVoice(v);
        slot = -1;
    }
    voice(v).bStopNow = 1;
}

} // namespace

void SetDolbyVolume(int16 wPlayId)
{
    PlayStruct &p = play(wPlayId);
    const uint32_t lr = p.bLRPan, fb = p.bFBPan;
    const bool dolby = (p.wEffectFlags & 0x40) != 0;
    int32_t front = p.bVol << 6, rear = front;
    if (dolby)
    {
        if (fb < 0xf)
            front = static_cast<int16_t>(ee_div(mul(front, static_cast<int32_t>(fb)), 0xf));
        else if (fb > 0xf)
            rear = static_cast<int16_t>(ee_div(mul(rear, 0x1f - static_cast<int32_t>(fb)), 0xf));
    }
    const int32_t frontLeft = mul(front, at<uint8_t>(kStereoLookup + lr)) >> 8;
    const int32_t frontRight = mul(front, at<uint8_t>(kStereoLookup + 0x1eu - lr)) >> 8;
    const int32_t rearLeft = mul(rear, at<uint8_t>(kRearLookup + lr)) >> 8;
    const int32_t rearRight = mul(rear, at<uint8_t>(kRearLookup + 0x1eu - lr)) >> 8;
    const int8 first = p.bVoice1;
    if (voice(first == -1 ? p.bVoice2 : first).wPlayId == wPlayId)
    {
        if (!dolby)
        {
            VoiceStruct &vc = voice(first);
            vc.bVolChanged = 1;
            vc.wLeftVol = static_cast<int16>(frontLeft);
            vc.wRightVol = static_cast<int16>(frontRight);
        }
        else if (fb == 0)
        {
            if (first >= 0)
                stopDolbyVoice(p.bVoice1);
        }
        else if (p.bVoice1 != -1)
        {
            VoiceStruct &vc = voice(first);
            vc.bVolChanged = 1;
            vc.wLeftVol = static_cast<int16>(frontLeft);
            vc.wRightVol = static_cast<int16>(frontRight);
        }
        else
        {
            const int16 fresh = GetFreeVoice();
            if (fresh != -1)
            {
                p.bVoice1 = static_cast<int8>(fresh);
                startPartner(wPlayId, fresh, p.bVoice2, false);
            }
        }
    }
    if (!dolby)
        return;
    if (fb >= 0x1e)
    {
        if (p.bVoice2 >= 0)
            stopDolbyVoice(p.bVoice2);
        return;
    }
    const int8 second = p.bVoice2;
    if (second != -1)
    {
        const int32_t sum = rearLeft + rearRight;
        const int32_t average = (sum + static_cast<int32_t>(static_cast<uint32_t>(sum) >> 31)) >> 1;
        VoiceStruct &vc = voice(second);
        if (vc.wPlayId == wPlayId)
        {
            vc.wLeftVol = static_cast<int16>(average);
            vc.bVolChanged = 1;
            vc.wRightVol = static_cast<int16>(-average);
        }
        return;
    }
    const int16 fresh = GetFreeVoice();
    if (fresh == -1)
        return;
    p.bVoice2 = static_cast<int8>(fresh);
    startPartner(wPlayId, fresh, p.bVoice1, true);
}

void VAGConstructDaemon(ptr32<memHandle> pHandle)
{
    const ptr32<void> p = ResReadAlloc();
    const int32_t id = SfxTransEffect(ptr32<void>(at<uint32_t>(pHandle.address)), nullptr, at<uint32_t>(0x31e120u));
    at<uint32_t>(pHandle.address) = static_cast<uint32_t>(id) << 2;
    ResReAlloc(p);
}

// 0 if the 15 bytes after the VAG's data (at 0x21 past its size: the last block's flags and samples) are those of a
// sample that does not loop, either end type; 1 if it loops.
int32 SfxDetermineLoop(ptr32<void> pVagPtr, int32 dwSize)
{
    uint8_t type1[16], type2[16], tail[15];
    for (int32_t i = 0; i < 16; ++i)
    {
        type1[i] = i == 0 ? 7 : 0;
        type2[i] = i == 0 ? 0 : 0x77;
    }
    const uint32_t from = pVagPtr.address + static_cast<uint32_t>(dwSize) + 0x21u;
    for (uint32_t i = 0; i < 15; ++i)
        tail[i] = at<uint8_t>(from + i);
    bool isType1 = true, isType2 = true;
    for (int32_t i = 0; i < 15; ++i)
    {
        isType1 = isType1 && tail[i] == type1[i];
        isType2 = isType2 && tail[i] == type2[i];
    }
    return isType1 || isType2 ? 0 : 1;
}

void SfxStopNow(int16 wPlayId)
{
    sfxClass &s = sfx();
    if (s.bInitOk == 0 || wPlayId == -1 || static_cast<uint16_t>(wPlayId) >= 0x30u)
        return;
    PlayStruct &p = play(wPlayId);
    const int8 first = p.bVoice1;
    if (first >= 0)
    {
        VoiceStruct &vc = voice(first);
        if (vc.dwStoreAddress == 0xc2310 || vc.dwStoreAddress == 0xc37d0)
        {
            s.dontStopSfxVoiceId = first; // these two samples are left playing (SfxKillAllChannels spares the voice)
            return;
        }
        if (vc.wPlayId == wPlayId)
        {
            keyOffVoice(first);
            clearVoice(vc);
            vc.bStopNow = 0;
        }
    }
    const int8 second = p.bVoice2;
    if (second >= 0)
    {
        VoiceStruct &vc = voice(second);
        if (vc.wPlayId == wPlayId)
        {
            keyOffVoice(second);
            clearVoice(vc);
            vc.bStopNow = 0;
        }
    }
    p.bVoice2 = -1;
    p.wControlFlags = 0;
    p.bVoice1 = -1;
}

void SfxKillAllChannels()
{
    sfxClass &s = sfx();
    const int32_t spared = s.dontStopSfxVoiceId;
    if (spared == -1)
    {
        sdRemote(kSetSwitch, 0x1600, 0x7fffff);
        sdRemote(kSetSwitch, 0x1601, 0x7fffff);
    }
    else if (spared < 0x18)
    {
        sdRemote(kSetSwitch, 0x1601, 0x7fffff);
        sdRemote(kSetSwitch, 0x1600, ~(1u << (s.dontStopSfxVoiceId & 31)) & 0x7fffffu);
    }
    else
    {
        // The core-1 voice's bit is not reduced by 24, so its own core is keyed off whole.
        sdRemote(kSetSwitch, 0x1600, 0x7fffff);
        sdRemote(kSetSwitch, 0x1601, ~(1u << (s.dontStopSfxVoiceId & 31)) & 0x7fffffu);
    }
    s.dontStopSfxVoiceId = -1;
}

void SfxReInitReverb()
{
    GuestFrame frame(0x30);
    const ptr32<sceSdEffectAttr> attr = frame.local<sceSdEffectAttr>(0);
    reverbDefaults(*attr);
    reverbSetup(attr, 0xffffff);
}

void SfxUnInitReverb() {}
void SfxReverbOn() {}
void SfxReverbOff() {}

void SfxSetVolume(uint8 bVolume)
{
    sfxClass &s = sfx();
    if (s.bInitOk == 0)
        return;
    s.bMasterVol = bVolume;
    s.masterVolChanged = 1;
}

uint8 SfxEffectGetVol(int16 wPlayId)
{
    if (sfx().bInitOk == 0 || wPlayId == -1 || static_cast<uint16_t>(wPlayId) >= 0x30u)
        return 0;
    const PlayStruct &p = play(wPlayId);
    return (p.wControlFlags & 2) != 0 ? p.bVol : 0;
}

void SfxSetMode(int32 mode)
{
    if (sfx().bInitOk != 0)
        sfx().mode = mode;
}

void SfxSetCallBack(ee_fnptr CallBack)
{
    if (sfx().bInitOk == 0)
        return;
    G(DoCallBack) = CallBack;
    sfx().bCallBackOn = 1;
}

void SfxUnInit() {}

void SfxOn()
{
    if (sfx().bInitOk != 0)
        sfx().bSfxOn = 1;
}

void SfxOff()
{
    sfxClass &s = sfx();
    if (s.bInitOk == 0)
        return;
    sdRemote(kSetSwitch, 0x1600, 0xffffff);
    for (int32_t v = 0; v < 0x30; ++v)
    {
        VoiceStruct &vc = voice(v);
        vc.wFlags = 0;
        vc.wDelay = 0;
        vc.noClickCount = 0;
        vc.wPlayId = -1;
        PlayStruct &p = play(v);
        p.wControlFlags = 0;
        p.bVoice1 = -1;
        p.bVoice2 = -1;
    }
    s.bSfxOn = 0;
}

// Saves every voice's volumes (RevSfxPauseFade's fade back in starts from them); with flags 1 also silences them.
void SfxPause(int32 flags)
{
    sfxClass &s = sfx();
    for (int32_t v = 0; v < 0x30; ++v)
    {
        s.pwVolumeStore[2 * v] = voice(v).wLeftVol;
        s.pwVolumeStore[2 * v + 1] = voice(v).wRightVol;
        if (flags == 1)
        {
            sdRemote(kSetParam, voiceEntry(v), 0);
            sdRemote(kSetParam, voiceEntry(v) | 0x100u, 0);
        }
    }
}

void SfxContinue() {}

int16 SfxFreeEffect(int32 dwSampleId)
{
    (void)dwSampleId;
    return 1;
}

void SfxEffectLRPan(int16 wPlayId, uint8 bLRPan)
{
    if (!playing(wPlayId) || bLRPan == play(wPlayId).bLRPan)
        return;
    play(wPlayId).bLRPan = bLRPan;
    SetEffectVolume(wPlayId);
}

void SfxEffectFBPan(int16 wPlayId, uint8 bFBPan)
{
    if (!playing(wPlayId) || bFBPan == play(wPlayId).bFBPan)
        return;
    play(wPlayId).bFBPan = bFBPan;
    SetEffectVolume(wPlayId);
}

void SfxEffectVol(int16 wPlayId, uint8 bVolume)
{
    if (!playing(wPlayId) || bVolume == play(wPlayId).bVol)
        return;
    play(wPlayId).bVol = bVolume;
    SetEffectVolume(wPlayId);
}

void SfxEffectPitch(int16 wPlayId, int16 wPitch)
{
    if (!playing(wPlayId) || wPitch == play(wPlayId).wPitch)
        return;
    PlayStruct &p = play(wPlayId);
    p.wPitch = wPitch;
    const int8 first = p.bVoice1;
    if (first != -1 && voice(first).wPlayId == wPlayId)
    {
        voice(first).wPitch = wPitch;
        voice(first).bPitchChanged = 1;
    }
    const int8 second = p.bVoice2;
    if (second != -1 && voice(second).wPlayId == wPlayId)
    {
        voice(second).wPitch = wPitch;
        voice(second).bPitchChanged = 1;
    }
}

// Records where the loaded VAGs end: type 0 (the level's generic sounds, loaded first) also sets where they start and
// the 4-aligned end of the generic area.
void SfxStoreAddress(int32 noVags, int32 type)
{
    sfxClass &s = sfx();
    const int16 last = static_cast<int16>(noVags - 1);
    if (type == 0)
    {
        const int32_t start = effect(0).dwAddr;
        s.lastAmbientIndex = last;
        const int32_t end = effect(noVags - 1).dwAddr + effect(noVags - 1).dwSize;
        s.startOfGenericAddr = start;
        s.firstAmbientIndex = last;
        s.endOfAmbientAddr = end;
        s.endOfGenericAddr = static_cast<int32_t>(static_cast<uint32_t>(end + 3) & 0xfffffffcu);
        return;
    }
    s.lastAmbientIndex = last;
    s.endOfAmbientAddr = effect(noVags - 1).dwAddr + effect(noVags - 1).dwSize;
}

// Forgets the ambient VAGs from effect no on (down from the last one) and puts the SPU2 allocation back to the end
// of the generic area.
void SfxClearSram(int32 no)
{
    sfxClass &s = sfx();
    int32_t e = s.lastAmbientIndex;
    s.firstAmbientIndex = static_cast<int16>(no);
    for (; e >= static_cast<int16_t>(no); --e)
    {
        EffectStruct &ef = effect(e);
        ef.dwSize = 0;
        ef.dwImgAddr = 0;
        ef.dwImgSize = 0;
        ef.dwLoop = 0;
    }
    s.SPUCurrentAddr = s.endOfGenericAddr;
    s.endOfAmbientAddr = s.endOfGenericAddr;
}

void SfxDumpSram(ptr32<uint8> addr) { (void)addr; }
void SfxCdInit() {}
void SfxSetCdVol(uint8 vol) { (void)vol; }
void SfxCdSetReverb(int32 depth, int32 onoff)
{
    (void)depth;
    (void)onoff;
}
void SFX_TVAG() {}

void SetMonoVolume(int16 wPlayId)
{
    const PlayStruct &p = play(wPlayId);
    VoiceStruct &vc = voice(p.bVoice1);
    const int16 volume = static_cast<int16>(p.bVol << 6);
    if (vc.wPlayId != wPlayId)
        return;
    vc.wRightVol = volume;
    vc.bVolChanged = 1;
    vc.wLeftVol = volume;
}

void SetStereoVolume(int16 wPlayId)
{
    const PlayStruct &p = play(wPlayId);
    VoiceStruct &vc = voice(p.bVoice1);
    const uint32_t volume = static_cast<uint32_t>(p.bVol) << 6;
    const uint32_t left = (volume * at<uint8_t>(kStereoLookup + p.bLRPan)) >> 8;
    const uint32_t right = (volume * at<uint8_t>(kStereoLookup + 0x1eu - p.bLRPan)) >> 8;
    if (vc.wPlayId != wPlayId)
        return;
    vc.wLeftVol = static_cast<int16>(left);
    vc.bVolChanged = 1;
    vc.wRightVol = static_cast<int16>(right);
}

int32 Big2Little(int32 dwNum)
{
    const uint32_t n = static_cast<uint32_t>(dwNum);
    return static_cast<int32_t>((n >> 24) | (n << 24) | ((n & 0xff00u) << 8) | ((n >> 8) & 0xff00u));
}

int16 GetFreePlayId()
{
    for (int32_t p = 0; p < 0x30; ++p)
        if ((play(p).wControlFlags & 1) == 0)
            return static_cast<int16>(p);
    return -1;
}

// The first free voice, skipping 23 and 47 (the last of each core), or -1.
int16 GetFreeVoice()
{
    for (int32_t v = 0; v < 0x30;)
    {
        if ((voice(v).wFlags & 1) == 0)
            return static_cast<int16>(v);
        ++v;
        if (v == 0x17 || v == 0x2f)
            ++v;
    }
    return -1;
}

void SfxStopVoice(int16 wVoiceId)
{
    if (sfx().bInitOk == 0 || wVoiceId < 0 || wVoiceId >= 0x30)
        return;
    keyOffVoice(wVoiceId);
    VoiceStruct &vc = voice(wVoiceId);
    vc.wPlayId = -1;
    vc.wFlags = 0;
    vc.wDelay = 0;
    vc.noClickCount = 0;
    vc.bVolChanged = 0;
    vc.bStopNow = 0;
}

// The loop start: the first 16-byte block of the VAG's data (after its header) with the loop-start flag (4 in the
// block's second byte), as an SPU2 address, if the sample loops; else 0.
void SfxGetLoopAddress(int32 wSampleId, ptr32<char> pMem)
{
    EffectStruct &ef = effect(wSampleId);
    const uint32_t data = pMem.address + 0x30u;
    bool found = false;
    int32_t offset = 0;
    if (ef.dwSize > 0)
    {
        if ((at<uint8_t>(data + 1u) & 4) != 0)
            found = true;
        else
            for (offset = 0x10; offset < ef.dwSize; offset += 0x10)
                if ((at<uint8_t>(data + static_cast<uint32_t>(offset) + 1u) & 4) != 0)
                {
                    found = true;
                    break;
                }
    }
    if (!found || ef.dwLoop == 0)
    {
        ef.dwLoopAddr = 0;
        return;
    }
    ef.dwLoopAddr = ef.dwAddr + offset;
}

} // namespace rc

// The IOP calls (sceSdRemote, the IOP heap, the transfers) are logged by the oracle's setups; a live comparison would
// send each command twice, so those functions are compared offline only.
RC_DECOMP_FN_BLOCKING_SETUP_HEAP(SfxInit, 0x1ac1b8, rc::fuzz::sfxInit)
RC_DECOMP_FN_BLOCKING_SETUP(SfxInitFirst, 0x1ac408, rc::fuzz::sfxIop)
RC_DECOMP_FN_BLOCKING_SETUP(SfxInitReverb, 0x1ac4c0, rc::fuzz::sfxReverb)
RC_DECOMP_FN_BLOCKING_SETUP(SfxSetReverbDepth, 0x1ac6a8, rc::fuzz::sfxReverbDepth)
RC_DECOMP_FN_BLOCKING_SETUP(SfxTransEffect, 0x1ac788, rc::fuzz::sfxTransEffect)
RC_DECOMP_FN_BLOCKING_SETUP(SfxDoSfx, 0x1ac9e0, rc::fuzz::sfxVoices)
RC_DECOMP_FN_BLOCKING_SETUP(SfxPlay, 0x1ad0a0, rc::fuzz::sfxPlay)
RC_DECOMP_FN_SETUP(SfxStop, 0x1ad448, rc::fuzz::sfxPlayId)
RC_DECOMP_FN_BLOCKING_SETUP(SetEffectVolume, 0x1ad590, rc::fuzz::sfxPlayId)
RC_DECOMP_FN_BLOCKING_SETUP(SetDolbyVolume, 0x1ad5f0, rc::fuzz::sfxPlayId)
RC_DECOMP_FN_BLOCKING_SETUP(VAGConstructDaemon, 0x1adc70, rc::fuzz::sfxVagDaemon)
RC_DECOMP_FN_SETUP(SfxDetermineLoop, 0x1adcc0, rc::fuzz::sfxVagTail)
RC_DECOMP_FN_BLOCKING_SETUP(SfxStopNow, 0x1adff0, rc::fuzz::sfxPlayId)
RC_DECOMP_FN_BLOCKING_SETUP(SfxKillAllChannels, 0x1ae188, rc::fuzz::sfxVoices)
RC_DECOMP_FN_BLOCKING_SETUP(SfxReInitReverb, 0x1ae2b0, rc::fuzz::sfxIop)
RC_DECOMP_FN(SfxUnInitReverb, 0x1ae430)
RC_DECOMP_FN(SfxReverbOn, 0x1ae438)
RC_DECOMP_FN(SfxReverbOff, 0x1ae440)
RC_DECOMP_FN_SETUP(SfxSetVolume, 0x1ae448, rc::fuzz::sfxByte)
RC_DECOMP_FN_SETUP(SfxEffectGetVol, 0x1ae470, rc::fuzz::sfxPlayId)
RC_DECOMP_FN_SETUP(SfxSetMode, 0x1ae4d0, rc::fuzz::sfxByte)
RC_DECOMP_FN_SETUP(SfxSetCallBack, 0x1ae4f0, rc::fuzz::sfxByte)
RC_DECOMP_FN(SfxUnInit, 0x1ae518)
RC_DECOMP_FN_SETUP(SfxOn, 0x1ae520, rc::fuzz::sfxByte)
RC_DECOMP_FN_BLOCKING_SETUP(SfxOff, 0x1ae540, rc::fuzz::sfxVoices)
RC_DECOMP_FN_BLOCKING_SETUP(SfxPause, 0x1ae680, rc::fuzz::sfxPause)
RC_DECOMP_FN(SfxContinue, 0x1ae7e8)
RC_DECOMP_FN(SfxFreeEffect, 0x1ae7f0)
RC_DECOMP_FN_BLOCKING_SETUP(SfxEffectLRPan, 0x1ae7f8, rc::fuzz::sfxPlayIdByte)
RC_DECOMP_FN_BLOCKING_SETUP(SfxEffectFBPan, 0x1ae870, rc::fuzz::sfxPlayIdByte)
RC_DECOMP_FN_BLOCKING_SETUP(SfxEffectVol, 0x1ae8e8, rc::fuzz::sfxPlayIdByte)
RC_DECOMP_FN_SETUP(SfxEffectPitch, 0x1ae960, rc::fuzz::sfxPlayIdByte)
RC_DECOMP_FN_SETUP(SfxStoreAddress, 0x1aea30, rc::fuzz::sfxStoreAddress)
RC_DECOMP_FN_SETUP(SfxClearSram, 0x1aeab0, rc::fuzz::sfxClearSram)
RC_DECOMP_FN(SfxDumpSram, 0x1aeb30)
RC_DECOMP_FN(SfxCdInit, 0x1aeb38)
RC_DECOMP_FN(SfxSetCdVol, 0x1aeb40)
RC_DECOMP_FN(SfxCdSetReverb, 0x1aeb48)
RC_DECOMP_FN(SFX_TVAG, 0x1aeb50)
RC_DECOMP_FN_SETUP(SetMonoVolume, 0x1aeb58, rc::fuzz::sfxPlayId)
RC_DECOMP_FN_SETUP(SetStereoVolume, 0x1aebb0, rc::fuzz::sfxPlayId)
RC_DECOMP_FN(Big2Little, 0x1aec38)
RC_DECOMP_FN_SETUP(GetFreePlayId, 0x1aec60, rc::fuzz::sfxVoices)
RC_DECOMP_FN_SETUP(GetFreeVoice, 0x1aed18, rc::fuzz::sfxVoices)
RC_DECOMP_FN_BLOCKING_SETUP(SfxStopVoice, 0x1aed90, rc::fuzz::sfxVoiceId)
RC_DECOMP_FN_SETUP(SfxGetLoopAddress, 0x1aee38, rc::fuzz::sfxLoopAddress)

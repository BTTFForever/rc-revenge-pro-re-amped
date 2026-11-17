// Decompiled from RCMUSIC.c and MUSIC.c: the music, streamed from XA2 files on the disc by the IOP's music module.
//
// MUSIC.c is the RPC client (server 0x12345): every command is one sceSifCallRpc with the command number and a data
// word in sbuf (or, for 0x802x, 0x40 bytes the data word points to), and the module's first reply word comes back in
// sbuf[0]. Commands: 0x50 play channel, 0x8070 stop, 0x8040 terminate, 0x8100 select file (its sector), 0x80e0 stream
// mode, 0xc0 size, 0xd0 position (sector + size / 2048), 0x90 switch (ratio << 20 + position, or the position), 0x7ff0
// switch at once (front end), 0x8060 channel, 0xa0 pause (0 pause, 1 and 2 resume), 0xb0 volume, 0x8000 init.
//
// RCMUSIC.c keeps the state in rcmusicVarG: 36 tracks (fileList names, fileListCD sectors and fileSizes from
// sceCdSearchFile, specialSizeRatio for the switch), the track per level (levels: a level's own track, the "final lap"
// track is the next one), cdState (0 off, 1 playing, 2 paused), a fade counter (positive fading out by volstore per
// frame, negative fading in), and a chain: RCMusicChain fades out (mode 1), then switches track (mode 2), fading the
// front-end track back in (mode 3). RCMusicFadeControl runs the fades once a frame.
//
// Never called by the game (no jal, no address taken, no data pointer): RCMusicInitStream, RCMusicLvlTerm, the empty
// RCMusicTest, MusicStart, MusicTerm, MusicSwitchSpecial and MusicInitStream. RCMusicPlayTrackEdit and
// RCMusicPlayLevel pass their caller's $a1 (and $a2) on as the track (and loop) for arguments outside their tables:
// which >= 7, and level 0x61.

#include "rc_decomp.h"
#include "stream_fuzz.h"

namespace rc {

uint16 RandomGetNDRange(uint16 lower, uint16 upper); // math_core.cpp

void RCMusicSetLevel(int32 fileCt);
void RCMusicSetVol(int32 flags);
void RCMusicResume(int32 fade);
void RCMusicChain(int32 secondXA, int8 fadeRate);
void MusicPlay(ptr32<char> file, int32 channel, int32 loop);
void MusicSwitch(int32 pos, int32 ratio, int32 size);
void MusicSwitchFront(int32 pos, int32 ratio, int32 size);
void MusicSwitchChannel(int32 channel);
void MusicPause(int32 flags);
void MusicStop();
void MusicInitStream(int32 mode);
int32 MusicInit();
int32 MusicRPCinit();
int32 MusicRPC(int32 command, int32 data);

namespace {

constexpr uint32_t kRc = 0x1ee8c0u; // rcmusicVarG
constexpr uint32_t kCd = 0x4bf5c0u, kSbuf = 0x4bf580u, kFrontFade = 0x327f68u;
constexpr uint32_t kSifCallRpc = 0x1c0968u, kSifInitRpc = 0x1c0180u, kSifBindRpc = 0x1c07b8u, kPrintf = 0x12e9e8u,
                   kCdDiskReady = 0x1cd708u, kCdSearchFile = 0x1cc110u;
// "SETVOL request for %d\n", "MODIFY VOLUME \n", "RCMUSIC SET VOL %d", "Error locating %s \n",
// "error: sceSifBindRpc \n".
constexpr uint32_t kSetVolFormat = 0x3241f8u, kModifyFormat = 0x324228u, kRcSetVolFormat = 0x324238u,
                   kLocateFormat = 0x323ea0u, kBindError = 0x325178u;
// The track names, "\\MUS1\\FRONTEND.XA2;1" to "\\MUS2\\PWBFL.XA2;1".
constexpr uint32_t kTrackNames[36] = {
    0x323eb8u, 0x323ed0u, 0x323ee8u, 0x323f00u, 0x323f18u, 0x323f30u, 0x323f48u, 0x323f60u, 0x323f78u,
    0x323f88u, 0x323fa0u, 0x323fb0u, 0x323fc8u, 0x323fd8u, 0x323ff0u, 0x324000u, 0x324018u, 0x324028u,
    0x324040u, 0x324050u, 0x324068u, 0x324080u, 0x324098u, 0x3240a8u, 0x3240c0u, 0x3240d0u, 0x3240e8u,
    0x3240f8u, 0x324110u, 0x324120u, 0x324138u, 0x324150u, 0x324168u, 0x324178u, 0x324190u, 0x3241a0u,
};

template <typename T>
T load(uint32_t address)
{
    T value;
    std::memcpy(&value, guest_host(address), sizeof(T));
    return value;
}

template <typename T>
void store(uint32_t address, T value)
{
    std::memcpy(guest_host(address), &value, sizeof(T));
}

rcmusicClass &music() { return G(rcmusicVarG); }

// Per-track arrays by index as the original addresses them (any index, no bounds).
int32_t trackSector(int32_t i) { return load<int32_t>(kRc + 0x1e8u + (static_cast<uint32_t>(i) << 2)); }
int32_t trackSize(int32_t i) { return load<int32_t>(kRc + 0xbcu + (static_cast<uint32_t>(i) << 2)); }
int32_t trackRatio(int32_t i) { return load<int32_t>(kRc + 0x2cu + (static_cast<uint32_t>(i) << 2)); }
uint8_t levelTrack(int32_t level) { return load<uint8_t>(kRc + 0x277u + static_cast<uint32_t>(level)); } // levels[level - 1]

uint32_t callerRegister(int reg) { return getRegU32(&g_runtime->eeScheduler().runningContext(), reg); }

void setVolume(int32_t vol)
{
    guest_call(kPrintf, kSetVolFormat, vol);
}

// Sector + size / 2048 (rounded toward zero, as the original's shift does for negative sizes).
int32_t sectorAfter(int32_t pos, int32_t size)
{
    const int32_t rounded = size > -1 ? size : static_cast<int32_t>(static_cast<uint32_t>(size) + 0x7ffu);
    return static_cast<int32_t>(static_cast<uint32_t>(pos) + static_cast<uint32_t>(rounded >> 11));
}

} // namespace

// --- RCMUSIC.c ---

// The track names, the switch ratios (0x213 for the odd tracks from 9 and the last, else 0x200) and each level's track.
void RCMusicInitLists()
{
    rcmusicClass &r = music();
    for (int i = 0; i < 36; ++i)
        r.fileList[i] = ptr32<char>(kTrackNames[i]);
    for (int i = 0; i < 36; ++i)
        r.specialSizeRatio[i] = 0x200;
    r.specialSizeRatio[35] = 0x213;
    for (int i = 9; i <= 33; i += 2)
        r.specialSizeRatio[i] = 0x213;
    for (int32_t level = 0; level < 0x60; ++level)
        RCMusicSetLevel(level);
    r.levels[0x62] = 0;
    r.levels[0x63] = 0xff;
    r.levels[0x61] = 0xff;
}

void RCMusicPlayTrackEdit(int32 which)
{
    uint32_t file = callerRegister(5), loop = callerRegister(6);
    GuestFrame frame(0x20);
    if (static_cast<uint32_t>(which) < 7u)
    {
        loop = 1;
        if (which == 0)
            file = 5;
        else
        {
            static constexpr uint32_t kOdd[7] = {0, 8, 0xc, 0x10, 0x16, 0x1a, 0x20}, kEven[7] = {0, 0xa, 0xe, 0x12, 0x18, 0x1c, 0x22};
            file = (RandomGetNDRange(1, 100) & 1u) != 0u ? kOdd[which] : kEven[which];
        }
    }
    rcmusicClass &r = music();
    r.file = static_cast<int32_t>(file);
    r.channel = 0;
    r.alternate = 0;
    MusicPlay(ptr32<char>(static_cast<uint32_t>(trackSector(static_cast<int32_t>(file)))), 0, static_cast<int32_t>(loop));
    r.cdState = 1;
}

// Fade 0 resumes at once, 0x7f resumes (pause command 2); otherwise sets up a fade in, from silence if paused.
void RCMusicResume(int32 fade)
{
    rcmusicClass &r = music();
    if (r.cdState == 0)
        return;
    if (r.preStart != 0)
    {
        RCMusicSetVol(0);
        return;
    }
    GuestFrame frame(0x20);
    if (fade == 0)
    {
        MusicPause(1);
        r.cdState = 1;
        return;
    }
    if (fade == 0x7f)
    {
        MusicPause(2);
        r.cdState = 1;
        return;
    }
    bool paused = false;
    int32_t span;
    if (r.cdState == 2)
    {
        span = r.userVol;
        paused = true;
        r.vol = 0;
    }
    else
        span = r.userVol - r.vol;
    r.volstore = static_cast<uint32_t>(ee_div(span, fade));
    r.fade = -fade;
    if (paused)
    {
        MusicPause(1);
        r.cdState = 1;
    }
}

void RCMusicChain(int32 secondXA, int8 fadeRate)
{
    GuestFrame frame(0x40);
    rcmusicClass &r = music();
    int8_t mode = r.chainMode;
    if (mode == 0)
    {
        if (secondXA != 0x69)
        {
            r.chainRate = fadeRate;
            r.chainMode = 2;
            mode = 2;
            r.chainTo = static_cast<int8>(secondXA);
        }
        else
        {
            const int32_t user = r.userVol;
            r.fade = fadeRate;
            mode = 1;
            r.chainMode = 1;
            r.chainRate = fadeRate;
            r.chainTo = static_cast<int8>(secondXA);
            r.volstore = static_cast<uint32_t>(ee_div(user, fadeRate));
        }
    }
    if (mode != 2)
        return;
    int32_t track;
    const uint32_t special = static_cast<uint32_t>(secondXA) - 100u;
    if (special < 5u)
    {
        static constexpr int32_t kTracks[5] = {1, 2, 3, 4, 7};
        track = kTracks[special];
    }
    else
        track = levelTrack(secondXA);
    if (track != 0)
    {
        MusicSwitch(trackSector(track), 0, trackSize(track));
        MusicRPC(0xb0, r.vol);
        r.file = track;
        r.chainMode = 0;
        return;
    }
    // Back to the front-end track at once, faded in at the chain's rate.
    MusicSwitchFront(trackSector(0), 0, trackSize(0));
    const int8_t rate = r.chainRate;
    const int32_t user = r.userVol;
    r.chainMode = 3;
    r.fade = -rate;
    r.file = 0;
    r.vol = 0;
    r.volstore = static_cast<uint32_t>(ee_div(user, rate));
}

void RCMusicFadeControl()
{
    GuestFrame frame(0x30);
    rcmusicClass &r = music();
    const int32_t fade = r.fade;
    if (fade > 0)
    {
        r.vol = r.vol - static_cast<int32_t>(r.volstore);
        if (r.vol < 0)
            r.vol = 0;
        r.fade = fade - 1;
        setVolume(r.vol);
        MusicRPC(0xb0, r.vol);
        if (r.fade != 0)
            return;
        if (r.chainMode == 1)
        {
            const int8 rate = r.chainRate, to = r.chainTo;
            r.chainMode = 2;
            RCMusicChain(to, rate);
        }
        else
        {
            MusicPause(0);
            r.cdState = 2;
        }
        return;
    }
    if (fade == 0)
        return;
    const int32_t vol = r.vol;
    if (r.file == 0 && vol == 0)
    {
        // The front-end track starts fading in after 26 frames of silence.
        const int32_t waited = load<int32_t>(kFrontFade) + 1;
        store<int32_t>(kFrontFade, waited);
        if (waited < 0x1a)
            return;
        store<int32_t>(kFrontFade, 0);
    }
    r.fade = fade + 1;
    const int32_t raised = static_cast<int32_t>(static_cast<uint32_t>(vol) + r.volstore);
    r.vol = raised;
    setVolume(raised);
    MusicRPC(0xb0, r.vol);
    if (r.fade != 0)
        return;
    const int32_t user = r.userVol;
    r.vol = user;
    setVolume(user);
    MusicRPC(0xb0, r.vol);
    if (r.chainMode == 3)
        r.chainMode = 0;
}

// In the front end (track 0) switches the channel; in a race switches to the alternate track (two on) and back.
void RCMusicSwitchAlternate(int32 state)
{
    GuestFrame frame(0x30);
    rcmusicClass &r = music();
    const int32_t file = r.file;
    if (file == 0)
    {
        const int32_t channel = r.channel;
        if (state == channel || channel >= 7)
            return;
        MusicSwitchChannel(state);
        r.channel = state;
        return;
    }
    if (state == r.alternate)
        return;
    const int32_t track = state != 0 ? file + 2 : file - 2;
    MusicSwitch(trackSector(track), trackRatio(track), trackSize(track));
    const int32_t now = r.file;
    r.alternate = state != 0 ? state : 0;
    r.file = state != 0 ? now + 2 : now - 2;
}

// fileCt's level track by its place in a group of six levels and the half of twelve it is in.
void RCMusicSetLevel(int32 fileCt)
{
    const int32_t inSix = fileCt % 6, inTwelve = fileCt % 12;
    if (static_cast<uint32_t>(inSix) >= 6u)
        return;
    static constexpr uint8_t kFirst[6] = {8, 0xc, 0x10, 0x16, 0x1a, 0x20}, kSecond[6] = {0xa, 0xe, 0x12, 0x18, 0x1c, 0x22};
    store<uint8_t>(kRc + 0x278u + static_cast<uint32_t>(fileCt), inTwelve < 6 ? kFirst[inSix] : kSecond[inSix]);
}

// Flag 0 silences, 1 resumes, else the user volume; a fade only records the new volume and its step.
void RCMusicModifyVolume(int32 fade, int32 flag)
{
    {
        GuestFrame frame(0x40);
        rcmusicClass &r = music();
        guest_call(kPrintf, kModifyFormat, flag);
        if (!(r.spawnDuringStart != 0 && flag == 1))
        {
            if (r.fade != 0)
                return;
            const int32_t user = r.userVol;
            r.vol = user;
            if (flag != 0)
            {
                if (user < 0)
                    r.vol = 0;
                setVolume(r.vol);
                MusicRPC(0xb0, r.vol);
                if (flag == 1 && r.spawnDuringStart == 0)
                    RCMusicResume(0);
            }
            const int32_t now = r.userVol;
            r.oldUserVol = now;
            r.volstore = static_cast<uint32_t>(ee_div(now, fade));
            return;
        }
    }
    RCMusicSetVol(2);
}

void RCMusicSetVol(int32 flags)
{
    {
        GuestFrame frame(0x40);
        rcmusicClass &r = music();
        guest_call(kPrintf, kRcSetVolFormat, flags);
        if (flags == 0)
        {
            r.vol = 0;
            setVolume(0);
        }
        else if (flags == 1)
        {
            r.preStart = 0;
            RCMusicResume(0x7f);
            r.cdState = flags;
            r.spawnDuringStart = 0;
            return;
        }
        else
        {
            const int32_t user = r.userVol;
            r.vol = user;
            setVolume(user);
        }
    }
    MusicRPC(0xb0, music().vol);
}

void RCMusicStop()
{
    MusicStop();
}

void RCMusicInitStream()
{
    MusicInitStream(0);
}

void RCMusicLvlTerm()
{
    MusicStop();
}

void RCMusicPlayLevel(int32 level)
{
    uint32_t file = callerRegister(5), loop = callerRegister(6);
    GuestFrame frame(0x20);
    const uint32_t special = static_cast<uint32_t>(level) - 0x61u;
    static constexpr int32_t kFiles[9] = {-1, -2, -2, 1, 2, 3, 4, 7, 0}, kLoops[9] = {0, 0, 0, 1, 1, 2, 2, 1, 1};
    if (special >= 9u || kFiles[special] == -2)
    {
        loop = 1;
        file = levelTrack(level);
    }
    else if (kFiles[special] >= 0)
    {
        file = static_cast<uint32_t>(kFiles[special]);
        loop = static_cast<uint32_t>(kLoops[special]);
    }
    rcmusicClass &r = music();
    r.file = static_cast<int32_t>(file);
    r.channel = 0;
    r.alternate = 0;
    MusicPlay(ptr32<char>(static_cast<uint32_t>(trackSector(static_cast<int32_t>(file)))), 0, static_cast<int32_t>(loop));
    r.cdState = 1;
}

void RCMusicTest(int32 track)
{
    (void)track;
}

// Starts the module and finds the tracks on the disc (an error message for each one not found).
int32 RCMusicInit(int32 numFiles)
{
    GuestFrame frame(0xa0);
    rcmusicClass &r = music();
    MusicInit();
    RCMusicInitLists();
    r.fade = 0;
    r.volstore = 0x3100;
    r.vol = 0x3100;
    r.userVol = 0x3100;
    r.oldUserVol = 0x3100;
    r.preStart = 0;
    r.alternate = 0;
    const uint32_t found = frame.local<sceCdlFILE>(0).address;
    for (int32_t i = 0; i < numFiles; ++i)
    {
        const uint32_t name = kRc + 0x158u + (static_cast<uint32_t>(i) << 2);
        if (guest_call<int32_t>(kCdSearchFile, found, load<uint32_t>(name)) == 0)
            guest_call(kPrintf, kLocateFormat, load<uint32_t>(name));
        else
        {
            store<uint32_t>(kRc + 0x1e8u + (static_cast<uint32_t>(i) << 2), load<uint32_t>(found));
            store<uint32_t>(kRc + 0xbcu + (static_cast<uint32_t>(i) << 2), load<uint32_t>(found + 4u));
        }
    }
    return 1;
}

// A fade pauses at its end (RCMusicFadeControl); fade 0 pauses at once.
void RCMusicPause(int32 fade)
{
    GuestFrame frame(0x50);
    rcmusicClass &r = music();
    if (r.preStart != 0 || r.cdState == 2 || r.cdState == 0)
        return;
    if (fade != 0)
    {
        const int32_t user = r.userVol;
        r.fade = fade;
        r.volstore = static_cast<uint32_t>(ee_div(user, fade));
        return;
    }
    MusicPause(0);
    r.cdState = 2;
    r.vol = 0;
}

// From a level's track (even) to its final lap track, the next one.
void RCMusicSwitchLastLap()
{
    GuestFrame frame(0x20);
    rcmusicClass &r = music();
    const int32_t file = r.file;
    const uint32_t bias = static_cast<uint32_t>(file) >> 31;
    const int32_t odd = static_cast<int32_t>(static_cast<uint32_t>(file) - (static_cast<uint32_t>(static_cast<int32_t>(static_cast<uint32_t>(file) + bias) >> 1) << 1));
    if (odd == 1 || file == 1)
        return;
    const int32_t track = file + 1;
    MusicSwitch(trackSector(track), trackRatio(track), trackSize(track));
    r.file = r.file + 1;
}

// --- MUSIC.c ---

void MusicStart(int32 channel)
{
    MusicRPC(0x50, channel);
}

void MusicStop()
{
    MusicRPC(0x8070, 0);
}

void MusicTerm()
{
    MusicRPC(0x8040, 0);
}

void MusicPlay(ptr32<char> file, int32 channel, int32 loop)
{
    (void)loop;
    {
        GuestFrame frame(0x30);
        guest_call(kCdDiskReady, 0);
        MusicRPC(0x8100, static_cast<int32_t>(file.address));
        MusicRPC(0x80e0, 1);
    }
    MusicRPC(0x50, channel);
}

void MusicSwitch(int32 pos, int32 ratio, int32 size)
{
    {
        GuestFrame frame(0x40);
        MusicRPC(0xc0, size);
        MusicRPC(0xd0, sectorAfter(pos, size));
    }
    MusicRPC(0x90, ratio != 0 ? static_cast<int32_t>((static_cast<uint32_t>(ratio) << 20) + static_cast<uint32_t>(pos)) : pos);
}

void MusicSwitchFront(int32 pos, int32 ratio, int32 size)
{
    (void)ratio;
    {
        GuestFrame frame(0x30);
        MusicRPC(0xc0, size);
        MusicRPC(0xd0, sectorAfter(pos, size));
    }
    MusicRPC(0x7ff0, pos);
}

void MusicInitStream(int32 mode)
{
    MusicRPC(0x80e0, mode);
}

void MusicSwitchSpecial(int32 channel)
{
    MusicRPC(0x90, channel);
}

void MusicSwitchChannel(int32 channel)
{
    MusicRPC(0x8060, channel);
}

void MusicPause(int32 flags)
{
    MusicRPC(0xa0, flags);
}

int32 MusicInit()
{
    GuestFrame frame(0x10);
    guest_call(kSifInitRpc, 0);
    if (MusicRPCinit() < 0)
        return -1;
    MusicRPC(0x8000, 0x4000);
    return 0;
}

// Binds the client to the module's server, retrying until the server is there.
int32 MusicRPCinit()
{
    GuestFrame frame(0x30);
    guest_call(kSifInitRpc, 0);
    for (;;)
    {
        if (guest_call<int32_t>(kSifBindRpc, kCd, 0x12345, 0) < 0)
        {
            guest_call(kPrintf, kBindError);
            return -1;
        }
        if (load<uint32_t>(kCd + 0x24u) != 0u)
            return 0;
    }
}

// One command to the module, blocking; returns the first word of its reply.
int32 MusicRPC(int32 command, int32 data)
{
    GuestFrame frame(0x30);
    store<uint32_t>(frame.local<uint32_t>(0).address, 0u); // the end function's argument, on the stack
    if ((static_cast<uint32_t>(command) & 0xfff0u) == 0x8020u)
        guest_call(kSifCallRpc, kCd, command, 0, data, 0x40, kSbuf, 0x40, 0);
    else
    {
        store<int32_t>(kSbuf, data);
        guest_call(kSifCallRpc, kCd, command, 0, kSbuf, 0x10, kSbuf, 0x40, 0);
    }
    return load<int32_t>(kSbuf);
}

} // namespace rc

RC_DECOMP_FN_SETUP(RCMusicInitLists, 0x1409b8, rc::fuzz::rcLists)
RC_DECOMP_FN_BLOCKING_SETUP(RCMusicPlayTrackEdit, 0x140cf8, rc::fuzz::rcMusic)
RC_DECOMP_FN_BLOCKING_SETUP(RCMusicResume, 0x140e20, rc::fuzz::rcMusic)
RC_DECOMP_FN_BLOCKING_SETUP(RCMusicChain, 0x140f08, rc::fuzz::rcChain)
RC_DECOMP_FN_BLOCKING_SETUP(RCMusicFadeControl, 0x141080, rc::fuzz::rcMusic)
RC_DECOMP_FN_BLOCKING_SETUP(RCMusicSwitchAlternate, 0x1411e8, rc::fuzz::rcMusic)
RC_DECOMP_FN_SETUP(RCMusicSetLevel, 0x1412c0, rc::fuzz::rcLevel)
RC_DECOMP_FN_BLOCKING_SETUP(RCMusicModifyVolume, 0x1413f0, rc::fuzz::rcVolume)
RC_DECOMP_FN_BLOCKING_SETUP(RCMusicSetVol, 0x1414e0, rc::fuzz::rcMusic)
RC_DECOMP_FN_BLOCKING_SETUP(RCMusicStop, 0x1415b8, rc::fuzz::rcMusic)
RC_DECOMP_FN_BLOCKING_SETUP(RCMusicInitStream, 0x1415c0, rc::fuzz::rcMusic)
RC_DECOMP_FN_BLOCKING_SETUP(RCMusicLvlTerm, 0x1415c8, rc::fuzz::rcMusic)
RC_DECOMP_FN_BLOCKING_SETUP(RCMusicPlayLevel, 0x1415d0, rc::fuzz::rcMusic)
RC_DECOMP_FN(RCMusicTest, 0x1416a0)
RC_DECOMP_FN_BLOCKING_SETUP(RCMusicInit, 0x1416a8, rc::fuzz::rcInit)
RC_DECOMP_FN_BLOCKING_SETUP(RCMusicPause, 0x141790, rc::fuzz::rcMusic)
RC_DECOMP_FN_BLOCKING_SETUP(RCMusicSwitchLastLap, 0x141828, rc::fuzz::rcMusic)
RC_DECOMP_FN_BLOCKING_SETUP(MusicStart, 0x1af430, rc::fuzz::musicArgs)
RC_DECOMP_FN_BLOCKING_SETUP(MusicStop, 0x1af440, rc::fuzz::musicArgs)
RC_DECOMP_FN_BLOCKING_SETUP(MusicTerm, 0x1af450, rc::fuzz::musicArgs)
RC_DECOMP_FN_BLOCKING_SETUP(MusicPlay, 0x1af460, rc::fuzz::musicArgs)
RC_DECOMP_FN_BLOCKING_SETUP(MusicSwitch, 0x1af4b8, rc::fuzz::musicArgs)
RC_DECOMP_FN_BLOCKING_SETUP(MusicSwitchFront, 0x1af578, rc::fuzz::musicArgs)
RC_DECOMP_FN_BLOCKING_SETUP(MusicInitStream, 0x1af5d8, rc::fuzz::musicArgs)
RC_DECOMP_FN_BLOCKING_SETUP(MusicSwitchSpecial, 0x1af5e8, rc::fuzz::musicArgs)
RC_DECOMP_FN_BLOCKING_SETUP(MusicSwitchChannel, 0x1af5f8, rc::fuzz::musicArgs)
RC_DECOMP_FN_BLOCKING_SETUP(MusicPause, 0x1af608, rc::fuzz::musicArgs)
RC_DECOMP_FN_BLOCKING_SETUP(MusicInit, 0x1af618, rc::fuzz::musicArgs)
RC_DECOMP_FN_BLOCKING_SETUP(MusicRPCinit, 0x1af658, rc::fuzz::musicArgs)
RC_DECOMP_FN_BLOCKING_SETUP(MusicRPC, 0x1af7e0, rc::fuzz::musicRpc)

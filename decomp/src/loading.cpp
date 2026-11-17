// Decompiled from LOADING.c: the loading screen, which is a movie (a car or a boat clip chosen by the level) started on
// the MPEG player's interrupt-driven path, and the front end's flying logo counter. The border and background
// drawers are empty in this version.
//
// Never called by the game: LoadingCallBackJumping, LoadingDrawBorder, LoadingFlagBackround (all empty).

#include "rc_decomp.h"

namespace rc {

namespace {

constexpr uint32_t kRCMusicStop = sym::RCMusicStop, kMpegLoad = sym::mpegOnInterruptLOAD, kMpegPlay = sym::mpegOnInterruptPLAY, kSprintf = sym::sprintf;
constexpr uint32_t kDisplaySetBackground = sym::DisplaySetBackground, kVSyncCallback = sym::VSyncCallback, kVSyncCallbackFunction = sym::VSyncCallbackFunction;
// Movie names: "scar1.m2v", "sbot2.m2v", "sbot1.m2v", "scar2.m2v"; fileUsed holds the one chosen.
constexpr uint32_t kScar1 = (sym::sec_rodata + 0xaa0u), kSbot2 = (sym::sec_rodata + 0xab0u), kSbot1 = (sym::sec_rodata + 0xac0u), kScar2 = (sym::sec_rodata + 0xad0u), kFileUsed = sym::fileUsed;
constexpr uint32_t kMpegBuffer1 = (sym::mpegBuffer + 0x4u), kMainLevel = (sym::mainVarG + 0x34au), kMainNoHuman = (sym::mainVarG + 0x369u), kFlyingLogo = (sym::frontEndVarG + 0x2a4u);

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

} // namespace

// Stops the music and, unless a loading screen is up already, starts one: type 2 the first car clip, type 1 the
// clip named in fileUsed (the first car clip if none, nothing if a movie buffer is in use), types 3 and 4 a clip for
// the level (boats for levels 0, 5 and 8 of each twelve) and the number of human players.
void LoadingInit(int32 iType)
{
    {
        GuestFrame frame(0x20);
        guest_call(kRCMusicStop);
        if (G(bLoadingG) == 0)
        {
            uint32_t movie = 0;
            bool play = true;
            if (iType == 2)
            {
                guest_call(kSprintf, kFileUsed, kScar1);
                movie = kFileUsed;
            }
            else if (iType < 3)
            {
                if (iType != 1 || load<uint32_t>(kMpegBuffer1) != 0u)
                    play = false;
                else if (load<int8_t>(kFileUsed) != 0)
                    movie = kFileUsed;
                else
                {
                    guest_call(kMpegLoad, kScar1, 1, 0, 1);
                    guest_call(kMpegPlay, 1, 1);
                    play = false;
                }
            }
            else if (iType >= 5)
                play = false;
            else
            {
                const uint8_t stage = static_cast<uint8_t>(load<uint8_t>(kMainLevel) % 12u);
                const uint8_t humans = load<uint8_t>(kMainNoHuman);
                if (stage == 5 || stage == 0 || stage == 8)
                    guest_call(kSprintf, kFileUsed, humans == 2 ? kSbot2 : kSbot1);
                else
                    guest_call(kSprintf, kFileUsed, humans == 2 ? kScar2 : kScar1);
                movie = kFileUsed;
            }
            if (play && movie != 0u)
            {
                guest_call(kMpegLoad, movie, 1, 0, 1);
                guest_call(kMpegPlay, 1, 1);
            }
        }
    }
    G(bLoadingG) = 1;
}

void LoadingCallbackTerm()
{
    {
        GuestFrame frame(0x10);
        guest_call(kDisplaySetBackground, 0, 0, 0);
    }
    guest_call(kVSyncCallback, kVSyncCallbackFunction);
}

void LoadingTerm() { G(bLoadingG) = 0; }

void LoadingCallBackJumping() {}

void LoadingDrawBorder(int32, int32, int32, int32, int32, uint32, uint32) {}

void LoadingFlagBackround() {}

// The front end's flying logo: its frame counter runs once started (non-zero).
void LoadingPlayFlyingLogo()
{
    const int32_t frame = load<int32_t>(kFlyingLogo);
    if (frame != 0)
        store<int32_t>(kFlyingLogo, frame + 1);
}

void LoadingShowFlyingLogo() {}

} // namespace rc

#include "res_fuzz.h"

RC_DECOMP_FN_BLOCKING_SETUP(LoadingInit, rc::fuzz::loadingInit)
RC_DECOMP_FN_SETUP(LoadingCallbackTerm, rc::fuzz::loadingFlags)
RC_DECOMP_FN_SETUP(LoadingTerm, rc::fuzz::loadingFlags)
RC_DECOMP_FN(LoadingCallBackJumping)
RC_DECOMP_FN(LoadingDrawBorder)
RC_DECOMP_FN(LoadingFlagBackround)
RC_DECOMP_FN_SETUP(LoadingPlayFlyingLogo, rc::fuzz::loadingFlags)
RC_DECOMP_FN(LoadingShowFlyingLogo)

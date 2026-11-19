// Decompiled from BACKDROP.c: the boot sequence's still screens and their sound. BackdropDoInitialScreens shows the
// legal screen, loads the two halves of a stereo VAG into SPU2 memory and starts them, plays the Acclaim movie, then
// shows three logo screens for up to 300 frames each (cross or circle moves on, start or select skips the rest), and
// starts the loading movie. The screens are 640x240 24-bit BMPs from \SPLASH\, converted to 32-bit and uploaded to
// both frame buffers in two 128-line halves.
//
// The other five functions are empty; BackdropInit and BackdropTerm are called by MainInit and MainTerm, and
// BackdropThrowUpBuffer, BackdropFadeUpBuffer and BackdropFadeDown never.

#include "rc_decomp.h"

namespace rc {

bool_ MemBlockAllocLo(ptr32<memHandle> pHandle, uint32 size); // mem.cpp
void MemBlockFree(ptr32<memHandle> pHandle);

namespace {

constexpr uint32_t kFMPrepare = sym::FMPrepare, kFMUnPrepare = sym::FMUnPrepare, kFMFileOpen = sym::FMFileOpen, kFMFileRead = sym::FMFileRead,
                   kFMFileClose = sym::FMFileClose, kFMFileSeek = sym::FMFileSeek, kFMFileLength = sym::FMFileLength;
constexpr uint32_t kSprintf = sym::sprintf, kStrcat = sym::strcat, kFlushCache = sym::FlushCache;
constexpr uint32_t kSetDefLoadImage = sym::sceGsSetDefLoadImage, kExecLoadImage = sym::sceGsExecLoadImage, kSyncPath = sym::sceGsSyncPath, kSyncV = sym::sceGsSyncV;
constexpr uint32_t kSifAllocIopHeap = sym::sceSifAllocIopHeap, kSifFreeIopHeap = sym::sceSifFreeIopHeap, kSdTransToIOP = sym::sceSdTransToIOP, kSdRemote = sym::sceSdRemote;
constexpr uint32_t kDisplayInitHiRes = sym::DisplayInitHiRes, kSwapBuffers = sym::GfxP2_SwapBuffers, kInputSnapshot = sym::InputSnapshot, kInputHit = sym::InputHit;
constexpr uint32_t kMpegLoad = sym::mpegOnInterruptLOAD, kMpegPlay = sym::mpegOnInterruptPLAY, kMpegTerm = sym::mpegOnInterruptTERM, kMpegFinished = sym::mpegOnInterruptFINISHED;
constexpr uint32_t kSplashFolder = (sym::sec_rodata + 0x248u); // "\splash\"
constexpr uint32_t kLegal = (sym::sec_rodata + 0x1c8u), kLeftVag = (sym::sec_rodata + 0x1d8u), kRightVag = (sym::sec_rodata + 0x1e8u), kAcclaimMovie = (sym::sec_rodata + 0x1f8u),
                   kCarMovie = (sym::sec_rodata + 0x208u), kAscScreen = (sym::sec_rodata + 0x218u), kDbyScreen = (sym::sec_rodata + 0x228u), kLogoScreen = (sym::sec_rodata + 0x238u);
constexpr uint32_t kGsLoadImage = sym::gs_limage, kErrorFn = (sym::resVarG + 0x3d0u); // gs_limage, resVarG.pfLoadErrorFunction
constexpr uint32_t kWidth = 640u, kHalfHeight = 0x80u;
// Pad keys InputHit takes: start and select end the sequence, cross and circle move to the next screen.
constexpr uint32_t kKeyStart = 0x03u, kKeySelect = 0x13u, kKeyCross = 0x0eu, kKeyCircle = 0x1eu;

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

// Reads a whole file from \splash\ into a new block at handle (a word in the caller's frame); the frame's first 0x50
// bytes hold the name. Retries as the loader does: seek back after each failure, the error function (with the file
// and 0) after six. Returns the file and its length.
std::pair<int32_t, uint32_t> readSplashFile(const GuestFrame &frame, uint32_t handle, ptr32<char> name)
{
    const ptr32<char> path = frame.local<char>(0);
    guest_call(kSprintf, path, kSplashFolder);
    guest_call(kStrcat, path, name);
    const int32_t fd = guest_call<int32_t>(kFMFileOpen, path);
    const uint32_t length = guest_call<uint32_t>(kFMFileLength, fd);
    MemBlockAllocLo(ptr32<memHandle>(handle), length);
    int32_t retries = 0;
    for (;;)
    {
        const uint32_t read = guest_call<uint32_t>(kFMFileRead, load<uint32_t>(handle), length, fd);
        ++retries;
        if (read == length)
            break;
        if (retries >= 6)
        {
            const uint32_t errorFn = load<uint32_t>(kErrorFn);
            if (errorFn != 0u)
            {
                retries = 0;
                guest_call(errorFn, fd, 0);
            }
        }
        guest_call(kFMFileSeek, fd, 0, 0);
    }
    return {fd, length};
}

bool hit(uint32_t key) { return guest_call<int32_t>(kInputHit, key) != 0; }

} // namespace

ptr32<void> BackdropLoadBMP24(ptr32<char> name)
{
    GuestFrame frame(0xb0);
    const uint32_t handle = frame.local<uint32_t>(0x50).address;
    store<uint32_t>(handle, 0);
    guest_call(kFMPrepare, 0x32000);
    const int32_t fd = readSplashFile(frame, handle, name).first;
    guest_call(kFMFileClose, fd);
    guest_call(kFMUnPrepare);
    return ptr32<void>(load<uint32_t>(handle));
}

// Converts the BMP's 640x240 BGR pixels (after its 0x36-byte header, rows as stored) to RGBX in a 640x256 block and
// uploads it as two 128-line halves to the frame buffer that is not shown when buffer is the shown one's parity
// (0xa00 when frameNumber & 1 == buffer, else 0).
void BackdropDisplayBMP24(ptr32<void> pBmp, uint32 buffer)
{
    RC_DIVERT_IF_CHANGED(BackdropDisplayBMP24, pBmp, buffer);
    GuestFrame frame(0xf0);
    // Side tag: the two uploads into the frame buffer.
    const rc::gfx::SpanKicks upload(rc::gfx::KickLabel{rc::gfx::KickBackdrop, pBmp.address, buffer, 0u});
    const uint32_t handle = frame.local<uint32_t>(0x50).address;
    store<uint32_t>(handle, 0);
    MemBlockAllocLo(ptr32<memHandle>(handle), 0xa0000);
    uint32_t source = pBmp.address + 0x36u;
    for (uint32_t pixel = 0; pixel < 0x25800u; ++pixel, source += 3u)
    {
        const uint32_t out = pixel * 4u;
        store<uint8_t>(load<uint32_t>(handle) + out, load<uint8_t>(source + 2u));
        store<uint8_t>(load<uint32_t>(handle) + out + 1u, load<uint8_t>(source + 1u));
        store<uint8_t>(load<uint32_t>(handle) + out + 2u, load<uint8_t>(source));
        store<uint8_t>(load<uint32_t>(handle) + out + 3u, 0);
    }
    const auto base = [buffer] { return (G(gRdr).frameNumber & 1u) == buffer ? 0xa00u : 0u; };
    guest_call(kSetDefLoadImage, kGsLoadImage, base(), kWidth >> 6, 0, 0, 0, kWidth, kHalfHeight);
    guest_call(kFlushCache, 0);
    guest_call(kExecLoadImage, kGsLoadImage, load<uint32_t>(handle));
    guest_call(kSyncPath, 0, 0);
    guest_call(kSetDefLoadImage, kGsLoadImage, base(), kWidth >> 6, 0, 0, kHalfHeight, kWidth, kHalfHeight);
    guest_call(kFlushCache, 0);
    guest_call(kExecLoadImage, kGsLoadImage, load<uint32_t>(handle) + kHalfHeight * kWidth * 4u);
    guest_call(kSyncPath, 0, 0);
    MemBlockFree(ptr32<memHandle>(handle));
}

// Loads a VAG (after its 0x30-byte header) into SPU2 memory at dwAddr through an IOP buffer and sets it as voice 0 or
// 1's start (the left half when dwAddr is still 0x5000, its first value). dwAddr moves on past it.
void BackdropLoadVAG(ptr32<char> name)
{
    GuestFrame frame(0xc0);
    const uint32_t handle = frame.local<uint32_t>(0x50).address;
    store<uint32_t>(handle, 0);
    guest_call(kFMPrepare, 0x4b000);
    const int32_t address = G(dwAddr);
    if (address != 0x5000)
        G(rightVAGAddr) = static_cast<uint32_t>(address);
    else
        G(leftVAGAddr) = static_cast<uint32_t>(address);
    const auto [fd, length] = readSplashFile(frame, handle, name);
    guest_call(kFlushCache, 0);
    const uint32_t iop = guest_call<uint32_t>(kSifAllocIopHeap, length);
    guest_call(kSdTransToIOP, load<uint32_t>(handle) + 0x30u, iop, length, 1);
    guest_call(kSdRemote, 1, 0x80d0, 1, 0, iop, G(dwAddr), length);
    guest_call(kSdRemote, 1, 0x80f0, 1, 1);
    guest_call(kSifFreeIopHeap, iop);
    G(dwAddr) = static_cast<int32_t>(static_cast<uint32_t>(G(dwAddr)) - 0x30u + length);
    guest_call(kFMFileClose, fd);
    guest_call(kFMUnPrepare);
    MemBlockFree(ptr32<memHandle>(handle));
}

// Voices 0 and 1 (left and right): start addresses, pitch 0xe63, envelope, and key on for both.
void BackdropPlayVAG()
{
    {
        GuestFrame frame(0x10);
        guest_call(kSdRemote, 1, 0x8050, 0x2040, G(leftVAGAddr));
        guest_call(kSdRemote, 1, 0x8010, 0x200, 0xe63);
        guest_call(kSdRemote, 1, 0x8050, 0x2042, G(rightVAGAddr));
        guest_call(kSdRemote, 1, 0x8010, 0x202, 0xe63);
        guest_call(kSdRemote, 1, 0x8010, 0, 0x3000);
        guest_call(kSdRemote, 1, 0x8010, 0x102, 0x3000);
        guest_call(kSdRemote, 1, 0x8030, 0x1500, 1);
    }
    guest_call(kSdRemote, 1, 0x8030, 0x1500, 2);
}

namespace {

// Loads a screen and shows it in both frame buffers, swapping between.
void showScreen(uint32_t pBuffer, uint32_t name)
{
    const uint32_t bmp = BackdropLoadBMP24(ptr32<char>(name)).address;
    store<uint32_t>(pBuffer, bmp);
    BackdropDisplayBMP24(ptr32<void>(bmp), 0);
    G(gRdr).frameNumber += 1;
    BackdropDisplayBMP24(ptr32<void>(load<uint32_t>(pBuffer)), 0);
    guest_call(kSwapBuffers);
    G(gRdr).frameNumber += 1;
    MemBlockFree(ptr32<memHandle>(pBuffer));
}

enum class Wait { Timeout, Next, Skip };

// Up to 300 frames; with anyKeyEnds every key ends the sequence.
Wait waitScreen(bool anyKeyEnds)
{
    for (int32_t frame = 0; frame < 300; ++frame)
    {
        guest_call(kInputSnapshot);
        guest_call(kSyncV, 0);
        if (hit(kKeyStart) || hit(kKeySelect))
            return Wait::Skip;
        if (hit(kKeyCross) || hit(kKeyCircle))
            return anyKeyEnds ? Wait::Skip : Wait::Next;
    }
    return Wait::Timeout;
}

} // namespace

void BackdropDoInitialScreens()
{
    GuestFrame frame(0x50);
    const uint32_t pBuffer = frame.local<uint32_t>(0).address;
    guest_call(kDisplayInitHiRes);
    showScreen(pBuffer, kLegal);
    BackdropLoadVAG(ptr32<char>(kLeftVag));
    BackdropLoadVAG(ptr32<char>(kRightVag));
    guest_call(kMpegLoad, kAcclaimMovie, 0, 1, 0);
    BackdropPlayVAG();
    guest_call(kMpegPlay, 0, 0);
    guest_call(kMpegLoad, kCarMovie, 1, 0, 1);
    store<uint32_t>(pBuffer, BackdropLoadBMP24(ptr32<char>(kAscScreen)).address);
    // Polls until the movie thread ends the movie: each poll is a call, so a scheduling point.
    do
        guest_checkpoint();
    while (guest_call<int32_t>(kMpegFinished) == 0);
    guest_call(kMpegTerm, 0);
    BackdropDisplayBMP24(ptr32<void>(load<uint32_t>(pBuffer)), 0);
    G(gRdr).frameNumber += 1;
    BackdropDisplayBMP24(ptr32<void>(load<uint32_t>(pBuffer)), 0);
    guest_call(kSwapBuffers);
    G(gRdr).frameNumber += 1;
    MemBlockFree(ptr32<memHandle>(pBuffer));
    if (waitScreen(false) != Wait::Skip)
    {
        showScreen(pBuffer, kDbyScreen);
        if (waitScreen(false) != Wait::Skip)
        {
            showScreen(pBuffer, kLogoScreen);
            waitScreen(true);
        }
    }
    guest_call(kMpegPlay, 1, 1);
}

void BackdropInit() {}
void BackdropTerm() {}
void BackdropThrowUpBuffer(ptr32<uint32>, uint32) {}
void BackdropFadeUpBuffer(ptr32<uint32>) {}
void BackdropFadeDown() {}

} // namespace rc

#include "res_fuzz.h"

RC_DECOMP_FN_BLOCKING_SETUP_HEAP(BackdropDoInitialScreens, rc::fuzz::backdropScreens)
RC_DECOMP_FN_BLOCKING_SETUP_HEAP(BackdropLoadBMP24, rc::fuzz::backdropBmp)
RC_DECOMP_FN_BLOCKING_SETUP_HEAP(BackdropDisplayBMP24, rc::fuzz::backdropDisplay)
RC_DECOMP_FN_BLOCKING_SETUP_HEAP(BackdropLoadVAG, rc::fuzz::backdropVag)
RC_DECOMP_FN_BLOCKING_SETUP(BackdropPlayVAG, rc::fuzz::backdropPlay)
RC_DECOMP_FN(BackdropInit)
RC_DECOMP_FN(BackdropTerm)
RC_DECOMP_FN(BackdropThrowUpBuffer)
RC_DECOMP_FN(BackdropFadeUpBuffer)
RC_DECOMP_FN(BackdropFadeDown)

// Decompiled from MPEG.c and MPDISPLAY.c: the movie player (the SDK's MPEG decoder on whole .M2V files read into
// memory from \SPLASH\ on the disc).
//
// The game uses the interrupt-driven path: mpegOnInterruptLOAD reads a movie into one of two buffers (mpegSwitch picks
// the one played), mpegOnInterruptPLAY starts the decoder and installs vblankPlayer as the vertical blank handler
// (INTC 2), and mpegOnInterruptTERM removes it and frees the buffer. vblankPlayer decodes a picture on even vertical
// blanks and uploads it to the frame buffer not on screen on odd ones (two upload chains, path3tag[0] and [1], for
// the two halves of a 640x512 frame buffer), then flips the display. At the end of the stream it restarts it when
// mpegLooping is 1 and otherwise sets mpegLooping to -1 and stops, which mpegOnInterruptFINISHED reports.
//
// Never called by the game (no jal, no address taken, no data pointer): playMpegFile and what only it reaches
// (playMpeg, vblankHandler, syncDisplay), MpegLoadFile, MpegDoAFrame, MpegInitSplash, MpegSplashFrame, MpegSplashTERM
// and vblankSplashPlayer. syncDisplay never returns once it has to wait (the vertical blank counter is read once,
// before its wait loop), so playMpeg could only have played movies whose frame numbers never pass half that counter.

#include "rc_decomp.h"
#include "stream_fuzz.h"

// Europe variants (NOTES.md "Europe version"): mpegOnInterruptPLAY, MpegLoadFile and MpegSplashFrame upload the picture
// 8 lines down in the 256-line PAL frame buffer (rows 8 and 0x108 instead of 0 and 0x100, kMovieTop); playMpeg keeps 0
// and 0x100 in both versions. playMpeg, MpegInitSplash and mpegOnInterruptLOAD store in another order (the same body).
RC_VERSION_VARIANT(mpegOnInterruptPLAY)
RC_VERSION_VARIANT(playMpeg)
RC_VERSION_VARIANT(MpegLoadFile)
RC_VERSION_VARIANT(MpegInitSplash)
RC_VERSION_VARIANT(MpegSplashFrame)
RC_VERSION_VARIANT(mpegOnInterruptLOAD)

namespace rc {

void MemBlockFree(ptr32<memHandle> pHandle); // mem.cpp
bool_ MemBlockAllocLo(ptr32<memHandle> pHandle, uint32 size);
bool_ MemBlockAllocHi(ptr32<memHandle> pHandle, uint32 size);
void loadImage(ptr32<u_int> tags); // ldimage.cpp
void setLoadImageTags(ptr32<u_int> tags, ptr32<void> image, int32_t x, int32_t y, int32_t width, int32_t height);
constexpr int32_t kMovieTop = RC_VERSION_EU ? 8 : 0; // the picture's first row in the frame buffer
void setLoadImageTagsTile(ptr32<u_int> tags, ptr32<void> image, int32_t x, int32_t y, int32_t width, int32_t height);

int32_t playMpeg(ptr32<sceMpeg> mp, ptr32<ee_uint128> bs, int32_t size);
int32_t readFile(ptr32<char> name, ptr32<ptr32<uint8_t>> data, ptr32<int32_t> size, int32 hi);
void clearBackground(u_int val);
void startDisplay(int32_t waitEven);
void endDisplay();
void syncDisplay(int32_t frame);

namespace {

constexpr uint32_t kMpegG = sym::mpegG, kSplashG = sym::mpegSplashG, kDisp = sym::disp, kDispFb = kDisp + 0x10u;
constexpr uint32_t kMpegStore = sym::mpegStore, kRgb32 = sym::rgb32, kPath3tag = sym::path3tag, kBackTile = sym::backTile,
                   kBackTileTags = sym::backTileTags;
constexpr uint32_t kDrawid = sym::drawid, kVblankHandle = sym::vblankHandle, kMpegSwitch = sym::mpegSwitch, kMpegLooping = sym::mpegLooping,
                   kMpegBuffer = sym::mpegBuffer, kMpegSize = sym::mpegSize, kSplashBuffer = sym::mpegSplashBuffer, kSplashSize = sym::mpegSplashSize,
                   kRet = sym::ret, kIsCountVblank = sym::isCountVblank, kVblankCount = sym::vblankCount, kGetit = sym::getit;
constexpr uint32_t kMpegInit = sym::sceMpegInit, kMpegCreate = sym::sceMpegCreate, kMpegDelete = sym::sceMpegDelete, kMpegAddBs = sym::sceMpegAddBs,
                   kMpegGetPicture = sym::sceMpegGetPicture, kMpegIsEnd = sym::sceMpegIsEnd, kMpegReset = sym::sceMpegReset;
constexpr uint32_t kGsSetDefDispEnv = sym::sceGsSetDefDispEnv, kGsPutDispEnv = sym::sceGsPutDispEnv, kGsSyncPath = sym::sceGsSyncPath, kGsSyncV = sym::sceGsSyncV;
constexpr uint32_t kAddIntcHandler = sym::AddIntcHandler, kRemoveIntcHandler = sym::RemoveIntcHandler, kEnableIntc = sym::EnableIntc,
                   kDisableIntc = sym::DisableIntc;
constexpr uint32_t kPrintf = sym::printf, kSprintf = sym::sprintf, kStrcat = sym::strcat, kFree = sym::free;
constexpr uint32_t kFMPrepare = sym::FMPrepare, kFMUnPrepare = sym::FMUnPrepare, kFMFileOpen = sym::FMFileOpen, kFMFileRead = sym::FMFileRead,
                   kFMFileClose = sym::FMFileClose, kFMFileSeek = sym::FMFileSeek;
constexpr uint32_t kVblankPlayer = sym::vblankPlayer, kVblankHandler = sym::vblankHandler;
// "\\splash\\", "Cannot open %s\n", "sceLseek() fails (%s)\n", "memalign(%d) fails\n", "Cannot read %s\n",
// "%s(%dbyte)\n", "sceMpegGetPicture Failed\n", "[frame drop] frame %d\n".
constexpr uint32_t kSplashDir = (sym::sec_rodata + 0x2480u), kCannotOpen = (sym::sec_rodata + 0x2490u), kSeekFails = (sym::sec_rodata + 0x24a0u), kAllocFails = (sym::sec_rodata + 0x24b8u),
                   kCannotRead = (sym::sec_rodata + 0x24d0u), kPlaying = (sym::sec_rodata + 0x2450u), kPictureFailed = (sym::sec_rodata + 0x2460u), kFrameDrop = (sym::sec_rodata + 0x24e0u);
constexpr uint32_t kTagBytes = 0xf100u; // one of the two upload chains in path3tag

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

// The ei the vertical blank handlers end with.
void enableInterrupts()
{
    g_runtime->eeScheduler().runningContext().cop0_status |= 0x10000u;
}

ptr32<u_int> uploadChain(uint32_t which) { return ptr32<u_int>(kPath3tag + which * kTagBytes); }

// The display at the frame buffer half not drawn to: DISPFB's FBW set to 640 pixels and FBP cleared, then the
// half chosen by drawid, which it flips.
void displaySetUp()
{
    guest_call(kGsSetDefDispEnv, kDisp, 0, 0x280, 0x100, 0, 0);
    store<uint32_t>(kDispFb, (load<uint32_t>(kDispFb) & 0xffff81ffu) | 0x1400u);
    guest_call(kGsPutDispEnv, kDisp);
}

// Returns the new drawid; the display base is 0x50 (the second half) when drawid was set.
uint32_t flipDisplay()
{
    if (load<int32_t>(kDrawid) != 0)
    {
        store<uint32_t>(kDispFb, (load<uint32_t>(kDispFb) & 0xfffffe00u) | 0x50u);
        return 0;
    }
    store<uint32_t>(kDispFb, load<uint32_t>(kDispFb) & 0xfffffe00u);
    return 1;
}

uint32_t bufferSlot(int32_t index) { return kMpegBuffer + (static_cast<uint32_t>(index) << 2); }
uint32_t sizeSlot(int32_t index) { return kMpegSize + (static_cast<uint32_t>(index) << 2); }

void addCurrentStream(uint32_t decoder)
{
    const int32_t current = load<int32_t>(kMpegSwitch);
    guest_call(kMpegAddBs, decoder, load<uint32_t>(bufferSlot(current)), load<int32_t>(sizeSlot(current)));
}

} // namespace

// --- MPEG.c ---

void playMpegFile(ptr32<char> filename)
{
    {
        GuestFrame frame(0x40);
        const int32_t current = load<int32_t>(kMpegSwitch);
        if (readFile(filename, ptr32<ptr32<uint8_t>>(bufferSlot(current)), ptr32<int32_t>(sizeSlot(current)), 0) == 0)
            return;
        guest_call(kPrintf, kPlaying, filename, kMpegSize);
        guest_call(kMpegInit);
        clearBackground(0x80000000u);
        guest_call(kMpegCreate, kMpegG, kMpegStore, 0xb4800);
        guest_call(kAddIntcHandler, 2, kVblankHandler, 0);
        guest_call(kEnableIntc, 2);
        const int32_t playing = load<int32_t>(kMpegSwitch);
        playMpeg(ptr32<sceMpeg>(kMpegG), ptr32<ee_uint128>(load<uint32_t>(bufferSlot(playing))), load<int32_t>(sizeSlot(playing)));
        guest_call(kMpegReset, kMpegG);
        guest_call(kDisableIntc, 2);
        guest_call(kMpegDelete, kMpegG);
    }
    MemBlockFree(ptr32<memHandle>(bufferSlot(load<int32_t>(kMpegSwitch))));
}

// Starts the movie in buffer swatch (nothing if that buffer is empty): decoder, display, first picture and the upload
// chains for its size, then vblankPlayer on INTC 2.
void mpegOnInterruptPLAY(int32 swatch, int32 looping)
{
    {
        GuestFrame frame(0x50);
#if RC_VERSION_EU
        // Europe reads the buffer slot before storing (the same unless swatch is out of range, -1 reading mpegLooping).
        const uint32_t buffer = load<uint32_t>(bufferSlot(swatch));
        store<int32_t>(kMpegLooping, looping);
        store<int32_t>(kMpegSwitch, swatch);
        if (buffer == 0u)
            return;
#else
        store<int32_t>(kMpegLooping, looping);
        store<int32_t>(kMpegSwitch, swatch);
        if (load<uint32_t>(bufferSlot(swatch)) == 0u)
            return;
#endif
        guest_call(kMpegInit);
        clearBackground(0x80000000u);
        guest_call(kMpegCreate, kMpegG, kMpegStore, 0xb4800);
        store<int32_t>(kDrawid, 1);
        displaySetUp();
        addCurrentStream(kMpegG);
        startDisplay(1);
        guest_call(kGsSyncPath, 0, 0);
        guest_call(kMpegGetPicture, kMpegG, kRgb32, 0x280);
        setLoadImageTags(uploadChain(0), ptr32<void>(kRgb32), 0, kMovieTop, load<int32_t>(kMpegG), load<int32_t>(kMpegG + 4u));
        setLoadImageTags(uploadChain(1), ptr32<void>(kRgb32), 0, kMovieTop + 0x100, load<int32_t>(kMpegG), load<int32_t>(kMpegG + 4u));
        store<int32_t>(kVblankHandle, guest_call<int32_t>(kAddIntcHandler, 2, kVblankPlayer, 0));
    }
    guest_call(kEnableIntc, 2);
}

void mpegOnInterruptTERM(int32 swatch)
{
    {
        GuestFrame frame(0x20);
        const uint32_t slot = bufferSlot(swatch);
        if (load<uint32_t>(slot) == 0u)
            return;
        if (load<int32_t>(kVblankHandle) != 0)
        {
            guest_call(kDisableIntc, 2);
            guest_call(kRemoveIntcHandler, 2, load<int32_t>(kVblankHandle));
            store<int32_t>(kVblankHandle, 0);
        }
        guest_call(kGsSyncV, 0);
        clearBackground(0x80000000u);
        guest_call(kMpegDelete, kMpegG);
        MemBlockFree(ptr32<memHandle>(slot));
        store<uint32_t>(slot, 0u);
    }
    endDisplay();
}

// Plays a stream to its end, uploading each picture and waiting for its frame (see syncDisplay). Returns
// sceMpegIsEnd's answer (non-zero), left in $v0 by the tail call to endDisplay.
int32_t playMpeg(ptr32<sceMpeg> mp, ptr32<ee_uint128> bs, int32_t size)
{
    int32_t end;
    {
        GuestFrame frame(0xa0);
        store<int32_t>(kDrawid, 1);
        displaySetUp();
        guest_call(kMpegAddBs, mp, bs, size);
        startDisplay(1);
        while ((end = guest_call<int32_t>(kMpegIsEnd, mp)) == 0)
        {
            int32_t got;
            while (store<int32_t>(kRet, got = guest_call<int32_t>(kMpegGetPicture, mp, kRgb32, 0x280)), got < 0)
                guest_call(kPrintf, kPictureFailed);
            if (mp->frameCount == 0)
            {
                setLoadImageTags(uploadChain(0), ptr32<void>(kRgb32), 0, 0, mp->width, mp->height);
                setLoadImageTags(uploadChain(1), ptr32<void>(kRgb32), 0, 0x100, mp->width, mp->height);
            }
            guest_call(kGsSyncPath, 0, 0);
            loadImage(uploadChain(static_cast<uint32_t>(load<int32_t>(kDrawid))));
            syncDisplay(mp->frameCount);
            store<int32_t>(kDrawid, static_cast<int32_t>(flipDisplay()));
            guest_call(kGsPutDispEnv, kDisp);
        }
    }
    endDisplay();
    return end;
}

// Reads \SPLASH\<name> into a new heap block (high or low) through a 0x28000-byte file cache. Returns 1, or 0 with
// *data cleared (or, if the read came up short, freed with free(), not MemBlockFree, leaving the handle).
int32_t readFile(ptr32<char> name, ptr32<ptr32<uint8_t>> data, ptr32<int32_t> size, int32 hi)
{
    GuestFrame frame(0xa0);
    const ptr32<char> path = frame.local<char>(0);
    store<uint32_t>(data.address, 0u);
    guest_call(kSprintf, path, kSplashDir);
    guest_call(kStrcat, path, name);
    guest_call(kFMPrepare, 0x28000);
    const int32_t fd = guest_call<int32_t>(kFMFileOpen, path);
    if (fd < 0)
        guest_call(kPrintf, kCannotOpen, path);
    else
    {
        const int32_t length = guest_call<int32_t>(kFMFileSeek, fd, 0, 2);
        *size = length;
        if (length < 0 || guest_call<int32_t>(kFMFileSeek, fd, 0, 0) < 0)
        {
            guest_call(kPrintf, kSeekFails, path);
            guest_call(kFMFileClose, fd);
        }
        else
        {
            const ptr32<memHandle> handle(data.address);
            if (hi != 0)
                MemBlockAllocHi(handle, static_cast<uint32_t>(*size));
            else
                MemBlockAllocLo(handle, static_cast<uint32_t>(*size));
            if (load<uint32_t>(data.address) == 0u)
            {
                guest_call(kPrintf, kAllocFails, *size);
                guest_call(kFMFileClose, fd);
            }
            else if (guest_call<int32_t>(kFMFileRead, load<uint32_t>(data.address), *size, fd) == *size)
            {
                guest_call(kFMFileClose, fd);
                guest_call(kFMUnPrepare);
                return 1;
            }
            else
            {
                guest_call(kPrintf, kCannotRead, path);
                guest_call(kFMFileClose, fd);
                guest_call(kFree, load<uint32_t>(data.address));
            }
        }
    }
    guest_call(kFMUnPrepare);
    return 0;
}

// Fills the 16x16 background tile with a colour (through its uncached address) and uploads it over the whole frame
// buffer.
void clearBackground(u_int val)
{
    {
        GuestFrame frame(0x20);
        for (uint32_t i = 0; i < 0x100u; ++i)
            store<uint32_t>(((kBackTile & 0x0fffffffu) | 0x20000000u) + i * 4u, val);
        setLoadImageTagsTile(ptr32<u_int>(kBackTileTags), ptr32<void>(kBackTile), 0, 0, 0x280, 0x200);
        guest_call(kGsSyncPath, 0, 0);
    }
    loadImage(ptr32<u_int>(kBackTileTags));
}

void MpegLoadFile(ptr32<char> filename)
{
    int32_t width, height;
    {
        GuestFrame frame(0x60);
        const int32_t current = load<int32_t>(kMpegSwitch);
        const int32_t read = readFile(filename, ptr32<ptr32<uint8_t>>(bufferSlot(current)), ptr32<int32_t>(sizeSlot(current)), 0);
        store<int32_t>(kRet, read);
        if (read == 0)
            return;
        guest_call(kMpegInit);
        clearBackground(0x80000000u);
        guest_call(kMpegCreate, kMpegG, kMpegStore, 0xb4800);
        guest_call(kGsSyncPath, 0, 0);
        store<int32_t>(kDrawid, 1);
        store<int32_t>(kGetit, 0);
        displaySetUp();
        addCurrentStream(kMpegG);
        clearBackground(0x80000000u);
        startDisplay(1);
        guest_call(kGsSyncPath, 0, 0);
        guest_call(kMpegGetPicture, kMpegG, kRgb32, 0x280);
        setLoadImageTags(uploadChain(0), ptr32<void>(kRgb32), 0, kMovieTop, load<int32_t>(kMpegG), load<int32_t>(kMpegG + 4u));
        width = load<int32_t>(kMpegG);
        height = load<int32_t>(kMpegG + 4u);
    }
    setLoadImageTags(uploadChain(1), ptr32<void>(kRgb32), 0, kMovieTop + 0x100, width, height);
}

// The vertical blank handler's work driven by a count: decode on even counts, upload on odd ones.
void MpegDoAFrame(int32 vblankCount)
{
    GuestFrame frame(0x40);
    const uint32_t odd = static_cast<uint32_t>(vblankCount) & 1u;
    if (load<uint32_t>(bufferSlot(load<int32_t>(kMpegSwitch))) == 0u)
        return;
    if (odd == 0u)
    {
        if (guest_call<int32_t>(kMpegIsEnd, kMpegG) != 0)
        {
            guest_call(kMpegReset, kMpegG, 0x340000);
            addCurrentStream(kMpegG);
        }
        store<int32_t>(kGetit, guest_call<int32_t>(kMpegGetPicture, kMpegG, kRgb32, 0x280));
    }
    if (load<int32_t>(kGetit) < 0)
        return;
    guest_call(kGsSyncPath, 1, 0);
    loadImage(uploadChain(odd));
}

void MpegInitSplash(ptr32<char> filename)
{
    {
        GuestFrame frame(0x30);
        const int32_t read = readFile(filename, ptr32<ptr32<uint8_t>>(kSplashBuffer), ptr32<int32_t>(kSplashSize), 0);
        store<int32_t>(kRet, read);
        if (read == 0)
            return;
        guest_call(kMpegInit);
        guest_call(kMpegCreate, kSplashG, kMpegStore, 0xb4800);
        guest_call(kGsSyncPath, 0, 0);
        store<int32_t>(kDrawid, 1);
        store<int32_t>(kGetit, 0);
        displaySetUp();
        guest_call(kMpegAddBs, kSplashG, load<uint32_t>(kSplashBuffer), load<int32_t>(kSplashSize));
    }
    startDisplay(1);
}

// One frame of the splash movie, waited for with sceGsSyncV. Returns the vertical blank counter, advanced by one.
int32 MpegSplashFrame()
{
    GuestFrame frame(0x40);
    if (guest_call<int32_t>(kMpegIsEnd, kSplashG) == 0)
    {
        store<int32_t>(kGetit, guest_call<int32_t>(kMpegGetPicture, kSplashG, kRgb32, 0x280));
        if (load<int32_t>(kSplashG + 8u) == 0)
        {
            setLoadImageTags(uploadChain(0), ptr32<void>(kRgb32), 0, kMovieTop, load<int32_t>(kSplashG), load<int32_t>(kSplashG + 4u));
            setLoadImageTags(uploadChain(1), ptr32<void>(kRgb32), 0, kMovieTop + 0x100, load<int32_t>(kSplashG), load<int32_t>(kSplashG + 4u));
        }
        if (load<int32_t>(kGetit) >= 0)
        {
            guest_call(kGsSyncV, 0);
            guest_call(kGsSyncPath, 0, 0);
            guest_call(kGsSyncV, 0);
            loadImage(uploadChain(load<uint32_t>(kVblankCount) & 1u));
            // Here drawid is stored before the display.
            if (load<int32_t>(kDrawid) != 0)
            {
                store<int32_t>(kDrawid, 0);
                store<uint32_t>(kDispFb, (load<uint32_t>(kDispFb) & 0xfffffe00u) | 0x50u);
            }
            else
            {
                store<int32_t>(kDrawid, 1);
                store<uint32_t>(kDispFb, load<uint32_t>(kDispFb) & 0xfffffe00u);
            }
            guest_call(kGsPutDispEnv, kDisp);
        }
    }
    const int32_t count = load<int32_t>(kVblankCount) + 1;
    store<int32_t>(kVblankCount, count);
    return count;
}

void MpegSplashTERM()
{
    {
        GuestFrame frame(0x10);
        if (load<uint32_t>(kSplashBuffer) == 0u)
            return;
        guest_call(kGsSyncV, 0);
        clearBackground(0x80000000u);
        guest_call(kMpegDelete, kSplashG);
        MemBlockFree(ptr32<memHandle>(kSplashBuffer));
        store<uint32_t>(kSplashBuffer, 0u);
    }
    endDisplay();
}

// Reads a movie into buffer swatch (clearing the screen first if asked); the buffer is 0 if that failed.
void mpegOnInterruptLOAD(ptr32<char> filename, int32 swatch, int32 hi, int32 clear)
{
    GuestFrame frame(0x40);
    if (clear != 0)
        clearBackground(0x80000000u);
    const int32_t read = readFile(filename, ptr32<ptr32<uint8_t>>(bufferSlot(swatch)), ptr32<int32_t>(sizeSlot(swatch)), hi);
    store<int32_t>(kRet, read);
    if (read == 0)
        store<uint32_t>(bufferSlot(swatch), 0u);
}

int32 mpegOnInterruptFINISHED()
{
    return load<int32_t>(kMpegLooping) == -1 ? 1 : 0;
}

// --- MPDISPLAY.c ---

// INTC 2's handler while a movie plays (see the file comment). Returns 0.
int32_t vblankPlayer(int32_t val)
{
    (void)val;
    GuestFrame frame(0x50);
    const int32_t count = load<int32_t>(kVblankCount);
    if (load<uint32_t>(bufferSlot(load<int32_t>(kMpegSwitch))) != 0u && load<int32_t>(kMpegLooping) != -1)
    {
        if ((count & 1) == 0)
        {
            if (guest_call<int32_t>(kMpegIsEnd, kMpegG) != 0)
            {
                guest_call(kMpegReset, kMpegG, 0x340000);
                addCurrentStream(kMpegG);
                if (load<int32_t>(kMpegLooping) != 1)
                {
                    const int32_t next = load<int32_t>(kVblankCount) + 1;
                    enableInterrupts();
                    store<int32_t>(kMpegLooping, -1);
                    store<int32_t>(kVblankCount, next);
                    return 0;
                }
            }
            store<int32_t>(kGetit, guest_call<int32_t>(kMpegGetPicture, kMpegG, kRgb32, 0x280));
        }
        else
        {
            if (load<int32_t>(kGetit) < 0)
            {
                enableInterrupts();
                store<int32_t>(kVblankCount, count + 1);
                return 0;
            }
            if (guest_call<int32_t>(kGsSyncPath, 1, 0) == 0)
            {
                loadImage(uploadChain(static_cast<uint32_t>(load<int32_t>(kDrawid))));
                const uint32_t drawid = flipDisplay();
                store<int32_t>(kDrawid, static_cast<int32_t>(drawid));
                guest_call(kGsPutDispEnv, kDisp, 0x200000, drawid);
            }
        }
    }
    const int32_t next = load<int32_t>(kVblankCount) + 1;
    enableInterrupts();
    store<int32_t>(kVblankCount, next);
    return 0;
}

// The splash movie's handler: decodes every 64th vertical blank, uploads 15 later; at the end it removes itself.
int32_t vblankSplashPlayer(int32_t val)
{
    (void)val;
    GuestFrame frame(0x20);
    const int32_t count = load<int32_t>(kVblankCount);
    const uint32_t phase = static_cast<uint32_t>(count) & 0x3fu;
    if (phase == 0u)
    {
        if (guest_call<int32_t>(kMpegIsEnd, kMpegG) != 0)
        {
            guest_call(kMpegReset, kMpegG, 0x340000);
            guest_call(kDisableIntc, 2);
            guest_call(kRemoveIntcHandler, 2, load<int32_t>(kVblankHandle));
            enableInterrupts();
            store<int32_t>(kVblankHandle, 0);
            return 0;
        }
        store<int32_t>(kGetit, guest_call<int32_t>(kMpegGetPicture, kMpegG, kRgb32, 0x280));
    }
    else
    {
        if (phase != 0xfu || load<int32_t>(kGetit) < 0)
        {
            enableInterrupts();
            store<int32_t>(kVblankCount, count + 1);
            return 0;
        }
        guest_call(kGsSyncPath, 1, 0);
        loadImage(uploadChain(static_cast<uint32_t>(load<int32_t>(kDrawid))));
        const uint32_t drawid = flipDisplay();
        store<int32_t>(kDrawid, static_cast<int32_t>(drawid));
        guest_call(kGsPutDispEnv, kDisp, 0x200000, drawid);
    }
    const int32_t next = load<int32_t>(kVblankCount) + 1;
    enableInterrupts();
    store<int32_t>(kVblankCount, next);
    return 0;
}

int32_t vblankHandler(int32_t val)
{
    (void)val;
    if (load<int32_t>(kIsCountVblank) != 0)
        store<int32_t>(kVblankCount, load<int32_t>(kVblankCount) + 1);
    enableInterrupts();
    return 0;
}

// Waits for a vertical blank whose sceGsSyncV answer differs from waitEven, then starts counting them.
void startDisplay(int32_t waitEven)
{
    {
        GuestFrame frame(0x20);
        while (guest_call<int32_t>(kGsSyncV, 0) == waitEven)
        {
        }
    }
    store<int32_t>(kVblankCount, 1);
    store<int32_t>(kIsCountVblank, 1);
}

void endDisplay()
{
    store<int32_t>(kIsCountVblank, 0);
}

// Reports a dropped frame (and resets the counter to it), then waits until the counter reaches the frame. The counter
// is read once, so the wait never ends if it has to wait at all.
void syncDisplay(int32_t frame)
{
    GuestFrame guestFrame(0x20);
    int32_t count = load<int32_t>(kVblankCount);
    if (frame < (count >> 1))
    {
        guest_call(kPrintf, kFrameDrop, frame);
        count = static_cast<int32_t>(static_cast<uint32_t>(frame) << 1);
        store<int32_t>(kVblankCount, count);
    }
    if ((count >> 1) < frame)
        for (;;)
            guest_checkpoint();
}

} // namespace rc

RC_DECOMP_FN_BLOCKING_SETUP_HEAP(playMpegFile, rc::fuzz::mpegFile)
RC_DECOMP_FN_BLOCKING_SETUP(mpegOnInterruptPLAY, rc::fuzz::mpegPlay)
RC_DECOMP_FN_BLOCKING_SETUP_HEAP(mpegOnInterruptTERM, rc::fuzz::mpegTerm)
RC_DECOMP_FN_BLOCKING_SETUP(playMpeg, rc::fuzz::mpegPlayMpeg)
RC_DECOMP_FN_BLOCKING_SETUP_HEAP(readFile, rc::fuzz::mpegReadFile)
RC_DECOMP_FN_BLOCKING_SETUP(clearBackground, rc::fuzz::mpegClear)
RC_DECOMP_FN_BLOCKING_SETUP_HEAP(MpegLoadFile, rc::fuzz::mpegFile)
RC_DECOMP_FN_BLOCKING_SETUP(MpegDoAFrame, rc::fuzz::mpegFrame)
RC_DECOMP_FN_BLOCKING_SETUP_HEAP(MpegInitSplash, rc::fuzz::mpegFile)
RC_DECOMP_FN_BLOCKING_SETUP(MpegSplashFrame, rc::fuzz::mpegState)
RC_DECOMP_FN_BLOCKING_SETUP_HEAP(MpegSplashTERM, rc::fuzz::mpegSplashTerm)
RC_DECOMP_FN_BLOCKING_SETUP_HEAP(mpegOnInterruptLOAD, rc::fuzz::mpegLoad)
RC_DECOMP_FN_SETUP(mpegOnInterruptFINISHED, rc::fuzz::mpegState)
RC_DECOMP_FN_BLOCKING_SETUP(vblankPlayer, rc::fuzz::mpegState)
RC_DECOMP_FN_BLOCKING_SETUP(vblankSplashPlayer, rc::fuzz::mpegState)
RC_DECOMP_FN_SETUP(vblankHandler, rc::fuzz::mpegState)
RC_DECOMP_FN_BLOCKING_SETUP(startDisplay, rc::fuzz::mpegDisplay)
RC_DECOMP_FN_SETUP(endDisplay, rc::fuzz::mpegState)
RC_DECOMP_FN_SETUP(syncDisplay, rc::fuzz::mpegSync)

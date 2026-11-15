// Decompiled from HUDSYS.c: the race HUD. HudTask runs every 40 ms (the flash counter, the ready-steady-go count and
// the slide-in); HudPanel draws a player's panel each frame: position and lap times (top right), the speedometer
// (bottom right), the map with a blip per car (bottom left), the wrong-way and lost signs and the weapon held
// (middle). The sprites go straight into GFX_P2's packet buffer as two triangles (six GIF "packed" vertices: texture
// coordinates, Q, colour, position, depth), every third vertex kicking a triangle, as SPRITE.c does.
//
// Written from the instructions (Ghidra's C was used for the sprite functions' vertex order, checked against them).

#include "hud_fuzz.h"
#include "weapon.h"

namespace rc {

void ResTypeSetConstructDaemon(ptr32<char> pType, uint32 constructDaemon, int16 options); // res.cpp
void ResTypeUnSetConstructDaemon(ptr32<char> pType);                                       // res.cpp
void FontSet(ptr32<font> pFont);                                                           // font.cpp
void FontSetColour(uint8 red, uint8 green, uint8 blue);                                    // font.cpp
ptr32<char> FontPrint(int16 x, int16 y, ptr32<char> pString);                              // font.cpp
void FontCentre(int16 y, ptr32<char> pString);                                             // font.cpp
void GfxP2_TexFX(ptr32<_sGfxTexture> pTex, u32 attrib);                                    // rdr.cpp
void GfxP2_ZWriteEnable();                                                                 // rdr.cpp
float sinf(ee_float x);                                                                    // libm_float.cpp
void VfxPS2SpriteRotated3D(ee_float zOffset);                                              // visfx.cpp
ptr32<_sSingleCamera> GetCameraFromTag(uint32 tag, ptr32<_sCamData> pCameras);             // path.cpp
void sceVu0ApplyMatrix(ptr32<ee_float> dst, ptr32<ee_float> m, ptr32<ee_float> v);         // vifpk.cpp

void HudSprite(ptr32<_sSprite> pSpr, ee_float fX, ee_float fY, uint32 rgb);
void HudSpriteMirror(ptr32<_sSprite> pSpr, ee_float fX, ee_float fY, uint32 rgb);
void HudSpriteHI(ptr32<_sSprite> pSpr, ee_float fX, ee_float fY, uint32 rgb, ee_float otz);
void HudRotSpriteHI(ptr32<_sSprite> pSpr, ee_float fX, ee_float fY, uint32 rgb, ee_float otz, ee_float ang);
void HudWrongWayX(int32 iX, int32 iY, int32 iD);
void HudLost(int32 iX, int32 iY, int32 iD);
void HudFormatNumFont(ptr32<char> pString, int32 lapTime);
void HudTopLeft(ptr32<_sObject> pObj, int8 size, int8 player);
void HudTopRight(ptr32<_sObject> pObj, int8 size, int8 player);
void HudBottomRight(ptr32<_sObject> pObj, int8 size, int8 player);
void HudBottomLeft(ptr32<_sObject> pObj, int8 size, int8 player);
void HudMiddle(ptr32<_sObject> pObj, int8 size, int8 player);

namespace {

using weapon::at;

constexpr uint32_t kHud = 0x1ecda0u; // hudVarG
constexpr uint32_t kHudFlash = kHud + 0x1eu, kHudPress = kHud + 0x1fu, kHudSlide = kHud + 0x1cu;
constexpr uint32_t kMainVars = 0x1ed900u, kMainTimer = kMainVars + 0x10u, kMainRaceType = kMainVars + 0x34cu;
constexpr uint32_t kMainLevel = kMainVars + 0x34au, kMainPauseMenu = kMainVars + 0x366u, kMainSplit = kMainVars + 0x368u;
constexpr uint32_t kMainTTBeaten = kMainVars + 0x36bu, kMainWhichSplit = kMainVars + 0x378u, kMainMirror = kMainVars + 0x398u;
constexpr uint32_t kLevelPtr = 0x327e80u, kFontPtr = 0x328190u, kBuff = 0x1f3980u, kFrontEnd = 0x1eca58u;
constexpr uint32_t kCameraPtr = 0x1eab20u, kCameraRSG = 0x1eab24u, kObjectList = 0x1ee390u, kNoObjects = 0x1ee500u;
constexpr uint32_t kAINode = 0x1e9478u, kVsyncInc = 0x1f3c34u, kVfxSprite = 0x1f3720u, kVfxRes = 0x1f106cu;
constexpr uint32_t kSprintf = 0x1b8910u, kTimerGet = 0x1b4188u, kReadyGoFade = 0x144178u, kInitialReverb = 0x1439a8u;
constexpr uint32_t kAMenuDisplayPsxKey = 0x10eea0u, kTEditBestLap = 0x154688u, kResLoad = 0x1ab1b0u, kHudTask = 0x124490u;
constexpr uint32_t kTimeTrial = 0x100001feu;

ee_float constant(uint32_t address) { return at<ee_float>(address); }
int16_t slide() { return at<int16_t>(kHudSlide); }
uint32_t hudRes() { return at<uint32_t>(at<uint32_t>(kLevelPtr) + 0x40u); }

// GIF packed data.
constexpr uint64_t kST01 = 0x3f80000000000000ull, kST11 = 0x3f8000003f800000ull, kST00 = 0, kST10 = 0x3f800000ull;
constexpr uint64_t kOne = 0x3f800000u, kAdc = 0x800000000000ull, kFarZ = 0xffffffu;

// Appends vertices to the open GIF tag of the GS packet buffer, opening one (ST, RGBAQ, XYZ2) if there is none.
class Strip
{
public:
    Strip() : m_gfx(G(gGfxP2)), m_buffer(m_gfx.pDmaBuff), m_tag(m_gfx.gifTagIdx) {}

    void vertex(uint64_t st, uint64_t rgbaLow, uint64_t rgbaHigh, uint64_t xy, uint64_t z)
    {
        if (m_tag == -1)
        {
            uint64_t *tag = quadword(m_gfx.buffIdx);
            tag[0] = 0x3000000000000000ull;
            tag[1] = 0x512;
            m_tag = m_gfx.gifTagIdx = m_gfx.buffIdx;
            m_gfx.buffIdx = m_gfx.buffIdx + 1;
        }
        uint16_t &count = *reinterpret_cast<uint16_t *>(quadword(m_tag));
        const uint16_t n = static_cast<uint16_t>(count + 1);
        count = n;
        uint64_t *q = quadword(m_gfx.buffIdx);
        q[0] = st;
        q[1] = kOne;
        q[2] = rgbaLow;
        q[3] = rgbaHigh;
        q[4] = xy;
        q[5] = n % 3u != 0 ? z | kAdc : z;
        m_gfx.buffIdx = m_gfx.buffIdx + 3;
    }

private:
    uint64_t *quadword(int32_t index) { return reinterpret_cast<uint64_t *>((m_buffer + index).get()); }

    sGfxP2 &m_gfx;
    ptr32<u128> m_buffer;
    int32_t m_tag;
};

struct Colour
{
    uint64_t low, high;
};

Colour colour(uint32_t r, uint32_t g, uint32_t b) { return {r | (static_cast<uint64_t>(g) << 32), b | 0x8000000000ull}; }
Colour colour(uint32_t rgb) { return colour(rgb & 0xffu, (rgb >> 8) & 0xffu, (rgb >> 16) & 0xffu); }

// A GS coordinate (12.4 fixed point) of a float or an integer.
uint64_t coord(ee_float value) { return static_cast<uint64_t>(ee_ftoi(value * ee_float(16.0f))) & 0xffffu; }
uint64_t coord(int32_t value) { return coord(ee_itof(value)); }
uint64_t xy(uint64_t x, uint64_t y) { return x | (y << 32); }

// The depth of an ordering value: (1 - otz) times the function's scale.
uint64_t depth(ee_float otz, uint32_t scaleAddress)
{
    return static_cast<uint64_t>(static_cast<int64_t>(ee_ftoi((ee_float(1.0f) - otz) * constant(scaleAddress))));
}

// The two triangles of a quad: A B C, B C D, with A at texture (0, 1) and D at (1, 0).
void quad(uint64_t a, uint64_t b, uint64_t c, uint64_t d, const Colour (&col)[4], uint64_t z)
{
    Strip strip;
    strip.vertex(kST01, col[0].low, col[0].high, a, z);
    strip.vertex(kST11, col[1].low, col[1].high, b, z);
    strip.vertex(kST00, col[2].low, col[2].high, c, z);
    strip.vertex(kST11, col[1].low, col[1].high, b, z);
    strip.vertex(kST00, col[2].low, col[2].high, c, z);
    strip.vertex(kST10, col[3].low, col[3].high, d, z);
}

void quad(uint64_t a, uint64_t b, uint64_t c, uint64_t d, Colour col, uint64_t z)
{
    const Colour all[4] = {col, col, col, col};
    quad(a, b, c, d, all, z);
}

void print(int32_t x, int32_t y, uint32_t text)
{
    FontPrint(static_cast<int16>(x), static_cast<int16>(y), ptr32<char>(text));
}

void formatTime(uint32_t buffer, int32_t lapTime)
{
    int32_t minutes = ee_div(lapTime, 60000), seconds = ee_div(lapTime, 1000) % 60, tenths = ee_div(lapTime, 100) % 10;
    if (minutes >= 100)
    {
        minutes = 99;
        seconds = 59;
        tenths = 99;
    }
    guest_call(kSprintf, buffer, 0x322e10u, minutes, seconds, tenths); // "%02d:%02d.%1d"
}

} // namespace

void HudSetupClass() {}

void HudInit()
{
    {
        GuestFrame frame(0x10);
        ResTypeSetConstructDaemon(ptr32<char>(0x327df0u), 0, 4);
    }
    ResTypeSetConstructDaemon(ptr32<char>(0x327df8u), 0, 4);
}

void HudTerm()
{
    ResTypeUnSetConstructDaemon(ptr32<char>(0x327df0u));
}

void HudDelete()
{
    guest_call(weapon::kTaskDelete, kHud);
}

// The HUD's task: the flash counter, the slide-in, the ready-steady-go count (a light each second from the moment
// the camera allows it), the press-to-continue countdown once the race is over.
void HudTask(ptr32<void>, tTime time)
{
    {
        GuestFrame frame(0x40);
        at<uint8_t>(kHudFlash) = static_cast<uint8_t>(at<uint8_t>(kHudFlash) + 1);
        const int32_t rsg = at<int32_t>(kHud + 0x70u);
        if (rsg != 0)
        {
            if (rsg < 4)
            {
                const int32_t t = at<int32_t>(kHud + 0x18u) * 7;
                at<int16_t>(kHudSlide) = static_cast<int16_t>(t >> 8);
                at<int32_t>(kHud + 0x18u) = t >> 3;
            }
            if (at<int32_t>(kHud + 0x74u) == 0)
            {
                if (at<int32_t>(kHud + 0x78u) != 0)
                {
                    at<uint32_t>(kHud + 0x74u) = guest_call<uint32_t>(kTimerGet, kMainTimer);
                    guest_call(kReadyGoFade, -10, 30);
                    guest_call(kInitialReverb, static_cast<uint32_t>(at<uint8_t>(kMainSplit)));
                }
            }
            else
            {
                const uint32_t now = guest_call<uint32_t>(kTimerGet, kMainTimer);
                const int32_t count = 5 - ee_div(static_cast<int32_t>(now - at<uint32_t>(kHud + 0x74u)), 1000);
                if (at<int32_t>(kHud + 0x70u) != count)
                {
                    at<int32_t>(kHud + 0x70u) = count;
                    if (count > 0 && at<uint32_t>(kCameraRSG) != 0)
                        VfxAddNew(ptr32<_sObject>(at<uint32_t>(at<uint32_t>(kCameraPtr) + 0xacu)), 4 - count, 0, 0x1f);
                }
            }
        }
        if (at<int32_t>(kMainVars) == 0x20 && at<uint8_t>(kHudPress) != 0)
            at<uint8_t>(kHudPress) = static_cast<uint8_t>(at<uint8_t>(kHudPress) - 1);
        at<int32_t>(kHud + 4u) = time + 40;
    }
    guest_call(weapon::kTaskCachedInsert, kHud); // a tail call
}

void HudCreate()
{
    GuestFrame frame(0x20);
    guest_call(weapon::kTaskSet, kHud, kHudTask, 1, 0);
    guest_call(weapon::kTaskCachedInsert, kHud);
    at<int32_t>(kHud + 0x18u) = 0x4000;
    at<int16_t>(kHudSlide) = 0x200;
    at<int32_t>(kHud + 0x70u) = at<int32_t>(kMainVars) == 0x10 ? 0 : 5;
    at<uint8_t>(kHudPress) = 0x78;
    at<int32_t>(kHud + 0x74u) = 0;
    at<int32_t>(kHud + 0x78u) = 0;
    if (at<uint8_t>(kMainSplit) != 0)
    {
        at<int32_t>(kHud + 0x8cu) = 0x1c;
        at<int32_t>(kHud + 0x90u) = 200;
        at<int32_t>(kHud + 0x88u) = 0x2c;
        at<int32_t>(kHud + 0x84u) = 0x2c;
    }
    else
    {
        at<int32_t>(kHud + 0x8cu) = 0x1c;
        at<int32_t>(kHud + 0x84u) = 0x140;
    }
}

void HudFormatLapTime(ptr32<char> pString, int32 lapTime) { formatTime(pString.address, lapTime); }
void HudFormatNumFont(ptr32<char> pString, int32 lapTime) { formatTime(pString.address, lapTime); }

void HudTopLeft(ptr32<_sObject>, int8, int8)
{
    FontSet(ptr32<font>(at<uint32_t>(hudRes() + 0x4cu)));
}

// A sprite at (fX, fY): width doubled, height to 127, at the far depth.
void HudSprite(ptr32<_sSprite> pSpr, ee_float fX, ee_float fY, uint32 rgb)
{
    const uint32_t width = (static_cast<uint32_t>(pSpr->width) << 1) & 0xffu, height = pSpr->height & 0x7fu;
    const ee_float x = fX + ee_float(1728.0f), y = fY + ee_float(1928.0f);
    GfxP2_TexFX(pSpr->pTextureHandle, rgb >> 24);
    const uint64_t x0 = coord(x), y0 = coord(y);
    const uint64_t x1 = coord(x + ee_itof(static_cast<int32_t>(width))), y1 = coord(y + ee_itof(static_cast<int32_t>(height)));
    quad(xy(x0, y0), xy(x1, y0), xy(x0, y1), xy(x1, y1), colour(rgb), kFarZ);
}

// As HudSprite, extending to the left (the mirrored map).
void HudSpriteMirror(ptr32<_sSprite> pSpr, ee_float fX, ee_float fY, uint32 rgb)
{
    const uint32_t width = (static_cast<uint32_t>(pSpr->width) << 1) & 0xffu, height = pSpr->height & 0x7fu;
    const ee_float x = fX + ee_float(1728.0f), y = fY + ee_float(1928.0f);
    GfxP2_TexFX(pSpr->pTextureHandle, rgb >> 24);
    const uint64_t x0 = coord(x), y0 = coord(y);
    const uint64_t x1 = coord(x - ee_itof(static_cast<int32_t>(width))), y1 = coord(y + ee_itof(static_cast<int32_t>(height)));
    quad(xy(x0, y0), xy(x1, y0), xy(x0, y1), xy(x1, y1), colour(rgb), kFarZ);
}

// A sprite at full width and half height, at a depth.
void HudSpriteHI(ptr32<_sSprite> pSpr, ee_float fX, ee_float fY, uint32 rgb, ee_float otz)
{
    const uint32_t height = pSpr->height, width = pSpr->width;
    const ee_float x = fX + ee_float(1728.0f);
    GfxP2_TexFX(pSpr->pTextureHandle, rgb >> 24);
    const uint64_t z = depth(otz, 0x32765cu);
    const ee_float y = fY + ee_float(1928.0f);
    const uint64_t x0 = coord(x), y0 = coord(y);
    const uint64_t x1 = coord(x + ee_itof(static_cast<int32_t>(width))), y1 = coord(y + ee_itof(static_cast<int32_t>(height >> 1)));
    quad(xy(x0, y0), xy(x1, y0), xy(x0, y1), xy(x1, y1), colour(rgb), z);
}

// HudSpriteHI turned by ang (radians, at least -3.2) about its centre, half height: the speedometer's needle mask.
// The corner vectors' z and w lanes are never set (stale stack): the rotation matrix's zeros multiply them.
void HudRotSpriteHI(ptr32<_sSprite> pSpr, ee_float fX, ee_float fY, uint32 rgb, ee_float otz, ee_float ang)
{
    GuestFrame frame(0x160);
    const uint32_t sp = frame.local<uint8_t>(0).address;
    const ee_float minimum = at<ee_float>(0x327660u);
    const ee_float half(0.5f);
    const ee_float h = ee_itof(pSpr->height) * half;
    const ee_float w = ee_itof(pSpr->width) * half;
    const ee_float quarter = h * half;
    if (ang < minimum)
        ang = minimum;
    const uint32_t mat = sp + 0x40u;
    guest_call(0x1c7ac8u, mat);                  // sceVu0UnitMatrix
    guest_call(0x1c7b68u, mat, mat, ang);        // sceVu0RotMatrixZ
    const ee_float x = fX + ee_float(1728.0f), y = fY + ee_float(1928.0f);
    at<ee_float>(sp + 0x34u) = h;
    at<ee_float>(sp + 0x20u) = -w;
    at<ee_float>(sp + 0x14u) = -h;
    at<ee_float>(sp + 0x00u) = -w;
    at<ee_float>(sp + 0x04u) = -h;
    at<ee_float>(sp + 0x10u) = w;
    at<ee_float>(sp + 0x24u) = h;
    at<ee_float>(sp + 0x30u) = w;
    for (uint32_t i = 0; i < 4; ++i)
        sceVu0ApplyMatrix(ptr32<ee_float>(sp + i * 16u), ptr32<ee_float>(mat), ptr32<ee_float>(sp + i * 16u));
    const ee_float cx = x + w;
    for (uint32_t i = 0; i < 4; ++i)
    {
        const uint32_t v = sp + i * 16u;
        at<ee_float>(v + 4u) = at<ee_float>(v + 4u) * half + y + quarter;
        at<ee_float>(v) = at<ee_float>(v) + cx;
    }
    GfxP2_TexFX(pSpr->pTextureHandle, rgb >> 24);
    const uint64_t z = depth(otz, 0x327664u);
    uint64_t corner[4];
    for (uint32_t i = 0; i < 4; ++i)
        corner[i] = xy(coord(at<ee_float>(sp + i * 16u)), coord(at<ee_float>(sp + i * 16u + 4u)));
    quad(corner[0], corner[1], corner[2], corner[3], colour(rgb), z);
}

// The wrong-way cross: a diamond of the level's cross texture about (iX, iY), half-height iD.
void HudWrongWayX(int32 iX, int32 iY, int32 iD)
{
    const int32_t x = iX + 0x6c0, y = iY + 0x788;
    GfxP2_TexFX(ptr32<_sGfxTexture>(at<uint32_t>(hudRes() + 0xd0u)), 0);
    const Colour grey{0x8000000080ull, 0x8000000080ull};
    quad(xy(coord(x), coord(y + iD)), xy(coord(x - 2 * iD), coord(y)), xy(coord(x + 2 * iD), coord(y)),
         xy(coord(x), coord(y - iD)), grey, kFarZ);
}

// The "lost" sign: the level's lost texture over (iX, iY) +- (2 iD, iD).
void HudLost(int32 iX, int32 iY, int32 iD)
{
    const int32_t x = iX + 0x6c0, y = iY + 0x788;
    GfxP2_TexFX(ptr32<_sGfxTexture>(at<uint32_t>(hudRes() + 0xd8u)), 0x20);
    const Colour grey{0x8000000080ull, 0x8000000080ull};
    quad(xy(coord(x - 2 * iD), coord(y - iD)), xy(coord(x + 2 * iD), coord(y - iD)), xy(coord(x - 2 * iD), coord(y + iD)),
         xy(coord(x + 2 * iD), coord(y + iD)), grey, kFarZ);
}

// A menu border sprite whose corners shimmer: each corner's red and green (equal) and blue follow a sine of its
// position and the frame count.
void HudMenuBorder(ptr32<_sSprite> pSpr, int32 iCX, int32 iCY, int32)
{
    GuestFrame frame(0xd0);
    const uint32_t sp = frame.local<uint8_t>(0).address;
    const int32_t phase = static_cast<int32_t>(0u - at<uint32_t>(kVsyncInc)) << 1;
    const int32_t x = iCX + 0x6c0, y = iCY + 0x788;
    const ee_float scale = at<ee_float>(0x327668u);
    const uint32_t height = pSpr->height, width = pSpr->width;
    const uint32_t halfHeight = height >> 1;
    const auto shade = [&](int32_t sum, int32_t blueAdd, int32_t redAdd, uint32_t k)
    {
        const int32_t arg = (static_cast<int32_t>(static_cast<uint32_t>(sum) << 3) & 0xfff) - 0x800;
        const int32_t s = ee_ftoi(ee_float(sinf(ee_itof(arg) * scale)) * ee_float(100.0f));
        int32_t red = s + redAdd, blue = s + blueAdd;
        if (red < 0x50)
            red = 0x50;
        if (red >= 0x100)
            red = 0xff;
        if (blue < 0x3c)
            blue = 0x3c;
        if (blue >= 0x100)
            blue = 0xff;
        at<int32_t>(sp + k * 4u) = red;
        at<int32_t>(sp + 0x10u + k * 4u) = blue;
    };
    shade(phase + x + y, 10, 0x28, 0);
    shade(phase + static_cast<int32_t>(width) + (y + x), 0x14, 0x32, 1);
    const int32_t bottom = phase + static_cast<int32_t>(halfHeight) + (y + x);
    shade(bottom, 10, 0x28, 2);
    shade(bottom + static_cast<int32_t>(width), 10, 0x28, 3);
    GfxP2_TexFX(pSpr->pTextureHandle, 0x20);
    Colour col[4];
    for (uint32_t k = 0; k < 4; ++k)
    {
        const uint32_t red = at<uint8_t>(sp + k * 4u), blue = at<uint8_t>(sp + 0x10u + k * 4u);
        col[k] = colour(red, red, blue);
    }
    const uint64_t z = static_cast<uint64_t>(static_cast<int64_t>(
        ee_ftoi((ee_float(1.0f) - at<ee_float>(0x32766cu)) * at<ee_float>(0x327670u))));
    const uint64_t x0 = coord(x), y0 = coord(y);
    const uint64_t x1 = coord(x + static_cast<int32_t>(width)), y1 = coord(y + static_cast<int32_t>(halfHeight));
    quad(xy(x0, y0), xy(x1, y0), xy(x0, y1), xy(x1, y1), col, z);
}

// Position (unless a time trial), lap and lap time, best lap, and in a time trial the target time and whether it
// has been beaten. The second player's panel is 0x70 lower.
void HudTopRight(ptr32<_sObject> pObj, int8, int8 player)
{
    const uint32_t car = pObj.address;
    const int32_t down = player == 0 ? 0 : 0x70;
    const uint32_t res = hudRes();
    {
        GuestFrame frame(0x90);
        FontSet(ptr32<font>(at<uint32_t>(res + 0x4cu)));
        FontSetColour(0x7f, 0x7f, 0x7f);
        int32_t lift = 0x20;
        int32_t laps;
        if (at<uint32_t>(kMainRaceType) == kTimeTrial)
            laps = at<int16_t>(car + 0x104u);
        else
        {
            if ((at<uint8_t>(kHudFlash) & 7) == 0)
                at<uint16_t>(car + 0x13eu) = at<uint16_t>(car + 0x13cu);
            const int32_t position = at<int32_t>(car + 0xe8u) != 0 ? at<int16_t>(car + 0x13cu) : at<int16_t>(car + 0x13eu);
            lift = 0;
            HudSprite(ptr32<_sSprite>(at<uint32_t>(res + 0x24u + static_cast<uint32_t>(position) * 4u)),
                      ee_itof(slide() + 0x228), ee_itof(down - (slide() - 8)), 0x2808080);
            laps = at<int16_t>(car + 0x104u);
        }
        const auto row = [&](int32_t offset, uint32_t text)
        {
            const uint16_t s = at<uint16_t>(kHudSlide);
            print(s + 0x212, static_cast<int32_t>(static_cast<uint32_t>(down + offset) - s - static_cast<uint32_t>(lift)), text);
        };
        if (laps >= 0 && at<int32_t>(car + 0xe8u) == 0)
        {
            guest_call(kSprintf, kBuff, 0x327e00u, laps + 1); // "Lap %d"
            row(0x30, kBuff);
            HudFormatNumFont(ptr32<char>(kBuff), at<int32_t>(car + 0x140u));
            row(0x3a, kBuff);
        }
        guest_call(kSprintf, kBuff, 0x322e20u); // "Best lap"
        row(0x46, kBuff);
        const int32_t best = at<int32_t>(car + 0x16cu);
        if (best == 0x10000000)
            row(0x50, 0x327e08u); // "--:--.-"
        else
        {
            HudFormatNumFont(ptr32<char>(kBuff), best);
            row(0x50, kBuff);
        }
        if (at<uint32_t>(kMainRaceType) == kTimeTrial)
        {
            guest_call(kSprintf, kBuff, 0x327e10u); // "Target"
            row(0x5c, kBuff);
            int32_t target;
            if (at<uint32_t>(kFrontEnd + 0x29cu) == 0x1000006bu)
                target = guest_call<int32_t>(kTEditBestLap);
            else
                target = at<int32_t>(kFrontEnd + 0x19cu + static_cast<uint32_t>((at<uint8_t>(kMainLevel) - 1) % 0x30) * 4u);
            HudFormatNumFont(ptr32<char>(kBuff), target);
            const uint16_t s = at<uint16_t>(kHudSlide);
            print(s + 0x212, static_cast<int32_t>(0x66u - static_cast<uint32_t>(lift) - s), kBuff);
            const int32_t key = at<uint8_t>(kMainTTBeaten) != 0 ? 0xd : 8;
            guest_call(kAMenuDisplayPsxKey, slide() + 0x1f4, down - (slide() - 0x5c) - lift, key);
        }
    }
    FontSet(ptr32<font>(at<uint32_t>(res + 0x4cu))); // a tail call
}

// The speedometer (its needle mask turned by the speed, the dial, the glass) and the speed in two digits; a split
// screen gets only the digits.
void HudBottomRight(ptr32<_sObject> pObj, int8 size, int8 player)
{
    const uint32_t res = hudRes();
    int32_t y = slide() + 0x98;
    uint32_t speedSpr0, speedSpr1;
    ee_float digitsY, x0, x1;
    uint32_t rgb;
    {
        GuestFrame frame(0x70);
        if (size == 2)
        {
            const int32_t half = y / 2;
            y = player == 1 ? half + 0x68 : half - 0x10;
            if (player != 0)
                y -= 8;
        }
        else
        {
            GfxP2_ZWriteEnable();
            ee_float needle = at<ee_float>(at<uint32_t>(pObj.address + 0x20u) + 0x38u) * constant(0x327638u);
            const ee_float most = constant(0x32763cu);
            if (most < needle)
                needle = most;
            const ee_float fy = ee_itof(y);
            HudRotSpriteHI(ptr32<_sSprite>(at<uint32_t>(res + 0x48u)), ee_itof(slide() + 0x1e4), fy, 0x2000000, constant(0x327640u), -needle);
            HudSpriteHI(ptr32<_sSprite>(at<uint32_t>(res + 0x48u)), ee_itof(slide() + 0x1e4), fy, 0x808080, constant(0x327644u));
            HudSpriteHI(ptr32<_sSprite>(at<uint32_t>(res + 0x44u)), ee_itof(slide() + 0x1e4), fy, 0x20808080, constant(0x327648u));
        }
        const int32_t speed = ee_ftoi(at<ee_float>(at<uint32_t>(pObj.address + 0x20u) + 0x38u) * ee_float(float_from_bits(0x3b6a0000u)));
        const int32_t s = static_cast<int16_t>(at<uint16_t>(kHudSlide));
        const bool split = size == 2;
        digitsY = ee_itof(y + (split ? 0x20 : 0x18));
        rgb = split ? 0x2404040u : 0x4404040u;
        x0 = ee_itof(s + (split ? 0x234 : 0x214));
        speedSpr0 = at<uint32_t>(res + 0xa0u + static_cast<uint32_t>(speed / 10) * 4u);
        HudSpriteHI(ptr32<_sSprite>(speedSpr0), x0, digitsY, rgb, ee_float(0.0f));
        speedSpr1 = at<uint32_t>(res + 0xa0u + static_cast<uint32_t>(speed % 10) * 4u);
        x1 = ee_itof(slide() + (split ? 0x244 : 0x224));
    }
    HudSpriteHI(ptr32<_sSprite>(speedSpr1), x1, digitsY, rgb, ee_float(0.0f)); // a tail call
}

// The map (mirrored in mirror mode) with a blip per car: the computer's cars green, others white, the players
// flashing by their state at +0x48. The level's bounds scale the positions (level 'a' has fixed ones).
void HudBottomLeft(ptr32<_sObject>, int8 size, int8 player)
{
    const uint32_t res = hudRes();
    const int32_t dx = at<int32_t>(kAINode + 0x1cu) - at<int32_t>(kAINode + 0x14u);
    const int32_t dz = at<int32_t>(kAINode + 0x20u) - at<int32_t>(kAINode + 0x18u);
    int32_t scale = (dz < dx ? dx : dz) >> 9;
    if (player != 0 || at<uint32_t>(res + 8u) == 0)
        return;
    const uint32_t map = at<uint32_t>(res + 8u);
    int32_t y0 = size == 2 ? 0x98 : 0xd8;
    int32_t x0 = at<int32_t>(kMainMirror) != 0 ? static_cast<int32_t>(at<uint8_t>(map + 6u)) * 2 + 0x1a : 0x1a;
    if (at<uint8_t>(kMainLevel) == 0x61)
    {
        y0 = size == 2 ? 0x98 : 0xd8;
        at<int32_t>(kAINode + 0x18u) = -0x3e00;
        at<int32_t>(kAINode + 0x14u) = -0x3e00;
        x0 = 1;
        scale = 0x3c;
    }
    {
        GuestFrame frame(0xb0);
        const ee_float fx0 = ee_itof(x0), fy0 = ee_itof(y0);
        for (uint32_t i = 0; i < at<uint32_t>(kNoObjects); ++i)
        {
            const uint32_t car = at<uint32_t>(kObjectList + i * 4u);
            uint32_t rgb;
            if (at<uint8_t>(car + 0x44u) == 2)
                rgb = 0xd000;
            else if ((at<uint8_t>(kHudFlash) & 8) == 0)
                rgb = 0xe0e0;
            else
            {
                const int32_t v = at<int32_t>(car + 0x48u) << 1;
                rgb = static_cast<uint32_t>(v < 0xe7 ? v : 0xe6) * 0x10101u;
            }
            const uint32_t state = at<uint32_t>(at<uint32_t>(car + 0x20u) + 0x1e8u);
            const int32_t mx = ee_div(((at<int32_t>(state + 0x10u) >> 8) - at<int32_t>(kAINode + 0x14u)) * 0x3c, scale);
            const ee_float offset = ee_itof(mx) * ee_float(float_from_bits(0x3b800000u));
            ee_float fx = at<int32_t>(kMainMirror) != 0 ? fx0 - offset : fx0 + offset;
            fx = fx - ee_itof(slide());
            fx = fx - ee_float(6.0f);
            fx = fx - ee_float(9.0f);
            const uint32_t state2 = at<uint32_t>(at<uint32_t>(car + 0x20u) + 0x1e8u);
            const int32_t mz = ee_div(((at<int32_t>(state2 + 0x18u) >> 8) - at<int32_t>(kAINode + 0x18u)) * 0x3c, scale);
            ee_float fy = fy0 - ee_itof(mz) * ee_float(float_from_bits(0x3b000000u));
            fy = fy + ee_itof(slide());
            fy = fy - ee_float(3.0f);
            fy = fy - ee_float(8.0f);
            HudSprite(ptr32<_sSprite>(at<uint32_t>(res + 0xccu)), fx, fy, rgb);
        }
    }
    const ee_float mapX = ee_itof(x0 - slide()), mapY = ee_itof(y0 + slide() - 0x40);
    if (at<int32_t>(kMainMirror) == 0)
        HudSprite(ptr32<_sSprite>(at<uint32_t>(res + 8u)), mapX, mapY, 0x2282828); // tail calls
    else
        HudSpriteMirror(ptr32<_sSprite>(at<uint32_t>(res + 8u)), mapX, mapY, 0x2282828);
}

// The wrong-way and lost signs (flashing), and the weapon held: its count and its icon, which fades in from white.
void HudMiddle(ptr32<_sObject> pObj, int8 size, int8)
{
    const uint32_t car = pObj.address;
    const uint32_t res = hudRes();
    uint32_t sprite;
    ee_float fx, fy, otz;
    uint32_t rgb;
    {
        GuestFrame frame(0x90);
        FontSetColour(0x64, 0x64, 0x64);
        if (at<int32_t>(car + 0xf0u) >= 0x3b7 && (at<uint8_t>(kHudFlash) & 8) != 0)
        {
            if (size == 2)
                HudWrongWayX(0x140, at<int32_t>(kMainWhichSplit) != 0 ? 0xa8 : 0x30, 0x10);
            else
                HudWrongWayX(0x140, 0x40, 0x20);
        }
        if (at<int32_t>(car + 0xecu) >= 0x15 && (at<uint8_t>(kHudFlash) & 8) != 0)
        {
            if (size == 2)
                HudLost(0x140, at<int32_t>(kMainWhichSplit) != 0 ? 0xa8 : 0x30, 0x10);
            else
                HudLost(0x140, 0x48, 0x18);
        }
        const uint32_t splat = kHud + 0x7cu + static_cast<uint32_t>(at<int32_t>(kMainWhichSplit)) * 4u;
        if (at<int8_t>(car + 0x194u) == 0)
        {
            at<int32_t>(kHud + 0x7cu + static_cast<uint32_t>(at<int32_t>(kMainWhichSplit)) * 4u) = -1;
            return;
        }
        const int32_t fade = at<int32_t>(splat);
        if (fade == -1)
            at<int32_t>(splat) = 0xff;
        else if (fade >= 0x81)
            at<int32_t>(splat) = fade - 2;
        const uint32_t which = static_cast<uint32_t>(at<int32_t>(kMainWhichSplit)) * 4u;
        const int32_t py = at<int32_t>(kHud + 0x8cu + which), px = at<int32_t>(kHud + 0x84u + which);
        rgb = static_cast<uint32_t>(at<int32_t>(kHud + 0x7cu + which)) * 0x10101u | 0x80000000u;
        guest_call(kSprintf, kBuff, 0x327e18u, static_cast<int32_t>(at<int8_t>(car + 0x194u))); // "%d"
        print(px + 0xd, py + 0xc, kBuff);
        fx = ee_itof(px - 0x20);
        fy = ee_itof(py - 0x10);
        const int32_t next = at<int8_t>(car + 0x1afu);
        if (next == 0)
        {
            const int32_t type = at<int8_t>(car + 0x195u);
            if (type < 12)
            {
                sprite = at<uint32_t>(res + 0x54u + static_cast<uint32_t>(type - 1) * 4u);
                otz = constant(0x327658u);
            }
            else
            {
                sprite = at<uint32_t>(res + 0x84u);
                otz = constant(0x327654u);
            }
        }
        else if (next < 12)
        {
            sprite = at<uint32_t>(res + 0x54u + static_cast<uint32_t>(next - 1) * 4u);
            otz = constant(0x327650u);
        }
        else
        {
            sprite = at<uint32_t>(res + 0x84u);
            otz = constant(0x32764cu);
        }
    }
    HudSpriteHI(ptr32<_sSprite>(sprite), fx, fy, rgb, otz); // a tail call
}

// A player's panel. A dot is printed off screen first (it sets up the font's packet state); a demo shows a flashing
// "DEMO" instead.
void HudPanel(ptr32<_sObject> pObj, int8 player)
{
    const uint32_t res = hudRes();
    {
        GuestFrame frame(0x90);
        uint32_t saved = at<uint32_t>(kFontPtr);
        FontSet(ptr32<font>(at<uint32_t>(res + 0x4cu)));
        FontSetColour(0x64, 0x64, 0x64);
        print(-100, -100, 0x327e20u); // "."
        FontSet(ptr32<font>(saved));
        if (at<int32_t>(kMainVars) == 0x10)
        {
            if (player != 0 || (at<uint8_t>(kHudFlash) & 8) == 0)
                return;
        }
        else
        {
            if (at<int32_t>(kHud + 0x78u) == 0 && at<uint32_t>(kCameraRSG) != 0)
                at<int32_t>(kHud + 0x78u) = 1;
            if (!pObj || res == 0 || at<uint16_t>(kMainPauseMenu) != 0)
                return;
            const int8 size = at<uint8_t>(kMainSplit) != 0 ? 2 : 1;
            saved = at<uint32_t>(kFontPtr); // read in the call's delay slot
            FontSet(ptr32<font>(at<uint32_t>(res + 0x4cu)));
            FontSetColour(0x64, 0x64, 0x64);
            HudTopLeft(pObj, size, player);
            HudTopRight(pObj, size, player);
            if (at<int32_t>(kMainVars) != 0x20)
            {
                HudBottomRight(pObj, size, player);
                HudBottomLeft(pObj, size, player);
            }
            HudMiddle(pObj, size, player);
            FontSet(ptr32<font>(saved));
            return;
        }
    }
    FontCentre(0xcc, ptr32<char>(0x327e28u)); // "DEMO", a tail call
}

// The front end's locked tracks: at each locked track's camera a car shadow and a padlock.
void HudDrawLockedTracks()
{
    const uint32_t car = at<uint32_t>(kObjectList);
    if (car == 0)
        return;
    GuestFrame frame(0xa0);
    for (uint32_t k = 0; k < 3; ++k)
    {
        const uint32_t tag = at<uint32_t>(kFrontEnd + 0x12cu + k * 4u);
        if (tag == 0)
            continue;
        const uint32_t camera = GetCameraFromTag(tag, ptr32<_sCamData>(at<uint32_t>(at<uint32_t>(kFrontEnd) + 0x30u))).address;
        const int32_t cy = at<int32_t>(camera + 0xcu), cz = at<int32_t>(camera + 0x10u), cx = at<int32_t>(camera + 8u);
        uint32_t spr = at<uint32_t>(kVfxSprite);
        at<uint8_t>(spr + 0x24u) = 4;
        at<uint32_t>(spr + 0x20u) = at<uint32_t>(car + 0x34u);
        at<ee_float>(spr + 0x14u) = ee_itof((cy >> 8) - 10);
        at<ee_float>(spr + 0x10u) = ee_itof(cx >> 8);
        at<ee_float>(spr + 0x18u) = ee_itof(cz >> 8);
        spr = at<uint32_t>(kVfxSprite);
        at<uint8_t>(spr + 0x25u) = 1;
        at<uint32_t>(spr + 0u) = 0x43d70000u; // 430.0
        spr = at<uint32_t>(kVfxSprite);
        at<int16_t>(spr + 0x26u) = 0;
        at<uint32_t>(spr + 0x28u) = 0x387038u;
        VfxPS2SpriteRotated3D(ee_float(0.0f));
        const uint32_t tex = at<uint32_t>(at<uint32_t>(kVfxRes) + 0x54u);
        spr = at<uint32_t>(kVfxSprite);
        at<uint8_t>(spr + 0x24u) = 2;
        at<uint32_t>(spr + 0x20u) = tex;
        spr = at<uint32_t>(kVfxSprite);
        at<uint32_t>(spr + 0u) = 0x43480000u; // 200.0
        at<uint32_t>(spr + 0x28u) = 0x505050u;
        at<int16_t>(spr + 0x26u) = 0;
        VfxPS2SpriteRotated3D(ee_float(-25.0f));
    }
}

// Loads every resource the HUD's resource names (each non-zero id becomes the loaded pointer).
void HudConstructDaemon(ptr32<memHandle> pHandle)
{
    GuestFrame frame(0x70);
    const uint32_t res = pHandle->address;
    static const uint8_t kSingle[] = {0x00, 0x04, 0x08, 0x0c, 0x10, 0x14};
    const auto load = [&](uint32_t offset)
    {
        const uint32_t id = at<uint32_t>(res + offset);
        if (id != 0)
            at<uint32_t>(res + offset) = guest_call<uint32_t>(kResLoad, id);
    };
    for (const uint8_t offset : kSingle)
        load(offset);
    for (uint32_t offset = 0x24; offset <= 0x50; offset += 4) // positions, speedo, fonts
        load(offset);
    for (uint32_t offset = 0x54; offset <= 0x94; offset += 4) // weapons, start lights
        load(offset);
    for (uint32_t offset = 0xa0; offset <= 0xd8; offset += 4) // speed digits, best lap, blip, textures
        load(offset);
}

} // namespace rc

RC_DECOMP_FN(HudSetupClass, 0x128b68)
RC_DECOMP_FN_SETUP_HEAP(HudInit, 0x124438, rc::fuzz::hudNone)
RC_DECOMP_FN_SETUP_HEAP(HudTerm, 0x124470, rc::fuzz::hudNone)
RC_DECOMP_FN_SETUP_HEAP(HudDelete, 0x124480, rc::fuzz::hudNone)
RC_DECOMP_FN_SETUP_HEAP(HudTask, 0x124490, rc::fuzz::hudTask)
RC_DECOMP_FN_SETUP_HEAP(HudCreate, 0x128aa8, rc::fuzz::hudNone)
RC_DECOMP_FN_SETUP(HudFormatLapTime, 0x1245e8, rc::fuzz::hudTime)
RC_DECOMP_FN_SETUP(HudFormatNumFont, 0x124660, rc::fuzz::hudTime)
RC_DECOMP_FN_SETUP_HEAP(HudTopLeft, 0x1246d8, rc::fuzz::hudCar)
RC_DECOMP_FN_SETUP_HEAP(HudSprite, 0x125b18, rc::fuzz::hudSprite)
RC_DECOMP_FN_SETUP_HEAP(HudSpriteMirror, 0x126ef0, rc::fuzz::hudSprite)
RC_DECOMP_FN_SETUP_HEAP(HudSpriteHI, 0x1260e8, rc::fuzz::hudSprite)
RC_DECOMP_FN_SETUP_HEAP(HudRotSpriteHI, 0x126710, rc::fuzz::hudSprite)
RC_DECOMP_FN_SETUP_HEAP(HudWrongWayX, 0x127db0, rc::fuzz::hudSign)
RC_DECOMP_FN_SETUP_HEAP(HudLost, 0x128390, rc::fuzz::hudSign)
RC_DECOMP_FN_SETUP_HEAP(HudMenuBorder, 0x1274c0, rc::fuzz::hudBorder)
RC_DECOMP_FN_SETUP_HEAP(HudTopRight, 0x1246e8, rc::fuzz::hudCar)
RC_DECOMP_FN_SETUP_HEAP(HudBottomRight, 0x124a90, rc::fuzz::hudCar)
RC_DECOMP_FN_SETUP_HEAP(HudBottomLeft, 0x124d90, rc::fuzz::hudCar)
RC_DECOMP_FN_SETUP_HEAP(HudMiddle, 0x1251b8, rc::fuzz::hudCar)
RC_DECOMP_FN_SETUP_HEAP(HudPanel, 0x125550, rc::fuzz::hudPanel)
RC_DECOMP_FN_SETUP_HEAP(HudDrawLockedTracks, 0x128930, rc::fuzz::hudLocked)
RC_DECOMP_FN_SETUP_HEAP(HudConstructDaemon, 0x125720, rc::fuzz::hudDaemon)

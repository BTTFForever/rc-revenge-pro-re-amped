// Decompiled from FONT.c: text drawn as font sprites. A font is a header followed by the sprite pointers of its
// character pages and, for proportional fonts (type 1), a width per character. Each character is drawn by filling
// the sprite parameters in scratchPadG and calling the sprite renderer. "}x" in a string draws button icon x
// through pGfxFnG (digits and capitals) or switches the colour ("}a" grey, "}b" back); a carriage return ends a line.

#include "ee_math.h"

namespace rc {

void ResTypeSetConstructDaemon(ptr32<char> pType, uint32 constructDaemon, int16 options);
void SpriteRenderFont(ptr32<_sSprite> pSprite, ptr32<_sSpriteParams> pSpriteParams, f32 otz);
void SpriteRenderScaled(ptr32<_sSprite> pSprite, ptr32<_sSpriteParams> pSpriteParams, f32 otz);
void GfxP2_RenderDmaSend();
int32_t FontSpecialCharacterOrColourChange(int16 x, int16 y, ptr32<char> pString);

namespace {

const uint32_t kSpriteParams = 0x1ed4c0; // scratchPadG, used as the sprite parameters.

_sSpriteParams &params() { return *static_cast<_sSpriteParams *>(guest_host(kSpriteParams)); }

uint32_t remu(uint32_t a, uint32_t b) { return b == 0 ? a : a % b; }

// A character's place in the font: its index into the character pages (with the second range of characters
// following the first), or the code itself if it is in neither range.
uint32_t glyphIndex(const font &f, uint32_t c)
{
    uint32_t base = f.firstCh0;
    if (c < f.firstCh0 || f.lastCh0 < c)
    {
        const uint32_t second = f.firstCh1;
        if (c < second || f.lastCh1 < c)
            return c;
        base = second + (base - f.lastCh0) + 0xff;
    }
    return (c - base) & 0xff;
}

// The texel position of a character on its page, and the page's sprite.
void placeGlyph(const font &f, uint32_t fontAddress, uint32_t index, int16_t rowOffset, ptr32<_sSprite> &sprite)
{
    const uint32_t perPage = f.chPerPage;
    const uint32_t onPage = remu(index, perPage);
    _sSpriteParams &p = params();
    p.xOffset = static_cast<uint16>(static_cast<uint16_t>(f.chWidth * static_cast<int16_t>(remu(onPage, f.chPerLine))) & 0xff);
    p.yOffset = static_cast<uint16>(static_cast<uint16_t>(f.chHeight * static_cast<int16_t>(ee_divu(onPage, f.chPerLine)) + rowOffset) & 0xff);
    sprite = *ptr32<ptr32<_sSprite>>(fontAddress + 0xc + ee_divu(index, perPage) * 4u);
}

uint8_t charWidth(const font &f, uint32_t fontAddress, uint32_t index)
{
    return *static_cast<uint8_t *>(guest_host(fontAddress + 0xc + index + f.numSprites * 4u));
}

int16_t add16(int32_t a, int32_t b) { return static_cast<int16_t>(a + b); }

} // namespace

// The width of the longest line of a string, in pixels.
uint32 FontLength(ptr32<char> pString)
{
    const ptr32<font> fontPtr = G(fontVarG).pFont;
    const font &f = *fontPtr;
    uint32_t line = 0, longest = 0;
    const uint8_t *s = reinterpret_cast<const uint8_t *>(pString.get());
    uint32_t at = pString.address;
    const auto ch = [&](uint32_t offset) { return *static_cast<const uint8_t *>(guest_host(at + offset)); };
    (void)s;
    if (f.type == 0)
    {
        for (; ch(0) != 0; ++at)
        {
            if (ch(0) == '\r')
            {
                if (longest < line)
                    longest = line;
                line = 0;
            }
            else
                line += fontPtr->chWidth;
        }
    }
    else if (f.type == 1)
    {
        while (ch(0) != 0)
        {
            const uint8_t c = ch(0);
            if (c == 0xd)
            {
                if (longest < line)
                    longest = line;
                line = 0;
                ++at;
            }
            else if (c == 0x20)
            {
                line += fontPtr->spWidth;
                ++at;
            }
            else if (c == 0x7d)
            {
                // A button icon is 32 wide; a colour change takes no room.
                const int32_t next = static_cast<int8_t>(ch(1));
                if (static_cast<uint32_t>(next - 0x30) < 10u || static_cast<uint32_t>(next - 0x41) < 0x1au)
                    line += 0x20;
                at += 2;
            }
            else
            {
                line = line + charWidth(f, fontPtr.address, glyphIndex(f, c)) - (fontPtr->flags & 1u);
                ++at;
            }
        }
    }
    return line < longest ? longest : line;
}

void FontSet(ptr32<font> pFont)
{
    G(fontVarG).pFont = pFont;
}

void FontSetColour(uint8 red, uint8 green, uint8 blue)
{
    fontClass &font = G(fontVarG);
    font.b = blue;
    font.r = red;
    font.g = green;
}

void FontSetGraphicFn(ptr32<void> pFn)
{
    G(pGfxFnG).address = pFn.address;
}

void FontInit()
{
    fontClass &font = G(fontVarG);
    font.b = 0x7f;
    font.pFont = nullptr;
    font.r = 0x7f;
    font.g = 0x7f;
    ResTypeSetConstructDaemon(ptr32<char>(0x328188u), 0, 4);
}

// Draws one line of a string at (x, y). Returns the start of the next line (after the carriage return), or null at
// the end of the string.
ptr32<char> FontPrint(int16 x, int16 y, ptr32<char> pString)
{
    GuestFrame frame(0xb0);
    const uint32_t frameSp = frame.local<uint8_t>(0).address;
    static const uint64_t kSavedS4 = kSpriteParams;
    const ptr32<font> fontPtr = G(fontVarG).pFont;
    const font &f = *fontPtr;
    _sSpriteParams &p = params();
    p.height = f.chHeight;
    p.y = y;
    p.red = G(fontVarG).r;
    p.green = G(fontVarG).g;
    p.blue = G(fontVarG).b;
    if (f.type != 0 && f.type != 1)
        return nullptr;
    if (f.type == 0)
        p.width = f.chWidth;
    int16_t pen = x;
    uint32_t at = pString.address;
    const auto ch = [](uint32_t address) { return *static_cast<const uint8_t *>(guest_host(address)); };
    for (;;)
    {
        const uint8_t c = ch(at);
        if (c == 0)
            return nullptr;
        if (c != 0xd)
        {
            if (c == 0x20)
                pen = add16(pen, f.spWidth);
            else if (c == 0x7d)
            {
                ++at;
                pen = static_cast<int16_t>(FontSpecialCharacterOrColourChange(pen, y, ptr32<char>(at)));
            }
            else
            {
                const uint32_t index = glyphIndex(f, c);
                ptr32<_sSprite> sprite;
                placeGlyph(f, fontPtr.address, index, 0, sprite);
                if (f.type == 0)
                {
                    if (c == 0x58) // 'X' sits four texels right on its page.
                        p.xOffset = static_cast<uint16>((p.xOffset + 4) & 0xff);
                }
                else
                    p.width = charWidth(f, fontPtr.address, index);
                p.x = pen;
                SpriteRenderFont(sprite, ptr32<_sSpriteParams>(kSpriteParams), 0.0f);
                // What the original's SpriteRenderFont leaves in its frame: this function's $s4 and $s5, saved at
                // 0x70 and 0x80. AMenuDisplayMIT's slider reads the first as its frame rectangle's flags.
                std::memcpy(guest_host(frameSp - 0x30u), &kSavedS4, 8);
                const uint64_t savedS5 = fontPtr.address + 0xcu;
                std::memcpy(guest_host(frameSp - 0x20u), &savedS5, 8);
                if (f.type == 0)
                    pen = add16(pen, fontPtr->chWidth);
                else
                {
                    pen = add16(pen, charWidth(f, fontPtr.address, index));
                    if ((fontPtr->flags & 1) != 0)
                        pen = add16(pen, -1);
                }
            }
        }
        ++at;
        if (ch(at) == 0xd)
            return ptr32<char>(at + 1);
    }
}

void FontPrintRight(int16 x, int16 y, ptr32<char> pString)
{
    GuestFrame frame(0x40);
    const uint32_t length = FontLength(pString);
    FontPrint(static_cast<int16>(x - static_cast<int16_t>(length)), y, pString);
}

void FontCentre(int16 y, ptr32<char> pString)
{
    GuestFrame frame(0x40);
    const uint32_t length = FontLength(pString);
    const int32_t xRes = G(displayVarG).state.pDispDef->xResolution;
    const int32_t x = static_cast<int32_t>((static_cast<uint32_t>(xRes) - length) * 0x8000u) >> 16;
    FontPrint(static_cast<int16>(x), y, pString);
}

// As FontPrint without line breaks, scaled (fx 1: width, 2: height by fxValue/256), raised (3) or shortened (4).
// In the fixed-width font a space, and a "}" sequence after its own advance, move on a character width.
int32_t FontPrintFX(int16 x, int16 y, ptr32<char> pString, uint16 fx, uint16 fxValue)
{
    GuestFrame frame(0xb0);
    const ptr32<font> fontPtr = G(fontVarG).pFont;
    const font &f = *fontPtr;
    _sSpriteParams &p = params();
    int16_t shorten = 0, raise = 0;
    p.yScale = 0x100;
    p.xScale = 0x100;
    switch (fx)
    {
    case 1: p.xScale = fxValue; break;
    case 2: p.yScale = fxValue; break;
    case 3:
        y = static_cast<int16>(y - fxValue);
        shorten = static_cast<int16_t>(fxValue);
        raise = static_cast<int16_t>(fxValue);
        break;
    case 4: shorten = static_cast<int16_t>(fxValue); break;
    default: break;
    }
    p.red = G(fontVarG).r;
    p.green = G(fontVarG).g;
    p.blue = G(fontVarG).b;
    p.flags = 0;
    p.height = static_cast<uint16>(f.chHeight - shorten);
    p.y = y;
    int16_t pen = x;
    uint32_t at = pString.address;
    const auto ch = [](uint32_t address) { return *static_cast<const uint8_t *>(guest_host(address)); };
    if (f.type == 0)
    {
        p.width = f.chWidth;
        for (; ch(at) != 0; ++at)
        {
            const uint8_t c = ch(at);
            if (c == 0x7d)
            {
                ++at;
                pen = static_cast<int16_t>(FontSpecialCharacterOrColourChange(pen, y, ptr32<char>(at)));
            }
            else if (c != 0x20)
            {
                ptr32<_sSprite> sprite;
                placeGlyph(f, fontPtr.address, glyphIndex(f, c), raise, sprite);
                p.x = pen;
                SpriteRenderScaled(sprite, ptr32<_sSpriteParams>(kSpriteParams), 0.0f);
            }
            pen = add16(pen, fontPtr->chWidth);
        }
    }
    else if (f.type == 1)
    {
        for (; ch(at) != 0; ++at)
        {
            const uint8_t c = ch(at);
            if (c == 0x20)
                pen = add16(pen, f.spWidth);
            else if (c == 0x7d)
            {
                ++at;
                pen = static_cast<int16_t>(FontSpecialCharacterOrColourChange(pen, y, ptr32<char>(at)));
            }
            else
            {
                const uint32_t index = glyphIndex(f, c);
                p.width = charWidth(f, fontPtr.address, index);
                ptr32<_sSprite> sprite;
                placeGlyph(f, fontPtr.address, index, raise, sprite);
                p.x = pen;
                SpriteRenderScaled(sprite, ptr32<_sSpriteParams>(kSpriteParams), 0.0f);
                pen = add16(pen, charWidth(f, fontPtr.address, index));
                if ((fontPtr->flags & 1) != 0)
                    pen = add16(pen, -1);
            }
        }
    }
    return fxValue;
}

// "}" sequences: a button icon (0-9, A-Z, drawn by the game's callback, 32 wide) or a colour change.
int32_t FontSpecialCharacterOrColourChange(int16 x, int16 y, ptr32<char> pString)
{
    GuestFrame frame(0x20);
    const int32_t c = static_cast<int8_t>(*pString);
    int32_t pen = x;
    if (static_cast<uint8_t>(c - 0x30) < 10u || static_cast<uint32_t>(c - 0x41) < 0x1au)
    {
        if (G(pGfxFnG).address != 0)
        {
            const int32_t icon = static_cast<uint8_t>(*pString - 0x30) < 10u ? c - 0x30 : c - 0x37;
            guest_call(G(pGfxFnG).address, static_cast<int32_t>(x), static_cast<int32_t>(y), icon);
            // AMenuDisplayPsxKey saves its return address (the instruction after the original's call here) at
            // 0x20 of its 0x30 frame, where a later slider's frame rectangle finds its flags.
            if (G(pGfxFnG).address == 0x10eea0u)
            {
                const uint64_t savedRa = 0x18a320u;
                std::memcpy(guest_host(frame.local<uint8_t>(0).address - 0x10u), &savedRa, 8);
            }
        }
        pen = static_cast<int16_t>(x + 0x20);
        GfxP2_RenderDmaSend();
    }
    else if (c == 'a')
    {
        fontClass &font = G(fontVarG);
        G(redG) = font.r;
        G(greenG) = font.g;
        G(blueG) = font.b;
        font.b = '6';
        font.r = '6';
        font.g = '6';
        params().red = font.r;
        params().green = font.g;
        params().blue = font.b;
    }
    else if (c == 'b')
    {
        fontClass &font = G(fontVarG);
        font.b = G(blueG);
        font.r = G(redG);
        font.g = G(greenG);
        params().red = font.r;
        params().green = font.g;
        params().blue = font.b;
    }
    return pen;
}

int32_t FontHandleSpecialCharacter(int16 x, int16 y, ptr32<char> pString)
{
    GuestFrame frame(0x20);
    if (G(pGfxFnG).address != 0)
    {
        const uint8_t c = *pString.cast<uint8_t>();
        const int32_t icon = static_cast<uint32_t>(c - 0x30) < 10u ? static_cast<int8_t>(c) - 0x30 : static_cast<int8_t>(c) - 0x37;
        guest_call(G(pGfxFnG).address, static_cast<int32_t>(x), static_cast<int32_t>(y), icon);
    }
    GfxP2_RenderDmaSend();
    return static_cast<int16_t>(x + 0x20);
}

} // namespace rc

namespace rc::fuzz {

// Random text for the loaded font: printable characters, spaces, "}" sequences (icons, colour changes, others) and
// line breaks, in the test's scratch area. a0 x, a1 y, a2 the string, a3/t0 an effect and its value for
// FontPrintFX.
void fontText(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    if (!G(fontVarG).pFont)
        return;
    char *text = static_cast<char *>(guest_host(scratch + 0x100));
    const int length = 1 + static_cast<int>(nextFuzz(state) % 40u);
    static const char kExtra[] = "}0}A}a}b}z ";
    for (int i = 0; i < length; ++i)
    {
        const uint32_t pick = nextFuzz(state) % 100u;
        if (pick < 8)
            text[i] = '\r';
        else if (pick < 20)
        {
            const char *e = kExtra + 2 * (nextFuzz(state) % 5u);
            text[i++] = e[0];
            if (i < length)
                text[i] = e[1];
        }
        else if (pick < 28)
            text[i] = ' ';
        else
            text[i] = static_cast<char>(0x21 + nextFuzz(state) % 0x5e);
    }
    text[length] = 0;
    SET_GPR_S32(ctx, 4, static_cast<int16_t>(nextFuzz(state) % 640u));
    SET_GPR_S32(ctx, 5, static_cast<int16_t>(nextFuzz(state) % 480u));
    SET_GPR_S32(ctx, 6, static_cast<int32_t>(scratch + 0x100));
    SET_GPR_S32(ctx, 7, static_cast<int32_t>(nextFuzz(state) % 6u));
    SET_GPR_S32(ctx, 8, static_cast<int32_t>(nextFuzz(state) % 0x200u));
}

// As fontText, for the functions that take the string first: a0 the string (FontLength), or a0 y, a1 the string.
void fontTextFirst(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    fontText(ctx, state, scratch);
    SET_GPR_S32(ctx, 4, static_cast<int32_t>(scratch + 0x100));
}

void fontTextCentre(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    fontText(ctx, state, scratch);
    SET_GPR_S32(ctx, 4, static_cast<int16_t>(nextFuzz(state) % 480u));
    SET_GPR_S32(ctx, 5, static_cast<int32_t>(scratch + 0x100));
}

} // namespace rc::fuzz

RC_DECOMP_FN_SETUP(FontLength, 0x189ca0, rc::fuzz::fontTextFirst)
RC_DECOMP_FN(FontSet, 0x18a3c0)
RC_DECOMP_FN(FontSetColour, 0x18a3c8)
RC_DECOMP_FN(FontSetGraphicFn, 0x18a3e0)
RC_DECOMP_FN_NOFUZZ(FontInit, 0x1897d8)
RC_DECOMP_FN_SETUP_HEAP(FontPrint, 0x189808, rc::fuzz::fontText)
RC_DECOMP_FN_SETUP_HEAP(FontPrintRight, 0x189bd8, rc::fuzz::fontText)
RC_DECOMP_FN_SETUP_HEAP(FontCentre, 0x189c38, rc::fuzz::fontTextCentre)
RC_DECOMP_FN_SETUP_HEAP(FontPrintFX, 0x189ea8, rc::fuzz::fontText)
RC_DECOMP_FN_SETUP_HEAP(FontSpecialCharacterOrColourChange, 0x18a2a8, rc::fuzz::fontText)
RC_DECOMP_FN_SETUP_HEAP(FontHandleSpecialCharacter, 0x18a3e8, rc::fuzz::fontText)

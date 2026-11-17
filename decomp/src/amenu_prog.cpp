// Decompiled from AMENU.c: the scrolling pages built on top of a menu's box: the championship progress card, the
// time-trial records and the Track Editor's help. Each frame the page is laid out again from the scroll position
// (frontEndVarG.scrollPTT): entries off the box only take their room, measured on the first, non-drawing pass
// (fisrtDrawFlagPTT), and PS2ScrollMenu moves the position with the pad.
//
// The xxx, xxy and Old builders are earlier versions nothing calls; BuildProgress, BuildTimeTrials and
// BuildTrackEditorHelp are the ones in use.

#include "amenu.h"
#include "amenu_fuzz.h"

namespace rc {

using namespace amenu;

namespace {

constexpr uint32_t kBox = kFrontEnd + 0xa8u; // The page's own menu header, inside frontEndVarG.
constexpr uint32_t kFastScroll = sym::fastScrollPoints;  // fastScrollPoints

int16_t &scrollSpeed() { return at<int16_t>(kFrontEnd + 0x2c8u); }
int16_t &scrollPos() { return at<int16_t>(kFrontEnd + 0x2cau); }
int16_t &scrollHeight() { return at<int16_t>(kFrontEnd + 0x2ccu); }
int16_t &scrollStartY() { return at<int16_t>(kFrontEnd + 0x2ceu); }
int32_t &fastScroll(int32_t index) { return at<int32_t>(kFastScroll + static_cast<uint32_t>(index) * 4u); }
ptr32<sAMenuHeader> box() { return ptr32<sAMenuHeader>(kBox); }

int32_t fontHeight()
{
    const ptr32<font> pFont = G(fontVarG).pFont;
    const int32_t half = pFont->chHeight >> 1;
    return (pFont->flags & 2u) != 0 ? half - 2 : half;
}

// Whether an entry of a known height at y lies off the box: above line 40 or below the box's bottom.
bool offBox(const sAMenuHeader &h, int32_t y, int32_t height)
{
    return y < 0x28 - height || h.y + h.height < y;
}

// The page's title: centred on one line if it fits, else wrapped from 16 pixels in, the page starting lower by the
// extra lines.
void pageTitle(ptr32<sAMenuHeader> pHeader, ptr32<char> pTitle)
{
    sAMenuHeader &h = *pHeader;
    FontSetColour(0, 0x7f, 0x7f);
    if (FontLength(pTitle) < h.width)
    {
        const int32_t x = AStringCentre(h.x, h.width, pTitle, -1);
        FontPrint(i16(x), h.y, pTitle);
        G(scrollLinesG) = 0;
        return;
    }
    const uint32_t x = static_cast<uint16_t>(h.x) + 0x10u;
    h.changeY = static_cast<uint8>(G(fontVarG).pFont->chHeight >> 1);
    h.x = i16(static_cast<int32_t>(x));
    const int32_t lines = AStringWrap(i16(static_cast<int32_t>(x)), h.y, pTitle, pHeader);
    const int32_t extra = (lines - 1) * h.changeY;
    h.x = i16(h.x - 0x10);
    G(scrollLinesG) = extra;
}

// The page's box: a copy of the source menu without its texts, items and links, 0x94 high at y 20, with the
// source's additional text as its key hints.
void makeBox(ptr32<sAMenuHeader> pSourceHeader, ptr32<sAMenuHeader> pHeader, int32_t width)
{
    for (uint32_t block = 0; block < 0x80u; block += 0x20u)
    {
        uint8_t words[0x20];
        std::memcpy(words, guest_host(pSourceHeader.address + block), 0x20);
        std::memcpy(guest_host(pHeader.address + block), words, 0x20);
    }
    sAMenuHeader &h = *pHeader;
    h.pTitleText = nullptr;
    h.pDialogTxt = nullptr;
    h.pAdditionalText = nullptr;
    h.pSpareText = nullptr;
    h.pLinkMenuReturn = nullptr;
    const ptr32<sTextGroup> hints = pSourceHeader->pAdditionalText;
    h.pMenuStart = nullptr;
    h.pGlobalDestMenu = nullptr;
    h.selectionTag = 0;
    h.pCharList = nullptr;
    h.pUIgroup = nullptr;
    h.x = 0;
    h.changeX = 0;
    h.changeY = 0;
    h.dlgLines = 0;
    h.scrollItems = 0;
    h.scrollStart = 0;
    h.colSelect = 0;
    h.rowSelect = 0;
    h.recurseFlag = 0;
    h.pOptionsList = hints;
    h.flags = 0x820;
    h.y = 0x14;
    h.width = static_cast<uint16>(width);
    h.height = 0x94;
    AMenuSnapsize(pHeader);
}

// The arrows showing there is more above (at the box's bottom) and below (at its top).
void arrowTop(const Locals &f, const sAMenuHeader &h, ee_float otz) { keySprite(f, menuSystem().pTickGfx, h.x, h.y, 0x1e, otz); }
void arrowBottom(const Locals &f, const sAMenuHeader &h, ee_float otz)
{
    keySprite(f, menuSystem().pTickGfx, h.x, h.y + h.height - 8, 0x1c, otz);
}

// Back or select on a page: leaves it.
void leavePage()
{
    at<int8_t>(kFrontEnd + 0x2d0u) = 0;
    menuSystem().scrollSelector = 0;
    sfxBack();
    menuSystem().executeCode = 0x11000000u;
}

// The jump list of the earlier versions: how many entries before its end mark.
int32_t fastScrollCount()
{
    int32_t count = 0;
    while (fastScroll(count) != -1)
        ++count;
    return count;
}

// CheckRaceTick with the rewards table at a place in the caller's frame.
int32_t raceTick(const Locals &f, uint32_t table, uint32_t state, int32_t level, int32_t raceType)
{
    guest_call<void>(kSetRewards, f.ptr<void>(table), ptr32<void>(state), at<int32_t>(state + 0x124u));
    if ((f.at<uint8_t>(table + 7u + static_cast<uint32_t>(level % 0x18)) & raceType) == 0)
        return 0;
    return level / 6 < at<int32_t>(state + 0x124u) ? 1 : 2;
}

void endPage(int32_t y, int32_t speed)
{
    G(bottomOfScrollG) = CheckBottomOfScroll(box(), y);
    AMenuRectClipEnd(box(), scrollStartY());
    PS2ScrollMenu(box(), speed);
    firstDrawPTT() = 0;
}

// The earlier builders' start: the title by card (-1 the progress, 1 and 2 the two halves), through BuildProgTTSub.
void oldTitle(ptr32<sAMenuHeader> pSourceHeader, int32_t card)
{
    if (card == -1 || card == 1 || card == 2)
        BuildProgTTSub(pSourceHeader, box(), 0x1e0, GetGeneralString(card == -1 ? 0x1e : card == 1 ? 0x1f : 0x20));
}

void oldBox()
{
    if (firstDrawPTT() == 0)
    {
        AMenuDrawBox(box(), 0);
        AMenuDisplayOptions(box());
    }
    AMenuRectClipStart(box(), scrollStartY(), 0);
}

} // namespace

// A track's picture (62 by 62 scaled by scale/256, twice that across) with its name wrapped beside it, a word at a
// time. Returns the y under the picture.
int32_t DisplayTrackGraphic(int32_t x, int32_t y, int32_t scale, int32_t width, int32_t level)
{
    Locals f(0x8d0);
    const uint32_t extra = at<uint32_t>(at<uint32_t>(kFrontEnd) + 0xfcu) + static_cast<uint32_t>(level) * 0x10u;
    const ptr32<char> buffer = f.ptr<char>(0x20), buffer2 = f.ptr<char>(0x410), space = f.ptr<char>(0x800);
    space[0] = at<char>((sym::sec_sdata + 0x90u));
    space[1] = at<char>((sym::sec_sdata + 0x91u));
    const ptr32<_sSprite> pSprite(at<uint32_t>(extra - 0xcu));
    std::memset(guest_host(f.sp), 0, 0x18);
    const int32_t xScale = scale * 2;
    f.sh(0x4, x + 1);
    f.sb(0x12, 0x7f);
    f.sw(0x0, 0x20);
    f.sh(0xe, 0x3e);
    int32_t yWrap = y;
    f.sh(0x6, y + 1);
    f.sb(0x10, 0x7f);
    f.sb(0x11, 0x7f);
    f.sh(0x14, xScale);
    f.sh(0x16, scale);
    f.sh(0xc, 0x3e);
    SpriteRenderScaled(pSprite, f.ptr<_sSpriteParams>(0), 0.0f);
    std::memset(buffer.get(), 0, 0x3e8);
    const int32_t xWrap = x + xScale / 4 + 0xa;
    const int32_t wrapWidth = width + x - xWrap;
    const ptr32<char> pLevelName = text(ptr32<sTextGroup>(at<uint32_t>(extra - 0x10u)));
    // The name with each space made the end of a string.
    for (uint32_t i = 0; i < std::strlen(pLevelName.get()); ++i)
    {
        const char c = pLevelName[i];
        buffer[i] = c;
        if (c == ' ')
            buffer[i] = 0;
        buffer[i + 1] = 0;
    }
    const uint32_t spaceSize = FontLength(space);
    int32_t i = 0;
    uint32_t length = 0;
    buffer2[0] = 0;
    const ptr32<font> pFont = G(fontVarG).pFont;
    int32_t lineHeight = pFont->chHeight >> 1;
    if ((pFont->flags & 2u) != 0)
    {
        lineHeight -= 2;
        yWrap -= 2;
        y -= 2;
    }
    if (buffer[0] != 0)
    {
        do
        {
            const ptr32<char> word(buffer.address + static_cast<uint32_t>(i));
            if (!(length + FontLength(word) < static_cast<uint32_t>(wrapWidth)))
            {
                FontPrint(i16(xWrap), i16(yWrap), buffer2);
                yWrap += lineHeight;
                length = 0;
                buffer2[0] = 0;
            }
            ++i;
            length += FontLength(word) + spaceSize;
            std::strcat(buffer2.get(), word.get());
            std::strcat(buffer2.get(), space.get());
            i += static_cast<int32_t>(std::strlen(word.get()));
        } while (buffer[i] != 0);
    }
    if (buffer2[0] != 0)
        FontPrint(i16(xWrap), i16(yWrap), buffer2);
    return y + scale / 4 + 2;
}

// The top of the progress card: the rewards won, then the cups (0 not reached, 2 the current one, 1 done), each a
// tick box with its name, in two columns, and a rule under them. Returns the y below.
int32_t DisplayProgressHeader(ptr32<sAMenuHeader> pHeader, ptr32<_sFrontGameStateData> pGameStateData, int32_t x, int32_t y)
{
    Locals f(0x120);
    const sAMenuHeader &h = *pHeader;
    const int32_t entryY = y;
    const int32_t known = G(amenuScrollProgHeaderHeightG);
    if (known != -1 && offBox(h, y, known))
        return y + known;
    const int32_t lineHeight = fontHeight();
    if (firstDrawPTT() != 0)
        y = y + 3 + lineHeight * 9;
    else
    {
        const auto heading = [&](int32_t message)
        {
            FontSetColour(0, 0x7f, 0x7f);
            FontPrint(i16(x), i16(y), GetGeneralString(message));
            FontSetColour(0x7f, 0x7f, 0x7f);
        };
        const auto row = [&](int32_t left, int32_t leftText, int32_t right, int32_t rightText)
        {
            DisplayTickBoxText(pHeader, x, y, left, GetGeneralString(leftText), 1);
            DisplayTickBoxText(pHeader, x, y, right, GetGeneralString(rightText), 2);
            y += lineHeight;
        };
        heading(0x22);
        y += lineHeight;
        const uint32_t state = pGameStateData.address;
        guest_call<void>(kSetRewards, f.ptr<void>(0x20), pGameStateData, at<int32_t>(state + 0x124u));
        const auto reward = [&](uint32_t index) { return static_cast<int32_t>(f.at<uint8_t>(0x20u + index)); };
        row(reward(2), 0x34, reward(4), 0x38);
        row(reward(3), 0x3a, reward(5), 0x39);
        row(reward(0), 0x3b, reward(1), 0x30);
        row(reward(0x1f), 0x3c, reward(0x20), 0x3d);
        row(reward(0x21), 0x3e, reward(0x22), 0x3f);
        heading(0x23);
        y += lineHeight;
        const int32_t cup = at<int32_t>(state + 0x124u);
        for (int32_t i = 0; i < 8; ++i)
            f.at<int32_t>(static_cast<uint32_t>(i) * 4u) = cup < i ? 0 : cup == i ? 2 : 1;
        const auto cups = [&](uint32_t index) { return f.at<int32_t>(index * 4u); };
        row(cups(0), 0x24, cups(4), 0x25);
        row(cups(1), 0x26, cups(5), 0x27);
        row(cups(2), 0x28, cups(6), 0x29);
        row(cups(3), 0x2a, cups(7), 0x2b);
        y += 4;
        rule(f, 0x50, h, y, constant(sym::lit4_3275a8));
        y += 9;
    }
    G(amenuScrollProgHeaderHeightG) = y - entryY;
    return y;
}

// A track's entry on the progress card: its picture and name, and tick boxes for the race and the reversed race.
int32_t DisplayProgressEntry(ptr32<sAMenuHeader> pHeader, ptr32<_sFrontGameStateData> pGameStateData, int32_t x, int32_t y, int32_t width, int32_t level)
{
    Locals f(0x140);
    const sAMenuHeader &h = *pHeader;
    const int32_t entryY = y;
    const int32_t known = G(amenuScrollProgEntryHeightG);
    if (known != -1 && offBox(h, y, known))
        return y + known;
    const ptr32<char> pNormal = GetGeneralString(0x1d);
    const ptr32<char> localReverse = f.ptr<char>(0x30);
    std::strcpy(localReverse.get(), GetGeneralString(0x17).get());
    // Without its trailing spaces (an empty string looks at the byte before the buffer).
    while (at<int8_t>(localReverse.address + static_cast<uint32_t>(std::strlen(localReverse.get())) - 1u) == ' ')
        at<int8_t>(localReverse.address + static_cast<uint32_t>(std::strlen(localReverse.get())) - 1u) = 0;
    if (firstDrawPTT() != 0)
        y += 0x40;
    else
        y = DisplayTrackGraphic(x, y, 0x100, width, level);
    const int32_t lineHeight = fontHeight();
    const uint32_t state = pGameStateData.address;
    guest_call<void>(kSetRewards, f.ptr<void>(0), pGameStateData, at<int32_t>(state + 0x124u));
    const int32_t tick = raceTick(f, 0x50, state, level - 1, 1);
    const int32_t reverseTick = raceTick(f, 0x50, state, level + 0x17, 2);
    if (firstDrawPTT() != 0)
        y = y + 3 + lineHeight;
    else
    {
        DisplayTickBoxText(pHeader, x, y, tick, pNormal, 1);
        DisplayTickBoxText(pHeader, x, y, reverseTick, localReverse, 2);
        y += lineHeight;
        rule(f, 0x50, h, y, constant(sym::lit4_3275ac));
        y += 9;
    }
    G(amenuScrollProgEntryHeightG) = y - entryY;
    return y;
}

// A track's entry on the time-trial page: its picture and name, and the best times forwards and reversed.
int32_t DisplayTTEntry(ptr32<sAMenuHeader> pHeader, int32_t x, int32_t y, int32_t width, int32_t level)
{
    Locals f(0x140);
    const sAMenuHeader &h = *pHeader;
    const int32_t entryY = y;
    const int32_t known = G(amenuScrollTTHeightG);
    if (known != -1 && offBox(h, y, known))
        return y + known;
    const ptr32<char> pNormal = GetGeneralString(0x1d), pReverse = GetGeneralString(0x17);
    if (firstDrawPTT() != 0)
        y += 0x40;
    else
        y = DisplayTrackGraphic(x, y, 0x100, width, level);
    const int32_t lineHeight = fontHeight();
    if (firstDrawPTT() != 0)
        y = y + 3 + lineHeight * 2;
    else
    {
        const ptr32<char> timeString = f.ptr<char>(0);
        const auto line = [&](ptr32<char> name, int32_t reverseFlag)
        {
            DisplayTickBoxText(pHeader, x, y, -1, name, 1);
            const int32_t beaten = guest_call<int32_t>(kGetTimeTrialTime, timeString, ptr32<void>(kFrontEnd + 0x138u), level, reverseFlag);
            DisplayTickBoxText(pHeader, x, y, beaten, timeString, 2);
            y += lineHeight;
        };
        line(pNormal, 0);
        line(pReverse, 1);
        rule(f, 0x70, h, y, constant(sym::lit4_3275b0));
        y += 9;
    }
    G(amenuScrollTTHeightG) = y - entryY;
    return y;
}

// The earlier versions' page start: makes the box once (while the page's height is not known), handles the jump
// keys, and draws the arrows and the title.
void BuildProgTTSub(ptr32<sAMenuHeader> pSourceHeader, ptr32<sAMenuHeader> pHeader, int32_t width, ptr32<char> pTitle)
{
    RC_DIVERT_IF_CHANGED(BuildProgTTSub, pSourceHeader, pHeader, width, pTitle);
    Locals f(0x60);
    const sAMenuHeader &h = *pHeader;
    if (scrollHeight() == 0)
    {
        makeBox(pSourceHeader, pHeader, width);
        G(scrollJumpPosG) = 0;
        at<int8_t>(kFrontEnd + 0x2d1u) = 0;
    }
    NewPTTScroll(pHeader);
    if (firstDrawPTT() == 0)
    {
        if (fastScroll(G(scrollJumpPosG) + 1) != -1)
            arrowTop(f, h, constant(sym::lit4_3275b4));
        if (G(scrollJumpPosG) > 0)
            arrowBottom(f, h, constant(sym::lit4_3275b8));
        if (pTitle)
            pageTitle(pHeader, pTitle);
    }
    scrollStartY() = static_cast<int16_t>(G(fontVarG).pFont->chHeight >> 1);
    FontSetColour(0x7f, 0x7f, 0x7f);
}

// An earlier version's scrolling: the position glides by a fifth of the speed between the box's top and the page's
// end (1 and 2 when it reaches them), and up and down jump along the jump list.
int32_t PTTScroll(ptr32<sAMenuHeader> pHeader)
{
    RC_DIVERT_IF_CHANGED(PTTScroll, pHeader);
    GuestFrame frame(0x50);
    const sAMenuHeader &h = *pHeader;
    int32_t retVal = 0;
    if (at<int8_t>(kFrontEnd + 0x2d1u) > 0)
        return 0;
    scrollPos() = static_cast<int16_t>(scrollPos() + scrollSpeed() / 5);
    if (!(scrollPos() < h.y))
    {
        scrollPos() = h.y;
        retVal = 1;
    }
    if (!(static_cast<int32_t>(h.height) - (h.y + scrollHeight()) < scrollPos()))
    {
        retVal = 2;
        scrollPos() = static_cast<int16_t>(h.height - (static_cast<uint16_t>(h.y) + static_cast<uint16_t>(scrollHeight())));
    }
    const int32_t hitKey = AMenuNewProcessInput(-1, static_cast<eMenuInputStyle>(2), pHeader);
    if (hitKey == 2)
    {
        scrollSpeed() = 0;
        sfxMove();
        int32_t j = G(scrollJumpPosG) + 1;
        G(scrollJumpPosG) = j;
        if (fastScroll(j) == -1)
        {
            G(scrollJumpPosG) = 0;
            j = 0;
        }
        scrollPos() = static_cast<int16_t>(static_cast<uint16_t>(fastScroll(j)) + 0x14);
    }
    else if (hitKey == 1)
    {
        scrollSpeed() = 0;
        sfxMove();
        int32_t j = G(scrollJumpPosG) - 1;
        G(scrollJumpPosG) = j;
        if (j < 0)
        {
            j = fastScrollCount() - 1;
            G(scrollJumpPosG) = j;
        }
        scrollPos() = static_cast<int16_t>(static_cast<uint16_t>(fastScroll(j)) + 0x14);
    }
    else if (hitKey == 5 || hitKey == 6)
        leavePage();
    if (hitKey != 0)
        at<uint32_t>(kFrontEnd + 0x2a8u) = 0;
    return retVal;
}

// The jump keys alone: up and down move along the jump list, wrapping.
void NewPTTScroll(ptr32<sAMenuHeader> pHeader)
{
    RC_DIVERT_IF_CHANGED(NewPTTScroll, pHeader);
    GuestFrame frame(0x10);
    const int32_t hitKey = AMenuNewProcessInput(-1, static_cast<eMenuInputStyle>(2), pHeader);
    if (hitKey == 2)
    {
        scrollSpeed() = 0;
        sfxMove();
        const int32_t j = G(scrollJumpPosG) + 1;
        G(scrollJumpPosG) = j;
        if (fastScroll(j) == -1)
            G(scrollJumpPosG) = 0;
    }
    else if (hitKey == 1)
    {
        scrollSpeed() = 0;
        sfxMove();
        const int32_t j = G(scrollJumpPosG) - 1;
        G(scrollJumpPosG) = j;
        if (j < 0)
            G(scrollJumpPosG) = fastScrollCount() - 1;
    }
    else if (hitKey == 5 || hitKey == 6)
        leavePage();
}

// The championship progress card. card -1 is the whole card; 1 and 2 are its two halves, drawn ten lines lower
// under a title that scrolls with them. For another card the page's offset is whatever the caller's $s2 held.
void BuildProgress(ptr32<sAMenuHeader> pSourceHeader, ptr32<_sFrontGameStateData> pGameStateData, int32_t card)
{
    RC_DIVERT_IF_CHANGED(BuildProgress, pSourceHeader, pGameStateData, card);
    int32_t y = static_cast<int32_t>(getRegU32(&g_runtime->eeScheduler().runningContext(), 18));
    GuestFrame frame(0x70);
    sAMenuHeader &h = *box();
    CreateProgTTMenuBox(pSourceHeader, box(), 0x1e0);
    AMenuDrawBox(box(), 0);
    AMenuDisplayOptions(box());
    if (card == -1)
    {
        y = 0;
        DisplayProgTTMenuTitle(pSourceHeader, box(), 0x1e0, GetGeneralString(0x1e));
    }
    else if (card == 1 || card == 2)
    {
        y = 10;
        DisplayProgTTMenuTitle(pSourceHeader, box(), 0x1e0, GetGeneralString(card == 1 ? 0x1f : 0x20));
        h.spare = -10;
        scrollStartY() = static_cast<int16_t>(scrollStartY() - 0xc);
    }
    AMenuRectClipStart(box(), scrollStartY(), 10);
    y = DisplayProgressHeader(box(), pGameStateData, h.x, y + 0x28 + scrollPos());
    for (int32_t i = 1; i < 0x19; ++i)
        y = DisplayProgressEntry(box(), pGameStateData, h.x, y, 0x1e0, i);
    endPage(y, 5);
}

// An earlier version: the header and three entries from the jump position, and a jump list of zeroes.
void xxyBuildProgress(ptr32<sAMenuHeader> pSourceHeader, ptr32<_sFrontGameStateData> pGameStateData, int32_t card)
{
    GuestFrame frame(0x70);
    sAMenuHeader &h = *box();
    oldTitle(pSourceHeader, card);
    scrollHeight() = -1;
    oldBox();
    int32_t y = scrollStartY();
    for (int32_t i = G(scrollJumpPosG); !(G(scrollJumpPosG) + 3 < i); ++i)
    {
        if (i == 0)
            y = DisplayProgressHeader(box(), pGameStateData, h.x, y);
        else if (i < 0x19)
            y = DisplayProgressEntry(box(), pGameStateData, h.x, y, 0x1e0, i);
        else
            break;
    }
    const int16_t clipY = scrollStartY();
    for (int32_t k = 0; k < 25; ++k)
        fastScroll(k) = 0;
    fastScroll(25) = -1;
    AMenuRectClipEnd(box(), clipY);
    firstDrawPTT() = 0;
}

// As xxyBuildProgress, starting a character's height lower.
void xxxBuildProgress(ptr32<sAMenuHeader> pSourceHeader, ptr32<_sFrontGameStateData> pGameStateData, int32_t card)
{
    GuestFrame frame(0x70);
    sAMenuHeader &h = *box();
    oldTitle(pSourceHeader, card);
    scrollHeight() = -1;
    oldBox();
    int32_t y = scrollStartY() + G(fontVarG).pFont->chHeight;
    for (int32_t i = G(scrollJumpPosG); !(G(scrollJumpPosG) + 3 < i); ++i)
    {
        if (i == 0)
            y = DisplayProgressHeader(box(), pGameStateData, h.x, y);
        else if (i < 0x19)
            y = DisplayProgressEntry(box(), pGameStateData, h.x, y, 0x1e0, i);
        else
            break;
    }
    const int16_t clipY = scrollStartY();
    for (int32_t k = 0; k < 25; ++k)
        fastScroll(k) = 0;
    fastScroll(25) = -1;
    AMenuRectClipEnd(box(), clipY);
    firstDrawPTT() = 0;
}

// The first version: every entry from the scroll position, skipping those above the box by the heights measured on
// the first pass (kept in bytes of frontEndVarG), and a jump list of each entry's place.
void OldBuildProgress(ptr32<sAMenuHeader> pSourceHeader, ptr32<_sFrontGameStateData> pGameStateData, int32_t card)
{
    GuestFrame frame(0xa0);
    sAMenuHeader &h = *box();
    int8_t &headerHeight = at<int8_t>(kFrontEnd + 0x2d2u), &itemHeight = at<int8_t>(kFrontEnd + 0x2d3u);
    oldTitle(pSourceHeader, card);
    oldBox();
    int32_t y = scrollPos() + scrollStartY();
    {
        const int32_t y0 = y;
        const int32_t height = headerHeight;
        int8_t measured = height;
        if (height != -1 && y < -height)
            y += height;
        else
        {
            y = DisplayProgressHeader(box(), pGameStateData, h.x, y);
            measured = headerHeight;
        }
        if (measured == -1)
            headerHeight = static_cast<int8_t>(y - y0);
    }
    int8_t step = itemHeight;
    for (int32_t i = 1; i < 0x19; ++i)
    {
        const int32_t y0 = y;
        if (step != -1 && y < 0x14 - step)
        {
            y += step;
            continue;
        }
        y = DisplayProgressEntry(box(), pGameStateData, h.x, y, 0x1e0, i);
        step = itemHeight;
        if (step == -1)
        {
            step = static_cast<int8_t>(y - y0);
            const int32_t header = headerHeight;
            itemHeight = step;
            scrollHeight() = static_cast<int16_t>(h.height + header + step * 0x13);
        }
        if (!(y < 0xf1))
            break;
    }
    int32_t place = headerHeight;
    fastScroll(0) = 0;
    const int16_t clipY = scrollStartY();
    for (int32_t k = 1; k < 25; ++k, place += step)
        fastScroll(k) = -place;
    fastScroll(25) = -1;
    AMenuRectClipEnd(box(), clipY);
    firstDrawPTT() = 0;
}

// The time-trial records of the 24 tracks.
void BuildTimeTrials(ptr32<sAMenuHeader> pSourceHeader)
{
    GuestFrame frame(0x50);
    sAMenuHeader &h = *box();
    CreateProgTTMenuBox(pSourceHeader, box(), 0x1e0);
    AMenuDrawBox(box(), 0);
    AMenuDisplayOptions(box());
    DisplayProgTTMenuTitle(pSourceHeader, box(), 0x1e0, GetGeneralString(0x21));
    AMenuRectClipStart(box(), scrollStartY(), 0);
    int32_t y = scrollPos() + 0x28;
    for (int32_t i = 1; i < 0x19; ++i)
        y = DisplayTTEntry(box(), h.x, y, 0x1e0, i);
    endPage(y, 5);
}

// An earlier version: four entries from the jump position, and a jump list of zeroes.
void xxxBuildTimeTrials(ptr32<sAMenuHeader> pSourceHeader)
{
    GuestFrame frame(0x60);
    sAMenuHeader &h = *box();
    BuildProgTTSub(pSourceHeader, box(), 0x1e0, GetGeneralString(0x21));
    scrollHeight() = -1;
    oldBox();
    int32_t y = scrollStartY() + G(fontVarG).pFont->chHeight;
    for (int32_t i = G(scrollJumpPosG) + 1; i < 0x19;)
    {
        y = DisplayTTEntry(box(), h.x, y, 0x1e0, i);
        ++i;
        if (G(scrollJumpPosG) + 4 < i)
            break;
    }
    const int16_t clipY = scrollStartY();
    for (int32_t k = 23; k >= 0; --k)
        fastScroll(k) = 0;
    fastScroll(24) = -1;
    AMenuRectClipEnd(box(), clipY);
    firstDrawPTT() = 0;
}

// The first version: as OldBuildProgress without a header.
void OldBuildTimeTrials(ptr32<sAMenuHeader> pSourceHeader)
{
    GuestFrame frame(0x90);
    sAMenuHeader &h = *box();
    int8_t &itemHeight = at<int8_t>(kFrontEnd + 0x2d3u);
    BuildProgTTSub(pSourceHeader, box(), 0x1e0, GetGeneralString(0x21));
    oldBox();
    int32_t y = scrollPos() + scrollStartY();
    int8_t step = itemHeight;
    for (int32_t i = 1; i < 0x19; ++i)
    {
        const int32_t y0 = y;
        if (step != -1 && y < 0x14 - step)
        {
            y += step;
            continue;
        }
        y = DisplayTTEntry(box(), h.x, y0, 0x1e0, i);
        step = itemHeight;
        if (step == -1)
        {
            step = static_cast<int8_t>(y - y0);
            itemHeight = step;
            scrollHeight() = static_cast<int16_t>(h.height + step * 0x13);
        }
        if (!(y < 0xf1))
            break;
    }
    int32_t place = step * -0x18;
    for (int32_t k = 24; k >= 0; --k, place += step)
        fastScroll(k) = place;
    fastScroll(25) = -1;
    AMenuRectClipEnd(box(), scrollStartY());
    firstDrawPTT() = 0;
}

// The Track Editor's help: paragraphs of wrapped text and headings. Each paragraph's height is worked out once.
void BuildTrackEditorHelp(ptr32<sAMenuHeader> pSourceHeader)
{
    Locals f(0xa00);
    sAMenuHeader &h = *box();
    const uint32_t help = at<uint32_t>((sym::tEditVarG + 0x24cu)); // tEditVarG's help data: flags, height, text group; 0xffff ends.
    CreateProgTTMenuBox(pSourceHeader, box(), 0x168);
    AMenuDrawBox(box(), 0);
    AMenuDisplayOptions(box());
    DisplayProgTTMenuTitle(pSourceHeader, box(), 0x168, GetGeneralString(0xc));
    if (at<uint16_t>(help + 2u) == 0 && at<uint16_t>(help) != 0xffffu)
    {
        uint32_t i = 0;
        do
        {
            const uint32_t entry = help + i * 8u;
            if (at<uint16_t>(entry) == 1)
                at<uint16_t>(entry + 2u) = 1;
            else
            {
                const int32_t lines = AStringPrepareWrap(f.ptr<aStringTemp>(0), text(ptr32<sTextGroup>(at<uint32_t>(entry + 4u))), box());
                at<uint16_t>(help + i * 8u + 2u) = static_cast<uint16_t>(lines);
            }
            at<uint16_t>(entry + 2u) = static_cast<uint16_t>(at<uint16_t>(entry + 2u) * h.changeY);
            ++i;
        } while (at<uint16_t>(help + i * 8u) != 0xffffu);
    }
    h.changeY = static_cast<uint8>((G(fontVarG).pFont->chHeight >> 1) + 4);
    AMenuRectClipStart(box(), scrollStartY(), 0);
    int32_t y = scrollPos() + 0x28;
    for (int32_t i = 0; at<uint16_t>(help + static_cast<uint32_t>(i) * 8u) != 0xffffu; ++i)
        y = DisplayTEHelpEntry(box(), ptr32<sTrackEditorHelpData>(help + static_cast<uint32_t>(i) * 8u), h.x, y, 0x168, i);
    endPage(y, 3);
}

// On the first pass of a page: makes its box and draws the arrows.
void CreateProgTTMenuBox(ptr32<sAMenuHeader> pSourceHeader, ptr32<sAMenuHeader> pHeader, int32_t width)
{
    RC_DIVERT_IF_CHANGED(CreateProgTTMenuBox, pSourceHeader, pHeader, width);
    Locals f(0x70);
    if (firstDrawPTT() == 0)
        return;
    scrollPos() = 0;
    makeBox(pSourceHeader, pHeader, width);
    at<int8_t>(kFrontEnd + 0x2d1u) = 0;
    G(scrollJumpPosG) = 0;
    const sAMenuHeader &h = *pHeader;
    if (fastScroll(1) != -1)
        arrowTop(f, h, constant(sym::lit4_3275bc));
    if (G(scrollJumpPosG) > 0)
        arrowBottom(f, h, constant(sym::lit4_3275c0));
}

// Scrolls a page with the pad: holding down or up builds the scroll speed up to speed (down negative), letting go
// runs it down, and the position follows until the page's top or bottom. Back or select leaves the page.
int32_t PS2ScrollMenu(ptr32<sAMenuHeader> pHeader, int32_t speed)
{
    GuestFrame frame(0x70);
    if (at<int8_t>(kFrontEnd + 0x2d1u) > 0)
        return 0;
    const int32_t heldKey = AMenuNewProcessInput(-1, static_cast<eMenuInputStyle>(3), pHeader);
    int16_t &s = scrollSpeed();
    if (heldKey == 2)
    {
        at<uint32_t>(kFrontEnd + 0x2a8u) = 0;
        pHeader->dlgLines = 2;
        for (int32_t i = 0; i < speed; ++i)
            if (s > 0)
                s = static_cast<int16_t>(s - 1);
        if (-(speed * speed) < s)
        {
            s = static_cast<int16_t>(s - 1);
            if (!(s > 0) && !(s < -speed))
                s = static_cast<int16_t>(-speed);
        }
    }
    else if (heldKey == 1)
    {
        at<uint32_t>(kFrontEnd + 0x2a8u) = 0;
        pHeader->dlgLines = 1;
        for (int32_t i = 0; i < speed; ++i)
            if (s < 0)
                s = static_cast<int16_t>(s + 1);
        if (s < speed * speed)
        {
            s = static_cast<int16_t>(s + 1);
            if (!(s < 0) && !(speed < s))
                s = static_cast<int16_t>(speed);
        }
    }
    else
    {
        for (int32_t i = 0; i < speed; ++i)
        {
            if (s > 0)
                s = static_cast<int16_t>(s - 1);
            else if (s < 0)
                s = static_cast<int16_t>(s + 1);
        }
    }
    if (s > 0)
    {
        // Towards the top, where it stops.
        if (scrollPos() < -s)
            scrollPos() = static_cast<int16_t>(scrollPos() + s);
        else
        {
            scrollPos() = 0;
            s = 0;
        }
    }
    else if (G(bottomOfScrollG) == 0)
        scrollPos() = static_cast<int16_t>(scrollPos() + s);
    const int32_t hitKey = AMenuNewProcessInput(-1, static_cast<eMenuInputStyle>(2), pHeader);
    if (hitKey == 5 || hitKey == 6)
        leavePage();
    if (hitKey != 0)
        at<uint32_t>(kFrontEnd + 0x2a8u) = 0;
    return 0;
}

} // namespace rc

RC_DECOMP_FN_SETUP_HEAP(DisplayTrackGraphic, rc::fuzz::amenuTrack)
RC_DECOMP_FN_SETUP_HEAP(DisplayProgressHeader, rc::fuzz::amenuProgHeader)
RC_DECOMP_FN_SETUP_HEAP(DisplayProgressEntry, rc::fuzz::amenuProgEntry)
RC_DECOMP_FN_SETUP_HEAP(DisplayTTEntry, rc::fuzz::amenuTTEntry)
RC_DECOMP_FN_SETUP_HEAP(BuildProgTTSub, rc::fuzz::amenuPageSub)
RC_DECOMP_FN_SETUP(PTTScroll, rc::fuzz::amenuPageScroll)
RC_DECOMP_FN_SETUP(NewPTTScroll, rc::fuzz::amenuPageScroll)
RC_DECOMP_FN_SETUP_HEAP(BuildProgress, rc::fuzz::amenuPage)
RC_DECOMP_FN_SETUP_HEAP(xxyBuildProgress, rc::fuzz::amenuPage)
RC_DECOMP_FN_SETUP_HEAP(xxxBuildProgress, rc::fuzz::amenuPage)
RC_DECOMP_FN_SETUP_HEAP(OldBuildProgress, rc::fuzz::amenuPage)
RC_DECOMP_FN_SETUP_HEAP(BuildTimeTrials, rc::fuzz::amenuPage)
RC_DECOMP_FN_SETUP_HEAP(xxxBuildTimeTrials, rc::fuzz::amenuPage)
RC_DECOMP_FN_SETUP_HEAP(OldBuildTimeTrials, rc::fuzz::amenuPage)
RC_DECOMP_FN_SETUP_HEAP(BuildTrackEditorHelp, rc::fuzz::amenuPage)
RC_DECOMP_FN_SETUP_HEAP(CreateProgTTMenuBox, rc::fuzz::amenuPageSub)
RC_DECOMP_FN_SETUP(PS2ScrollMenu, rc::fuzz::amenuPageScroll)

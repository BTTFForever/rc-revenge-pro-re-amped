// Decompiled from AMENU.c: measuring and drawing a menu. AMenuDisplayAll draws one of three kinds: an ordinary menu
// (AMenuDisplay: title, dialog text, then each visible item through AMenuDisplayMIT), a scrolling list of text
// groups (AMenuDisplayNewScroll) or a character grid for entering names (AMenuDisplayAreaMenu). Each draws its
// text first, sizes the menu from what it drew and draws the box last (it lies behind by depth).
//
// An item's text depends on its variable: none (plain text), a choice ("*"-separated list, shown in the text's %s or
// on a second line), a number, a slider, or a tabbed line.

#include "rc_widescreen.h" // enhanced recomp: menu scale
#include "amenu.h"
#include "amenu_fuzz.h"

// Europe variants (NOTES.md "Europe version"): AMenuDisplayNewScroll (clears the menu's spare offset and always clips
// to the snapped height), AMenuDisplayOptions (the key-hint line and the whole-screen box 16 lines lower, text offset
// +4 instead of -4); AMenuDisplayAreaMenu only stores in another order (the same body).
RC_VERSION_VARIANT(AMenuDisplayNewScroll)
RC_VERSION_VARIANT(AMenuDisplayAreaMenu)
RC_VERSION_VARIANT(AMenuDisplayOptions)

namespace rc {

using namespace amenu;

namespace {

// 0 plain, 1 choice, 2 number, 3 slider, 4 tabbed.
int32_t displayType(const sMenuItemStd &menuItem)
{
    int32_t type = 0;
    if (menuItem.pMenuVar)
    {
        const uint16_t flags = menuItem.pMenuVar->flags;
        type = (flags & 2u) != 0 ? 2 : (flags & 4u) != 0 ? 3 : 1;
    }
    if ((menuItem.flags & 2u) != 0)
        type = 4;
    return type;
}

// A tabbed line: the sections between 0xaa characters, each printed at its tab stop.
void tabPrint(int32_t posX, int32_t posY, ptr32<char> pBuffer, ptr32<char> pString)
{
    int32_t dest = 0, section = 0;
    for (uint32_t p = pString.address;; ++p)
    {
        const uint8_t c = at<uint8_t>(p);
        if (c == 0xaa || c == 0)
        {
            pBuffer[dest] = 0;
            const uint32_t tab = at<uint16_t>(G(pMenuSystemG).address + 0x82u + static_cast<uint32_t>(section) * 2u);
            dest = 0;
            ++section;
            FontPrint(i16(static_cast<int32_t>(tab) + posX), i16(posY), pBuffer);
        }
        else
            pBuffer[dest++] = static_cast<char>(c);
        if (at<int8_t>(p) == 0)
            return;
    }
}

} // namespace

void AMenuDisplayAll(ptr32<sAMenuHeader> pHeader)
{
    ptr32<sAMenuHeader> parent(nullptr);
    {
        GuestFrame frame(0x20);
        GfxP2_ZWriteEnable();
        if ((pHeader->evenMoreFlags & 0x40u) != 0)
            AMenuDisplayNewScroll(pHeader);
        else if ((pHeader->flags & 0x4000u) != 0)
            AMenuDisplayAreaMenu(pHeader);
        else
            AMenuDisplay(pHeader);
        if ((pHeader->flags & 2u) == 0)
            return;
        parent = pHeader->pLinkMenuReturn;
        if (!parent)
            return;
    }
    // The menu this one came from is drawn behind it.
    if ((parent->flags & 0x4000u) != 0)
        AMenuDisplayAreaMenu(parent);
    else
        AMenuDisplay(parent);
}

// Prints the title at the menu's x (centred with flag 4) and posY (the menu's y for 0), widening the menu to fit.
ptr32<char> AMenuDisplayTitle(ptr32<sAMenuHeader> pHeader, ptr32<sUIcolourSet> pColourSet, int32_t posY)
{
    GuestFrame frame(0x60);
    const ptr32<char> pTitleString = text(pHeader->pTitleText);
    if (!pTitleString)
        return pTitleString;
    const int32_t titleWidth = static_cast<int32_t>(FontLength(pTitleString));
    if (static_cast<int32_t>(pHeader->width) < titleWidth)
    {
        pHeader->width = static_cast<uint16>(titleWidth);
        AMenuSnapsize(pHeader);
    }
    int32_t printPosX = pHeader->x;
    FontSetColour(pColourSet->titRed, pColourSet->titGreen, pColourSet->titBlue);
    if ((pHeader->flags & 4u) != 0)
        printPosX = AMenuStringCentre(pHeader, pTitleString, titleWidth);
    FontPrint(i16(printPosX), posY != 0 ? i16(posY) : pHeader->y, pTitleString);
    return pTitleString;
}

// Draws one item at (posX, posY) and returns the y of the next line.
int32_t AMenuDisplayMIT(ptr32<sAMenuHeader> pHeader, ptr32<sMenuItemStd> pMenuItem, ptr32<sUIcolourSet> pColourSet, int32_t posX, int32_t posY, int32_t)
{
    Locals f(0x290);
    const ptr32<char> buffer = f.ptr<char>(0), buffer2 = f.ptr<char>(0x70), p1p2Buffer = f.ptr<char>(0xe0);
    sAMenuHeader &h = *pHeader;
    sMenuItemStd &menuItem = *pMenuItem;
    int32_t changeY = h.changeY;
    ptr32<char> pMITString = text(menuItem.pTextGroup);
    // Items marked for a player get " - P1" or " - P2" after their text.
    const auto mark = [&](uint32_t suffix)
    {
        if (pMITString != p1p2Buffer)
            std::strcpy(p1p2Buffer.get(), pMITString.get());
        pMITString = p1p2Buffer;
        std::strcat(p1p2Buffer.get(), ptr32<char>(suffix).get());
    };
    if ((menuItem.statusFlags & 0x10u) != 0)
        mark(kTextP1);
    if ((menuItem.statusFlags & 0x20u) != 0)
        mark(kTextP2);
    // In the controller setup menus a line with a key picture is three pixels taller for each.
    if (h.selectionTag == 0x10000282u || h.selectionTag == 0x10000284u)
        for (uint32_t i = 0; i < std::strlen(pMITString.get()); ++i)
            if (pMITString[i] == '}')
                changeY += 3;
    const int32_t type = displayType(menuItem);
    if (menuItem.pMenuVar)
    {
        const ptr32<int32> feedback = menuItem.pMenuVar->pFeedbackVar;
        if (feedback)
            menuItem.pMenuVar->value = *feedback;
    }
    const bool centre = (h.flags & 4u) != 0;
    const auto centred = [&](ptr32<char> string) { return AMenuStringCentre(pHeader, string, -1); };
    // The text without its %s on one line and the value on the next, each centred if the menu is.
    const auto twoLines = [&]
    {
        AmenuRemovePercentS(buffer2, pMITString);
        if (centre)
            posX = centred(buffer2);
        int32_t x = posX;
        FontPrint(i16(posX), i16(posY), buffer2);
        posY += changeY;
        if ((h.flags & 4u) != 0)
            x = centred(buffer);
        FontPrint(i16(x), i16(posY), buffer);
        return posY + changeY;
    };
    // The value in the text's %s.
    const auto oneLine = [&]
    {
        guestSprintf(buffer2, pMITString, buffer);
        if ((h.flags & 4u) != 0)
            posX = centred(buffer2);
        FontPrint(i16(posX), i16(posY), buffer2);
        return posY + changeY;
    };
    switch (type)
    {
    case 0:
        if (!pMITString)
            return posY + changeY;
        if (centre)
            posX = centred(pMITString);
        FontPrint(i16(posX), i16(posY), pMITString);
        return posY + changeY;
    case 1:
    {
        // The value-th choice of the variable's list.
        const sMenuItemVar &var = *menuItem.pMenuVar;
        const ptr32<char> pMVARString = text(var.pTextGroup);
        int32_t srcPos = 0, destPos = 0, starCount = 0;
        if (pMVARString[0] != 0)
        {
            bool found = true;
            while (starCount != var.value)
            {
                const char c = pMVARString[srcPos];
                ++srcPos;
                if (c == '*')
                    ++starCount;
                if (pMVARString[srcPos] == 0)
                {
                    found = false;
                    break;
                }
            }
            if (found)
                while (pMVARString[srcPos] != 0 && pMVARString[srcPos] != '*')
                    buffer[destPos++] = pMVARString[srcPos++];
        }
        buffer[destPos] = 0;
        return (var.flags & 0x10u) != 0 ? twoLines() : oneLine();
    }
    case 2:
        guestSprintf(buffer, ptr32<char>(kFormatD), menuItem.pMenuVar->value);
        return (menuItem.pMenuVar->flags & 0x10u) != 0 ? twoLines() : oneLine();
    case 3:
    {
        if (centre)
        {
            const int32_t length = static_cast<int32_t>(FontLength(pMITString));
            posX += (static_cast<int32_t>(h.width) - (length + menuItem.pMenuVar->maxValue / 2 + 0x14)) / 2;
        }
        const auto colours = [&]
        {
            f.sb(0x160, pColourSet->sRed);
            f.sb(0x161, pColourSet->sGreen);
            f.sb(0x162, pColourSet->sBlue);
            f.sb(0x163, pColourSet->tRed);
            f.sb(0x164, pColourSet->tGreen);
            f.sb(0x165, pColourSet->tBlue);
        };
        if ((menuItem.pMenuVar->flags & 0x10u) != 0)
        {
            // The text, then the slider across the menu on the next line.
            if ((h.flags & 4u) != 0)
                posX = centred(pMITString);
            FontPrint(i16(posX), i16(posY), pMITString);
            posY += changeY;
            f.sh(0x15e, h.changeY - 4u);
            f.sh(0x150, static_cast<uint16_t>(h.x));
            f.sh(0x152, posY + 2);
            f.sh(0x15c, h.width);
            const sMenuItemVar &var = *menuItem.pMenuVar;
            f.sh(0x156, static_cast<uint32_t>(var.value));
            f.sh(0x158, static_cast<uint32_t>(var.minValue));
            f.sh(0x15a, static_cast<uint32_t>(var.maxValue));
            f.sh(0x154, menuItem.flags);
            colours();
            DisplayUIbar(f.ptr<sBarDetails>(0x150));
            return posY + changeY;
        }
        // The text, and a half-scale slider against the menu's right edge on the same line.
        FontPrint(i16(posX), i16(posY), pMITString);
        const sMenuItemVar &var = *menuItem.pMenuVar;
        const int32_t half = var.maxValue / 2;
        f.sh(0x15e, h.changeY - 2u);
        f.sh(0x150, static_cast<uint16_t>(h.x) + h.width - static_cast<uint32_t>(half));
        f.sh(0x152, posY + 1);
        f.sh(0x156, static_cast<uint32_t>(var.value / 2));
        f.sh(0x158, static_cast<uint32_t>(var.minValue / 2));
        f.sh(0x15a, static_cast<uint32_t>(half));
        f.sh(0x154, menuItem.flags);
        colours();
        f.sh(0x15c, static_cast<uint32_t>(half));
        DisplayUIbar(f.ptr<sBarDetails>(0x150));
        return posY + changeY;
    }
    default:
    {
        if ((menuItem.flags & 8u) == 0)
        {
            tabPrint(posX, posY, buffer, pMITString);
            return posY + changeY;
        }
        // The line that sets the tab stops: each "*" puts a stop at the width of the text so far, and the text is
        // printed without them.
        const ptr32<char> tabFreeString = f.ptr<char>(0x170);
        const uint32_t ms = G(pMenuSystemG).address;
        for (uint32_t tab = 1; tab < 10; ++tab)
            at<uint16_t>(ms + 0x82u + tab * 2u) = 0;
        at<uint16_t>(ms + 0x82u) = 0;
        int32_t tabPos = 0, destString = 0, tabNo = 1;
        for (uint32_t i = 0; i < std::strlen(pMITString.get()); ++i)
        {
            const char c = pMITString[i];
            if (c == '*')
            {
                at<uint16_t>(G(pMenuSystemG).address + 0x82u + static_cast<uint32_t>(tabNo) * 2u) = static_cast<uint16_t>(tabPos);
                ++tabNo;
            }
            else
            {
                at<char>(kCharString) = c;
                tabPos += static_cast<int32_t>(FontLength(ptr32<char>(kCharString)));
                tabFreeString[destString++] = c;
            }
        }
        tabFreeString[destString] = 0;
        FontPrint(i16(posX), i16(posY), tabFreeString);
        menuSystem().gameDataSet2Pos = 0;
        menuSystem().gameDataSet1Pos = 0;
        return posY + changeY;
    }
    }
}

// The width an item needs, or maxWidth if that is more. For a choice item every choice but the last is measured;
// a tab-setting line sets the tab stops (from stop 0) and returns its own width whatever maxWidth was.
int32_t AmenuCalcSizeMIT(ptr32<sMenuItemStd> pMenuItem, int32_t maxWidth)
{
    Locals f(0x680);
    const ptr32<char> buffer = f.ptr<char>(0), buffer2 = f.ptr<char>(0x70);
    sMenuItemStd &menuItem = *pMenuItem;
    const ptr32<char> pMITString = text(menuItem.pTextGroup);
    const auto widen = [&](uint32_t width)
    {
        if (maxWidth < static_cast<int32_t>(width))
            maxWidth = static_cast<int32_t>(width);
    };
    const auto textWidth = [&]
    {
        if ((menuItem.pMenuVar->flags & 0x10u) != 0)
        {
            AmenuRemovePercentS(buffer2, pMITString);
            return FontLength(buffer2);
        }
        return FontLength(pMITString);
    };
    switch (displayType(menuItem))
    {
    case 0:
        if (pMITString)
            widen(FontLength(pMITString));
        return maxWidth;
    case 1:
    {
        const ptr32<char> pMVARString = text(menuItem.pMenuVar->pTextGroup);
        const auto choice = [&](int32_t index) { return f.ptr<char>(0xe0u + static_cast<uint32_t>(index) * 0x40u); };
        int32_t choiceCount = 0, destPos = 0;
        for (uint32_t i = 0; i < std::strlen(pMVARString.get()); ++i)
        {
            const char c = pMVARString[i];
            if (c == '*')
            {
                choice(choiceCount)[destPos] = 0;
                ++choiceCount;
                destPos = 0;
            }
            else
                choice(choiceCount)[destPos++] = c;
        }
        choice(choiceCount)[destPos] = 0;
        widen(textWidth());
        for (int32_t i = 0; i < choiceCount; ++i)
        {
            if ((menuItem.pMenuVar->flags & 0x10u) != 0)
                widen(FontLength(choice(i)));
            else
            {
                guestSprintf(buffer, pMITString, choice(i));
                widen(FontLength(buffer));
            }
        }
        return maxWidth;
    }
    case 2:
        guestSprintf(buffer, ptr32<char>(kFormatD), menuItem.pMenuVar->value);
        widen(textWidth());
        if ((menuItem.pMenuVar->flags & 0x10u) != 0)
            widen(FontLength(buffer));
        else
        {
            guestSprintf(buffer2, pMITString, buffer);
            widen(FontLength(buffer2));
        }
        return maxWidth;
    case 3:
        if ((menuItem.pMenuVar->flags & 0x10u) != 0)
            widen(FontLength(pMITString));
        else
        {
            const uint32_t width = FontLength(pMITString);
            widen(width + static_cast<uint32_t>(menuItem.pMenuVar->maxValue / 2 + 0x14));
        }
        return maxWidth;
    default:
    {
        if ((menuItem.flags & 8u) == 0)
            return maxWidth;
        int32_t tabNo = 0, tabPos = 0;
        for (uint32_t i = 0; i < std::strlen(pMITString.get()); ++i)
        {
            const char c = pMITString[i];
            if (c == '*')
            {
                at<uint16_t>(G(pMenuSystemG).address + 0x82u + static_cast<uint32_t>(tabNo) * 2u) = static_cast<uint16_t>(tabPos);
                ++tabNo;
            }
            else
            {
                at<char>(kCharString) = c;
                tabPos += static_cast<int32_t>(FontLength(ptr32<char>(kCharString)));
            }
        }
        return tabPos;
    }
    }
}

// Sets a menu's width (the widest of its title and items) and height (a line for the title, each visible item and
// each dialog line, two for an item with its value below), or for a scrolling menu the number of lines that fit its
// height. All eight text offsets become the height the snap added.
void AMenuCalcSize(ptr32<sAMenuHeader> pHeader)
{
    Locals f(0x9f0);
    sAMenuHeader &h = *pHeader;
    int32_t titWidth = 0, lineCount = 0;
    const ptr32<char> pTitleString = text(h.pTitleText);
    const ptr32<char> pDialogString = text(h.pDialogTxt);
    int32_t maxWidth = h.width;
    if (pTitleString)
    {
        titWidth = static_cast<int32_t>(FontLength(pTitleString));
        lineCount = 1;
    }
    if (maxWidth < titWidth)
        maxWidth = titWidth;
    if ((h.evenMoreFlags & 0x40u) != 0)
    {
        // A scrolling menu's list is text groups, ended by an empty one.
        const uint32_t list = h.pMenuStart.address;
        for (uint32_t i = 0; at<uint32_t>(list + i * 0x10u) != 0; ++i)
            ++lineCount;
    }
    else
    {
        for (ptr32<sMenuItemStd> p = h.pMenuStart; p; p = p->pNext)
        {
            if ((p->flags & 0x20u) != 0)
                continue;
            const int32_t mitWidth = AmenuCalcSizeMIT(p, maxWidth);
            if (maxWidth < mitWidth)
                maxWidth = mitWidth;
            if (p->pMenuVar && (p->pMenuVar->flags & 0x10u) != 0)
                ++lineCount;
            ++lineCount;
        }
    }
    int32_t dialogLines = h.dlgLines;
    h.width = static_cast<uint16>(maxWidth);
    if (dialogLines == -1)
    {
        h.dlgLines = 0;
        dialogLines = 0;
        if (pDialogString)
        {
            dialogLines = static_cast<int8_t>(AStringPrepareWrap(f.ptr<aStringTemp>(0), pDialogString, pHeader));
            h.dlgLines = static_cast<int8>(dialogLines);
        }
    }
    lineCount += dialogLines;
    uint32_t height;
    if ((h.flags & 0x2000u) != 0 || (h.evenMoreFlags & 0x40u) != 0)
    {
        height = h.height;
        const int32_t lines = ee_div(static_cast<int32_t>(height), h.changeY) - 1;
        h.scrollItems = static_cast<int8>(lines);
        if ((lines & 0xff) == 0)
            h.scrollItems = 1;
    }
    else
    {
        h.height = static_cast<uint16>(lineCount * h.changeY);
        height = h.height;
    }
    AMenuSnapsize(pHeader);
    const int8 yShift = static_cast<int8>(h.height - height);
    int8 *offsets = &h.tofspEnglish;
    for (int i = 0; i < 8; ++i)
        offsets[i] = yShift;
}

// An ordinary menu.
void AMenuDisplay(ptr32<sAMenuHeader> pHeader)
{
    sAMenuHeader &h = *pHeader;
    {
        Locals f(0xb0);
        int32_t menuItem = 0;
        const ptr32<sUIcolourSet> pColourSet = AMenuGetColourSet(pHeader);
        h.changeY = fontLineHeight();
        if ((h.evenMoreFlags & 0x200u) != 0)
            h.changeY = static_cast<uint8>(h.changeY + 2);
        AMenuCalcSize(pHeader);
        const int32_t fiddle = GetNewMenuFiddleFactor(pHeader);
        f.sw(0, static_cast<uint32_t>(static_cast<int32_t>(h.y))); // fixedPosY
        const int32_t textYPosFiddleFactor = fiddle / 2;
        ptr32<sMenuItemStd> pMenuItem = h.pMenuStart;
        int32_t posX = h.x;
        int32_t posY = f.at<int32_t>(0) + textYPosFiddleFactor;
        const ptr32<char> title = AMenuDisplayTitle(pHeader, pColourSet, posY);
        const ptr32<char> pDialogString = text(h.pDialogTxt);
        if (title)
        {
            posX += h.changeX;
            posY += h.changeY;
        }
        int32_t lineCount = 0;
        if (pDialogString)
        {
            FontSetColour(pColourSet->titRed, pColourSet->titGreen, pColourSet->titBlue);
            lineCount = AStringWrap(posX, posY, pDialogString, pHeader);
            AMenuSnapsize(pHeader);
        }
        posY += lineCount * h.changeY;
        while (pMenuItem)
        {
            // A scrolling menu shows scrollItems items.
            const int32_t scrollItems = h.scrollItems;
            if (scrollItems != 0 && !(menuItem - (h.selection - h.scrollStart) < scrollItems))
                break;
            if ((pMenuItem->flags & 0x20u) != 0)
            {
                pMenuItem = pMenuItem->pNext;
                continue;
            }
            AMenuSetMITColour(pHeader, pMenuItem, pColourSet, menuItem);
            if ((h.moreFlags & 0x800u) != 0)
                FontSetColour(pColourSet->sRed, pColourSet->sGreen, pColourSet->sBlue);
            posY = AMenuDisplayMIT(pHeader, pMenuItem, pColourSet, posX, posY, 0);
            ++menuItem;
            posX += h.changeX;
            pMenuItem = pMenuItem->pNext;
        }
        h.height = static_cast<uint16>(posY - textYPosFiddleFactor - h.y);
        AMenuSnapsize(pHeader);
        h.y = i16(f.at<int32_t>(0));
    }
    AMenuDrawBox(pHeader, 0);
}

// A scrolling list: every line is drawn, moved up by the scroll position and clipped to the menu's box.
void AMenuDisplayNewScroll(ptr32<sAMenuHeader> pHeader)
{
    sAMenuHeader &h = *pHeader;
    {
        GuestFrame frame(0xa0);
        int32_t menuItem = 0;
        const ptr32<sUIcolourSet> pColourSet = AMenuGetColourSet(pHeader);
        h.changeY = fontLineHeight();
        AMenuCalcSize(pHeader);
        int32_t posY = h.y, posX = h.x;
        const ptr32<char> title = AMenuDisplayTitle(pHeader, pColourSet, posY);
        const ptr32<char> pDialogString = text(h.pDialogTxt);
        if (title)
        {
            posX += h.changeX;
            posY += h.changeY;
        }
        int32_t lineCount = 0;
        if (pDialogString)
        {
            FontSetColour(pColourSet->titRed, pColourSet->titGreen, pColourSet->titBlue);
            lineCount = AStringWrap(posX, posY, pDialogString, pHeader);
            AMenuSnapsize(pHeader);
        }
        const int32_t changeY = h.changeY;
        posY += lineCount * changeY;
        // Where the lines start below the menu's top (in a byte, from the low byte of y).
        h.scrollStart = static_cast<int8>(posY - (static_cast<uint16_t>(h.y) & 0xff));
        const int32_t yOffset = h.scrollStart;
        const int32_t room = static_cast<int32_t>(h.height) - yOffset - (G(scrollLinesG) + 8);
        const int32_t snapYscrollRemainder = changeY != 0 ? room % changeY : room;
        const int32_t snapYscroll = (lineCount + 1) * changeY + (changeY - snapYscrollRemainder);
#if RC_VERSION_EU
        // Europe: the spare offset (AMenuRectClipStart's) cleared, and no line less for a menu at x 0xf5.
        h.spare = 0;
        AMenuRectClipStart(pHeader, yOffset, snapYscroll);
#else
        AMenuRectClipStart(pHeader, yOffset, h.x != 0xf5 ? snapYscroll : snapYscroll - changeY);
#endif
        firstDrawPTT() = 0;
        uint32_t pTGRPList = h.pMenuStart.address;
        if (at<uint32_t>(pTGRPList) != 0)
        {
            do
            {
                if (h.selection == menuItem)
                    FontSetColour(pColourSet->sRed, pColourSet->sGreen, pColourSet->sBlue);
                else
                    FontSetColour(pColourSet->tRed, pColourSet->tGreen, pColourSet->tBlue);
                int32_t printPosX = posX;
                const ptr32<char> pTextItem = text(ptr32<sTextGroup>(pTGRPList));
                if ((h.flags & 4u) != 0)
                    printPosX = AMenuStringCentre(pHeader, pTextItem, -1);
                const int32_t scroll = at<uint16_t>(kFrontEnd + 0x2cau);
                pTGRPList += 0x10u;
                ++menuItem;
                FontPrint(i16(printPosX), i16(posY - scroll), pTextItem);
                posY += menuSystem().pActiveMenu->changeY; // The active menu's, not this one's.
            } while (at<uint32_t>(pTGRPList) != 0);
        }
        AMenuRectClipEnd(pHeader, h.scrollStart);
        AMenuSnapsize(pHeader);
    }
    AMenuDrawBox(pHeader, 0);
}

// Lays out the character grid of a name-entry menu, once per menu: the columns and rows, where the characters end,
// and the OK and cancel texts that follow them in the character list (0xaa between). Sets the menu's height.
ptr32<sAreaMenuInfo> AMenuAreaMenuSetup(ptr32<sAMenuHeader> pHeader)
{
    GuestFrame frame(0xb0);
    const ptr32<sAreaMenuInfo> pAreaMenu(G(pMenuSystemG).address + 0x38u);
    sAreaMenuInfo &info = *pAreaMenu;
    if (info.pHeader == pHeader)
        return pAreaMenu;
    sAMenuHeader &h = *pHeader;
    uint32_t pOKtext = 0;
    int32_t newRowFlag = 0, col = 0, row = 0;
    int32_t posY = h.y;
    h.changeY = fontLineHeight();
    info.pHeader = pHeader;
    info.colCount = static_cast<uint16>(static_cast<int32_t>(h.scrollItems));
    info.rowCount = static_cast<uint16>(static_cast<int32_t>(h.scrollStart));
    const ptr32<char> pChars = text(h.pCharList);
    menuSystem().areaMenuEnd = 0;
    for (uint32_t i = 0; i < std::strlen(pChars.get()); ++i)
    {
        newRowFlag = 0;
        if (static_cast<uint8_t>(pChars[i]) == 0xaa)
        {
            pOKtext = pChars.address + i + 1u;
            break;
        }
        ++col;
        if (!(col < static_cast<int32_t>(info.colCount)))
        {
            col = 0;
            ++row;
            newRowFlag = 1;
            posY += h.changeY;
        }
        menuSystem().areaMenuEnd = static_cast<int16>(menuSystem().areaMenuEnd + 1);
    }
    info.endCol = static_cast<uint16>(col);
    info.endRow = static_cast<uint16>(row);
    uint32_t n = 0;
    while (at<uint8_t>(pOKtext + n) != 0xaa)
    {
        at<uint8_t>(pAreaMenu.address + 8u + n) = at<uint8_t>(pOKtext + n);
        ++n;
    }
    at<uint8_t>(pAreaMenu.address + 8u + n) = 0;
    info.pCancelText = ptr32<char>(pOKtext + n + 1u);
    h.scrollStart = static_cast<int8>(row);
    info.rowCount = static_cast<uint16>(row);
    if (newRowFlag == 0)
        posY += h.changeY;
    posY += h.changeY * 4;
    h.height = static_cast<uint16>(posY - h.y);
    AMenuSnapsize(pHeader);
    return pAreaMenu;
}

// 1 if the selected row is past the grid's last, 2 if it is the last row and the column past its last character.
int32_t AMenuAreaMenuCheckValidSelection(ptr32<sAMenuHeader> pHeader)
{
    GuestFrame frame(0x20);
    const ptr32<sAreaMenuInfo> pAreaMenu = AMenuAreaMenuSetup(pHeader);
    const int32_t row = pHeader->rowSelect, rowCount = pAreaMenu->rowCount;
    if (rowCount < row)
        return 1;
    if (row != rowCount)
        return 0;
    return pHeader->colSelect < static_cast<int32_t>(pAreaMenu->endCol) ? 0 : 2;
}

// A name-entry menu: the grid of characters with the selected one highlighted, the name so far with a cursor, then
// OK and cancel. On the first, measuring pass (amenuFirstDrawFlag) nothing is printed.
void AMenuDisplayAreaMenu(ptr32<sAMenuHeader> pHeader)
{
    GuestFrame frame(0xc0);
    sAMenuHeader &h = *pHeader;
    int8_t &firstDraw = at<int8_t>(kFrontEnd + 0x2d5u);
    int32_t col = 0, row = 0, newRowFlag = 0;
    h.changeX = 0x16;
    const ptr32<sUIcolourSet> pColourSet = AMenuGetColourSet(pHeader);
    const sUIcolourSet &c = *pColourSet;
    const auto textColour = [&] { FontSetColour(c.tRed, c.tGreen, c.tBlue); };
    const auto selectColour = [&] { FontSetColour(c.sRed, c.sGreen, c.sBlue); };
    if (firstDraw == 0)
        AMenuDisplayTitle(pHeader, pColourSet, 0);
    const ptr32<sAreaMenuInfo> pAreaMenu = AMenuAreaMenuSetup(pHeader);
    const int32_t colCount = menuSystem().areaMenu.colCount;
    int32_t posY = h.y, xColStart = h.x;
    if (firstDraw == 0)
        AMenuDisplayTitle(pHeader, pColourSet, posY);
    if (text(h.pTitleText))
        posY += h.changeY;
    const int32_t dataWidth = colCount * h.changeX;
    if (!(dataWidth < static_cast<int32_t>(h.width)))
        h.width = static_cast<uint16>(dataWidth);
    else
        xColStart += (static_cast<int32_t>(h.width) - dataWidth) / 2;
    textColour();
    int32_t posX = xColStart;
    const ptr32<char> pChars = text(h.pCharList);
    menuSystem().areaMenuEnd = 0;
    const ptr32<char> cursor(kCharString);
    const ptr32<char> pOKtext(pAreaMenu.address + 8u);
    for (uint32_t i = 0; i < std::strlen(pChars.get()); ++i)
    {
        newRowFlag = 0;
        const uint8_t ch = static_cast<uint8_t>(pChars[i]);
        if (ch == 0xaa)
            break;
        at<uint8_t>(kCharString) = ch;
        if (col == h.colSelect && row == h.rowSelect)
        {
            selectColour();
            if (firstDraw == 0)
                FontPrint(i16(posX), i16(posY), cursor);
            textColour();
        }
        else if (firstDraw == 0)
            FontPrint(i16(posX), i16(posY), cursor);
        ++col;
        posX += h.changeX;
        if (!(col < colCount))
        {
            ++row;
            col = 0;
            posX = xColStart;
            posY += h.changeY;
            newRowFlag = 1;
        }
        menuSystem().areaMenuEnd = static_cast<int16>(menuSystem().areaMenuEnd + 1);
    }
    at<uint8_t>(kCharString) = '_';
    if (newRowFlag == 0)
        posY += h.changeY;
    if (firstDraw == 0)
    {
        const ptr32<char> input(G(pMenuSystemG).address + 0xc4u);
        if (std::strlen(input.get()) != 0)
            FontPrint(h.x, i16(posY), input);
        const uint32_t length = FontLength(input);
        FontPrint(i16(static_cast<int32_t>(static_cast<uint16_t>(h.x) + length)), i16(posY), cursor);
    }
    posY += h.changeY;
    if (h.rowSelect == row + 1)
        selectColour();
    int32_t x = AMenuStringCentre(pHeader, pOKtext, -1);
    if (firstDraw == 0)
        FontPrint(i16(x), i16(posY), pOKtext);
    textColour();
    posY += h.changeY;
    if (h.rowSelect == row + 2)
        selectColour();
    x = AMenuStringCentre(pHeader, pAreaMenu->pCancelText, -1);
    if (firstDraw == 0)
        FontPrint(i16(x), i16(posY), pAreaMenu->pCancelText);
    textColour();
    posY += h.changeY;
    h.height = static_cast<uint16>(posY - h.y);
    AMenuSnapsize(pHeader);
    if (firstDraw == 0)
        AMenuDrawBox(pHeader, 0);
    firstDraw = 0;
}

// The line of key hints under a menu (its options list, or the selected item's own for status flags 2 and 4), drawn
// as a small menu of its own. A menu without one gets the whole-screen box and the screen-adjust mode instead.
void AMenuDisplayOptions(ptr32<sAMenuHeader> pHeader)
{
    Locals f(0xb0);
    const ptr32<sAMenuHeader> pInstHeader = f.ptr<sAMenuHeader>(0);
    sAMenuHeader &inst = *pInstHeader;
    sAMenuHeader &h = *pHeader;
    std::memset(&inst, 0, 0x80);
    inst.pBorderSet = h.pBorderSet;
    inst.pColourSet = h.pColourSet;
    const ptr32<sTextGroup> options = h.pOptionsList;
    if (!options)
    {
        if (at<uint8_t>(kFrontEnd + 0x2dau) != 0)
            return;
        inst.flags = 0x24;
        inst.x = 0x1e;
        inst.y = 8;
        inst.width = 0x23a;
        inst.height = RC_VERSION_EU ? 0xe2 : 0xd2;
        menuSystem().screenAdjust = 1;
        AMenuSnapsize(pInstHeader);
        AMenuDrawScreenBox(pInstHeader, 0);
        return;
    }
    inst.pTitleText = options;
    if (h.pMenuStart && (h.evenMoreFlags & 0x40u) == 0)
    {
        const uint8_t status = item(pHeader, h.selection)->statusFlags; // From address 0x17 for no item.
        if ((status & 2u) != 0)
            inst.pTitleText = h.pOptionsList2;
        else if ((status & 4u) != 0)
            inst.pTitleText = h.pOptionsList3;
    }
    if (!inst.pTitleText)
        inst.pTitleText = options;
    inst.flags = 0x824;
    inst.height = 0x28;
    inst.y = RC_VERSION_EU ? 0xd8 : 0xc8;
    inst.width = static_cast<uint16>(FontLength(text(inst.pTitleText)));
    inst.changeY = 0xf;
    SetTextOffsetFiddleFactor(pInstHeader, RC_VERSION_EU ? 4 : -4);
    menuSystem().screenAdjust = 0;
    const widescreen::MenuRowScope bottom(1); // enhanced recomp: the menu scale keeps the hints at the bottom
    AMenuDisplayAll(pInstHeader);
}

int32_t AMenuDisplayDialogText(int32_t posX, int32_t posY, ptr32<sAMenuHeader> pHeader, ptr32<sUIcolourSet> pColourSet)
{
    GuestFrame frame(0x60);
    const ptr32<char> pDialogString = text(pHeader->pDialogTxt);
    if (!pDialogString)
        return 0;
    FontSetColour(pColourSet->titRed, pColourSet->titGreen, pColourSet->titBlue);
    const int32_t lineCount = AStringWrap(posX, posY, pDialogString, pHeader);
    AMenuSnapsize(pHeader);
    return lineCount;
}

int32_t AMenuTabPrint(int32_t posX, int32_t posY, ptr32<char> pBuffer, ptr32<char> pString)
{
    GuestFrame frame(0x90);
    tabPrint(posX, posY, pBuffer, pString);
    return posY;
}

// The title over a progress or time-trial list: on one line if it fits the menu, else wrapped (and the list starts
// that many lines lower). Returns the y under it.
int32_t DisplayProgTTMenuTitle(ptr32<sAMenuHeader>, ptr32<sAMenuHeader> pHeader, int32_t, ptr32<char> pTitle)
{
    GuestFrame frame(0x30);
    sAMenuHeader &h = *pHeader;
    if (pTitle)
    {
        FontSetColour(0, 0x7f, 0x7f);
        if (FontLength(pTitle) < h.width)
        {
            const int32_t x = AStringCentre(h.x, h.width, pTitle, -1);
            FontPrint(i16(x), h.y, pTitle);
            G(scrollLinesG) = 0;
        }
        else
        {
            const uint32_t x = static_cast<uint16_t>(h.x) + 0x10u;
            h.changeY = static_cast<uint8>(G(fontVarG).pFont->chHeight >> 1);
            h.x = i16(static_cast<int32_t>(x));
            const int32_t lines = AStringWrap(i16(static_cast<int32_t>(x)), h.y, pTitle, pHeader);
            const int32_t extra = (lines - 1) * h.changeY;
            h.x = i16(h.x - 0x10);
            G(scrollLinesG) = extra;
        }
    }
    at<int16_t>(kFrontEnd + 0x2ceu) = static_cast<int16_t>(G(fontVarG).pFont->chHeight >> 1);
    FontSetColour(0x7f, 0x7f, 0x7f);
    return h.y + h.changeY;
}

// A paragraph of the Track Editor's help at y: a heading (flags 1) on one line or wrapped text. Returns the y under
// it; a paragraph off the menu, or any on the measuring pass, only takes its room.
int32_t DisplayTEHelpEntry(ptr32<sAMenuHeader> pHeader, ptr32<sTrackEditorHelpData> pTrackEditorHelpData, int32_t x, int32_t y, int32_t, int32_t)
{
    GuestFrame frame(0x50);
    sAMenuHeader &h = *pHeader;
    const sTrackEditorHelpData &help = *pTrackEditorHelpData;
    const int32_t paraHeight = help.height;
    if (y < 0x28 - paraHeight || h.y + h.height < y)
        return y + paraHeight;
    const ptr32<font> pFont = G(fontVarG).pFont;
    int32_t fontHeight = pFont->chHeight >> 1;
    if ((pFont->flags & 2u) != 0)
        fontHeight -= 2;
    if (firstDrawPTT() != 0)
        return y + 3 + fontHeight * 2;
    const ptr32<sTextGroup> pTextGroup = help.pTrackEditorHelp;
    if (help.flags == 1)
    {
        FontSetColour(0, 0x7f, 0x7f);
        FontPrint(i16(x), i16(y), text(pTextGroup));
        return y + h.changeY;
    }
    FontSetColour(0x7f, 0x7f, 0x7f);
    return AStringWrap(x, y, text(pTextGroup), pHeader) * h.changeY + y;
}

} // namespace rc

RC_DECOMP_FN_SETUP_HEAP(AMenuDisplayAll, rc::fuzz::amenuDraw)
RC_DECOMP_FN_SETUP_HEAP(AMenuDisplayTitle, rc::fuzz::amenuDrawTitle)
RC_DECOMP_FN_SETUP_HEAP(AMenuDisplayMIT, rc::fuzz::amenuDrawItem)
RC_DECOMP_FN_SETUP_HEAP(AMenuDisplay, rc::fuzz::amenuDrawOrdinary)
RC_DECOMP_FN_SETUP_HEAP(AMenuDisplayNewScroll, rc::fuzz::amenuDrawScroll)
RC_DECOMP_FN_SETUP_HEAP(AMenuDisplayAreaMenu, rc::fuzz::amenuAreaHeader)
RC_DECOMP_FN_SETUP(AMenuAreaMenuSetup, rc::fuzz::amenuAreaHeader)
RC_DECOMP_FN_SETUP_HEAP(AMenuDisplayOptions, rc::fuzz::amenuDrawOrdinary)
RC_DECOMP_FN_SETUP(AmenuCalcSizeMIT, rc::fuzz::amenuSizeItem)
RC_DECOMP_FN_SETUP(AMenuCalcSize, rc::fuzz::amenuSize)
RC_DECOMP_FN_SETUP_HEAP(AMenuDisplayDialogText, rc::fuzz::amenuDrawDialog)
RC_DECOMP_FN_SETUP_HEAP(AMenuTabPrint, rc::fuzz::amenuDrawTabs)
RC_DECOMP_FN_SETUP_HEAP(DisplayProgTTMenuTitle, rc::fuzz::amenuPageSub)
RC_DECOMP_FN_SETUP_HEAP(DisplayTEHelpEntry, rc::fuzz::amenuHelpEntry)
RC_DECOMP_FN_SETUP(AMenuAreaMenuCheckValidSelection, rc::fuzz::amenuAreaHeader)

// Decompiled from AMENU.c: the menu system of the front end and the pause menu. This file holds the small parts: the
// menu lists (items, links, flags), the string and set getters, the box, border, bar and tick-box drawing, and the
// selection helpers. A menu (sAMenuHeader) is a linked list of items (sMenuItemStd), each with an optional variable
// (sMenuItemVar); text comes in groups of four languages (sTextGroup).
//
// Written from the instructions (Ghidra's C as a first reading, checked against them). The original inlines
// AMenuGetMIT's walk, unrolled eight times, wherever it needs the selected item: amenu::item is that walk.

#include "rc_widescreen.h" // enhanced recomp: menu scale
#include "amenu.h"
#include "amenu_fuzz.h"
#include "../rc_cars.h"

// Europe variants (the PAL screen and the text offsets; NOTES.md "Europe version"): GetNewMenuFiddleFactor (the
// other four offsets, plus 3), AMenuSnapsize, AMenuRectClipEnd, AMenuDrawScreenBox (a 256-line screen), AMenuDrawRect
// (one line lower), AMenuDrawBorder (top row 10 lines lower, one more side row in a box 30 or more high).
RC_VERSION_VARIANT(GetNewMenuFiddleFactor)
RC_VERSION_VARIANT(AMenuSnapsize)
RC_VERSION_VARIANT(AMenuDrawRect)
RC_VERSION_VARIANT(AMenuDrawBorder)
RC_VERSION_VARIANT(AMenuDrawScreenBox)
RC_VERSION_VARIANT(AMenuRectClipEnd)

namespace rc {

using namespace amenu;

void AMenuInitHeader(ptr32<sAMenuHeader> pHeader, int32_t flags)
{
    std::memset(pHeader.get(), 0, 0x80);
    pHeader->flags = static_cast<uint16>(flags);
}

void AmenuSetflags(ptr32<sAMenuHeader> pHeader, int32_t flags)
{
    pHeader->flags = static_cast<uint16>(pHeader->flags | flags);
}

void AmenuClearflags(ptr32<sAMenuHeader> pHeader, int32_t flags)
{
    pHeader->flags = static_cast<uint16>(pHeader->flags & ~flags);
}

ptr32<sMenuItemStd> AMenuGetMIT(ptr32<sAMenuHeader> pHeader, int32_t entry)
{
    return item(pHeader, entry);
}

// Appends an item. The count goes up by one, or becomes 1 for the first item whatever it was.
void AMenuAddMIT(ptr32<sAMenuHeader> pHeader, ptr32<sMenuItemStd> pMenuItem)
{
    ptr32<sMenuItemStd> p = pHeader->pMenuStart;
    if (!p)
    {
        pHeader->pMenuStart = pMenuItem;
        pHeader->menuItems = 1;
        return;
    }
    while (p->pNext)
        p = p->pNext;
    p->pNext = pMenuItem;
    pHeader->menuItems = static_cast<uint8>(pHeader->menuItems + 1);
}

ptr32<char> AMenuGetText(ptr32<sTextGroup> pTextGroup)
{
    return text(pTextGroup);
}

void MoveUIbox(ptr32<sAMenuHeader>) {}
void UIborderInit() {}
void UIborderTerm() {}
void UImenuItemInit() {}
void UImenuItemTerm() {}
void UImenuVarInit() {}
void UImenuVarTerm() {}
void UIgroupInit() {}
void UIgroupTerm() {}
void AMenuMenuInit() {}
void AMenuMenuTerm() {}
void AMenuTGrpInit() {}
void AMenuTGrpTerm() {}
void AMenuTLstInit() {}
void AMenuTLstTerm() {}

// Links an item (or, for position -1, the menu itself) to another menu. The walk to the item does not look for the
// end of the list.
ptr32<sMenuItemStd> AMenuLinkItems(ptr32<sAMenuHeader> pMenuBase, int32_t baseMenuPosition, ptr32<sAMenuHeader> pMenuLink, int32_t linkType,
                                   int32_t selectionTag)
{
    if (baseMenuPosition == -1)
    {
        pMenuBase->selectionTag = static_cast<uint32>(selectionTag);
        pMenuBase->pGlobalDestMenu = pMenuLink;
        return nullptr;
    }
    ptr32<sMenuItemStd> p = pMenuBase->pMenuStart;
    if (!p)
        return nullptr;
    for (int32_t i = 0; i != baseMenuPosition; ++i)
        p = p->pNext;
    p->pLinkMenu = pMenuLink;
    p->selectionTag = static_cast<uint32>(selectionTag);
    p->linkType = static_cast<uint8>(linkType);
    return p;
}

// A car's name: from the level's table, or the Track Editor's when no level is loaded. Types from 20 on share the
// names from 1 on.
ptr32<char> GetCarText(int32_t carType)
{
    const uint32_t level = G(pLvlG).address;
    const uint32_t names = level != 0xffffffffu ? at<uint32_t>(level + 0x58u) : at<uint32_t>(kTEditCarNames);
    if (carType == 0)
        return nullptr;
    if (carType >= 0x14)
        carType -= 0x13;
    return ptr32<char>(at<uint32_t>(names + static_cast<uint32_t>(carType) * 4u));
}

// Counts the items before the first selectable one (flag 0x40) into *pMinMenuSel, the visible ones of those into
// *pVisMenuSel and the selectable ones into *pSelMenuItemCount. Returns the first selectable item's index less the
// visible count, at least 0.
int32_t AMenuProcessGetMinSel(ptr32<sAMenuHeader> pHeader, ptr32<int32> pMinMenuSel, ptr32<int32> pVisMenuSel, ptr32<int32> pSelMenuItemCount)
{
    int32_t startMenuSel = 9999;
    ptr32<sMenuItemStd> p = pHeader->pMenuStart;
    if ((pHeader->evenMoreFlags & 4u) != 0)
        p->flags = static_cast<uint16>(p->flags & 0xff9fu);
    if (!p)
        return 0;
    for (int32_t i = 0; p; ++i, p = p->pNext)
    {
        const uint16_t flags = p->flags;
        if ((flags & 0x40u) != 0)
        {
            *pMinMenuSel = *pMinMenuSel + 1;
            if ((flags & 0x20u) == 0)
                *pVisMenuSel = *pVisMenuSel + 1;
        }
        else
        {
            if (i < startMenuSel)
                startMenuSel = i;
            *pSelMenuItemCount = *pSelMenuItemCount + 1;
        }
    }
    const int32_t retVal = startMenuSel - *pVisMenuSel;
    return retVal < 0 ? 0 : retVal;
}

void AMenuSetAreaMenuMaxStringLength(int32_t length)
{
    menuSystem().areaMenuMaxStringLength = static_cast<uint16>(length);
}

// Copies a string without its "%s" (and without any other '%', which also swallows the character after it).
void AmenuRemovePercentS(ptr32<char> pDestBuffer, ptr32<char> pSourceBuffer)
{
    int32_t j = 0;
    for (uint32_t i = 0; i < std::strlen(pSourceBuffer.get()); ++i)
    {
        const char c = pSourceBuffer[i];
        if (c == '%')
            ++i;
        else if (pSourceBuffer[i + 1] == 's')
            ++i;
        else
            pDestBuffer[j++] = c;
    }
    pDestBuffer[j] = 0;
}

int32_t AMenuStringCentre(ptr32<sAMenuHeader> pHeader, ptr32<char> pText, int32_t stringWidth)
{
    GuestFrame frame(0x10);
    return AStringCentre(pHeader->x, pHeader->width, pText, stringWidth);
}

// A message of the level's general text, or the Track Editor's when no level is loaded.
ptr32<char> GetGeneralString(int32_t messageNo)
{
    const uint32_t level = G(pLvlG).address;
    const uint32_t table = level != 0xffffffffu ? at<uint32_t>(level + 0x2cu) : at<uint32_t>(kTEditGeneralText);
    return text(ptr32<sTextGroup>(at<uint32_t>(table + static_cast<uint32_t>(messageNo) * 4u + 4u)));
}

void AMenuAddTextToTGRP(ptr32<sTextGroup> pTextGroup, ptr32<char> pText)
{
    pTextGroup->pText[3] = pText;
    pTextGroup->pText[0] = pText;
    pTextGroup->pText[2] = pText;
    pTextGroup->pText[1] = pText;
}

// The text offset for the working language in a scrolling menu.
int32_t GetNewMenuFiddleFactor(ptr32<sAMenuHeader> pHeader)
{
#if RC_VERSION_EU
    // Europe: the tofsp offsets, plus 3 (3 for no language).
    switch (language())
    {
    case 0: return pHeader->tofspEnglish + 3;
    case 1: return pHeader->tofspFrench + 3;
    case 2: return pHeader->tofspGerman + 3;
    case 3: return pHeader->tofspSpanish + 3;
    default: return 3;
    }
#else
    switch (language())
    {
    case 0: return pHeader->tofsnEnglish;
    case 1: return pHeader->tofsnFrench;
    case 2: return pHeader->tofsnGerman;
    case 3: return pHeader->tofsnSpanish;
    default: return 0;
    }
#endif
}

// Sets all eight text offsets to 2 ff, keeping each one's lowest bit.
void SetTextOffsetFiddleFactor(ptr32<sAMenuHeader> pHeader, int32_t ff)
{
    int8 *offsets = &pHeader->tofspEnglish;
    for (int i = 0; i < 8; ++i)
        offsets[i] = static_cast<int8>(ff * 2 + (offsets[i] & 1));
}

ptr32<sUIcolourSet> AMenuGetColourSet(ptr32<sAMenuHeader> pHeader)
{
    ptr32<sUIcolourSet> pColourSet = pHeader->pColourSet;
    if (!pColourSet)
    {
        pColourSet = menuSystem().pColourSet;
        if (!pColourSet)
            pColourSet = menuSystem().pLastColourSet;
    }
    return pColourSet;
}

ptr32<sBorderSet> AMenuGetBorderSet(ptr32<sAMenuHeader> pHeader)
{
    ptr32<sBorderSet> pBorderSet = pHeader->pBorderSet;
    if (!pBorderSet)
    {
        pBorderSet = menuSystem().pBorderSet;
        if (!pBorderSet)
            pBorderSet = menuSystem().pLastBorderSet;
    }
    return pBorderSet;
}

// Whether a pad is not in the mode its controller setup menu needs: checked when the menu's tag is the one given, or
// when no tag is given. A DualShock must answer id 7, another pad id 5.
uint8 DualShockAnalogModeTest(eInputPad ePad, uint32 tag, ptr32<sAMenuHeader> pHeader)
{
    GuestFrame frame(0x20);
    if (pHeader->selectionTag != tag && tag != 0)
        return 0;
    if (!InputDualShockPresent(ePad))
        return InputGetPadID(ePad) != 5;
    return InputGetPadID(ePad) != 7;
}

// Leaving the options without accepting: puts the music and sound volumes back to what they were on entry.
void SpecialCaseMusicVolFixupBackout(ptr32<sAMenuHeader> pHeader)
{
    GuestFrame frame(0x60);
    if (!pHeader)
        return;
    for (ptr32<sMenuItemStd> p = pHeader->pMenuStart; p; p = p->pNext)
    {
        if (p->selectionTag == 0x10000286u)
        {
            at<int32_t>(kMusicUserVol) = at<int32_t>(kMusicVolG);
            guest_call<void>(kRCMusicModifyVolume, 0x19, 3);
            p->pMenuVar->value = at<int32_t>(kMusicVolG);
        }
        else if (p->selectionTag == 0x10000288u)
        {
            at<int32_t>(kSfxUserVol) = at<int32_t>(kSfxVolG);
            guest_call<void>(kRevSfxModifyVolume, 3);
            p->pMenuVar->value = at<int32_t>(kSfxVolG);
        }
    }
}

int32_t CheckBottomOfScroll(ptr32<sAMenuHeader> pHeader, int32_t y)
{
    return y < pHeader->y + pHeader->height - 10;
}

ptr32<char> AMenuGetTitleString(ptr32<sAMenuHeader> pHeader)
{
    return text(pHeader->pTitleText);
}

ptr32<char> AMenuGetDialogString(ptr32<sAMenuHeader> pHeader)
{
    return text(pHeader->pDialogTxt);
}

// Clears the hit state of the menu keys of one pad (the second pad's keys are 16 on).
void InputPadReset(int32_t pad)
{
    GuestFrame frame(0x30);
    const uint32_t padSelect = pad == 1 ? 0x10u : 0u;
    for (uint32_t i = 0; at<uint8_t>(kMenuInputs + i) != 0; ++i)
        InputResetHit(static_cast<uint8>(at<uint8_t>(kMenuInputs + i) + padSelect));
}

int32_t AMenuSelectMenuItem(ptr32<sAMenuHeader> pHeader, ptr32<sMenuItemStd> pMenuItem)
{
    GuestFrame frame(0x30);
    return selectItem(pHeader, pMenuItem);
}

// The index of the selection-th visible item (the item after it, counting hidden ones), or in a one-item-at-a-time
// menu (moreFlags 0x2000) the index of the first visible item; -1 for none.
int32_t GetNthVisibleItem(ptr32<sAMenuHeader> pHeader, int32_t selection)
{
    ptr32<sMenuItemStd> p = pHeader->pMenuStart;
    if ((pHeader->moreFlags & 0x2000u) != 0)
    {
        for (int32_t i = 0; p; ++i, p = p->pNext)
            if ((p->flags & 0x20u) == 0)
                return i;
        return -1;
    }
    int32_t retVal = -1;
    while (p && selection >= 0)
    {
        if ((p->flags & 0x20u) == 0)
            --selection;
        p = p->pNext;
        ++retVal;
    }
    return retVal;
}

// In a one-item-at-a-time menu, hides every item but the selected one.
void UpdateSingleVisibleMenu()
{
    sMenuSystem &ms = menuSystem();
    const ptr32<sAMenuHeader> menu = ms.pActiveMenu;
    if ((menu->moreFlags & 0x2000u) == 0)
        return;
    int32_t i = 0;
    for (ptr32<sMenuItemStd> p = menu->pMenuStart; p; p = p->pNext, ++i)
        p->flags = menu->selection == i ? static_cast<uint16>(p->flags & 0xffdfu) : static_cast<uint16>(p->flags | 0x20u);
    const ptr32<sAMenuHeader> active = ms.pActiveMenu;
    if (!active->pTitleText && !active->pDialogTxt)
        active->width = 0;
}

// Whether the items' variable must be shown disabled. Also keeps the vibration items (tags 0x1000028b for pad 1 and
// 0x10000078 for pad 2) disabled while their pad is not a DualShock, one call late: the tag is remembered when the
// pad is missing and acted on at the next call for the same item.
int32_t AMenuTestDisableMVAR(ptr32<sAMenuHeader> pHeader, ptr32<sMenuItemStd> pMenuItem)
{
    GuestFrame frame(0x40);
    int32_t retVal = 0;
    const uint32_t menuTag = pHeader->selectionTag;
    if (menuTag == 0x10000282u || menuTag == 0x10000284u)
    {
        const int32_t pad = menuTag == 0x10000282u ? 0 : 5;
        if (DualShockAnalogModeTest(static_cast<eInputPad>(pad), menuTag, pHeader) != 0 && pMenuItem->pMenuVar)
        {
            const int32_t value = pMenuItem->pMenuVar->value;
            if (value == 1 || value == 3)
                retVal = 1;
        }
    }
    const auto delayed = [&](int32_t &delay, uint32_t tag, int32_t pad)
    {
        if (static_cast<uint32_t>(delay) != tag || pMenuItem->selectionTag != tag)
            return;
        if (InputDualShockPresent(static_cast<eInputPad>(pad)))
            pMenuItem->statusFlags = static_cast<uint8>(pMenuItem->statusFlags & 0xfeu);
        else
        {
            retVal = 1;
            pMenuItem->statusFlags = static_cast<uint8>(pMenuItem->statusFlags | 1u);
            if (pMenuItem == item(pHeader, pHeader->selection))
                pHeader->selection = 0;
        }
        delay = 0;
    };
    delayed(G(fixIDSPdelay1G), 0x1000028bu, 0);
    delayed(G(fixIDSPdelay2G), 0x10000078u, 5);
    const uint32_t tag = pMenuItem->selectionTag;
    if (tag == 0x10000078u)
    {
        if (!InputDualShockPresent(static_cast<eInputPad>(5)))
            G(fixIDSPdelay2G) = static_cast<int32_t>(pMenuItem->selectionTag);
    }
    else if (tag == 0x1000028bu)
    {
        if (!InputDualShockPresent(static_cast<eInputPad>(0)))
            G(fixIDSPdelay1G) = static_cast<int32_t>(pMenuItem->selectionTag);
    }
    return retVal;
}

// The font colour of an item: green or cyan for the marked ones, dimmed for the disabled, the selection colour for
// the selected item (dimmed again in a one-item menu whose selection is disabled), else the text colour.
void AMenuSetMITColour(ptr32<sAMenuHeader> pHeader, ptr32<sMenuItemStd> pMenuItem, ptr32<sUIcolourSet> pColourSet, int32_t menuItem)
{
    GuestFrame frame(0x50);
    const auto dimmed = [&] { FontSetColour(pColourSet->tRed, static_cast<uint8>(pColourSet->tGreen >> 1), static_cast<uint8>(pColourSet->tBlue >> 1)); };
    const uint8_t status = pMenuItem->statusFlags;
    if ((status & 0x10u) != 0)
        return FontSetColour(0, 0x7f, 0);
    if ((status & 0x20u) != 0)
        return FontSetColour(0, 0x7f, 0x7f);
    if (AMenuTestDisableMVAR(pHeader, pMenuItem) != 0)
        return dimmed();
    if ((pMenuItem->statusFlags & 9u) != 0)
        return dimmed();
    if (menuItem == pHeader->selection + menuSystem().scrollSelector || (pHeader->moreFlags & 0x2000u) != 0)
    {
        FontSetColour(pColourSet->sRed, pColourSet->sGreen, pColourSet->sBlue);
        if ((pHeader->moreFlags & 0x2000u) != 0 && menuSystem().selectDisabled == 1)
            dimmed();
        return;
    }
    FontSetColour(pColourSet->tRed, pColourSet->tGreen, pColourSet->tBlue);
}

// Changes the selected item's variable: down (1) or up (2) by a step (64 for the music volume, the variable's own
// step with flag 8, 4 for a slider), clamped; a two-valued variable just flips. The new value goes to the variable's
// feedback word.
void AMenuChangeMVAR(ptr32<sAMenuHeader> pHeader, int32_t changeType, int32_t)
{
    GuestFrame frame(0x50);
    const ptr32<sMenuItemStd> pItem = item(pHeader, pHeader->selection);
    ptr32<sMenuItemVar> pVar = pItem->pMenuVar; // From address 4 for no item, as the original.
    if (!pVar)
        return;
    int32_t change = static_cast<uint32_t>(pVar->tagMVar) == 0x1000001fu ? 0x40 : 1;
    const uint16_t varFlags = pVar->flags;
    if ((varFlags & 8u) != 0)
        change = static_cast<int32_t>(pVar->pTextGroup.address);
    if ((varFlags & 0x300u) != 0)
        change = 4;
    if ((pItem->flags & 0x300u) == 0)
        sfxMove();
    pVar = pItem->pMenuVar;
    const int32_t maxValue = pVar->maxValue, minValue = pVar->minValue;
    if (maxValue - minValue == 1)
    {
        if (pVar->value == maxValue)
            pVar->value = minValue;
        else if (pVar->value == minValue)
            pVar->value = maxValue;
        change = 0;
    }
    if (changeType == 1)
    {
        const int32_t low = pVar->minValue;
        const int32_t value = pVar->value - change;
        pVar->value = value;
        if (value < low)
            pVar->value = low;
        menuSystem().keyReturnCode = 0x11000001u;
    }
    else if (changeType == 2)
    {
        const int32_t high = pVar->maxValue;
        const int32_t value = pVar->value + change;
        pVar->value = value;
        if (high < value)
            pVar->value = high;
        menuSystem().keyReturnCode = 0x11000002u;
    }
    const ptr32<int32> feedback = pVar->pFeedbackVar;
    if (feedback)
        *feedback = pVar->value;
}

// On entering a menu: the option variables take the current settings, and the two volumes are remembered for
// SpecialCaseMusicVolFixupBackout.
void AMenuEntrySettingsMVAR(ptr32<sAMenuHeader> pHeader)
{
    if (!pHeader)
        return;
    for (ptr32<sMenuItemStd> p = pHeader->pMenuStart; p; p = p->pNext)
    {
        const auto set = [&](int32_t value) { p->pMenuVar->value = value; };
        switch (p->selectionTag)
        {
        case 0x10000285u: set(at<int32_t>(kFrontEnd + 0x18cu)); break;
        case 0x10000286u:
            set(at<int32_t>(kMusicUserVol));
            at<int32_t>(kMusicVolG) = at<int32_t>(kMusicUserVol);
            break;
        case 0x10000287u: set(at<int32_t>(kFrontEnd + 0x188u)); break;
        case 0x10000288u:
            set(at<int32_t>(kSfxUserVol));
            at<int32_t>(kSfxVolG) = at<int32_t>(kSfxUserVol);
            break;
        case 0x10000289u: set(at<int32_t>((sym::sfxVarG + 0x8u))); break; // sfxVarG + 8
        case 0x1000028du:
        case 0x1000028eu: cars::entrySettings(p.address); break; // enhancement: up to 12 cars, the settings' counts (rc_cars.h)
        default: break;
        }
    }
}

// Rounds a menu's size up to whole border tiles (30 by 15) and places it as its flags ask: centred (0x800 across,
// 0x1000 down), or at one of the fixed columns of moreFlags.
void AMenuSnapsize(ptr32<sAMenuHeader> pHeader)
{
    sAMenuHeader &h = *pHeader;
    h.width = static_cast<uint16>((h.width + 0x1d) / 0x1e * 0x1e);
    h.height = static_cast<uint16>((h.height + 9) / 0xf * 0xf);
    const uint16_t flags = h.flags;
    if ((flags & 0x800u) != 0)
        h.x = i16((0x280 - h.width) / 2);
    if ((flags & 0x1000u) != 0)
        h.y = i16((kScreenLines - h.height) / 2);
    const uint16_t more = h.moreFlags;
    if ((more & 8u) != 0)
        h.x = i16(0x140 - h.width);
    if ((more & 0x10u) != 0)
        h.x = 0x140;
    if ((more & 2u) != 0)
        h.x = i16(0x260 - h.width);
    if ((more & 4u) != 0)
        h.x = 0x20;
    if ((more & 0x1000u) != 0)
        h.y = 0x20;
}

// The menu's background: a translucent rectangle a little larger than the menu, with four thin strips around it.
void AMenuDrawRect(ptr32<sAMenuHeader> pHeader, int32_t arg1)
{
    Locals f(0x80);
    GfxP2_ZWriteEnable();
    const ee_float otz = constant(sym::lit4_32758c);
    const ptr32<sUIcolourSet> pColourSet = AMenuGetColourSet(pHeader);
    const ptr32<sBorderSet> pBorderSet = AMenuGetBorderSet(pHeader);
    const uint32_t yCurve = pBorderSet->yCurve, xCurve = pBorderSet->xCurve;
    const sAMenuHeader &h = *pHeader;
    const uint32_t top = static_cast<uint32_t>(h.y) - yCurve + (RC_VERSION_EU ? 11u : 10u); // Europe: one line lower
    const uint32_t left = static_cast<uint32_t>(h.x) - xCurve;
    const uint32_t bottom = at<uint16_t>(sym::ffRectBottomG) + top + (h.height + yCurve * 2u) - 0xfu;
    const uint32_t right = at<uint16_t>(sym::ffRectRightG) + left + (h.width + xCurve * 2u);
    const uint32_t x0 = left - at<uint16_t>(sym::ffRectLeftG), y0 = top - at<uint16_t>(sym::ffRectTopG);
    f.sh(0xc, right);
    f.sh(0xa, y0);
    f.sh(0x10, x0);
    f.sh(0x12, bottom);
    f.sw(0x0, 0x11);
    f.sh(0x4, x0);
    f.sh(0x6, y0);
    f.sh(0x8, right);
    f.sh(0xe, bottom);
    f.sb(0x17, pColourSet->transLevel);
    f.sb(0x14, pColourSet->bRed);
    f.sb(0x15, pColourSet->bGreen);
    f.sb(0x16, pColourSet->bBlue);
    RectRender(f.ptr<_sRect>(0), otz);
    const ptr32<_sRect> small = f.ptr<_sRect>(0x20);
    const auto strip = [&](uint32_t xa, uint32_t xb, uint32_t ya, uint32_t yb)
    {
        f.sh(0x24, xa);
        f.sh(0x30, xa);
        f.sh(0x28, xb);
        f.sh(0x2c, xb);
        f.sh(0x26, ya);
        f.sh(0x2a, ya);
        f.sh(0x2e, yb);
        f.sh(0x32, yb);
        RectRender(small, otz);
    };
    std::memcpy(guest_host(f.sp + 0x20u), guest_host(f.sp), 0x18);
    strip(f.lhu(0x4) + 6u, f.lhu(0x8) - 6u, f.lhu(0x6) - 2u, f.lhu(0x6));
    {
        // Below: only the y coordinates change.
        const uint32_t y = f.lhu(0xe);
        f.sh(0x26, y);
        f.sh(0x32, y + 2u);
        f.sh(0x2a, y);
        f.sh(0x2e, y + 2u);
        RectRender(small, otz);
    }
    strip(f.lhu(0x4) - 4u, f.lhu(0x4), f.lhu(0x6) + 3u, f.lhu(0x12) - 3u);
    strip(f.lhu(0x8), f.lhu(0x8) + 4u, f.lhu(0x6) + 3u, f.lhu(0x12) - 3u);
}

// The menu's border: corner, edge and side tiles (30 by 15) around the menu.
void AMenuDrawBorder(ptr32<sAMenuHeader> pHeader, int32_t arg1)
{
    GuestFrame frame(0xc0);
    GfxP2_ZWriteEnable();
    const ptr32<sBorderSet> pBorderSet = AMenuGetBorderSet(pHeader);
    const sAMenuHeader &h = *pHeader;
    const auto part = [&](int index, int32_t x, int32_t y) { HudMenuBorder(pBorderSet->pBorderPart[index], x, y, static_cast<int32>(shine())); };
    const auto inside = [&](int16_t x) { return !(h.x + h.width - 0x1e < x); };
    // Europe: the top row 5 lines above the menu instead of 15.
    int16_t x = i16(static_cast<uint16_t>(h.x) - 0x1e), y = i16(static_cast<uint16_t>(h.y) - (RC_VERSION_EU ? 5 : 0xf));
    const int16_t leftEdge = x;
    part(0, leftEdge, y);
    for (x = i16(x + 0x1e); inside(x); x = i16(x + 0x1e))
        part(1, x, y);
    const int16_t rightEdge = x;
    part(2, rightEdge, y);
    y = i16(y + 0xf);
#if RC_VERSION_EU
    // Europe: one more side row (15 lines) in a box 30 or more high whose bottom is not above the row.
    const int32_t bottom = h.y + h.height;
    int32_t lowest = bottom - 0x19;
    if (!(h.height < 0x1e) && !(bottom - 0xa < y))
        lowest += 0xf;
#else
    const int32_t lowest = h.y + h.height - 0x19;
#endif
    for (; !(lowest < y); y = i16(y + 0xf))
    {
        part(4, i16(leftEdge - at<uint16_t>(sym::ffBordLeftG)), y);
        part(7, rightEdge, y);
    }
    part(5, leftEdge, y);
    for (x = i16(leftEdge + 0x1e); inside(x); x = i16(x + 0x1e))
        part(6, x, y + G(ffBordBottomG));
    part(3, x, y);
}

void AMenuDrawBox(ptr32<sAMenuHeader> pHeader, int32_t flags)
{
    {
        GuestFrame frame(0x30);
        AMenuDrawBorder(pHeader, flags);
    }
    AMenuDrawRect(pHeader, flags);
}

// A box around the whole screen.
void AMenuDrawScreenBox(ptr32<sAMenuHeader> pHeader, int32_t arg1)
{
    Locals f(0xd0);
    GfxP2_ZWriteEnable();
    const ptr32<sUIcolourSet> pColourSet = AMenuGetColourSet(pHeader);
    f.sh(0xc, 0x280);
    f.sh(0x12, kScreenLines);
    f.sw(0x0, 0x11);
    f.sh(0x4, 0);
    f.sh(0x6, 0);
    f.sh(0x8, 0x280);
    f.sh(0xa, 0);
    f.sh(0xe, kScreenLines);
    f.sh(0x10, 0);
    f.sb(0x17, pColourSet->transLevel);
    f.sb(0x14, pColourSet->bRed);
    f.sb(0x15, pColourSet->bGreen);
    f.sb(0x16, pColourSet->bBlue);
    RectRender(f.ptr<_sRect>(0), constant(sym::lit4_327590));
    const ptr32<sBorderSet> pBorderSet = AMenuGetBorderSet(pHeader);
    const auto part = [&](int index, int32_t x, int32_t y) { HudMenuBorder(pBorderSet->pBorderPart[index], x, y, static_cast<int32>(shine())); };
    // The bottom row and the last side row: 0xcc and 0xbd, in Europe 0xef and 0xe0 (a 256-line screen).
    constexpr int32_t bottomRow = RC_VERSION_EU ? 0xef : 0xcc, lastSide = RC_VERSION_EU ? 0xe0 : 0xbd;
    for (int32_t x = 0x1e; x < 0x245; x += 0x1e)
    {
        part(1, x, 3);
        part(6, x, bottomRow);
    }
    for (int32_t y = 0x12; !(lastSide < y); y += 0xf)
    {
        part(4, 0, y);
        part(7, 0x262, y);
    }
    part(0, 0, 3);
    part(1, 0x244, 3);
    part(2, 0x262, 3);
    part(4, 0, lastSide);
    part(5, 0, bottomRow);
    part(6, 0x244, bottomRow);
    part(7, 0x262, lastSide);
    part(3, 0x262, bottomRow);
}

// A slider: the knob sprite at the value's place, the bar in two shaded halves and a black frame behind.
void DisplayUIbar(ptr32<sBarDetails> pBar)
{
    Locals f(0x100);
    const sBarDetails &bar = *pBar;
    int32_t pos = bar.x + ee_div(bar.value * (static_cast<int32_t>(bar.width) - 8), bar.maxValue);
    const ee_float otz = constant(sym::lit4_327594);
    const uint32_t height = bar.height;
    int32_t y = bar.y;
    const ptr32<_sSprite> pSprite = menuSystem().pSliderSprite;
    std::memset(guest_host(f.sp), 0, 0x18);
    const int32_t split = pos + 4;
    f.sh(0x6, y);
    f.sh(0x4, pos);
    f.sb(0x12, 0x7f);
    f.sh(0x16, 1);
    f.sw(0x0, 0x20);
    f.sh(0xe, 0xf);
    f.sb(0x10, 0x7f);
    f.sb(0x11, 0x7f);
    f.sh(0x14, 1);
    f.sh(0xc, 0xf);
    SpriteRender(pSprite, f.ptr<_sSpriteParams>(0), constant(sym::lit4_327598));
    y += 3;
    int32_t bottom = y + static_cast<int32_t>((height >> 1) + 2u);
    const ptr32<_sGRect> shaded = f.ptr<_sGRect>(0x40);
    f.sh(0x46, y);
    f.sh(0x4a, y);
    f.sh(0x4e, bottom);
    f.sh(0x52, bottom);
    f.sh(0x48, split);
    f.sh(0x4c, split);
    for (const uint32_t offset : {0x56u, 0x62u, 0x59u, 0x5au, 0x5du, 0x5eu})
        f.sb(offset, 0xff);
    f.sh(0x50, static_cast<uint16_t>(bar.x));
    f.sw(0x40, 0);
    f.sh(0x44, static_cast<uint16_t>(bar.x));
    for (const uint32_t offset : {0x54u, 0x55u, 0x60u, 0x61u, 0x58u, 0x5cu})
        f.sb(offset, 0);
    RectRenderGouraud(shaded, otz);
    {
        const uint32_t right = static_cast<uint16_t>(bar.x) + bar.width;
        f.sh(0x50, split);
        f.sh(0x44, split);
        for (const uint32_t offset : {0x5eu, 0x55u, 0x56u, 0x61u, 0x62u, 0x5au})
            f.sb(offset, 0xff);
        f.sh(0x4c, right);
        f.sh(0x48, right);
        for (const uint32_t offset : {0x54u, 0x60u, 0x58u, 0x59u, 0x5cu, 0x5du})
            f.sb(offset, 0);
        RectRenderGouraud(shaded, otz);
    }
    y -= 1;
    bottom += 1;
    const uint32_t left = static_cast<uint16_t>(bar.x) - 1u, right = static_cast<uint16_t>(bar.x) + bar.width + 1u;
    f.sh(0x2a, y);
    f.sh(0x32, bottom);
    f.sh(0x26, y);
    f.sh(0x2e, bottom);
    f.sh(0x30, left);
    f.sh(0x2c, right);
    f.sh(0x24, left);
    f.sh(0x28, right);
    f.sb(0x34, 0);
    f.sb(0x35, 0);
    f.sb(0x36, 0);
    RectRender(f.ptr<_sRect>(0x20), otz);
}

void UserInterfaceInit()
{
    GuestFrame frame(0x20);
    std::memset(&G(menuSystemG), 0, 0xf8);
    G(pMenuSystemG) = ptr32<sMenuSystem>(sym::menuSystemG);
    G(menuSystemG).status = 1;
    at<uint32_t>(kMainVars + 0x16cu) = 0xffffffffu;
    at<uint32_t>(kMainVars + 0x170u) = 0;
    UIborderInit();
    UImenuItemInit();
    UImenuVarInit();
    UIgroupInit();
    AMenuTGrpInit();
    AMenuTLstInit();
    AMenuMenuInit();
    FontSetGraphicFn(ptr32<void>(sym::AMenuDisplayPsxKey)); // AMenuDisplayPsxKey
}

void UserInterfaceTerm()
{
    G(pMenuSystemG) = nullptr;
    UIborderTerm();
    UImenuItemTerm();
    UImenuVarTerm();
    UIgroupTerm();
    AMenuTGrpTerm();
    AMenuTLstTerm();
    AMenuMenuTerm();
}

// Limits drawing to the menu's box (less the scrolled-off lines), unless this is the first, measuring pass.
void AMenuRectClipStart(ptr32<sAMenuHeader> pHeader, int32_t yOffset, int32_t clipExtraHeight)
{
    if (firstDrawPTT() != 0)
        return;
    const sAMenuHeader &h = *pHeader;
    const int32_t scrollLines = G(scrollLinesG);
    int32_t x1 = h.x, y1 = h.y + yOffset + scrollLines - h.spare, x2 = h.x + h.width;
    int32_t y2 = h.y + h.height - yOffset - (scrollLines + 8) + clipExtraHeight;
    widescreen::menuScissor(x1, y1, x2, y2); // enhanced recomp: the clip follows the menu's scale
    GfxP2_Scissor(x1, y1, x2, y2);
}

void AMenuRectClipEnd(ptr32<sAMenuHeader> arg0, int32_t arg1)
{
    if (firstDrawPTT() != 0)
        return;
    GfxP2_Scissor(0, 0, 0x280, kScreenLines);
}

// A tick box: empty (0), ticked (1) or crossed (2).
void DisplayTickBox(int32_t x, int32_t y, int32_t value)
{
    Locals f(0x30);
    const ptr32<_sSprite> pSprite = menuSystem().pTickGfx;
    if (value == 0)
        keySprite(f, pSprite, x, y, 0x18, constant(sym::lit4_32759c));
    else if (value == 1)
        keySprite(f, pSprite, x, y, 0x1a, constant(sym::lit4_3275a0));
    else if (value == 2)
        keySprite(f, pSprite, x, y, 0x10, constant(sym::lit4_3275a4));
}

// A tick box with its text: box then text from x (1), text then box against the menu's right edge (2), or the same
// two about the menu's middle (3, 4). A value of -1 draws no box and leaves no room for one.
void DisplayTickBoxText(ptr32<sAMenuHeader> pHeader, int32_t x, int32_t y, int32_t value, ptr32<char> pText, int32_t type)
{
    const int32_t tickBoxSpace = value == -1 ? 0 : 0x20;
    switch (type)
    {
    case 1:
    {
        {
            GuestFrame frame(0x70);
            DisplayTickBox(x, y, value);
        }
        FontPrint(i16(x + tickBoxSpace), i16(y), pText);
        break;
    }
    case 2:
    case 4:
    {
        int32_t rightTickPos;
        {
            GuestFrame frame(0x70);
            const uint32_t width = type == 2 ? pHeader->width : pHeader->width >> 1;
            rightTickPos = pHeader->x + static_cast<int32_t>(width) - tickBoxSpace;
            FontPrintRight(i16(rightTickPos - 4), i16(y), pText);
        }
        DisplayTickBox(rightTickPos, y, value);
        break;
    }
    case 3:
    {
        {
            GuestFrame frame(0x70);
            DisplayTickBox(static_cast<int32_t>(pHeader->width >> 1) + x, y, value);
        }
        FontPrint(i16(static_cast<int32_t>(pHeader->width >> 1) + x + tickBoxSpace), i16(y), pText);
        break;
    }
    default: break;
    }
}

// A pad key (0-7) or a tick-sheet picture (8 on) inside a line of text: the font calls this for its "}" codes.
void AMenuDisplayPsxKey(int32_t x, int32_t y, int32_t key)
{
    Locals f(0x30);
    const ptr32<_sSprite> pSprite = key < 8 ? menuSystem().pPsxKeys : menuSystem().pTickGfx;
    keySprite(f, pSprite, x, y, static_cast<uint32_t>(key) * 2u, constant(sym::lit4_3275c4));
}

// A horizontal rule across the menu at y, shading from blue at the top to cyan. Returns the y below it.
int32_t DisplayLine(ptr32<sAMenuHeader> pHeader, int32_t y)
{
    Locals f(0x50);
    rule(f, 0, *pHeader, y, constant(sym::lit4_3275c8));
    return y + 9;
}

// A black rectangle gap pixels larger all round than a sprite's.
void DisplayTrackGraphicBorder(int32_t gap, ptr32<_sSpriteParams> pSpriteParams)
{
    Locals f(0x30);
    const _sSpriteParams &p = *pSpriteParams;
    const uint32_t y = static_cast<uint16_t>(p.y), x = static_cast<uint16_t>(p.x), g = static_cast<uint32_t>(gap);
    const uint32_t right = p.width + (x + g), bottom = p.height + (y + g), top = y - g, left = x - g;
    f.sh(0xa, top);
    f.sh(0x12, bottom);
    f.sh(0xc, right);
    f.sh(0x10, left);
    f.sw(0x0, 0);
    f.sh(0x6, top);
    f.sh(0xe, bottom);
    f.sh(0x4, left);
    f.sh(0x8, right);
    f.sb(0x14, 0);
    f.sb(0x15, 0);
    f.sb(0x16, 0);
    RectRender(f.ptr<_sRect>(0), constant(sym::lit4_3275cc));
}

// Whether a level's race of a type has been won: 0 no, 1 yes in a cup already finished, 2 yes in the current cup.
int32_t CheckRaceTick(ptr32<_sFrontGameStateData> pGameStateData, int32_t level, int32_t raceType)
{
    Locals f(0x80);
    const uint32_t state = pGameStateData.address;
    guest_call<void>(kSetRewards, f.ptr<void>(0), pGameStateData, at<int32_t>(state + 0x124u));
    level = static_cast<int32_t>(static_cast<uint32_t>(level) - 1u);
    if ((f.at<uint8_t>(7u + static_cast<uint32_t>(level % 0x18)) & raceType) == 0)
        return 0;
    return level / 6 < at<int32_t>(state + 0x124u) ? 1 : 2;
}

} // namespace rc

RC_DECOMP_FN_SETUP(AMenuInitHeader, rc::fuzz::amenuHeader)
RC_DECOMP_FN_SETUP(AmenuSetflags, rc::fuzz::amenuHeader)
RC_DECOMP_FN_SETUP(AmenuClearflags, rc::fuzz::amenuHeader)
RC_DECOMP_FN_SETUP(AMenuGetMIT, rc::fuzz::amenuItemIndex)
RC_DECOMP_FN_SETUP(AMenuAddMIT, rc::fuzz::amenuAddItem)
RC_DECOMP_FN_SETUP(AMenuGetText, rc::fuzz::amenuTextGroup)
RC_DECOMP_FN(MoveUIbox)
RC_DECOMP_FN(UIborderInit)
RC_DECOMP_FN(UIborderTerm)
RC_DECOMP_FN(UImenuItemInit)
RC_DECOMP_FN(UImenuItemTerm)
RC_DECOMP_FN(UImenuVarInit)
RC_DECOMP_FN(UImenuVarTerm)
RC_DECOMP_FN(UIgroupInit)
RC_DECOMP_FN(UIgroupTerm)
RC_DECOMP_FN(AMenuMenuInit)
RC_DECOMP_FN(AMenuMenuTerm)
RC_DECOMP_FN_SETUP(AMenuLinkItems, rc::fuzz::amenuLink)
RC_DECOMP_FN(AMenuTGrpInit)
RC_DECOMP_FN(AMenuTGrpTerm)
RC_DECOMP_FN(AMenuTLstInit)
RC_DECOMP_FN(AMenuTLstTerm)
RC_DECOMP_FN_SETUP(GetCarText, rc::fuzz::amenuMessage)
RC_DECOMP_FN_SETUP(AMenuProcessGetMinSel, rc::fuzz::amenuMinSel)
RC_DECOMP_FN_SETUP(AMenuSetAreaMenuMaxStringLength, rc::fuzz::amenuMessage)
RC_DECOMP_FN_SETUP(AmenuRemovePercentS, rc::fuzz::amenuStrings)
RC_DECOMP_FN_SETUP(AMenuStringCentre, rc::fuzz::amenuCentre)
RC_DECOMP_FN_SETUP(GetGeneralString, rc::fuzz::amenuMessage)
RC_DECOMP_FN_SETUP(AMenuAddTextToTGRP, rc::fuzz::amenuTextGroup)
RC_DECOMP_FN_SETUP(GetNewMenuFiddleFactor, rc::fuzz::amenuHeader)
RC_DECOMP_FN_SETUP(SetTextOffsetFiddleFactor, rc::fuzz::amenuHeader)
RC_DECOMP_FN_SETUP(AMenuGetColourSet, rc::fuzz::amenuHeader)
RC_DECOMP_FN_SETUP(AMenuGetBorderSet, rc::fuzz::amenuHeader)
RC_DECOMP_FN_SETUP(DualShockAnalogModeTest, rc::fuzz::amenuPadTest)
RC_DECOMP_FN_SETUP(SpecialCaseMusicVolFixupBackout, rc::fuzz::amenuHeader)
RC_DECOMP_FN_SETUP(CheckBottomOfScroll, rc::fuzz::amenuHeader)
RC_DECOMP_FN_SETUP(AMenuGetTitleString, rc::fuzz::amenuHeader)
RC_DECOMP_FN_SETUP(AMenuGetDialogString, rc::fuzz::amenuHeader)
RC_DECOMP_FN_SETUP(InputPadReset, rc::fuzz::amenuInput)
RC_DECOMP_FN_SETUP(AMenuSelectMenuItem, rc::fuzz::amenuItem)
RC_DECOMP_FN_SETUP(GetNthVisibleItem, rc::fuzz::amenuItemIndex)
RC_DECOMP_FN_SETUP(UpdateSingleVisibleMenu, rc::fuzz::amenuNone)
RC_DECOMP_FN_SETUP(AMenuTestDisableMVAR, rc::fuzz::amenuItem)
RC_DECOMP_FN_SETUP(AMenuSetMITColour, rc::fuzz::amenuItem)
RC_DECOMP_FN_SETUP(AMenuChangeMVAR, rc::fuzz::amenuChange)
RC_DECOMP_FN_SETUP(AMenuEntrySettingsMVAR, rc::fuzz::amenuHeader)
RC_DECOMP_FN_SETUP(AMenuSnapsize, rc::fuzz::amenuHeader)
RC_DECOMP_FN_SETUP_HEAP(AMenuDrawRect, rc::fuzz::amenuDrawOrdinary)
RC_DECOMP_FN_SETUP_HEAP(AMenuDrawBorder, rc::fuzz::amenuDrawOrdinary)
RC_DECOMP_FN_SETUP_HEAP(AMenuDrawBox, rc::fuzz::amenuDrawOrdinary)
RC_DECOMP_FN_SETUP_HEAP(AMenuDrawScreenBox, rc::fuzz::amenuDrawOrdinary)
RC_DECOMP_FN_SETUP_HEAP(DisplayUIbar, rc::fuzz::amenuBar)
RC_DECOMP_FN_SETUP(UserInterfaceInit, rc::fuzz::amenuInit)
RC_DECOMP_FN_SETUP(UserInterfaceTerm, rc::fuzz::amenuInit)
RC_DECOMP_FN_SETUP_HEAP(AMenuRectClipStart, rc::fuzz::amenuClip)
RC_DECOMP_FN_SETUP_HEAP(AMenuRectClipEnd, rc::fuzz::amenuClip)
RC_DECOMP_FN_SETUP_HEAP(DisplayTickBox, rc::fuzz::amenuTick)
RC_DECOMP_FN_SETUP_HEAP(DisplayTickBoxText, rc::fuzz::amenuTickText)
RC_DECOMP_FN_SETUP_HEAP(AMenuDisplayPsxKey, rc::fuzz::amenuTick)
RC_DECOMP_FN_SETUP_HEAP(DisplayLine, rc::fuzz::amenuLine)
RC_DECOMP_FN_SETUP_HEAP(DisplayTrackGraphicBorder, rc::fuzz::amenuSpriteBorder)
RC_DECOMP_FN_SETUP(CheckRaceTick, rc::fuzz::amenuRaceTick)

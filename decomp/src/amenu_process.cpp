// Decompiled from AMENU.c: a menu's input. AMenuProcessAll draws the active menu and runs the handler for its kind:
// AMenuProcess (an ordinary menu: up and down move the selection past hidden and disabled items, left and right
// change the item's variable, select follows the item's link or stores its tag as the execute code, back returns to
// the parent menu), AMenuProcessNewScroll (a scrolling list), AMenuProcessAreaMenu (the character grid of a name
// entry) or AMenuProcessScreenAdjust (moving the picture).
//
// Keys are read through AMenuNewProcessInput, which maps both pads' buttons and left sticks to codes: 1 up, 2 down,
// 3 left, 4 right, 5 back, 6 select, 9 and 10 the other two face buttons, 11 start.

#include "amenu.h"
#include "amenu_fuzz.h"

// Europe variants (NOTES.md "Europe version"): AMenuProcessScreenAdjust (the 2D offset's y 8 lines more: a 256-line
// screen); AMenuProcessAreaMenu and AMenuProcessAll differ only in instruction order (the same body).
RC_VERSION_VARIANT(AMenuProcessScreenAdjust)
RC_VERSION_VARIANT(AMenuProcessAreaMenu)
RC_VERSION_VARIANT(AMenuProcessAll)

namespace rc {

using namespace amenu;

namespace {

constexpr uint32_t kInputVars = sym::inputVarG;

void resetHits(uint32_t padSelect)
{
    for (uint32_t i = 0; at<uint8_t>(kMenuInputs + i) != 0; ++i)
        InputResetHit(static_cast<uint8>(at<uint8_t>(kMenuInputs + i) + padSelect));
}

// AMenuAreaMenuCheckValidSelection, inline in AMenuProcessAreaMenu.
int32_t validSelection(ptr32<sAMenuHeader> pHeader)
{
    const ptr32<sAreaMenuInfo> pAreaMenu = AMenuAreaMenuSetup(pHeader);
    const int32_t row = pHeader->rowSelect, rowCount = pAreaMenu->rowCount;
    if (rowCount < row)
        return 1;
    if (row != rowCount)
        return 0;
    return pHeader->colSelect < static_cast<int32_t>(pAreaMenu->endCol) ? 0 : 2;
}

// Makes a menu's global destination the active menu, selecting its first selectable item. The counts of hidden
// items are started again; the count of selectable ones carries on.
int32_t firstSelectable(const Locals &f, ptr32<sAMenuHeader> pHeader)
{
    return AMenuProcessGetMinSel(pHeader, f.ptr<int32>(0), f.ptr<int32>(4), f.ptr<int32>(8));
}

} // namespace

// The code of the first menu key hit (style 2) or held (style 3) on a pad, or failing that of its left stick pushed
// past 80: when hit, only as the direction changes.
int32_t AMenuNewProcessInputSub(int32_t pad, eMenuInputStyle style)
{
    GuestFrame frame(0x90);
    const uint32_t padSelect = pad == 1 ? 0x10u : 0u;
    const int32_t inputStyle = static_cast<int32_t>(style);
    for (uint32_t i = 0; at<uint8_t>(kStickInputs + i * 2u + 1u) != 0; ++i)
    {
        const uint8 key = static_cast<uint8>(at<uint8_t>(kStickInputs + i * 2u) + padSelect);
        bool_ down = 0;
        if (inputStyle == 2)
            down = InputHit(key);
        else if (inputStyle == 3)
            down = InputHeld(key);
        if (down)
            return at<uint8_t>(kStickInputs + i * 2u + 1u);
    }
    if (inputStyle == 3)
    {
        const uint32_t ms = G(pMenuSystemG).address + static_cast<uint32_t>(pad);
        at<uint8_t>(ms + 0xf2u) = at<uint8_t>(ms + 0xf4u);
    }
    if (pad == 0)
        at<uint32_t>(G(pMenuSystemG).address + 0xa4u) = at<uint32_t>(kInputVars + 0xbcu);
    else if (pad == 1)
        at<uint32_t>(G(pMenuSystemG).address + 0xb8u) = at<uint32_t>(kInputVars + 0xc0u);
    const uint32_t ms = G(pMenuSystemG).address;
    const uint32_t stick = ms + static_cast<uint32_t>(pad * 0x14);
    int32_t thisStickInput = 0;
    const int32_t x = at<int8_t>(stick + 0xa4u), y = at<int8_t>(stick + 0xa5u);
    if (!((x < 0 ? -x : x) < 0x51))
        thisStickInput = x < 1 ? 3 : 4;
    if (!((y < 0 ? -y : y) < 0x51))
        thisStickInput = y < 1 ? 1 : 2;
    if (inputStyle == 2)
    {
        at<uint8_t>(ms + static_cast<uint32_t>(pad) + 0xf4u) = static_cast<uint8_t>(thisStickInput);
        const uint32_t now = G(pMenuSystemG).address + static_cast<uint32_t>(pad);
        const uint8_t current = at<uint8_t>(now + 0xf4u);
        return current == at<uint8_t>(now + 0xf2u) ? 0 : current;
    }
    return inputStyle == 3 ? thisStickInput : 0;
}

// A key code from pad 0, pad 1, or (-1) either pad the menu allows (evenMoreFlags 0x80 the first, 8 the second;
// neither means both). The other pad's hits are thrown away. Remembers which pad it was.
int32_t AMenuNewProcessInput(int32_t pad, eMenuInputStyle style, ptr32<sAMenuHeader> pHeader)
{
    GuestFrame frame(0x50);
    int32_t padUsed = -1, result = 0;
    if (pad == 0)
    {
        result = AMenuNewProcessInputSub(0, style);
        if (result != 0)
            padUsed = 0;
        resetHits(0x10);
    }
    else if (pad == 1)
    {
        result = AMenuNewProcessInputSub(1, style);
        if (result != 0)
            padUsed = 1;
        resetHits(0);
    }
    else if (pad == -1)
    {
        uint32_t pads = 0x88;
        if (pHeader)
        {
            pads = pHeader->evenMoreFlags & 0x88u;
            if (pads == 0)
                pads = 0x88;
        }
        if ((pads & 0x80u) != 0)
        {
            result = AMenuNewProcessInputSub(0, style);
            if (result != 0)
                padUsed = 0;
        }
        if ((pads & 8u) != 0 && result == 0)
        {
            result = AMenuNewProcessInputSub(1, style);
            if (result != 0)
                padUsed = 1;
        }
        if (pads == 8)
            resetHits(0);
        else if (pads == 0x80)
            resetHits(0x10);
    }
    if (result != 0)
    {
        if (static_cast<int32_t>(style) == 2)
            G(lastHitPadG) = static_cast<uint8>(padUsed);
        else if (static_cast<int32_t>(style) == 3)
            G(lastHeldPadG) = static_cast<uint8>(padUsed);
    }
    return result;
}

void AMenuProcess(ptr32<sAMenuHeader> pHeader, int32_t special)
{
    Locals f(0xb0);
    sAMenuHeader &h = *pHeader;
    int32_t &minMenuSel = f.at<int32_t>(0), &visMenuSel = f.at<int32_t>(4), &selMenuItemCount = f.at<int32_t>(8);
    const auto ms = []() -> sMenuSystem & { return menuSystem(); };
    int32_t sfxFlag = 0;
    minMenuSel = 0;
    visMenuSel = 0;
    selMenuItemCount = 0;
    ms().newMenuFlag = 0;
    // The selection can live outside the menu: 1 and 2 stand for the two players' stored selections.
    if (h.pSelection)
    {
        if (h.pSelection.address == 1 || h.pSelection.address == 2)
        {
            const uint32_t store = kMainVars + 0x3e0u + h.pSelection.address;
            h.pSelection = ptr32<uint8>(store);
            h.pValueStore = ptr32<uint8>(store - 2u);
        }
        h.selection = static_cast<int8>(*h.pSelection);
    }
    if ((h.moreFlags & 0x8000u) != 0 && at<int32_t>(at<uint32_t>(kCameraPtr) + 0xd8u) != -1)
        InputReset();
    if (ms().specialStatsHoldoff != 0)
        ms().specialStatsHoldoff = static_cast<uint8>(ms().specialStatsHoldoff - 1);
    AMenuProcessGetMinSel(pHeader, f.ptr<int32>(0), f.ptr<int32>(4), f.ptr<int32>(8));
    const int32_t lastSelection = h.selection + ms().scrollSelector;
    // A scroll in progress runs down two pixels a frame.
    if (ms().smoothScroll < 0)
    {
        ms().smoothScroll = static_cast<int8>(ms().smoothScroll + 2);
        if (!(ms().smoothScroll < 0))
            ms().smoothScroll = 0;
    }
    else if (ms().smoothScroll > 0)
    {
        ms().smoothScroll = static_cast<int8>(ms().smoothScroll - 2);
        if (!(ms().smoothScroll > 0))
            ms().smoothScroll = 0;
    }
    ms().executeCode = 0;
    ms().keyReturnCode = 0x11000000u;
    int32_t heldKey = AMenuNewProcessInput(-1, static_cast<eMenuInputStyle>(3), pHeader);
    int32_t hitKey = AMenuNewProcessInput(-1, static_cast<eMenuInputStyle>(2), pHeader);
    const uint16_t evenMore = h.evenMoreFlags;
    if ((evenMore & 0x100u) != 0)
    {
        // Left and right change places.
        for (int32_t *key : {&hitKey, &heldKey})
            *key = *key == 3 ? 4 : *key == 4 ? 3 : *key;
    }
    if ((h.flags & 0x80u) != 0)
    {
        // Select does nothing and the top button selects.
        for (int32_t *key : {&hitKey, &heldKey})
            *key = *key == 6 ? 0 : *key == 9 ? 6 : *key;
    }
    if ((h.moreFlags & 0x100u) != 0)
    {
        // A sideways menu: left and right move, up and down do nothing.
        if (hitKey == 3)
            hitKey = 1;
        else if (hitKey == 4)
            hitKey = 2;
        else if (hitKey < 4 && !(hitKey < 1))
            hitKey = 0;
    }
    else
    {
        // A slider repeats while left or right is held.
        const ptr32<sAMenuHeader> active = ms().pActiveMenu;
        const ptr32<sMenuItemStd> selected = item(active, active->selection);
        if (selected && selected->pMenuVar && (selected->flags & 0x300u) != 0 && heldKey < 5 && !(heldKey < 3))
            hitKey = heldKey;
    }
    if (hitKey != 0)
        at<uint32_t>(kFrontEnd + 0x2a8u) = 0;
    const auto enterGlobalDest = [&]
    {
        minMenuSel = 0;
        visMenuSel = 0;
        ms().pLastActiveMenu = ms().pActiveMenu;
        const ptr32<sAMenuHeader> dest = h.pGlobalDestMenu;
        ms().pActiveMenu = dest;
        const int32_t first = firstSelectable(f, dest);
        ms().pActiveMenu->selection = static_cast<int8>(first);
    };
    if (!h.pMenuStart)
    {
        // A menu without items: with flag 1 of evenMoreFlags up and down count a selection up to its special store
        // and select goes to its global destination; without it select means back. Its other keys do nothing.
        switch (hitKey)
        {
        case 1:
            if ((evenMore & 1u) != 0)
            {
                h.selection = static_cast<int8>(h.selection - 1);
                if (h.selection < 0)
                    h.selection = static_cast<int8>(static_cast<uint8_t>(h.specialStore) - 1u);
                sfxFlag = 1;
            }
            ms().keyReturnCode = 0x11000003u;
            hitKey = 0;
            break;
        case 2:
            if ((evenMore & 1u) != 0)
            {
                h.selection = static_cast<int8>(h.selection + 1);
                if (!(static_cast<uint32_t>(static_cast<int32_t>(h.selection)) < h.specialStore))
                    h.selection = 0;
                sfxFlag = 1;
            }
            ms().keyReturnCode = 0x11000004u;
            hitKey = 0;
            break;
        case 3:
        case 4:
            hitKey = 0;
            break;
        case 6:
            if ((evenMore & 1u) == 0)
            {
                hitKey = 5;
                break;
            }
            sfxFlag = 2;
            if (h.pGlobalDestMenu)
            {
                enterGlobalDest();
                ms().pActiveMenu->pLinkMenuReturn = pHeader;
                ms().newMenuFlag = 1;
            }
            ms().executeCode = 0x10101010u;
            hitKey = 0;
            break;
        default: break;
        }
    }
    if (special == 1)
        hitKey = 5;

    enum class Next { End, Back };
    Next next = Next::End;
    const auto scrolls = [&](int32_t newSelection) { return lastSelection != newSelection && (h.flags & 0x2000u) != 0 && ms().smoothScroll == 0; };
    switch (hitKey)
    {
    case 10: ms().keyReturnCode = 0x11000006u; break;
    case 9: ms().keyReturnCode = 0x11000005u; break;
    case 3:
        ms().selectDisabled = 0;
        AMenuChangeMVAR(pHeader, 1, 0);
        break;
    case 4:
        ms().selectDisabled = 0;
        AMenuChangeMVAR(pHeader, 2, 0);
        break;
    case 1:
    {
        ms().selectDisabled = 0;
        if (selMenuItemCount < 2)
            break;
        if (ms().smoothScroll != 0)
        {
            ms().keyReturnCode = 0x11000003u;
            break;
        }
        bool scrolled = false;
        const int32_t scrollItems = h.scrollItems;
        sfxFlag = 1;
        if (scrollItems != 0 && static_cast<int32_t>(h.menuItems) - scrollItems < h.selection + ms().scrollSelector)
        {
            // In the lower part of a scrolling menu the selector moves, not the list.
            ms().scrollSelector = static_cast<int8>(ms().scrollSelector - 1);
            scrolled = !(ms().scrollSelector < 0);
        }
        uint8_t selection = static_cast<uint8_t>(h.selection);
        if (!scrolled)
        {
            const ptr32<sMenuItemStd> list = h.pMenuStart;
            const int32_t scrollStart = h.scrollStart;
            const int32_t vis = visMenuSel;
            do
            {
                const int32_t before = static_cast<int8_t>(selection);
                if (scrollStart + vis < before)
                    h.selection = static_cast<int8>(selection - 1u);
                else if ((h.flags & 1u) != 0)
                    h.selection = static_cast<int8>(f.at<uint8_t>(4) + (h.menuItems - 1u - f.at<uint8_t>(0)));
                else
                {
                    h.selection = 0;
                    if (before == 0)
                        sfxFlag = 0;
                }
                selection = static_cast<uint8_t>(h.selection);
                ptr32<sMenuItemStd> p = list;
                for (int32_t i = 0; p && i != h.selection; ++i)
                    p = p->pNext;
                if ((p->statusFlags & 1u) == 0) // From address 0x17 for no item.
                    break;
            } while (true);
        }
        const int32_t now = static_cast<int8_t>(selection);
        if (scrolls(now + ms().scrollSelector) && !scrolled && !(now < h.scrollStart))
            ms().smoothScroll = static_cast<int8>(0 - h.changeY);
        ms().keyReturnCode = 0x11000003u;
        break;
    }
    case 2:
    {
        ms().selectDisabled = 0;
        if (selMenuItemCount < 2)
            break;
        if (ms().smoothScroll != 0)
        {
            ms().keyReturnCode = 0x11000004u;
            break;
        }
        const int32_t menuItems = h.menuItems;
        sfxFlag = 1;
        const int32_t scrollItems = h.scrollItems;
        const ptr32<sMenuItemStd> list = h.pMenuStart;
        const int32_t lastScrolled = menuItems - scrollItems;
        const int32_t end = menuItems - minMenuSel + visMenuSel;
        const uint8_t visLow = f.at<uint8_t>(4);
        uint8_t selection = static_cast<uint8_t>(h.selection);
        do
        {
            h.selection = static_cast<int8>(selection + 1u);
            const int32_t moved = h.selection;
            const bool inside = scrollItems == 0 ? moved < end : !(lastScrolled + h.scrollStart < moved);
            if (!inside)
            {
                if ((h.flags & 1u) != 0)
                    h.selection = static_cast<int8>(static_cast<uint8_t>(h.scrollStart) + visLow);
                else
                {
                    // Past the end without wrapping: the selection becomes 1.
                    const int32_t was = h.selection;
                    h.selection = 1;
                    if (was == 1)
                        sfxFlag = 0;
                }
            }
            selection = static_cast<uint8_t>(h.selection);
            ptr32<sMenuItemStd> p = list;
            for (int32_t i = 0; p && i != h.selection; ++i)
                p = p->pNext;
            if ((p->statusFlags & 1u) == 0)
                break;
        } while (true);
        if (scrolls(static_cast<int8_t>(selection) + ms().scrollSelector))
            ms().smoothScroll = static_cast<int8>(h.changeY);
        ms().keyReturnCode = 0x11000004u;
        break;
    }
    case 5: next = Next::Back; break;
    case 6:
    {
        const auto visibleSelection = [&]
        {
            return h.pSelection ? static_cast<int32_t>(h.selection) : GetNthVisibleItem(pHeader, h.selection + ms().scrollSelector);
        };
        const int32_t keepSelection = visibleSelection();
        ptr32<sMenuItemStd> pMenuItem = item(pHeader, keepSelection);
        if (AMenuTestDisableMVAR(pHeader, pMenuItem) != 0)
            break;
        // An item can be for one pad only.
        const uint16_t itemFlags = pMenuItem->flags;
        if ((itemFlags & 5u) != 0)
        {
            if ((itemFlags & 1u) != 0 && G(lastHitPadG) != 0)
                break;
            if ((itemFlags & 4u) != 0 && G(lastHitPadG) != 1)
                break;
        }
        sfxFlag = 2;
        if (ms().selectDisabled != 0)
            break;
        if (ms().EnabledSelectionCallBack.address != 0 && guest_call<int32_t>(ms().EnabledSelectionCallBack.address) == 1)
            break;
        const uint8_t selectionBefore = static_cast<uint8_t>(h.selection);
        pMenuItem = item(pHeader, visibleSelection());
        const ptr32<sMenuItemStd> list = h.pMenuStart;
        if ((h.moreFlags & 0x400u) == 0)
        {
            // Back to the first selectable item (the counts run on from the ones made at the start).
            if ((h.evenMoreFlags & 4u) != 0)
                list->flags = static_cast<uint16>(list->flags & 0xff9fu);
            int32_t startMenuSel = 9999, first = 0;
            if (list)
            {
                int32_t i = 0;
                for (ptr32<sMenuItemStd> p = list; p; p = p->pNext, ++i)
                {
                    const uint16_t flags = p->flags;
                    if ((flags & 0x40u) != 0)
                    {
                        ++minMenuSel;
                        if ((flags & 0x20u) == 0)
                            ++visMenuSel;
                    }
                    else
                    {
                        if (i < startMenuSel)
                            startMenuSel = i;
                        ++selMenuItemCount;
                    }
                }
                first = startMenuSel - visMenuSel;
                if (first < 0)
                    first = 0;
            }
            h.selection = static_cast<int8>(first);
        }
        else
            h.selection = static_cast<int8>(selectionBefore + static_cast<uint8_t>(ms().scrollSelector));
        ms().scrollSelector = 0;
        if ((pMenuItem->flags & 0x10u) != 0)
        {
            next = Next::Back;
            break;
        }
        const int32_t itemSelected = selectItem(pHeader, pMenuItem);
        if ((itemSelected & 1) != 0)
        {
            const ptr32<sAMenuHeader> active = ms().pActiveMenu;
            if ((active->evenMoreFlags & 0x20u) != 0)
                active->selection = 1;
            ms().newMenuFlag = 1;
            break;
        }
        const ptr32<sAMenuHeader> dest = h.pGlobalDestMenu;
        if (dest)
        {
            if ((dest->evenMoreFlags & 8u) != 0 && !InputPadPresent(static_cast<eInputPad>(5)))
                break;
            if (ms().executeCode == 0x100001f9u)
            {
                next = Next::Back;
                break;
            }
            enterGlobalDest();
            const ptr32<sAMenuHeader> active = ms().pActiveMenu;
            if (active->selectionTag == 0x1000020du)
                active->selection = 1;
            if ((itemSelected & 2) == 0)
                ms().executeCode = h.selectionTag;
            ms().pActiveMenu->pLinkMenuReturn = pHeader;
            ms().newMenuFlag = 1;
            break;
        }
        if ((itemSelected & 2) == 0)
        {
            next = Next::Back;
            break;
        }
        // An item with only a tag: the selection stays where it was; most such items make no sound.
        h.selection = static_cast<int8>(keepSelection);
        if (!pMenuItem)
        {
            sfxFlag = 0;
            break;
        }
        if (pMenuItem->selectionTag - 0x10000223u < 2u)
        {
            sfxFlag = 2;
            break;
        }
        const uint32_t menuTag = h.selectionTag;
        if (menuTag == 0x10000282u || menuTag == 0x10000284u || menuTag == 0x10000290u)
            break;
        if (menuTag != 0x1000028fu)
            sfxFlag = 0;
        break;
    }
    default: break;
    }
    if (next == Next::Back)
    {
        SpecialCaseMusicVolFixupBackout(ms().pActiveMenu);
        ms().selectDisabled = 0;
        ms().scrollSelector = 0;
        if (!h.pLinkMenuReturn)
            ms().executeCode = 0x11000000u;
        else
        {
            if (hitKey != 6)
                sfxFlag = 3;
            ms().pLastActiveMenu = ms().pActiveMenu;
            const ptr32<sAMenuHeader> parent = h.pLinkMenuReturn;
            ms().newMenuFlag = 1;
            ms().pActiveMenu = parent;
            ms().executeCode = 0x11000001u;
            // Leaving a controller setup menu puts its first item's variable back to the stored layout.
            const uint32_t menuTag = h.selectionTag;
            if (menuTag == 0x10000282u || menuTag == 0x10000284u)
            {
                const uint32_t stored = kMainVars + (menuTag == 0x10000282u ? 0x3e3u : 0x3e4u);
                const ptr32<sMenuItemVar> pVar = h.pMenuStart->pMenuVar; // From address 4 for no item.
                if (pVar)
                {
                    const ptr32<int32> feedback = pVar->pFeedbackVar;
                    pVar->value = at<uint8_t>(stored);
                    if (feedback)
                        *feedback = at<uint8_t>(stored);
                }
            }
        }
    }
    guest_call<int32_t>(kHideEmptyMenu, ms().pActiveMenu);
    UpdateSingleVisibleMenu();
    ms().currentMenuSelection = h.selection;
    if (h.pSelection)
        *h.pSelection = static_cast<uint8>(h.selection);
    if (sfxFlag == 1)
        sfxMove();
    else if (sfxFlag == 2)
        sfxSelect();
    else if (sfxFlag == 3)
        sfxBack();
}

// A scrolling list: up and down move the selection, the list scrolls a pixel a frame to keep it in the middle, and
// select gives the line's number (or 0x10 for a line starting "-") as the execute code.
void AMenuProcessNewScroll(ptr32<sAMenuHeader> pHeader, int32_t)
{
    Locals f(0x40);
    sAMenuHeader &h = *pHeader;
    const auto ms = []() -> sMenuSystem & { return menuSystem(); };
    int16_t &scroll = at<int16_t>(kFrontEnd + 0x2cau);
    f.sw(0, 0);
    f.sw(4, 0);
    f.sw(8, 0);
    ms().newMenuFlag = 0;
    const uint32_t lastDir = ms().keyReturnCode;
    ms().keyReturnCode = 0x11000000u;
    ms().executeCode = 0;
    const int32_t hitKey = AMenuNewProcessInput(-1, static_cast<eMenuInputStyle>(2), pHeader);
    if (hitKey != 0)
        at<uint32_t>(kFrontEnd + 0x2a8u) = 0;
    const auto overflow = [&] { return (h.menuItems + 1) * h.changeY - (static_cast<int32_t>(h.height) - h.scrollStart); };
    if (overflow() > 0)
    {
        const int32_t middle = h.menuItems >> 1;
        if (h.selection < middle)
            scroll = static_cast<int16_t>(scroll - 1);
        else if (middle < h.selection)
            scroll = static_cast<int16_t>(scroll + 1);
    }
    else
        scroll = 0;
    if (scroll < 0)
        scroll = 0;
    {
        const int32_t most = overflow();
        if (most < scroll && most > 0)
            scroll = static_cast<int16_t>(most);
    }
    switch (hitKey)
    {
    case 1:
        if (lastDir != 0x11000003u && h.selection > 0)
        {
            h.selection = static_cast<int8>(h.selection - 1);
            sfxMove();
        }
        ms().keyReturnCode = 0x11000003u;
        break;
    case 2:
        if (lastDir != 0x11000004u && h.selection < static_cast<int32_t>(h.menuItems) - 1)
        {
            h.selection = static_cast<int8>(h.selection + 1);
            sfxMove();
        }
        ms().keyReturnCode = 0x11000004u;
        break;
    case 5:
        ms().selectDisabled = 0;
        ms().scrollSelector = 0;
        if (h.pLinkMenuReturn)
        {
            sfxBack();
            ms().pLastActiveMenu = ms().pActiveMenu;
            const ptr32<sAMenuHeader> parent = h.pLinkMenuReturn;
            ms().newMenuFlag = 1;
            ms().pActiveMenu = parent;
            ms().executeCode = 0x11000001u;
        }
        else
            ms().executeCode = 0x11000000u;
        break;
    case 6:
    {
        ms().executeCode = h.selectionTag;
        const ptr32<sAMenuHeader> dest = h.pGlobalDestMenu;
        if (dest)
        {
            if ((dest->evenMoreFlags & 8u) != 0 && !InputPadPresent(static_cast<eInputPad>(5)))
                break;
            sfxSelect();
            f.sw(0, 0);
            f.sw(4, 0);
            ms().pLastActiveMenu = ms().pActiveMenu;
            const ptr32<sAMenuHeader> next = h.pGlobalDestMenu;
            ms().pActiveMenu = next;
            const int32_t first = firstSelectable(f, next);
            ms().pActiveMenu->selection = static_cast<int8>(first);
        }
        else if (h.selectionTag != 0)
            sfxSelect();
        else
        {
            ms().executeCode = static_cast<uint32_t>(h.selection + 1);
            const ptr32<char> line = text(ptr32<sTextGroup>(h.pMenuStart.address + static_cast<uint32_t>(h.selection * 0x10)));
            if (line[0] == '-')
                ms().executeCode = 0x10;
        }
        break;
    }
    default: break;
    }
    ms().currentMenuSelection = h.selection;
}

// Moving the picture on the screen: the offsets go into both display buffers' DISPLAY registers.
void AMenuProcessScreenAdjust(ptr32<sAMenuHeader> pHeader)
{
    GuestFrame frame(0x50);
    const auto ms = []() -> sMenuSystem & { return menuSystem(); };
    int32_t x = at<int16_t>(kFrontEnd + 0x264u), y = at<int16_t>(kFrontEnd + 0x266u);
    ms().keyReturnCode = 0x11000000u;
    ms().executeCode = 0;
    const int32_t hitKey = AMenuNewProcessInput(-1, static_cast<eMenuInputStyle>(2), pHeader);
    switch (hitKey)
    {
    case 1:
        y -= 2;
        ms().keyReturnCode = 0x11000003u;
        break;
    case 2:
        y += 2;
        ms().keyReturnCode = 0x11000004u;
        break;
    case 3:
        x -= 1;
        ms().keyReturnCode = 0x11000001u;
        break;
    case 4:
        x += 1;
        ms().keyReturnCode = 0x11000002u;
        break;
    case 5:
    case 6:
        if (hitKey == 5)
        {
            // Back: the offsets as they were.
            y = at<int16_t>(kFrontEnd + 0x2c4u);
            x = at<int16_t>(kFrontEnd + 0x2c0u);
            sfxBack();
        }
        else
            sfxSelect();
        if (pHeader->pLinkMenuReturn)
        {
            const ptr32<sAMenuHeader> active = ms().pActiveMenu;
            ms().executeCode = 0x11000001u;
            ms().pLastActiveMenu = active;
            ms().pActiveMenu = pHeader->pLinkMenuReturn;
        }
        else
            ms().executeCode = 0x11000000u;
        break;
    default: break;
    }
    if (!(x < 0x1f))
        x = 0x1e;
    if (!(-0xb < x))
        x = -0xa;
    if (!(y < 0x1f))
        y = 0x1e;
    if (!(-0xb < y))
        y = -0xa;
    const uint64_t dx = static_cast<uint32_t>(x * 3 + 0x290) & 0xfffu;
    const uint64_t dy = static_cast<uint64_t>(static_cast<uint32_t>(y + (RC_VERSION_EU ? 0x38 : 0x30)) & 0x7ffu) << 12;
    for (const uint32_t display : {(sym::gGfxP2_DBuff + 0x40u), (sym::gGfxP2_DBuff + 0x18u)})
    {
        uint64_t &value = at<uint64_t>(display);
        value = (((value & 0xfffffffffffff000ull) | dx) & 0xffffffffff800fffull) | dy;
    }
    at<int16_t>(kFrontEnd + 0x266u) = static_cast<int16_t>(y);
    at<int16_t>(kFrontEnd + 0x264u) = static_cast<int16_t>(x);
}

// A name-entry menu: the cursor moves over the character grid (skipping the empty end of the last row) and on to
// the OK and cancel rows below it; select adds the character, accepts or cancels; the top button deletes.
void AMenuProcessAreaMenu(ptr32<sAMenuHeader> pHeader)
{
    GuestFrame frame(0x50);
    sAMenuHeader &h = *pHeader;
    const auto ms = []() -> sMenuSystem & { return menuSystem(); };
    int32_t &pause = G(areaMenuPauseG);
    pause = pause - 1;
    if (pause < 0)
        pause = 0;
    const int32_t rowCount = ms().areaMenu.rowCount;
    int32_t colCount = ms().areaMenu.colCount;
    const int32_t hitKey = AMenuNewProcessInput(-1, static_cast<eMenuInputStyle>(2), pHeader);
    const auto moved = [&]
    {
        if (pause != 0)
            return false;
        pause = 10;
        sfxMove();
        return true;
    };
    switch (hitKey)
    {
    case 1:
        if (!moved())
            break;
        if (h.rowSelect > 0)
        {
            h.rowSelect = static_cast<int8>(h.rowSelect - 1);
            if (validSelection(pHeader) == 2)
                h.rowSelect = static_cast<int8>(h.rowSelect - 1);
        }
        else
            h.rowSelect = static_cast<int8>(rowCount + 2);
        break;
    case 2:
    {
        if (!moved())
            break;
        h.rowSelect = static_cast<int8>(h.rowSelect + 1);
        if (validSelection(pHeader) == 2)
            h.rowSelect = static_cast<int8>(h.rowSelect + 1);
        if (rowCount + 2 < h.rowSelect)
            h.rowSelect = 0;
        break;
    }
    case 3:
        if (!moved())
            break;
        if (rowCount < h.rowSelect)
            break;
        h.colSelect = static_cast<int8>(h.colSelect != 0 ? h.colSelect - 1 : colCount - 1);
        --colCount;
        while (validSelection(pHeader) == 2)
        {
            h.colSelect = static_cast<int8>(h.colSelect - 1);
            if (h.colSelect == 0)
                h.colSelect = static_cast<int8>(colCount);
        }
        break;
    case 4:
        if (!moved())
            break;
        if (rowCount < h.rowSelect)
            break;
        h.colSelect = static_cast<int8>(h.colSelect < colCount - 1 ? h.colSelect + 1 : 0);
        while (validSelection(pHeader) == 2)
        {
            h.colSelect = static_cast<int8>(h.colSelect + 1);
            if (colCount < h.colSelect)
                h.colSelect = 0;
        }
        break;
    case 5:
        sfxBack();
        if (h.pLinkMenuReturn)
        {
            sfxBack();
            ms().pActiveMenu = h.pLinkMenuReturn;
            ms().executeCode = 0x11000001u;
        }
        else
            ms().executeCode = 0x11000000u;
        break;
    case 6:
        sfxSelect();
        if (h.rowSelect == rowCount + 1)
            selectItem(pHeader, h.pMenuStart);
        else if (h.rowSelect == rowCount + 2)
        {
            if (h.pLinkMenuReturn)
                ms().pActiveMenu = h.pLinkMenuReturn;
            else
                ms().executeCode = 0x11000000u;
        }
        else
        {
            const ptr32<char> input(G(pMenuSystemG).address + 0xc4u);
            if (std::strlen(input.get()) < ms().areaMenuMaxStringLength)
            {
                const ptr32<char> pChars = text(h.pCharList);
                const uint8_t c = at<uint8_t>(pChars.address + static_cast<uint32_t>(h.colSelect + h.rowSelect * colCount));
                at<uint8_t>(kCharString) = c == 0xc6 ? 0x20 : c; // 0xc6 stands for a space.
                std::strcat(ptr32<char>(G(pMenuSystemG).address + 0xc4u).get(), ptr32<char>(kCharString).get());
            }
        }
        break;
    case 9:
    {
        sfxBack();
        const ptr32<char> input(G(pMenuSystemG).address + 0xc4u);
        const int32_t length = static_cast<int32_t>(std::strlen(input.get()));
        if (length > 0)
            at<uint8_t>(G(pMenuSystemG).address + static_cast<uint32_t>(length) + 0xc3u) = 0;
        break;
    }
    default: break;
    }
    ms().currentMenuSelection = -1;
}

void AMenuProcessAll(ptr32<sAMenuHeader> pHeader)
{
    sAMenuHeader &h = *pHeader;
    {
        GuestFrame frame(0x20);
        if (G(pLastHeader) != pHeader && (h.evenMoreFlags & 0x40u) == 0 && (h.flags & 0x4000u) == 0)
            AMenuEntrySettingsMVAR(pHeader);
        G(pLastHeader) = pHeader;
        // A menu over the front-end camera's fly-by is not drawn while the camera moves, or for a few frames after.
        const bool hidden = (h.moreFlags & 1u) != 0 &&
                            (at<int32_t>(at<uint32_t>(kCameraPtr) + 0xd8u) != -1 || menuSystem().specialStatsHoldoff != 0);
        if (!hidden)
            AMenuDisplayAll(pHeader);
        AMenuDisplayOptions(pHeader);
    }
    if ((h.flags & 0x4000u) != 0)
        AMenuProcessAreaMenu(pHeader);
    else if (menuSystem().screenAdjust != 0)
        AMenuProcessScreenAdjust(pHeader);
    else if ((h.evenMoreFlags & 0x40u) != 0)
        AMenuProcessNewScroll(pHeader, 0);
    else
        AMenuProcess(pHeader, 0);
}

} // namespace rc

RC_DECOMP_FN_SETUP(AMenuNewProcessInput, rc::fuzz::amenuInput)
RC_DECOMP_FN_SETUP(AMenuNewProcessInputSub, rc::fuzz::amenuInput)
RC_DECOMP_FN_SETUP(AMenuProcess, rc::fuzz::amenuProcess)
RC_DECOMP_FN_SETUP(AMenuProcessNewScroll, rc::fuzz::amenuProcessScroll)
RC_DECOMP_FN_SETUP(AMenuProcessScreenAdjust, rc::fuzz::amenuProcess)
RC_DECOMP_FN_SETUP(AMenuProcessAreaMenu, rc::fuzz::amenuProcessArea)
RC_DECOMP_FN_SETUP_HEAP(AMenuProcessAll, rc::fuzz::amenuDraw)

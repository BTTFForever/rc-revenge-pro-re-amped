// Shared by the AMENU.c sources (amenu.cpp, amenu_display.cpp, amenu_process.cpp, amenu_prog.cpp): the unit's
// functions, the native functions of other units they call, guest addresses and small helpers.
#pragma once

#include "rc_decomp.h"

#include <cstring>

namespace rc {

// Other units, native.
uint32 FontLength(ptr32<char> pString);                                                    // font.cpp
ptr32<char> FontPrint(int16 x, int16 y, ptr32<char> pString);
void FontPrintRight(int16 x, int16 y, ptr32<char> pString);
void FontSetColour(uint8 red, uint8 green, uint8 blue);
void FontSetGraphicFn(ptr32<void> pFn);
int32_t AStringPrepareWrap(ptr32<aStringTemp> pDestStrings, ptr32<char> pString, ptr32<sAMenuHeader> pHeader); // astring.cpp
int32_t AStringWrap(int32_t posX, int32_t posY, ptr32<char> pString, ptr32<sAMenuHeader> pHeader);
int32_t AStringCentre(int32_t startX, int32_t width, ptr32<char> pText, int32_t stringWidth);
void RectRender(ptr32<_sRect> pRect, f32 otz);                                             // rect.cpp
void RectRenderGouraud(ptr32<_sGRect> pGRect, f32 otz);
void SpriteRender(ptr32<_sSprite> pSprite, ptr32<_sSpriteParams> pSpriteParams, f32 otz);  // sprite.cpp
void SpriteRenderHalf(ptr32<_sSprite> pSprite, ptr32<_sSpriteParams> pSpriteParams, f32 otz);
void SpriteRenderScaled(ptr32<_sSprite> pSprite, ptr32<_sSpriteParams> pSpriteParams, f32 otz);
void GfxP2_Scissor(int32_t x1, int32_t y1, int32_t x2, int32_t y2);                        // rdr.cpp
void GfxP2_ZWriteEnable();
void HudMenuBorder(ptr32<_sSprite> pSpr, int32 iCX, int32 iCY, int32 iShine);              // hud.cpp
bool_ InputHit(uint8 key);                                                                 // input.cpp
bool_ InputHeld(uint8 key);                                                                // ai.cpp
void InputReset();
void InputResetHit(uint8 key);
bool_ InputPadPresent(eInputPad ePad);
bool_ InputDualShockPresent(eInputPad ePad);
uint8 InputGetPadID(eInputPad ePad);
int16 SfxPlay(int16 wSampleId, uint8 bVolume, uint8 bLRPan, uint8 bFBPan, int16 wPitch, int16 wFlags, int16 chainId); // sfx.cpp
void RevSfxModifyVolume(int32 flag);                                                       // revsfx.cpp
void RCMusicModifyVolume(int32 fade, int32 flag);                                          // rcmusic.cpp

// AMENU.c.
void AMenuDisplayAll(ptr32<sAMenuHeader> pHeader);
ptr32<char> AMenuDisplayTitle(ptr32<sAMenuHeader> pHeader, ptr32<sUIcolourSet> pColourSet, int32_t posY);
int32_t AMenuDisplayMIT(ptr32<sAMenuHeader> pHeader, ptr32<sMenuItemStd> pMenuItem, ptr32<sUIcolourSet> pColourSet, int32_t posX, int32_t posY, int32_t flags);
int32_t AMenuTestDisableMVAR(ptr32<sAMenuHeader> pHeader, ptr32<sMenuItemStd> pMenuItem);
void AMenuSetMITColour(ptr32<sAMenuHeader> pHeader, ptr32<sMenuItemStd> pMenuItem, ptr32<sUIcolourSet> pColourSet, int32_t menuItem);
void AMenuDisplay(ptr32<sAMenuHeader> pHeader);
void AMenuDisplayNewScroll(ptr32<sAMenuHeader> pHeader);
void AMenuDisplayAreaMenu(ptr32<sAMenuHeader> pHeader);
int32_t AMenuNewProcessInput(int32_t pad, eMenuInputStyle style, ptr32<sAMenuHeader> pHeader);
int32_t AMenuNewProcessInputSub(int32_t pad, eMenuInputStyle style);
void AMenuChangeMVAR(ptr32<sAMenuHeader> pHeader, int32_t changeType, int32_t special);
void AMenuProcess(ptr32<sAMenuHeader> pHeader, int32_t special);
void AMenuProcessNewScroll(ptr32<sAMenuHeader> pHeader, int32_t special);
void AMenuProcessScreenAdjust(ptr32<sAMenuHeader> pHeader);
ptr32<sAreaMenuInfo> AMenuAreaMenuSetup(ptr32<sAMenuHeader> pHeader);
void AMenuProcessAreaMenu(ptr32<sAMenuHeader> pHeader);
void AMenuDisplayOptions(ptr32<sAMenuHeader> pHeader);
void AMenuProcessAll(ptr32<sAMenuHeader> pHeader);
void AMenuDrawRect(ptr32<sAMenuHeader> pHeader, int32_t flags);
void AMenuDrawBorder(ptr32<sAMenuHeader> pHeader, int32_t flags);
void AMenuDrawBox(ptr32<sAMenuHeader> pHeader, int32_t flags);
void AMenuDrawScreenBox(ptr32<sAMenuHeader> pHeader, int32_t flags);
void AMenuSnapsize(ptr32<sAMenuHeader> pHeader);
int32_t AmenuCalcSizeMIT(ptr32<sMenuItemStd> pMenuItem, int32_t maxWidth);
void AMenuCalcSize(ptr32<sAMenuHeader> pHeader);
void DisplayUIbar(ptr32<sBarDetails> pBar);
void UserInterfaceInit();
void UserInterfaceTerm();
void AMenuRectClipStart(ptr32<sAMenuHeader> pHeader, int32_t yOffset, int32_t clipExtraHeight);
void AMenuRectClipEnd(ptr32<sAMenuHeader> pHeader, int32_t yOffset);
void DisplayTickBox(int32_t x, int32_t y, int32_t value);
void DisplayTickBoxText(ptr32<sAMenuHeader> pHeader, int32_t x, int32_t y, int32_t value, ptr32<char> pText, int32_t type);
int32_t DisplayTrackGraphic(int32_t x, int32_t y, int32_t scale, int32_t width, int32_t level);
int32_t DisplayProgressHeader(ptr32<sAMenuHeader> pHeader, ptr32<_sFrontGameStateData> pGameStateData, int32_t x, int32_t y);
int32_t DisplayProgressEntry(ptr32<sAMenuHeader> pHeader, ptr32<_sFrontGameStateData> pGameStateData, int32_t x, int32_t y, int32_t width, int32_t level);
int32_t DisplayTTEntry(ptr32<sAMenuHeader> pHeader, int32_t x, int32_t y, int32_t width, int32_t level);
void BuildProgTTSub(ptr32<sAMenuHeader> pSourceHeader, ptr32<sAMenuHeader> pHeader, int32_t width, ptr32<char> pTitle);
int32_t PTTScroll(ptr32<sAMenuHeader> pHeader);
void NewPTTScroll(ptr32<sAMenuHeader> pHeader);
void BuildProgress(ptr32<sAMenuHeader> pSourceHeader, ptr32<_sFrontGameStateData> pGameStateData, int32_t card);
void xxyBuildProgress(ptr32<sAMenuHeader> pSourceHeader, ptr32<_sFrontGameStateData> pGameStateData, int32_t card);
void xxxBuildProgress(ptr32<sAMenuHeader> pSourceHeader, ptr32<_sFrontGameStateData> pGameStateData, int32_t card);
void OldBuildProgress(ptr32<sAMenuHeader> pSourceHeader, ptr32<_sFrontGameStateData> pGameStateData, int32_t card);
void BuildTimeTrials(ptr32<sAMenuHeader> pSourceHeader);
void xxxBuildTimeTrials(ptr32<sAMenuHeader> pSourceHeader);
void OldBuildTimeTrials(ptr32<sAMenuHeader> pSourceHeader);
void BuildTrackEditorHelp(ptr32<sAMenuHeader> pSourceHeader);
void AMenuEntrySettingsMVAR(ptr32<sAMenuHeader> pHeader);
void CreateProgTTMenuBox(ptr32<sAMenuHeader> pSourceHeader, ptr32<sAMenuHeader> pHeader, int32_t width);
int32_t PS2ScrollMenu(ptr32<sAMenuHeader> pHeader, int32_t speed);
int32_t DisplayTEHelpEntry(ptr32<sAMenuHeader> pHeader, ptr32<sTrackEditorHelpData> pTrackEditorHelpData, int32_t x, int32_t y, int32_t width, int32_t paragraph);
void AMenuInitHeader(ptr32<sAMenuHeader> pHeader, int32_t flags);
void AmenuSetflags(ptr32<sAMenuHeader> pHeader, int32_t flags);
void AmenuClearflags(ptr32<sAMenuHeader> pHeader, int32_t flags);
ptr32<sMenuItemStd> AMenuGetMIT(ptr32<sAMenuHeader> pHeader, int32_t entry);
void AMenuAddMIT(ptr32<sAMenuHeader> pHeader, ptr32<sMenuItemStd> pMenuItem);
ptr32<char> AMenuGetText(ptr32<sTextGroup> pTextGroup);
void MoveUIbox(ptr32<sAMenuHeader> pHeader);
void UIborderInit();
void UIborderTerm();
void UImenuItemInit();
void UImenuItemTerm();
void UImenuVarInit();
void UImenuVarTerm();
void UIgroupInit();
void UIgroupTerm();
void AMenuMenuInit();
void AMenuMenuTerm();
ptr32<sMenuItemStd> AMenuLinkItems(ptr32<sAMenuHeader> pMenuBase, int32_t baseMenuPosition, ptr32<sAMenuHeader> pMenuLink, int32_t linkType, int32_t selectionTag);
void AMenuTGrpInit();
void AMenuTGrpTerm();
void AMenuTLstInit();
void AMenuTLstTerm();
ptr32<char> GetCarText(int32_t carType);
int32_t AMenuProcessGetMinSel(ptr32<sAMenuHeader> pHeader, ptr32<int32> pMinMenuSel, ptr32<int32> pVisMenuSel, ptr32<int32> pSelMenuItemCount);
void AMenuSetAreaMenuMaxStringLength(int32_t length);
void AmenuRemovePercentS(ptr32<char> pDestBuffer, ptr32<char> pSourceBuffer);
int32_t AMenuStringCentre(ptr32<sAMenuHeader> pHeader, ptr32<char> pText, int32_t stringWidth);
ptr32<char> GetGeneralString(int32_t messageNo);
void AMenuAddTextToTGRP(ptr32<sTextGroup> pTextGroup, ptr32<char> pText);
int32_t GetNewMenuFiddleFactor(ptr32<sAMenuHeader> pHeader);
void SetTextOffsetFiddleFactor(ptr32<sAMenuHeader> pHeader, int32_t ff);
ptr32<sUIcolourSet> AMenuGetColourSet(ptr32<sAMenuHeader> pHeader);
ptr32<sBorderSet> AMenuGetBorderSet(ptr32<sAMenuHeader> pHeader);
uint8 DualShockAnalogModeTest(eInputPad ePad, uint32 tag, ptr32<sAMenuHeader> pHeader);
void SpecialCaseMusicVolFixupBackout(ptr32<sAMenuHeader> pHeader);
int32_t DisplayProgTTMenuTitle(ptr32<sAMenuHeader> pSourceHeader, ptr32<sAMenuHeader> pHeader, int32_t width, ptr32<char> pTitle);
int32_t CheckBottomOfScroll(ptr32<sAMenuHeader> pHeader, int32_t y);
ptr32<char> AMenuGetTitleString(ptr32<sAMenuHeader> pHeader);
ptr32<char> AMenuGetDialogString(ptr32<sAMenuHeader> pHeader);
int32_t AMenuDisplayDialogText(int32_t posX, int32_t posY, ptr32<sAMenuHeader> pHeader, ptr32<sUIcolourSet> pColourSet);
int32_t AMenuTabPrint(int32_t posX, int32_t posY, ptr32<char> pBuffer, ptr32<char> pString);
void InputPadReset(int32_t pad);
int32_t AMenuSelectMenuItem(ptr32<sAMenuHeader> pHeader, ptr32<sMenuItemStd> pMenuItem);
int32_t GetNthVisibleItem(ptr32<sAMenuHeader> pHeader, int32_t selection);
void UpdateSingleVisibleMenu();
int32_t AMenuAreaMenuCheckValidSelection(ptr32<sAMenuHeader> pHeader);
void AMenuDisplayPsxKey(int32_t x, int32_t y, int32_t key);
int32_t DisplayLine(ptr32<sAMenuHeader> pHeader, int32_t y);
void DisplayTrackGraphicBorder(int32_t gap, ptr32<_sSpriteParams> pSpriteParams);
int32_t CheckRaceTick(ptr32<_sFrontGameStateData> pGameStateData, int32_t level, int32_t raceType);

namespace amenu {

// Guest code not decompiled yet.
constexpr uint32_t kRCMusicModifyVolume = 0x1413f0u, kRevSfxModifyVolume = 0x144098u;
constexpr uint32_t kSprintf = 0x1b8910u, kSetRewards = 0x121580u, kGetTimeTrialTime = 0x122b28u, kHideEmptyMenu = 0x11e270u;
// Globals without a typed accessor (offsets into mainVarG, frontEndVarG, tEditVarG, cameraVarG, the sound classes).
constexpr uint32_t kMainVars = 0x1ed900u, kFrontEnd = 0x1eca58u, kLevelPtr = 0x327e80u, kFontPtr = 0x328190u;
constexpr uint32_t kTEditGeneralText = 0x1f0fecu, kTEditCarNames = 0x1f0ff0u, kCameraPtr = 0x1eab20u;
constexpr uint32_t kMusicUserVol = 0x1ee8e0u, kSfxUserVol = 0x1f04f4u, kMusicVolG = 0x327dc4u, kSfxVolG = 0x327dc0u;
constexpr uint32_t kKeyPos = 0x1e96e8u, kMenuInputs = 0x1e96c0u, kStickInputs = 0x1e96d0u;

template <typename T>
T &at(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

inline ee_float constant(uint32_t address) { return at<ee_float>(address); }
inline int32_t language() { return at<int32_t>(kMainVars + 0x170u); }
inline sMenuSystem &menuSystem() { return *G(pMenuSystemG); }
inline int8_t &firstDrawPTT() { return at<int8_t>(kFrontEnd + 0x2d4u); }
inline uint32_t shine() { return at<uint32_t>(kFrontEnd + 0x2a8u); }

// The text of a group in the working language; null for no group.
inline ptr32<char> text(ptr32<sTextGroup> group)
{
    return group ? ptr32<char>(at<uint32_t>(group.address + static_cast<uint32_t>(language()) * 4u)) : ptr32<char>(nullptr);
}

// The entry-th item of a menu, or null past the end (AMenuGetMIT, which the original has inline in many places).
inline ptr32<sMenuItemStd> item(ptr32<sAMenuHeader> pHeader, int32_t entry)
{
    ptr32<sMenuItemStd> p = pHeader->pMenuStart;
    for (int32_t i = 0; p && i != entry; ++i)
        p = p->pNext;
    return p;
}

// The original's stack frame, with stores at its offsets: locals passed to drawing functions are built field by
// field, and the fields the original leaves unset keep whatever the stack held.
class Locals
{
public:
    explicit Locals(uint32_t size) : m_frame(size), sp(m_frame.local<uint8_t>(0).address) {}
    template <typename T>
    T &at(uint32_t offset) const { return *static_cast<T *>(guest_host(sp + offset)); }
    void sb(uint32_t offset, uint32_t value) const { at<uint8_t>(offset) = static_cast<uint8_t>(value); }
    void sh(uint32_t offset, uint32_t value) const { at<uint16_t>(offset) = static_cast<uint16_t>(value); }
    void sw(uint32_t offset, uint32_t value) const { at<uint32_t>(offset) = value; }
    uint32_t lhu(uint32_t offset) const { return at<uint16_t>(offset); }
    int32_t lh(uint32_t offset) const { return at<int16_t>(offset); }
    template <typename T>
    ptr32<T> ptr(uint32_t offset) const { return ptr32<T>(sp + offset); }

private:
    GuestFrame m_frame;

public:
    const uint32_t sp;
};

// A 31 by 31 cell of a key sheet, drawn half size at (x, y - 2); the cell's place on the sheet comes from keyPos.
inline void keySprite(const Locals &f, ptr32<_sSprite> pSprite, int32_t x, int32_t y, uint32_t cell, ee_float otz)
{
    f.sh(0x8, at<uint8_t>(kKeyPos + cell) * 2u);
    f.sh(0xa, at<uint8_t>(kKeyPos + cell + 1u) * 2u);
    f.sb(0x12, 0x7f);
    f.sh(0x16, 1);
    f.sw(0x0, 0x20);
    f.sh(0xe, 0x1f);
    f.sh(0x4, x);
    f.sh(0x6, y - 2);
    f.sb(0x10, 0x7f);
    f.sb(0x11, 0x7f);
    f.sh(0x14, 1);
    f.sh(0xc, 0x1f);
    SpriteRenderHalf(pSprite, f.ptr<_sSpriteParams>(0), otz);
}

// A horizontal rule across the menu, one pixel high at y + 4, shading from blue to cyan: a Gouraud rectangle built at
// an offset of the frame.
inline void rule(const Locals &f, uint32_t base, const sAMenuHeader &h, int32_t y, ee_float otz)
{
    const uint32_t left = static_cast<uint16_t>(h.x), right = left + h.width;
    f.sh(base + 0xa, y + 4);
    f.sh(base + 0x12, y + 5);
    f.sh(base + 0xc, right);
    f.sh(base + 0x10, left);
    f.sb(base + 0x1e, 0x7f);
    f.sw(base + 0x0, 0);
    f.sh(base + 0x6, y + 4);
    f.sh(base + 0xe, y + 5);
    f.sh(base + 0x4, left);
    f.sh(base + 0x8, right);
    f.sb(base + 0x14, 0);
    f.sb(base + 0x15, 0);
    f.sb(base + 0x16, 0x7f);
    f.sb(base + 0x20, 0);
    f.sb(base + 0x21, 0);
    f.sb(base + 0x22, 0x7f);
    f.sb(base + 0x18, 0);
    f.sb(base + 0x19, 0x7f);
    f.sb(base + 0x1a, 0x7f);
    f.sb(base + 0x1c, 0);
    f.sb(base + 0x1d, 0x7f);
    RectRenderGouraud(f.ptr<_sGRect>(base), otz);
}

inline int16 i16(int32_t value) { return static_cast<int16>(value); }

// The line height the menus take from the font: half its character height, two less for a font with flag 2.
inline uint8 fontLineHeight()
{
    const ptr32<font> pFont = G(fontVarG).pFont;
    const uint32_t half = pFont->chHeight >> 1;
    return static_cast<uint8>((G(fontVarG).pFont->flags & 2u) != 0 ? half - 2u : half);
}

// The guest's sprintf with one argument after the format.
template <typename A>
void guestSprintf(ptr32<char> dest, ptr32<char> format, A argument) { guest_call<int32_t>(kSprintf, dest, format, argument); }

// The menu sounds: move (0x44), select (0x45) and back (0x43). Through the guest's table: the game gets the native
// SfxPlay from it, and the offline test a logging stand-in.
inline void playSfx(int32_t sample, int32_t pitch) { guest_call<int32_t>(0x1ad0a0u, sample, 0x80, 0xf, 0xf, pitch, 0, 0); }
inline void sfxMove() { playSfx(0x44, 0xf6); }
inline void sfxSelect() { playSfx(0x45, 0x100); }
inline void sfxBack() { playSfx(0x43, 0x100); }

// AMenuSelectMenuItem's body, which the original also has inline in AMenuProcess and AMenuProcessAreaMenu: a link to
// another menu makes it the active one (unless it needs a second pad that is not there), a code link stores the code
// and leaves the menu (back to the parent for 0x100001f9). Bit 0 of the result: something was selected; bit 1: the
// item has a tag, now the execute code.
inline int32_t selectItem(ptr32<sAMenuHeader> pHeader, ptr32<sMenuItemStd> pMenuItem)
{
    int32_t itemSelected = 0;
    uint32_t tag;
    if (pMenuItem->linkType == 0)
    {
        const ptr32<sAMenuHeader> link = pMenuItem->pLinkMenu;
        bool enter = false;
        if (!link)
            tag = pMenuItem->selectionTag;
        else if ((link->evenMoreFlags & 8u) != 0 || pMenuItem->selectionTag == 0x10000200u)
        {
            enter = InputPadPresent(static_cast<eInputPad>(5)) != 0;
            tag = pMenuItem->selectionTag;
            itemSelected = 1;
        }
        else
        {
            tag = pMenuItem->selectionTag;
            itemSelected = 1;
            enter = true;
        }
        if (enter)
        {
            sMenuSystem &ms = menuSystem();
            ms.pLastActiveMenu = ms.pActiveMenu;
            const ptr32<sAMenuHeader> next = pMenuItem->pLinkMenu;
            ms.pActiveMenu = next;
            next->pLinkMenuReturn = pHeader;
        }
    }
    else if (pMenuItem->linkType == 1)
    {
        sMenuSystem &ms = menuSystem();
        const uint32_t code = pMenuItem->pLinkMenu.address;
        ms.executeCode = code;
        if (code == 0x100001f9u)
        {
            ms.pLastActiveMenu = ms.pActiveMenu;
            tag = pMenuItem->selectionTag;
            ms.pActiveMenu = pHeader->pLinkMenuReturn;
        }
        else
        {
            const ptr32<sAMenuHeader> active = ms.pActiveMenu;
            tag = pMenuItem->selectionTag;
            ms.pLastActiveMenu = active;
            ms.pActiveMenu = nullptr;
        }
    }
    else
        tag = pMenuItem->selectionTag;
    sMenuSystem &ms = menuSystem();
    if (tag == 0)
        ms.executeCode = 0;
    else
    {
        ms.executeCode = tag;
        itemSelected |= 2;
    }
    return itemSelected;
}

constexpr uint32_t kCharString = 0x327c28u; // charStringG: a one-character string
constexpr uint32_t kFormatD = 0x327c58u, kTextP1 = 0x327c48u, kTextP2 = 0x327c50u;

} // namespace amenu

} // namespace rc

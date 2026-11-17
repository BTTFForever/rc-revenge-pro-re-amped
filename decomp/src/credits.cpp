// Decompiled from CREDITS.c: the credits page of the front end. The names scroll up the screen (a line every 32
// fields, '*' lines in cyan, '@' lines blank) over fireworks, until the list ends or a key leaves.
//
// Written from the instructions (Ghidra's C as a first reading).

#include "front.h"
#include "front_fuzz.h"

namespace rc {

using namespace front;

namespace {

constexpr int16_t kFirework = 0x23; // the fireworks' effect type

// Unlinks an effect from the effect list and frees it (MemBlockFree with the effect's address in the caller's frame,
// as the original passes it). Returns the next effect.
ptr32<_sVfx_Object> removeEffect(ptr32<_sVfx_Object> pFX, ptr32<ptr32<_sVfx_Object>> pHandle)
{
    _visFxClass &g = G(visFxVarG);
    if (pFX->Prev)
        pFX->Prev->Next = pFX->Next;
    if (pFX->Next)
        pFX->Next->Prev = pFX->Prev;
    if (pFX == g.vfxHeader)
    {
        g.vfxHeader = pFX->Next;
        if (g.vfxHeader)
            g.vfxHeader->Prev = nullptr;
    }
    if (pFX == g.vfxFooter)
    {
        g.vfxFooter = pFX->Prev;
        if (g.vfxFooter)
            g.vfxFooter->Next = nullptr;
    }
    const ptr32<_sVfx_Object> next = pFX->Next;
    *pHandle = pFX;
    guest_call<void>(sym::MemBlockFree, pHandle);
    return next;
}

// Removes the effects for which the test holds (the effect's address kept at the bottom of the original's frame).
template <typename Test>
void removeEffects(uint32_t frameSize, Test test)
{
    GuestFrame frame(frameSize);
    const ptr32<ptr32<_sVfx_Object>> pHandle = frame.local<ptr32<_sVfx_Object>>(0);
    ptr32<_sVfx_Object> pFX = G(visFxVarG).vfxHeader;
    while (pFX)
        pFX = test(pFX) ? removeEffect(pFX, pHandle) : pFX->Next;
}

} // namespace

// The credits, one frame per pass until left (the menu system's execute code becomes 0x11000000).
void CreditsDisplay(ptr32<sAMenuHeader>)
{
    const amenu::Locals f(0xc0);
    sFrontEndClass &fe_ = fe();
    const uint32_t timer = guest_addr(&mainVars().gameTimer);
    G(iCreditInitG) = at<uint32_t>(sym::displayVarG + 0x64u); // uiVSyncInc
    const uint32_t names = fe_.pFELevelData->ppCredits.address;
    guest_call<void>(sym::TimerPause, timer);
    uint32_t shown = 0xffffff9cu; // -100: the field count the lines were last drawn for
    int32_t ended = 0;            // frames since the list ended
    guest_call<void>(sym::VfxSetupCameraMatrix);
    const auto line = [names](int32_t n) { return at<uint32_t>(names + 4u * static_cast<uint32_t>(n)); };
    for (;;)
    {
        const uint32_t fields = at<uint32_t>(sym::displayVarG + 0x64u) - G(iCreditInitG);
        guest_call<void>(sym::InputSnapshot);
        guest_call<void>(sym::VfxCallHandlers);
        killDeadFX();
        guest_call<void>(sym::Rdr_StartFrame);
        int32_t endPhase = 0;
        if (fields != shown && ended == 0)
        {
            int32_t n = (static_cast<int32_t>(fields) >> 5) - 8;
            shown = fields;
            if (n > 0 && line(n) == 0)
                ended = 1;
            else
            {
                // Up to ten lines, from the line scrolling in at the bottom.
                int32_t count = 0;
                for (;;)
                {
                        if (n > 0)
                        {
                            const int32_t y = count * 32 - static_cast<int32_t>(fields & 0x1fu) - 0x10;
                            guest_call<uint32_t>(sym::strcpy, sym::buff, line(n));
                            const char first = at<char>(sym::buff);
                            if (first != '@')
                            {
                                if (first == '*')
                                {
                                    FontSetColour(0, 0x7f, 0x7f);
                                    guest_call<void>(sym::FontCentre, static_cast<int16_t>(y), sym::buff + 1u);
                                }
                                else
                                {
                                    FontSetColour(0x7f, 0x7f, 0x7f);
                                    guest_call<void>(sym::FontCentre, static_cast<int16_t>(y), sym::buff);
                                }
                            }
                        }
                        ++count;
                        if (count >= 10)
                            break;
                        ++n;
                        if (n > 0 && line(n) == 0)
                            break;
                    }
            }
        }
        else if (fields != shown)
            ++ended;
        endPhase = ended >> 5;
        // A firework now and then near the camera, until well after the end.
        if ((fields & 0xfu) == 0 && endPhase < 7)
        {
            const _visFxClass &v = G(visFxVarG);
            f.sh(0x0, static_cast<uint32_t>(static_cast<uint16_t>(v.svCamPos.vx) + (guest_call<uint32_t>(sym::rand) & 0x7ffu) - 0x400u));
            f.sh(0x2, static_cast<uint32_t>(static_cast<uint16_t>(v.svCamPos.vy) + (guest_call<uint32_t>(sym::rand) & 0x7ffu) - 0x400u));
            const uint32_t z = static_cast<uint16_t>(v.svCamPos.vz) + (guest_call<uint32_t>(sym::rand) & 0x7ffu) - 0x400u;
            const auto half = [](int16 m) { return static_cast<uint32_t>(static_cast<int32_t>(m) >> 1); };
            const uint32_t x = f.lhu(0x0) + half(v.mCamInverseRot.m[0][2]);
            const uint32_t y = f.lhu(0x2) + half(v.mCamInverseRot.m[1][2]);
            const uint32_t zz = z + half(v.mCamInverseRot.m[2][2]);
            f.sh(0x0, x);
            f.sh(0x2, y);
            f.sh(0x4, zz);
            guest_call<int32_t>(sym::VfxAddNewPlace, static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(zz), kFirework);
        }
        guest_call<void>(sym::RdrZedsWriteOff, 0);
        VfxCallFireWorkRenderers();
        const int32_t input = guest_call<int32_t>(sym::AMenuNewProcessInput, -1, 2, 0);
        const int32_t key = endPhase == 0xb ? 6 : input;
        if (key >= 5 && (key < 7 || key == 0xb))
        {
            fe_.flagPTT = 0;
            menuSystem().scrollSelector = 0;
            if (key != 6)
                guest_call<int32_t>(sym::SfxPlay, 0x43, 0xc4, 0xf, 0xf, 0x100, 0, 0);
            menuSystem().executeCode = 0x11000000u;
            G(iCreditInitG) = 0;
        }
        if (key != 0)
            fe_.uiDemoLoopWait = 0;
        guest_call<void>(sym::Rdr_EndFrame);
        if (menuSystem().executeCode == 0x11000000u)
            break;
    }
    G(screenSwapFlagG) = 0;
    VfxDeleteFireWorks();
    guest_call<void>(sym::TimerResume, timer);
}

// Frees the effects that have run out.
void killDeadFX()
{
    removeEffects(0x40, [](ptr32<_sVfx_Object> pFX) { return pFX->iLifeSpan == 0; });
}

// Draws the live fireworks.
void VfxCallFireWorkRenderers()
{
    {
        GuestFrame frame(0x30);
        for (ptr32<_sVfx_Object> pFX = G(visFxVarG).vfxHeader; pFX; pFX = pFX->Next)
            if (pFX->iType == kFirework && pFX->iLifeSpan != 0)
                guest_call<void>(pFX->Renderer.address, pFX);
    }
    guest_call<void>(sym::RdrPolyFlush, 0); // a tail jump, after the frame is left
}

// Frees every firework.
void VfxDeleteFireWorks()
{
    removeEffects(0x50, [](ptr32<_sVfx_Object> pFX) { return pFX->iType == kFirework; });
}

void CreditsInit(int32) {}
void CreditsTerm() {}

} // namespace rc

RC_DECOMP_FN_BLOCKING_SETUP_HEAP(CreditsDisplay, rc::fuzz::frontCredits)
RC_DECOMP_FN_SETUP_HEAP(killDeadFX, rc::fuzz::frontEffects)
RC_DECOMP_FN_SETUP_HEAP(VfxCallFireWorkRenderers, rc::fuzz::frontEffects)
RC_DECOMP_FN_SETUP_HEAP(VfxDeleteFireWorks, rc::fuzz::frontEffects)
RC_DECOMP_FN(CreditsInit)
RC_DECOMP_FN(CreditsTerm)

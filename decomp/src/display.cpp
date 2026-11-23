// Decompiled from DISPLAY.c, CELL.c, VIEW3D.c and INIT3D.c: the PlayStation-era display and 3D set-up layer. On the
// PS2 most of it is empty: the display driver is GfxP2, the cell renderer is gone, and what is left keeps the window
// definitions, the PlayStation-style view matrices (normal, Y negated and transposed) and the view cone's side normals.
//
// Never called by the game (no jal and no address taken): DisplaySwapScreens, DisplaySetBackground's neighbours
// DisplayClearBackground, DisplayRenderFrame, DisplayRenderFrameProfileNoDraw, DisplayRegisterDeviceWindow,
// DisplaySetHardwareXY, DisplayTermHiRes and every CELL.c function but CellInit and CellRenderInit.

#include "rc_settings.h" // enhanced recomp: widescreen
#include "rc_widescreen.h"
#include "display_fuzz.h"

#include <cstring>

namespace rc {

bool_ HeapInit(uint32 heapSize); // heap.cpp
void HeapTerm();
void HeapReset();
void RenderInit(uint32 size); // rdr.cpp
void RenderTerm();
void RenderFrameInit(int32 flags);
void TexInit();      // gfxtex.cpp
void FontInit();     // font.cpp
void SpriteInit();   // sprite.cpp
void RendObjInit();  // misc.cpp
void LsInit();
void MeshInit();
void DepthCueInit();
void VISInit();
void Gfx_Init(s32 wd, s32 ht);
void SvfInit(); // skinanim.cpp
void MatPsxInit(ptr32<_matPsx> in);                         // maths.cpp
void Mat3nBuild(ptr32<_mat3n> m, ptr32<_matAngles> a);     // matrix.cpp
uint32 ps2__Vec16Normalise(ptr32<_sVec16> pOut, ptr32<_sVec16> pVec); // psxdefs.cpp
void PS2C_Vec16MulMat(ptr32<_sMat> pMat, ptr32<_sVec16> pIn, ptr32<_sVec32> pOut);
void PS2C_SetTransMatrix(ptr32<_sVec16> pVec);

namespace {

constexpr uint32_t kValidWindow = 0x7f645459u; // window3dDefinition.validID once defined

int32_t add(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b)); }
int32_t sub(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b)); }

template <typename T>
T &mem(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

} // namespace

// DISPLAY.c

void DisplaySetMode()
{
    RC_DIVERT_IF_CHANGED(DisplaySetMode);
    displayClass &display = G(displayVarG);
    display.dispDef.yResolution = 0xf0;
    display.dispDef.modulo = 0x280;
    display.dispDef.colourDepth = 0x100;
    display.state.pDispDef = ptr32<_sDisplayDefinition>(guest_addr(&display.dispDef));
    display.state.driverId = 1;
    display.dispDef.xResolution = 0x280;
    display.state.pDeviceWindow = nullptr;
    display.state.pPort = nullptr;
    display.state.pDriverData = nullptr;
    Gfx_Init(0x280, 0xf0);
    display.viewBuffer = 0;
}

void DisplayInit() { DisplaySetMode(); }
void DisplayTerm() {}
void DisplayEnable() {}
void DisplayDisable() {}

void DisplaySwapScreens()
{
    displayClass &display = G(displayVarG);
    display.viewBuffer ^= 1u;
    display.pDB = ptr32<_sDB>(guest_addr(display.viewBuffer != 0 ? &display.db[1] : &display.db[0]));
}

void DisplaySetBackground(uint8, uint8, uint8) {}
void DisplayClearBackground(uint32) {}
void DisplayRenderFrame() {}
void DisplayRenderFrameProfile(uint32) {}
void DisplayRenderFrameProfileNoDraw(uint32) {}

// Defines a window: the area on screen and the drawing area inside it, clipped to the display's resolution. Returns 0
// if the window is empty or lies wholly off screen. Takes 13 arguments: the last five (from drawWinTopY) come on the
// stack. The two device window origins are not used.
int32_t DisplayDefineDeviceWindow(ptr32<_sDisplayDeviceWindow> pWindowDef, int32_t winTopX, int32_t winTopY,
                                  int32_t winBotX, int32_t winBotY, int32_t, int32_t, int32_t drawWinTopX)
{
    const int32_t drawWinTopY = stack_arg<int32_t>(0), drawWinBotX = stack_arg<int32_t>(1);
    const int32_t drawWinBotY = stack_arg<int32_t>(2), originX = stack_arg<int32_t>(3), originY = stack_arg<int32_t>(4);
    if (winTopX >= winBotX || winTopY >= winBotY)
        return 0;
    _sDisplayDeviceWindow &w = *pWindowDef;
    w.topLeftX = winTopX;
    const int32_t x1 = add(drawWinTopX, originX);
    w.topLeftY = winTopY;
    const int32_t y1 = add(drawWinTopY, originY);
    w.bottomRightY = winBotY;
    const int32_t x2 = add(drawWinBotX, originX);
    w.bottomRightX = winBotX;
    const int32_t y2 = add(drawWinBotY, originY);
    if (x1 < 0 && x2 < 0)
        return 0;
    const ptr32<_sDisplayDefinition> def = G(displayVarG).state.pDispDef;
    const int32_t xRes = def->xResolution;
    if (xRes < x1 && xRes < x2)
        return 0;
    if (y1 < 0 && y2 < 0)
        return 0;
    const int32_t yRes = def->yResolution;
    if (yRes < y1 && yRes < y2)
        return 0;

    _sDisplayRegion &port = w.windowDisplayPort.region, &clip = w.cWRegion;
    if (x1 >= 0)
    {
        port.x1 = x1;
        clip.x1 = drawWinTopX;
    }
    else
    {
        port.x1 = 0;
        clip.x1 = sub(drawWinTopX, x1);
    }
    if (y1 >= 0)
    {
        port.y1 = y1;
        clip.y1 = drawWinTopY;
    }
    else
    {
        port.y1 = 0;
        clip.y1 = sub(drawWinTopY, y1);
    }
    // The resolution is read again for each edge.
    const int32_t xResNow = def->xResolution;
    if (x2 < xResNow)
    {
        port.x2 = x2;
        clip.x2 = drawWinBotX;
    }
    else
    {
        port.x2 = sub(xResNow, 1);
        clip.x2 = sub(sub(drawWinBotX, 1), sub(x2, def->xResolution));
    }
    const int32_t yResNow = def->yResolution;
    if (y2 < yResNow)
    {
        port.y2 = y2;
        clip.y2 = drawWinBotY;
    }
    else
    {
        port.y2 = sub(yResNow, 1);
        clip.y2 = sub(sub(drawWinBotY, 1), sub(y2, def->yResolution));
    }
    const int32_t clipX1 = clip.x1, clipY1 = clip.y1, clipX2 = clip.x2, clipY2 = clip.y2;
    const int32_t portX2 = port.x2, portX1 = port.x1, portY2 = port.y2, portY1 = port.y1;
    w.windowDisplayPort.originX = originX;
    w.windowDisplayPort.originY = originY;
    clip.width = add(sub(clipX2, clipX1), 1);
    port.height = add(sub(portY2, portY1), 1);
    clip.height = add(sub(clipY2, clipY1), 1);
    port.width = add(sub(portX2, portX1), 1);
    return 1;
}

// Makes a window the current one: the screen centre and the drawing limits relative to it.
void DisplayRegisterDeviceWindow(ptr32<_sDisplayDeviceWindow> pWindowDef, uint32)
{
    displayClass &display = G(displayVarG);
    _sDisplayDeviceWindow &w = *pWindowDef;
    _sDisplayPort &port = w.windowDisplayPort;
    const int32_t halfWidth = port.region.width >> 1;
    const int32_t xOffset = static_cast<int16_t>(halfWidth);
    const int32_t halfHeight = port.region.height >> 1;
    const int32_t yOffset = static_cast<int16_t>(halfHeight);
    display.minXS = add(sub(port.region.x1, port.originX), xOffset);
    display.xOffset = static_cast<int16>(halfWidth);
    const int32_t maxX = sub(port.region.x2, port.originX);
    display.state.pPort = ptr32<_sDisplayPort>(pWindowDef.address + 0x28u);
    display.state.pDeviceWindow = pWindowDef;
    display.yOffset = static_cast<int16>(halfHeight);
    display.maxXS = add(maxX, xOffset);
    display.minYS = add(sub(port.region.y1, port.originY), yOffset);
    display.maxYS = add(sub(port.region.y2, port.originY), yOffset);
}

void DisplayInit3dWindows(ptr32<_sDisplayDeviceWindow> pWindowDef, uint32)
{
    displayClass &display = G(displayVarG);
    display.state.pPort = ptr32<_sDisplayPort>(pWindowDef.address + 0x28u);
    display.state.pDeviceWindow = pWindowDef;
}

void DisplaySetHardwareXY(int32, int32) {}
void DisplayInitHiRes() {}
void DisplayTermHiRes() {}

// CELL.c

void CellInit() {}

// The translation for drawing a cell at pTrans: kept, and turned into view space (16-bit) as the GTE's translation.
void CellRenderInit(ptr32<vec32_> pTrans)
{
    const ptr32<_matPsx> view = G(view3dVarG).pPsxViewMatrixNegY;
    std::memcpy(&G(cellRenderVector), pTrans.get(), sizeof(vec32_));
    const uint32_t scratch = guest_addr(&G(scratchPadG));
    const int32_t z = sub(pTrans->z, view->trans.z), x = sub(pTrans->x, view->trans.x), y = sub(pTrans->y, view->trans.y);
    mem<int16_t>(scratch + 4u) = static_cast<int16_t>(z);
    mem<int16_t>(scratch + 0u) = static_cast<int16_t>(x);
    mem<int16_t>(scratch + 2u) = static_cast<int16_t>(y);
    PS2C_Vec16MulMat(view.cast<_sMat>(), ptr32<_sVec16>(scratch), ptr32<_sVec32>(scratch + 0x38u));
    PS2C_SetTransMatrix(ptr32<_sVec16>(scratch + 0x38u));
}

void CellRender(ptr32<cellData>) {}
void CellRenderEnd() {}
void CellRenderPolygonsFast(ptr32<_sPolyGT3>, uint32) {}
void CellRenderPolygonsFastEnd() {}
void CellRenderPolygons(ptr32<_sPolyGT3>, uint32) {}
void CellRenderPolygonsEnd() {}

// VIEW3D.c

// The side planes of the view cone, as normals: (D, 0, -width/2) and (0, D, -height/2), normalised.
void View3dInitViewconeReject()
{
    view3dClass &view = G(view3dVarG);
    const uint16 d = G(init3dVarG).currentD;
    const int16 halfY = static_cast<int16>(-(static_cast<int16_t>(view.dUY) >> 1));
    int16 halfX = static_cast<int16>(-(static_cast<int16_t>(view.dUX) >> 1));
    if (rc::settings::widescreen()) // widescreen: the view cone widened as the projection is
        halfX = static_cast<int16>(static_cast<float>(halfX) / rc::settings::hscale());
    view.srcXNorm.x = static_cast<int16>(d);
    view.srcYNorm.z = halfY;
    view.srcXNorm.z = halfX;
    view.srcYNorm.x = 0;
    view.srcYNorm.y = static_cast<int16>(d);
    view.srcXNorm.y = 0;
    ps2__Vec16Normalise(ptr32<_sVec16>(guest_addr(&view.yNorm)), ptr32<_sVec16>(guest_addr(&view.srcYNorm)));
    ps2__Vec16Normalise(ptr32<_sVec16>(guest_addr(&view.xNorm)), ptr32<_sVec16>(guest_addr(&view.srcXNorm)));
}

void View3dInit()
{
    view3dClass &view = G(view3dVarG);
    view.pPsxViewMatrix = ptr32<_matPsx>(guest_addr(&view.psxViewMatrix));
    view.pPsxViewMatrixNegY = ptr32<_matPsx>(guest_addr(&view.psxViewMatrixNegY));
    view.pPsxTViewMatrix = ptr32<_matPsx>(guest_addr(&view.psxTViewMatrix));
    const ptr32<_matPsx> identity(guest_addr(&view.psxIMatrix));
    MatPsxInit(identity);
    Mat3nBuild(identity.cast<_mat3n>(), ptr32<_matAngles>(guest_addr(&G(nullAngles))));
    view.psxIMatrix.trans.z = 0;
    view.psxIMatrix.trans.x = 0;
    view.psxIMatrix.trans.y = 0;
}

// Takes the frame's view matrix (mirrored in X in mirror mode), and makes the Y-negated and the transposed copies.
void View3dFrameInit(ptr32<window3dDefinition>, ptr32<_matPsx> pViewMatrix)
{
    view3dClass &view = G(view3dVarG);
    const uint32_t base = guest_addr(&view);
    const bool mirrored = G(mirrorVarG).bMirrored != 0;
    uint8_t copy[0x20];
    std::memcpy(copy, pViewMatrix.get(), sizeof(copy));
    std::memcpy(guest_host(base + 0xcu), copy, sizeof(copy));
    if (mirrored)
    {
        const uint16_t xx = mem<uint16_t>(base + 0xcu), yx = mem<uint16_t>(base + 0xeu), zx = mem<uint16_t>(base + 0x10u);
        mem<uint16_t>(base + 0xcu) = static_cast<uint16_t>(-xx);
        mem<uint16_t>(base + 0xeu) = static_cast<uint16_t>(-yx);
        mem<uint16_t>(base + 0x10u) = static_cast<uint16_t>(-zx);
    }
    std::memcpy(copy, guest_host(base + 0xcu), sizeof(copy));
    std::memcpy(guest_host(base + 0x2cu), copy, sizeof(copy));
    {
        const uint16_t xy = mem<uint16_t>(base + 0x32u), yy = mem<uint16_t>(base + 0x34u), zy = mem<uint16_t>(base + 0x36u);
        mem<uint16_t>(base + 0x32u) = static_cast<uint16_t>(-xy);
        mem<uint16_t>(base + 0x34u) = static_cast<uint16_t>(-yy);
        mem<uint16_t>(base + 0x36u) = static_cast<uint16_t>(-zy);
    }
    // The transpose, through the two pointers (in the order of the original: they may overlap).
    const uint32_t in = view.pPsxViewMatrix.address, out = view.pPsxTViewMatrix.address;
    const auto h = [](uint32_t address) -> uint16_t & { return mem<uint16_t>(address); };
    const auto w = [](uint32_t address) -> uint32_t & { return mem<uint32_t>(address); };
    const uint16_t xx = h(in + 0x0u);
    const uint32_t transX = w(in + 0x14u);
    h(out + 0x0u) = xx;
    w(out + 0x14u) = transX;
    const uint16_t yy = h(in + 0x8u);
    const uint32_t transY = w(in + 0x18u);
    h(out + 0x8u) = yy;
    w(out + 0x18u) = transY;
    const uint16_t zz = h(in + 0x10u);
    const uint32_t transZ = w(in + 0x1cu);
    h(out + 0x10u) = zz;
    h(out + 0x6u) = h(in + 0x2u);
    h(out + 0xcu) = h(in + 0x4u);
    h(out + 0xeu) = h(in + 0xau);
    h(out + 0x2u) = h(in + 0x6u);
    h(out + 0x4u) = h(in + 0xcu);
    h(out + 0xau) = h(in + 0xeu);
    w(out + 0x1cu) = transZ;
}

void View3dInitProjectionConstants()
{
    view3dClass &view = G(view3dVarG);
    const uint32_t window = G(init3dVarG).window3dDefinitionPtr.address;
    const uint16_t width = mem<uint16_t>(window + 0x40u);
    view.dUX = static_cast<int16>(width);
    const uint16_t height = mem<uint16_t>(window + 0x44u);
    view.dUXS = static_cast<int16>(static_cast<int16_t>(width) >> 1);
    view.dUY = static_cast<int16>(height);
    view.dUYS = static_cast<int16>(static_cast<int16_t>(height) >> 1);
}

// As DisplayDefineDeviceWindow (the last five arguments on the stack), then marks the definition valid.
int32_t View3dDefine3dDeviceWindow(ptr32<window3dDefinition> windowDefPtr, int32_t winTopX, int32_t winTopY,
                                   int32_t winBotX, int32_t winBotY, int32_t deviceWindowOriginX,
                                   int32_t deviceWindowOriginY, int32_t drawWinTopX)
{
    uint32_t rest[5];
    for (uint32_t i = 0; i < 5; ++i)
        rest[i] = stack_arg<uint32_t>(i);
    {
        GuestFrame frame(0x50); // The original passes them on at the bottom of its own frame.
        for (uint32_t i = 0; i < 5; ++i)
            *frame.local<uint32_t>(8u * i) = rest[i];
        DisplayDefineDeviceWindow(windowDefPtr.cast<_sDisplayDeviceWindow>(), winTopX, winTopY, winBotX, winBotY,
                                  deviceWindowOriginX, deviceWindowOriginY, drawWinTopX);
    }
    windowDefPtr->validID = kValidWindow;
    return 1;
}

// INIT3D.c

void Init3dWindows(ptr32<window3dDefinition> windowDefPtr, int32 flags)
{
    DisplayInit3dWindows(windowDefPtr.cast<_sDisplayDeviceWindow>(), static_cast<uint32>(flags));
}

void Init3dChangeViewcone(uint16 dValue)
{
    if (dValue == G(init3dVarG).currentD)
        return;
    G(init3dVarG).currentD = dValue;
    View3dInitViewconeReject();
}

// Sets up the 3D layer: the depth ranges, the 3D heap, then each part of the renderer.
bool_ Init3d(uint32 heapSize, uint32 d, int32 zMin, int32 zMax, int32 dZMin, int32 dZMax, uint32 bufferSize)
{
    init3dClass &v = G(init3dVarG);
    v.currentD = static_cast<uint16>(d);
    v.vVZMin = zMin;
    v.vVZMax = zMax;
    v.depthSafeMinZ = dZMin;
    v.depthSafeMaxZ = dZMax;
    v.vVZMinGTE = 10;
    const ptr32<_sDisplayDefinition> def = G(displayVarG).state.pDispDef;
    v.depthOrigMinZ = dZMin;
    v.depthOrigMaxZ = dZMax;
    v.frameCount = 0;
    v.heapSize = heapSize;
    v.window3dDefinitionPtr = nullptr;
    v.hardwareX = mem<uint16_t>(def.address + 0u);
    const uint16_t hardwareY = mem<uint16_t>(def.address + 4u);
    v.baseHardwareX = 0x140;
    v.hardwareY = hardwareY;
    v.baseHardwareY = 200;
    v.baseD = 0x100;
    const uint16_t modulo = mem<uint16_t>(def.address + 8u);
    v.yRatio = 1;
    v.modulo = modulo;
    v.xRatio = 1;
    if (!HeapInit(heapSize))
        return 0;
    const int32 vMin = v.vVZMin, vMax = v.vVZMax;
    v.origVVZMin = vMin;
    v.origVVZMax = vMax;
    v.winStore = nullptr;
    v.subShapeFlag = 0;
    View3dInit();
    RenderInit(bufferSize);
    TexInit();
    FontInit();
    SvfInit();
    RendObjInit();
    SpriteInit();
    LsInit();
    CellInit();
    MeshInit();
    DepthCueInit();
    VISInit();
    return 1;
}

// Starts a 3D frame in a window: a valid window's view matrices, and its projection constants when it changes.
bool_ Init3dFrame(ptr32<window3dDefinition> windowDefPtr, ptr32<_matPsx> viewMPtr, uint32 time, int32 flags)
{
    init3dClass &v = G(init3dVarG);
    v.time3d = time;
    v.frameCount = v.frameCount + 1u;
    HeapReset();
    if (windowDefPtr->validID != kValidWindow)
        return 0;
    v.window3dDefinitionPtr = windowDefPtr;
    View3dFrameInit(windowDefPtr, viewMPtr);
    if (v.winStore.address != windowDefPtr.address)
    {
        View3dInitProjectionConstants();
        View3dInitViewconeReject();
        v.winStore = windowDefPtr.cast<void>();
    }
    RenderFrameInit(flags);
    return 1;
}

bool_ Init3dFrame2(ptr32<window3dDefinition> windowDefPtr, ptr32<_matPsx> viewMPtr, uint32, int32)
{
    if (windowDefPtr->validID != kValidWindow)
        return 0;
    View3dFrameInit(windowDefPtr, viewMPtr);
    G(init3dVarG).window3dDefinitionPtr = windowDefPtr;
    return 1;
}

void Init3dClean()
{
    HeapTerm();
    RenderTerm();
    G(init3dVarG).winStore = nullptr;
}

} // namespace rc

RC_DECOMP_FN_BLOCKING_SETUP(DisplaySetMode, rc::fuzz::displayMode)
RC_DECOMP_FN_BLOCKING_SETUP(DisplayInit, rc::fuzz::displayMode)
RC_DECOMP_FN(DisplayTerm)
RC_DECOMP_FN(DisplayEnable)
RC_DECOMP_FN(DisplayDisable)
RC_DECOMP_FN_SETUP(DisplaySwapScreens, rc::fuzz::displaySwap)
RC_DECOMP_FN(DisplaySetBackground)
RC_DECOMP_FN(DisplayClearBackground)
RC_DECOMP_FN(DisplayRenderFrame)
RC_DECOMP_FN(DisplayRenderFrameProfile)
RC_DECOMP_FN(DisplayRenderFrameProfileNoDraw)
RC_DECOMP_FN_SETUP(DisplayDefineDeviceWindow, rc::fuzz::displayWindow)
RC_DECOMP_FN_SETUP(DisplayRegisterDeviceWindow, rc::fuzz::displayRegister)
RC_DECOMP_FN(DisplayInit3dWindows)
RC_DECOMP_FN(DisplaySetHardwareXY)
RC_DECOMP_FN(DisplayInitHiRes)
RC_DECOMP_FN(DisplayTermHiRes)
RC_DECOMP_FN(CellInit)
RC_DECOMP_FN_SETUP(CellRenderInit, rc::fuzz::cellTrans)
RC_DECOMP_FN(CellRender)
RC_DECOMP_FN(CellRenderEnd)
RC_DECOMP_FN(CellRenderPolygonsFast)
RC_DECOMP_FN(CellRenderPolygonsFastEnd)
RC_DECOMP_FN(CellRenderPolygons)
RC_DECOMP_FN(CellRenderPolygonsEnd)
RC_DECOMP_FN_SETUP(View3dInitViewconeReject, rc::fuzz::viewVars)
RC_DECOMP_FN_SETUP(View3dInit, rc::fuzz::viewVars)
RC_DECOMP_FN_SETUP(View3dFrameInit, rc::fuzz::viewFrame)
RC_DECOMP_FN_SETUP(View3dInitProjectionConstants, rc::fuzz::viewVars)
RC_DECOMP_FN_SETUP(View3dDefine3dDeviceWindow, rc::fuzz::displayWindow)
RC_DECOMP_FN(Init3dWindows)
RC_DECOMP_FN_SETUP(Init3dChangeViewcone, rc::fuzz::viewcone)
RC_DECOMP_FN_BLOCKING_SETUP_HEAP(Init3d, rc::fuzz::init3d)
RC_DECOMP_FN_SETUP(Init3dFrame, rc::fuzz::viewFrame)
RC_DECOMP_FN_SETUP(Init3dFrame2, rc::fuzz::viewFrame)
RC_DECOMP_FN_SETUP_HEAP(Init3dClean, rc::fuzz::init3dClean)

// Decompiled from several small units: DISPLAY2, G0, GRAB, MTL, SPLINE, VERSION, MIRROR, CARLIST, CARDEF, DEPTHCUE,
// GFX, LOADERR, MESH, NODE, RCMENU, RENDOBJ, TOB, VIS, LSGEN and WHEEL. Most are set-up and tear-down functions that
// register a resource type's construct daemon or store a constant; the rest are small lookups.

#include "ee_math.h"

#include <cstring>
#include "../rc_frame_tick.h"
#include "../rc_split.h"
#include "rc_decomp.h"

// Europe variants (NOTES.md "Europe version"): LsFrameInit (the split-screen offset 64 lines instead of 56).
RC_VERSION_VARIANT(LsFrameInit)
// Europe: CarListConstructDaemon and RendObjRender store in another order (the same body).
RC_VERSION_VARIANT(CarListConstructDaemon)
RC_VERSION_VARIANT(RendObjRender)

namespace {
constexpr rc::ee_float operator""_f(long double v) { return rc::ee_float(static_cast<float>(v)); }
} // namespace

namespace rc {

void ResTypeSetConstructDaemon(ptr32<char> pType, uint32 constructDaemon, int16 options); // res.cpp
void ResTypeUnSetConstructDaemon(ptr32<char> pType);
uint16 Sqr32(uint32 n);                                                                    // math_core.cpp
void GfxP2_DefRenderState();                                                               // rdr.cpp
void Rdr_UseCamera(ptr32<_sRdrCamera> pCam);
void PS2C_GteMatrixMulVec16(ptr32<_sVec16> pVec); // psxdefs.cpp
void PS2C_SetGteMatrix(ptr32<_sMat> pMat);
void PS2C_GetVec16GteResult(ptr32<_sVec16> pOut);
uint32 FontLength(ptr32<char> pString);   // font.cpp
void FontSetColour(uint8 red, uint8 green, uint8 blue);
void FontCentre(int16 y, ptr32<char> pString);

namespace {

const uint32_t kResSetReadErrorFn = sym::ResSetReadErrorFn, kRdrSetupCamera = sym::Rdr_SetupCamera, kRdrVUShape = sym::Rdr_VUShape,
               kTRdrVUShape = sym::TRdr_VUShape, kRdrFixVUShape = sym::Rdr_FixVUShape, kGfxP2Init = sym::GfxP2_Init, kGfxP2Term = sym::GfxP2_Term,
               kSprintf = sym::sprintf, kResLoad = sym::ResLoad, kLoadErrorCallback = sym::LoadErrorCallback;

ee_float constantAt(uint32_t address) { return *static_cast<ee_float *>(guest_host(address)); }

// MIPS remainder, as the recompiler gives it: no trap for a zero divisor (the break is passed on).
int32_t ee_rem(int32_t a, int32_t b)
{
    if (b == 0)
        return a;
    if (a == INT32_MIN && b == -1)
        return 0;
    return a % b;
}

} // namespace

// DISPLAY2.c: the vertical blank interrupt's callback counts frames.
void VSyncCallbackFunction()
{
    if (frametick::videoFieldElapsed()) // enhancement: video-mode fields, however fast the vblank runs (rc_frame_tick.h)
        G(displayVarG).uiVSyncInc = G(displayVarG).uiVSyncInc + 1;
}

// G0.c, GRAB.c, MESH.c, TOB.c, VIS.c, CARDEF.c, RECT.c: functions that do nothing.
int32_t G0Init() { return 0; }
void GrabBlock(int32 iXPos, int32 iYPos, int32 iW, int32 iH, int32 quad) {}
void MeshInit() {}
void MeshInitFrame() {}
void MeshSetQuality(eMeshQuality quality) {}
void TOBInit() {}
void TOBTerm() {}
int16 TOBRender(ptr32<_sTOBInst> pTOBInst) { return 0; }
void VISTerm() {}
void VISUpdate(ptr32<vec32_> pPos) {}
void CarDefCDFConstructDaemon(memHandle pHandle) {}
void CarDefLvlInit() {}
void CarDefLvlTerm() {}
void Gfx_ConstructTex(ptr32<_sGfxTexture> pGfxTexture) {}
void LoadErrorCallback() {}

void VISInit()
{
    G(visVarG).visMask0 = G(visVarG).visMask0 | 1u;
}

// GFX.c: both just jump to GFX_P2's.
void Gfx_Init(s32 wd, s32 ht) { guest_call(kGfxP2Init, wd, ht); }
void Gfx_Term() { guest_call(kGfxP2Term); }

// MTL.c: the surface types' skid and dust colours, wheel marks, damping and grip.
void MtlInit()
{
    struct Surface
    {
        int32_t wheelMark, skid, dust, damping, grip;
    };
    for (uint32_t i = 0; i < 0x47; ++i)
    {
        Surface s{0, 0x403030, 0x304040, 0, 6000}; // The default.
        switch (i)
        {
        case 1: case 4: case 9: s = {0, 0x504040, 0x404040, 0, 6000}; break;
        case 2: s = {0, 0x504040, 0x304040, 0, 6000}; break;
        case 3: s = {0, 0x402040, 0x2500, 0, 6000}; break;
        case 5: s = {0, 0x2030, 0x404040, 0, 0x2ee}; break;
        case 6: s = {8, 0xf08040, 0x404040, 0, 6000}; break;
        case 7: case 0xc: case 0x18: s = {0, 0x404040, 0x404040, 0, 6000}; break;
        case 8: s = {0, 0x403020, 0x2040, 0, 6000}; break;
        case 10: s = {2, 0x2040, 0x303030, 10, 2000}; break;
        case 0xb: s = {8, 0x402020, 0x104040, 0, 6000}; break;
        case 0xd: s = {8, 0x403020, 0x103040, 0, 6000}; break;
        case 0xe: s = {0, 0xe0e0e0, 0x200000, 0, 2000}; break;
        case 0xf: s = {0, 0x808080, 0x384040, 0, 6000}; break;
        case 0x10: s = {0, 0x802020, 0x404040, 0, 6000}; break;
        case 0x12: case 0x13: case 0x45: s = {2, 0x2040, 0x303030, 0, 6000}; break;
        case 0x14: s = {8, 0x4080, 0x40, 0, 6000}; break;
        case 0x20: case 0x46: s = {0, 0, 0, 0, 6000}; break;
        case 0x23: s = {0, 0x505050, 0x404040, 0, 500}; break;
        case 0x11: case 0x15: case 0x16: case 0x17:
        case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d: case 0x1e: case 0x1f:
        case 0x21: case 0x22:
            s = {0, 0x403028, 0x303030, 0, 6000};
            break;
        default:
            if (i >= 0x24 && i <= 0x44)
                s = {0, 0x403028, 0x303030, 0, 6000};
            break;
        }
        sMtl &mtl = G(mtlG).mtl[i];
        mtl.iWheelMark = s.wheelMark;
        mtl.iSkidRGB = s.skid;
        mtl.iDustRGB = s.dust;
        mtl.damping = s.damping;
        mtl.grip = s.grip;
        mtl.type = static_cast<uint8>(i);
    }
}

// SPLINE.c: the point at u (0 to 1) on the Bezier-like segment `curve` of a spline, whose next control point is
// `dir` segments on. A segment is 0x24 bytes after the 8-byte header, the first word of which is the segment count.
ptr32<sVec3> returnPoint(ptr32<sVec3> out, ptr32<void> spl, int32 curve, int32 dir, ee_float u)
{
    const uint32_t base = spl.address;
    const int32_t count = *static_cast<int32_t *>(guest_host(base));
    const auto floatsAt = [&](int32_t segment) { return static_cast<ee_float *>(guest_host(base + static_cast<uint32_t>(segment) * 0x24u + 8u)); };
    const ee_float *p2 = floatsAt(curve);
    const ee_float *p1 = floatsAt(ee_rem(curve + dir, count));
    // One coordinate: p0 and the tangent t0 of this segment, the tangent t1 and the position q of the next. The
    // machine code adds the two constant terms (-3 p0 + 3 t0, and 3 p0 - 6 t0 + 3 t1) before using them.
    const auto coordinate = [&](ee_float p0, ee_float t0, ee_float t1, ee_float q)
    {
        const ee_float threeT0 = t0 * 3.0_f;
        const ee_float quadratic = p0 * -3.0_f + threeT0;
        const ee_float threeT1 = t1 * 3.0_f;
        const ee_float linear = (p0 * 3.0_f - t0 * 6.0_f) + threeT1;
        const ee_float cubic = ((threeT0 - p0) - threeT1) + q;
        return ((cubic * u + linear) * u + quadratic) * u + p0;
    };
    sVec3 &result = *out;
    result.x = coordinate(p2[0], p2[6], p1[3], p1[0]);
    result.z = coordinate(p2[1], p2[7], p1[4], p1[1]);
    result.y = coordinate(p2[2], p2[8], p1[5], p1[2]);
    return out;
}

// VERSION.c: the version number in the middle of the screen, in grey.
void VersionDisplay()
{
    GuestFrame frame(0x30);
    const ptr32<char> buffer = frame.local<char>(0);
    FontSetColour(0x7f, 0x7f, 0x7f);
    guest_call(kSprintf, buffer, ptr32<char>((sym::sec_rodata + 0x1de0u)), 0, 0xd, 0x52, ptr32<char>((sym::sec_sdata + 0x4c0u)));
    FontCentre(static_cast<int16>(0xff9c), buffer);
}

// MIRROR.c: mirror mode flips the track left to right, and the anti-crack value tells the renderer which way
// the polygons now face.
int32_t Mirror()
{
    sMirrorClass &mirror = G(mirrorVarG);
    const int32_t flipped = mirror.bMirrored ^ 1;
    mirror.antiCrack = flipped != 0 ? 1 : 0;
    mirror.bMirrored = flipped;
    return mirror.bMirrored;
}

void MirrorSet(bool_ bMirrored)
{
    if (bMirrored != G(mirrorVarG).bMirrored)
        Mirror();
    G(mirrorVarG).antiCrack = bMirrored != 0 ? -1 : 1;
}

// DEPTHCUE.c: how far the world is drawn. The cell depth cue is that less the half-diagonal of a cell.
void DepthCueInit()
{
    sDepthCueClass &cue = G(depthCueVarG);
    cue.depthCue = 25000;
    cue.cellDepthCue = 0x5bc6;
    cue.lastUpdateTime = 0;
}

void DepthCueSet(uint16 depthCue, uint8 splitScreen)
{
    uint32_t depth = 0x1800;
    if (splitScreen == 0)
        depth = depthCue;
    const int32_t half = static_cast<int16_t>(G(lsVarG).pParent->cellSize) >> 1;
    G(depthCueVarG).depthCue = static_cast<uint16>(depth);
    const uint32_t diagonal = Sqr32(static_cast<uint32_t>(half * half * 4));
    G(init3dVarG).origVVZMax = static_cast<int32>(depth);
    G(init3dVarG).vVZMax = static_cast<int32>(depth);
    G(depthCueVarG).cellDepthCue = static_cast<uint16>(static_cast<int16_t>(depth) - (diagonal + 0x100));
}

void DepthCueTrackEditorSet(uint16 depthCue)
{
    G(depthCueVarG).depthCue = depthCue;
    const uint32_t diagonal = Sqr32(0x800000);
    G(init3dVarG).origVVZMax = depthCue;
    G(init3dVarG).vVZMax = depthCue;
    G(depthCueVarG).cellDepthCue = static_cast<uint16>(depthCue - (diagonal + 0x100));
}

// LOADERR.c
void LoadErrorInit() { guest_call(kResSetReadErrorFn, kLoadErrorCallback); }
void LoadErrorTerm() { G(resVarG).pfLoadErrorFunction.address = 0; }

// NODE.c: nodes follow their 4-byte header, 0x2c bytes each.
ptr32<_sNode> NodeFindByTag(ptr32<_sNodeHead> pNodeHead, uint32 tag)
{
    const uint32_t first = pNodeHead.address + 4u;
    for (uint32_t i = 0; i < pNodeHead->numNodes; ++i)
    {
        const ptr32<_sNode> node(first + i * 0x2cu);
        if (node->tag == tag)
            return node;
    }
    return nullptr;
}

// The next node after pCurrNode with this tag. A current node past the end leaves a remaining count that wraps in 16
// bits, as the original's does.
ptr32<_sNode> NodeFindNextByTag(ptr32<_sNode> pCurrNode, ptr32<_sNodeHead> pNodeHead, uint32 tag)
{
    const uint32_t first = pNodeHead.address + 4u;
    const uint32_t index = ((pCurrNode.address - first) / 0x2cu + 1u) & 0xffffu; // An unsigned division.
    uint32_t remaining = (static_cast<uint32_t>(pNodeHead->numNodes) - index) & 0xffffu;
    for (uint32_t i = index; remaining != 0; ++i, remaining = (remaining - 1) & 0xffffu)
    {
        const ptr32<_sNode> node(first + i * 0x2cu);
        if (node->tag == tag)
            return node;
    }
    return nullptr;
}

// A node's world position: its position in the parent's frame, turned by the parent's matrix, plus the parent's.
void NodeCalcWorldPos(ptr32<_sNode> pNode, ptr32<_matPsx> pParentMatrix, ptr32<vec32_> pParentPos, ptr32<vec32_> pOutputPos)
{
    uint16_t *pad = static_cast<uint16_t *>(guest_host(sym::scratchPadG)); // scratchPadG
    pad[0] = static_cast<uint16_t>(static_cast<int16_t>(pNode->matrix.trans.x) << 6);
    pad[1] = static_cast<uint16_t>(static_cast<int16_t>(pNode->matrix.trans.y) << 6);
    pad[2] = static_cast<uint16_t>(static_cast<int16_t>(pNode->matrix.trans.z) << 6);
    PS2C_SetGteMatrix(ptr32<_sMat>(pParentMatrix.address));
    PS2C_GteMatrixMulVec16(ptr32<_sVec16>(sym::scratchPadG));
    const int32_t z = pParentPos->z, x = pParentPos->x, y = pParentPos->y;
    PS2C_GetVec16GteResult(ptr32<_sVec16>((sym::scratchPadG + 0x8u)));
    pOutputPos->x = x + static_cast<int16_t>(pad[4]);
    pOutputPos->y = y + static_cast<int16_t>(pad[5]);
    pOutputPos->z = z + static_cast<int16_t>(pad[6]);
}

// RCMENU.c: points each menu variable of the level (or the Track Editor) at the game variable it controls. The
// level data's list ends at a null; a tag it does not have gives a null item, written to as is.
void MVARLinkup()
{
    ptr32<ptr32<sMenuItemVar>> list = G(tEditVarG).pMVARList;
    if (G(pLvlG).address != 0xffffffffu)
        list = G(pLvlG)->pMVARList;
    struct Link
    {
        int32_t tag;
        ptr32<int32> pLinkVar;
    };
    const Link *links = static_cast<const Link *>(guest_host(sym::menuVariableLinksG)); // menuVariableLinksG
    if (links[0].tag == 0)
        return;
    const ptr32<sMenuItemVar> first = list[0];
    for (uint32_t i = 0; links[i].tag != 0; ++i)
    {
        uint32_t index = 0;
        if (first.address != 0 && first->tagMVar != links[i].tag)
        {
            index = 1;
            for (;;)
            {
                if (list[index].address == 0)
                    break;
                if (list[index]->tagMVar == links[i].tag)
                    break;
                ++index;
            }
        }
        const uint32_t item = list[index].address;
        const uint32_t variable = links[i].pLinkVar.address;
        const int32_t value = *static_cast<int32_t *>(guest_host(variable));
        *static_cast<uint32_t *>(guest_host(item + 8u)) = variable;
        *static_cast<int32_t *>(guest_host(item + 0xcu)) = value;
    }
}

void RCMenuMVarLinkup() { MVARLinkup(); }

// The debug menus' level skip and restart.
void ProcessDebugMenus(ptr32<mainClass> pMainVar, ptr32<sMenuSystem> pMenuSystem)
{
    const uint32_t code = G(pMenuSystemG)->executeCode;
    if (code == 0x7fff1001 || code == 0x7fff1000)
    {
        const uint8 level = pMainVar->level;
        pMainVar->status = MAIN_RESTART_LEVEL;
        pMainVar->level = static_cast<uint8>(level + 1);
    }
    else if (code == 0x7fff1002)
        pMainVar->status = MAIN_RESTART_LEVEL;
}

// LSGEN.c: the level's camera for the renderer is the game's PlayStation view matrix, turned to floats.
void LsCameraToRdr(ptr32<_sRdrCamera> pCam)
{
    const _matPsx &view = *G(view3dVarG).pPsxTViewMatrix;
    const ptr32<_matPsx> viewMatrix = G(view3dVarG).pPsxViewMatrix;
    const int32_t z = viewMatrix->trans.z, x = viewMatrix->trans.x, y = viewMatrix->trans.y;
    const ee_float scale = float_from_bits(0x39800000u); // 1/4096
    const auto turn = [&](int16_t value) { return ee_itof(value) * scale; };
    sRdrCamera &cam = *pCam;
    cam.mat.Xx = turn(static_cast<int16_t>(view.mat.Xx));
    cam.mat.Xy = turn(static_cast<int16_t>(view.mat.Xy));
    cam.mat.Xz = turn(static_cast<int16_t>(view.mat.Xz));
    cam.mat.Yx = turn(static_cast<int16_t>(view.mat.Yx));
    cam.mat.Yy = turn(static_cast<int16_t>(view.mat.Yy));
    cam.mat.Yz = turn(static_cast<int16_t>(view.mat.Yz));
    cam.mat.Zx = turn(static_cast<int16_t>(view.mat.Zx));
    cam.mat.Zy = turn(static_cast<int16_t>(view.mat.Zy));
    const int16_t zz = static_cast<int16_t>(view.mat.Zz);
    cam.pos.z = ee_itof(z);
    cam.pos.x = ee_itof(x);
    cam.pos.y = ee_itof(y);
    cam.mat.Zz = turn(zz);
}

void LsInit()
{
    G(lsVarG).fov = 60.0_f;
}

// Draws the level's shape with the VU1 program for its version (2 and 3 are the world and track kinds).
void LsRenderWorld()
{
    const ptr32<lsParent> parent = G(lsVarG).pParent;
    const ee_float version = parent->shape.version;
    const ptr32<sRdrCamera> camera(sym::rdrCameraG);
    if (version == 2.0_f)
        guest_call(kRdrVUShape, ptr32<sRdrVUShape>(parent.address), camera, 0);
    else if (version == 3.0_f)
        guest_call(kTRdrVUShape, ptr32<sRdrVUShape>(parent.address), camera, 0);
}

// Sets up the camera for a frame (the field of view in the game's 4096-per-turn angle, shifted up or down for
// split screen), makes it the renderer's, resets the render state and clears the last texture.
void LsFrameInit(tTime time, uint8 splitScreen, int32_t whichScreen)
{
    GuestFrame frame(0x50);
    sRdrCamera &camera = G(rdrCameraG);
    const ptr32<sRdrCamera> cameraAddress(sym::rdrCameraG);
    guest_call(kRdrSetupCamera, cameraAddress, static_cast<int32_t>(ee_ftoi(G(lsVarG).fov * constantAt(sym::lit4_32792c))));
    camera.vertDrawOffset = 0.0_f;
    if (splitScreen != 0 && !split::sideBySide()) // vertical split screen (rc_split.h): no vertical offset
        camera.vertDrawOffset = whichScreen == 0 ? (RC_VERSION_EU ? -64.0_f : -56.0_f) : (RC_VERSION_EU ? 64.0_f : 56.0_f);
    LsCameraToRdr(cameraAddress);
    Rdr_UseCamera(cameraAddress);
    GfxP2_DefRenderState();
    sGfxP2 &gfx = G(gGfxP2);
    uint64_t *quad = reinterpret_cast<uint64_t *>((gfx.pDmaBuff + gfx.buffIdx).get());
    gfx.buffIdx = gfx.buffIdx + 2;
    quad[0] = 0x1000000000000001ull;
    gfx.gifTagIdx = -1;
    quad[2] = 0x1b;
    G(gLastTex) = ptr32<_sGfxTexture>(0xffffffffu);
    quad[1] = 0;
}

// CARDEF.c, WHEEL.c, RENDOBJ.c, CARLIST.c: resource types' construct daemons.
void CarDefInit() { ResTypeSetConstructDaemon(ptr32<char>((sym::sec_sdata + 0x120u)), sym::CarDefCDFConstructDaemon, 2); }
void CarDefTerm() { ResTypeUnSetConstructDaemon(ptr32<char>((sym::sec_sdata + 0x120u))); }
void CarListTerm() { ResTypeUnSetConstructDaemon(ptr32<char>((sym::sec_sdata + 0x130u))); }

void CarListInit()
{
    std::memset(guest_host(sym::carListVarG), 0, 0x1e0); // The 20 cars and 20 boats; the shared flag stays.
    ResTypeSetConstructDaemon(ptr32<char>((sym::sec_sdata + 0x130u)), sym::CarListConstructDaemon, 1);
    G(carListVarG).altRendObjResourceBoat = nullptr;
    G(carListVarG).altRendObjResource = nullptr;
}

void WheelInit()
{
    ResTypeSetConstructDaemon(ptr32<char>((sym::sec_sdata + 0x530u)), sym::WheelConstructDaemon, 1);
    ResTypeSetConstructDaemon(ptr32<char>((sym::sec_sdata + 0x538u)), sym::ROBLConstructDaemon, 1);
}

void WheelTerm()
{
    ResTypeUnSetConstructDaemon(ptr32<char>((sym::sec_sdata + 0x530u)));
    ResTypeUnSetConstructDaemon(ptr32<char>((sym::sec_sdata + 0x538u)));
}

void RendObjInit()
{
    ResTypeSetConstructDaemon(ptr32<char>((sym::sec_sdata + 0x5a0u)), sym::RendObjConstructDaemon, 6);
    ResTypeSetConstructDaemon(ptr32<char>((sym::sec_sdata + 0x5a8u)), sym::RendObjConstructDaemon, 6);
}

// Loads each car and boat model the race needs, once per kind of car; a car or boat that appears twice shares the
// first one's model but is told (altRendObj) to draw with its own paint. Levels 5, 0, 8, 9 and 11 (mod 12) have boats.
void CarListConstructDaemon(ptr32<memHandle> pHandle)
{
    GuestFrame frame(0xa0);
    sCarListClass &list = G(carListVarG);
    const uint32_t resource = pHandle->address;
    const uint32_t total = static_cast<uint32_t>(G(mainVarG).noHuman) + G(mainVarG).noAI;
    list.altRendObjResource = nullptr;
    list.altRendObjResourceBoat = nullptr;
    const auto modelOf = [&](int32_t type) { return *static_cast<uint32_t *>(guest_host(resource + static_cast<uint32_t>(type) * 4u)); };
    for (uint32_t i = 0; i < total; ++i)
    {
        const int32_t type = list.car[i].type;
        list.boat[i].type = static_cast<eObjectType>(type == OBJECT_TYPE_NONE ? OBJECT_TYPE_NONE : type + OBJECT_TYPE_ROCKETCAR);
        list.car[i].altRendObj = 0;
        list.boat[i].altRendObj = 0;
    }
    for (uint32_t i = 0; i < total; ++i)
    {
        const int32_t type = list.car[i].type;
        if (type != OBJECT_TYPE_NONE)
        {
            list.alternativeRendObj = 0;
            for (uint32_t j = i + 1; j < total; ++j)
                if (type == list.car[j].type)
                {
                    list.alternativeRendObj = 1;
                    list.car[j].altRendObj = 1;
                }
            const uint32_t level = G(mainVarG).level % 12u;
            if (level == 5 || level == 0 || level == 8)
                G(objectConstructorNoLoadROBs) = 1;
            list.car[i].pDef = ptr32<_sObjectDef>(guest_call<uint32_t>(kResLoad, modelOf(type)));
            G(objectConstructorNoLoadROBs) = 0;
        }
        const uint32_t level = G(mainVarG).level % 12u;
        if ((level == 5 || level == 8 || level == 0 || level == 9 || level == 0xb) && G(mainVarG).level != 'a')
        {
            const int32_t boat = list.boat[i].type;
            if (boat != OBJECT_TYPE_NONE)
            {
                list.alternativeRendObj = 0;
                for (uint32_t j = i + 1; j < total; ++j)
                    if (boat == list.boat[j].type)
                    {
                        list.alternativeRendObj = 2;
                        list.car[j].altRendObj = 1;
                        list.boat[j].altRendObj = 1;
                    }
                list.boat[i].pDef = ptr32<_sObjectDef>(guest_call<uint32_t>(kResLoad, modelOf(boat)));
            }
        }
        else
            list.boat[i].pDef = ptr32<_sObjectDef>(0xffffffffu);
    }
}

// The wheels' resource lists every wheel model by id, ending in zero; the loader gives each its loaded address.
void WheelConstructDaemon(ptr32<memHandle> pHandle)
{
    GuestFrame frame(0x40);
    const ptr32<uint32> list = ptr32<uint32>(pHandle->address);
    for (uint32_t i = 0; list[i] != 0; ++i)
        list[i] = guest_call<uint32_t>(kResLoad, list[i]);
}

// The ROB list's first word is the count, then that many ids.
void ROBLConstructDaemon(ptr32<memHandle> pHandle)
{
    GuestFrame frame(0x50);
    const ptr32<uint32> list = ptr32<uint32>(pHandle->address);
    uint32_t index = 0;
    if (list[0] != 0)
    {
        do
        {
            ++index;
            const uint32_t loaded = guest_call<uint32_t>(kResLoad, list[index]);
            list[index] = loaded;
        } while (index < list[0]);
    }
}

// Every shape that comes from a RendObj resource of version 2 or 3 needs its pointers fixed up.
void RendObjConstructDaemon(ptr32<memHandle> pHandle)
{
    const ptr32<sRdrVUShape> shape(pHandle->address);
    const ee_float version = shape->version;
    if (version != 2.0_f && version != 3.0_f)
        return;
    guest_call(kRdrFixVUShape, shape);
}


namespace {

// The GS's size code for a texture side in pixels (log2 for a power of two from 1 to 1024), 0 for anything else.
int64_t sizeCode(int32_t pixels)
{
    switch (pixels)
    {
    case 1: return 0;
    case 2: return 1;
    case 4: return 2;
    case 8: return 3;
    case 0x10: return 4;
    case 0x20: return 5;
    case 0x40: return 6;
    case 0x80: return 7;
    case 0x100: return 8;
    case 0x200: return 9;
    case 0x400: return 10;
    default: return 0;
    }
}

// The TEX0 value for the environment map (a texture drawn on shiny surfaces): its address, buffer width, the sizes,
// and its palette if it has one.
uint64_t envMapTex0(const _sGfxTexture &texture)
{
    const int64_t width = sizeCode(texture.wd), height = sizeCode(texture.ht);
    const int32_t wd = texture.wd;
    const int32_t blocks = (wd < 0 ? wd + 0x3f : wd) >> 6;
    const int64_t bufferWidth = 0 < blocks ? blocks : 1;
    const uint64_t address = static_cast<uint64_t>(static_cast<int64_t>(texture.idxList[0]));
    const uint64_t format = static_cast<uint64_t>(static_cast<int64_t>(texture.fmt)) << 0x14;
    if (static_cast<uint32_t>(texture.fmt) - 0x13u < 2u) // 8 and 4-bit palette textures
        return address | 0x2000000000000000ull | static_cast<uint64_t>(width << 0x1a) | 0x400000000ull |
               static_cast<uint64_t>(bufferWidth << 0xe) | format | static_cast<uint64_t>(height << 0x1e) |
               (static_cast<uint64_t>(static_cast<int64_t>(texture.clutVramAddr)) << 0x25);
    return address | 0x400000000ull | static_cast<uint64_t>(width << 0x1a) | format | static_cast<uint64_t>(bufferWidth << 0xe) |
           static_cast<uint64_t>(height << 0x1e);
}

} // namespace

// Draws a car part (or other rendering object): gives its shape the object's position and turn, drops it if it is
// behind the camera, outside the view or too far, and otherwise draws it with the VU1 program for its kind. A
// shiny (environment mapped) part is told how far it is from the camera so its reflection can fade out. Returns the
// distance from the camera plane, or 0x7fff if the object is out of sight.
int16 RendObjRender(ptr32<_sRendObj> pRendObj, ptr32<SVECTOR> pPOI__unused, ptr32<SVECTOR> pNormal__unused, int32_t doTint)
{
    GuestFrame frame(0x30);
    _sRendObj &object = *pRendObj;
    const ptr32<_sRendObjHead> headAddress = object.pRendObjHead;
    _sRendObjHead &head = *headAddress;
    sRdrVUShape &shape = head.shape;
    const sRdrCamera &camera = G(rdrCameraG);
    const ptr32<sRdrCamera> cameraAddress(sym::rdrCameraG);
    ee_float version = shape.version;
    if (version != 2.0_f && version != 3.0_f)
        return 0;
    const int32_t py = object.pos.y;
    const uint16_t flags = object.flags;
    const int32_t pz = object.pos.z;
    object.flags = static_cast<uint16>(flags | 1);
    shape.pos.x = ee_itof(object.pos.x) * 0.00390625_f;
    shape.pos.y = ee_itof(py) * 0.00390625_f;
    shape.pos.z = ee_itof(pz) * 0.00390625_f;
    const ee_float dx = shape.pos.x - camera.pos.x;
    ee_float dy = shape.pos.y - camera.pos.y;
    const ee_float dz = shape.pos.z - camera.pos.z;
    const ee_float radius = ee_itof(static_cast<int32_t>(head.radius));
    const ee_float depth = dx * camera.mat.Zx + dy * camera.mat.Zy + dz * camera.mat.Zz;
    ee_float side = dx * camera.mat.Xx + dy * camera.mat.Xy + dz * camera.mat.Xz;
    dy = dx * camera.mat.Yx + dy * camera.mat.Yy + dz * camera.mat.Yz;
    if (depth < -radius)
    {
        object.flags = static_cast<uint16>(flags & 0xfffe);
        return 0x7fff;
    }
    if (0.0_f < side)
        side = -side;
    if (0.0_f < dy)
        dy = -dy;
    const auto outOfSight = [&]
    {
        object.flags = static_cast<uint16>(object.flags & 0xfffe);
        return static_cast<int16>(0x7fff);
    };
    if (radius < side * camera.normLeft.x + dy * camera.normLeft.y + depth * camera.normLeft.z)
        return outOfSight();
    if (radius < side * camera.normBottom.x + dy * camera.normBottom.y + depth * camera.normBottom.z)
        return outOfSight();
    if (constantAt(sym::distanceThreshold_438) * constantAt(sym::lit4_32799c) * radius * camera.normDVal < depth)
        return outOfSight();
    const ee_float scale = float_from_bits(0x39800000u); // 1/4096
    const auto turn = [&](int16_t value) { return ee_itof(value) * scale; };
    shape.mat.Xx = turn(static_cast<int16_t>(object.matrix.mat.Xx));
    shape.mat.Xy = turn(static_cast<int16_t>(object.matrix.mat.Xy));
    shape.mat.Xz = turn(static_cast<int16_t>(object.matrix.mat.Xz));
    shape.mat.Yx = turn(static_cast<int16_t>(object.matrix.mat.Yx));
    shape.mat.Yy = turn(static_cast<int16_t>(object.matrix.mat.Yy));
    shape.mat.Yz = turn(static_cast<int16_t>(object.matrix.mat.Yz));
    shape.mat.Zx = turn(static_cast<int16_t>(object.matrix.mat.Zx));
    shape.mat.Zy = turn(static_cast<int16_t>(object.matrix.mat.Zy));
    shape.mat.Zz = turn(static_cast<int16_t>(object.matrix.mat.Zz));
    uint32_t tint = 0;
    if (doTint != 0)
        tint = static_cast<uint32_t>(head.r) | (static_cast<uint32_t>(head.invBrightness) << 0x18) | (static_cast<uint32_t>(head.g) << 0x10) |
               (static_cast<uint32_t>(head.b) << 8);
    version = shape.version;
    const ptr32<sRdrVUShape> shapeAddress(headAddress.address);
    if (version == 2.0_f)
    {
        guest_call(kRdrVUShape, shapeAddress, cameraAddress, tint);
        return static_cast<int16>(ee_ftoi(depth));
    }
    if (version != 3.0_f)
        return static_cast<int16>(ee_ftoi(depth));
    const uint32_t partFlags = head.flags;
    if ((partFlags & 0x20) != 0)
    {
        const ptr32<_sGfxTexture> envMap = head.envMap;
        if (doTint == 5)
        {
            G(maxEnvMapDistG) = G(maxEnvMapDistG) * constantAt(sym::lit4_3279a0);
            G(fadeEnvMapDistG) = G(fadeEnvMapDistG) * constantAt(sym::lit4_3279a0);
        }
        if ((partFlags & 0x8000) == 0)
        {
            if ((partFlags & 0x4000) != 0 || G(maxEnvMapDistG) < depth)
                G(gEnvFirstPassOnly) = 1;
            else if (G(maxEnvMapDistG) < side)
                G(gEnvFirstPassOnly) = 1;
            else if (G(maxEnvMapDistG) < dy)
                G(gEnvFirstPassOnly) = 1;
            else if (G(fadeEnvMapDistG) < depth)
                G(gEnvRamp) = ((G(maxEnvMapDistG) - G(fadeEnvMapDistG)) - (depth - G(fadeEnvMapDistG))) / (G(maxEnvMapDistG) - G(fadeEnvMapDistG));
        }
        else
        {
            G(gEnvShield) = 1;
            if ((partFlags & 0x2000) != 0)
                G(gEnvRamp) = ee_itof(ee_rem(static_cast<int32_t>(G(gRdr).frameNumber), 0xd)) * constantAt(sym::lit4_3279a4);
        }
        if (doTint == 5)
        {
            G(maxEnvMapDistG) = G(maxEnvMapDistG) * 6.25_f;
            G(fadeEnvMapDistG) = G(fadeEnvMapDistG) * 6.25_f;
        }
        G(gEnvMap) = envMap.address == 0 ? 0 : envMapTex0(*envMap);
    }
    if (G(gEnvShield) == 0 && doTint == 5)
    {
        const ee_float ramp = G(gEnvRamp) + constantAt(sym::lit4_3279a8);
        G(gEnvRamp) = ramp < 1.0_f ? ramp : ee_float(1.0_f);
    }
    guest_call(kTRdrVUShape, shapeAddress, cameraAddress, tint);
    G(gEnvMap) = 0;
    G(gEnvRamp) = 1.0_f;
    G(gEnvShield) = 0;
    G(gEnvFirstPassOnly) = 0;
    return static_cast<int16>(ee_ftoi(depth));
}

} // namespace rc

namespace rc::fuzz {

namespace {

void setArg(R5900Context *ctx, int index, uint32_t value) { SET_GPR_S32(ctx, 4 + index, static_cast<int32_t>(value)); }

float randomFloat(uint64_t &state, float spread)
{
    return static_cast<float>(static_cast<int32_t>(nextFuzz(state))) / 2147483648.0f * spread;
}

} // namespace

// For functions that take no arguments and only work on the game's own state.
void noArgs(R5900Context *, uint64_t &, uint32_t) {}

// A spline of 1 to 6 segments (a count, then 0x24 bytes each from +8), a segment of it and the one `dir` on.
void splineArgs(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t base = scratch + 0x100;
    const uint32_t count = 1 + nextFuzz(state) % 6u;
    *static_cast<uint32_t *>(guest_host(base)) = count;
    for (uint32_t i = 0; i < count * 9u; ++i)
        *static_cast<float *>(guest_host(base + 8u + i * 4u)) = randomFloat(state, 2000.0f);
    setArg(ctx, 0, scratch);
    setArg(ctx, 1, base);
    const uint32_t curve = nextFuzz(state) % count;
    setArg(ctx, 2, curve);
    setArg(ctx, 3, nextFuzz(state) % 3u);
    ctx->f[12] = static_cast<float>(nextFuzz(state) >> 8) / 16777216.0f;
}

// A node list of up to 30 nodes whose tags come from 1 to 5.
uint32_t nodeList(uint64_t &state, uint32_t scratch)
{
    const uint32_t head = scratch + 0x100;
    const uint32_t count = nextFuzz(state) % 31u;
    *static_cast<uint16_t *>(guest_host(head + 2)) = static_cast<uint16_t>(count);
    for (uint32_t i = 0; i < count; ++i)
        *static_cast<uint32_t *>(guest_host(head + 4u + i * 0x2cu)) = 1 + nextFuzz(state) % 5u;
    return head;
}

void nodeFindArgs(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    setArg(ctx, 0, nodeList(state, scratch));
    setArg(ctx, 1, nextFuzz(state) % 7u);
}

void nodeFindNextArgs(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t head = nodeList(state, scratch);
    const uint32_t count = *static_cast<uint16_t *>(guest_host(head + 2));
    const int32_t current = static_cast<int32_t>(nextFuzz(state) % (count + 1u)) - 1; // -1 (before the first) to count - 1
    setArg(ctx, 0, head + 4u + static_cast<uint32_t>(current) * 0x2cu);
    setArg(ctx, 1, head);
    setArg(ctx, 2, nextFuzz(state) % 7u);
}

// A rendering object of a real car part (or boat part) of the race, near the camera in front of it or not, with the
// part's flags and environment map varied. a3 is the tint: 0, 1 or 5 (the shiny pass's fade is only for 5).
void rendObjArgs(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    sCarListClass &list = G(carListVarG);
    ptr32<_sRendObjHead> head = nullptr;
    for (int tries = 0; tries < 80 && !head; ++tries)
    {
        const uint32_t index = nextFuzz(state) % 20u;
        const ptr32<_sObjectDef> def = (nextFuzz(state) & 1u) != 0u ? list.car[index].pDef : list.boat[index].pDef;
        if (def.address == 0 || def.address >= 0x02000000u)
            continue;
        const ptr32<_sRendObjHead> candidate = (nextFuzz(state) & 3u) == 0u ? def->pRendObjHeadAlt : def->pRendObjHead;
        if (candidate.address != 0 && candidate.address < 0x02000000u)
            head = candidate;
    }
    if (!head)
        return;
    _sRendObjHead &part = *head;
    const uint32_t kinds[] = {0x20u, 0x20u, 0x8020u, 0x4020u, 0x2020u, 0xa020u, 0u};
    part.flags = static_cast<uint16>((part.flags & ~0xe020u) | kinds[nextFuzz(state) % 7u]);
    if (G(pLvlG).address != 0xffffffffu && G(pLvlG).address != 0 && (nextFuzz(state) % 4u) != 0u)
        part.envMap = G(pLvlG)->pEnvMap;
    if ((nextFuzz(state) & 1u) != 0u)
    {
        // A made-up environment map, for the sizes and formats the level's does not have.
        static const int32_t sizes[] = {1, 2, 3, 4, 8, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200, 0x400, 0x800, 0};
        static const int32_t formats[] = {0, 2, 10, 0x13, 0x14};
        _sGfxTexture &texture = *static_cast<_sGfxTexture *>(guest_host(scratch + 0x800));
        texture.wd = sizes[nextFuzz(state) % 14u];
        texture.ht = sizes[nextFuzz(state) % 14u];
        texture.fmt = formats[nextFuzz(state) % 5u];
        texture.idxList[0] = static_cast<s32>(nextFuzz(state));
        texture.clutVramAddr = static_cast<s32>(nextFuzz(state));
        part.envMap = ptr32<_sGfxTexture>(scratch + 0x800);
    }
    _sRendObj &object = *static_cast<_sRendObj *>(guest_host(scratch));
    object.pRendObjHead = head;
    const sRdrCamera &camera = G(rdrCameraG);
    const float depth = randomFloat(state, 1.0f) * 1500.0f + 700.0f, side = randomFloat(state, 700.0f), up = randomFloat(state, 500.0f);
    const auto place = [&](float origin, float z, float x, float y) { return static_cast<int32>((origin + depth * z + side * x + up * y) * 256.0f); };
    object.pos.x = place(camera.pos.x, camera.mat.Zx, camera.mat.Xx, camera.mat.Yx);
    object.pos.y = place(camera.pos.y, camera.mat.Zy, camera.mat.Xy, camera.mat.Yy);
    object.pos.z = place(camera.pos.z, camera.mat.Zz, camera.mat.Xz, camera.mat.Yz);
    object.flags = static_cast<uint16>(nextFuzz(state));
    object.matrix.mat.Xx = static_cast<int16>(nextFuzz(state) % 8193u - 4096);
    object.matrix.mat.Yy = static_cast<int16>(nextFuzz(state) % 8193u - 4096);
    object.matrix.mat.Zz = static_cast<int16>(nextFuzz(state) % 8193u - 4096);
    static const int32_t tints[] = {0, 1, 5};
    setArg(ctx, 0, scratch);
    setArg(ctx, 1, 0);
    setArg(ctx, 2, 0);
    setArg(ctx, 3, static_cast<uint32_t>(tints[nextFuzz(state) % 3u]));
}

// The menu system's execute code is one of the three the debug menus act on, or another; a0 a main record of random
// bytes with a status and level.
void debugMenuArgs(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    static const uint32_t codes[] = {0x7fff1000u, 0x7fff1001u, 0x7fff1002u, 0x7fff1003u, 0u};
    const ptr32<sMenuSystem> menus = G(pMenuSystemG);
    if (menus.address != 0 && menus.address < 0x02000000u)
        menus->executeCode = codes[nextFuzz(state) % 5u];
    setArg(ctx, 0, scratch);
    setArg(ctx, 1, scratch + 0x800);
}

// The id of a resource the loader already has (a handle with its low bits clear, which ResLoad returns as it is), or 0.
uint32_t loadedResource(uint64_t &state)
{
    const resClass &res = G(resVarG);
    const uint32_t maxId = res.lookupHeader.maxId;
    if (res.handlesHandle.address == 0 || res.handlesHandle.address >= 0x02000000u || maxId < 2 || maxId > 20000)
        return 0;
    for (int tries = 0; tries < 200; ++tries)
    {
        const uint32_t id = 1 + nextFuzz(state) % (maxId - 1);
        const uint32_t handle = res.handlesHandle[id];
        if ((handle & 3u) == 0 && (handle != 0 || (nextFuzz(state) & 7u) == 0))
            return id;
    }
    return 0;
}

// a0 points at a handle that points at a list of ids, ending in zero.
void wheelListArgs(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t list = scratch + 0x100;
    const uint32_t count = nextFuzz(state) % 7u;
    uint32_t at = 0;
    for (uint32_t i = 0; i < count; ++i)
        if (const uint32_t id = loadedResource(state))
            *static_cast<uint32_t *>(guest_host(list + 4u * at++)) = id;
    *static_cast<uint32_t *>(guest_host(list + 4u * at)) = 0;
    *static_cast<uint32_t *>(guest_host(scratch + 0x80)) = list;
    setArg(ctx, 0, scratch + 0x80);
}

// As wheelListArgs, for a list that starts with its count.
void roblListArgs(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t list = scratch + 0x100;
    uint32_t count = 0;
    const uint32_t wanted = nextFuzz(state) % 7u;
    for (uint32_t i = 0; i < wanted; ++i)
        if (const uint32_t id = loadedResource(state))
            *static_cast<uint32_t *>(guest_host(list + 4u * ++count)) = id;
    *static_cast<uint32_t *>(guest_host(list)) = count;
    *static_cast<uint32_t *>(guest_host(scratch + 0x80)) = list;
    setArg(ctx, 0, scratch + 0x80);
}

// The race's cars and boats (types of 0 to 19 for cars; 0 is none), a level number, and a resource whose ids for
// every type are loaded ones.
void carListArgs(R5900Context *ctx, uint64_t &state, uint32_t scratch)
{
    const uint32_t table = scratch + 0x100;
    for (uint32_t type = 0; type < 40; ++type)
        *static_cast<uint32_t *>(guest_host(table + 4u * type)) = loadedResource(state);
    mainClass &main = G(mainVarG);
    main.noHuman = static_cast<uint8>(nextFuzz(state) % 5u);
    main.noAI = static_cast<uint8>(nextFuzz(state) % 12u);
    static const uint8_t levels[] = {0, 5, 8, 9, 11, 12, 17, 0x61, 0x6d, 3, 1};
    main.level = levels[nextFuzz(state) % 11u];
    sCarListClass &list = G(carListVarG);
    for (uint32_t i = 0; i < 20; ++i)
    {
        list.car[i].type = static_cast<eObjectType>(nextFuzz(state) % 5u == 0 ? 0 : 1 + nextFuzz(state) % 4u);
        list.car[i].altRendObj = static_cast<int32>(nextFuzz(state));
        list.boat[i].type = static_cast<eObjectType>(nextFuzz(state));
        list.boat[i].altRendObj = static_cast<int32>(nextFuzz(state));
        list.car[i].pDef = ptr32<_sObjectDef>(nextFuzz(state));
        list.boat[i].pDef = ptr32<_sObjectDef>(nextFuzz(state));
    }
    list.alternativeRendObj = static_cast<int32>(nextFuzz(state));
    G(objectConstructorNoLoadROBs) = static_cast<int32>(nextFuzz(state));
    *static_cast<uint32_t *>(guest_host(scratch + 0x80)) = table;
    setArg(ctx, 0, scratch + 0x80);
}

// The car list full of random words (the set-up must clear all of it).
void carListScramble(R5900Context *, uint64_t &state, uint32_t)
{
    uint32_t *words = static_cast<uint32_t *>(guest_host(sym::carListVarG));
    for (uint32_t i = 0; i < 0x1ec / 4u; ++i)
        words[i] = nextFuzz(state);
}

// Random values in the game variables the menu variables are linked to, and in every menu variable's feedback pointer
// and value, so the linking shows.
void menuVariableArgs(R5900Context *, uint64_t &state, uint32_t)
{
    struct Link
    {
        int32_t tag;
        ptr32<int32> pLinkVar;
    };
    const Link *links = static_cast<const Link *>(guest_host(sym::menuVariableLinksG));
    for (uint32_t i = 0; links[i].tag != 0; ++i)
        *static_cast<uint32_t *>(guest_host(links[i].pLinkVar.address)) = nextFuzz(state);
    ptr32<ptr32<sMenuItemVar>> list = G(tEditVarG).pMVARList;
    if (G(pLvlG).address != 0xffffffffu)
        list = G(pLvlG)->pMVARList;
    for (uint32_t i = 0; list[i].address != 0 && list[i].address < 0x02000000u; ++i)
    {
        list[i]->pFeedbackVar = ptr32<int32>(nextFuzz(state));
        list[i]->value = static_cast<int32>(nextFuzz(state));
    }
}

// a0 the time, a1 whether the screen is split, a2 which half.
void lsFrameArgs(R5900Context *ctx, uint64_t &state, uint32_t)
{
    setArg(ctx, 0, nextFuzz(state));
    setArg(ctx, 1, nextFuzz(state) % 2u);
    setArg(ctx, 2, nextFuzz(state) % 2u);
}

} // namespace rc::fuzz

RC_DECOMP_FN(VSyncCallbackFunction)
RC_DECOMP_FN(G0Init)
RC_DECOMP_FN(GrabBlock)
RC_DECOMP_FN(MtlInit)
RC_DECOMP_FN(MeshInit)
RC_DECOMP_FN(MeshInitFrame)
RC_DECOMP_FN(MeshSetQuality)
RC_DECOMP_FN(TOBInit)
RC_DECOMP_FN(TOBTerm)
RC_DECOMP_FN(TOBRender)
RC_DECOMP_FN(VISInit)
RC_DECOMP_FN(VISTerm)
RC_DECOMP_FN(VISUpdate)
RC_DECOMP_FN(CarDefCDFConstructDaemon)
RC_DECOMP_FN(CarDefLvlInit)
RC_DECOMP_FN(CarDefLvlTerm)
RC_DECOMP_FN(Gfx_ConstructTex)
RC_DECOMP_FN(LoadErrorCallback)
RC_DECOMP_FN(Mirror)
RC_DECOMP_FN(MirrorSet)
RC_DECOMP_FN(DepthCueInit)
RC_DECOMP_FN(DepthCueSet)
RC_DECOMP_FN(DepthCueTrackEditorSet)
RC_DECOMP_FN(LoadErrorInit)
RC_DECOMP_FN(LoadErrorTerm)
RC_DECOMP_FN_SETUP_HEAP(MVARLinkup, rc::fuzz::menuVariableArgs)
RC_DECOMP_FN_SETUP_HEAP(RCMenuMVarLinkup, rc::fuzz::menuVariableArgs)
RC_DECOMP_FN_SETUP_HEAP(ProcessDebugMenus, rc::fuzz::debugMenuArgs)
RC_DECOMP_FN(LsInit)
RC_DECOMP_FN(LsCameraToRdr)
RC_DECOMP_FN(NodeCalcWorldPos)
RC_DECOMP_FN_SETUP(NodeFindByTag, rc::fuzz::nodeFindArgs)
RC_DECOMP_FN_SETUP(NodeFindNextByTag, rc::fuzz::nodeFindNextArgs)
RC_DECOMP_FN_SETUP(returnPoint, rc::fuzz::splineArgs)
// These work on the resource loader's construct daemon list, which lives on the heap.
RC_DECOMP_FN_SETUP_HEAP(CarDefInit, rc::fuzz::noArgs)
RC_DECOMP_FN_SETUP_HEAP(CarDefTerm, rc::fuzz::noArgs)
RC_DECOMP_FN_SETUP_HEAP(CarListInit, rc::fuzz::carListScramble)
RC_DECOMP_FN_SETUP_HEAP(CarListTerm, rc::fuzz::noArgs)
RC_DECOMP_FN_SETUP_HEAP(WheelInit, rc::fuzz::noArgs)
RC_DECOMP_FN_SETUP_HEAP(WheelTerm, rc::fuzz::noArgs)
RC_DECOMP_FN_SETUP_HEAP(RendObjInit, rc::fuzz::noArgs)
// These draw through GFX_P2's packet buffer on the heap.
RC_DECOMP_FN_SETUP_HEAP(VersionDisplay, rc::fuzz::noArgs)
RC_DECOMP_FN_SETUP_HEAP(LsFrameInit, rc::fuzz::lsFrameArgs)
RC_DECOMP_FN_SETUP_HEAP(LsRenderWorld, rc::fuzz::noArgs)
// These call the loader (ResLoad): the random inputs give them resources that are already loaded, which it hands back
// as they are. Loading a new one is only compared in a game (RendObjConstructDaemon needs a real shape).
RC_DECOMP_FN_SETUP(CarListConstructDaemon, rc::fuzz::carListArgs)
RC_DECOMP_FN_SETUP(WheelConstructDaemon, rc::fuzz::wheelListArgs)
RC_DECOMP_FN_SETUP(ROBLConstructDaemon, rc::fuzz::roblListArgs)
RC_DECOMP_FN_NOFUZZ(RendObjConstructDaemon)
RC_DECOMP_FN_SETUP_HEAP(RendObjRender, rc::fuzz::rendObjArgs)
// Called once, at start-up, to set up the GS: only a whole-game check.
RC_DECOMP_FN_BLOCKING(Gfx_Init)
RC_DECOMP_FN_BLOCKING(Gfx_Term)

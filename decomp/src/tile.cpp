// Decompiled from TILE.c: the track editor's tiles. A built track is a grid of track units (a tile, an elevation and one
// of four orientations); each tile is a VU shape drawn with the theme's four lights turned to the unit's orientation.
// tileVarG holds the four orientation matrices (mRot), the same turned by the camera each frame (viewRot) and the
// level's tile list (pTrackTiles: a count, the cell size, then one shape per tile).
//
// Written from the instructions. TileRender (the editor's grid) and TileRenderGameROB (a built track in a race, centred
// on the grid) differ in where the tile goes and in that only the editor's swaps in the turned view matrices.

#include "tile_fuzz.h"

namespace rc {

void LsCameraToRdr(ptr32<_sRdrCamera> pCam);                                                   // misc.cpp
void MeshInitFrame();                                                                          // misc.cpp
void PS2C_GteMatrixMulMatrix(ptr32<_sMat> pMatOut, ptr32<_sMat> pMat1, ptr32<_sMat> pMat2);    // psxdefs.cpp
void ResTypeSetConstructDaemon(ptr32<char> pType, uint32 constructDaemon, int16 options);       // res.cpp
void ResTypeUnSetConstructDaemon(ptr32<char> pType);                                           // res.cpp
void Rdr_VUShape(ptr32<sRdrVUShape> pShape, ptr32<_sRdrCamera> pCamera, u32 tint);             // shape.cpp
void TERdr_VUShape(ptr32<sRdrVUShape> pShape, ptr32<_sRdrCamera> pCamera, u32 tint);           // shape.cpp

namespace {

constexpr uint32_t kTile = sym::tileVarG, kView3d = sym::view3dVarG, kLightVecs = sym::lightVecsG, kLightVals = sym::lightValsG;
constexpr uint32_t kTeLightVecs = sym::rdr_TELightVecsG, kTeLightVals = sym::rdr_TELightValsG, kRdrCamera = sym::rdrCameraG;
// Resource types: the tile list's (no daemon) and the tiles' (TileConstructDaemon).
constexpr uint32_t kTileListType = (sym::sec_sdata + 0x568u), kTileType = (sym::sec_sdata + 0x570u), kTileConstructDaemon = sym::TileConstructDaemon;
constexpr uint32_t kCellRenderInit = sym::CellRenderInit, kMat3nBuild = sym::Mat3nBuild, kResLoad = sym::ResLoad;
// view3dVarG: the transposed view matrix (in use, and the one the tiles start from) and the view matrix's translation.
constexpr uint32_t kPsxTViewMatrixPtr = kView3d + 0x8u, kPsxTViewMatrix = kView3d + 0x4cu, kViewTrans = kView3d + 0x20u;

template <typename T>
T &mem(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

uint32_t trackTiles() { return mem<uint32_t>(kTile + 0x100u); }
uint32_t tileShape(int16 tileId) { return mem<uint32_t>(trackTiles() + 4u + static_cast<uint32_t>(static_cast<int32_t>(tileId) * 4)); }

// The common part of the two renderers: the unit's tile at (x, y, z) with the lights turned to its orientation.
void renderUnit(GuestFrame &frame, ptr32<_sTrackUnit> pData, uint32 themeType, int32_t x, int32_t z)
{
    const _sTrackUnit unit = *pData;
    const int32_t y = unit.elevation;
    const uint32_t orientation = static_cast<uint32_t>(unit.orientation);
    // zeroVec (passed to CellRenderInit) at sp+0x10, lightVec at sp+0x20.
    const ptr32<int32_t> zeroVec = frame.local<int32_t>(0x10);
    zeroVec[0] = 0;
    zeroVec[1] = 0;
    zeroVec[2] = 0;
    ee_float light[4][3];
    const auto lights = reinterpret_cast<const ee_float(*)[3]>(guest_host(kLightVecs));
    for (int i = 0; i < 4; ++i)
    {
        light[i][0] = lights[i][0];
        light[i][1] = lights[i][1];
        light[i][2] = lights[i][2];
        switch (orientation)
        {
        case 1: // EAST
            light[i][0] = -lights[i][2];
            light[i][2] = lights[i][0];
            break;
        case 2: // SOUTH
            light[i][0] = -lights[i][0];
            light[i][2] = -lights[i][2];
            break;
        case 3: // WEST
            light[i][0] = lights[i][2];
            light[i][2] = -lights[i][0];
            break;
        default:
            break;
        }
    }
    const ptr32<ee_float> stackLights = frame.local<ee_float>(0x20);
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 3; ++k)
            stackLights[i * 3 + k] = light[i][k];
    const uint32_t shape = tileShape(unit.tileId);
    guest_call(kCellRenderInit, zeroVec.address);
    LsCameraToRdr(ptr32<_sRdrCamera>(kRdrCamera));
    const ee_float version = mem<ee_float>(shape);
    if (!(version == ee_float(2.0f)) && !(version == ee_float(3.0f)))
        return;
    mem<ee_float>(shape + 0x2cu) = ee_itof(x);
    mem<ee_float>(shape + 0x30u) = ee_itof(y);
    mem<ee_float>(shape + 0x34u) = ee_itof(z);
    // The orientation is read again (a guest call came between); the matrix goes in transposed.
    const uint32_t rot = kTile + static_cast<uint32_t>(pData->orientation) * 0x20u;
    static const uint32_t kFrom[9] = {0x0, 0x6, 0xc, 0x2, 0x8, 0xe, 0x4, 0xa, 0x10};
    for (uint32_t i = 0; i < 9; ++i)
        mem<ee_float>(shape + 8u + i * 4u) = ee_itof(mem<int16>(rot + kFrom[i])) * ee_float(1.0f / 4096.0f);
    const auto teVecs = reinterpret_cast<ee_float(*)[3]>(guest_host(kTeLightVecs));
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 3; ++k)
            teVecs[i][k] = light[i][k];
    const auto teVals = reinterpret_cast<ee_float(*)[3]>(guest_host(kTeLightVals));
    const uint32_t vals = kLightVals + themeType * 0x30u;
    for (uint32_t i = 0; i < 4; ++i)
        for (uint32_t k = 0; k < 3; ++k)
            teVals[i][k] = mem<ee_float>(vals + i * 12u + k * 4u);
    if (mem<ee_float>(shape) == ee_float(2.0f))
        Rdr_VUShape(ptr32<sRdrVUShape>(shape), ptr32<_sRdrCamera>(kRdrCamera), 0);
    else
        TERdr_VUShape(ptr32<sRdrVUShape>(shape), ptr32<_sRdrCamera>(kRdrCamera), 0);
}

} // namespace

void TileTerm()
{
    mem<uint32_t>(kTile + 0x100u) = 0;
    ResTypeUnSetConstructDaemon(ptr32<char>(kTileListType));
    ResTypeUnSetConstructDaemon(ptr32<char>(kTileType));
}

void TileFrameInit()
{
    for (uint32_t i = 0; i < 4; ++i)
    {
        const uint32_t view = kTile + 0x80u + i * 0x20u;
        PS2C_GteMatrixMulMatrix(ptr32<_sMat>(view), ptr32<_sMat>(kPsxTViewMatrix), ptr32<_sMat>(kTile + i * 0x20u));
        std::memcpy(guest_host(view + 0x14u), guest_host(kViewTrans), 12);
    }
    MeshInitFrame();
}

void TileRender(uint8 xCell, uint8 zCell, ptr32<_sTrackUnit> pData, uint32 themeType)
{
    GuestFrame frame(0x100);
    const uint32_t oldView = mem<uint32_t>(kPsxTViewMatrixPtr);
    const uint32_t cellSize = mem<uint16>(trackTiles() + 2u);
    mem<uint32_t>(kPsxTViewMatrixPtr) = kTile + 0x80u;
    // transVec (sp+0) holds -x * size and -z * size; the tile goes at their negations.
    const uint32_t tx = (0u - xCell) * cellSize, tz = (0u - zCell) * cellSize;
    frame.local<uint32_t>(0)[0] = tx;
    frame.local<uint32_t>(0)[1] = static_cast<uint32_t>(static_cast<int32_t>(pData->elevation));
    frame.local<uint32_t>(0)[2] = tz;
    renderUnit(frame, pData, themeType, static_cast<int32_t>(0u - tx), static_cast<int32_t>(0u - tz));
    mem<uint32_t>(kPsxTViewMatrixPtr) = oldView;
}

void TileRenderGameROB(uint8 xCell, uint8 zCell, uint8 xMax, uint8 zMax, ptr32<_sTrackUnit> pData, uint32 themeType)
{
    GuestFrame frame(0xf0);
    const uint32_t oldView = mem<uint32_t>(kPsxTViewMatrixPtr);
    const uint32_t tx = ((static_cast<uint32_t>(xCell) - (xMax >> 1)) << 11) + 0x400u;
    const uint32_t tz = ((static_cast<uint32_t>(zCell) - (zMax >> 1)) << 11) + 0x400u;
    frame.local<uint32_t>(0)[0] = tx;
    frame.local<uint32_t>(0)[1] = static_cast<uint32_t>(static_cast<int32_t>(pData->elevation));
    frame.local<uint32_t>(0)[2] = tz;
    renderUnit(frame, pData, themeType, static_cast<int32_t>(tx), static_cast<int32_t>(tz));
    mem<uint32_t>(kPsxTViewMatrixPtr) = oldView;
}

void TileInit()
{
    GuestFrame frame(0x30);
    const ptr32<int16> angles = frame.local<int16>(0);
    angles[0] = 0;
    angles[2] = 0;
    angles[1] = 0;
    guest_call(kMat3nBuild, kTile, angles.address);
    static const struct { int16 head; uint32_t offset; } kTurns[3] = {{0x4000, 0x60u}, {-0x8000, 0x40u}, {-0x4000, 0x20u}};
    for (const auto &turn : kTurns)
    {
        angles[1] = turn.head;
        guest_call(kMat3nBuild, kTile + turn.offset, angles.address);
    }
    ResTypeSetConstructDaemon(ptr32<char>(kTileListType), 0, 6);
    ResTypeSetConstructDaemon(ptr32<char>(kTileType), kTileConstructDaemon, 1);
}

int16 TileGetYStart(int16 tileId)
{
    return mem<int16>(tileShape(tileId) + 0x6au);
}

uint32 TileGetSize(int16) { return 1000; }

void TileConstructALFCell(uint8, uint8, uint8, uint8, ptr32<_sTrackUnit>, uint32) {}

void TileConstructDaemon(ptr32<memHandle>) {}

void TileResourceConstructDaemon(ptr32<memHandle> pHandle)
{
    GuestFrame frame(0x50);
    const uint32_t tiles = mem<uint32_t>(pHandle.address);
    mem<uint32_t>(kTile + 0x100u) = tiles;
    for (uint32_t i = mem<uint16>(tiles); i-- > 0;)
    {
        const uint32_t at = tiles + 4u + i * 4u;
        mem<uint32_t>(at) = guest_call<uint32_t>(kResLoad, mem<uint32_t>(at));
    }
}

} // namespace rc

using rc::fuzz::tileDaemon;
using rc::fuzz::tileEditor;
using rc::fuzz::tileNone;
using rc::fuzz::tileRender;
using rc::fuzz::tileYStart;

RC_DECOMP_FN_SETUP_HEAP(TileTerm, tileNone)
RC_DECOMP_FN_SETUP(TileFrameInit, tileNone)
// These draw through the shape renderers' packet rings and the DMA chain, on the heap.
RC_DECOMP_FN_SETUP_HEAP(TileRender, tileEditor)
RC_DECOMP_FN_SETUP_HEAP(TileRenderGameROB, tileRender)
RC_DECOMP_FN_SETUP_HEAP(TileInit, tileNone)
RC_DECOMP_FN_SETUP(TileGetYStart, tileYStart)
RC_DECOMP_FN(TileGetSize)
RC_DECOMP_FN(TileConstructALFCell)
RC_DECOMP_FN(TileConstructDaemon)
RC_DECOMP_FN_SETUP(TileResourceConstructDaemon, tileDaemon)

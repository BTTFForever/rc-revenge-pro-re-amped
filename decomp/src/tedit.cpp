// Decompiled from TEDIT.c: the Track Editor's set-up and tear-down, its track (the list of edit modules and the grid
// of track units it builds), the cursor's module list, the edit menu's slopes and restrictions, the resource meter and
// the track's save buffers. See tedit.h for the data.
//
// Calls out of the unit go through the function table (guest_call), so the oracle's setups can replace them; the
// unit's native functions call each other directly. Locals passed by address sit in the original's frame (GuestFrame)
// at the original's offsets.

#include "tedit.h"
#include "tedit_fuzz.h"

namespace rc {

using namespace tedit;

namespace {

// The heap: MemBlockAllocLo(&handle, size) and MemBlockFree(&handle) on a handle in the frame.
void allocLo(uint32_t handle, uint32_t size) { guest_call<int32_t>(sym::MemBlockAllocLo, handle, size); }
void memFree(uint32_t handle) { guest_call<int32_t>(sym::MemBlockFree, handle); }

// Frees a list of 8-byte nodes linked through their first word, each through a handle at $sp + 0, reading the next
// node before the free (ClearTrackModuleList, ClearCursorModuleList, TEditGenerateTrackFromPermSaveBuffer).
void freeList(uint32_t first, uint32_t handle)
{
    uint32_t node = first;
    at<uint32_t>(handle) = node;
    do
    {
        const uint32_t next = at<uint32_t>(node);
        memFree(handle);
        at<uint32_t>(handle) = next;
        node = next;
    } while (node != 0);
}

// Appends a node to a list linked through its first word (the original walks it unrolled eight times).
void appendNode(ptr32<uint32_t> head, uint32_t node)
{
    if (*head == 0)
    {
        *head = node;
        return;
    }
    uint32_t last = *head;
    while (at<uint32_t>(last) != 0)
        last = at<uint32_t>(last);
    at<uint32_t>(last) = node;
}

// The edit menu's slope row of a slope (0-4; 5 if none matches).
uint32_t slopeRow(int32_t slope)
{
    uint32_t i = 0;
    while (i < 5u && at<int32_t>(kSlopeLookUp + i * 4u) != slope)
        ++i;
    return i;
}

// Writes the track's description and its module ids (then -1) from `to`, the description's first word replaced by the
// save mark (TEditSaveTrackDescToPermSaveBuffer, TEditInsertTrackDescToSaveBuffer).
void writeTrackDesc(uint32_t to)
{
    std::memcpy(guest_host(to), guest_host(kTE + 0x20u), 0x18u);
    at<uint32_t>(to) = 0x76543210u;
    uint32_t out = to + 0x18u;
    for (uint32_t node = te().trackDesc.pTrackModuleList.address; node != 0;)
    {
        const uint32_t id = at<uint32_t>(node + 4u);
        node = at<uint32_t>(node);
        at<uint32_t>(out) = id;
        out += 4u;
    }
    at<uint32_t>(out) = 0xffffffffu;
}

} // namespace

void TERInit()
{
    {
        GuestFrame frame(0x10);
        guest_call(sym::ResTypeSetConstructDaemon, kResTRM, 0, 2);
        guest_call(sym::ResTypeSetConstructDaemon, kResTRS, 0, 6);
        guest_call(sym::ResTypeSetConstructDaemon, kResICO, 0, 4);
        guest_call(sym::ResTypeSetConstructDaemon, kResLSD, 0, 6);
    }
    guest_call(sym::TileInit);
}

void TEditLevelInit()
{
    {
        GuestFrame frame(0x10);
        guest_call(sym::ResTypeSetConstructDaemon, kResTRM, 0, 2);
        guest_call(sym::ResTypeSetConstructDaemon, kResTSKY, 0, 6);
        guest_call(sym::ResTypeSetConstructDaemon, kResRLST, 0, 6);
    }
    guest_call(sym::TileInit);
}

void TEditLevelTerm()
{
    {
        GuestFrame frame(0x10);
        guest_call(sym::ResTypeUnSetConstructDaemon, kResTRM);
        guest_call(sym::ResTypeUnSetConstructDaemon, kResRLST);
        guest_call(sym::ResTypeUnSetConstructDaemon, kResTSKY);
    }
    guest_call(sym::TileTerm);
}

void TEditSetupTELvLClass(ptr32<_sTELVLData> pData)
{
    GuestFrame frame(0x30);
    sTEditClass &t = te();
    t.pEditModuleResourceHeader = pData->pEditModule;
    for (int i = 0; i < 6; ++i)
    {
        t.pTrackTiles[i] = pData->pTrackTiles[i];
        t.pTrackSkies[i] = pData->pTrackSkies[i];
    }
    t.pTrackSprites = pData->pTrackSprites;
    t.pTrackWarnings[0] = pData->pTrackWarn1;
    t.pEditModuleList = ptr32<_sEditModule>(t.pEditModuleResourceHeader.address + 4u);
    t.pTrackWarnings[1] = pData->pTrackWarn2;
    t.pTrackWarnings[2] = pData->pTrackWarn3;
    t.pTrackWarnings[3] = pData->pTrackWarn4;
    t.pTrackUnsaved = pData->pTrackWarn5;
    t.pTrackIncomplete = pData->pTrackWarn6;
    t.pTrackSavedOk = pData->pTrackWarn7;
    t.pTrackComplete = pData->pTrackWarn8;
    t.pTrackMainMenu = pData->pTrackMainMenu;
    t.pTrackOptionsMenu = pData->pTrackOptionsMenu;
    t.pTrackAutoCompMsg[0] = pData->pTrackAutoCompMsg1;
    t.pTrackAutoCompMsg[1] = pData->pTrackAutoCompMsg2;
    t.pTrackAutoCompMsg[2] = pData->pTrackAutoCompMsg3;
    t.pTrackAutoCompMsg[3] = pData->pTrackAutoCompMsg4;
    t.pTrackAutoCompActive = pData->pTrackAutoCompActive;
    t.pTrackUnPlayable = pData->pTrackUnPlayable;
    t.pTrackPlayer2CarSelectMenu = pData->pTrackPlayer2CarSelect;
    t.pTrackMenuBar[0] = pData->pTrackMenuBar[0];
    t.pTrackMenuBar[1] = pData->pTrackMenuBar[1];
    t.pTrackMenuBar[2] = pData->pTrackMenuBar[2];
    guest_call(sym::FontSet, pData->pFont);
    guest_call(sym::LoadSaveSetupClass, pData->pLoadSaveData);
    t.ppCarNames = pData->ppCarNames;
    t.pTrackEditorHelpData = pData->pTrackEditorHelpData;
    t.pMVARList = pData->pMVARList;
    t.pGeneralText = pData->pGeneralText;
}

// Loads the editor's level and starts with an empty grid and the cursor at its start; a track kept from an earlier
// visit (loadSaveFlags bit 1) is built again from the permanent save buffer.
void TEditInit()
{
    GuestFrame frame(0x40);
    sTEditClass &t = te();
    TERInit();
    guest_call(sym::LevelLoad, 0x63);
    guest_call(sym::Rdr_ShapeInit);
    TEditInitialiseCursor();
    TEditInitialiseEditMenu();
    guest_call(sym::DepthCueTrackEditorSet, 0x4000);
    guest_call(sym::AMenuSetAreaMenuMaxStringLength, 7);
    TEditClearTrack();
    TEditResetResourceMeter();
    guest_call(sym::RandomSetSeed, G(sysTimerVarG).tickTime);
    t.trackDesc.pTrackModuleList = nullptr;
    t.trackDesc.themeTypeNdx = 0;
    t.trackDesc.bestLapTime = kUnsavedLapTime;
    t.trackDesc.soundTrackNdx = 0;
    guest_call(sym::MVARLinkup);
    t.newCameraMode = 1;
    t.trackState.currentMode = 0;
    G(tileVarG).pTrackTiles = t.pTrackTiles[t.trackDesc.themeTypeNdx];
    t.trackState.editState = 0;
    t.placementViewDir = NORTH;
    t.cameraTurning = 0;
    t.display2DGridMap = 0;
    t.errorFlags = 0;
    t.placementErrorChecking = 0;
    t.trackIsComplete = 0;
    t.input.analog1LastInput = 0;
    if ((t.loadSaveFlags & 2u) != 0)
        TEditGenerateTrackFromPermSaveBuffer();
    G(mainVarG).splitScreen = 0;
    G(mainVarG).trackEdRace = 0;
    G(rdrCameraG).vertDrawOffset = 0.0f;
}

void TEditAllocPermTrackStructures()
{
    {
        GuestFrame frame(0x20);
        TEditCreateTrack();
        TEditClearTrack();
    }
    guest_call<int32_t>(sym::MemBlockAllocLo, kTE + 0x3cu, 0x200);
}

void TEditFreePermTrackStructures()
{
    {
        GuestFrame frame(0x20);
        TEditFreeTrack();
    }
    guest_call<int32_t>(sym::MemBlockFree, kTE + 0x3cu);
}

// Leaves the editor: to a test race on the built track if one was asked for (mainVarG.trackEdRace), else to the front
// end.
void TEditTerm()
{
    mainClass &m = G(mainVarG);
    {
        GuestFrame frame(0x30);
        ClearCursorModuleList();
        ClearTrackModuleList();
        guest_call(sym::TileTerm);
        guest_call(sym::Rdr_ShapeTerm);
        guest_call(sym::ResTypeUnSetConstructDaemon, kResTRM);
        guest_call(sym::ResTypeUnSetConstructDaemon, kResTRS);
        guest_call(sym::ResTypeUnSetConstructDaemon, kResICO);
        guest_call(sym::ResTypeUnSetConstructDaemon, kResLSD);
        if (m.trackEdRace != 1)
        {
            m.toDo = static_cast<eMainToDo>(1); // DO_FRONT_END
            return;
        }
        m.toDo = static_cast<eMainToDo>(3); // DO_MAIN_GAME
        m.level = 0x61;
        guest_call(sym::strcpy, sym::mainVarG + 0x1c0u, guest_call<uint32_t>(sym::GetGeneralString, 8));
        guest_call(sym::MainSetCars, 3);
    }
    guest_call(sym::LoadingInit, m.toDo);
}

void TEditControl(tTime time)
{
    sTEditClass &t = te();
    {
        GuestFrame frame(0x20);
        switch (t.trackState.currentMode)
        {
        case 0:
            TEditControlGridEdit(time);
            break;
        case 1:
            TEditControlMenu(time);
            break;
        case 3:
        {
            const ptr32<_sTrackInput> input = TEditGetInput(time);
            if (TEditCheckDeActivateFarView(input) == 0)
                TEditCheckRotateCamera(input);
            break;
        }
        default:
            break;
        }
    }
    TEditCameraFollowCursor();
}

void TEditDisplay(tTime time, uint32 frameCnt)
{
    sTEditClass &t = te();
    {
        GuestFrame frame(0x10);
        switch (t.trackState.currentMode)
        {
        case 0:
            TEditDisplayGridEdit(time);
            break;
        case 1:
            guest_call(sym::TEditDisplayMainMenus, time, frameCnt);
            break;
        case 2:
            TEditDisplayHelpScreen(time);
            break;
        case 3:
            TEditDisplay3DGridLines();
            TEditRenderCursor();
            TEditRenderTrack();
            break;
        default:
            break;
        }
    }
    TEditRenderSkyDome();
}

// The cursor at (7, 7) on the 16 x 16 grid, flat, facing north, with the first module (the start grid) on it.
void TEditInitialiseCursor()
{
    sCursorDesc &c = te().cursor;
    c.xPos = 7;
    c.yPos = 7;
    c.yMax = 0x10;
    c.ySize = 1;
    c.pCursorModuleList = nullptr;
    c.xMax = 0x10;
    c.xSize = 1;
    c.currentSlope = SLOPE_FLAT;
    c.direction = NORTH;
    c.elevation = 0;
    AddEditModuleToCursorModuleList(ptr32<_sEditModuleDesc>(kEditModuleList));
}

void TEditCreateTrack() { guest_call<int32_t>(sym::MemBlockAllocLo, kTE + 0x1cu, 0x1800); }

void TEditFreeTrack() { guest_call<int32_t>(sym::MemBlockFree, kTE + 0x1cu); }

void tedit::controlEditing(ptr32<_sTrackInput> pInput, tTime time, uint32_t s5)
{
    {
        GuestFrame frame(0x30);
        TEditProcessEditMenuSpecialKeys(pInput, time);
    }
    processEditMenuKeys(pInput, time, s5);
}

void TEditControlEditing(ptr32<_sTrackInput> pInput, tTime time)
{
    controlEditing(pInput, time, getRegU32(&g_runtime->eeScheduler().runningContext(), 21));
}

void TEditInitPermTrackStructures()
{
    sTEditClass &t = te();
    t.loadSaveFlags = 0;
    t.track.pTrackUnits = nullptr;
    t.trackDesc.pTrackModuleList = nullptr;
    t.pSavedTrackData = nullptr;
}

void TEditSetupTCLvLClass(ptr32<_sTCLVLData> pData)
{
    GuestFrame frame(0x30);
    sTEditClass &t = te();
    t.pEditModuleResourceHeader = pData->pEditModule;
    t.pEditModuleList = ptr32<_sEditModule>(t.pEditModuleResourceHeader.address + 4u);
    G(tcolG).pTCAMeshList = pData->pTrackColData[TEditGetThemeTypeForTrack()];
}

void TEditSetupTALvLClass(ptr32<_sTALVLData> pData)
{
    GuestFrame frame(0x40);
    sTEditClass &t = te();
    t.pEditModuleResourceHeader = pData->pEditModule;
    t.pEditModuleList = ptr32<_sEditModule>(t.pEditModuleResourceHeader.address + 4u);
    G(tileVarG).pTrackTiles = pData->pTrackTiles[TEditGetThemeTypeForTrack()];
    t.pTrackMapBits = pData->pTrackMapBits;
    t.pAINode = pData->pAINode;
    G(svfVarG).pSvf = pData->pSvf;
}

void TEditFreeTrackALF()
{
    GuestFrame frame(0x20);
    guest_call<int32_t>(sym::MemBlockFree, sym::lsVarG);
    G(lsVarG).pParent = nullptr;
}

// The track into the memory card's save buffer: a checksum at 0x200, then the description (its first word the save
// mark) and the module ids from 0x204.
void TEditInsertTrackDescToSaveBuffer()
{
    GuestFrame frame(0x20);
    const uint32_t buffer = G(loadSaveVarG).pSaveGameBuffer.address;
    writeTrackDesc(buffer + 0x204u);
    at<uint32_t>(buffer + 0x200u) = guest_call<uint32_t>(sym::ResBinaryChecksum, 0, buffer + 0x204u, 0x200);
}

// A loaded track from the save buffer into the permanent one, if its checksum is right.
bool_ TEditExtractTrackDescFromSaveBuffer()
{
    GuestFrame frame(0x40);
    const uint32_t saved = te().pSavedTrackData.address;
    const uint32_t buffer = G(loadSaveVarG).pSaveGameBuffer.address;
    if (guest_call<uint32_t>(sym::ResBinaryChecksum, 0, buffer + 0x204u, 0x200) != at<uint32_t>(buffer + 0x200u))
        return 0;
    for (uint32_t i = 0; i < 0x200u; i += 8u)
        at<uint64_t>(saved + i) = at<uint64_t>(buffer + 0x204u + i);
    return 1;
}

uint32 TEditGetThemeTypeForTrack() { return te().trackDesc.themeTypeNdx; }

uint32 TEditGetSoundTrackNdxForTrack() { return te().trackDesc.soundTrackNdx; }

uint32 TEditGetBestLapTimeFromPermSaveBuffer() { return te().pSavedTrackData[4]; }

void TEditSetBestLapTimeToPermSaveBuffer(uint32 bestLapTime) { te().pSavedTrackData[4] = static_cast<int32>(bestLapTime); }

void TEditSetTileListForThemeType(ptr32<void> pTileList, uint32 themeType)
{
    G(tileVarG).pTrackTiles = ptr32<_sTrackTiles>(at<uint32_t>(pTileList.address + themeType * 4u));
}

// Empties the whole grid: every unit of the three layers gets tile -1, elevation 0, north.
void TEditClearTrack()
{
    const uint32_t units = te().track.pTrackUnits.address;
    for (uint32_t i = 0; i < 0x300u; ++i)
    {
        at<int16_t>(units + i * 8u) = -1;
        at<int16_t>(units + i * 8u + 2u) = 0;
        at<int32_t>(units + i * 8u + 4u) = NORTH;
    }
}

// How many layers of a grid position hold a tile (up to 3), and each one's elevation plus its tile's base height.
void TEditGetTrackUnitInstanceNum(int16 xPos, int16 yPos, ptr32<uint8> instanceNum, ptr32<int16> unitElevation)
{
    GuestFrame frame(0x80);
    const int32_t index = xPos + yPos * 16;
    uint32_t count = 0;
    uint32_t unit = unitAddress(index);
    while (at<int16_t>(unit) != -1)
    {
        const int16_t yStart = guest_call<int16_t>(sym::TileGetYStart, at<int16_t>(unit));
        unitElevation[count] = static_cast<int16_t>(at<uint16_t>(unit + 2u) + yStart);
        count = (count + 1u) & 0xffu;
        if (count >= 3u)
            break;
        unit = unitAddress(static_cast<int32_t>(count) * 256 + index);
    }
    *instanceNum = static_cast<uint8_t>(count);
}

void TEditSetTrackUnit(uint16 xPos, uint16 yPos, ptr32<_sTrackUnit> pTrackUnitData, uint8 instanceNum)
{
    const int32_t layer = instanceNum != 0 ? static_cast<int8_t>(instanceNum) * 256 : 0;
    const uint32_t unit = unitAddress(layer + xPos + yPos * 16);
    const uint16_t tileId = at<uint16_t>(pTrackUnitData.address);
    const int32_t orientation = at<int32_t>(pTrackUnitData.address + 4u);
    at<uint16_t>(unit) = tileId;
    at<int32_t>(unit + 4u) = orientation;
    at<uint16_t>(unit + 2u) = at<uint16_t>(pTrackUnitData.address + 2u);
}

// The far view (mode 3) ends with key 11.
bool_ TEditCheckDeActivateFarView(ptr32<_sTrackInput>)
{
    GuestFrame frame(0x20);
    const bool_ hit = guest_call<bool_>(sym::InputHit, 11);
    if (hit != 0)
    {
        te().trackState.currentMode = 0;
        te().newCameraMode = 1;
    }
    return hit;
}

// Keys 8 and 9 turn the placement view a quarter each way, unless the camera is still turning.
void TEditCheckRotateCamera(ptr32<_sTrackInput>)
{
    GuestFrame frame(0x20);
    sTEditClass &t = te();
    if (t.cameraTurning == 1)
        return;
    if (guest_call<bool_>(sym::InputHit, 8) != 0)
        t.newPlacementViewDir = static_cast<eDirection>((t.placementViewDir + 1) & 3);
    else if (guest_call<bool_>(sym::InputHit, 9) != 0)
        t.newPlacementViewDir = t.placementViewDir == NORTH ? WEST : static_cast<eDirection>(t.placementViewDir - 1);
    t.placementViewDir = t.newPlacementViewDir;
}

void TEditProcessEditMenuSpecialKeys(ptr32<_sTrackInput>, tTime) {}

// The column of the edit menu's row for a slope that holds an editModuleListG entry (7 if none).
uint8 GetEditMenuModuleIndex(eSlope currentSlope, uint32 editModuleId)
{
    const uint32_t row = kGridSlopes + slopeRow(currentSlope) * 0x2cu;
    const uint32_t desc = editModuleDesc(editModuleId);
    uint32_t i = 0;
    while (i < 7u && at<uint32_t>(row + i * 4u) != desc)
        ++i;
    return static_cast<uint8_t>(i);
}

void AddEditModuleToCursorModuleList(ptr32<_sEditModuleDesc> pEditModuleDesc)
{
    GuestFrame frame(0x40);
    const uint32_t handle = frame.local<uint32_t>(0).address;
    at<uint32_t>(handle) = 0;
    allocLo(handle, 8);
    const uint32_t node = at<uint32_t>(handle);
    at<uint32_t>(node + 4u) = pEditModuleDesc.address;
    at<uint32_t>(node) = 0;
    appendNode(ptr32<uint32_t>(kTE + 0x14u), node);
}

void ClearCursorModuleList()
{
    GuestFrame frame(0x40);
    const uint32_t first = te().cursor.pCursorModuleList.address;
    if (first == 0)
        return;
    freeList(first, frame.local<uint32_t>(0).address);
    te().cursor.pCursorModuleList = nullptr;
}

void ClearTrackModuleList()
{
    GuestFrame frame(0x40);
    const uint32_t first = te().trackDesc.pTrackModuleList.address;
    if (first == 0)
        return;
    freeList(first, frame.local<uint32_t>(0).address);
    te().trackDesc.pTrackModuleList = nullptr;
}

void AddTrackModuleToTrackModuleList(int32 editModuleId, bool_ setUnsavedFlag)
{
    GuestFrame frame(0x50);
    const uint32_t handle = frame.local<uint32_t>(0).address;
    at<uint32_t>(handle) = 0;
    allocLo(handle, 8);
    const uint32_t node = at<uint32_t>(handle);
    at<int32_t>(node + 4u) = editModuleId;
    at<uint32_t>(node) = 0;
    appendNode(ptr32<uint32_t>(kTE + 0x20u), node);
    if (setUnsavedFlag == 1)
    {
        te().trackDesc.bestLapTime = kUnsavedLapTime;
        te().loadSaveFlags |= 1u;
    }
}

// Builds the grid from the module list, from the start position, and leaves the cursor at the track's end.
void tedit::generateTrackFromList(uint32_t a3)
{
    GuestFrame frame(0xa0);
    sTEditClass &t = te();
    const uint32_t sp = frame.local<uint8_t>(0).address; // cursorModule 0x0, x 0x10, y 0x12, elevation 0x14, direction 0x18
    uint32_t node = t.trackDesc.pTrackModuleList.address;
    at<int32_t>(sp + 0x18u) = t.trackDesc.startOrientation;
    at<int16_t>(sp + 0x14u) = t.trackDesc.startElevation;
    at<int16_t>(sp + 0x10u) = t.trackDesc.startXPos;
    at<int16_t>(sp + 0x12u) = t.trackDesc.startYPos;
    uint32_t slope = a3;
    for (; node != 0; node = at<uint32_t>(node))
    {
        at<uint32_t>(sp + 4u) = editModuleDesc(at<uint32_t>(node + 4u));
        slope = static_cast<uint32_t>(TEditAdjustCursorModule(ptr32<_sCursorModule>(sp), ptr32<int16>(sp + 0x10u), ptr32<int16>(sp + 0x12u),
                                                              ptr32<eDirection>(sp + 0x18u), ptr32<int16>(sp + 0x14u), 2));
    }
    t.cursor.direction = static_cast<eDirection>(at<int32_t>(sp + 0x18u));
    t.cursor.yPos = at<int16_t>(sp + 0x12u);
    t.cursor.elevation = at<int16_t>(sp + 0x14u);
    t.cursor.currentSlope = static_cast<eSlope>(slope);
    t.cursor.xPos = at<int16_t>(sp + 0x10u);
}

void GenerateTrackFromTrackModuleList()
{
    generateTrackFromList(getRegU32(&g_runtime->eeScheduler().runningContext(), 7));
}

void TEditResetResourceMeter() { te().resourceMemoryUsed = 0; }

void TEditUpdateResourceMeter(ptr32<uint32> pResourceCounter, int16 tileId)
{
    GuestFrame frame(0x20);
    const uint32_t size = guest_call<uint32_t>(sym::TileGetSize, tileId);
    *pResourceCounter = *pResourceCounter + size;
}

uint8 TEditGetSlopeNdxFromTrueSlope(eSlope slope) { return static_cast<uint8_t>(slopeRow(slope)); }

eSlope TEditGetEditModuleOutputSlope(uint32 editModuleId)
{
    const uint32_t desc = editModuleDesc(editModuleId);
    const uint32_t module = editModule(at<uint16_t>(desc));
    if ((at<uint16_t>(desc + 2u) & 1u) != 0)
        return static_cast<eSlope>(-at<int32_t>(module + 8u));
    return static_cast<eSlope>(at<int32_t>(module + 0xcu));
}

eSlope TEditGetEditModuleInputSlope(uint8 slopeNdx, uint8 moduleNdx)
{
    const uint32_t desc = at<uint32_t>(kGridSlopes + slopeNdx * 0x2cu + moduleNdx * 4u);
    const uint32_t module = editModule(at<uint16_t>(desc));
    if ((at<uint16_t>(desc + 2u) & 1u) != 0)
        return static_cast<eSlope>(-at<int32_t>(module + 0xcu));
    return static_cast<eSlope>(at<int32_t>(module + 8u));
}

// The corners that go from flat to a slope or back: their slope.
bool_ TEditCheckForFlatInOutSlopedCorners(uint8 editModuleId, ptr32<eSlope> outputSlope)
{
    eSlope slope;
    switch (editModuleId)
    {
    case 0xd: case 0xf:
        slope = SLOPE_INCLINE_3_1;
        break;
    case 0x10: case 0x11: case 0x13: case 0x14:
        slope = SLOPE_INCLINE_2_1;
        break;
    case 0x1c: case 0x1d: case 0x1f: case 0x20:
        slope = SLOPE_DECLINE_2_1;
        break;
    case 0x21: case 0x23:
        slope = SLOPE_DECLINE_3_1;
        break;
    default:
        return 0;
    }
    *outputSlope = slope;
    return 1;
}

void TEditRestrictSpecial(uint32 restrictSpecial)
{
    sEditMenuState &e = te().editMenu;
    if ((restrictSpecial & 1u) != 0)
        e.noSpecialAvail = 1;
    if ((restrictSpecial & 2u) != 0)
        e.specialRestrict[1][5] = 1;
    if ((restrictSpecial & 4u) != 0)
        e.specialRestrict[1][6] = 1;
}

// Bit c of restrictLeft (c < 3) restricts the left turns of column c on each slope row whose bit is set in
// restrictSlope (row r: bit r).
void TEditRestrictLeftTurn(int32 restrictLeft, int32 restrictSlope)
{
    uint32_t left = static_cast<uint32_t>(restrictLeft);
    for (uint32_t c = 0; c < 3u; ++c, left >>= 1)
        if ((left & 1u) != 0)
            for (uint32_t r = 0; r < 5u; ++r)
                if (((static_cast<uint32_t>(restrictSlope) >> r) & 1u) != 0)
                    te().editMenuGridSlopes[r].restrictAvail[c] = 1;
}

// As TEditRestrictLeftTurn, for the right turns (columns 4-6).
void TEditRestrictRightTurn(int32 restrictRight, int32 restrictSlope)
{
    uint32_t right = static_cast<uint32_t>(restrictRight);
    for (uint32_t c = 0; c < 3u; ++c, right >>= 1)
        if ((right & 1u) != 0)
            for (uint32_t r = 0; r < 5u; ++r)
                if (((static_cast<uint32_t>(restrictSlope) >> r) & 1u) != 0)
                    te().editMenuGridSlopes[r].restrictAvail[c + 4u] = 1;
}

void TEditRestrictStraight(int32 restrictSlope)
{
    for (uint32_t r = 0; r < 5u; ++r)
        if (((static_cast<uint32_t>(restrictSlope) >> r) & 1u) != 0)
            te().editMenuGridSlopes[r].restrictAvail[3] = 1;
}

void TEditRestrictSlopeChange(int32 restrictSlope)
{
    for (uint32_t r = 0; r < 5u; ++r)
        if (((static_cast<uint32_t>(restrictSlope) >> r) & 1u) != 0)
            te().editMenu.slopeRestrict[r] = 1;
}

// No change straight from one slope to the other: a decline cannot be followed by the two inclines, nor an incline by
// the two declines.
void TEditRestrictSlopeToSlopeChange(eSlope currentSlope)
{
    sEditMenuState &e = te().editMenu;
    if (currentSlope == SLOPE_FLAT)
        return;
    if (currentSlope < SLOPE_FLAT)
    {
        e.slopeRestrict[1] = 1;
        e.slopeRestrict[0] = 1;
        return;
    }
    e.slopeRestrict[4] = 1;
    e.slopeRestrict[3] = 1;
}

void TEditClearAllRestricts()
{
    sTEditClass &t = te();
    t.editMenu.noSpecialAvail = 0;
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 7; ++j)
            t.editMenu.specialRestrict[i][j] = 0;
    for (int r = 0; r < 5; ++r)
    {
        t.editMenu.slopeRestrict[r] = 0;
        for (int j = 0; j < 7; ++j)
            t.editMenuGridSlopes[r].restrictAvail[j] = 0;
    }
}

// Complete: the cursor is back at the start, flat, at the start's height and direction.
bool_ TEditCheckForCompleteTrack()
{
    const sTEditClass &t = te();
    if (!t.trackDesc.pTrackModuleList)
        return 0;
    return t.cursor.xPos == t.trackDesc.startXPos && t.cursor.yPos == t.trackDesc.startYPos &&
                   t.cursor.currentSlope == SLOPE_FLAT && t.cursor.elevation == t.trackDesc.startElevation &&
                   t.cursor.direction == t.trackDesc.startOrientation
               ? 1
               : 0;
}

void TEditSaveTrackDescToPermSaveBuffer() { writeTrackDesc(te().pSavedTrackData.address); }

// Builds the kept track again (TEditInit): the module list, the grid, the edit menu for the cursor; a complete track
// leaves no pieces on the cursor.
void TEditGenerateTrackFromPermSaveBuffer()
{
    GuestFrame frame(0x40);
    sTEditClass &t = te();
    TEditCreateTrackDescFromPermSaveBuffer();
    GenerateTrackFromTrackModuleList();
    TEditClearAllRestricts();
    TEditCheckCursorDirectionConstraints();
    TEditCheckCursorMinMaxHeightConstraints();
    availSlopeAndModule(getRegU32(&g_runtime->eeScheduler().runningContext(), 5));
    TEditSetNewCursorModule();
    const bool_ complete = TEditCheckForCompleteTrack();
    if (complete == 1)
    {
        const uint32_t first = t.cursor.pCursorModuleList.address;
        t.trackIsComplete = 1;
        if (first != 0)
        {
            freeList(first, frame.local<uint32_t>(0).address);
            t.cursor.pCursorModuleList = nullptr;
        }
    }
    t.trackState.editState = 2;
}

// The sprite of an edit module's picture (moduleToColListG); an id not in the table reads the entry past its end.
ptr32<uint8> TEditGetTileSprPtr(uint8 editModuleId)
{
    const uint32_t table = sym::moduleToColListG;
    uint32_t i = 0;
    while (i < 0x2au && at<uint16_t>(table + i * 8u) != editModuleId)
        ++i;
    return ptr32<uint8>(at<uint32_t>(table + i * 8u + 4u));
}

// Each node's distance to the finish, walking back from node 3 through the first previous links.
void TEditGenerateTrackAINodesFinishDist()
{
    sAINodeClass &ai = G(aiNodeVarG);
    const int16_t count = ai.noNodesSingle;
    int32_t total = 0;
    uint32_t node = ai.pNode.address + 3u * 0x6cu;
    for (int32_t i = 0; i < count; ++i)
    {
        at<int32_t>(node + 4u) = total; // iFinishDist
        node = at<uint32_t>(node + 8u); // prev[0]
        total += at<int32_t>(node + 0x60u); // link.dist[0]
    }
    ai.nodeTotalDist = total;
}

// Whether a grid position already holds a tile within 0x600 of height (start grid placement).
bool_ TEditAutoCompIsStartGridBlocked(int16 xPos, int16 yPos)
{
    GuestFrame frame(0x30);
    const ptr32<uint8> instanceNum = frame.local<uint8>(0x10);
    const ptr32<int16> elevation = frame.local<int16>(0);
    TEditGetTrackUnitInstanceNum(xPos, yPos, instanceNum, elevation);
    for (int32_t i = static_cast<int8_t>(*instanceNum - 1); i >= 0; --i)
    {
        int16_t e = elevation[i];
        if (e < 0)
            e = static_cast<int16_t>(-e);
        if (e < 0x600)
            return 1;
    }
    return 0;
}

// The track's description and module list from the permanent save buffer: the first module is taken whatever it is,
// the rest up to the -1 that ends the list.
void TEditCreateTrackDescFromPermSaveBuffer()
{
    GuestFrame frame(0x80);
    sTEditClass &t = te();
    const uint32_t handle = frame.local<uint32_t>(0).address;
    uint32_t in = t.pSavedTrackData.address;
    std::memcpy(guest_host(kTE + 0x20u), guest_host(in), 0x18u);
    t.trackDesc.pTrackModuleList = nullptr;
    in += 0x18u;
    uint32_t id = at<uint32_t>(in);
    do
    {
        at<uint32_t>(handle) = 0;
        allocLo(handle, 8);
        const uint32_t node = at<uint32_t>(handle);
        at<uint32_t>(node + 4u) = id;
        at<uint32_t>(node) = 0;
        appendNode(ptr32<uint32_t>(kTE + 0x20u), node);
        in += 4u;
        id = at<uint32_t>(in);
    } while (id != 0xffffffffu);
}

} // namespace rc

RC_DECOMP_FN_SETUP(TERInit, rc::fuzz::teditPlain)
RC_DECOMP_FN_SETUP(TEditLevelInit, rc::fuzz::teditPlain)
RC_DECOMP_FN_SETUP(TEditLevelTerm, rc::fuzz::teditPlain)
RC_DECOMP_FN_SETUP(TEditSetupTELvLClass, rc::fuzz::teditLevelData)
RC_DECOMP_FN_BLOCKING_SETUP(TEditInit, rc::fuzz::teditWalk)
RC_DECOMP_FN_SETUP_HEAP(TEditAllocPermTrackStructures, rc::fuzz::teditHeap)
RC_DECOMP_FN_SETUP_HEAP(TEditFreePermTrackStructures, rc::fuzz::teditHeap)
RC_DECOMP_FN_BLOCKING_SETUP(TEditTerm, rc::fuzz::teditPlain) // a test race tail-jumps into LoadingInit (the movie, the music RPC)
RC_DECOMP_FN_SETUP(TEditControl, rc::fuzz::teditGridControl)
RC_DECOMP_FN_BLOCKING_SETUP_HEAP(TEditDisplay, rc::fuzz::teditDisplay) // TEditDisplayMainMenus reaches LoadSaveProcess (libmc)
RC_DECOMP_FN_SETUP_HEAP(TEditInitialiseCursor, rc::fuzz::teditHeap)
RC_DECOMP_FN_SETUP_HEAP(TEditCreateTrack, rc::fuzz::teditHeap)
RC_DECOMP_FN_SETUP_HEAP(TEditFreeTrack, rc::fuzz::teditHeap)
RC_DECOMP_FN_SETUP(TEditControlEditing, rc::fuzz::teditKeys)
RC_DECOMP_FN_SETUP(TEditInitPermTrackStructures, rc::fuzz::teditPlain)
RC_DECOMP_FN_SETUP(TEditSetupTCLvLClass, rc::fuzz::teditLevelData)
RC_DECOMP_FN_SETUP(TEditSetupTALvLClass, rc::fuzz::teditLevelData)
RC_DECOMP_FN_SETUP(TEditFreeTrackALF, rc::fuzz::teditPlain)
RC_DECOMP_FN_SETUP(TEditInsertTrackDescToSaveBuffer, rc::fuzz::teditPlain)
RC_DECOMP_FN_SETUP(TEditExtractTrackDescFromSaveBuffer, rc::fuzz::teditPlain)
RC_DECOMP_FN_SETUP(TEditGetThemeTypeForTrack, rc::fuzz::teditPlain)
RC_DECOMP_FN_SETUP(TEditGetSoundTrackNdxForTrack, rc::fuzz::teditPlain)
RC_DECOMP_FN_SETUP(TEditGetBestLapTimeFromPermSaveBuffer, rc::fuzz::teditPlain)
RC_DECOMP_FN_SETUP(TEditSetBestLapTimeToPermSaveBuffer, rc::fuzz::teditPlain)
RC_DECOMP_FN_SETUP(TEditSetTileListForThemeType, rc::fuzz::teditPlain)
RC_DECOMP_FN_SETUP(TEditClearTrack, rc::fuzz::teditPlain)
RC_DECOMP_FN_SETUP(TEditGetTrackUnitInstanceNum, rc::fuzz::teditUnitCount)
RC_DECOMP_FN_SETUP(TEditSetTrackUnit, rc::fuzz::teditSetUnit)
RC_DECOMP_FN_SETUP(TEditCheckDeActivateFarView, rc::fuzz::teditPlain)
RC_DECOMP_FN_SETUP(TEditCheckRotateCamera, rc::fuzz::teditPlain)
RC_DECOMP_FN(TEditProcessEditMenuSpecialKeys)
RC_DECOMP_FN_SETUP(GetEditMenuModuleIndex, rc::fuzz::teditSlopeModule)
RC_DECOMP_FN_SETUP(AddEditModuleToCursorModuleList, rc::fuzz::teditPlain)
RC_DECOMP_FN_SETUP(ClearCursorModuleList, rc::fuzz::teditPlain)
RC_DECOMP_FN_SETUP(ClearTrackModuleList, rc::fuzz::teditPlain)
RC_DECOMP_FN_SETUP(AddTrackModuleToTrackModuleList, rc::fuzz::teditSlopeModule)
RC_DECOMP_FN_SETUP(GenerateTrackFromTrackModuleList, rc::fuzz::teditWalk)
RC_DECOMP_FN_SETUP(TEditResetResourceMeter, rc::fuzz::teditPlain)
RC_DECOMP_FN_SETUP(TEditUpdateResourceMeter, rc::fuzz::teditMeter)
RC_DECOMP_FN_SETUP(TEditGetSlopeNdxFromTrueSlope, rc::fuzz::teditSlopeModule)
RC_DECOMP_FN_SETUP(TEditGetEditModuleOutputSlope, rc::fuzz::teditSlopeModule)
RC_DECOMP_FN_SETUP(TEditGetEditModuleInputSlope, rc::fuzz::teditMenuCell)
RC_DECOMP_FN_SETUP(TEditCheckForFlatInOutSlopedCorners, rc::fuzz::teditSlopeModule)
RC_DECOMP_FN_SETUP(TEditRestrictSpecial, rc::fuzz::teditRestrict)
RC_DECOMP_FN_SETUP(TEditRestrictLeftTurn, rc::fuzz::teditRestrict)
RC_DECOMP_FN_SETUP(TEditRestrictRightTurn, rc::fuzz::teditRestrict)
RC_DECOMP_FN_SETUP(TEditRestrictStraight, rc::fuzz::teditRestrict)
RC_DECOMP_FN_SETUP(TEditRestrictSlopeChange, rc::fuzz::teditRestrict)
RC_DECOMP_FN_SETUP(TEditRestrictSlopeToSlopeChange, rc::fuzz::teditSlopeModule)
RC_DECOMP_FN_SETUP(TEditClearAllRestricts, rc::fuzz::teditPlain)
RC_DECOMP_FN_SETUP(TEditCheckForCompleteTrack, rc::fuzz::teditPlain)
RC_DECOMP_FN_SETUP(TEditSaveTrackDescToPermSaveBuffer, rc::fuzz::teditPlain)
RC_DECOMP_FN_SETUP(TEditGenerateTrackFromPermSaveBuffer, rc::fuzz::teditWalk)
RC_DECOMP_FN_SETUP(TEditGetTileSprPtr, rc::fuzz::teditSlopeModule)
RC_DECOMP_FN_SETUP(TEditGenerateTrackAINodesFinishDist, rc::fuzz::teditAINodes)
RC_DECOMP_FN_SETUP(TEditAutoCompIsStartGridBlocked, rc::fuzz::teditGridBlocked)
RC_DECOMP_FN_SETUP(TEditCreateTrackDescFromPermSaveBuffer, rc::fuzz::teditPlain)

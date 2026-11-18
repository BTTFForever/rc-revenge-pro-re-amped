// Shared by the TEDIT.c sources (tedit.cpp: the editor's state, track grid, module lists, restrictions and save
// buffer; tedit_autocomp.cpp: auto-complete and random tracks): the unit's functions, guest addresses and helpers.
//
// The Track Editor (tEditVarG, sTEditClass) keeps the track as a list of edit module ids (trackDesc.pTrackModuleList,
// heap nodes of 8 bytes) from a start position, and the grid it builds from them: 16 x 16 track units in three layers
// (track.pTrackUnits, 0x300 units of 8 bytes: tile id, elevation, orientation; unit x + 16 y + 256 layer). The cursor
// (tEditVarG.cursor) is the end of the track with the pieces about to be placed (a list of edit module descriptors).
// Each editModuleListG entry names an edit module in the level's resource (pEditModuleList: size, direction change,
// height change, input and output slopes, the units it covers and their tiles) and flag 1 (mirrored: slopes swapped and
// negated). The edit menu is a grid of five slopes by seven modules (editMenuGridSlopes) with per-slope restrictions.
#pragma once

#include "rc_decomp.h"

#include <cstring>

namespace rc {

// TEDIT.c.
void TERInit();
void TEditLevelInit();
void TEditLevelTerm();
void TEditSetupTELvLClass(ptr32<_sTELVLData> pData);
void TEditInit();
void TEditAllocPermTrackStructures();
void TEditFreePermTrackStructures();
void TEditTerm();
void TEditControl(tTime time);
void TEditDisplay(tTime time, uint32 frameCnt);
void TEditInitialiseCursor();
void TEditCreateTrack();
void TEditFreeTrack();
void TEditControlEditing(ptr32<_sTrackInput> pInput, tTime time);
void TEditInitPermTrackStructures();
void TEditSetupTCLvLClass(ptr32<_sTCLVLData> pData);
void TEditSetupTALvLClass(ptr32<_sTALVLData> pData);
void TEditFreeTrackALF();
void TEditInsertTrackDescToSaveBuffer();
bool_ TEditExtractTrackDescFromSaveBuffer();
uint32 TEditGetThemeTypeForTrack();
uint32 TEditGetSoundTrackNdxForTrack();
uint32 TEditGetBestLapTimeFromPermSaveBuffer();
void TEditSetBestLapTimeToPermSaveBuffer(uint32 bestLapTime);
void TEditSetTileListForThemeType(ptr32<void> pTileList, uint32 themeType);
void TEditClearTrack();
void TEditGetTrackUnitInstanceNum(int16 xPos, int16 yPos, ptr32<uint8> instanceNum, ptr32<int16> unitElevation);
void TEditSetTrackUnit(uint16 xPos, uint16 yPos, ptr32<_sTrackUnit> pTrackUnitData, uint8 instanceNum);
bool_ TEditCheckDeActivateFarView(ptr32<_sTrackInput> pInput);
void TEditCheckRotateCamera(ptr32<_sTrackInput> pInput);
void TEditProcessEditMenuSpecialKeys(ptr32<_sTrackInput> pInput, tTime time);
uint8 GetEditMenuModuleIndex(eSlope currentSlope, uint32 editModuleId);
void AddEditModuleToCursorModuleList(ptr32<_sEditModuleDesc> pEditModuleDesc);
void ClearCursorModuleList();
void ClearTrackModuleList();
void AddTrackModuleToTrackModuleList(int32 editModuleId, bool_ setUnsavedFlag);
void GenerateTrackFromTrackModuleList();
void TEditResetResourceMeter();
void TEditUpdateResourceMeter(ptr32<uint32> pResourceCounter, int16 tileId);
uint8 TEditGetSlopeNdxFromTrueSlope(eSlope slope);
eSlope TEditGetEditModuleOutputSlope(uint32 editModuleId);
eSlope TEditGetEditModuleInputSlope(uint8 slopeNdx, uint8 moduleNdx);
bool_ TEditCheckForFlatInOutSlopedCorners(uint8 editModuleId, ptr32<eSlope> outputSlope);
void TEditRestrictSpecial(uint32 restrictSpecial);
void TEditRestrictLeftTurn(int32 restrictLeft, int32 restrictSlope);
void TEditRestrictRightTurn(int32 restrictRight, int32 restrictSlope);
void TEditRestrictStraight(int32 restrictSlope);
void TEditRestrictSlopeChange(int32 restrictSlope);
void TEditRestrictSlopeToSlopeChange(eSlope currentSlope);
void TEditClearAllRestricts();
bool_ TEditCheckForCompleteTrack();
void TEditSaveTrackDescToPermSaveBuffer();
void TEditGenerateTrackFromPermSaveBuffer();
ptr32<uint8> TEditGetTileSprPtr(uint8 editModuleId);
void TEditGenerateTrackAINodesFinishDist();
bool_ TEditAutoCompIsStartGridBlocked(int16 xPos, int16 yPos);
void TEditCreateTrackDescFromPermSaveBuffer();
void TEditControlPlaceStartGrid(ptr32<_sTrackInput> pInput, tTime time);
void TEditControlRotateStartGrid(ptr32<_sTrackInput> pInput, tTime time);
void TEditFreeTrackAINodes();
void TEditRenderSkyDome();
void TEditControlMenu(tTime time);
void TEditDisplayHelpScreen(tTime time);
void TEditConstructStartGridCameraPaths();
void TEditCopyAINodePosToCameraNode(ptr32<_sSingleCamera> pCam, ptr32<ptr32<_sAINode>> ppAINode);
void TEditCheckCursorDirectionConstraints();
void TEditCheckCursorMinMaxHeightConstraints();
void TEditSetCursorDirectionConstraints(uint8 xPos, uint8 yPos);
void TEditRenderCursor();
void TEditSetNewCursorModule();
void TEditDisplay2DGridMap();
void TEditRender2DHudMapModule(int16 xPos, int16 yPos, int8 tileId, int16 elevation, uint8 tileOrientation);
void TEditRender2DModule(int16 xPos, int16 yPos, int8 tileId, int16 elevation, uint8 tileOrientation);
bool_ TEditProcessEditMenuModuleSelection(uint32 input);
bool_ TEditProcessEditMenuSpecialSelection(uint32 input);
bool_ TEditProcessEditMenuSlopeSelection(uint32 input);
void TEditGetEditMenuAvailSlopeAndModule();
void TEditGetEditMenuAvailModuleForSlope();
void TEditPlaceModule(int16 xPos, int16 yPos, int16 tileId, int16 elevation, eDirection tileOrientation);
uint32 DeleteTrackModuleFromTrackModuleList();
bool_ TEditCheckForEditWarnings();
void TEditInitialiseEditMenu();
void TEditRenderResourceMeter();
void TEditDisplay2DGridLines();
ptr32<_sTrackInput> TEditGetInput(tTime time);
void TEditControlGridEdit(tTime time);
void TEditDisplayGridEdit(tTime time);
void TEditCameraFollowCursor();
int16 TEditGetAutoCompPieceScore();
void TEditGetAutoCompAllPiecesScore(ptr32<_sAutoCompStackData> pAutoCompPiece);
void TEditAutoCompRemoveLastPiece(ptr32<_sAutoCompStackData> pAutoCompPiece);
void TEditAutoCompPlacePiece(ptr32<_sAutoCompStackData> pAutoCompPiece);
int32 TEditMenuDoCompleteTrack();
void TEditRandomTrackAssignStartGrid();
void TEditRandomTrackPlaceSpecialPiece(ptr32<_sAutoCompStackData> pAutoCompPiece);
bool_ TEditRandomTrackGenerate();
void TEditMenuDoRandomTrack();
void TEditDisplayMainMenus(tTime time, uint32 frameCnt);
void TEditDEBUGDisplayCursor();
void TEditRenderTrack();
void TEditProcessEditMenuKeys(ptr32<_sTrackInput> pInput, tTime time);
void TEditProcessPlaceStartGrid(ptr32<_sTrackInput> pInput, tTime time);
void TEditProcessRotateStartGrid(ptr32<_sTrackInput> pInput, tTime time);
void TEditDisplay3DGridLines();
void TEditRenderEditMenu(tTime time);
eSlope TEditAdjustCursorModule(ptr32<_sCursorModule> pCursorModule, ptr32<int16> xPos, ptr32<int16> yPos, ptr32<eDirection> direction,
                               ptr32<int16> elevation, uint8 flag);
void TEditRenderTileToHudMap(int16 x, int16 y, int16 xOffset, int16 yOffset, uint32 flags);
void TEditBuildTransitionCursorModules(eSlope currentSlope, eSlope desiredSlope);
void TEditGeneratePreTColData(ptr32<uint16> pModIds, ptr32<void> pOff, ptr32<uint16> pRFlags, ptr32<uint16> pMFlags);
void TEditGenerateTrackAINodes();
void TEditGenerateTCol();
void TEditUpdateStartGridPositions();
void TEditUpdatePickupNumAndPositions();
void TEditConvertTrackToALF();
void TEditRenderGameTrack();
void TEditGenerateMemCardIconFromLevel(ptr32<int32> pBitMap);
void TEditGenerateHudMapForTrackLevel();
void TEditCopyHudMapForTrackLevelToVRAM();

namespace tedit {

template <typename T>
T &at(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

inline sTEditClass &te() { return G(tEditVarG); }
constexpr uint32_t kTE = sym::tEditVarG;
constexpr uint32_t kEditModuleList = sym::editModuleListG; // sEditModuleDesc[50]
constexpr uint32_t kSlopeLookUp = sym::curvedata_541 + 0x5b0u;  // the edit menu's slopes from row 0: 2, 1, 0, -1, -2
constexpr uint32_t kUnsavedLapTime = 0x9275cu;               // the best lap time of a new or changed track
constexpr uint32_t kGridSlopes = kTE + 0xfcu;                // editMenuGridSlopes, 0x2c bytes each

// The resource types the editor's levels load (ResTypeSetConstructDaemon names, in .sdata).
constexpr uint32_t kResTRM = sym::tileSpriteArraySpecial12G + 0x8u, kResTRS = sym::tileSpriteArraySpecial12G + 0x10u;
constexpr uint32_t kResICO = sym::tileSpriteArraySpecial12G + 0x18u, kResLSD = sym::tileSpriteArraySpecial12G + 0x20u;
constexpr uint32_t kResTSKY = sym::tileSpriteArraySpecial12G + 0x28u, kResRLST = sym::tileSpriteArraySpecial12G + 0x30u;
constexpr uint32_t kRots = sym::tileSpriteArraySpecial12G + 0x38u; // sprite flags per orientation: 0, 2, 4, 8

// A unit of the grid, as the original indexes it (16-bit index, then 8 bytes each).
inline uint32_t unitAddress(int32_t index)
{
    return te().track.pTrackUnits.address + static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(index)) * 8);
}

// The edit module an editModuleListG entry names.
inline uint32_t editModuleDesc(uint32_t id) { return kEditModuleList + id * 4u; }
inline uint32_t editModule(uint32_t moduleId) { return te().pEditModuleList.address + moduleId * 0x18u; }

// GenerateTrackFromTrackModuleList with the $a3 its caller leaves (the cursor's slope when the list is empty).
void generateTrackFromList(uint32_t a3);
// TEditGetEditMenuAvailSlopeAndModule with the $a1 its caller leaves (the module when every slope is restricted).
void availSlopeAndModule(uint32_t a1);
// TEditProcessEditMenuKeys and TEditControlEditing (which ends by jumping to it) with the $s5 their caller leaves: the
// cursor's slope when key 4 finds no pieces on the cursor (TEditControlGridEdit holds tEditVarG's upper half there).
void processEditMenuKeys(ptr32<_sTrackInput> pInput, tTime time, uint32_t s5);
void controlEditing(ptr32<_sTrackInput> pInput, tTime time, uint32_t s5);
// TEditMenuDoCompleteTrack with the $s5 its caller leaves (the camera's time if the search ends at its first step).
int32 completeTrack(uint32_t s5);

} // namespace tedit
} // namespace rc

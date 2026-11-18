// Shared by CARD.c (card.cpp) and LOADSAVE.c (loadsave.cpp, loadsave_process.cpp): the memory card layer over the
// SDK's libmc (sceMc*, outside calls through guest_call so the oracle's setups can replace them), the save/load state
// machine's globals, and small helpers.
#pragma once

#include "rc_decomp.h"

#include <cstring>

namespace rc {

// CARD.c.
eCardStatus CardGetStatusNow(eCard card);
bool_ CardGetStatusASync(eCard card, uint32 mode, ptr32<eCardStatus> cardStatus);
void CardCreateIconHeader(ptr32<anon_88ca8770> pIcon, ptr32<sceVu0IVECTOR> pBgColor, ptr32<sceVu0FVECTOR> pLightDir,
                          ptr32<sceVu0FVECTOR> pLightCol, ptr32<sceVu0FVECTOR> pAmbient, ptr32<char> title,
                          ptr32<ptr32<char>> iconName, uint32 bgTrans); // lineBreakPos: the ninth, on the stack
int32 CardReadData(eCard card, ptr32<char> fileName, uint32 numBytes, ptr32<uint8> pData);
void CardConstruct();
void CardInit();
void CardUnInit();
bool_ CardWaitASyncFuncResult(ptr32<bool_> result);
bool_ CardWaitASyncWriteResult(ptr32<bool_> result);
bool_ CardWaitASyncOpenResult(ptr32<bool_> result);
bool_ CardWaitASyncExistResult(ptr32<bool_> result);
void CardWaitASyncFuncTerm();
int32 CardFileRename(eCard card, ptr32<char> oldName, ptr32<char> newName);
int32 CardFormat(eCard card);
int32 CardUnFormatNow(eCard card);
int32 CardFileExists(eCard card, ptr32<char> fileName);
int32 CardFileExistsNow(eCard card, ptr32<char> fileName);
int32 CardFileErase(eCard card, ptr32<char> fileName);
int32 CardCreateData(eCard card, ptr32<char> fileName, uint32 numBytes, ptr32<uint8> pData);
int32 CardOpenFile(eCard card, ptr32<char> fileName, uint32 flags);
int32 CardCloseFile();
int32 CardWriteData(eCard card, ptr32<char> fileName, uint32 numBytes, ptr32<uint8> pData);
void CardMarkAllRefreshNeeded();
eCard CardPresent(eCard card, int32_t direction);
int32 CardCreateDirectory(eCard card, ptr32<char> dirName);
int32 CardChangeDirectory(eCard card, ptr32<char> path, ptr32<char> pwd);
int32 CardChangeDirectoryNow(eCard card, ptr32<char> path, ptr32<char> pwd);
int32 CardGetDirectoryNow(eCard card, ptr32<char> name, int32 maxEnt, ptr32<anon_57bcf673> dirTable);
int32 Card2Channel(eCard card);

// LOADSAVE.c.
bool_ LoadSaveCreateGameSaveDirAndIcon(eCard card, ptr32<char> dirName);
bool_ LoadSaveCreateTrackSaveDirAndIcon(eCard card, ptr32<char> dirName);
uint32 LoadSaveGetRevoltTrackSaveNames(eCard card);
uint32 LoadSaveGetRevoltTrackLoadNames(eCard card);
void LoadSaveGenerateDefaultTrackSaveName(eCard card, ptr32<char> saveName, bool_ overwriteExistingSave);
bool_ LoadSaveCheckForUniqueTrackSaveName(eCard card, ptr32<char> saveName, bool_ overwriteExistingSave);
void LoadSaveExtractGameSaveData();
void LoadSaveInsertGameSaveData();
void LoadSaveMergeGameDataWithLoadedData();
void LoadSaveMergeSavedDataWithGameData();
void LoadSaveInit(eLoadSaveMode mode);
bool_ LoadSaveAutoLoadSavedGame();
eLoadSaveFreeSpace LoadSaveCheckFreeSpaceOnCards();
eLoadSaveFreeSpace LoadSaveKeepCheckingFreeSpaceOnCards(uint32 frameCnt);
int32 LoadSaveProcess(uint32 frameCnt);
void RenderMemoryCards();
void LoadSaveCheckModeAvailable();
bool_ LoadSaveWriteFile(eCard card, ptr32<char> fileName, uint32 numBytes, ptr32<uint8> pData, ptr32<bool_> result);
void LoadSaveSetupClass(ptr32<_sLoadSaveData> pLoadSaveData);
bool_ LoadSaveCheckIfCardInSlot1();
bool_ LoadSaveFileExists(eCard card, ptr32<char> fileName, ptr32<bool_> result, ptr32<bool_> exists);
bool_ LoadSaveFileErase(eCard card, ptr32<char> fileName, ptr32<bool_> result);
bool_ LoadSaveFileRename(eCard card, ptr32<char> oldName, ptr32<char> newName, ptr32<bool_> result);

namespace card {

template <typename T>
T &at(uint32_t address) { return *static_cast<T *>(guest_host(address)); }

// libmc, through the function table (the runtime's sceMc* in the game, the setups' fakes offline).
template <typename... A>
int32_t mc(uint32_t function, A... args) { return guest_call<int32_t>(function, args...); }

// cardVarG (CARD.c's sCardClass, 0x58c0 bytes): one 0x1b40-byte directory per card from 0x400.
constexpr uint32_t kCardVars = sym::cardVarG;
constexpr uint32_t kDescriptor = kCardVars + 0x5880u, kBytesWritten = kCardVars + 0x5884u, kIcon = kCardVars + 0x10u;
// The address arithmetic is the original's (32-bit, any card number).
inline uint32_t dir(int32_t card) { return kCardVars + 0x400u + static_cast<uint32_t>(card) * 0x1b40u; }
inline int32_t &status(int32_t card) { return at<int32_t>(dir(card)); }
inline int32_t &refreshNeeded(int32_t card) { return at<int32_t>(dir(card) + 4u); }
inline int32_t &freeClusters(int32_t card) { return at<int32_t>(dir(card) + 8u); }
inline uint32_t dirTable(int32_t card) { return dir(card) + 0x240u; }          // sceMcTblGetDir[99], 0x40 bytes each
inline uint32_t entryName(int32_t card, int32_t i) { return dirTable(card) + static_cast<uint32_t>(i) * 0x40u + 0x20u; }
inline int32_t &entriesFound(int32_t card) { return at<int32_t>(dir(card) + 0x1b00u); }

// loadSaveVarG (sLoadSaveClass).
inline sLoadSaveClass &ls() { return G(loadSaveVarG); }
constexpr uint32_t kLoadSave = sym::loadSaveVarG;
inline ptr32<sAMenuHeader> msg(int i) { return ls().loadSaveData.pLoadSaveMsgs[i]; }
inline ptr32<sMenuSystem> menuSystem() { return G(pMenuSystemG); }

inline char *str(uint32_t address) { return static_cast<char *>(guest_host(address)); }

} // namespace card
} // namespace rc

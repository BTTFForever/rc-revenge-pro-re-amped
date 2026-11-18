// Decompiled from LOADSAVE.c (all but the state machine, loadsave_process.cpp): saving and loading the game's options
// and progress (a 0x2000-byte file in the BASLUS-20153G directory: a checksum at 0x200 over the 0x130-byte game state
// at 0x204) and Track Editor tracks (BASLUS-20153T<name>), each directory with an icon.sys and its icons. The save and
// load screens are steps of LoadSaveProcess, which starts one card command per frame and polls for its end: the
// LoadSaveFile* and LoadSaveWriteFile steps keep their position in loadSaveVarG.subState.
//
// Leftover registers: when subState is not one of their steps, LoadSaveFileExists and LoadSaveFileRename return their
// caller's $t0, LoadSaveFileErase its $a3 and LoadSaveWriteFile its $t1 (the original never sets them). The registered
// functions read them from the running context; LoadSaveProcess passes the values its original holds there.

#include "card.h"
#include "card_fuzz.h"

namespace rc {

using namespace card;

namespace {

constexpr uint32_t kSlash = sym::bgTrans2G + 4u;          // "/"
constexpr uint32_t kIconSys = sym::levelLoadG + 0x2e0u;    // "icon.sys"
constexpr uint32_t kTrack = sym::bgTrans2G + 0xcu;         // "Track"
constexpr uint32_t kTrackFormat = sym::bgTrans2G + 0x14u;  // "Track%d"

uint32_t reg(int index) { return getRegU32(&g_runtime->eeScheduler().runningContext(), index); }

// The original builds "/" by copying the string's two bytes (the slash and its zero) into the buffer.
void startPath(uint32_t buffer) { at<uint16_t>(buffer) = at<uint16_t>(kSlash); }

// "/<dir>/<dir>": the file a save directory is named after.
void savePath(uint32_t buffer, uint32_t dir)
{
    startPath(buffer);
    std::strcat(str(buffer), str(dir));
    std::strcat(str(buffer), str(kSlash));
    std::strcat(str(buffer), str(dir));
}

bool startsWith(uint32_t prefix, uint32_t name) { return std::strncmp(str(prefix), str(name), std::strlen(str(prefix))) == 0; }

// The icon header's ninth argument (the title's line break, 0x1e) goes at $sp + 0 of the caller's frame.
void iconHeader(uint32_t sp, bool track)
{
    at<int32_t>(sp) = 0x1e;
    if (track)
        CardCreateIconHeader(ptr32<anon_88ca8770>(kIcon), ptr32<sceVu0IVECTOR>(sym::bgColor2G), ptr32<sceVu0FVECTOR>(sym::lightDir2G),
                             ptr32<sceVu0FVECTOR>(sym::lightCol2G), ptr32<sceVu0FVECTOR>(sym::ambient2G), ptr32<char>(sym::revolt2TrackSaveName),
                             ptr32<ptr32<char>>(sym::iconName2G), G(bgTrans2G));
    else
        CardCreateIconHeader(ptr32<anon_88ca8770>(kIcon), ptr32<sceVu0IVECTOR>(sym::bgColor1G), ptr32<sceVu0FVECTOR>(sym::lightDir1G),
                             ptr32<sceVu0FVECTOR>(sym::lightCol1G), ptr32<sceVu0FVECTOR>(sym::ambient1G), ptr32<char>(sym::revolt2GameSaveName),
                             ptr32<ptr32<char>>(sym::iconName1G), G(bgTrans1G));
}

// LoadSaveCreateGameSaveDirAndIcon and LoadSaveCreateTrackSaveDirAndIcon (3 or 1 icons): a new directory gets the
// header and icons; an existing one has each file rewritten that is missing or does not read back (a 100000-byte
// buffer from the heap to read into). Once one icon needed writing, every later one is written too.
bool_ createDirAndIcon(eCard card, ptr32<char> dirName, bool track)
{
    GuestFrame frame(0x100u);
    const uint32_t sp = frame.local<uint8_t>(0).address;
    const uint32_t directoryName = sp + 0x10u, rwBuffer = sp + 0x50u;
    const uint32_t names = track ? sym::iconName2G : sym::iconName1G, sizes = track ? sym::iconSize2G : sym::iconSize1G;
    const uint32_t icons = kLoadSave + (track ? 0x10cu : 0x100u);
    const int iconCount = track ? 1 : 3;
    startPath(directoryName);
    at<uint32_t>(rwBuffer) = 0;
    std::strcat(str(directoryName), dirName.get());
    if (CardChangeDirectory(card, dirName, nullptr) == 0)
    {
        if (CardCreateDirectory(card, ptr32<char>(directoryName)) == 0 || CardChangeDirectory(card, dirName, nullptr) == 0)
            return 0;
        iconHeader(sp, track);
        if (CardCreateData(card, ptr32<char>(kIconSys), 0x3c4u, ptr32<uint8>(kIcon)) == 0)
            return 0;
        for (int i = 0; i < iconCount; ++i)
            if (CardCreateData(card, ptr32<char>(at<uint32_t>(names + 4u * i)), at<uint32_t>(sizes + 4u * i),
                               ptr32<uint8>(at<uint32_t>(icons + 4u * i))) == 0)
                return 0;
        return 1;
    }
    at<uint32_t>(sp + 0x54u) = rwBuffer;
    guest_call<int32_t>(sym::MemBlockAllocLo, rwBuffer, 100000);
    bool failed = false;
    bool rewrite = CardFileExists(card, ptr32<char>(kIconSys)) == 0 ||
                   CardReadData(card, ptr32<char>(kIconSys), 0x3c4u, ptr32<uint8>(at<uint32_t>(rwBuffer))) == 0;
    if (rewrite)
    {
        iconHeader(sp, track);
        failed = CardCreateData(card, ptr32<char>(kIconSys), 0x3c4u, ptr32<uint8>(kIcon)) == 0;
    }
    if (failed)
    {
        guest_call<void>(sym::MemBlockFree, rwBuffer);
        return 0;
    }
    rewrite = false;
    for (int i = 0; i < iconCount && !failed; ++i)
    {
        const ptr32<char> name(at<uint32_t>(names + 4u * i));
        const uint32_t size = at<uint32_t>(sizes + 4u * i);
        if (CardFileExists(card, name) == 0 || CardReadData(card, name, size, ptr32<uint8>(at<uint32_t>(rwBuffer))) == 0)
            rewrite = true;
        if (rewrite && CardCreateData(card, name, size, ptr32<uint8>(at<uint32_t>(icons + 4u * i))) == 0)
            failed = true;
    }
    guest_call<void>(sym::MemBlockFree, rwBuffer);
    return failed ? 0 : 1;
}

// LoadSaveGetRevoltTrackSaveNames and ...LoadNames: the names of the tracks on the card whose save file opens (an
// entry whose file does not open loses its name's first byte), into trackSaveNamesG.
uint32 trackNames(eCard card, bool forSave)
{
    GuestFrame frame(0xe0u);
    const uint32_t fileName = frame.local<uint8_t>(0).address;
    uint32 count = 0;
    for (int32_t i = 0; i < entriesFound(card); ++i)
    {
        const uint32_t name = entryName(card, i);
        if (!startsWith(sym::revolt2TrackId, name))
            continue;
        savePath(fileName, name);
        if (CardFileExistsNow(card, ptr32<char>(fileName)) == 0)
            at<char>(name) = 0;
        else
        {
            const uint32_t length = static_cast<uint32_t>(std::strlen(str(sym::revolt2TrackId)));
            std::strcpy(str(sym::trackSaveNamesG + 0x10u * count++), str(name + length));
        }
    }
    // Saving: a "new track" entry if there is room for one more (75 clusters).
    if (forSave && static_cast<int32_t>(count) < 0xf && freeClusters(card) >= 0x4b)
    {
        const uint32_t to = sym::trackSaveNamesG + 0x10u * count++;
        std::strcpy(str(to), guest_call<ptr32<char>>(sym::GetGeneralString, 0xb).get());
    }
    return count;
}

} // namespace

namespace loadsave {

// LoadSaveFileExists' steps: 6 starts opening the file, 7 waits (no file: result true, exists false), 4 closes it, 5
// waits (exists true). A step that could not start sets result false and returns true.
bool_ fileExists(eCard card, ptr32<char> fileName, ptr32<bool_> result, ptr32<bool_> exists, uint32_t leftover)
{
    GuestFrame frame(0x40u);
    sLoadSaveClass &l = ls();
    switch (l.subState)
    {
    case 4:
        if (CardCloseFile() == 0)
            break;
        l.subState = 5;
        return 0;
    case 5:
        if (CardWaitASyncFuncResult(result) == 0)
            return 0;
        l.subState = 0xc;
        *exists = 1;
        return 1;
    case 6:
        if (CardFileExists(card, fileName) == 0)
            break;
        l.subState = 7;
        return 0;
    case 7:
        if (CardWaitASyncExistResult(result) == 0)
            return 0;
        if (*result != 1)
        {
            *result = 1;
            *exists = 0;
            return 1;
        }
        l.subState = 4;
        return 0;
    default:
        return static_cast<bool_>(leftover);
    }
    *result = 0;
    return 1;
}

// 8 starts deleting, 9 waits.
bool_ fileErase(eCard card, ptr32<char> fileName, ptr32<bool_> result, uint32_t leftover)
{
    GuestFrame frame(0x40u);
    sLoadSaveClass &l = ls();
    if (l.subState == 9)
    {
        if (CardWaitASyncFuncResult(result) == 0)
            return 0;
        l.subState = 0xc;
        return 1;
    }
    if (l.subState != 8)
        return static_cast<bool_>(leftover);
    if (CardFileErase(card, fileName) == 0)
    {
        *result = 0;
        return 1;
    }
    l.subState = 9;
    return 0;
}

// 10 starts renaming, 11 waits.
bool_ fileRename(eCard card, ptr32<char> oldName, ptr32<char> newName, ptr32<bool_> result, uint32_t leftover)
{
    GuestFrame frame(0x30u);
    sLoadSaveClass &l = ls();
    if (l.subState == 10)
    {
        if (CardFileRename(card, oldName, newName) == 0)
        {
            *result = 0;
            return 1;
        }
        l.subState = 0xb;
        return 0;
    }
    if (l.subState != 0xb)
        return static_cast<bool_>(leftover);
    if (CardWaitASyncFuncResult(result) == 0)
        return 0;
    l.subState = 0xc;
    return 1;
}

// 0 opens the file (created, truncated), 1 waits, 2 starts the write, 3 waits (every byte or result false), 4 closes,
// 5 waits.
bool_ writeFile(eCard card, ptr32<char> fileName, uint32 numBytes, ptr32<uint8> pData, ptr32<bool_> result, uint32_t leftover)
{
    GuestFrame frame(0x50u);
    sLoadSaveClass &l = ls();
    switch (l.subState)
    {
    case 0:
        if (CardOpenFile(card, fileName, 0x203u) == 0)
            break;
        l.subState = 1;
        return 0;
    case 1:
        if (CardWaitASyncOpenResult(result) == 0)
            return 0;
        if (*result != 1)
            return 1;
        l.subState = 2;
        return 0;
    case 2:
        if (CardCreateData(card, fileName, numBytes, pData) == 0)
            break;
        l.subState = 3;
        return 0;
    case 3:
        if (CardWaitASyncWriteResult(result) == 0)
            return 0;
        if (*result != 1 || at<uint32_t>(kBytesWritten) != numBytes)
        {
            *result = 0;
            return 1;
        }
        l.subState = 4;
        return 0;
    case 4:
        if (CardCloseFile() == 0)
            break;
        l.subState = 5;
        return 0;
    case 5:
        if (CardWaitASyncFuncResult(result) == 0)
            return 0;
        l.subState = 0xc;
        return 1;
    default:
        return static_cast<bool_>(leftover);
    }
    *result = 0;
    return 1;
}

} // namespace loadsave

bool_ LoadSaveCreateGameSaveDirAndIcon(eCard card, ptr32<char> dirName) { return createDirAndIcon(card, dirName, false); }

bool_ LoadSaveCreateTrackSaveDirAndIcon(eCard card, ptr32<char> dirName) { return createDirAndIcon(card, dirName, true); }

uint32 LoadSaveGetRevoltTrackSaveNames(eCard card) { return trackNames(card, true); }

uint32 LoadSaveGetRevoltTrackLoadNames(eCard card) { return trackNames(card, false); }

// When overwriting a "Track<n>" save, its name; else the first "Track<n>" (1 to 15) no track on the card has (or
// "Track15" when all are taken).
void LoadSaveGenerateDefaultTrackSaveName(eCard card, ptr32<char> saveName, bool_ overwriteExistingSave)
{
    GuestFrame frame(0xa0u);
    const uint32_t idLength = static_cast<uint32_t>(std::strlen(str(sym::revolt2TrackId)));
    if (overwriteExistingSave == 1)
    {
        const uint32_t old = sym::revolt2TrackSaveIdToDelete + idLength;
        if (std::strncmp(str(kTrack), str(old), 5) == 0)
        {
            std::strcpy(saveName.get(), str(old)); // a tail jump in the original
            return;
        }
    }
    for (int32_t n = 1;; ++n)
    {
        guest_call<int32_t>(sym::sprintf, saveName, kTrackFormat, n);
        bool unique = true;
        for (int32_t i = 0; i < entriesFound(card); ++i)
        {
            const uint32_t name = entryName(card, i);
            if (startsWith(sym::revolt2TrackId, name) &&
                std::strcmp(str(name + static_cast<uint32_t>(std::strlen(str(sym::revolt2TrackId)))), saveName.get()) == 0)
            {
                unique = false;
                break;
            }
        }
        if (unique || n >= 0xf)
            return;
    }
}

// Whether no track on the card has the name (trailing spaces removed); when overwriting, the save being replaced
// may have it.
bool_ LoadSaveCheckForUniqueTrackSaveName(eCard card, ptr32<char> saveName, bool_ overwriteExistingSave)
{
    GuestFrame frame(0xc0u);
    const uint32_t name = frame.local<uint8_t>(0).address; // 32 bytes
    std::strcpy(str(name), saveName.get());
    for (int32_t i = static_cast<int32_t>(std::strlen(str(name))) - 1; i >= 0 && at<char>(name + static_cast<uint32_t>(i)) == ' '; --i)
        at<char>(name + static_cast<uint32_t>(i)) = 0;
    for (int32_t i = 0; i < entriesFound(card); ++i)
    {
        const uint32_t entry = entryName(card, i);
        if (!startsWith(sym::revolt2TrackId, entry))
            continue;
        const uint32_t trackName = entry + static_cast<uint32_t>(std::strlen(str(sym::revolt2TrackId)));
        if (overwriteExistingSave == 1 && std::strcmp(str(sym::revolt2TrackSaveIdToDelete), str(entry)) == 0)
        {
            if (std::strcmp(str(trackName), str(name)) == 0)
                return 1;
        }
        else if (std::strcmp(str(trackName), str(name)) == 0)
            return 0;
    }
    return 1;
}

void LoadSaveExtractGameSaveData()
{
    sLoadSaveClass &l = ls();
    guest_call<void>(sym::MemBlockCopy32, l.pGameSaveData, l.pSaveGameBuffer.address + 0x204u, 0x130);
}

void LoadSaveInsertGameSaveData()
{
    sLoadSaveClass &l = ls();
    guest_call<void>(sym::MemBlockCopy32, l.pSaveGameBuffer.address + 0x204u, l.pGameSaveData, 0x130);
}

// The loaded game state (pGameSaveData) replaces the player names and pad settings, volumes, options and screen
// position (the display registers set from it), and the progress merges: the most cups won and the best times.
void LoadSaveMergeGameDataWithLoadedData()
{
    GuestFrame frame(0x50u);
    const uint32_t saved = ls().pGameSaveData.address, game = sym::frontEndVarG + 0x138u;
    std::memmove(guest_host(game + 4u), guest_host(saved + 4u), 0x40u);
    at<int32_t>(sym::rcmusicVarG + 0x20u) = at<int32_t>(saved + 0x44u); // userVol, oldUserVol
    at<int32_t>(sym::rcmusicVarG + 0x24u) = at<int32_t>(saved + 0x44u);
    guest_call<void>(sym::RCMusicModifyVolume, 1, 3);
    at<int32_t>(sym::revSfxVarG + 0x1144u) = at<int32_t>(saved + 0x48u);
    const uint8_t sfxVolume = at<uint8_t>(sym::revSfxVarG + 0x1144u);
    at<int32_t>(sym::revSfxVarG + 0x1148u) = at<int32_t>(saved + 0x48u);
    at<int32_t>(sym::sfxVarG + 8u) = at<int32_t>(saved + 0x4cu); // mode
    guest_call<void>(sym::SfxSetVolume, sfxVolume);
    at<int32_t>(game + 0x50u) = at<int32_t>(saved + 0x50u);
    const uint16_t adjustX = at<uint16_t>(saved + 0x12cu);
    at<uint16_t>(game + 0x12cu) = adjustX;
    at<int32_t>(game + 0x54u) = at<int32_t>(saved + 0x54u);
    const uint16_t adjustY = at<uint16_t>(saved + 0x12eu);
#if RC_VERSION_EU
    constexpr int32_t kTop = 0x38; // PAL
#else
    constexpr int32_t kTop = 0x30;
#endif
    const uint64_t x = static_cast<uint64_t>((static_cast<int16_t>(adjustX) * 3 + 0x290) & 0xfff);
    const uint64_t y = static_cast<uint64_t>((static_cast<int16_t>(adjustY) + kTop) & 0x7ff) << 12;
    const uint64_t keep = 0xffffffffff800fffull & ~0xfffull;
    at<int32_t>(game + 0x58u) = at<int32_t>(saved + 0x58u);
    for (const uint32_t display : {sym::gGfxP2_DBuff + 0x40u, sym::gGfxP2_DBuff + 0x18u})
        at<uint64_t>(display) = (at<uint64_t>(display) & keep) | x | y;
    at<int32_t>(game + 0x5cu) = at<int32_t>(saved + 0x5cu);
    at<uint16_t>(game + 0x12eu) = adjustY;
    const int32_t cups = at<int32_t>(saved + 0x124u);
    if (at<int32_t>(sym::frontEndVarG + 0x25cu) < cups)
    {
        at<int32_t>(sym::frontEndVarG + 0x25cu) = cups;
        guest_call<void>(sym::SetCupAvailability, 0);
    }
    for (uint32_t i = 0; i < 0x30u; ++i)
    {
        const int32_t time = at<int32_t>(saved + 0x64u + 4u * i);
        if (time < at<int32_t>(game + 0x64u + 4u * i))
            at<int32_t>(game + 0x64u + 4u * i) = time;
    }
}

// The other way: the game's state into pGameSaveData before saving (the most cups, the best times).
void LoadSaveMergeSavedDataWithGameData()
{
    const uint32_t saved = ls().pGameSaveData.address, game = sym::frontEndVarG + 0x138u;
    std::memmove(guest_host(saved + 4u), guest_host(game + 4u), 0x40u);
    at<int32_t>(saved + 0x44u) = at<int32_t>(sym::rcmusicVarG + 0x20u);
    at<uint16_t>(saved + 0x12cu) = at<uint16_t>(game + 0x12cu);
    at<int32_t>(saved + 0x48u) = at<int32_t>(sym::revSfxVarG + 0x1144u);
    const int32_t savedCups = at<int32_t>(saved + 0x124u);
    at<uint16_t>(saved + 0x12eu) = at<uint16_t>(game + 0x12eu);
    at<int32_t>(saved + 0x4cu) = at<int32_t>(sym::sfxVarG + 8u);
    for (uint32_t offset = 0x50u; offset <= 0x5cu; offset += 4u)
        at<int32_t>(saved + offset) = at<int32_t>(game + offset);
    const int32_t cups = at<int32_t>(sym::frontEndVarG + 0x25cu);
    if (savedCups < cups)
        at<int32_t>(saved + 0x124u) = cups;
    for (uint32_t i = 0; i < 0x30u; ++i)
    {
        const int32_t time = at<int32_t>(game + 0x64u + 4u * i);
        if (time < at<int32_t>(saved + 0x64u + 4u * i))
            at<int32_t>(saved + 0x64u + 4u * i) = time;
    }
}

void LoadSaveInit(eLoadSaveMode mode)
{
    sLoadSaveClass &l = ls();
    l.pSaveGameBuffer = nullptr;
    l.loadSaveMode = mode;
    l.card2Status = CARD_NOT_PRESENT;
    l.card2Checked = 0;
    l.cardSelected = CARD_1;
    l.card1Status = CARD_NOT_PRESENT;
    l.cardStatus = CARD_NOT_PRESENT;
    l.status = STATE_FIRST_TIME;
    l.pGameSaveData = nullptr;
    l.checkingCards = 0;
    l.card1Checked = 0;
    CardMarkAllRefreshNeeded();
}

// At boot: up to three tries to find and read a saved game on card 1; a good one (its checksum matches) is merged
// into the game. A card with no save ends the tries at once.
bool_ LoadSaveAutoLoadSavedGame()
{
    GuestFrame frame(0x130u);
    const uint32_t sp = frame.local<uint8_t>(0).address;
    const uint32_t gameLoaded = sp + 0x80u, fileName = sp + 0x40u;
    sLoadSaveClass &l = ls();
    l.cardSelected = CARD_1;
    at<int32_t>(gameLoaded) = 0;
    for (int32_t retries = 2;; --retries)
    {
        const eCardStatus card1 = CardGetStatusNow(CARD_1);
        l.card1Status = card1;
        if (card1 != CARD_NOT_PRESENT && card1 != CARD_LIBRARY_BUG)
        {
            CardGetDirectoryNow(l.cardSelected, ptr32<char>(sym::revolt2ProductId), 99, ptr32<anon_57bcf673>(dirTable(l.cardSelected)));
            const eCard card = l.cardSelected;
            savePath(fileName, sym::revolt2GameId);
            int32_t found = -1;
            for (int32_t i = 0; i < entriesFound(card); ++i)
                if (startsWith(sym::revolt2GameId, entryName(card, i)) && CardFileExistsNow(card, ptr32<char>(fileName)) != 0)
                {
                    found = i;
                    break;
                }
            if (found == -1)
                return at<int32_t>(gameLoaded);
            guest_call<int32_t>(sym::MemBlockAllocLo, kLoadSave, 0x2000);
            guest_call<void>(sym::MemBlockSet32, l.pSaveGameBuffer, 0, 0x2000);
            savePath(sp, sym::revolt2GameId);
            if (CardReadData(l.cardSelected, ptr32<char>(sp), 0x2000u, l.pSaveGameBuffer.cast<uint8>()) == 0)
                guest_call<void>(sym::MemBlockFree, kLoadSave);
            else
            {
                const uint32_t buffer = l.pSaveGameBuffer.address;
                if (guest_call<uint32_t>(sym::ResBinaryChecksum, 0, buffer + 0x204u, 0x130) == at<uint32_t>(buffer + 0x200u))
                {
                    guest_call<int32_t>(sym::MemBlockAllocLo, kLoadSave + 0x118u, 0x130);
                    at<int32_t>(gameLoaded) = 1;
                    guest_call<void>(sym::MemBlockSet32, l.pGameSaveData, 0, 0x130);
                    LoadSaveExtractGameSaveData();
                    LoadSaveMergeGameDataWithLoadedData();
                    guest_call<void>(sym::MemBlockFree, kLoadSave);
                    guest_call<void>(sym::MemBlockFree, kLoadSave + 0x118u);
                }
                else
                    guest_call<void>(sym::MemBlockFree, kLoadSave);
            }
        }
        if (retries == 0 || at<int32_t>(gameLoaded) != 0)
            return at<int32_t>(gameLoaded);
    }
}

namespace {

// Whether either card is unformatted or has room for a game save (206 clusters), from the statuses in loadSaveVarG.
bool roomOnCards()
{
    const sLoadSaveClass &l = ls();
    bool room = false;
    if (l.card1Status != CARD_NOT_PRESENT && l.card1Status != CARD_LIBRARY_BUG)
        room = l.card1Status == CARD_UNFORMATTED || freeClusters(CARD_1) >= 0xce;
    if (l.card2Status != CARD_NOT_PRESENT && l.card2Status != CARD_LIBRARY_BUG)
        if (l.card2Status == CARD_UNFORMATTED || freeClusters(CARD_2) >= 0xce)
            room = true;
    return room;
}

} // namespace

// Up to three looks at both cards: no card, room on one, or which are full.
eLoadSaveFreeSpace LoadSaveCheckFreeSpaceOnCards()
{
    GuestFrame frame(0x70u);
    sLoadSaveClass &l = ls();
    bool room = false;
    for (int32_t retries = 2;; --retries)
    {
        l.card1Status = CardGetStatusNow(CARD_1);
        l.card2Status = CardGetStatusNow(CARD_2);
        room = roomOnCards();
        if (l.card1Status == CARD_NOT_PRESENT && l.card2Status == CARD_NOT_PRESENT)
            return LOADSAVE_FS_NOCARDS_FOUND;
        if (retries == 0 || room)
            break;
    }
    if (room)
        return LOADSAVE_FS_SPACE_OK;
    if (l.card1Status != CARD_OKAY)
        return LOADSAVE_FS_CARDFULL_2;
    return l.card2Status == CARD_OKAY ? LOADSAVE_FS_CARDFULL_BOTH : LOADSAVE_FS_CARDFULL_1;
}

// The same from the status checks that run in the background (one card every 25 frames).
eLoadSaveFreeSpace LoadSaveKeepCheckingFreeSpaceOnCards(uint32 frameCnt)
{
    GuestFrame frame(0x50u);
    const uint32_t sp = frame.local<uint8_t>(0).address; // card1Status 0x0, card2Status 0x4
    sLoadSaveClass &l = ls();
    if (l.checkingCards == 0)
    {
        if (frameCnt % 25u == 0)
        {
            const bool second = l.card1Checked != 0;
            if (CardGetStatusASync(second ? CARD_2 : CARD_1, 1, ptr32<eCardStatus>(second ? sp + 4u : sp)) != 0)
                l.checkingCards = 1;
        }
    }
    else if (l.card1Checked == 0)
    {
        if (CardGetStatusASync(CARD_1, 0, ptr32<eCardStatus>(sp)) != 0)
        {
            l.card1Checked = 1;
            l.card1Status = at<eCardStatus>(sp);
            l.checkingCards = 0;
            l.card2Checked = 0;
        }
    }
    else if (CardGetStatusASync(CARD_2, 0, ptr32<eCardStatus>(sp + 4u)) != 0)
    {
        l.card2Checked = 1;
        l.card2Status = at<eCardStatus>(sp + 4u);
        l.checkingCards = 0;
        l.card1Checked = 0;
    }
    const bool room = roomOnCards();
    if (l.card1Status == CARD_NOT_PRESENT && l.card2Status == CARD_NOT_PRESENT)
        return LOADSAVE_FS_NOCARDS_FOUND;
    if (room)
        return LOADSAVE_FS_SPACE_OK;
    if (l.card1Status == CARD_OKAY)
        return l.card2Status == CARD_OKAY ? LOADSAVE_FS_CARDFULL_BOTH : LOADSAVE_FS_CARDFULL_1;
    if (l.card2Status == CARD_OKAY)
        return LOADSAVE_FS_CARDFULL_2;
#if RC_VERSION_EU
    return LOADSAVE_FS_SPACE_OK; // Europe: neither card ready (both full, bad or new) counts as room
#else
    return LOADSAVE_FS_CARDFULL_BOTH;
#endif
}

bool_ LoadSaveWriteFile(eCard card, ptr32<char> fileName, uint32 numBytes, ptr32<uint8> pData, ptr32<bool_> result)
{
    return loadsave::writeFile(card, fileName, numBytes, pData, result, reg(9));
}

// The front end's level data names the messages, the card pictures and the icons.
void LoadSaveSetupClass(ptr32<_sLoadSaveData> pLoadSaveData)
{
    std::memmove(guest_host(kLoadSave + 0x44u), pLoadSaveData.get(), 45u * 4u);
    std::memmove(guest_host(kLoadSave + 0xf8u), guest_host(pLoadSaveData.address + 0xb4u), 2u * 4u);
    std::memmove(guest_host(kLoadSave + 0x100u), guest_host(pLoadSaveData.address + 0xbcu), 6u * 4u);
}

// Up to seven looks for a card in slot 1, a vertical blank apart.
bool_ LoadSaveCheckIfCardInSlot1()
{
    GuestFrame frame(0x60u);
    sLoadSaveClass &l = ls();
    for (int i = 0; i < 7; ++i)
    {
        l.card1Status = CardGetStatusNow(CARD_1);
        l.card2Status = CardGetStatusNow(CARD_2);
        if (l.card1Status != CARD_NOT_PRESENT && l.card1Status != CARD_LIBRARY_BUG)
            return 1;
        guest_call<int32_t>(sym::sceGsSyncV, 0);
    }
    return 0;
}

bool_ LoadSaveFileExists(eCard card, ptr32<char> fileName, ptr32<bool_> result, ptr32<bool_> exists)
{
    return loadsave::fileExists(card, fileName, result, exists, reg(8));
}

bool_ LoadSaveFileErase(eCard card, ptr32<char> fileName, ptr32<bool_> result)
{
    return loadsave::fileErase(card, fileName, result, reg(7));
}

bool_ LoadSaveFileRename(eCard card, ptr32<char> oldName, ptr32<char> newName, ptr32<bool_> result)
{
    return loadsave::fileRename(card, oldName, newName, result, reg(8));
}

} // namespace rc

RC_VERSION_VARIANT(LoadSaveMergeGameDataWithLoadedData)
RC_VERSION_VARIANT(LoadSaveKeepCheckingFreeSpaceOnCards)

RC_DECOMP_FN_BLOCKING_SETUP(LoadSaveCreateGameSaveDirAndIcon, rc::fuzz::lsCreateDir)
RC_DECOMP_FN_BLOCKING_SETUP(LoadSaveCreateTrackSaveDirAndIcon, rc::fuzz::lsCreateDir)
RC_DECOMP_FN_BLOCKING_SETUP(LoadSaveGetRevoltTrackSaveNames, rc::fuzz::lsTrackNames)
RC_DECOMP_FN_BLOCKING_SETUP(LoadSaveGetRevoltTrackLoadNames, rc::fuzz::lsTrackNames)
RC_DECOMP_FN_SETUP(LoadSaveGenerateDefaultTrackSaveName, rc::fuzz::lsDefaultName)
RC_DECOMP_FN_SETUP(LoadSaveCheckForUniqueTrackSaveName, rc::fuzz::lsUniqueName)
RC_DECOMP_FN_SETUP(LoadSaveExtractGameSaveData, rc::fuzz::lsGameData)
RC_DECOMP_FN_SETUP(LoadSaveInsertGameSaveData, rc::fuzz::lsGameData)
RC_DECOMP_FN_SETUP(LoadSaveMergeGameDataWithLoadedData, rc::fuzz::lsMerge)
RC_DECOMP_FN_SETUP(LoadSaveMergeSavedDataWithGameData, rc::fuzz::lsMerge)
RC_DECOMP_FN_SETUP(LoadSaveInit, rc::fuzz::lsInit)
RC_DECOMP_FN_BLOCKING_SETUP(LoadSaveAutoLoadSavedGame, rc::fuzz::lsAutoLoad)
RC_DECOMP_FN_BLOCKING_SETUP(LoadSaveCheckFreeSpaceOnCards, rc::fuzz::lsFreeSpace)
RC_DECOMP_FN_BLOCKING_SETUP(LoadSaveKeepCheckingFreeSpaceOnCards, rc::fuzz::lsFreeSpace)
RC_DECOMP_FN_BLOCKING_SETUP(LoadSaveWriteFile, rc::fuzz::lsWrite)
RC_DECOMP_FN_SETUP(LoadSaveSetupClass, rc::fuzz::lsSetupClass)
RC_DECOMP_FN_BLOCKING_SETUP(LoadSaveCheckIfCardInSlot1, rc::fuzz::lsFreeSpace)
RC_DECOMP_FN_BLOCKING_SETUP(LoadSaveFileExists, rc::fuzz::lsExists)
RC_DECOMP_FN_BLOCKING_SETUP(LoadSaveFileErase, rc::fuzz::lsErase)
RC_DECOMP_FN_BLOCKING_SETUP(LoadSaveFileRename, rc::fuzz::lsRename)

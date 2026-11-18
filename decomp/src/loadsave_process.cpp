// Decompiled from LOADSAVE.c: LoadSaveProcess, the save/load screens' state machine (run once a frame by the front
// end and the Track Editor while a load or save page is up), LoadSaveCheckModeAvailable (what the chosen card allows:
// a save game to load or overwrite, track saves to list, room for a new one) and RenderMemoryCards (the two card
// pictures on the card-choice page).
//
// The state machine starts at most one card command a frame and polls for its end. Steps that take an address for
// their result keep it in the original's frame (GuestFrame, the same offsets). While a page is up the selected card's
// status is checked in the background (pollSelectedCard), and the page ends when the card goes.
//
// Europe (SLES-50077): selecting a track to load no longer resets the confirmation's selection, a track load returns
// 1 at once (no "Load successful" page), and LoadSaveCheckModeAvailable frees the read buffer and shows an error when
// a game save cannot be read or fails its checksum (USA offers to overwrite it), and does not reset the load
// question's selection.

#include "card.h"
#include "card_fuzz.h"

RC_VERSION_VARIANT(LoadSaveProcess)
RC_VERSION_VARIANT(LoadSaveCheckModeAvailable)

namespace rc {

namespace loadsave {
bool_ fileExists(eCard card, ptr32<char> fileName, ptr32<bool_> result, ptr32<bool_> exists, uint32_t leftover);
bool_ fileErase(eCard card, ptr32<char> fileName, ptr32<bool_> result, uint32_t leftover);
bool_ fileRename(eCard card, ptr32<char> oldName, ptr32<char> newName, ptr32<bool_> result, uint32_t leftover);
bool_ writeFile(eCard card, ptr32<char> fileName, uint32 numBytes, ptr32<uint8> pData, ptr32<bool_> result, uint32_t leftover);
} // namespace loadsave

using namespace card;

namespace {

constexpr uint32_t kSlash = sym::bgTrans2G + 4u;       // "/"
constexpr uint32_t kIconSys = sym::levelLoadG + 0x2e0u; // "icon.sys"
constexpr uint32_t kCardStatus = kLoadSave + 0x10u, kSaveBuffer = kLoadSave, kGameSaveData = kLoadSave + 0x118u;
constexpr uint32_t kBack = 0x11000000u, kYes = 0x7fff0000u, kNo = 0x7fff0001u;

uint32_t reg(int index) { return getRegU32(&g_runtime->eeScheduler().runningContext(), index); }
void startPath(uint32_t buffer) { at<uint16_t>(buffer) = at<uint16_t>(kSlash); }
void savePath(uint32_t buffer, uint32_t dir)
{
    startPath(buffer);
    std::strcat(str(buffer), str(dir));
    std::strcat(str(buffer), str(kSlash));
    std::strcat(str(buffer), str(dir));
}
bool startsWith(uint32_t prefix, uint32_t name) { return std::strncmp(str(prefix), str(name), std::strlen(str(prefix))) == 0; }
bool present(int32_t status) { return status != CARD_NOT_PRESENT && status != CARD_LIBRARY_BUG; }

sMenuSystem &ms() { return *menuSystem(); }
void showMsg(int i) { ms().pActiveMenu = msg(i); }
void sfx(int16_t sound, int32_t pitch = 0x100) { guest_call<void>(sym::SfxPlay, sound, 0x80, 0xf, 0xf, pitch, 0, 0); }
bool hit(int key) { return guest_call<int32_t>(sym::InputHit, key) != 0; }
void resetHit(int key) { guest_call<void>(sym::InputResetHit, key); }
void memAlloc(uint32_t handle, uint32_t size) { guest_call<int32_t>(sym::MemBlockAllocLo, handle, size); }
void memFree(uint32_t handle) { guest_call<void>(sym::MemBlockFree, handle); }
void memSet(uint32_t block, uint32_t size) { guest_call<void>(sym::MemBlockSet32, block, 0, size); }
uint32_t checksum(uint32_t buffer) { return guest_call<uint32_t>(sym::ResBinaryChecksum, 0, buffer + 0x204u, 0x130); }

// Starts a status check of the selected card every 25 frames and collects it when it ends (into cardStatus).
void pollSelectedCard(uint32 frameCnt)
{
    sLoadSaveClass &l = ls();
    const eCard card = l.cardSelected;
    if (l.checkingCards == 0)
    {
        if (frameCnt % 25u == 0 && CardGetStatusASync(card, 1, ptr32<eCardStatus>(kCardStatus)) != 0)
            l.checkingCards = 1;
    }
    else if (CardGetStatusASync(card, 0, ptr32<eCardStatus>(kCardStatus)) != 0)
        l.checkingCards = 0;
}

// The selected card is still ready, or was at the last check.
bool selectedCardReady() { return ls().cardStatus == CARD_OKAY || ls().cardPrevStatus == CARD_OKAY; }

// A card's first message of a pair (card 1's or card 2's).
int forCard(int card1Msg, int card2Msg) { return ls().cardSelected == CARD_1 ? card1Msg : card2Msg; }

// Back to the card-choice page (from a message's "back" or a failure).
void backToCardChoice(int16_t sound)
{
    sfx(sound);
    ls().status = STATE_PRE_WAITING;
    ms().executeCode = 0;
    showMsg(forCard(0, 1));
}

void saveFailed()
{
    ls().status = STATE_SAVE_FAILED;
    showMsg(0x12);
}

// The name the player typed (or a default "Track<n>" when it is blank), appended to the track save's directory
// name and title, trailing spaces removed from the directory name.
void nameTrackSave(uint32_t sp, bool_ overwrite)
{
    sLoadSaveClass &l = ls();
    std::strcpy(str(sym::revolt2TrackSaveId), str(sym::revolt2TrackId));
    std::strcpy(str(sym::revolt2TrackSaveName), str(sym::revolt2TrackNameGeneric));
    const uint32_t typed = menuSystem().address + 0xc4u; // menuInputString
    bool blank = true;
    for (int32_t i = static_cast<int32_t>(std::strlen(str(typed))) - 1; i >= 0; --i)
        if (at<char>(typed + static_cast<uint32_t>(i)) != ' ')
        {
            blank = false;
            break;
        }
    if (blank)
    {
        const uint32_t defaultName = sp + 0x30u;
        LoadSaveGenerateDefaultTrackSaveName(l.cardSelected, ptr32<char>(defaultName), overwrite);
        std::strcat(str(sym::revolt2TrackSaveId), str(defaultName));
        std::strcat(str(sym::revolt2TrackSaveName), str(defaultName));
    }
    else
    {
        std::strcat(str(sym::revolt2TrackSaveId), str(menuSystem().address + 0xc4u));
        std::strcat(str(sym::revolt2TrackSaveName), str(menuSystem().address + 0xc4u));
    }
    const uint32_t id = sym::revolt2TrackSaveId;
    for (int32_t i = static_cast<int32_t>(std::strlen(str(id))) - 1; i >= 0 && at<char>(id + static_cast<uint32_t>(i)) == ' '; --i)
        at<char>(id + static_cast<uint32_t>(i)) = 0;
}

// The n-th track save on the selected card (counting only track saves, the count kept in a byte).
void findTrackSave(uint32_t to, int32_t n)
{
    const eCard card = ls().cardSelected;
    int8_t found = 0;
    for (int32_t i = 0; i < entriesFound(card); ++i)
    {
        const uint32_t name = entryName(card, i);
        if (!startsWith(sym::revolt2TrackId, name))
            continue;
        if (n == found)
        {
            std::strcpy(str(to), str(name));
            return;
        }
        found = static_cast<int8_t>(found + 1);
    }
}

} // namespace

int32 LoadSaveProcess(uint32 frameCnt)
{
    // Registers the original leaves as they came: the file steps return them for a subState that is not theirs.
    const uint32_t entryT0 = reg(8), entryT1 = reg(9);
    GuestFrame frame(0x240u);
    const uint32_t sp = frame.local<uint8_t>(0).address;
    sLoadSaveClass &l = ls();
    const auto res = [sp](uint32_t offset) { return ptr32<bool_>(sp + offset); };
    const auto val = [sp](uint32_t offset) { return at<int32_t>(sp + offset); };
    const auto writeFile = [&](uint32_t name, uint32_t size, uint32_t data, uint32_t result) {
        return loadsave::writeFile(l.cardSelected, ptr32<char>(name), size, ptr32<uint8>(data), res(result), entryT1) != 0;
    };
    const auto fileExists = [&](uint32_t name, uint32_t result, uint32_t exists) {
        return loadsave::fileExists(l.cardSelected, ptr32<char>(name), res(result), res(exists), entryT0) != 0;
    };
    const auto fileErase = [&](uint32_t name, uint32_t result) {
        return loadsave::fileErase(l.cardSelected, ptr32<char>(name), res(result), frameCnt) != 0;
    };
    const auto waitResult = [&](uint32_t result) { return CardWaitASyncFuncResult(res(result)) != 0; };
    // The next step of a delete or a check: its subState, then the state.
    const auto next = [&](eLoadSaveStatus state, uint32_t subState) {
        l.status = state;
        l.subState = subState;
    };
    const auto resetCheck = [&]() {
        l.checkingCards = 0;
        l.card1Checked = 0;
        l.card2Checked = 0;
    };
    enum class End { Draw, ResetKeys };
    End end = End::Draw;

    switch (static_cast<uint32_t>(l.status))
    {
    case STATE_FIRST_TIME:
        l.status = STATE_INITIALISING;
        break;
    case STATE_INITIALISING:
        l.status = STATE_CHECKING;
        l.waitCount = 3;
        break;
    case STATE_CHECKING:
        l.waitCount -= 1;
        showMsg(2);
        if (l.waitCount != 0)
            break;
        CardWaitASyncFuncTerm();
        l.card1Status = CardGetStatusNow(CARD_1);
        l.card2Status = CardGetStatusNow(CARD_2);
        // Keep the selection on a card that is there.
        if (l.cardSelected == CARD_NONE)
        {
            if (present(l.card1Status) || present(l.card2Status))
                l.cardSelected = CARD_1;
        }
        else if (l.cardSelected == CARD_1)
        {
            if (!present(l.card1Status) && present(l.card2Status))
                l.cardSelected = CARD_2;
        }
        else if (l.cardSelected == CARD_2)
        {
            if (!present(l.card2Status) && present(l.card1Status))
                l.cardSelected = CARD_1;
        }
        l.status = STATE_WAITING;
        l.card2PrevStatus = CARD_NOT_PRESENT;
        l.card1PrevStatus = CARD_NOT_PRESENT;
        resetCheck();
        break;
    case STATE_PRE_WAITING:
        CardWaitASyncFuncTerm();
        l.status = STATE_WAITING;
        resetCheck();
        break;
    case STATE_WAITING: {
        // The card-choice page: both cards checked in turn in the background.
        at<int32_t>(sp + 0xe0u) = l.card1Status;
        at<int32_t>(sp + 0xe4u) = l.card2Status;
        if (l.checkingCards == 0)
        {
            if (frameCnt % 25u == 0)
            {
                const bool second = l.card1Checked != 0;
                if (CardGetStatusASync(second ? CARD_2 : CARD_1, 1, ptr32<eCardStatus>(sp + (second ? 0xe4u : 0xe0u))) != 0)
                    l.checkingCards = 1;
            }
        }
        else if (l.card1Checked == 0)
        {
            if (CardGetStatusASync(CARD_1, 0, ptr32<eCardStatus>(sp + 0xe0u)) != 0)
            {
                l.checkingCards = 0;
                l.card1Checked = 1;
                l.card2Checked = 0;
            }
        }
        else if (CardGetStatusASync(CARD_2, 0, ptr32<eCardStatus>(sp + 0xe4u)) != 0)
        {
            l.checkingCards = 0;
            l.card2Checked = 1;
            l.card1Checked = 0;
        }
        const int32_t c1 = val(0xe0u), c2 = val(0xe4u);
        if (c1 == CARD_NEW_CARD || c2 == CARD_NEW_CARD)
        {
            l.status = STATE_INITIALISING;
            break;
        }
        int32_t prev1, prev2;
        if (l.cardSelected == CARD_NONE)
        {
            showMsg(3);
            prev1 = l.card1Status;
            prev2 = l.card2Status;
            if (prev1 != c1 || prev2 != c2)
                l.status = STATE_INITIALISING;
        }
        else
        {
            l.card1Status = static_cast<eCardStatus>(c1);
            l.card2Status = static_cast<eCardStatus>(c2);
            const bool onCard1 = l.cardSelected == CARD_1;
            const int32_t mine = onCard1 ? c1 : c2, other = onCard1 ? c2 : c1;
            if (!present(mine))
            {
                if (!present(other))
                {
                    l.status = STATE_INITIALISING;
                    showMsg(2);
                    l.cardSelected = CARD_NONE;
                }
                else
                {
                    l.cardSelected = onCard1 ? CARD_2 : CARD_1;
                    showMsg(onCard1 ? 1 : 0);
                }
                prev1 = c1;
                prev2 = c2;
            }
            else if (mine == CARD_UNFORMATTED && (onCard1 ? l.card1PrevStatus : l.card2PrevStatus) == CARD_UNFORMATTED)
            {
                showMsg(onCard1 ? 0x2b : 0x2c); // still unformatted: the format offer
                prev1 = c1;
                prev2 = c2;
            }
            else
            {
                showMsg(onCard1 ? 0 : 1);
                prev1 = l.card1Status;
                prev2 = l.card2Status;
            }
        }
        l.card1PrevStatus = static_cast<eCardStatus>(prev1);
        l.card2PrevStatus = static_cast<eCardStatus>(prev2);
        // Left and right change the card.
        if ((hit(7) || hit(0x17)) && l.cardSelected == CARD_2)
        {
            if (present(l.card1Status))
            {
                l.cardSelected = CARD_1;
                sfx(0x44, 0xf6);
            }
        }
        else if ((hit(5) || hit(0x15)) && l.cardSelected == CARD_1)
        {
            if (present(l.card2Status))
            {
                l.cardSelected = CARD_2;
                sfx(0x44, 0xf6);
            }
        }
        if ((hit(0xe) || hit(0x1e)) && l.cardSelected != CARD_NONE)
        {
            bool go = true;
            if (l.loadSaveMode == LOADSAVE_MODE_GAMELOAD || l.loadSaveMode == LOADSAVE_MODE_TRACKLOAD)
                go = (l.cardSelected != CARD_1 ? l.card2Status : l.card1Status) != CARD_UNFORMATTED;
            if (go)
            {
                sfx(0x45);
                l.status = STATE_GET_CARDDIRECTORY;
                l.waitCount = 3;
                showMsg(forCard(4, 0x1c));
            }
            resetCheck();
            end = End::ResetKeys;
            break;
        }
        if (hit(0xc) || hit(0x1c))
        {
            sfx(0x43);
            resetHit(0xc);
            resetHit(0x1c);
            return -1;
        }
        break;
    }
    case STATE_GET_CARDDIRECTORY:
        l.waitCount -= 1;
        if (l.waitCount != 0)
            break;
        CardWaitASyncFuncTerm();
        CardChangeDirectoryNow(l.cardSelected, ptr32<char>(kSlash), nullptr);
        CardGetDirectoryNow(l.cardSelected, ptr32<char>(sym::revolt2ProductId), 99, ptr32<anon_57bcf673>(dirTable(l.cardSelected)));
        l.cardStatus = CardGetStatusNow(l.cardSelected);
        if (static_cast<uint32_t>(l.cardStatus) < CARD_NOT_PRESENT)
        {
            LoadSaveCheckModeAvailable();
            guest_call<void>(sym::InputResetAllHit);
        }
        else
            l.status = STATE_INITIALISING;
        l.cardPrevStatus = CARD_OKAY;
        break;
    case STATE_FORMATTING:
        l.waitCount -= 1;
        if (l.waitCount == 0)
            l.status = STATE_DO_FORMATTING;
        break;
    case STATE_DO_FORMATTING:
        CardWaitASyncFuncTerm();
        if (CardFormat(l.cardSelected) == 0)
        {
            l.status = STATE_FORMAT_FAILED;
            showMsg(0x13);
        }
        else
            l.status = STATE_AWAIT_FORMATTING_END;
        ms().executeCode = 0;
        break;
    case STATE_AWAIT_FORMATTING_END:
        if (!waitResult(0xe8u))
            break;
        if (val(0xe8u) == 1)
        {
            l.cardStatus = CardGetStatusNow(l.cardSelected);
            if (l.loadSaveMode == LOADSAVE_MODE_GAMESAVE)
            {
                l.gameSaveLoaded = 0;
                memAlloc(kSaveBuffer, 0x2000u);
                memSet(l.pSaveGameBuffer.address, 0x2000u);
                l.status = STATE_GAME_SAVING;
                showMsg(forCard(0x16, 0x1f));
                l.waitCount = 3;
            }
            else
            {
                l.status = STATE_TRACK_SAVE_TYPE_1;
                showMsg(8);
            }
        }
        else
        {
            l.status = STATE_FORMAT_FAILED;
            showMsg(0x13);
        }
        ms().executeCode = 0;
        break;
    case STATE_CONFIRMING_FORMAT: {
        pollSelectedCard(frameCnt);
        if (static_cast<uint32_t>(l.cardStatus) >= CARD_NOT_PRESENT)
        {
            l.status = STATE_INITIALISING;
            break;
        }
        const uint32_t code = ms().executeCode;
        if (code == kBack || code == kNo)
            backToCardChoice(0x43);
        else if (code == 0x10000075u) // format
        {
            sfx(0x45);
            l.status = STATE_FORMATTING;
            l.waitCount = 3;
            showMsg(forCard(0xb, 0x22));
            ms().executeCode = 0;
        }
        break;
    }
    case STATE_FORMAT_FAILED:
        if (ms().executeCode != kYes)
            break;
        sfx(0x45);
        l.status = STATE_PRE_WAITING;
        ms().executeCode = 0;
        showMsg(forCard(0x2b, 0x2c));
        break;
    case STATE_TRACK_SAVING:
        l.waitCount -= 1;
        if (l.waitCount == 0)
            l.status = STATE_DO_TRACK_SAVING;
        break;
    case STATE_DO_TRACK_SAVING:
        CardWaitASyncFuncTerm();
        if (l.overwriteExistingSave == 0)
        {
            if (CardChangeDirectory(l.cardSelected, ptr32<char>(sym::revolt2TrackSaveId), nullptr) == 0)
                saveFailed();
            else
                l.status = STATE_DO_TRACK_SAVING_PATH1_A;
        }
        else if (std::strcmp(str(sym::revolt2TrackSaveIdToDelete), str(sym::revolt2TrackSaveId)) == 0)
            l.status = STATE_DO_TRACK_SAVING_PATH3_B;
        else
            next(STATE_DO_TRACK_SAVING_PATH3_A, 10);
        break;
    case STATE_DO_TRACK_SAVING_FINAL:
        if (!writeFile(sym::revolt2TrackSaveId, 0x2000u, l.pSaveGameBuffer.address, 0x130u))
            break;
        memFree(kSaveBuffer);
        if (val(0x130u) == 0)
        {
            l.status = STATE_SAVE_FAILED;
            showMsg(0x12);
        }
        else
        {
            l.status = STATE_SAVE_SUCCESSFUL;
            showMsg(0x11);
        }
        ms().executeCode = 0;
        end = End::ResetKeys;
        break;
    // A new track save: make its directory, enter it, write icon.sys and the icon, then the track.
    case STATE_DO_TRACK_SAVING_PATH1_A:
        if (!waitResult(0xf8u))
            break;
        if (val(0xf8u) == 0)
        {
            startPath(sp + 0x60u);
            std::strcat(str(sp + 0x60u), str(sym::revolt2TrackSaveId));
            if (CardCreateDirectory(l.cardSelected, ptr32<char>(sp + 0x60u)) == 0)
                saveFailed();
            else
                l.status = STATE_DO_TRACK_SAVING_PATH1_B;
        }
        else
            next(STATE_DO_TRACK_SAVING_PATH2_A, 6); // it exists: replace its files
        break;
    case STATE_DO_TRACK_SAVING_PATH1_B:
        if (!waitResult(0x120u))
            break;
        if (val(0x120u) != 0)
        {
            startPath(sp + 0x30u);
            std::strcat(str(sp + 0x30u), str(sym::revolt2TrackSaveId));
            if (CardChangeDirectory(l.cardSelected, ptr32<char>(sp + 0x30u), nullptr) != 0)
            {
                l.status = STATE_DO_TRACK_SAVING_PATH1_C;
                break;
            }
        }
        saveFailed();
        break;
    case STATE_DO_TRACK_SAVING_PATH1_C:
        if (!waitResult(0x124u))
            break;
        if (val(0x124u) == 0)
        {
            saveFailed();
            break;
        }
        at<int32_t>(sp) = 0x1e;
        CardCreateIconHeader(ptr32<anon_88ca8770>(kIcon), ptr32<sceVu0IVECTOR>(sym::bgColor2G), ptr32<sceVu0FVECTOR>(sym::lightDir2G),
                             ptr32<sceVu0FVECTOR>(sym::lightCol2G), ptr32<sceVu0FVECTOR>(sym::ambient2G), ptr32<char>(sym::revolt2TrackSaveName),
                             ptr32<ptr32<char>>(sym::iconName2G), G(bgTrans2G));
        next(STATE_DO_TRACK_SAVING_PATH1_D, 0);
        break;
    case STATE_DO_TRACK_SAVING_PATH1_D:
        if (!writeFile(kIconSys, 0x3c4u, kIcon, 0x128u))
            break;
        if (val(0x128u) == 0)
            saveFailed();
        else
            next(STATE_DO_TRACK_SAVING_PATH1_E, 0);
        break;
    case STATE_DO_TRACK_SAVING_PATH1_E:
        if (!writeFile(at<uint32_t>(sym::iconName2G), at<uint32_t>(sym::iconSize2G), l.loadSaveData.pTrackSaveIcon[0].address, 0x12cu))
            break;
        if (val(0x12cu) == 0)
        {
            saveFailed();
            break;
        }
        l.subState = 0;
        l.status = STATE_DO_TRACK_SAVING_FINAL;
        memAlloc(kSaveBuffer, 0x2000u);
        memSet(l.pSaveGameBuffer.address, 0x2000u);
        guest_call<void>(sym::TEditInsertTrackDescToSaveBuffer);
        break;
    // Replacing a track save's files: each one that is there is deleted first.
    case STATE_DO_TRACK_SAVING_PATH2_A:
        if (!fileExists(kIconSys, 0xfcu, 0x100u))
            break;
        if (val(0xfcu) != 1)
            saveFailed();
        else if (val(0x100u) == 1)
            next(STATE_DO_TRACK_SAVING_PATH2_B, 8);
        else
            next(STATE_DO_TRACK_SAVING_PATH2_C, 6);
        break;
    case STATE_DO_TRACK_SAVING_PATH2_B:
        if (!fileErase(kIconSys, 0x104u))
            break;
        if (val(0x104u) != 1)
            saveFailed();
        else
            next(STATE_DO_TRACK_SAVING_PATH2_C, 6);
        break;
    case STATE_DO_TRACK_SAVING_PATH2_C:
        if (!fileExists(at<uint32_t>(sym::iconName2G), 0x108u, 0x10cu))
            break;
        if (val(0x108u) != 1)
            saveFailed();
        else if (val(0x10cu) == 1)
            next(STATE_DO_TRACK_SAVING_PATH2_D, 8);
        else
            next(STATE_DO_TRACK_SAVING_PATH2_E, 6);
        break;
    case STATE_DO_TRACK_SAVING_PATH2_D:
        if (!fileErase(at<uint32_t>(sym::iconName2G), 0x110u))
            break;
        if (val(0x110u) != 1)
            saveFailed();
        else
            next(STATE_DO_TRACK_SAVING_PATH2_E, 6);
        break;
    case STATE_DO_TRACK_SAVING_PATH2_E:
        if (!fileExists(sym::revolt2TrackSaveId, 0x114u, 0x118u))
            break;
        if (val(0x114u) != 1)
            saveFailed();
        else if (val(0x118u) == 1)
            next(STATE_DO_TRACK_SAVING_PATH2_F, 8);
        else
            next(STATE_DO_TRACK_SAVING_PATH2_G, 6);
        break;
    case STATE_DO_TRACK_SAVING_PATH2_F:
        if (!fileErase(sym::revolt2TrackSaveId, 0x11cu))
            break;
        if (val(0x11cu) != 1)
            saveFailed();
        else
            next(STATE_DO_TRACK_SAVING_PATH2_G, 6);
        break;
    case STATE_DO_TRACK_SAVING_PATH2_G:
        at<int32_t>(sp) = 0x1e;
        CardCreateIconHeader(ptr32<anon_88ca8770>(kIcon), ptr32<sceVu0IVECTOR>(sym::bgColor2G), ptr32<sceVu0FVECTOR>(sym::lightDir2G),
                             ptr32<sceVu0FVECTOR>(sym::lightCol2G), ptr32<sceVu0FVECTOR>(sym::ambient2G), ptr32<char>(sym::revolt2TrackSaveName),
                             ptr32<ptr32<char>>(sym::iconName2G), G(bgTrans2G));
        next(STATE_DO_TRACK_SAVING_PATH1_D, 0);
        break;
    // Overwriting a track save under a new name: rename its directory, enter it, delete the old track file.
    case STATE_DO_TRACK_SAVING_PATH3_A:
        if (loadsave::fileRename(l.cardSelected, ptr32<char>(sym::revolt2TrackSaveIdToDelete), ptr32<char>(sym::revolt2TrackSaveId),
                                 res(0xecu), entryT0) == 0)
            break;
        if (val(0xecu) == 1)
            l.status = STATE_DO_TRACK_SAVING_PATH3_B;
        else
            saveFailed();
        break;
    case STATE_DO_TRACK_SAVING_PATH3_B:
        if (CardChangeDirectory(l.cardSelected, ptr32<char>(sym::revolt2TrackSaveId), nullptr) == 0)
            saveFailed();
        else
            l.status = STATE_DO_TRACK_SAVING_PATH3_C;
        break;
    case STATE_DO_TRACK_SAVING_PATH3_C:
        if (!waitResult(0xf0u))
            break;
        if (val(0xf0u) == 0)
            saveFailed();
        else
            next(STATE_DO_TRACK_SAVING_PATH3_D, 8);
        break;
    case STATE_DO_TRACK_SAVING_PATH3_D:
        if (!fileErase(sym::revolt2TrackSaveIdToDelete, 0xf4u))
            break;
        if (val(0xf4u) == 0)
            saveFailed();
        else
            next(STATE_DO_TRACK_SAVING_PATH2_A, 6);
        break;
    case STATE_SAVE_SUCCESSFUL:
        if (ms().executeCode != kYes)
            break;
        sfx(0x45);
        ms().executeCode = 0;
        return 1;
    case STATE_SAVE_FAILED:
        ms().executeCode = 0;
        resetHit(0xe);
        resetHit(0x1e);
        if (l.pSaveGameBuffer)
            memFree(kSaveBuffer);
        if (l.pGameSaveData)
            memFree(kGameSaveData);
        l.status = STATE_SAVE_FAILED_MSG;
        break;
    case STATE_SAVE_FAILED_MSG:
        if (ms().executeCode == kYes)
            backToCardChoice(0x45);
        break;
    case STATE_LOAD_FAILED:
        if (ms().executeCode == kYes)
            backToCardChoice(0x45);
        break;
    case STATE_LOAD_SUCCESSFUL:
        if (ms().executeCode != kYes)
            break;
        sfx(0x45);
        return 1;
    case STATE_TRACK_SAVE_TYPE_1: {
        // No track saves on the card yet: the name page.
        pollSelectedCard(frameCnt);
        if (!selectedCardReady())
        {
            l.status = STATE_INITIALISING;
            break;
        }
        l.cardPrevStatus = l.cardStatus;
        const uint32_t code = ms().executeCode;
        if (code == kBack)
            backToCardChoice(0x43);
        else if (code == kYes)
        {
            sfx(0x45);
            l.status = STATE_TRACK_SAVING;
            l.waitCount = 3;
            nameTrackSave(sp, 0);
            l.overwriteExistingSave = 0;
            showMsg(forCard(9, 0x20));
        }
        break;
    }
    case STATE_TRACK_SAVE_TYPE_2: {
        // The list of track saves (and "new"): choosing one asks whether to overwrite it.
        pollSelectedCard(frameCnt);
        if (!selectedCardReady())
        {
            l.status = STATE_INITIALISING;
            break;
        }
        l.cardPrevStatus = l.cardStatus;
        uint32_t code = ms().executeCode;
        if (code == kBack)
        {
            backToCardChoice(0x43);
            break;
        }
        if (code - 1u < 0xfu)
        {
            sfx(0x45);
            const int32_t n = static_cast<int32_t>(ms().executeCode - 1u);
            at<int32_t>(sp + 0x198u) = n;
            findTrackSave(sym::revolt2TrackSaveIdToDelete, n);
            l.overwriteExistingSave = 1;
            ms().executeCode = 0;
            showMsg(0xe);
            l.status = STATE_TRACK_SAVE_TYPE_2_OVERWRITE_Q;
            msg(0xe)->selection = 0;
            resetHit(0xe);
            resetHit(0x1e);
            code = ms().executeCode;
        }
        if (code == 0x10u) // a new save
        {
            sfx(0x45);
            l.status = STATE_TRACK_SAVE_TYPE_2_ENTER_NAME;
            showMsg(8);
            l.overwriteExistingSave = 0;
        }
        break;
    }
    case STATE_TRACK_SAVE_TYPE_2_OVERWRITE_Q: {
        pollSelectedCard(frameCnt);
        l.cardPrevStatus = l.cardStatus;
        if (l.cardStatus != CARD_OKAY)
        {
            l.status = STATE_INITIALISING;
            break;
        }
        const uint32_t code = ms().executeCode;
        if (code == kYes)
        {
            sfx(0x45);
            l.status = STATE_TRACK_SAVE_TYPE_2_ENTER_NAME;
            showMsg(8);
            ms().executeCode = 0;
        }
        else if (code == kNo || code == kBack)
        {
            sfx(0x43);
            l.status = STATE_TRACK_SAVE_TYPE_2;
            showMsg(0xc);
            ms().executeCode = 0;
        }
        break;
    }
    case STATE_TRACK_SAVE_TYPE_2_ENTER_NAME: {
        pollSelectedCard(frameCnt);
        l.cardPrevStatus = l.cardStatus;
        if (l.cardStatus != CARD_OKAY)
        {
            l.status = STATE_INITIALISING;
            break;
        }
        const uint32_t code = ms().executeCode;
        if (code == kBack)
        {
            sfx(0x43);
            l.status = STATE_TRACK_SAVE_TYPE_2;
            showMsg(0xc);
            ms().executeCode = 0;
        }
        else if (code == kYes)
        {
            sfx(0x45);
            if (LoadSaveCheckForUniqueTrackSaveName(l.cardSelected, ptr32<char>(menuSystem().address + 0xc4u), l.overwriteExistingSave) == 1)
            {
                l.status = STATE_TRACK_SAVING;
                l.waitCount = 3;
                nameTrackSave(sp, l.overwriteExistingSave);
                showMsg(forCard(9, 0x20));
            }
            else
            {
                l.status = STATE_TRACK_SAVE_TYPE_2_DUPLICATE_NAME;
                showMsg(0x19);
            }
        }
        break;
    }
    case STATE_TRACK_SAVE_TYPE_2_DUPLICATE_NAME:
        if (ms().executeCode != kYes)
            break;
        sfx(0x45);
        l.status = STATE_TRACK_SAVE_TYPE_2_ENTER_NAME;
        showMsg(8);
        ms().executeCode = 0;
        break;
    case STATE_TRACK_LOAD_SELECT: {
        at<int8_t>(sym::frontEndVarG + 0x2d4u) = 0; // fisrtDrawFlagPTT
        pollSelectedCard(frameCnt);
        if (!selectedCardReady())
        {
            l.status = STATE_INITIALISING;
            break;
        }
        l.cardPrevStatus = l.cardStatus;
        if (ms().executeCode == kBack)
        {
            backToCardChoice(0x43);
            break;
        }
        if (ms().executeCode - 1u < 0xfu)
        {
            sfx(0x45);
            const int32_t n = static_cast<int32_t>(ms().executeCode - 1u);
            at<int32_t>(sp + 0x19cu) = n;
            findTrackSave(sym::revolt2TrackSaveId, n);
            l.status = STATE_TRACK_LOAD_CONFIRM;
            showMsg(0xf);
            ms().executeCode = 0;
#if !RC_VERSION_EU
            msg(0xf)->selection = 0;
#endif
        }
        break;
    }
    case STATE_TRACK_LOAD_CONFIRM: {
        pollSelectedCard(frameCnt);
        if (!selectedCardReady())
        {
            l.status = STATE_INITIALISING;
            break;
        }
        l.cardPrevStatus = l.cardStatus;
        const uint32_t code = ms().executeCode;
        if (code == kYes)
        {
            sfx(0x45);
            l.status = STATE_TRACK_LOADING;
            showMsg(forCard(0x10, 0x21));
            ms().executeCode = 0;
            l.waitCount = 3;
        }
        else if (code == kNo || code == kBack)
        {
            sfx(0x43);
            l.status = STATE_TRACK_LOAD_SELECT;
            showMsg(0xd);
            ms().executeCode = 0;
        }
        break;
    }
    case STATE_TRACK_LOADING:
        l.waitCount -= 1;
        if (l.waitCount == 0)
            l.status = STATE_DO_TRACK_LOADING;
        break;
    case STATE_DO_TRACK_LOADING:
        savePath(sp + 0x30u, sym::revolt2TrackSaveId);
        memAlloc(kSaveBuffer, 0x2000u);
        memSet(l.pSaveGameBuffer.address, 0x2000u);
        CardWaitASyncFuncTerm();
        if (CardReadData(l.cardSelected, ptr32<char>(sp + 0x30u), 0x2000u, l.pSaveGameBuffer.cast<uint8>()) != 0 &&
            guest_call<int32_t>(sym::TEditExtractTrackDescFromSaveBuffer) != 0)
        {
            memFree(kSaveBuffer);
#if RC_VERSION_EU
            return 1;
#else
            l.status = STATE_LOAD_SUCCESSFUL;
            showMsg(0x2a);
            ms().executeCode = 0;
#endif
        }
        else
        {
            l.status = STATE_LOAD_FAILED;
            showMsg(0x14);
            memFree(kSaveBuffer);
            ms().executeCode = 0;
        }
        break;
    case STATE_GAME_SAVE_Q: {
        // "Save game?" (or "overwrite?"): the save buffer, and a loaded save's data, were set up by
        // LoadSaveCheckModeAvailable.
        pollSelectedCard(frameCnt);
        if (!selectedCardReady())
        {
            l.status = STATE_INITIALISING;
            if (l.gameSaveLoaded == 1)
                memFree(kGameSaveData);
            memFree(kSaveBuffer);
            break;
        }
        l.cardPrevStatus = l.cardStatus;
        const uint32_t code = ms().executeCode;
        if (code == kYes)
        {
            sfx(0x45);
            l.status = STATE_GAME_SAVING;
            showMsg(forCard(0x16, 0x1f));
            ms().executeCode = 0;
            l.waitCount = 3;
        }
        else if (code == kNo || code == kBack)
        {
            sfx(0x43);
            l.status = STATE_PRE_WAITING;
            ms().executeCode = 0;
            if (l.gameSaveLoaded == 1)
                memFree(kGameSaveData);
            memFree(kSaveBuffer);
            showMsg(forCard(0, 1));
        }
        break;
    }
    case STATE_GAME_LOAD_Q: {
        // "Load game?" with the save's progress card on request.
        sFrontEndClass &fe = G(frontEndVarG);
        if (fe.flagPTT == 0)
            pollSelectedCard(frameCnt);
        if (!selectedCardReady())
        {
            l.status = STATE_INITIALISING;
            memFree(kSaveBuffer);
            memFree(kGameSaveData);
            break;
        }
        const uint32_t code = ms().executeCode;
        l.cardPrevStatus = l.cardStatus;
        if (code == kBack || code == kNo)
        {
            sfx(0x43);
            l.status = STATE_PRE_WAITING;
            ms().executeCode = 0;
            memFree(kSaveBuffer);
            memFree(kGameSaveData);
            showMsg(forCard(0, 1));
        }
        else if (code == kYes)
        {
            sfx(0x45);
            LoadSaveMergeGameDataWithLoadedData();
            memFree(kSaveBuffer);
            memFree(kGameSaveData);
            l.status = STATE_LOAD_SUCCESSFUL;
            showMsg(0x2a);
            ms().executeCode = 0;
        }
        else if (code == 0x1000007bu) // view the saved progress
        {
            if (fe.flagPTT == 0)
            {
                sfx(0x45);
                fe.scrollPTT = 0x32;
                fe.scrollPTTspeed = 1;
                fe.scrollPTTitemHeight = -1;
                fe.scrollPTTheaderHeight = -1;
                fe.scrollPTTHeight = 0;
            }
            fe.flagPTT = 1;
        }
        break;
    }
    case STATE_GAME_SAVING:
        l.waitCount -= 1;
        if (l.waitCount == 0)
            l.status = STATE_DO_GAME_SAVING;
        break;
    case STATE_DO_GAME_SAVING:
        CardWaitASyncFuncTerm();
        if (CardChangeDirectory(l.cardSelected, ptr32<char>(sym::revolt2GameId), nullptr) == 0)
            saveFailed();
        else
            l.status = l.gameSaveLoaded == 1 ? STATE_DO_GAME_SAVING_PATH1_A : STATE_DO_GAME_SAVING_PATH2_A;
        break;
    // Overwriting a good save: delete the game file, merge the game into the loaded data, write it.
    case STATE_DO_GAME_SAVING_PATH1_A:
        if (!waitResult(0x134u))
            break;
        if (val(0x134u) != 1)
            saveFailed();
        else
            next(STATE_DO_GAME_SAVING_PATH1_B, 8);
        break;
    case STATE_DO_GAME_SAVING_PATH1_B: {
        if (!fileErase(sym::revolt2GameId, 0x138u))
            break;
        if (val(0x138u) != 1)
        {
            saveFailed();
            break;
        }
        LoadSaveMergeSavedDataWithGameData();
        LoadSaveInsertGameSaveData();
        const uint32_t buffer = l.pSaveGameBuffer.address;
        at<uint32_t>(buffer + 0x200u) = checksum(buffer);
        next(STATE_DO_GAME_SAVING_PATH1_C, 0);
        break;
    }
    case STATE_DO_GAME_SAVING_PATH1_C:
        if (!writeFile(sym::revolt2GameId, 0x2000u, l.pSaveGameBuffer.address, 0x13cu))
            break;
        memFree(kGameSaveData);
        memFree(kSaveBuffer);
        if (val(0x13cu) == 1)
        {
            l.status = STATE_SAVE_SUCCESSFUL;
            showMsg(0x11);
        }
        else
            saveFailed();
        break;
    // A new save: make the directory, enter it, write icon.sys and the three icons, then the game.
    case STATE_DO_GAME_SAVING_PATH2_A:
        if (!waitResult(0x140u))
            break;
        if (val(0x140u) == 0)
        {
            startPath(sp + 0xa0u);
            std::strcat(str(sp + 0xa0u), str(sym::revolt2GameId));
            if (CardCreateDirectory(l.cardSelected, ptr32<char>(sp + 0xa0u)) == 0)
                saveFailed();
            else
                l.status = STATE_DO_GAME_SAVING_PATH2_B;
        }
        else
            next(STATE_DO_GAME_SAVING_PATH3_A, 6); // it exists: replace its files
        break;
    case STATE_DO_GAME_SAVING_PATH2_B:
        if (!waitResult(0x144u))
            break;
        if (val(0x144u) != 0)
        {
            startPath(sp + 0x70u);
            std::strcat(str(sp + 0x70u), str(sym::revolt2GameId));
            if (CardChangeDirectory(l.cardSelected, ptr32<char>(sp + 0x70u), nullptr) != 0)
            {
                l.status = STATE_DO_GAME_SAVING_PATH2_C;
                break;
            }
        }
        saveFailed();
        break;
    case STATE_DO_GAME_SAVING_PATH2_C:
        if (!waitResult(0x148u))
            break;
        if (val(0x148u) == 0)
            saveFailed();
        else
            goto gameIconHeader;
        break;
    case STATE_DO_GAME_SAVING_PATH2_D:
        if (!writeFile(kIconSys, 0x3c4u, kIcon, 0x14cu))
            break;
        if (val(0x14cu) == 0)
            saveFailed();
        else
            next(STATE_DO_GAME_SAVING_PATH2_E, 0);
        break;
    case STATE_DO_GAME_SAVING_PATH2_E:
    case STATE_DO_GAME_SAVING_PATH2_F: {
        const uint32_t i = l.status == STATE_DO_GAME_SAVING_PATH2_E ? 0u : 1u;
        const uint32_t result = i == 0 ? 0x150u : 0x154u;
        if (!writeFile(at<uint32_t>(sym::iconName1G + 4u * i), at<uint32_t>(sym::iconSize1G + 4u * i),
                       at<uint32_t>(kLoadSave + 0x100u + 4u * i), result))
            break;
        if (val(result) == 0)
            saveFailed();
        else
            next(i == 0 ? STATE_DO_GAME_SAVING_PATH2_F : STATE_DO_GAME_SAVING_PATH2_G, 0);
        break;
    }
    case STATE_DO_GAME_SAVING_PATH2_G: {
        if (!writeFile(at<uint32_t>(sym::iconName1G + 8u), at<uint32_t>(sym::iconSize1G + 8u), at<uint32_t>(kLoadSave + 0x108u), 0x158u))
            break;
        if (val(0x158u) == 0)
        {
            saveFailed();
            break;
        }
        // The game's state, as it is now, into a fresh save buffer.
        memAlloc(kGameSaveData, 0x130u);
        memSet(l.pGameSaveData.address, 0x130u);
        const uint32_t data = l.pGameSaveData.address, game = sym::frontEndVarG + 0x138u;
        guest_call<void>(sym::MemBlockCopy32, data, game, 0x130);
        at<int32_t>(data + 0x124u) = at<int32_t>(sym::frontEndVarG + 0x25cu);
        at<uint16_t>(data + 0x12cu) = at<uint16_t>(sym::frontEndVarG + 0x264u);
        at<int32_t>(data + 0x44u) = at<int32_t>(sym::rcmusicVarG + 0x20u);
        at<uint16_t>(data + 0x12eu) = at<uint16_t>(sym::frontEndVarG + 0x266u);
        at<int32_t>(data + 0x48u) = at<int32_t>(sym::revSfxVarG + 0x1144u);
        at<int32_t>(data + 0x4cu) = at<int32_t>(sym::sfxVarG + 8u);
        LoadSaveInsertGameSaveData();
        const uint32_t buffer = l.pSaveGameBuffer.address;
        at<uint32_t>(buffer + 0x200u) = checksum(buffer);
        next(STATE_DO_GAME_SAVING_PATH1_C, 0);
        break;
    }
    // Replacing the files of a save that did not load: each one that is there is deleted first.
    case STATE_DO_GAME_SAVING_PATH3_A:
    case STATE_DO_GAME_SAVING_PATH3_C:
    case STATE_DO_GAME_SAVING_PATH3_E:
    case STATE_DO_GAME_SAVING_PATH3_G:
    case STATE_DO_GAME_SAVING_PATH3_I: {
        const uint32_t step = (static_cast<uint32_t>(l.status) - STATE_DO_GAME_SAVING_PATH3_A) / 2u; // 0..4
        static constexpr uint32_t kResults[] = {0x15cu, 0x168u, 0x174u, 0x180u, 0x18cu};
        const uint32_t name = step == 0 ? kIconSys : step == 4 ? sym::revolt2GameId : at<uint32_t>(sym::iconName1G + 4u * (step - 1u));
        const uint32_t result = kResults[step];
        if (!fileExists(name, result, result + 4u))
            break;
        if (val(result) != 1)
            saveFailed();
        else if (val(result + 4u) == 1)
            next(static_cast<eLoadSaveStatus>(l.status + 1), 8);
        else if (step == 4)
            l.status = STATE_DO_GAME_SAVING_PATH3_K;
        else
            next(static_cast<eLoadSaveStatus>(l.status + 2), 6);
        break;
    }
    case STATE_DO_GAME_SAVING_PATH3_B:
    case STATE_DO_GAME_SAVING_PATH3_D:
    case STATE_DO_GAME_SAVING_PATH3_F:
    case STATE_DO_GAME_SAVING_PATH3_H:
    case STATE_DO_GAME_SAVING_PATH3_J: {
        const uint32_t step = (static_cast<uint32_t>(l.status) - STATE_DO_GAME_SAVING_PATH3_B) / 2u;
        static constexpr uint32_t kResults[] = {0x164u, 0x170u, 0x17cu, 0x188u, 0x194u};
        const uint32_t name = step == 0 ? kIconSys : step == 4 ? sym::revolt2GameId : at<uint32_t>(sym::iconName1G + 4u * (step - 1u));
        const uint32_t result = kResults[step];
        if (!fileErase(name, result))
            break;
        if (val(result) != 1)
            saveFailed();
        else if (step == 4)
            l.status = STATE_DO_GAME_SAVING_PATH3_K;
        else
            next(static_cast<eLoadSaveStatus>(l.status + 1), 6);
        break;
    }
    case STATE_DO_GAME_SAVING_PATH3_K:
    gameIconHeader:
        at<int32_t>(sp) = 0x1e;
        CardCreateIconHeader(ptr32<anon_88ca8770>(kIcon), ptr32<sceVu0IVECTOR>(sym::bgColor1G), ptr32<sceVu0FVECTOR>(sym::lightDir1G),
                             ptr32<sceVu0FVECTOR>(sym::lightCol1G), ptr32<sceVu0FVECTOR>(sym::ambient1G), ptr32<char>(sym::revolt2GameSaveName),
                             ptr32<ptr32<char>>(sym::iconName1G), G(bgTrans1G));
        next(STATE_DO_GAME_SAVING_PATH2_D, 0);
        break;
    case STATE_SHOW_ERROR:
        // An error message stays three frames, then any select key returns to the card choice.
        if (l.waitCount != 0)
        {
            l.waitCount -= 1;
            break;
        }
        if (!hit(0xe) && !hit(0x1e))
            break;
        sfx(0x45);
        l.status = STATE_PRE_WAITING;
        end = End::ResetKeys;
        break;
    default: // STATE_GAME_LOADING and STATE_DO_GAME_LOADING do nothing; other values index past the original's table
        break;
    }
    if (end == End::ResetKeys)
    {
        resetHit(0xe);
        resetHit(0x1e);
    }
    if (G(frontEndVarG).flagPTT == 0)
    {
        RenderMemoryCards();
        guest_call<void>(sym::AMenuProcessAll, ms().pActiveMenu);
        G(frontEndVarG).fisrtDrawFlagPTT = 1;
    }
    else
        guest_call<void>(sym::BuildProgress, ms().pActiveMenu, l.pGameSaveData, l.cardSelected == CARD_1 ? 1 : 2);
    return 0;
}

// The two card pictures on the card-choice page; the unselected one is dimmed by a black rectangle drawn first.
void RenderMemoryCards()
{
    GuestFrame frame(0x70u);
    const sLoadSaveClass &l = ls();
    if (static_cast<uint32_t>(l.status) >= STATE_GET_CARDDIRECTORY || l.status == STATE_FIRST_TIME)
        return;
    const uint32_t sp = frame.local<uint8_t>(0).address, rect = sp + 0x20u; // sSpriteParams at 0, sRect at 0x20
    _sSpriteParams &sprite = at<_sSpriteParams>(sp);
    sprite.width = 0x40;
    sprite.height = 0x60;
    sprite.blue = 0x80;
    sprite.xScale = 0x140;
    sprite.yScale = 0xa0;
    sprite.xOffset = 0;
    sprite.yOffset = 0;
    sprite.red = 0x80;
    sprite.green = 0x80;
    sprite.y = 0x37;
    const auto card = [&](int32_t status, eCard which, int16_t x, uint32_t rectOtz, uint32_t spriteOtz, ptr32<_sSprite> picture) {
        if (!present(status))
            return;
        sprite.flags = 0;
        sprite.x = x;
        if (l.cardSelected != which)
        {
            const int16_t y = sprite.y;
            at<uint32_t>(rect) = 1; // flags
            at<int16_t>(rect + 4u) = x;
            at<int16_t>(rect + 6u) = y;
            at<int16_t>(rect + 8u) = static_cast<int16_t>(x + 0x50);
            at<int16_t>(rect + 0xau) = y;
            at<int16_t>(rect + 0xcu) = static_cast<int16_t>(x + 0x50);
            at<int16_t>(rect + 0xeu) = static_cast<int16_t>(y + 0x3c);
            at<int16_t>(rect + 0x10u) = x;
            at<int16_t>(rect + 0x12u) = static_cast<int16_t>(y + 0x3c);
            at<uint8_t>(rect + 0x14u) = 0;
            at<uint8_t>(rect + 0x15u) = 0;
            at<uint8_t>(rect + 0x16u) = 0;
            guest_call<void>(sym::RectRender, rect, at<ee_float>(rectOtz));
            sprite.flags = 1;
        }
        guest_call<void>(sym::SpriteRenderScaled, picture, sp, at<ee_float>(spriteOtz));
    };
    card(l.card1Status, CARD_1, 0xd3, sym::lit4_327674, sym::lit4_327678, l.loadSaveData.pSprite[0]);
    card(l.card2Status, CARD_2, 0x172, sym::lit4_32767c, sym::lit4_327680, l.loadSaveData.pSprite[1]);
}

namespace {

// The first entry with the given prefix whose save file opens (the file named after prefixFile, or after the entry
// itself), or -1. The game file's name is built once; a track's for each entry.
int32_t findSave(eCard card, uint32_t fileName, bool gameSave)
{
    if (gameSave)
        savePath(fileName, sym::revolt2GameId);
    for (int32_t i = 0; i < entriesFound(card); ++i)
    {
        const uint32_t name = entryName(card, i);
        if (!startsWith(gameSave ? sym::revolt2GameId : sym::revolt2TrackId, name))
            continue;
        if (!gameSave)
            savePath(fileName, name);
        if (CardFileExistsNow(card, ptr32<char>(fileName)) != 0)
            return i;
    }
    return -1;
}

void showError(int message)
{
    ls().status = STATE_SHOW_ERROR;
    showMsg(message);
    ls().waitCount = 3;
}

// The track save list for the save or load page: names into the scroll menu's text groups.
void listTracks(int message, uint32 count, eLoadSaveStatus state)
{
    const ptr32<sAMenuHeader> header = msg(message);
    ms().pActiveMenu = header;
    for (uint32_t i = 0; i < 16u; ++i)
        guest_call<void>(sym::AMenuAddTextToTGRP, sym::scrollMenuTGRPsG + 0x10u * i, 0);
    for (uint32_t i = 0; i < count; ++i)
        guest_call<void>(sym::AMenuAddTextToTGRP, sym::scrollMenuTGRPsG + 0x10u * i, sym::trackSaveNamesG + 0x10u * i);
    const uint32_t h = header.address;
    at<int8_t>(h + 0x70u) = -1;                                              // scrollStart
    at<uint8_t>(h + 0x62u) = static_cast<uint8_t>(count);                    // menuItems
    at<uint16_t>(h + 0x32u) = static_cast<uint16_t>(at<uint16_t>(h + 0x32u) | 0x40u); // evenMoreFlags
    at<uint8_t>(h + 0x63u) = 0;                                              // selection
    at<int16_t>(h + 0x6au) = 0x80;                                           // height
    ls().status = state;
    at<uint32_t>(h + 4u) = sym::scrollMenuTGRPsG;                            // pMenuStart
    at<int16_t>(sym::frontEndVarG + 0x2cau) = 0;                             // scrollPTT
}

} // namespace

// After the card was chosen: the page the mode leads to on this card.
void LoadSaveCheckModeAvailable()
{
    GuestFrame frame(0x120u);
    const uint32_t sp = frame.local<uint8_t>(0).address, fileName = sp + 0x40u;
    sLoadSaveClass &l = ls();
    switch (static_cast<int32_t>(l.loadSaveMode))
    {
    case LOADSAVE_MODE_GAMESAVE: {
        l.gameSaveLoaded = 0;
        if ((l.cardSelected == CARD_1 ? l.card1Status : l.card2Status) == CARD_UNFORMATTED)
        {
            const int m = l.cardSelected == CARD_1 ? 10 : 0x1d;
            l.status = STATE_CONFIRMING_FORMAT;
            showMsg(m);
            msg(m)->selection = 1;
            return;
        }
        memAlloc(kSaveBuffer, 0x2000u);
        memSet(l.pSaveGameBuffer.address, 0x2000u);
        if (findSave(l.cardSelected, fileName, true) == -1)
        {
            if (freeClusters(l.cardSelected) >= 0xce)
            {
                l.status = STATE_GAME_SAVE_Q;
                showMsg(0x17);
            }
            else
            {
                memFree(kSaveBuffer);
                showError(forCard(0x23, 0x24));
            }
            return;
        }
        savePath(sp, sym::revolt2GameId);
        if (CardReadData(l.cardSelected, ptr32<char>(sp), 0x2000u, l.pSaveGameBuffer.cast<uint8>()) == 0)
        {
#if RC_VERSION_EU
            memFree(kSaveBuffer);
            showError(0x15);
#else
            l.status = STATE_GAME_SAVE_Q;
            showMsg(0x17);
#endif
            return;
        }
        const uint32_t buffer = l.pSaveGameBuffer.address;
        if (checksum(buffer) != at<uint32_t>(buffer + 0x200u))
        {
#if RC_VERSION_EU
            memFree(kSaveBuffer);
            showError(0x1a);
#else
            l.status = STATE_GAME_SAVE_Q;
            showMsg(0x17);
#endif
            return;
        }
        memAlloc(kGameSaveData, 0x130u);
        memSet(l.pGameSaveData.address, 0x130u);
        LoadSaveExtractGameSaveData();
        l.status = STATE_GAME_SAVE_Q;
        showMsg(0x1e); // overwrite the save?
        msg(0x1e)->selection = 1;
        l.gameSaveLoaded = 1;
        return;
    }
    case LOADSAVE_MODE_GAMELOAD: {
        if (findSave(l.cardSelected, fileName, true) == -1)
        {
            l.status = STATE_SHOW_ERROR;
            showMsg(5);
            return;
        }
        memAlloc(kSaveBuffer, 0x2000u);
        memSet(l.pSaveGameBuffer.address, 0x2000u);
        savePath(sp, sym::revolt2GameId);
        if (CardReadData(l.cardSelected, ptr32<char>(sp), 0x2000u, l.pSaveGameBuffer.cast<uint8>()) == 0)
        {
            memFree(kSaveBuffer);
            showError(0x15);
            return;
        }
        const uint32_t buffer = l.pSaveGameBuffer.address;
        if (checksum(buffer) != at<uint32_t>(buffer + 0x200u))
        {
            memFree(kSaveBuffer);
            showError(0x1a);
            return;
        }
        memAlloc(kGameSaveData, 0x130u);
        memSet(l.pGameSaveData.address, 0x130u);
        LoadSaveExtractGameSaveData();
        l.status = STATE_GAME_LOAD_Q;
        showMsg(0x18);
#if !RC_VERSION_EU
        msg(0x18)->selection = 0;
#endif
        return;
    }
    case LOADSAVE_MODE_TRACKSAVE: {
        if ((l.cardSelected == CARD_1 ? l.card1Status : l.card2Status) == CARD_UNFORMATTED)
        {
            const int m = l.cardSelected == CARD_1 ? 10 : 0x1d;
            l.status = STATE_CONFIRMING_FORMAT;
            showMsg(m);
            msg(m)->selection = 1;
            return;
        }
        if (findSave(l.cardSelected, fileName, false) == -1)
        {
            if (freeClusters(l.cardSelected) >= 0x4b)
            {
                l.status = STATE_TRACK_SAVE_TYPE_1;
                showMsg(8);
            }
            else
                showError(forCard(0x25, 0x26));
            return;
        }
        listTracks(0xc, LoadSaveGetRevoltTrackSaveNames(l.cardSelected), STATE_TRACK_SAVE_TYPE_2);
        return;
    }
    case LOADSAVE_MODE_TRACKLOAD:
        if (findSave(l.cardSelected, fileName, false) == -1)
        {
            showError(7);
            return;
        }
        listTracks(0xd, LoadSaveGetRevoltTrackLoadNames(l.cardSelected), STATE_TRACK_LOAD_SELECT);
        return;
    default:
        return;
    }
}

} // namespace rc

RC_DECOMP_FN_BLOCKING_SETUP(LoadSaveProcess, rc::fuzz::lsProcess)
RC_DECOMP_FN_SETUP(RenderMemoryCards, rc::fuzz::lsRenderCards)
RC_DECOMP_FN_BLOCKING_SETUP(LoadSaveCheckModeAvailable, rc::fuzz::lsModeAvailable)

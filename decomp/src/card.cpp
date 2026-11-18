// Decompiled from CARD.c: the game's memory card layer over the SDK's libmc. Each card (port 0 or 1, slot 0) has a
// directory in cardVarG (its status, a refresh flag, its free clusters, the entries sceMcGetDir found). "Now" functions
// wait for the command (sceMcSync mode 0); the others start a command and the save/load state machine (LOADSAVE.c)
// polls for its end with the CardWaitASync* functions (sceMcSync mode 1). libmc is called through the function table
// (card::mc): the runtime's sceMc* in the game, the setups' scripted fakes offline (card_fuzz.cpp).
//
// Locals passed by address to libmc sit in the original's frame (GuestFrame), so a result libmc does not write is the
// same stale stack word for both.

#include "card.h"
#include "card_fuzz.h"

namespace rc {

using namespace card;

namespace {

// The status the card's directory takes from what sceMcGetInfo found (a PS2 card: unformatted or ready; anything
// else: not present), and its free clusters for a PS2 card.
void setStatusFromInfo(int32_t card, int32_t type, int32_t formatted, int32_t free)
{
    if (type == 2)
    {
        status(card) = formatted == 0 ? CARD_UNFORMATTED : CARD_OKAY;
        freeClusters(card) = free;
    }
    else
        status(card) = CARD_NOT_PRESENT; // a PS1 card (1), a PocketStation (3) or no card
}

} // namespace

// Asks libmc for the card's type, free space and format and waits for the answer. A new card (-1) or an unformatted
// one (-2) marks the directory for a refresh; any other error means no card.
eCardStatus CardGetStatusNow(eCard card)
{
    GuestFrame frame(0x40u);
    const uint32_t sp = frame.local<int32_t>(0).address; // type 0x0, free 0x4, formatted 0x8, result 0xc
    if (mc(sym::sceMcGetInfo, card, 0, sp, sp + 4u, sp + 8u) != 0)
        return CARD_LIBRARY_BUG;
    mc(sym::sceMcSync, 0, 0, sp + 0xcu);
    const int32_t result = at<int32_t>(sp + 0xcu);
    if (result == -1 || result == -2)
        refreshNeeded(card) = 1;
    else if (result != 0)
    {
        status(card) = CARD_NOT_PRESENT;
        return CARD_NOT_PRESENT;
    }
    setStatusFromInfo(card, at<int32_t>(sp), at<int32_t>(sp + 8u), at<int32_t>(sp + 4u));
    return static_cast<eCardStatus>(status(card));
}

// mode 1 starts sceMcGetInfo (into typeG, freeG, formattedG) and returns whether it started; mode 0 polls for its
// end: false while it runs, else true with the status in *cardStatus, as CardGetStatusNow finds it.
bool_ CardGetStatusASync(eCard card, uint32 mode, ptr32<eCardStatus> cardStatus)
{
    GuestFrame frame(0x50u);
    if (mode != 0)
        return mc(sym::sceMcGetInfo, card, 0, sym::typeG, sym::freeG, sym::formattedG) == 0 ? 1 : 0;
    const uint32_t sp = frame.local<int32_t>(0).address;
    if (mc(sym::sceMcSync, 1, 0, sp) == 0)
        return 0;
    const int32_t result = at<int32_t>(sp);
    if (result == -1 || result == -2)
        refreshNeeded(card) = 1;
    else if (result != 0)
    {
        status(card) = CARD_NOT_PRESENT;
        *cardStatus = CARD_NOT_PRESENT;
        return 1;
    }
    setStatusFromInfo(card, G(typeG), G(formattedG), G(freeG));
    *cardStatus = static_cast<eCardStatus>(status(card));
    return 1;
}

// Fills a save's icon.sys header: the "PS2D" mark, the title (ASCII first, then JIS over it), the line break, the
// background, lights and the three icon file names.
void CardCreateIconHeader(ptr32<anon_88ca8770> pIcon, ptr32<sceVu0IVECTOR> pBgColor, ptr32<sceVu0FVECTOR> pLightDir,
                          ptr32<sceVu0FVECTOR> pLightCol, ptr32<sceVu0FVECTOR> pAmbient, ptr32<char> title,
                          ptr32<ptr32<char>> iconName, uint32 bgTrans)
{
    const uint16_t lineBreakPos = stack_arg<uint16_t>(0);
    GuestFrame frame(0xb0u);
    const uint32_t icon = pIcon.address;
    std::memset(guest_host(icon), 0, 0x3c4u);
    at<uint32_t>(icon) = at<uint32_t>(sym::gNumAnimTextures + 0x14u); // "PS2D"
    at<int8_t>(icon + 4u) = at<int8_t>(sym::gNumAnimTextures + 0x18u);
    std::strncpy(str(icon + 0xc0u), title.get(), 0x20u);
    guest_call<void>(sym::ASCIIStringToJIS, title, icon + 0xc0u, 0x20);
    at<uint16_t>(icon + 6u) = lineBreakPos;
    at<uint32_t>(icon + 0xcu) = bgTrans;
    std::memmove(guest_host(icon + 0x10u), guest_host(pBgColor.address), 0x40u);
    std::memmove(guest_host(icon + 0x50u), guest_host(pLightDir.address), 0x30u);
    std::memmove(guest_host(icon + 0x80u), guest_host(pLightCol.address), 0x30u);
    std::memmove(guest_host(icon + 0xb0u), guest_host(pAmbient.address), 0x10u);
    std::strcpy(str(icon + 0x104u), iconName[0].get());
    std::strcpy(str(icon + 0x144u), iconName[1].get());
    std::strcpy(str(icon + 0x184u), iconName[2].get());
}

// Reads a whole file: the file must open for writing first (a test that it exists and is not read-only), then it is
// read; true only if all numBytes came back.
int32 CardReadData(eCard card, ptr32<char> fileName, uint32 numBytes, ptr32<uint8> pData)
{
    GuestFrame frame(0x70u);
    const uint32_t sp = frame.local<int32_t>(0).address; // descriptors 0x0 and 0x8, results 0x4, 0xc (length), 0x10
    bool exists = false;
    if (mc(sym::sceMcOpen, card, 0, fileName, 2) == 0)
    {
        mc(sym::sceMcSync, 0, 0, sp);
        if (at<int32_t>(sp) >= 0)
        {
            mc(sym::sceMcClose, at<int32_t>(sp));
            mc(sym::sceMcSync, 0, 0, sp + 4u);
            exists = at<int32_t>(sp + 4u) == 0;
        }
    }
    if (!exists)
        return 0;
    if (mc(sym::sceMcOpen, card, 0, fileName, 1) != 0)
        return 0;
    mc(sym::sceMcSync, 0, 0, sp + 8u);
    const int32_t descriptor = at<int32_t>(sp + 8u);
    if (descriptor < 0)
        return 0;
    if (mc(sym::sceMcRead, descriptor, pData, numBytes) != 0)
    {
        mc(sym::sceMcClose, at<int32_t>(sp + 8u));
        return 0;
    }
    mc(sym::sceMcSync, 0, 0, sp + 0xcu);
    mc(sym::sceMcClose, at<int32_t>(sp + 8u));
    mc(sym::sceMcSync, 0, 0, sp + 0x10u);
    if (at<int32_t>(sp + 0x10u) != 0)
        return 0;
    return numBytes == at<uint32_t>(sp + 0xcu) ? 1 : 0;
}

void CardConstruct() { at<int32_t>(kCardVars) = 0; }

// Marks both directories for a refresh, selects card 1 and starts libmc (its messages printed as the original does).
void CardInit()
{
    GuestFrame frame(0x20u);
    CardMarkAllRefreshNeeded();
    at<int32_t>(kCardVars + 4u) = CARD_1;
    at<int32_t>(kCardVars + 0xcu) = SLOT_NONE;
    at<int32_t>(kCardVars + 8u) = 0; // CARD_FUNC_NONE
    const int32_t result = mc(sym::sceMcInit);
    if (result == -0x78)
        guest_call<void>(sym::printf, sym::curvedata_541 + 0xbf8u); // "Version of mcserv.irx is old"
    else if (result < -0x77)
    {
        if (result == -0x79)
            guest_call<void>(sym::printf, sym::curvedata_541 + 0xc18u); // "Version of mcman.irx is old"
    }
    else if (result == -0x65)
        guest_call<void>(sym::printf, sym::curvedata_541 + 0xbd0u); // "Memory Cards Initialisation Failed"
    else if (result == 0)
        guest_call<void>(sym::printf, sym::curvedata_541 + 0xbb0u); // "Memory Cards Initialised Ok"
    at<int32_t>(kCardVars) = 1;
}

void CardUnInit() { at<int32_t>(kCardVars) = 0; }

// Polls the running command: false while it runs; else *result says whether it returned 0.
bool_ CardWaitASyncFuncResult(ptr32<bool_> result)
{
    GuestFrame frame(0x30u);
    const uint32_t sp = frame.local<int32_t>(0).address;
    if (mc(sym::sceMcSync, 1, 0, sp) == 0)
        return 0;
    *result = at<int32_t>(sp) == 0 ? 1 : 0;
    return 1;
}

// Polls a write: when it ends, the bytes written go to cardVarG and *result is set to true even after an error (the
// original clears it first on an error, then sets it).
bool_ CardWaitASyncWriteResult(ptr32<bool_> result)
{
    GuestFrame frame(0x40u);
    const uint32_t sp = frame.local<int32_t>(0).address;
    if (mc(sym::sceMcSync, 1, 0, sp) == 0)
        return 0;
    const int32_t written = at<int32_t>(sp);
    if (written < 0)
        *result = 0;
    at<int32_t>(kBytesWritten) = written;
    *result = 1;
    return 1;
}

namespace {

// CardWaitASyncOpenResult and CardWaitASyncExistResult, the same code: the descriptor is kept on success.
bool_ waitOpen(ptr32<bool_> result)
{
    GuestFrame frame(0x40u);
    const uint32_t sp = frame.local<int32_t>(0).address;
    if (mc(sym::sceMcSync, 1, 0, sp) == 0)
        return 0;
    const int32_t descriptor = at<int32_t>(sp);
    if (descriptor >= 0)
    {
        at<int32_t>(kDescriptor) = descriptor;
        *result = 1;
    }
    else
        *result = 0;
    return 1;
}

} // namespace

bool_ CardWaitASyncOpenResult(ptr32<bool_> result) { return waitOpen(result); }

bool_ CardWaitASyncExistResult(ptr32<bool_> result) { return waitOpen(result); }

// Waits for whatever command is running.
void CardWaitASyncFuncTerm()
{
    GuestFrame frame(0x20u);
    mc(sym::sceMcSync, 0, 0, frame.local<int32_t>(0).address);
}

int32 CardFileRename(eCard card, ptr32<char> oldName, ptr32<char> newName)
{
    GuestFrame frame(0x10u);
    return mc(sym::sceMcRename, card, 0, oldName, newName) == 0 ? 1 : 0;
}

int32 CardFormat(eCard card)
{
    GuestFrame frame(0x10u);
    return mc(sym::sceMcFormat, card, 0) == 0 ? 1 : 0;
}

int32 CardUnFormatNow(eCard card)
{
    GuestFrame frame(0x20u);
    if (mc(sym::sceMcUnformat, card, 0) != 0)
        return 0;
    const uint32_t sp = frame.local<int32_t>(0).address;
    mc(sym::sceMcSync, 0, 0, sp);
    return at<int32_t>(sp) == 0 ? 1 : 0;
}

// Starts opening the file for writing (the existence test CardWaitASyncExistResult ends).
int32 CardFileExists(eCard card, ptr32<char> fileName)
{
    GuestFrame frame(0x10u);
    return mc(sym::sceMcOpen, card, 0, fileName, 2) == 0 ? 1 : 0;
}

// Whether the file opens for writing (and closes again).
int32 CardFileExistsNow(eCard card, ptr32<char> fileName)
{
    GuestFrame frame(0x20u);
    if (mc(sym::sceMcOpen, card, 0, fileName, 2) != 0)
        return 0;
    const uint32_t sp = frame.local<int32_t>(0).address;
    mc(sym::sceMcSync, 0, 0, sp);
    if (at<int32_t>(sp) < 0)
        return 0;
    mc(sym::sceMcClose, at<int32_t>(sp));
    mc(sym::sceMcSync, 0, 0, sp + 4u);
    return at<int32_t>(sp + 4u) == 0 ? 1 : 0;
}

int32 CardFileErase(eCard card, ptr32<char> fileName)
{
    GuestFrame frame(0x10u);
    return mc(sym::sceMcDelete, card, 0, fileName) == 0 ? 1 : 0;
}

// Starts writing to the file CardWaitASyncOpenResult opened; closes it if the write did not start.
int32 CardCreateData(eCard card, ptr32<char> fileName, uint32 numBytes, ptr32<uint8> pData)
{
    GuestFrame frame(0x20u);
    if (mc(sym::sceMcWrite, at<int32_t>(kDescriptor), pData, numBytes) == 0)
        return 1;
    mc(sym::sceMcClose, at<int32_t>(kDescriptor));
    return 0;
}

int32 CardOpenFile(eCard card, ptr32<char> fileName, uint32 flags)
{
    GuestFrame frame(0x10u);
    return mc(sym::sceMcOpen, card, 0, fileName, flags) == 0 ? 1 : 0;
}

int32 CardCloseFile()
{
    GuestFrame frame(0x10u);
    return mc(sym::sceMcClose, at<int32_t>(kDescriptor)) == 0 ? 1 : 0;
}

// Overwrites an existing file and waits: true only if every byte was written.
int32 CardWriteData(eCard card, ptr32<char> fileName, uint32 numBytes, ptr32<uint8> pData)
{
    GuestFrame frame(0x60u);
    const uint32_t sp = frame.local<int32_t>(0).address; // descriptor 0x0, length 0x4, result 0x8
    if (CardFileExists(card, fileName) == 0)
        return 0;
    if (mc(sym::sceMcOpen, card, 0, fileName, 2) != 0)
        return 0;
    mc(sym::sceMcSync, 0, 0, sp);
    if (at<int32_t>(sp) < 0)
        return 0;
    if (mc(sym::sceMcWrite, at<int32_t>(sp), pData, numBytes) != 0)
    {
        mc(sym::sceMcClose, at<int32_t>(sp));
        return 0;
    }
    mc(sym::sceMcSync, 0, 0, sp + 4u);
    mc(sym::sceMcClose, at<int32_t>(sp));
    mc(sym::sceMcSync, 0, 0, sp + 8u);
    if (at<int32_t>(sp + 8u) != 0)
        return 0;
    return numBytes == at<uint32_t>(sp + 4u) ? 1 : 0;
}

void CardMarkAllRefreshNeeded()
{
    refreshNeeded(CARD_2) = 1;
    refreshNeeded(CARD_1) = 1;
}

eCard CardPresent(eCard, int32_t) { return CARD_NONE; }

int32 CardCreateDirectory(eCard card, ptr32<char> dirName)
{
    GuestFrame frame(0x10u);
    return mc(sym::sceMcMkdir, card, 0, dirName) == 0 ? 1 : 0;
}

int32 CardChangeDirectory(eCard card, ptr32<char> path, ptr32<char> pwd)
{
    GuestFrame frame(0x10u);
    return mc(sym::sceMcChdir, card, 0, path, pwd) == 0 ? 1 : 0;
}

int32 CardChangeDirectoryNow(eCard card, ptr32<char> path, ptr32<char> pwd)
{
    GuestFrame frame(0x20u);
    if (mc(sym::sceMcChdir, card, 0, path, pwd) != 0)
        return 0;
    const uint32_t sp = frame.local<int32_t>(0).address;
    mc(sym::sceMcSync, 0, 0, sp);
    return at<int32_t>(sp) == 0 ? 1 : 0;
}

// Lists the entries matching name into dirTable and keeps their count (0 after an error).
int32 CardGetDirectoryNow(eCard card, ptr32<char> name, int32 maxEnt, ptr32<anon_57bcf673> dirTable)
{
    GuestFrame frame(0x40u);
    if (mc(sym::sceMcGetDir, card, 0, name, 0, maxEnt, dirTable) == 0)
    {
        const uint32_t sp = frame.local<int32_t>(0).address;
        mc(sym::sceMcSync, 0, 0, sp);
        const int32_t found = at<int32_t>(sp);
        if (found >= 0)
        {
            entriesFound(card) = found;
            return 1;
        }
    }
    entriesFound(card) = 0;
    return 0;
}

int32 Card2Channel(eCard card) { return card == CARD_2 ? 0x10 : 0; }

} // namespace rc

RC_DECOMP_FN_BLOCKING_SETUP(CardGetStatusNow, rc::fuzz::cardStatusNow)
RC_DECOMP_FN_BLOCKING_SETUP(CardGetStatusASync, rc::fuzz::cardStatusASync)
RC_DECOMP_FN_SETUP(CardCreateIconHeader, rc::fuzz::cardIconHeader)
RC_DECOMP_FN_BLOCKING_SETUP(CardReadData, rc::fuzz::cardFile)
RC_DECOMP_FN(CardConstruct)
RC_DECOMP_FN_BLOCKING_SETUP(CardInit, rc::fuzz::cardPlain)
RC_DECOMP_FN(CardUnInit)
RC_DECOMP_FN_BLOCKING_SETUP(CardWaitASyncFuncResult, rc::fuzz::cardWait)
RC_DECOMP_FN_BLOCKING_SETUP(CardWaitASyncWriteResult, rc::fuzz::cardWait)
RC_DECOMP_FN_BLOCKING_SETUP(CardWaitASyncOpenResult, rc::fuzz::cardWait)
RC_DECOMP_FN_BLOCKING_SETUP(CardWaitASyncExistResult, rc::fuzz::cardWait)
RC_DECOMP_FN_BLOCKING_SETUP(CardWaitASyncFuncTerm, rc::fuzz::cardPlain)
RC_DECOMP_FN_BLOCKING_SETUP(CardFileRename, rc::fuzz::cardRename)
RC_DECOMP_FN_BLOCKING_SETUP(CardFormat, rc::fuzz::cardPlain)
RC_DECOMP_FN_BLOCKING_SETUP(CardUnFormatNow, rc::fuzz::cardPlain)
RC_DECOMP_FN_BLOCKING_SETUP(CardFileExists, rc::fuzz::cardFile)
RC_DECOMP_FN_BLOCKING_SETUP(CardFileExistsNow, rc::fuzz::cardFile)
RC_DECOMP_FN_BLOCKING_SETUP(CardFileErase, rc::fuzz::cardFile)
RC_DECOMP_FN_BLOCKING_SETUP(CardCreateData, rc::fuzz::cardFile)
RC_DECOMP_FN_BLOCKING_SETUP(CardOpenFile, rc::fuzz::cardFile)
RC_DECOMP_FN_BLOCKING_SETUP(CardCloseFile, rc::fuzz::cardPlain)
RC_DECOMP_FN_BLOCKING_SETUP(CardWriteData, rc::fuzz::cardFile)
RC_DECOMP_FN(CardMarkAllRefreshNeeded)
RC_DECOMP_FN(CardPresent)
RC_DECOMP_FN_BLOCKING_SETUP(CardCreateDirectory, rc::fuzz::cardFile)
RC_DECOMP_FN_BLOCKING_SETUP(CardChangeDirectory, rc::fuzz::cardRename)
RC_DECOMP_FN_BLOCKING_SETUP(CardChangeDirectoryNow, rc::fuzz::cardRename)
RC_DECOMP_FN_BLOCKING_SETUP(CardGetDirectoryNow, rc::fuzz::cardGetDir)
RC_DECOMP_FN(Card2Channel)

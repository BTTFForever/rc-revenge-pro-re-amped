// Decompiled from TEDIT.c: auto-complete (Complete Track) and random tracks. Both search with a stack of up to 100
// entries (sAutoCompStackData, 0x9c bytes: the cursor before the piece, then the edit menu's 35 cells scored and
// sorted best first): each step scores every cell by trying it with the error check (TEditGetAutoCompPieceScore),
// places the best not yet tried and goes on, or steps back when none is left. See tedit.h.

#include "tedit.h"
#include "tedit_fuzz.h"

namespace rc {

using namespace tedit;

namespace {

constexpr uint32_t kEntry = 0x9cu, kStackBytes = 0x3cf0u; // 100 entries
constexpr uint32_t kScratch = sym::scratchPadG;

R5900Context &context() { return g_runtime->eeScheduler().runningContext(); }

int16_t add16(int32_t score, int32_t n) { return static_cast<int16_t>(score + n); }

// |a - b| as the original takes it: 16 bits.
int32_t distance(int32_t a, int32_t b) { return a - b > 0 ? static_cast<int16_t>(a - b) : static_cast<int16_t>(b - a); }

// The cursor's pieces tried from (x, y, direction, elevation) in a frame, with the error check on: true if they fit.
// Leaves the last piece's output slope in *slope (unchanged with no pieces).
bool tryPieces(uint32_t x, uint32_t y, uint32_t direction, uint32_t elevation, uint32_t *slope)
{
    sTEditClass &t = te();
    at<int32_t>(direction) = t.cursor.direction;
    at<int16_t>(elevation) = t.cursor.elevation;
    at<int16_t>(x) = t.cursor.xPos;
    at<int16_t>(y) = t.cursor.yPos;
    t.placementErrorChecking = 1;
    t.resourceErrorCheckMemoryUsed = t.resourceMemoryUsed;
    t.errorFlags = 0;
    for (uint32_t module = t.cursor.pCursorModuleList.address; module != 0; module = at<uint32_t>(module))
        *slope = static_cast<uint32_t>(TEditAdjustCursorModule(ptr32<_sCursorModule>(module), ptr32<int16>(x), ptr32<int16>(y),
                                                               ptr32<eDirection>(direction), ptr32<int16>(elevation), 2));
    const uint32_t errors = t.errorFlags;
    t.placementErrorChecking = 0;
    if (errors != 0)
    {
        t.errorFlags = 0;
        return false;
    }
    return true;
}

// The cursor's pieces placed from the locals of a frame, each added to the track; returns how many (a byte).
int8_t placePieces(uint32_t x, uint32_t y, uint32_t direction, uint32_t elevation, uint32_t *slope)
{
    int8_t count = 0;
    for (uint32_t module = te().cursor.pCursorModuleList.address; module != 0; module = at<uint32_t>(module))
    {
        *slope = static_cast<uint32_t>(TEditAdjustCursorModule(ptr32<_sCursorModule>(module), ptr32<int16>(x), ptr32<int16>(y),
                                                               ptr32<eDirection>(direction), ptr32<int16>(elevation), 2));
        AddTrackModuleToTrackModuleList(static_cast<int32_t>(at<uint32_t>(module + 4u) - kEditModuleList) >> 2, 1);
        count = static_cast<int8_t>(count + 1);
    }
    return count;
}

// A new stack entry: the cursor as it is, then every cell's score (worked out on a copy in scratchPadG).
void startEntry(uint32_t entry)
{
    const sTEditClass &t = te();
    at<uint16_t>(entry + 2u) = static_cast<uint16_t>(t.cursor.xPos);
    at<int32_t>(entry + 8u) = t.cursor.currentSlope;
    at<uint16_t>(entry + 4u) = static_cast<uint16_t>(t.cursor.yPos);
    at<int32_t>(entry + 0xcu) = t.cursor.direction;
    at<int16_t>(entry + 6u) = t.cursor.elevation;
    std::memcpy(guest_host(kScratch), guest_host(entry), kEntry);
    TEditGetAutoCompAllPiecesScore(ptr32<_sAutoCompStackData>(kScratch));
}

// TEditGetAutoCompPieceScore with the registers its caller leaves ($s6, $s7, $fp).
int16_t pieceScore(uint32_t slope, int32_t newYDist, int32_t oldYDist);

bool hasPiece(uint32_t entry)
{
    const uint32_t tried = at<uint8_t>(entry);
    return tried < 0x23u && at<int16_t>(entry + 0x12u + tried * 4u) != -1;
}

} // namespace

// How good the cursor's pieces are as the next step back to the start grid: -1 if they do not fit (or would block or
// miss the start), else a score from the heights, the side of the start the track is on and the distances.
//
// With no pieces on the cursor the slope is the caller's $s6, and with a start orientation outside 0-3 the four
// distances are what the caller left ($fp, $s7 and two words of the stack).
int16 TEditGetAutoCompPieceScore()
{
    R5900Context &ctx = context();
    return pieceScore(getRegU32(&ctx, 22), static_cast<int32_t>(getRegU32(&ctx, 23)), static_cast<int32_t>(getRegU32(&ctx, 30)));
}

namespace {

int16_t pieceScore(uint32_t slope, int32_t newYDist, int32_t oldYDist)
{
    GuestFrame frame(0xc0);
    const uint32_t sp = frame.local<uint8_t>(0).address; // x 0x0, y 0x2, elevation 0x4, direction 0x8
    sTEditClass &t = te();
    if (!tryPieces(sp, sp + 2u, sp + 8u, sp + 4u, &slope))
        return -1;
    const int32_t startElevation = t.trackDesc.startElevation;
    const auto levels = [](int32_t d)
    {
        d = (d > -1 ? d : d + 0xff) >> 8;
        return static_cast<int32_t>(static_cast<int16_t>(d < 0 ? -d : d));
    };
    const int32_t oldLevels = levels(t.cursor.elevation - startElevation);
    const int32_t newLevels = levels(at<int16_t>(sp + 4u) - startElevation);
    int32_t score = 0;
    if (oldLevels <= 0)
    {
        if (slope == 0)
            score = newLevels != 0 ? 0 : 0x14;
    }
    else if (slope == 0)
    {
        if (newLevels == 0)
            score = 0x28;
        else if (newLevels != 1)
            score = static_cast<int16_t>((oldLevels - newLevels) * 10);
    }
    else
    {
        const int32_t steps = slope == 1u || slope == 0xffffffffu ? 1 : 2;
        score = newLevels == steps ? 0x23 : static_cast<int16_t>((oldLevels - newLevels) * 6);
    }

    const uint32_t orientation = static_cast<uint32_t>(t.trackDesc.startOrientation);
    int32_t oldXDist = 0, newXDist = 0;
    if (orientation > 3u)
    {
        oldXDist = at<int32_t>(sp + 0xcu);
        newXDist = at<int32_t>(sp + 0x10u);
    }
    else
    {
        // The track must come back to the unit behind the start grid: (bx, by), along the axis the start faces.
        const bool alongY = orientation == 0u || orientation == 2u;
        const int32_t sx = t.trackDesc.startXPos, sy = t.trackDesc.startYPos;
        const int32_t bx = orientation == 3u ? sx + 1 : orientation == 1u ? sx - 1 : sx;
        const int32_t by = orientation == 0u ? sy - 1 : orientation == 2u ? sy + 1 : sy;
        // The distances are taken from the unit behind, but for both east and west from the one at x + 1.
        const int32_t dx = alongY ? sx : sx + 1, dy = by;
        oldXDist = distance(t.cursor.xPos, dx);
        newXDist = distance(at<int16_t>(sp), dx);
        oldYDist = distance(t.cursor.yPos, dy);
        newYDist = distance(at<int16_t>(sp + 2u), dy);
        at<int32_t>(sp + 0xcu) = oldXDist;
        at<int32_t>(sp + 0x10u) = newXDist;
        if (TEditAutoCompIsStartGridBlocked(static_cast<int16_t>(bx), static_cast<int16_t>(by)) == 1)
            return -1;
        const int32_t x = at<int16_t>(sp), y = at<int16_t>(sp + 2u);
        if (x == bx && y == by)
        {
            if (newLevels != 0)
                return -1;
            if (slope != 0)
                return -1;
        }
        // Along the start's axis: `along` the track's coordinate, `across` the other; forward the start's direction.
        const int32_t direction = at<int32_t>(sp + 8u);
        const bool positive = orientation == 0u || orientation == 1u; // north and east run up their axis
        const int32_t start = alongY ? sy : sx, cursorAlong = alongY ? t.cursor.yPos : t.cursor.xPos;
        const int32_t along = alongY ? y : x;
        const bool acrossAtStart = alongY ? x == sx : y == sy;
        const int32_t forward = static_cast<int32_t>(orientation), back = static_cast<int32_t>(orientation ^ 2u);
        const bool cursorBefore = positive ? cursorAlong < start : start < cursorAlong;
        const bool newBefore = positive ? along < start : start < along;
        if (cursorBefore)
        {
            if (along == start)
            {
                if (acrossAtStart)
                {
                    if (direction == forward)
                        score = add16(score, 100);
                }
                else if (direction == back)
                    score = add16(score, 6);
            }
            else if (!acrossAtStart)
            {
                if (!newBefore)
                    score = add16(score, -0x10);
            }
            else if (direction == forward)
                score = add16(score, 0x12);
            else if (direction == ((forward + 1) & 3) || direction == ((forward + 3) & 3))
                score = add16(score, 0xc);
        }
        else if (newBefore)
            score = add16(score, 6);
        else if (t.cursor.direction == forward && direction != forward)
            score = add16(score, 10);
    }
    if (!(oldXDist < newXDist))
        score = add16(score, 8);
    else if (newXDist < oldXDist + 1)
        score = add16(score, 4);
    if (!(oldYDist < newYDist))
        score = add16(score, 8);
    else if (newYDist < oldYDist + 1)
        score = add16(score, 4);
    return static_cast<int16_t>(score > -1 ? score : 0);
}

} // namespace

// Scores the edit menu's 35 cells for the cursor (-1: not offered, restricted or no fit) and sorts them best first.
void TEditGetAutoCompAllPiecesScore(ptr32<_sAutoCompStackData> pAutoCompPiece)
{
    GuestFrame frame(0xd0);
    const uint32_t sp = frame.local<uint8_t>(0).address;
    const uint32_t pieces = pAutoCompPiece.address + 0x10u;
    sTEditClass &t = te();
    ClearCursorModuleList();
    for (int8_t row = 0; row < 5; row = static_cast<int8_t>(row + 1))
    {
        const uint32_t first = pieces + static_cast<uint32_t>(row) * 7u * 4u;
        if (t.editMenu.slopeRestrict[row] != 0)
        {
            for (uint32_t j = 0; j < 7u; ++j)
                at<int16_t>(first + j * 4u + 2u) = -1;
            continue;
        }
        const uint32_t cells = kGridSlopes + static_cast<uint32_t>(row) * 0x2cu;
        for (int8_t column = 0; column < 7; column = static_cast<int8_t>(column + 1))
        {
            const uint32_t c = static_cast<uint32_t>(column), piece = first + c * 4u;
            if (at<int8_t>(cells + 0x1cu + c) != 0 && at<int8_t>(cells + 0x23u + c) == 0)
            {
                const eSlope slope = TEditGetEditModuleInputSlope(static_cast<uint8_t>(row), static_cast<uint8_t>(column));
                if (t.cursor.currentSlope == slope)
                    AddEditModuleToCursorModuleList(ptr32<_sEditModuleDesc>(at<uint32_t>(cells + c * 4u)));
                else
                    TEditBuildTransitionCursorModules(t.cursor.currentSlope, slope);
                // The registers this function holds at the call: the cell pair's modules, the next column's counter, -1.
                const int16_t score = pieceScore(cells + (c & ~1u) * 4u, static_cast<int32_t>((c + 1u) << 24), -1);
                at<int16_t>(piece + 2u) = score;
                if (score != -1)
                {
                    at<uint8_t>(piece) = static_cast<uint8_t>(column);
                    at<uint8_t>(piece + 1u) = static_cast<uint8_t>(row);
                }
            }
            else
                at<int16_t>(piece + 2u) = -1;
            ClearCursorModuleList();
        }
    }
    // An insertion sort, highest score first.
    for (int8_t i = 1; i < 0x23; i = static_cast<int8_t>(i + 1))
    {
        const uint32_t held = at<uint32_t>(pieces + static_cast<uint32_t>(i) * 4u);
        at<uint32_t>(sp) = held;
        const int16_t score = static_cast<int16_t>(held >> 16);
        int8_t j = static_cast<int8_t>(i - 1);
        while (j >= 0 && at<int16_t>(pieces + static_cast<uint32_t>(j) * 4u + 2u) < score)
        {
            at<uint32_t>(pieces + static_cast<uint32_t>(j) * 4u + 4u) = at<uint32_t>(pieces + static_cast<uint32_t>(j) * 4u);
            j = static_cast<int8_t>(j - 1);
        }
        at<uint32_t>(pieces + static_cast<uint32_t>(static_cast<int32_t>(j) * 4 + 4)) = held;
    }
}

// Takes an entry's piece (its modules) off the track again and builds the grid and the cursor from what is left.
void TEditAutoCompRemoveLastPiece(ptr32<_sAutoCompStackData> pAutoCompPiece)
{
    {
        GuestFrame frame(0x30);
        for (uint32_t i = 0; i < at<uint8_t>(pAutoCompPiece.address + 1u); ++i)
            DeleteTrackModuleFromTrackModuleList();
        TEditClearTrack();
        TEditResetResourceMeter();
        generateTrackFromList(0xffffffffu); // $a3 as TEditClearTrack leaves it (the slope, were the track empty)
        TEditClearAllRestricts();
        TEditCheckCursorDirectionConstraints();
    }
    TEditCheckCursorMinMaxHeightConstraints();
}

// Places the entry's next untried cell: its module, or the transition pieces to its slope. With nothing on the cursor
// the slope is the caller's $fp.
void TEditAutoCompPlacePiece(ptr32<_sAutoCompStackData> pAutoCompPiece)
{
    uint32_t slope = getRegU32(&context(), 30);
    GuestFrame frame(0xb0);
    const uint32_t sp = frame.local<uint8_t>(0).address; // x 0x0, y 0x2, elevation 0x4, direction 0x8
    sTEditClass &t = te();
    const uint32_t piece = pAutoCompPiece.address + at<uint8_t>(pAutoCompPiece.address) * 4u;
    const int32_t row = at<int8_t>(piece + 0x11u), column = at<int8_t>(piece + 0x10u);
    at<int16_t>(sp + 4u) = t.cursor.elevation;
    at<int32_t>(sp + 8u) = t.cursor.direction;
    at<int16_t>(sp) = t.cursor.xPos;
    at<int16_t>(sp + 2u) = t.cursor.yPos;
    const eSlope desired = TEditGetEditModuleInputSlope(static_cast<uint8_t>(row), static_cast<uint8_t>(column));
    if (t.cursor.currentSlope == desired)
        AddEditModuleToCursorModuleList(ptr32<_sEditModuleDesc>(at<uint32_t>(kGridSlopes + static_cast<uint32_t>(row * 0x2c + column * 4))));
    else
        TEditBuildTransitionCursorModules(t.cursor.currentSlope, desired);
    const int8_t count = placePieces(sp, sp + 2u, sp + 8u, sp + 4u, &slope);
    at<uint8_t>(pAutoCompPiece.address + 1u) = static_cast<uint8_t>(count);
    t.cursor.currentSlope = static_cast<eSlope>(slope);
    t.cursor.yPos = at<int16_t>(sp + 2u);
    t.cursor.direction = static_cast<eDirection>(at<int32_t>(sp + 8u));
    t.cursor.elevation = at<int16_t>(sp + 4u);
    t.cursor.xPos = at<int16_t>(sp);
    TEditClearAllRestricts();
    TEditCheckCursorDirectionConstraints();
    TEditCheckCursorMinMaxHeightConstraints();
}

// The editor menu's Complete Track: searches for pieces that close the track, drawing a frame every sixth step (the
// track growing, the "working" box, which can stop it). Returns 0 if the track is complete (or was already), else
// what stopped it: 1 the 800 steps, 3 the player, 2 nothing left to try; the track is then as it was.
int32 TEditMenuDoCompleteTrack() { return completeTrack(getRegU32(&context(), 21)); }

// `time`: the camera's time, the caller's $s5 until the first step ends (TEditDisplayMainMenus holds tEditVarG's
// address there).
int32 tedit::completeTrack(uint32_t time)
{
    GuestFrame frame(0xd0);
    const uint32_t sp = frame.local<uint8_t>(0).address; // temp 0x0, the stack 0x10, pLastModule 0x14, pLookModule 0x18
    sTEditClass &t = te();
    at<uint32_t>(sp + 0x10u) = 0;
    if (TEditCheckForCompleteTrack() == 1)
        return 0;
    guest_call<int32_t>(sym::MemBlockAllocLo, sp + 0x10u, kStackBytes);
    const uint32_t stack = at<uint32_t>(sp + 0x10u), end = stack + kStackBytes;
    uint32_t entry = stack;
    bool result = false, userBreak = false;
    at<uint8_t>(entry) = 0;
    uint32_t last = t.trackDesc.pTrackModuleList.address;
    while (at<uint32_t>(last) != 0)
        last = at<uint32_t>(last);
    at<uint32_t>(sp + 0x14u) = last;
    const uint32_t menu = t.pTrackAutoCompActive.address;
    at<uint32_t>(G(pMenuSystemG).address) = menu;
    guest_call(sym::AMenuLinkItems, menu, 0, 0, 0, 0x7fff0000);
    const auto frame3d = [&](uint32_t now)
    {
        return guest_call<uint32_t>(sym::Init3dFrame, sym::mainVarG + 0x20u, at<uint32_t>(sym::cameraVarG + 0xb70u) + 0x24u, now, 0) != 0;
    };
    int32_t step = 0;
    for (;;)
    {
        if (at<uint8_t>(entry) == 0)
        {
            startEntry(entry);
            std::memcpy(guest_host(entry), guest_host(kScratch), kEntry);
        }
        else
            TEditAutoCompRemoveLastPiece(ptr32<_sAutoCompStackData>(entry));
        if (hasPiece(entry))
        {
            TEditAutoCompPlacePiece(ptr32<_sAutoCompStackData>(entry));
            if (TEditCheckForCompleteTrack() == 1)
            {
                result = true;
                break;
            }
            at<uint8_t>(entry) = static_cast<uint8_t>(at<uint8_t>(entry) + 1u);
            entry += kEntry;
            if (entry == end)
                break;
            at<uint8_t>(entry) = 0;
        }
        else
        {
            if (entry == stack)
                break;
            entry -= kEntry;
        }
        TEditCameraFollowCursor();
        time = guest_call<uint32_t>(sym::TimerGet, sym::mainVarG + 0x10u);
        guest_call(sym::CameraProcess, time, 0);
        guest_call(sym::SfxDoSfx);
        if (step % 6 == 0)
        {
            if (step == 0)
            {
                TEditRenderTrack();
                TEditRenderSkyDome();
            }
            guest_call(sym::Rdr_EndFrame);
            guest_call(sym::memset, sp, 0, 0xc);
            guest_call(sym::Rdr_StartFrame);
            guest_call(sym::RdrState_ApplyDefaultStateBlock);
            guest_call(sym::GfxP2_ZWriteEnable);
            if (frame3d(time))
            {
                guest_call(sym::TileFrameInit);
                guest_call(sym::CellRenderInit, sp);
                TEditDisplay3DGridLines();
                TEditRenderTrack();
                TEditRenderSkyDome();
                guest_call(sym::InputSnapshot);
                guest_call(sym::AMenuProcessAll, at<uint32_t>(G(pMenuSystemG).address));
                guest_call(sym::DisplayRenderFrameProfile, 0);
            }
        }
        if (at<uint32_t>(G(pMenuSystemG).address + 0x68u) == 0x11000000u)
        {
            userBreak = true;
            break;
        }
        ++step;
        if (step >= 800)
            break;
    }
    guest_call(sym::memset, sp, 0, 0xc);
    const uint32_t now = guest_call<uint32_t>(sym::TimerGet, sym::mainVarG + 0x10u);
    if (frame3d(now))
    {
        guest_call(sym::TileFrameInit);
        guest_call(sym::CellRenderInit, sp);
        TEditDisplay3DGridLines();
        TEditDEBUGDisplayCursor();
    }
    guest_call<int32_t>(sym::MemBlockFree, sp + 0x10u);
    at<uint32_t>(G(pMenuSystemG).address + 0x68u) = 0;
    if (result)
    {
        t.trackIsComplete = 1;
        ClearCursorModuleList();
        return 0;
    }
    // Not closed: the track is cut back to its last module before the search.
    last = at<uint32_t>(sp + 0x14u);
    uint32_t look = t.trackDesc.pTrackModuleList.address;
    while (look != last)
        look = at<uint32_t>(look);
    at<uint32_t>(sp + 0x18u) = look;
    for (uint32_t node = at<uint32_t>(look); node != 0;)
    {
        at<uint32_t>(sp + 0x18u) = node;
        const uint32_t next = at<uint32_t>(node);
        guest_call<int32_t>(sym::MemBlockFree, sp + 0x18u);
        node = next;
        if (node == 0)
            at<uint32_t>(sp + 0x18u) = 0;
    }
    at<uint32_t>(last) = 0;
    TEditClearTrack();
    TEditResetResourceMeter();
    generateTrackFromList(0xffffffffu);
    TEditClearAllRestricts();
    TEditCheckCursorDirectionConstraints();
    TEditCheckCursorMinMaxHeightConstraints();
    TEditSetNewCursorModule();
    TEditCameraFollowCursor();
    guest_call(sym::CameraProcess, time, 0);
    if (step == 800)
        return 1;
    return userBreak ? 3 : 2;
}

// A random start: the grid at 1-14, 1-14 in a random direction, the start grid module placed, the cursor in front.
void TEditRandomTrackAssignStartGrid()
{
    GuestFrame frame(0x60);
    sTEditClass &t = te();
    const uint32_t modules = t.pEditModuleList.address;
    t.cursor.xPos = static_cast<int16_t>(guest_call<uint32_t>(sym::RandomGetRange, 1, 0xe));
    t.cursor.yPos = static_cast<int16_t>(guest_call<uint32_t>(sym::RandomGetRange, 1, 0xe));
    const uint32_t direction = guest_call<uint32_t>(sym::RandomGetRange, 0, 3);
    t.trackDesc.startXPos = static_cast<uint8_t>(t.cursor.xPos);
    t.trackDesc.startYPos = static_cast<uint8_t>(t.cursor.yPos);
    t.trackDesc.startOrientation = static_cast<eDirection>(direction);
    t.cursor.elevation = 0;
    t.cursor.direction = static_cast<eDirection>(direction);
    t.cursor.currentSlope = SLOPE_FLAT;
    t.trackDesc.startElevation = 0;
    AddTrackModuleToTrackModuleList(0, 1);
    const uint32_t tiles = at<uint32_t>(modules + 0x14u);
    TEditPlaceModule(t.cursor.xPos, t.cursor.yPos, at<int16_t>(tiles), t.cursor.elevation, t.cursor.direction);
    switch (static_cast<uint32_t>(t.cursor.direction))
    {
    case 0:
        t.cursor.yPos = static_cast<int16_t>(t.cursor.yPos + 1);
        break;
    case 1:
        t.cursor.xPos = static_cast<int16_t>(t.cursor.xPos + 1);
        break;
    case 2:
        t.cursor.yPos = static_cast<int16_t>(t.cursor.yPos - 1);
        break;
    case 3:
        t.cursor.xPos = static_cast<int16_t>(t.cursor.xPos - 1);
        break;
    default:
        break;
    }
    t.trackState.editState = 2;
    TEditClearAllRestricts();
    TEditCheckCursorDirectionConstraints();
    availSlopeAndModule(getRegU32(&context(), 5));
}

// A special piece instead of the entry's best: the last two specials of the second row are first tried (and closed if
// they do not fit), then every open special has 5 chances in 23, the last one drawn winning (none: the first).
void TEditRandomTrackPlaceSpecialPiece(ptr32<_sAutoCompStackData> pAutoCompPiece)
{
    GuestFrame frame(0xd0);
    const uint32_t sp = frame.local<uint8_t>(0).address; // the trial's x 0x0, y 0x2, elevation 0x4, direction 0x18;
                                                         // x 0x10, y 0x12, elevation 0x14, direction 0x1c, slope 0x24
    sTEditClass &t = te();
    for (int8_t i = 5; i < 7; i = static_cast<int8_t>(i + 1))
    {
        if (t.editMenu.specialRestrict[1][i] != 0)
            continue;
        AddEditModuleToCursorModuleList(t.editMenu.pEditModuleSpecial[1][i]);
        uint32_t slope = 0;
        if (!tryPieces(sp, sp + 2u, sp + 0x18u, sp + 4u, &slope))
            t.editMenu.specialRestrict[1][i] = 1;
        ClearCursorModuleList();
    }
    uint32_t desc = 0;
    at<int32_t>(sp + 0x1cu) = t.cursor.direction;
    at<int16_t>(sp + 0x14u) = t.cursor.elevation;
    at<int16_t>(sp + 0x10u) = t.cursor.xPos;
    at<int16_t>(sp + 0x12u) = t.cursor.yPos;
    for (int8_t row = 0; row < 2; row = static_cast<int8_t>(row + 1))
        for (int8_t column = 0; column < 7; column = static_cast<int8_t>(column + 1))
        {
            if (t.editMenu.specialRestrict[row][column] != 0)
                continue;
            const int32_t draw = static_cast<int32_t>(guest_call<uint32_t>(sym::RandomGetRange, 0, 400));
            if (draw % 0x17 < 5)
            {
                desc = t.editMenu.pEditModuleSpecial[row][column].address;
                break;
            }
        }
    if (desc == 0)
        desc = t.editMenu.pEditModuleSpecial[0][0].address;
    AddEditModuleToCursorModuleList(ptr32<_sEditModuleDesc>(desc));
    uint32_t slope = at<uint32_t>(sp + 0x24u);
    const int8_t count = placePieces(sp + 0x10u, sp + 0x12u, sp + 0x1cu, sp + 0x14u, &slope);
    at<uint32_t>(sp + 0x24u) = slope;
    at<uint8_t>(pAutoCompPiece.address + 1u) = static_cast<uint8_t>(count);
    t.cursor.yPos = at<int16_t>(sp + 0x12u);
    t.cursor.direction = static_cast<eDirection>(at<int32_t>(sp + 0x1cu));
    t.cursor.currentSlope = static_cast<eSlope>(slope);
    t.cursor.elevation = at<int16_t>(sp + 0x14u);
    t.cursor.xPos = at<int16_t>(sp + 0x10u);
    TEditClearAllRestricts();
    TEditCheckCursorDirectionConstraints();
    TEditCheckCursorMinMaxHeightConstraints();
}

// A random track: a random start, then the search of Complete Track with, for the first 14-20 pieces, a random one of
// the cells that fit in place of the best and a special piece one time in three. True if it closed within 100 steps.
bool_ TEditRandomTrackGenerate()
{
    GuestFrame frame(0xc0);
    const uint32_t sp = frame.local<uint8_t>(0).address; // the stack 0x10
    sTEditClass &t = te();
    at<uint32_t>(sp + 0x10u) = 0;
    guest_call<int32_t>(sym::MemBlockAllocLo, sp + 0x10u, kStackBytes);
    const uint32_t stack = at<uint32_t>(sp + 0x10u), end = stack + kStackBytes;
    uint32_t entry = stack;
    bool_ result = 0;
    int32_t placed = 0;
    at<uint8_t>(entry) = 0;
    TEditRandomTrackAssignStartGrid();
    const int32_t randomPieces = static_cast<int32_t>(guest_call<uint32_t>(sym::RandomGetRange, 0xe, 0x14));
    for (int32_t step = 0; step < 100; ++step)
    {
        if (at<uint8_t>(entry) == 0)
        {
            const bool random = placed < randomPieces;
            startEntry(entry);
            if (random)
            {
                // The cells that fit lead the sorted list (the last of the 35 is never counted).
                int32_t legal = 0;
                while (legal < 0x22 && at<int16_t>(kScratch + 0x12u + static_cast<uint32_t>(legal) * 4u) != -1)
                    ++legal;
                if (legal >= 2)
                {
                    const int32_t draw = static_cast<int32_t>(guest_call<uint32_t>(sym::RandomGetRange, 0, 300));
                    at<uint32_t>(kScratch + 0x10u) = at<uint32_t>(kScratch + 0x10u + static_cast<uint32_t>(draw % (legal - 1)) * 4u);
                }
            }
            std::memcpy(guest_host(entry), guest_host(kScratch), kEntry);
        }
        else
        {
            --placed;
            TEditAutoCompRemoveLastPiece(ptr32<_sAutoCompStackData>(entry));
        }
        guest_call(sym::SfxDoSfx);
        if (hasPiece(entry))
        {
            const int32_t draw = static_cast<int32_t>(guest_call<uint32_t>(sym::RandomGetRange, 0, 400));
            if (placed < randomPieces && draw % 12 < 4 && t.editMenu.noSpecialAvail == 0)
                TEditRandomTrackPlaceSpecialPiece(ptr32<_sAutoCompStackData>(entry));
            else
                TEditAutoCompPlacePiece(ptr32<_sAutoCompStackData>(entry));
            ++placed;
            if (TEditCheckForCompleteTrack() == 1)
            {
                result = 1;
                break;
            }
            at<uint8_t>(entry) = static_cast<uint8_t>(at<uint8_t>(entry) + 1u);
            entry += kEntry;
            if (entry == end)
                break;
            at<uint8_t>(entry) = 0;
        }
        else
        {
            if (entry == stack)
                break;
            entry -= kEntry;
        }
    }
    guest_call<int32_t>(sym::MemBlockFree, sp + 0x10u);
    return result;
}

// The editor menu's Random Track: tries until a track closes.
void TEditMenuDoRandomTrack()
{
    {
        GuestFrame frame(0x20);
        do
        {
            TEditClearTrack();
            ClearCursorModuleList();
            ClearTrackModuleList();
            TEditResetResourceMeter();
        } while (TEditRandomTrackGenerate() == 0);
        te().trackIsComplete = 1;
        te().trackDesc.bestLapTime = kUnsavedLapTime;
    }
    ClearCursorModuleList();
}

} // namespace rc

RC_DECOMP_FN_SETUP(TEditGetAutoCompPieceScore, rc::fuzz::teditAutoComp)
RC_DECOMP_FN_SETUP(TEditGetAutoCompAllPiecesScore, rc::fuzz::teditAutoComp)
RC_DECOMP_FN_SETUP(TEditAutoCompRemoveLastPiece, rc::fuzz::teditAutoComp)
RC_DECOMP_FN_SETUP(TEditAutoCompPlacePiece, rc::fuzz::teditAutoComp)
RC_DECOMP_FN_BLOCKING_SETUP_HEAP(TEditMenuDoCompleteTrack, rc::fuzz::teditSearch) // draws frames while it searches
RC_DECOMP_FN_SETUP(TEditRandomTrackAssignStartGrid, rc::fuzz::teditWalk)
RC_DECOMP_FN_SETUP(TEditRandomTrackPlaceSpecialPiece, rc::fuzz::teditAutoComp)
RC_DECOMP_FN_SETUP_HEAP(TEditRandomTrackGenerate, rc::fuzz::teditSearch)
RC_DECOMP_FN_SETUP_HEAP(TEditMenuDoRandomTrack, rc::fuzz::teditRandomTrack)

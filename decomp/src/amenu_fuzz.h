// Inputs for the random test of AMENU.c. Each has the signature RC_DECOMP_FN_SETUP expects.
//
// Every input builds a small menu world in the test's scratch area: up to four menu headers (the menu under test, a
// link target, a global destination and a parent), a list of items with variables, text groups in the four
// languages, a colour set and a copy of the dump's border set, and fills menuSystemG, the pad state (inputVarG), the
// front-end scroll variables and the general text and car name tables (through pLvlG or the Track Editor's) to match.
// Flags, tags, link types and selections are drawn from the values the code tests for. Drawing goes into the dump's
// own GFX_P2 packet buffer (whole RAM compared).
//
// Offline, the functions outside the unit that touch hardware or the rest of the front end are replaced by stand-ins
// that log their arguments into scratch and answer from the input's seed: SfxPlay, the two volume functions,
// HideEmptyMenu, SetRewards (fills the table), GetTimeTrialTime (writes a time), the selection callback and
// scePadInfoMode.
#pragma once

#include "rc_decomp.h"

namespace rc::fuzz {

void amenuHeader(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // a0 a header, a1 flags or a small value
void amenuItemIndex(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 a header, a1 an item index
void amenuAddItem(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 a header, a1 a new item
void amenuLink(R5900Context *ctx, uint64_t &state, uint32_t scratch);          // a0 a header, a1 a position, a2 a menu, a3 a type, t0 a tag
void amenuMinSel(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // a0 a header, a1-a3 three counters
void amenuItem(R5900Context *ctx, uint64_t &state, uint32_t scratch);          // a0 a header, a1 an item, a2 a colour set, a3 an index
void amenuChange(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // a0 a header, a1 a change type, a2 special
void amenuPadTest(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 a pad, a1 a tag, a2 a header
void amenuNone(R5900Context *ctx, uint64_t &state, uint32_t scratch);          // no arguments; the world built
void amenuTextGroup(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 a text group (or 0), a1 a string
void amenuMessage(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 a message or car number
void amenuStrings(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 a buffer, a1 a string with %s
void amenuCentre(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // a0 a header, a1 a string, a2 a width or -1
void amenuInput(R5900Context *ctx, uint64_t &state, uint32_t scratch);         // a0 a pad (-1, 0, 1), a1 a style, a2 a header
void amenuProcess(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 a header, a1 special; keys pressed
void amenuProcessScroll(R5900Context *ctx, uint64_t &state, uint32_t scratch); // a0 a scrolling list's header, a1 special
void amenuProcessArea(R5900Context *ctx, uint64_t &state, uint32_t scratch);   // a0 a name-entry menu's header
void amenuSizeItem(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 an item, a1 a width
void amenuSize(R5900Context *ctx, uint64_t &state, uint32_t scratch);          // a0 a header of any kind (for sizing)
void amenuAreaHeader(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0 a name-entry menu's header
void amenuDraw(R5900Context *ctx, uint64_t &state, uint32_t scratch);          // a0 a header of any kind, a1 flags
void amenuDrawOrdinary(R5900Context *ctx, uint64_t &state, uint32_t scratch);  // a0 an ordinary menu's header
void amenuDrawScroll(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0 a scrolling list's header
void amenuDrawTitle(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 a header, a1 a colour set, a2 y
void amenuDrawItem(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 a header, a1 an item, a2 colours, a3 x, t0 y
void amenuDrawDialog(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0 x, a1 y, a2 a header, a3 colours
void amenuDrawTabs(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 x, a1 y, a2 a buffer, a3 a tabbed string
void amenuBar(R5900Context *ctx, uint64_t &state, uint32_t scratch);           // a0 a slider's details
void amenuClip(R5900Context *ctx, uint64_t &state, uint32_t scratch);          // a0 a header, a1 an offset, a2 extra height
void amenuTick(R5900Context *ctx, uint64_t &state, uint32_t scratch);          // a0 x, a1 y, a2 a value or key
void amenuTickText(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 a header, a1 x, a2 y, a3 a value, t0 text, t1 a type
void amenuTrack(R5900Context *ctx, uint64_t &state, uint32_t scratch);         // a0 x, a1 y, a2 a scale, a3 a width, t0 a level
void amenuProgHeader(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0 a header, a1 game state, a2 x, a3 y
void amenuProgEntry(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 a header, a1 game state, a2 x, a3 y, t0 width, t1 level
void amenuTTEntry(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 a header, a1 x, a2 y, a3 width, t0 level
void amenuPageSub(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 a source header, a1 a box, a2 a width, a3 a title
void amenuPageScroll(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0 a box, a1 a speed; the scroll variables
void amenuPage(R5900Context *ctx, uint64_t &state, uint32_t scratch);          // a0 a source header, a1 game state, a2 a card
void amenuHelpEntry(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 a header, a1 a help paragraph, a2 x, a3 y
void amenuLine(R5900Context *ctx, uint64_t &state, uint32_t scratch);          // a0 a header, a1 y
void amenuSpriteBorder(R5900Context *ctx, uint64_t &state, uint32_t scratch);  // a0 a gap, a1 sprite parameters
void amenuRaceTick(R5900Context *ctx, uint64_t &state, uint32_t scratch);      // a0 game state, a1 a level, a2 a race type
void amenuInit(R5900Context *ctx, uint64_t &state, uint32_t scratch);          // no arguments; menuSystemG random

} // namespace rc::fuzz

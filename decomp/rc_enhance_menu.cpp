// Game Options > Enhancements: the enhanced recomp's settings (rc_settings.h) as a page of the game's own menus.
//
// The front end's menus come from its level file and are rebuilt each time the front end loads (MenuInitScan). After
// that, install() adds to the Game Options menu (the one holding the developers' hidden "Debug Menus" item, which it
// also shows): an "Enhancements" item linking to a menu built here in guest memory, a copy of the "Game options" page
// (its look and placement) with one choice item per setting. The game's menu code draws and runs it: left and right
// change a choice, which it writes to the item's feedback word; poll() (each front-end frame) applies and saves a
// changed setting. The structures are made once and kept; only the links into the rebuilt menus are redone.
#include "rc_decomp.h"
#include "rc_settings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace rc::enhance {

namespace {

constexpr uint32_t kHeaderBytes = 0x80, kItemBytes = 0x18, kVarBytes = 0x1c;
constexpr uint32_t kDebugMenusTag = 0x10000208u;

struct Choice
{
    const char *text;    // with %s for the value
    const char *choices; // '*' between them
    int count;
};
// The order of the page; value() and apply() follow it.
constexpr Choice kChoices[] = {
    {"Aspect ratio: %s", "4:3*16:9*16:10*21:9", 4},
    {"HUD and menus: %s", "Corrected*Stretched", 2},
    {"Display: %s", "Window*Fullscreen", 2},
    {"Resolution (restart): %s", "1x*2x*3x*4x*5x*6x", 6},
    {"Race frame rate: %s", "60*90*120*144*165*240", 6},
    {"Smooth motion: %s", "Off*On", 2},
};
constexpr int kCount = static_cast<int>(sizeof(kChoices) / sizeof(kChoices[0]));

struct Built
{
    bool made = false;
    uint32_t header = 0, link = 0, feedback = 0;
    uint32_t items[kCount]{}, vars[kCount]{};
    int last[kCount]{};
} g;

uint32_t alloc(uint32_t bytes)
{
    const uint32_t at = g_runtime->guestMalloc(bytes, 16u);
    if (at != 0u)
        std::memset(guest_host(at), 0, bytes);
    return at;
}

// A text group with the same string for all four languages.
uint32_t textGroup(const char *text)
{
    const uint32_t length = static_cast<uint32_t>(std::strlen(text)) + 1u;
    const uint32_t group = alloc(16u + length);
    if (group == 0u)
        return 0u;
    std::memcpy(guest_host(group + 16u), text, length);
    for (uint32_t i = 0; i < 4; ++i)
        *static_cast<uint32_t *>(guest_host(group + i * 4u)) = group + 16u;
    return group;
}

int32_t &feedback(int i) { return *static_cast<int32_t *>(guest_host(g.feedback + static_cast<uint32_t>(i) * 4u)); }

int value(const settings::Settings &s, int i)
{
    switch (i)
    {
    case 0: return s.aspect;
    case 1: return s.hudCorrected ? 0 : 1;
    case 2: return s.fullscreen ? 1 : 0;
    case 3: return s.scale - 1;
    case 4: return s.raceFps;
    default: return s.smoothMotion ? 1 : 0;
    }
}

void apply(settings::Settings &s, int i, int v)
{
    switch (i)
    {
    case 0: s.aspect = v; break;
    case 1: s.hudCorrected = v == 0; break;
    case 2: s.fullscreen = v != 0; break;
    case 3: s.scale = v + 1; break;
    case 4: s.raceFps = v; break;
    default: s.smoothMotion = v != 0; break;
    }
}

ptr32<sMenuItemStd> lastItem(ptr32<sAMenuHeader> h)
{
    ptr32<sMenuItemStd> p = h->pMenuStart;
    while (p && p->pNext)
        p = p->pNext;
    return p;
}

// The page, modelled on the game's "Game options" page and its "Weapon pick-ups" choice item.
bool make(ptr32<sAMenuHeader> gameOptionsPage, ptr32<sMenuItemStd> choiceItem, ptr32<sMenuItemStd> linkItem)
{
    g.header = alloc(kHeaderBytes);
    g.link = alloc(kItemBytes);
    g.feedback = alloc(kCount * 4u);
    if (!g.header || !g.link || !g.feedback)
        return false;
    std::memcpy(guest_host(g.header), guest_host(gameOptionsPage.address), kHeaderBytes);
    ptr32<sAMenuHeader> h(g.header);
    h->pTitleText = ptr32<sTextGroup>(textGroup("Enhancements"));
    h->pDialogTxt = nullptr;
    h->pAdditionalText = nullptr;
    h->pSpareText = nullptr;
    h->pSelection = nullptr;
    h->pValueStore = nullptr;
    h->pSpecialMenu = nullptr;
    h->pGlobalDestMenu = nullptr;
    h->selectionTag = 0;
    h->selection = 0;
    // A plain list, sized for its own items when it is first shown (not the copied page's scroll state or size).
    h->scrollStart = 0;
    h->scrollItems = 0;
    h->colSelect = 0;
    h->rowSelect = 0;
    h->width = 0;
    h->height = 0;
    h->menuItems = static_cast<uint8>(kCount);
    const settings::Settings now = settings::get();
    for (int i = 0; i < kCount; ++i)
    {
        g.items[i] = alloc(kItemBytes);
        g.vars[i] = alloc(kVarBytes);
        if (!g.items[i] || !g.vars[i])
            return false;
    }
    for (int i = 0; i < kCount; ++i)
    {
        std::memcpy(guest_host(g.items[i]), guest_host(choiceItem.address), kItemBytes);
        std::memcpy(guest_host(g.vars[i]), guest_host(choiceItem->pMenuVar.address), kVarBytes);
        ptr32<sMenuItemStd> item(g.items[i]);
        ptr32<sMenuItemVar> var(g.vars[i]);
        item->pTextGroup = ptr32<sTextGroup>(textGroup(kChoices[i].text));
        item->pMenuVar = var;
        item->pNext = i + 1 < kCount ? ptr32<sMenuItemStd>(g.items[i + 1]) : nullptr;
        item->pLinkMenu = nullptr;
        item->selectionTag = 0;
        var->pTextGroup = ptr32<sTextGroup>(textGroup(kChoices[i].choices));
        var->tagMVar = static_cast<int32>(0x10000f00u + static_cast<uint32_t>(i)); // in no link table
        var->minValue = 0;
        var->maxValue = kChoices[i].count - 1;
        var->flags = 0; // a list of choices
        var->pFeedbackVar = ptr32<int32>(g.feedback + static_cast<uint32_t>(i) * 4u);
        feedback(i) = var->value = g.last[i] = value(now, i);
    }
    h->pMenuStart = ptr32<sMenuItemStd>(g.items[0]);
    std::memcpy(guest_host(g.link), guest_host(linkItem.address), kItemBytes);
    ptr32<sMenuItemStd> link(g.link);
    link->pTextGroup = ptr32<sTextGroup>(textGroup("Enhancements"));
    link->pMenuVar = nullptr;
    link->pNext = nullptr;
    link->pLinkMenu = h;
    link->selectionTag = 0;
    link->flags = 0;
    link->statusFlags = 0;
    link->linkType = 0;
    g.made = true;
    return true;
}

} // namespace

// After MenuInitScan has hidden the debug item: show it, and add Enhancements after it.
void install(ptr32<sUIgroup> pUIGroup)
{
    ptr32<sAMenuHeader> gameOptions = nullptr;
    ptr32<sMenuItemStd> debugItem = nullptr;
    for (ptr32<sUIgroup> grp = pUIGroup; grp->pSourceHeader.address != 0xffffffffu && !gameOptions; grp = grp + 1)
        if (const ptr32<sAMenuHeader> h = grp->pSourceHeader)
            for (ptr32<sMenuItemStd> p = h->pMenuStart; p; p = p->pNext)
                if (p->selectionTag == kDebugMenusTag)
                {
                    gameOptions = h;
                    debugItem = p;
                    break;
                }
    if (!gameOptions)
        return; // not the main front end (the Track Editor's menus, say)
    debugItem->flags = static_cast<uint16>(debugItem->flags & ~0x60u); // the developers' Debug Menus, shown
    // The "Game options" page is the first item's link (Game Setup); its "Weapon pick-ups" item is a two-choice item.
    const ptr32<sMenuItemStd> setupItem = gameOptions->pMenuStart;
    const ptr32<sAMenuHeader> page = setupItem ? setupItem->pLinkMenu : ptr32<sAMenuHeader>(nullptr);
    ptr32<sMenuItemStd> choiceItem = nullptr;
    if (page)
        for (ptr32<sMenuItemStd> p = page->pMenuStart; p; p = p->pNext)
            if (p->pMenuVar && p->pMenuVar->maxValue - p->pMenuVar->minValue == 1 && p->pMenuVar->pTextGroup)
            {
                choiceItem = p;
                break;
            }
    if (!page || !choiceItem)
        return;
    if (!g.made && !make(page, choiceItem, setupItem))
    {
        std::fprintf(stderr, "[enhance] could not build the Enhancements menu (guest memory)\n");
        return;
    }
    // The rebuilt Game Options menu: Enhancements after its last item (once).
    const ptr32<sMenuItemStd> last = lastItem(gameOptions);
    if (last.address != g.link)
    {
        last->pNext = ptr32<sMenuItemStd>(g.link);
        ptr32<sMenuItemStd>(g.link)->pNext = nullptr;
        gameOptions->menuItems = static_cast<uint8>(gameOptions->menuItems + 1);
    }
    ptr32<sAMenuHeader>(g.header)->pLinkMenuReturn = gameOptions;
    // What the page shows comes from the settings (the file may have been changed elsewhere).
    const settings::Settings now = settings::get();
    for (int i = 0; i < kCount; ++i)
        feedback(i) = g.last[i] = value(now, i);
}

// Each front-end frame: a choice the player changed becomes the setting (applied live where it can be, and saved).
void poll()
{
    if (!g.made)
        return;
    if (std::getenv("RC_ENHANCE_DEBUG"))
    {
        static uint32_t lastActive = 0;
        const ptr32<sAMenuHeader> a = G(pMenuSystemG)->pActiveMenu;
        if (a && a.address != lastActive)
        {
            lastActive = a.address;
            std::fprintf(stderr, "[enhance-debug] active %08x ours %d flags %04x more %04x even %04x items %u sel %d scrollItems %d scrollStart %d dlg %d w %u h %u changeY %u sel* %08x\n",
                         a.address, a.address == g.header, a->flags, a->moreFlags, a->evenMoreFlags, a->menuItems, a->selection, a->scrollItems,
                         a->scrollStart, a->dlgLines, a->width, a->height, a->changeY, a->pSelection.address);
            for (ptr32<sMenuItemStd> p = a->pMenuStart; p; p = p->pNext)
                std::fprintf(stderr, "[enhance-debug]   item %08x flags %04x status %02x var %08x\n", p.address, p->flags, p->statusFlags, p->pMenuVar.address);
        }
    }
    settings::Settings s = settings::get();
    bool changed = false;
    for (int i = 0; i < kCount; ++i)
    {
        const int v = feedback(i);
        if (v == g.last[i] || v < 0 || v >= kChoices[i].count)
            continue;
        g.last[i] = v;
        apply(s, i, v);
        changed = true;
    }
    if (changed)
    {
        settings::set(s);
        std::fprintf(stderr, "[enhance] settings changed and saved to %s\n", settings::filePath());
    }
}

} // namespace rc::enhance

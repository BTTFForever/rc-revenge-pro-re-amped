// Game Options > Enhancements and Pause Menu > Enhancements: the enhanced recomp's settings (rc_settings.h) as a page of
// the game's own menus.
//
// The front end's menus come from its level file and are rebuilt each time the front end loads (MenuInitScan); the
// pause menu comes from each race's level file. After MenuInitScan, install() adds to the menu holding the developers'
// hidden "Debug Menus" item (the front end's Game Options, or the pause menu; it also shows that item): an
// "Enhancements" item linking to a page built here in guest memory, a copy of a page of that menu system (the front
// end's "Game options" page, or the pause menu itself, for their look, placement, colours and borders) with one choice
// item per setting. The game's menu code draws and runs it: left and right change a choice, which it writes to the
// item's feedback word (one shared by both pages); poll() (each front-end frame, each paused race frame) applies and
// saves a changed setting. Each page's memory is allocated once; its contents are copied again from the current menus
// at every install, since the level data they point into (colour and border sets) is reloaded with each level.
#include "rc_decomp.h"
#include "rc_settings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace rc {
void RcRefreshRaceUpdate(); // gametask.cpp
}

namespace rc::enhance {

namespace {
void raceSettingsChanged() { RcRefreshRaceUpdate(); }
} // namespace

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
    {"V-Sync: %s", "Off*On*Adaptive", 3},
    {"Variable refresh: %s", "Off*On", 2},
};
constexpr int kCount = static_cast<int>(sizeof(kChoices) / sizeof(kChoices[0]));

// One page (and the item linking to it) in guest memory, with its texts.
struct Page
{
    bool made = false;
    uint32_t header = 0, link = 0;
    uint32_t items[kCount]{}, vars[kCount]{};
    uint32_t title = 0, texts[kCount]{}, choices[kCount]{};
};
struct Built
{
    uint32_t feedback = 0; // the choices' feedback words, shared by the pages
    int last[kCount]{};
    Page front, pause;
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
    case 5: return s.smoothMotion ? 1 : 0;
    case 6: return s.vsync;
    default: return s.vrr ? 1 : 0;
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
    case 5: s.smoothMotion = v != 0; break;
    case 6: s.vsync = v; break;
    default: s.vrr = v != 0; break;
    }
}

ptr32<sMenuItemStd> lastItem(ptr32<sAMenuHeader> h)
{
    ptr32<sMenuItemStd> p = h->pMenuStart;
    while (p && p->pNext)
        p = p->pNext;
    return p;
}

bool allocate(Page &page)
{
    if (g.feedback == 0u && (g.feedback = alloc(kCount * 4u)) == 0u)
        return false;
    page.header = alloc(kHeaderBytes);
    page.link = alloc(kItemBytes);
    page.title = textGroup("Enhancements");
    if (!page.header || !page.link || !page.title)
        return false;
    for (int i = 0; i < kCount; ++i)
    {
        page.items[i] = alloc(kItemBytes);
        page.vars[i] = alloc(kVarBytes);
        page.texts[i] = textGroup(kChoices[i].text);
        page.choices[i] = textGroup(kChoices[i].choices);
        if (!page.items[i] || !page.vars[i] || !page.texts[i] || !page.choices[i])
            return false;
    }
    page.made = true;
    return true;
}

// The page, modelled on a page of the current menus (its header), one of its two-choice items and one of its link
// items; filled again at each install.
void fill(Page &page, ptr32<sAMenuHeader> modelPage, ptr32<sMenuItemStd> choiceItem, ptr32<sMenuItemStd> linkItem)
{
    std::memcpy(guest_host(page.header), guest_host(modelPage.address), kHeaderBytes);
    ptr32<sAMenuHeader> h(page.header);
    h->pTitleText = ptr32<sTextGroup>(page.title);
    h->pDialogTxt = nullptr;
    h->pAdditionalText = nullptr;
    h->pSpareText = nullptr;
    h->pSelection = nullptr;
    h->pValueStore = nullptr;
    h->pSpecialMenu = nullptr;
    h->pGlobalDestMenu = nullptr;
    h->pLinkMenuReturn = nullptr;
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
    for (int i = 0; i < kCount; ++i)
    {
        std::memcpy(guest_host(page.items[i]), guest_host(choiceItem.address), kItemBytes);
        std::memcpy(guest_host(page.vars[i]), guest_host(choiceItem->pMenuVar.address), kVarBytes);
        ptr32<sMenuItemStd> item(page.items[i]);
        ptr32<sMenuItemVar> var(page.vars[i]);
        item->pTextGroup = ptr32<sTextGroup>(page.texts[i]);
        item->pMenuVar = var;
        item->pNext = i + 1 < kCount ? ptr32<sMenuItemStd>(page.items[i + 1]) : nullptr;
        item->pLinkMenu = nullptr;
        item->selectionTag = 0;
        item->flags = 0;
        item->statusFlags = static_cast<uint8>(item->statusFlags & ~1u); // not disabled
        var->pTextGroup = ptr32<sTextGroup>(page.choices[i]);
        var->tagMVar = static_cast<int32>(0x10000f00u + static_cast<uint32_t>(i)); // in no link table
        var->minValue = 0;
        var->maxValue = kChoices[i].count - 1;
        var->flags = 0; // a list of choices
        var->pFeedbackVar = ptr32<int32>(g.feedback + static_cast<uint32_t>(i) * 4u);
        var->value = feedback(i);
    }
    h->pMenuStart = ptr32<sMenuItemStd>(page.items[0]);
    std::memcpy(guest_host(page.link), guest_host(linkItem.address), kItemBytes);
    ptr32<sMenuItemStd> link(page.link);
    link->pTextGroup = ptr32<sTextGroup>(page.title);
    link->pMenuVar = nullptr;
    link->pNext = nullptr;
    link->pLinkMenu = h;
    link->selectionTag = 0;
    link->flags = 0;
    link->statusFlags = 0;
    link->linkType = 0;
}

// The first two-choice item of a page (the front end's "Weapon pick-ups", the pause menu's "Show Track Directions").
ptr32<sMenuItemStd> twoChoiceItem(ptr32<sAMenuHeader> page)
{
    for (ptr32<sMenuItemStd> p = page->pMenuStart; p; p = p->pNext)
        if (p->pMenuVar && p->pMenuVar->maxValue - p->pMenuVar->minValue == 1 && p->pMenuVar->pTextGroup)
            return p;
    return nullptr;
}

} // namespace

// After MenuInitScan has hidden the debug item: show it, and add Enhancements after the menu's last item.
void install(ptr32<sUIgroup> pUIGroup)
{
    ptr32<sAMenuHeader> menu = nullptr;
    ptr32<sMenuItemStd> debugItem = nullptr;
    for (ptr32<sUIgroup> grp = pUIGroup; grp->pSourceHeader.address != 0xffffffffu && !menu; grp = grp + 1)
        if (const ptr32<sAMenuHeader> h = grp->pSourceHeader)
            for (ptr32<sMenuItemStd> p = h->pMenuStart; p; p = p->pNext)
                if (p->selectionTag == kDebugMenusTag)
                {
                    menu = h;
                    debugItem = p;
                    break;
                }
    if (!menu)
        return; // neither the main front end nor a race (the Track Editor's menus, say)
    debugItem->flags = static_cast<uint16>(debugItem->flags & ~0x60u); // the developers' Debug Menus, shown
    // What the pages show comes from the settings (the file may have been changed elsewhere).
    if (g.feedback != 0u)
    {
        const settings::Settings now = settings::get();
        for (int i = 0; i < kCount; ++i)
            feedback(i) = g.last[i] = value(now, i);
    }
    // The front end's Game Options: its first item (Game Setup) links to the "Game options" page, the model. The pause
    // menu (its first item is Resume Race, no link): the pause menu itself is the model.
    const ptr32<sMenuItemStd> first = menu->pMenuStart;
    const bool front = first && first->pLinkMenu;
    Page &page = front ? g.front : g.pause;
    const ptr32<sAMenuHeader> model = front ? ptr32<sAMenuHeader>(first->pLinkMenu) : menu;
    const ptr32<sMenuItemStd> choiceItem = twoChoiceItem(model);
    const ptr32<sMenuItemStd> linkItem = front ? first : debugItem;
    if (!choiceItem)
        return;
    const bool firstTime = g.feedback == 0u;
    if (!page.made && !allocate(page))
    {
        std::fprintf(stderr, "[enhance] could not build the Enhancements menu (guest memory)\n");
        return;
    }
    if (firstTime)
    {
        const settings::Settings now = settings::get();
        for (int i = 0; i < kCount; ++i)
            feedback(i) = g.last[i] = value(now, i);
    }
    fill(page, model, choiceItem, linkItem);
    // The rebuilt menu: Enhancements after its last item (once). In a cup the pause menu is another one, the first
    // group's view (MenuInitScan): the same item ends that list too (the menu code sets the page's way back to the
    // menu it was entered from).
    const auto append = [&](ptr32<sAMenuHeader> h)
    {
        const ptr32<sMenuItemStd> last = lastItem(h);
        if (last && last.address != page.link)
        {
            last->pNext = ptr32<sMenuItemStd>(page.link);
            h->menuItems = static_cast<uint8>(h->menuItems + 1);
        }
    };
    append(menu);
    if (!front)
        if (const uint32_t cupMenu = pUIGroup->viewPosTag; cupMenu != 0u && cupMenu != menu.address && cupMenu < 0x2000000u)
            append(ptr32<sAMenuHeader>(cupMenu));
    ptr32<sAMenuHeader>(page.header)->pLinkMenuReturn = menu;
}

// Each front-end frame and each paused race frame: a choice the player changed becomes the setting (applied live where
// it can be, and saved).
void poll()
{
    if (g.feedback == 0u)
        return;
    if (std::getenv("RC_ENHANCE_DEBUG"))
    {
        static uint32_t lastActive = 0;
        const ptr32<sAMenuHeader> a = G(pMenuSystemG)->pActiveMenu;
        if (a && a.address != lastActive)
        {
            lastActive = a.address;
            std::fprintf(stderr, "[enhance-debug] active %08x ours %d flags %04x more %04x even %04x items %u sel %d scrollItems %d scrollStart %d dlg %d w %u h %u changeY %u sel* %08x\n",
                         a.address, a.address == g.front.header || a.address == g.pause.header, a->flags, a->moreFlags, a->evenMoreFlags, a->menuItems, a->selection, a->scrollItems,
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
        raceSettingsChanged(); // the race frame rate and smooth motion, if a race is on
        std::fprintf(stderr, "[enhance] settings changed and saved to %s\n", settings::filePath());
    }
}

} // namespace rc::enhance

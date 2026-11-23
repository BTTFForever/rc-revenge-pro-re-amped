// Game Options > Enhancements, the players' PC controls in Game Options > Controls, and the same pages in the Pause
// Menu: the enhanced recomp's settings (rc_settings.h) and each player's bindings (runtime/ps2_input_bindings.h) as
// pages of the game's own menus.
//
// The front end's menus come from its level file and are rebuilt each time the front end loads (MenuInitScan); the
// pause menu comes from each race's level file. After MenuInitScan, install() adds to the menu holding the developers'
// hidden "Debug Menus" item (the front end's Game Options, or the pause menu; it also shows that item) an
// "Enhancements" item, and in the pause menu a "Controls" item; in the front end the game's own Controller
// configuration page gets "Player 1 PC Controls..." and "Player 2 PC Controls...". They link to pages built here in
// guest memory: copies of a page of that menu system (the front end's "Game options" page, or the pause menu itself,
// for their look, placement, colours and borders) with choice items and link items copied from its own. The game's
// menu code draws and runs them: left and right change a choice, which it writes to the item's feedback word; poll()
// (each front-end frame, each paused race frame) acts on a changed one. Each page's memory is allocated once; its
// contents are copied again from the current menus at every install, since the level data they point into (colour
// and border sets) is reloaded with each level.
//
// A player's page: the device (Auto, keyboard, a gamepad or both), Buttons... and D-pad and sticks... (twelve controls each:
// a page shows about twelve rows, and the menu code's scrolling is not usable for these pages), Reset to defaults.
// Each control shows what it does in the player's controller layout (the game's own setting) and its key and gamepad
// input; left or right on it waits for the new key or button (Escape cancels, Delete clears, 6 s gives up; an input
// already used by another control of that player moves to the old one's). Texts that change (bindings, gamepad names,
// actions) are buffers rewritten in place.
#include "rc_decomp.h"
#include "rc_settings.h"
#include "runtime/ps2_input_bindings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <functional>
#include <vector>

namespace rc {
void RcRefreshRaceUpdate(); // gametask.cpp
}

namespace rc::enhance {

namespace {

namespace B = ps2_input_bindings;

constexpr uint32_t kHeaderBytes = 0x80, kItemBytes = 0x18, kVarBytes = 0x1c;
constexpr uint32_t kDebugMenusTag = 0x10000208u;
constexpr uint32_t kTextBytes = 96, kDeviceChoicesBytes = 768;

// ---- Enhancements ----
struct Choice
{
    const char *text;    // with %s for the value
    const char *choices; // '*' between them
    int count;
};
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
constexpr int kEnhanceCount = static_cast<int>(sizeof(kChoices) / sizeof(kChoices[0]));

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

// ---- Pages ----
// A player's page: the device, links to the buttons and the d-pad and sticks (a page each: twelve rows fit a page),
// reset.
enum PageId { PageEnhance, PageControls, PagePlayer1, PagePlayer2, PageButtons1, PageButtons2, PageSticks1, PageSticks2, PageCount };
enum Role { RoleSetting, RoleLink, RoleDevice, RoleControl, RoleReset };
// The controls of the two control pages, in the order shown.
constexpr int kButtonsPage[] = {B::Cross, B::Circle, B::Square, B::Triangle, B::L1, B::R1, B::L2, B::R2, B::Start, B::Select, B::L3, B::R3};
constexpr int kSticksPage[] = {B::Up, B::Down, B::Left, B::Right, B::LeftStickLeft, B::LeftStickRight, B::LeftStickUp, B::LeftStickDown,
                               B::RightStickLeft, B::RightStickRight, B::RightStickUp, B::RightStickDown};

struct Item
{
    uint32_t text = 0;    // text group (shared by the front-end and pause copies)
    uint32_t choices = 0; // choice item: its choices' text group; 0 for a link
    int count = 2;
    int link = -1; // link item: the page it opens
    Role role = RoleSetting;
    int player = 0, control = 0;
};
struct PageDef
{
    uint32_t title = 0;
    std::vector<Item> items;
    uint32_t feedback = 0; // a word per item
    std::vector<int> last;
};
struct Instance // one page in one menu system
{
    bool made = false;
    uint32_t header = 0;
    std::vector<uint32_t> items, vars;
};
struct Built
{
    bool made = false;
    PageDef defs[PageCount];
    Instance front[PageCount], pause[PageCount];
    uint32_t frontLinks[2]{}, pauseLinks[2]{}; // Enhancements and Controls in the menu that holds them
    uint32_t frontPlayerLinks[2]{};             // the player pages' links in the game's Controller configuration page
    // a rebind waiting for its input
    int capturePlayer = -1, captureControl = -1;
} g;

uint32_t alloc(uint32_t bytes)
{
    const uint32_t at = g_runtime->guestMalloc(bytes, 16u);
    if (at != 0u)
        std::memset(guest_host(at), 0, bytes);
    return at;
}

// A text group with the same string for all four languages, with room for capacity bytes (a text rewritten later).
uint32_t textGroup(const char *text, uint32_t capacity = 0)
{
    const uint32_t length = static_cast<uint32_t>(std::strlen(text)) + 1u;
    const uint32_t group = alloc(16u + std::max(length, capacity));
    if (group == 0u)
        return 0u;
    std::memcpy(guest_host(group + 16u), text, length);
    for (uint32_t i = 0; i < 4; ++i)
        *static_cast<uint32_t *>(guest_host(group + i * 4u)) = group + 16u;
    return group;
}

// Rewrites a text group made with a capacity (only when it changed).
void setText(uint32_t group, const std::string &text, uint32_t capacity)
{
    if (group == 0u)
        return;
    char *at = static_cast<char *>(guest_host(group + 16u));
    std::string t = text.substr(0, capacity - 1u);
    if (t != at)
        std::memcpy(at, t.c_str(), t.size() + 1u);
}

int32_t &feedback(const PageDef &d, int i) { return *static_cast<int32_t *>(guest_host(d.feedback + static_cast<uint32_t>(i) * 4u)); }

// ---- Controls texts ----
// What each DualShock 2 button does in the game's controller layouts (PlayerGetInput: the keys tables in
// PlayerSetKeyDefinitions; bit numbers as the pad sends them). Layouts 1 and 3 steer with the left stick and drive
// with the right one.
constexpr const char *kActions[12] = {"Steer left", "Steer right", "Accelerate", "Reverse", "Handbrake", "Fire weapon",
                                      "Flip vehicle", "Reposition", "Camera", "Aim up", "Aim down", "Look backwards"};
constexpr uint8_t kLayouts[4][12] = {
    {7, 5, 14, 15, 11, 13, 12, 10, 0, 4, 6, 8},
    {0xff, 0xff, 0xff, 0xff, 11, 9, 10, 8, 0, 4, 6, 12},
    {7, 5, 14, 15, 11, 9, 12, 10, 0, 4, 6, 8},
    {0xff, 0xff, 0xff, 0xff, 12, 14, 15, 10, 0, 4, 6, 11},
};
// The pad bit of each button control (Control order): select 0, L3 1, R3 2, start 3, up 4, right 5, down 6, left 7,
// L2 8, R2 9, L1 10, R1 11, triangle 12, circle 13, cross 14, square 15.
constexpr int kControlBit[16] = {4, 6, 7, 5, 14, 13, 15, 12, 10, 11, 8, 9, 3, 0, 1, 2};

int playerLayout(int player)
{
    const int32_t layout = G(frontEndVarG).gameStateData.playerConfig[player].controllerConfig;
    return layout >= 0 && layout < 5 ? (layout == 4 ? 0 : layout) : 0;
}

std::string action(int player, int control)
{
    const int layout = playerLayout(player);
    std::string out;
    if (control < 16)
    {
        for (int a = 0; a < 12; ++a)
            if (kLayouts[layout][a] == kControlBit[control])
                out += (out.empty() ? "" : ", ") + std::string(kActions[a]);
        if (control == B::Start)
            out = "Pause";
        return out;
    }
    switch (control)
    {
    case B::LeftStickLeft: return "Steer left";
    case B::LeftStickRight: return "Steer right";
    case B::LeftStickUp: return "Aim up";
    case B::LeftStickDown: return "Aim down";
    case B::RightStickUp: return layout == 1 || layout == 3 ? "Accelerate" : "";
    case B::RightStickDown: return layout == 1 || layout == 3 ? "Reverse" : "";
    default: return "";
    }
}

// What a player's controls are read from: the keyboard (player 1 on Auto, or a device with it) and a gamepad (Auto or a
// device with one).
bool usesKeyboard(int player, const B::Player &p) { return B::deviceHasKeyboard(p.device) || (p.device == B::DeviceAuto && player == 0) || B::active(player).keyboard; }
bool usesPad(const B::Player &p) { return p.device == B::DeviceAuto || B::deviceGamepad(p.device) >= 0; }

std::string bindingText(int player, const B::Player &p, int control)
{
    std::string out;
    if (usesKeyboard(player, p))
        out = B::sourceName(p.key[control]);
    if (usesPad(p))
        out += (out.empty() ? "" : " / ") + B::sourceName(p.pad[control]);
    return out.empty() ? "-" : out;
}

std::string shortName(std::string name, size_t max = 24)
{
    if (name.size() > max)
        name.resize(max);
    return name;
}

// The device choices: Auto with what it reads now, each gamepad with its name or "not connected" (those are skipped
// when changing the device; one chosen earlier shows that Auto stands in for it).
std::string deviceChoices(int player, const B::Player &p)
{
    const B::Active a = B::active(player);
    std::string out;
    for (int d = 0; d < B::DeviceCount; ++d)
    {
        if (d)
            out += '*';
        out += B::deviceName(d);
        if (d == B::DeviceAuto)
            out += " (" + shortName(B::activeName(player), 40) + ")";
        const int slot = B::deviceGamepad(d);
        if (slot >= 0)
        {
            if (B::gamepadPresent(slot))
                out += " (" + shortName(B::gamepadName(slot)) + ")";
            else if (d == p.device && a.fallback)
                out += " (not connected: Auto)";
            else
                out += " (not connected)";
        }
    }
    return out;
}

bool deviceAvailable(int d) { const int slot = B::deviceGamepad(d); return d >= 0 && d < B::DeviceCount && (slot < 0 || B::gamepadPresent(slot)); }

// The player pages' texts from the bindings (and the capture in progress).
void refreshControlTexts()
{
    const B::Player players[2] = {B::player(0), B::player(1)};
    for (int id = PagePlayer1; id < PageCount; ++id)
    {
        PageDef &d = g.defs[id];
        for (size_t i = 0; i < d.items.size(); ++i)
        {
            const Item &item = d.items[i];
            const B::Player &p = players[item.player];
            if (item.role == RoleDevice)
            {
                setText(item.choices, deviceChoices(item.player, p), kDeviceChoicesBytes);
                if (feedback(d, static_cast<int>(i)) != p.device)
                    feedback(d, static_cast<int>(i)) = d.last[i] = p.device;
            }
            else if (item.role == RoleControl)
            {
                const int c = item.control;
                const std::string act = action(item.player, c);
                setText(item.text, std::string(B::controlName(c)) + (act.empty() ? "" : " - " + act) + ": %s", kTextBytes);
                const std::string value =
                    g.capturePlayer == item.player && g.captureControl == c ? "press a key or button (Esc cancels, Del clears)" : bindingText(item.player, p, c);
                setText(item.choices, value + "*" + value, kTextBytes * 2u);
            }
        }
    }
}

bool makeDefs()
{
    PageDef &e = g.defs[PageEnhance];
    e.title = textGroup("Enhancements");
    for (const Choice &c : kChoices)
        e.items.push_back({textGroup(c.text), textGroup(c.choices), c.count, -1});
    PageDef &c = g.defs[PageControls];
    c.title = textGroup("Controls");
    c.items.push_back({textGroup("Player 1 PC Controls..."), 0, 0, PagePlayer1});
    c.items.push_back({textGroup("Player 2 PC Controls..."), 0, 0, PagePlayer2});
    for (int player = 0; player < 2; ++player)
    {
        PageDef &d = g.defs[PagePlayer1 + player];
        d.title = textGroup(player == 0 ? "Player 1 PC Controls" : "Player 2 PC Controls");
        Item device{textGroup("Device: %s"), textGroup("", kDeviceChoicesBytes), B::DeviceCount, -1, RoleDevice, player};
        d.items.push_back(device);
        d.items.push_back({textGroup("Buttons..."), 0, 0, PageButtons1 + player, RoleLink, player});
        d.items.push_back({textGroup("D-pad and sticks..."), 0, 0, PageSticks1 + player, RoleLink, player});
        d.items.push_back({textGroup("Reset to defaults: %s"), textGroup("Left or right*Left or right"), 2, -1, RoleReset, player});
        for (int page = 0; page < 2; ++page)
        {
            PageDef &sub = g.defs[(page == 0 ? PageButtons1 : PageSticks1) + player];
            sub.title = textGroup(page == 0 ? (player == 0 ? "Player 1 Buttons" : "Player 2 Buttons") : (player == 0 ? "Player 1 D-pad and Sticks" : "Player 2 D-pad and Sticks"));
            for (const int c : page == 0 ? std::vector<int>(std::begin(kButtonsPage), std::end(kButtonsPage)) : std::vector<int>(std::begin(kSticksPage), std::end(kSticksPage)))
                sub.items.push_back({textGroup("", kTextBytes), textGroup("", kTextBytes * 2u), 2, -1, RoleControl, player, c});
        }
    }
    for (PageDef &d : g.defs)
    {
        if (!d.title)
            return false;
        for (const Item &item : d.items)
            if (!item.text || (item.link < 0 && !item.choices))
                return false;
        d.feedback = alloc(static_cast<uint32_t>(d.items.size()) * 4u);
        d.last.assign(d.items.size(), 0);
        if (!d.feedback)
            return false;
    }
    // The values shown.
    const settings::Settings now = settings::get();
    for (int i = 0; i < kEnhanceCount; ++i)
        feedback(e, i) = e.last[i] = value(now, i);
    g.made = true;
    refreshControlTexts();
    return true;
}

bool allocate(Instance &inst, const PageDef &d)
{
    inst.header = alloc(kHeaderBytes);
    if (!inst.header)
        return false;
    for (size_t i = 0; i < d.items.size(); ++i)
    {
        inst.items.push_back(alloc(kItemBytes));
        inst.vars.push_back(d.items[i].link < 0 ? alloc(kVarBytes) : 0u);
        if (!inst.items.back() || (d.items[i].link < 0 && !inst.vars.back()))
            return false;
    }
    inst.made = true;
    return true;
}

ptr32<sMenuItemStd> lastItem(ptr32<sAMenuHeader> h)
{
    ptr32<sMenuItemStd> p = h->pMenuStart;
    while (p && p->pNext)
        p = p->pNext;
    return p;
}

void makeLink(uint32_t at, ptr32<sMenuItemStd> linkItem, uint32_t title, uint32_t target)
{
    std::memcpy(guest_host(at), guest_host(linkItem.address), kItemBytes);
    ptr32<sMenuItemStd> link(at);
    link->pTextGroup = ptr32<sTextGroup>(title);
    link->pMenuVar = nullptr;
    link->pNext = nullptr;
    link->pLinkMenu = ptr32<sAMenuHeader>(target);
    link->selectionTag = 0;
    link->flags = 0;
    link->statusFlags = 0;
    link->linkType = 0;
}

// A page, modelled on a page of the current menus (its header), one of its two-choice items and one of its link
// items; filled again at each install.
void fill(Instance (&set)[PageCount], int id, ptr32<sAMenuHeader> modelPage, ptr32<sMenuItemStd> choiceItem,
          ptr32<sMenuItemStd> linkItem, ptr32<sAMenuHeader> returnTo)
{
    const PageDef &d = g.defs[id];
    Instance &inst = set[id];
    std::memcpy(guest_host(inst.header), guest_host(modelPage.address), kHeaderBytes);
    ptr32<sAMenuHeader> h(inst.header);
    h->pTitleText = ptr32<sTextGroup>(d.title);
    h->pDialogTxt = nullptr;
    h->pAdditionalText = nullptr;
    h->pSpareText = nullptr;
    h->pSelection = nullptr;
    h->pValueStore = nullptr;
    h->pSpecialMenu = nullptr;
    h->pGlobalDestMenu = nullptr;
    h->pLinkMenuReturn = returnTo;
    h->selectionTag = 0;
    h->selection = 0;
    // A plain list, sized for its own items when it is first shown (not the copied page's scroll state or size).
    h->scrollStart = 0;
    h->scrollItems = 0;
    h->colSelect = 0;
    h->rowSelect = 0;
    h->width = 0;
    h->height = 0;
    const int count = static_cast<int>(d.items.size());
    h->menuItems = static_cast<uint8>(count);
    for (int i = 0; i < count; ++i)
    {
        const Item &it = d.items[i];
        const ptr32<sMenuItemStd> next = i + 1 < count ? ptr32<sMenuItemStd>(inst.items[i + 1]) : nullptr;
        if (it.link >= 0)
        {
            makeLink(inst.items[i], linkItem, it.text, set[it.link].header);
            ptr32<sMenuItemStd>(inst.items[i])->pNext = next;
            continue;
        }
        std::memcpy(guest_host(inst.items[i]), guest_host(choiceItem.address), kItemBytes);
        std::memcpy(guest_host(inst.vars[i]), guest_host(choiceItem->pMenuVar.address), kVarBytes);
        ptr32<sMenuItemStd> item(inst.items[i]);
        ptr32<sMenuItemVar> var(inst.vars[i]);
        item->pTextGroup = ptr32<sTextGroup>(it.text);
        item->pMenuVar = var;
        item->pNext = next;
        item->pLinkMenu = nullptr;
        item->selectionTag = 0;
        item->flags = 0;
        item->statusFlags = static_cast<uint8>(item->statusFlags & ~1u); // not disabled
        var->pTextGroup = ptr32<sTextGroup>(it.choices);
        var->tagMVar = static_cast<int32>(0x10000f00u + static_cast<uint32_t>(id) * 0x40u + static_cast<uint32_t>(i)); // in no link table
        var->minValue = 0;
        var->maxValue = it.count - 1;
        var->flags = 0; // a list of choices
        var->pFeedbackVar = ptr32<int32>(d.feedback + static_cast<uint32_t>(i) * 4u);
        var->value = feedback(d, i);
    }
    h->pMenuStart = ptr32<sMenuItemStd>(inst.items[0]);
}

// The first two-choice item of a page (the front end's "Weapon pick-ups", the pause menu's "Show Track Directions").
ptr32<sMenuItemStd> twoChoiceItem(ptr32<sAMenuHeader> page)
{
    for (ptr32<sMenuItemStd> p = page->pMenuStart; p; p = p->pNext)
        if (p->pMenuVar && p->pMenuVar->maxValue - p->pMenuVar->minValue == 1 && p->pMenuVar->pTextGroup)
            return p;
    return nullptr;
}

// The front end's own "Controller configuration" page (Game Options > Controls): the page one of whose items opens a
// player's "Define Player n Controls" page (its group tagged 0x10000214 / 0x10000215, as BuildKeyDefinitions knows it).
ptr32<sAMenuHeader> controllerConfigPage(ptr32<sAMenuHeader> options)
{
    for (ptr32<sMenuItemStd> p = options->pMenuStart; p; p = p->pNext)
    {
        const ptr32<sAMenuHeader> page = p->pLinkMenu;
        if (!page || page.address >= 0x2000000u)
            continue;
        for (ptr32<sMenuItemStd> q = page->pMenuStart; q; q = q->pNext)
        {
            const ptr32<sAMenuHeader> define = q->pLinkMenu;
            if (define && define.address < 0x2000000u && define->pUIgroup && define->pUIgroup.address < 0x2000000u &&
                (define->pUIgroup->selectionTag == 0x10000214u || define->pUIgroup->selectionTag == 0x10000215u))
                return page;
        }
    }
    return nullptr;
}

// ---- Rebinding ----
void startCapture(int player, int control)
{
    const B::Player p = B::player(player);
    const B::Active a = B::active(player);
    const bool keyboard = usesKeyboard(player, p);
    const int pad = a.activePad; // the gamepad it reads (on Auto, the one used last)
    if (!keyboard && pad < 0)
        return;
    g.capturePlayer = player;
    g.captureControl = control;
    B::beginCapture(keyboard, pad, 6000);
    std::fprintf(stderr, "[controls] player %d %s: waiting for a key or button\n", player + 1, B::controlName(control));
}

void finishCapture()
{
    int source = 0;
    const int result = B::captureResult(&source);
    if (result == B::CaptureWaiting)
        return;
    const int player = g.capturePlayer, control = g.captureControl;
    g.capturePlayer = g.captureControl = -1;
    B::Player p = B::player(player);
    if (result == B::CaptureKey || result == B::CapturePad)
    {
        int *slot = result == B::CaptureKey ? p.key : p.pad;
        const int old = slot[control];
        for (int c = 0; c < B::ControlCount; ++c)
            if (c != control && slot[c] == source)
            {
                slot[c] = old; // the other control takes this one's old input
                std::fprintf(stderr, "[controls] player %d %s: was on %s, now %s\n", player + 1, B::controlName(c), B::sourceName(source).c_str(),
                             B::sourceName(old).c_str());
            }
        slot[control] = source;
    }
    else if (result == B::CaptureClear)
    {
        // Delete is pressed on the keyboard: it clears the key, or for a gamepad-only player the gamepad input.
        (usesKeyboard(player, p) ? p.key : p.pad)[control] = 0;
    }
    std::fprintf(stderr, "[controls] player %d %s: %s\n", player + 1, B::controlName(control),
                 result == B::CaptureCancelled ? "cancelled" : result == B::CaptureTimedOut ? "timed out" : bindingText(player, p, control).c_str());
    if (result == B::CaptureKey || result == B::CapturePad || result == B::CaptureClear)
    {
        B::setPlayer(player, p);
        settings::saveControls();
    }
}

void pollControls()
{
    if (g.capturePlayer >= 0)
        finishCapture();
    for (int id = PagePlayer1; id < PageCount; ++id)
    {
        PageDef &d = g.defs[id];
        for (int i = 0; i < static_cast<int>(d.items.size()); ++i)
        {
            const Item &item = d.items[static_cast<size_t>(i)];
            const int v = feedback(d, i);
            if (item.role == RoleLink || v == d.last[static_cast<size_t>(i)])
                continue;
            d.last[static_cast<size_t>(i)] = v;
            const int player = item.player;
            if (item.role == RoleDevice)
            {
                B::Player p = B::player(player);
                // A gamepad that is not connected is skipped (on in the same direction; back to the old choice at the end).
                int dev = v;
                const int step = v > p.device ? 1 : -1;
                while (dev >= 0 && dev < B::DeviceCount && !deviceAvailable(dev))
                    dev += step;
                if (dev < 0 || dev >= B::DeviceCount)
                    dev = p.device;
                feedback(d, i) = d.last[static_cast<size_t>(i)] = dev;
                if (dev != p.device)
                {
                    p.device = dev;
                    B::setPlayer(player, p);
                    settings::saveControls();
                    std::fprintf(stderr, "[controls] player %d device: %s\n", player + 1, B::deviceName(dev));
                }
            }
            else if (item.role == RoleReset)
            {
                B::setPlayer(player, B::defaults(player));
                settings::saveControls();
                std::fprintf(stderr, "[controls] player %d: defaults\n", player + 1);
            }
            else if (item.role == RoleControl && g.capturePlayer < 0)
                startCapture(player, item.control);
        }
    }
    refreshControlTexts();
}

} // namespace

// After MenuInitScan has hidden the debug item: show it, and add Enhancements and Controls after the menu's last item.
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
    if (g.capturePlayer >= 0)
    {
        B::cancelCapture();
        g.capturePlayer = g.captureControl = -1;
    }
    if (!g.made && !makeDefs())
    {
        std::fprintf(stderr, "[enhance] could not build the Enhancements menu (guest memory)\n");
        return;
    }
    // What the pages show comes from the settings (the file may have been changed elsewhere).
    {
        const settings::Settings now = settings::get();
        PageDef &e = g.defs[PageEnhance];
        for (int i = 0; i < kEnhanceCount; ++i)
            feedback(e, i) = e.last[i] = value(now, i);
        refreshControlTexts();
    }
    // The front end's Game Options: its first item (Game Setup) links to the "Game options" page, the model. The pause
    // menu (its first item is Resume Race, no link): the pause menu itself is the model.
    const ptr32<sMenuItemStd> first = menu->pMenuStart;
    const bool front = first && first->pLinkMenu;
    Instance(&set)[PageCount] = front ? g.front : g.pause;
    uint32_t(&links)[2] = front ? g.frontLinks : g.pauseLinks;
    const ptr32<sAMenuHeader> model = front ? ptr32<sAMenuHeader>(first->pLinkMenu) : menu;
    const ptr32<sMenuItemStd> choiceItem = twoChoiceItem(model);
    const ptr32<sMenuItemStd> linkItem = front ? first : debugItem;
    if (!choiceItem)
        return;
    for (int id = 0; id < PageCount; ++id)
        if (!set[id].made && !allocate(set[id], g.defs[id]))
        {
            std::fprintf(stderr, "[enhance] could not build the Enhancements menu (guest memory)\n");
            return;
        }
    if ((!links[0] && !(links[0] = alloc(kItemBytes))) || (!links[1] && !(links[1] = alloc(kItemBytes))))
        return;
    // The front end: the player pages are reached from the game's own Controller configuration page (two items added
    // after its vibration settings); the pause menu (and the front end, should that page not be found) has a Controls
    // page for them.
    const ptr32<sAMenuHeader> config = front ? controllerConfigPage(menu) : nullptr;
    const ptr32<sAMenuHeader> playersFrom = config ? config : ptr32<sAMenuHeader>(set[PageControls].header);
    fill(set, PageEnhance, model, choiceItem, linkItem, menu);
    fill(set, PagePlayer1, model, choiceItem, linkItem, playersFrom);
    fill(set, PagePlayer2, model, choiceItem, linkItem, playersFrom);
    for (int player = 0; player < 2; ++player)
    {
        const ptr32<sAMenuHeader> back(set[PagePlayer1 + player].header);
        fill(set, PageButtons1 + player, model, choiceItem, linkItem, back);
        fill(set, PageSticks1 + player, model, choiceItem, linkItem, back);
    }
    fill(set, PageControls, model, choiceItem, linkItem, menu);
    if (config)
    {
        for (int i = 0; i < 2; ++i)
            if (!g.frontPlayerLinks[i] && !(g.frontPlayerLinks[i] = alloc(kItemBytes)))
                return;
        for (int i = 0; i < 2; ++i)
            makeLink(g.frontPlayerLinks[i], linkItem, g.defs[PageControls].items[static_cast<size_t>(i)].text, set[PagePlayer1 + i].header);
        ptr32<sMenuItemStd>(g.frontPlayerLinks[0])->pNext = ptr32<sMenuItemStd>(g.frontPlayerLinks[1]);
        const ptr32<sMenuItemStd> last = lastItem(config);
        if (last && last.address != g.frontPlayerLinks[1])
        {
            last->pNext = ptr32<sMenuItemStd>(g.frontPlayerLinks[0]);
            config->menuItems = static_cast<uint8>(config->menuItems + 2);
        }
    }
    makeLink(links[0], linkItem, g.defs[PageEnhance].title, set[PageEnhance].header);
    makeLink(links[1], linkItem, g.defs[PageControls].title, set[PageControls].header);
    ptr32<sMenuItemStd>(links[0])->pNext = config ? nullptr : ptr32<sMenuItemStd>(links[1]);
    const uint32_t lastLink = config ? links[0] : links[1];
    const uint8 added = config ? 1 : 2;
    // The rebuilt menu: Enhancements and Controls after its last item (once). In a cup the pause menu is another one,
    // the first group's view (MenuInitScan): the same items end that list too (the menu code sets each page's way back
    // to the menu it was entered from).
    const auto append = [&](ptr32<sAMenuHeader> h)
    {
        const ptr32<sMenuItemStd> last = lastItem(h);
        if (last && last.address != lastLink && last.address != links[0])
        {
            last->pNext = ptr32<sMenuItemStd>(links[0]);
            h->menuItems = static_cast<uint8>(h->menuItems + added);
        }
    };
    append(menu);
    if (!front)
        if (const uint32_t cupMenu = pUIGroup->viewPosTag; cupMenu != 0u && cupMenu != menu.address && cupMenu < 0x2000000u)
            append(ptr32<sAMenuHeader>(cupMenu));
}

// Each front-end frame and each paused race frame: a choice the player changed becomes the setting (applied live where
// it can be, and saved), or starts a rebind.
void poll()
{
    B::menuFrame(); // menus: the keyboard and every gamepad can drive them, whatever the device settings
    if (!g.made)
        return;
    if (std::getenv("RC_ENHANCE_DEBUG"))
    {
        static uint32_t lastActive = 0;
        const ptr32<sAMenuHeader> a = G(pMenuSystemG)->pActiveMenu;
        if (a && a.address != lastActive)
        {
            lastActive = a.address;
            std::fprintf(stderr, "[enhance-debug] active %08x flags %04x more %04x even %04x items %u sel %d scrollItems %d scrollStart %d dlg %d w %u h %u changeY %u sel* %08x\n",
                         a.address, a->flags, a->moreFlags, a->evenMoreFlags, a->menuItems, a->selection, a->scrollItems,
                         a->scrollStart, a->dlgLines, a->width, a->height, a->changeY, a->pSelection.address);
            for (ptr32<sMenuItemStd> p = a->pMenuStart; p; p = p->pNext)
                std::fprintf(stderr, "[enhance-debug]   item %08x flags %04x status %02x var %08x\n", p.address, p->flags, p->statusFlags, p->pMenuVar.address);
        }
    }
    PageDef &e = g.defs[PageEnhance];
    settings::Settings s = settings::get();
    bool changed = false;
    for (int i = 0; i < kEnhanceCount; ++i)
    {
        const int v = feedback(e, i);
        if (v == e.last[i] || v < 0 || v >= kChoices[i].count)
            continue;
        e.last[i] = v;
        apply(s, i, v);
        changed = true;
    }
    if (changed)
    {
        settings::set(s);
        RcRefreshRaceUpdate(); // the race frame rate and smooth motion, if a race is on
        std::fprintf(stderr, "[enhance] settings changed and saved to %s\n", settings::filePath());
    }
    pollControls();
}

} // namespace rc::enhance

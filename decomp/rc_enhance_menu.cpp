// The enhanced recomp's settings (rc_settings.h) and each player's bindings (runtime/ps2_input_bindings.h) as pages of
// the game's own menus.
//
// Front end: Game Options > Screen Adjust (the PS2's picture position, not needed on a PC) becomes Display (the
// screen settings: aspect ratio, HUD and menus, screen mode, resolution, frame rate, smooth motion, V-Sync,
// variable refresh, HUD scale, menu scale); the game's own Controller configuration page (Game
// Options > Controls) gets "Player 1 PC Controls..." and "Player 2 PC Controls...". The developers' hidden "Debug
// Menus" item is shown.
//
// Pause menu (a race's, and a cup's, which shares its items): the race's own items (Resume, Restart, Quit / Quit
// Championship, Show Track Directions), then "Game Options", which mirrors the front end's: Audio Setup (the pause
// menu's own volume bars), Display, Controls (the vibration settings and the PC controls pages) and Debug Menus. Not
// there: Game Setup (laps, weapons, number of cars: set before a race), Sound Mode (applied by the front end only),
// the controller layouts ("Define Player n Controls": front-end pages), Load/Save Game, Load Track, Credits.
//
// The front end's menus come from its level file and are rebuilt each time the front end loads (MenuInitScan); the
// pause menu comes from each race's level file. After MenuInitScan, install() finds the menu holding "Debug Menus"
// (the front end's Game Options, or the pause menu) and links in pages built here in guest memory: copies of a page of
// that menu system (the front end's "Game options" page, or the pause menu itself, for their look, placement, colours
// and borders) with choice items and link items copied from its own; the pause menu's moved items are its own. The
// game's menu code draws and runs them: left and right change a choice, which it writes to the item's feedback word;
// poll() (each front-end frame, each paused race frame) acts on a changed one. Each page's memory is allocated once;
// its contents are copied again from the current menus at every install, since the level data they point into
// (colour and border sets, the moved items) is reloaded with each level. Names the front end has in its four
// languages (Game Options, Audio Setup, Controls and their pages' titles) are copied from it for the pause menu;
// "Display" is Display / Affichage / Anzeige / Pantalla; the settings themselves are in English.
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

#include <algorithm>
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

// ---- Display ----
struct Choice
{
    const char *text;    // with %s for the value
    const char *choices; // '*' between them
    int count;
};
constexpr Choice kChoices[] = {
    {"Aspect ratio: %s", "4:3*16:9*16:10*21:9", 4},
    {"HUD and menus: %s", "Corrected*Stretched", 2},
    {"Screen mode: %s", "Window*Fullscreen", 2},
    {"Resolution (restart): %s", "1x*2x*3x*4x*5x*6x", 6},
    {"Frame rate: %s", "60*90*120*144*165*240", 6}, // races, and the menus with Smooth motion (settings key race_fps)
    {"Smooth motion: %s", "Off*On", 2},
    {"V-Sync: %s", "Off*On*Adaptive", 3},
    {"Variable refresh: %s", "Off*On", 2},
    {"HUD scale: %s", "1.0x*0.9x*0.8x*0.7x*0.6x*0.5x", 6},
    {"Menu scale: %s", "1.0x*0.9x*0.8x*0.7x*0.6x*0.5x", 6},
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
    case 7: return s.vrr ? 1 : 0;
    case 8: return s.hudScale;
    case 9: return s.menuScale;
    default: return s.prompts;
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
    case 7: s.vrr = v != 0; break;
    case 8: s.hudScale = v; break;
    case 9: s.menuScale = v; break;
    default: s.prompts = v; break;
    }
}

// ---- Pages ----
// Display: the screen settings. Options, Sound: the pause menu's Game Options and its Audio Setup. Controls: the pause
// menu's (in the front end the game's own Controller configuration page has the player links). A player's page: the
// device, links to the buttons and the d-pad and sticks (a page each: twelve rows fit a page), reset.
enum PageId { PageDisplay, PageOptions, PageSound, PageControls, PagePlayer1, PagePlayer2, PageButtons1, PageButtons2, PageSticks1, PageSticks2, PageCount };
enum Role { RoleSetting, RoleLink, RoleDevice, RoleControl, RoleReset, RoleMoved };
// The pause menu's items that move into its Game Options pages.
enum Moved { MovedMusic, MovedSfx, MovedVibration1, MovedVibration2, MovedDebug, MovedCount };
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
    int player = 0, control = 0; // a moved item: control is its Moved
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
// Texts the front end has in four languages, copied for the pause menu.
enum Copied { CopiedGameOptions, CopiedAudioSetup, CopiedAudioTitle, CopiedControls, CopiedControlsTitle, CopiedCount };
constexpr uint32_t kCopiedBytes = 64; // per language
struct Built
{
    bool made = false;
    PageDef defs[PageCount];
    Instance front[PageCount], pause[PageCount];
    uint32_t displayText = 0;                   // "Display" in the four languages
    uint32_t copied[CopiedCount]{};
    uint32_t pauseLink = 0;                     // the pause menu's Game Options item
    uint32_t frontFallbackLink = 0;             // Display after the front end's last item, should Screen Adjust not be found
    uint32_t frontPlayerLinks[2]{};             // the player pages' links in the game's Controller configuration page
    uint32_t moved[MovedCount]{};               // the pause menu's moved items (this level's)
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

// A text group with a string per language (English, French, German, Spanish), each with room for capacity bytes.
uint32_t textGroup4(const char *const (&text)[4], uint32_t capacity)
{
    const uint32_t group = alloc(16u + 4u * capacity);
    if (group == 0u)
        return 0u;
    for (uint32_t i = 0; i < 4; ++i)
    {
        const uint32_t at = group + 16u + i * capacity;
        std::strncpy(static_cast<char *>(guest_host(at)), text[i], capacity - 1u);
        *static_cast<uint32_t *>(guest_host(group + i * 4u)) = at;
    }
    return group;
}

// Copies a text group of the game's (each language's string) into one made by textGroup4.
void copyText4(uint32_t group, ptr32<sTextGroup> from, uint32_t capacity)
{
    if (group == 0u || !from)
        return;
    for (uint32_t i = 0; i < 4; ++i)
    {
        const ptr32<char> src = from->pText[i] ? from->pText[i] : from->pText[0];
        if (!src)
            continue;
        char *dst = static_cast<char *>(guest_host(group + 16u + i * capacity));
        std::strncpy(dst, static_cast<const char *>(guest_host(src.address)), capacity - 1u);
        dst[capacity - 1u] = 0;
    }
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
    g.displayText = textGroup4({"Display", "Affichage", "Anzeige", "Pantalla"}, 16);
    const char *const english[CopiedCount] = {"Game Options", "Audio Setup", "Audio options", "Controls", "Controller configuration"};
    for (int i = 0; i < CopiedCount; ++i)
        if (!(g.copied[i] = textGroup4({english[i], english[i], english[i], english[i]}, kCopiedBytes)))
            return false;
    PageDef &e = g.defs[PageDisplay];
    e.title = g.displayText;
    for (const Choice &c : kChoices)
        e.items.push_back({textGroup(c.text), textGroup(c.choices), c.count, -1});
    PageDef &o = g.defs[PageOptions];
    o.title = g.copied[CopiedGameOptions];
    o.items.push_back({g.copied[CopiedAudioSetup], 0, 0, PageSound});
    o.items.push_back({g.displayText, 0, 0, PageDisplay});
    o.items.push_back({g.copied[CopiedControls], 0, 0, PageControls});
    o.items.push_back({1, 0, 0, -1, RoleMoved, 0, MovedDebug});
    PageDef &snd = g.defs[PageSound];
    snd.title = g.copied[CopiedAudioTitle];
    snd.items.push_back({1, 0, 0, -1, RoleMoved, 0, MovedMusic});
    snd.items.push_back({1, 0, 0, -1, RoleMoved, 0, MovedSfx});
    PageDef &c = g.defs[PageControls];
    c.title = g.copied[CopiedControlsTitle];
    c.items.push_back({1, 0, 0, -1, RoleMoved, 0, MovedVibration1});
    c.items.push_back({1, 0, 0, -1, RoleMoved, 0, MovedVibration2});
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
            if (!item.text || (item.link < 0 && item.role != RoleMoved && !item.choices))
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
        const bool choice = d.items[i].link < 0 && d.items[i].role != RoleMoved;
        inst.items.push_back(choice || d.items[i].link >= 0 ? alloc(kItemBytes) : 0u);
        inst.vars.push_back(choice ? alloc(kVarBytes) : 0u);
        if ((choice || d.items[i].link >= 0) && !inst.items.back())
            return false;
        if (choice && !inst.vars.back())
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
    // The items in order (a moved item this level does not have is left out), then chained.
    std::vector<uint32_t> list;
    for (size_t i = 0; i < d.items.size(); ++i)
    {
        const Item &it = d.items[i];
        if (it.role == RoleMoved)
        {
            if (const uint32_t at = g.moved[it.control])
                list.push_back(at);
            continue;
        }
        list.push_back(inst.items[i]);
        if (it.link >= 0)
        {
            makeLink(inst.items[i], linkItem, it.text, set[it.link].header);
            continue;
        }
        std::memcpy(guest_host(inst.items[i]), guest_host(choiceItem.address), kItemBytes);
        std::memcpy(guest_host(inst.vars[i]), guest_host(choiceItem->pMenuVar.address), kVarBytes);
        ptr32<sMenuItemStd> item(inst.items[i]);
        ptr32<sMenuItemVar> var(inst.vars[i]);
        item->pTextGroup = ptr32<sTextGroup>(it.text);
        item->pMenuVar = var;
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
        var->value = feedback(d, static_cast<int>(i));
    }
    for (size_t i = 0; i < list.size(); ++i)
        ptr32<sMenuItemStd>(list[i])->pNext = i + 1 < list.size() ? ptr32<sMenuItemStd>(list[i + 1]) : nullptr;
    h->menuItems = static_cast<uint8>(list.size());
    h->pMenuStart = list.empty() ? nullptr : ptr32<sMenuItemStd>(list[0]);
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


// The front end's texts the pause menu reuses (in four languages): Game Options (the main menu's item that opens the
// menu), Audio Setup and its page's title, Controls and the Controller configuration page's title.
void copyFrontTexts(ptr32<sUIgroup> pUIGroup, ptr32<sAMenuHeader> options, ptr32<sAMenuHeader> config)
{
    for (ptr32<sUIgroup> grp = pUIGroup; grp->pSourceHeader.address != 0xffffffffu; grp = grp + 1)
        if (const ptr32<sAMenuHeader> h = grp->pSourceHeader)
            for (ptr32<sMenuItemStd> p = h->pMenuStart; p; p = p->pNext)
                if (p->pLinkMenu == options)
                    copyText4(g.copied[CopiedGameOptions], p->pTextGroup, kCopiedBytes);
    for (ptr32<sMenuItemStd> p = options->pMenuStart; p; p = p->pNext)
    {
        const ptr32<sAMenuHeader> page = p->pLinkMenu;
        if (!page || page.address >= 0x2000000u)
            continue;
        if (config && page == config)
        {
            copyText4(g.copied[CopiedControls], p->pTextGroup, kCopiedBytes);
            copyText4(g.copied[CopiedControlsTitle], page->pTitleText, kCopiedBytes);
        }
        for (ptr32<sMenuItemStd> q = page->pMenuStart; q; q = q->pNext)
            if (q->pMenuVar && q->pMenuVar->tagMVar == 0x1000001f) // the music volume: Audio options
            {
                copyText4(g.copied[CopiedAudioSetup], p->pTextGroup, kCopiedBytes);
                copyText4(g.copied[CopiedAudioTitle], page->pTitleText, kCopiedBytes);
                break;
            }
    }
}

// Which of the pause menu's items moves into its Game Options pages (-1: stays).
int movedKind(ptr32<sMenuItemStd> p)
{
    if (p->pMenuVar && p->pMenuVar->tagMVar == 0x1000001f)
        return MovedMusic;
    if (p->pMenuVar && p->pMenuVar->tagMVar == 0x10000026)
        return MovedSfx;
    switch (p->selectionTag)
    {
    case 0x1000028bu: return MovedVibration1;
    case 0x10000078u: return MovedVibration2;
    case kDebugMenusTag: return MovedDebug;
    default: return -1;
    }
}

// A pause menu: its moved items taken out (the first menu's kept for the pages: the cup menu has its own copies of the
// same settings), Game Options appended.
void shortenPauseMenu(ptr32<sAMenuHeader> h, bool keep)
{
    std::vector<uint32_t> list;
    for (ptr32<sMenuItemStd> p = h->pMenuStart; p; p = p->pNext)
    {
        if (p.address == g.pauseLink)
            continue;
        const int kind = movedKind(p);
        if (kind < 0)
            list.push_back(p.address);
        else if (keep && !g.moved[kind])
            g.moved[kind] = p.address;
    }
    list.push_back(g.pauseLink);
    for (size_t i = 0; i < list.size(); ++i)
        ptr32<sMenuItemStd>(list[i])->pNext = i + 1 < list.size() ? ptr32<sMenuItemStd>(list[i + 1]) : nullptr;
    h->pMenuStart = ptr32<sMenuItemStd>(list[0]);
    h->menuItems = static_cast<uint8>(list.size());
    if (h->selection >= static_cast<int8>(list.size()))
        h->selection = 0;
}

} // namespace

// After MenuInitScan has hidden the debug item (and, in the pause menu, the two-player items outside a two-player
// race): show it, and link in the pages.
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
        std::fprintf(stderr, "[enhance] could not build the settings pages (guest memory)\n");
        return;
    }
    // What the pages show comes from the settings (the file may have been changed elsewhere).
    {
        const settings::Settings now = settings::get();
        PageDef &e = g.defs[PageDisplay];
        for (int i = 0; i < kEnhanceCount; ++i)
            feedback(e, i) = e.last[i] = value(now, i);
        refreshControlTexts();
    }
    // The front end's Game Options: its first item (Game Setup) links to the "Game options" page, the model. The pause
    // menu (its first item is Resume Race, no link): the pause menu itself is the model.
    const ptr32<sMenuItemStd> first = menu->pMenuStart;
    const bool front = first && first->pLinkMenu;
    Instance(&set)[PageCount] = front ? g.front : g.pause;
    const ptr32<sAMenuHeader> model = front ? ptr32<sAMenuHeader>(first->pLinkMenu) : menu;
    const ptr32<sMenuItemStd> choiceItem = twoChoiceItem(model);
    if (!choiceItem)
        return;
    // A link item of this menu system to copy (the pause menu's Debug Menus moves, so a copy is taken first).
    static uint32_t linkModel = 0;
    if (!linkModel && !(linkModel = alloc(kItemBytes)))
        return;
    std::memcpy(guest_host(linkModel), guest_host((front ? first : debugItem).address), kItemBytes);
    const ptr32<sMenuItemStd> linkItem(linkModel);
    for (int id = 0; id < PageCount; ++id)
        if (!set[id].made && !allocate(set[id], g.defs[id]))
        {
            std::fprintf(stderr, "[enhance] could not build the settings pages (guest memory)\n");
            return;
        }
    if (front)
    {
        const ptr32<sAMenuHeader> config = controllerConfigPage(menu);
        copyFrontTexts(pUIGroup, menu, config);
        // The players' pages from the game's own Controller configuration page (two items after its vibration
        // settings); should that page not be found, from a Controls page of ours.
        const ptr32<sAMenuHeader> playersFrom = config ? config : ptr32<sAMenuHeader>(set[PageControls].header);
        std::fill(std::begin(g.moved), std::end(g.moved), 0u);
        fill(set, PageDisplay, model, choiceItem, linkItem, menu);
        fill(set, PageControls, model, choiceItem, linkItem, menu);
        for (int player = 0; player < 2; ++player)
        {
            fill(set, PagePlayer1 + player, model, choiceItem, linkItem, playersFrom);
            const ptr32<sAMenuHeader> back(set[PagePlayer1 + player].header);
            fill(set, PageButtons1 + player, model, choiceItem, linkItem, back);
            fill(set, PageSticks1 + player, model, choiceItem, linkItem, back);
        }
        if (config)
        {
            for (int i = 0; i < 2; ++i)
                if (!g.frontPlayerLinks[i] && !(g.frontPlayerLinks[i] = alloc(kItemBytes)))
                    return;
            for (int i = 0; i < 2; ++i)
                makeLink(g.frontPlayerLinks[i], linkItem, g.defs[PageControls].items[static_cast<size_t>(2 + i)].text, set[PagePlayer1 + i].header);
            ptr32<sMenuItemStd>(g.frontPlayerLinks[0])->pNext = ptr32<sMenuItemStd>(g.frontPlayerLinks[1]);
            const ptr32<sMenuItemStd> last = lastItem(config);
            if (last && last.address != g.frontPlayerLinks[1])
            {
                last->pNext = ptr32<sMenuItemStd>(g.frontPlayerLinks[0]);
                config->menuItems = static_cast<uint8>(config->menuItems + 2);
            }
        }
        // Screen Adjust (moving the PS2's picture) becomes Display.
        ptr32<sMenuItemStd> screenAdjust = nullptr;
        for (ptr32<sMenuItemStd> p = menu->pMenuStart; p; p = p->pNext)
            if (p->selectionTag == 0x10000203u)
                screenAdjust = p;
        if (screenAdjust)
        {
            screenAdjust->pTextGroup = ptr32<sTextGroup>(g.displayText);
            screenAdjust->pLinkMenu = ptr32<sAMenuHeader>(set[PageDisplay].header);
            screenAdjust->selectionTag = 0;
        }
        else if (g.frontFallbackLink || (g.frontFallbackLink = alloc(kItemBytes)))
        {
            makeLink(g.frontFallbackLink, linkItem, g.displayText, set[PageDisplay].header);
            const ptr32<sMenuItemStd> last = lastItem(menu);
            if (last && last.address != g.frontFallbackLink)
            {
                last->pNext = ptr32<sMenuItemStd>(g.frontFallbackLink);
                menu->menuItems = static_cast<uint8>(menu->menuItems + 1);
            }
        }
        return;
    }
    // The pause menu: the race's own items, then Game Options. In a cup the pause menu is another one, the first
    // group's view (MenuInitScan), with its own items for the same settings: shortened too (the menu code sets each
    // page's way back to the menu it was entered from).
    if (!g.pauseLink && !(g.pauseLink = alloc(kItemBytes)))
        return;
    std::fill(std::begin(g.moved), std::end(g.moved), 0u);
    makeLink(g.pauseLink, linkItem, g.copied[CopiedGameOptions], set[PageOptions].header);
    shortenPauseMenu(menu, true);
    if (const uint32_t cupMenu = pUIGroup->viewPosTag; cupMenu != 0u && cupMenu != menu.address && cupMenu < 0x2000000u)
        shortenPauseMenu(ptr32<sAMenuHeader>(cupMenu), false);
    fill(set, PageOptions, model, choiceItem, linkItem, menu);
    fill(set, PageSound, model, choiceItem, linkItem, ptr32<sAMenuHeader>(set[PageOptions].header));
    fill(set, PageDisplay, model, choiceItem, linkItem, ptr32<sAMenuHeader>(set[PageOptions].header));
    fill(set, PageControls, model, choiceItem, linkItem, ptr32<sAMenuHeader>(set[PageOptions].header));
    for (int player = 0; player < 2; ++player)
    {
        fill(set, PagePlayer1 + player, model, choiceItem, linkItem, ptr32<sAMenuHeader>(set[PageControls].header));
        const ptr32<sAMenuHeader> back(set[PagePlayer1 + player].header);
        fill(set, PageButtons1 + player, model, choiceItem, linkItem, back);
        fill(set, PageSticks1 + player, model, choiceItem, linkItem, back);
    }
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
    PageDef &e = g.defs[PageDisplay];
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

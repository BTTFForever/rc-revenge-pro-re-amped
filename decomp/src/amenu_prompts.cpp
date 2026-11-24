// Button prompts (rc_prompts.h): the menus' button pictures as the PS2's, as what the controls are bound to, or none.
#include "rc_prompts.h"
#include "rc_settings.h"
#include "runtime/ps2_input_bindings.h"
#include "amenu.h"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

namespace rc::prompts {

namespace {

namespace B = ps2_input_bindings;
using namespace rc::amenu;

// The PS2 key sheet's pictures ("}0" to "}7"): the d-pad's directions and the four face buttons (the key hints use
// "}0Change", "}1Move", "}7Back", "}6Select"; the controller layout pages "}4" to "}7" for square, circle, cross,
// triangle).
enum Key : int { KeyLeftRight = 0, KeyUpDown, KeyUp, KeyDown, KeySquare, KeyCircle, KeyCross, KeyTriangle, KeyCount };

// What a picture stands for: one control, or two (the arrows' pairs).
struct Meaning
{
    int a, b;
};
Meaning meaning(int key)
{
    switch (key)
    {
    case KeyUpDown: return {B::Up, B::Down};
    case KeyLeftRight: return {B::Left, B::Right};
    case KeyUp: return {B::Up, -1};
    case KeyDown: return {B::Down, -1};
    case KeySquare: return {B::Square, -1};
    case KeyCircle: return {B::Circle, -1};
    case KeyCross: return {B::Cross, -1};
    default: return {B::Triangle, -1};
    }
}

// raylib's arrow keys and SDL's d-pad buttons.
constexpr int kKeyRight = 262, kKeyLeft = 263, kKeyDown = 264, kKeyUp = 265;
constexpr int kPadUp = 11, kPadDown = 12, kPadLeft = 13, kPadRight = 14;

enum Art { ArtPs2, ArtFace, ArtCap, ArtText, ArtNone };
struct Label
{
    Art art = ArtPs2;
    int ps2 = 0;          // ArtPs2: the sheet's picture
    char letter = 0;      // ArtFace: A, B, X or Y
    std::string text;     // ArtCap
};

// The player whose bindings the prompts show: player 2 on the game's "Define Player 2 Controls" page, else player 1
// (the menus run on player 1's port).
int promptPlayer()
{
    const ptr32<sMenuSystem> ms = G(pMenuSystemG);
    if (!ms)
        return 0;
    const ptr32<sAMenuHeader> a = ms->pActiveMenu;
    if (a && a.address < 0x2000000u && a->pUIgroup && a->pUIgroup.address < 0x2000000u && a->pUIgroup->selectionTag == 0x10000215u)
        return 1;
    return 0;
}

std::string shortName(int source)
{
    std::string n = B::sourceName(source);
    if (n.rfind("Pad ", 0) == 0)
        n = n.substr(4);
    return n;
}

Label label(int key)
{
    if (settings::prompts() == settings::PromptsOff)
    {
        // Off: a face button's picture in a line of text (the controller layout pages: "Accelerate }6 button") reads
        // as its name, as the page's other lines do ("R1 button"); the arrows go.
        static const char *const kNames[KeyCount] = {"", "", "", "", "Square", "Circle", "Cross", "Triangle"};
        Label out;
        out.art = key >= KeySquare ? ArtText : ArtNone;
        out.text = kNames[key];
        return out;
    }
    const int player = promptPlayer();
    const B::PromptDevice dev = B::promptDevice(player);
    const B::Player p = B::player(player), d = B::defaults(player);
    const Meaning m = meaning(key);
    // An unbound control: the menus use the default.
    const auto source = [&](int control) { const int s = dev.gamepad ? p.pad[control] : p.key[control]; return s ? s : (dev.gamepad ? d.pad[control] : d.key[control]); };
    Label out;
    out.ps2 = key;
    if (m.b >= 0)
    {
        // The arrows: the PS2 picture while both are on the d-pad or the arrow keys, else "W/S".
        const int sa = source(m.a), sb = source(m.b);
        const auto arrow = [&](int s, int padButton, int keyCode)
        { return dev.gamepad ? s == (B::kPadButton | padButton) : s == (B::kKey | keyCode); };
        const bool updown = key == KeyUpDown;
        if (arrow(sa, updown ? kPadUp : kPadLeft, updown ? kKeyUp : kKeyLeft) && arrow(sb, updown ? kPadDown : kPadRight, updown ? kKeyDown : kKeyRight))
            return out;
        out.art = ArtCap;
        out.text = shortName(sa) + "/" + shortName(sb);
        return out;
    }
    const int s = source(m.a);
    if (dev.gamepad && (s & 0xF0000) == B::kPadButton)
    {
        const int b = s & 0xFFFF;
        switch (B::gamepadButtonLabel(dev.slot, b))
        {
        case 1: out.art = ArtFace; out.letter = 'A'; return out;
        case 2: out.art = ArtFace; out.letter = 'B'; return out;
        case 3: out.art = ArtFace; out.letter = 'X'; return out;
        case 4: out.art = ArtFace; out.letter = 'Y'; return out;
        case 5: out.ps2 = KeyCross; return out;
        case 6: out.ps2 = KeyCircle; return out;
        case 7: out.ps2 = KeySquare; return out;
        case 8: out.ps2 = KeyTriangle; return out;
        default: break;
        }
        if (b == kPadUp || b == kPadDown)
        {
            out.ps2 = b == kPadUp ? KeyUp : KeyDown;
            return out;
        }
    }
    out.art = ArtCap;
    out.text = s ? shortName(s) : "-";
    return out;
}

uint32_t g_text = 0; // a guest string for a label's text
constexpr uint32_t kTextBytes = 64;

uint32_t guestText(const std::string &text)
{
    if (!g_text && !(g_text = g_runtime->guestMalloc(kTextBytes, 16u)))
        return 0;
    std::string t = text.substr(0, kTextBytes - 1);
    std::memcpy(guest_host(g_text), t.c_str(), t.size() + 1);
    return g_text;
}

int textWidth(const std::string &text)
{
    const uint32_t at = guestText(text);
    return at ? static_cast<int>(FontLength(ptr32<char>(at))) : 8 * static_cast<int>(text.size());
}

constexpr int kCapPad = 5; // the key cap's room either side of its text

// A convex polygon (an even number of points about its centre) as fans of RectRender quads: each draws the triangles
// (centre, p[i+2], p[i+1]) and (centre, p[i], p[i+1]).
void polygon(int cx, int cy, const int (*pts)[2], int n, uint32_t rgb, ee_float otz)
{
    Locals f(0x30);
    for (int i = 0; i < n; i += 2)
    {
        const int *p0 = pts[i], *p1 = pts[(i + 1) % n], *p2 = pts[(i + 2) % n];
        f.sw(0x0, 0);
        f.sh(0x4, static_cast<uint32_t>(cx));
        f.sh(0x6, static_cast<uint32_t>(cy));
        f.sh(0x8, static_cast<uint32_t>(p0[0]));
        f.sh(0xa, static_cast<uint32_t>(p0[1]));
        f.sh(0xc, static_cast<uint32_t>(p1[0]));
        f.sh(0xe, static_cast<uint32_t>(p1[1]));
        f.sh(0x10, static_cast<uint32_t>(p2[0]));
        f.sh(0x12, static_cast<uint32_t>(p2[1]));
        f.sb(0x14, rgb >> 16);
        f.sb(0x15, rgb >> 8);
        f.sb(0x16, rgb);
        f.sb(0x17, 0);
        RectRender(f.ptr<_sRect>(0), otz);
    }
}

// An ellipse about (cx, cy) (the screen's lines are twice as tall as its pixels are wide: ry about rx / 2).
void ellipse(int cx, int cy, int rx, int ry, uint32_t rgb, ee_float otz)
{
    static const float kCos[16] = {1.0f, 0.924f, 0.707f, 0.383f, 0.0f, -0.383f, -0.707f, -0.924f, -1.0f, -0.924f, -0.707f, -0.383f, 0.0f, 0.383f, 0.707f, 0.924f};
    int pts[16][2];
    for (int i = 0; i < 16; ++i)
    {
        pts[i][0] = cx + static_cast<int>(static_cast<float>(rx) * kCos[i] + (kCos[i] < 0 ? -0.5f : 0.5f));
        const float s = kCos[(i + 12) % 16];
        pts[i][1] = cy + static_cast<int>(static_cast<float>(ry) * s + (s < 0 ? -0.5f : 0.5f));
    }
    polygon(cx, cy, pts, 16, rgb, otz);
}

// A rectangle with its corners cut (c pixels across, one line down).
void cap(int x0, int y0, int x1, int y1, int c, uint32_t rgb, ee_float otz)
{
    const int pts[8][2] = {{x0 + c, y0}, {x1 - c, y0}, {x1, y0 + 1}, {x1, y1 - 1}, {x1 - c, y1}, {x0 + c, y1}, {x0, y1 - 1}, {x0, y0 + 1}};
    polygon((x0 + x1) / 2, (y0 + y1) / 2, pts, 8, rgb, otz);
}

// Text in the current font at (x, y), in a colour, leaving the font's colour and the sprite parameters (the line
// being printed uses them) as they were.
void text(int x, int y, const std::string &s, uint8_t r, uint8_t g, uint8_t b)
{
    const uint32_t at = guestText(s);
    if (!at)
        return;
    uint8_t params[0x18];
    std::memcpy(params, guest_host(sym::scratchPadG), sizeof params);
    fontClass &font = G(fontVarG);
    const uint8_t sr = font.r, sg = font.g, sb = font.b;
    FontSetColour(r, g, b);
    FontPrint(static_cast<int16>(x), static_cast<int16>(y), ptr32<char>(at));
    font.r = sr;
    font.g = sg;
    font.b = sb;
    std::memcpy(guest_host(sym::scratchPadG), params, sizeof params);
}

bool logging()
{
    static const bool on = std::getenv("RC_PROMPTS_LOG") != nullptr;
    return on;
}

} // namespace

bool replaces(int key) { return key >= 0 && key < KeyCount && settings::prompts() != settings::PromptsPs2; }

bool dropsSpace(int key) { return label(key).art == ArtNone; }

int width(int key)
{
    const Label l = label(key);
    switch (l.art)
    {
    case ArtNone: return 0;
    case ArtText: return textWidth(l.text);
    case ArtCap: return textWidth(l.text) + 2 * kCapPad + 8;
    default: return 0x20;
    }
}

void draw(int x, int y, int key)
{
    const Label l = label(key);
    // The 2D is depth tested (greater): the rim at the PS2 pictures' depth, the face in front of it (the text, at 0,
    // in front of both).
    const ee_float otz = constant(sym::lit4_3275c4), front = otz * ee_float(0.5f);
    if (logging())
    {
        static std::set<std::string> seen;
        const std::string line = std::to_string(key) + " -> " + (l.art == ArtPs2 ? "ps2 " + std::to_string(l.ps2) : l.art == ArtFace ? std::string(1, l.letter) : l.art == ArtNone ? "none" : (l.art == ArtCap ? "cap " : "text ") + l.text);
        if (seen.insert(line).second)
            std::fprintf(stderr, "[prompts] key %s\n", line.c_str());
    }
    switch (l.art)
    {
    case ArtNone:
        return;
    case ArtText:
    {
        const fontClass &font = G(fontVarG);
        text(x, y, l.text, font.r, font.g, font.b); // in the line's colour
        return;
    }
    case ArtPs2:
        AMenuDisplayPsxKey(x, y, l.ps2);
        return;
    case ArtFace:
    {
        // A dark round button with its letter in the letter's colour, the size of the PS2's pictures.
        const int cx = x + 15, cy = y + 6;
        ellipse(cx, cy, 13, 7, 0x707070, otz);
        ellipse(cx, cy, 12, 6, 0x181818, front);
        static const uint8_t kColour[4][3] = {{0x20, 0x70, 0x20}, {0x7f, 0x20, 0x18}, {0x20, 0x48, 0x7f}, {0x7f, 0x68, 0x10}};
        const int i = l.letter == 'A' ? 0 : l.letter == 'B' ? 1 : l.letter == 'X' ? 2 : 3;
        const std::string s(1, l.letter);
        text(cx - textWidth(s) / 2, y, s, kColour[i][0], kColour[i][1], kColour[i][2]);
        return;
    }
    case ArtCap:
    {
        // A dark key cap with a light rim and the name in white, as the PS2 pictures look.
        const int w = textWidth(l.text) + 2 * kCapPad;
        cap(x + 1, y - 2, x + 1 + w, y + 13, 3, 0x909090, otz);
        cap(x + 2, y - 1, x + w, y + 12, 2, 0x202020, front);
        text(x + 1 + kCapPad, y, l.text, 0x7f, 0x7f, 0x7f);
        return;
    }
    }
}

bool hidesHints(uint32_t textAddress)
{
    if (!textAddress)
        return false;
    const char *s = static_cast<const char *>(guest_host(textAddress));
    bool pictures = false;
    for (const char *c = s; *c; ++c)
        if (c[0] == '}' && c[1] >= '0' && c[1] < '0' + KeyCount)
            pictures = true;
    if (logging())
    {
        static std::set<std::string> seen;
        if (seen.insert(s).second)
            std::fprintf(stderr, "[prompts] hints \"%s\"\n", s);
    }
    return pictures && settings::prompts() == settings::PromptsOff;
}

} // namespace rc::prompts

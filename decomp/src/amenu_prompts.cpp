// Button prompts (rc_prompts.h): the menus' button pictures and button names as the PS2's, as what the controls are
// bound to, or in neutral words.
#include "rc_prompts.h"
#include "rc_settings.h"
#include "runtime/ps2_input_bindings.h"
#include "amenu.h"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

namespace rc::prompts {

namespace {

namespace B = ps2_input_bindings;
using namespace rc::amenu;

// The PS2 key sheet's pictures ("}0" to "}7"): the d-pad's directions and the four face buttons (the key hints use
// "}0Change", "}1Move", "}7Back", "}6Select"; the controller layout pages "}4" to "}7" for square, circle, cross,
// triangle).
enum Key : int { KeyLeftRight = 0, KeyUpDown, KeyUp, KeyDown, KeySquare, KeyCircle, KeyCross, KeyTriangle, KeyCount };

// raylib's arrow keys and SDL's d-pad buttons and face buttons.
constexpr int kKeyRight = 262, kKeyLeft = 263, kKeyDown = 264, kKeyUp = 265;
constexpr int kPadSouth = 0, kPadUp = 11, kPadDown = 12, kPadLeft = 13, kPadRight = 14;

// Button names the texts spell out, found and turned into "}" + a token code (kTokenBase + its index in g_tokens).
enum Kind : int { KindL1, KindR1, KindL2, KindR2, KindStart, KindDpad, KindLeftStick, KindRightStick, KindRightStickBack, KindCount };
struct Token
{
    std::string original; // the text it stood for, in the game's encoding
    Kind kind;
};
std::vector<Token> g_tokens;
constexpr uint8_t kTokenBase = 0xe0;
constexpr size_t kMaxTokens = 0x1f;
int g_nested = 0; // printing a prompt's own text: no rewriting

// The game's text encoding (its font's pages): e 0xa9, o (acute) 0xb3, and "1" after L or R is 0xc5.
struct Phrase
{
    const char *text;
    Kind kind;
};
const Phrase kPhrases[] = {
    {"directional buttons", KindDpad}, {"touches directionnelle", KindDpad}, {"Richtungstasten", KindDpad},
    {"bot\xb3nes de direcci\xb3n", KindDpad},
    {"left analog stick", KindLeftStick}, {"joystick analogique gauche", KindLeftStick}, {"Linker Analog-Stick", KindLeftStick},
    {"palanca anal\xb3gica izquierda", KindLeftStick},
    {"right analog stick back", KindRightStickBack},
    {"right analog stick", KindRightStick}, {"joystick analogique droit", KindRightStick}, {"Rechter Analog-Stick", KindRightStick},
    {"palanca anal\xb3gica derecha", KindRightStick},
};
struct Word
{
    const char *text;
    Kind kind;
};
const Word kWords[] = {{"L1", KindL1}, {"L\xc5", KindL1}, {"R1", KindR1}, {"R\xc5", KindR1}, {"L2", KindL2}, {"R2", KindR2}, {"START", KindStart}};

// Off: neutral words in place of a button's name, per language (English, French, German, Spanish), made to read in
// each language's pattern: "<word> button", "Touche <word>", "<word>-Taste", "bot\xb3n <word>" (and the Track Editor
// help's prose: "the L2 button", "the START menu").
const char *const kNeutral[4][KindStart + 1] = {
    {"left shoulder", "right shoulder", "left trigger", "right trigger", "pause"},
    {"gauche haute", "droite haute", "gauche basse", "droite basse", "pause"},
    {"Linke-Schulter", "Rechte-Schulter", "Linke-Trigger", "Rechte-Trigger", "Pause"},
    {"lateral izquierdo", "lateral derecho", "gatillo izquierdo", "gatillo derecho", "pausa"},
};
// The face buttons by place: square, circle, cross, triangle.
const char *const kNeutralFace[4][4] = {
    {"left", "right", "bottom", "top"},
    {"de gauche", "de droite", "du bas", "du haut"},
    {"Linke", "Rechte", "Untere", "Obere"},
    {"izquierdo", "derecho", "inferior", "superior"},
};

int lang()
{
    const int l = language();
    return l >= 0 && l < 4 ? l : 0;
}

bool letter(uint8_t c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0x80 && c != 0xaa); }

int tokenIndex(const std::string &original, Kind kind)
{
    for (size_t i = 0; i < g_tokens.size(); ++i)
        if (g_tokens[i].original == original)
            return static_cast<int>(i);
    if (g_tokens.size() >= kMaxTokens)
        return -1;
    g_tokens.push_back({original, kind});
    return static_cast<int>(g_tokens.size() - 1);
}

// What a picture or a token is drawn as: pieces side by side.
enum Art { ArtPs2, ArtFace, ArtCap, ArtText };
struct Piece
{
    Art art = ArtPs2;
    int ps2 = 0;      // ArtPs2: the sheet's picture
    char letter = 0;  // ArtFace: A, B, X or Y
    std::string text; // ArtCap, ArtText
};
using Pieces = std::vector<Piece>;
Piece ps2(int key) { Piece p; p.art = ArtPs2; p.ps2 = key; return p; }
Piece capPiece(const std::string &t) { Piece p; p.art = ArtCap; p.text = t; return p; }
Piece textPiece(const std::string &t) { Piece p; p.art = ArtText; p.text = t; return p; }

// The player whose bindings the prompts show: player 2 on the game's "Define Player 2 Controls" page and in the race's
// pause menu when player 2 paused (mainVarG.pauser 2 while pauseMenu is up: only that player's pad picks there), else
// player 1 (the menus run on player 1's port).
int promptPlayer()
{
    if (at<uint16_t>(sym::mainVarG + 0x366u) == 1 && at<int16_t>(sym::mainVarG + 0x364u) == 2)
        return 1;
    const ptr32<sMenuSystem> ms = G(pMenuSystemG);
    if (!ms)
        return 0;
    const ptr32<sAMenuHeader> a = ms->pActiveMenu;
    if (a && a.address < 0x2000000u && a->pUIgroup && a->pUIgroup.address < 0x2000000u && a->pUIgroup->selectionTag == 0x10000215u)
        return 1;
    return 0;
}

struct Binding
{
    B::PromptDevice dev;
    B::Player p, d;
    bool playstation = false; // the gamepad's face buttons are labelled Cross, Circle, ...
    // An unbound control: the menus use the default.
    int source(int control) const
    {
        const int s = dev.gamepad ? p.pad[control] : p.key[control];
        return s ? s : (dev.gamepad ? d.pad[control] : d.key[control]);
    }
    bool isDefault(int control) const { return source(control) == (dev.gamepad ? d.pad[control] : d.key[control]); }
    // A source's name in the device's style: "RB", "LT", "Start" (a PlayStation pad: "R1", "L2", "Select"), "Space".
    std::string name(int s) const
    {
        std::string n = s ? B::sourceName(s) : "-";
        if (n.rfind("Pad ", 0) == 0)
            n = n.substr(4);
        if (playstation)
        {
            static const char *const kPs[][2] = {{"LB", "L1"}, {"RB", "R1"}, {"LT", "L2"}, {"RT", "R2"}, {"LS", "L3"}, {"RS", "R3"}, {"Back", "Select"}};
            for (const auto &m : kPs)
                if (n == m[0])
                    return m[1];
        }
        return n;
    }
};

Binding binding()
{
    Binding b;
    const int player = promptPlayer();
    b.dev = B::promptDevice(player);
    b.p = B::player(player);
    b.d = B::defaults(player);
    b.playstation = b.dev.gamepad && B::gamepadButtonLabel(b.dev.slot, kPadSouth) == 5;
    return b;
}

// One control as bound: a gamepad's face button as it is labelled (PS shapes, or A/B/X/Y), else a key cap.
Piece control(const Binding &b, int c)
{
    const int s = b.source(c);
    if (b.dev.gamepad && (s & 0xF0000) == B::kPadButton)
    {
        const int button = s & 0xFFFF;
        Piece p;
        switch (B::gamepadButtonLabel(b.dev.slot, button))
        {
        case 1: case 2: case 3: case 4:
            p.art = ArtFace;
            p.letter = "ABXY"[B::gamepadButtonLabel(b.dev.slot, button) - 1];
            return p;
        case 5: return ps2(KeyCross);
        case 6: return ps2(KeyCircle);
        case 7: return ps2(KeySquare);
        case 8: return ps2(KeyTriangle);
        default: break;
        }
        if (button == kPadUp || button == kPadDown)
            return ps2(button == kPadUp ? KeyUp : KeyDown);
    }
    return capPiece(b.name(s));
}

// Two controls (a direction's pair): the PS2's arrows while both are on the d-pad or the arrow keys, else "J/L".
Piece pair(const Binding &b, int c0, int c1, bool upDown)
{
    const int s0 = b.source(c0), s1 = b.source(c1);
    const auto arrow = [&](int s, int padButton, int keyCode) { return b.dev.gamepad ? s == (B::kPadButton | padButton) : s == (B::kKey | keyCode); };
    if (arrow(s0, upDown ? kPadUp : kPadLeft, upDown ? kKeyUp : kKeyLeft) && arrow(s1, upDown ? kPadDown : kPadRight, upDown ? kKeyDown : kKeyRight))
        return ps2(upDown ? KeyUpDown : KeyLeftRight);
    return capPiece(b.name(s0) + "/" + b.name(s1));
}

Pieces keyPieces(int key)
{
    const int mode = settings::prompts();
    if (mode == settings::PromptsPs2)
        return {ps2(key)};
    if (mode == settings::PromptsOff)
    {
        // A face button by its place, in the line's colour; the arrows go (with the space after them).
        if (key >= KeySquare)
            return {textPiece(kNeutralFace[lang()][key - KeySquare])};
        return {};
    }
    const Binding b = binding();
    switch (key)
    {
    case KeyLeftRight: return {pair(b, B::Left, B::Right, false)};
    case KeyUpDown: return {pair(b, B::Up, B::Down, true)};
    case KeyUp: return {control(b, B::Up)};
    case KeyDown: return {control(b, B::Down)};
    case KeySquare: return {control(b, B::Square)};
    case KeyCircle: return {control(b, B::Circle)};
    case KeyCross: return {control(b, B::Cross)};
    default: return {control(b, B::Triangle)};
    }
}

Pieces tokenPieces(int id)
{
    if (id < 0 || id >= static_cast<int>(g_tokens.size()))
        return {};
    const Token &t = g_tokens[static_cast<size_t>(id)];
    const int mode = settings::prompts();
    const bool phrase = t.kind >= KindDpad;
    if (mode == settings::PromptsPs2 || (mode == settings::PromptsOff && phrase))
        return {textPiece(t.original)}; // the d-pad and the sticks are named neutrally already
    if (mode == settings::PromptsOff)
        return {textPiece(kNeutral[lang()][t.kind])};
    const Binding b = binding();
    // A stick or the d-pad with nothing bound (player 2's keyboard leaves the sticks unbound): the words.
    const auto unbound = [&](int c0, int c1) { return b.source(c0) == 0 && b.source(c1) == 0; };
    if ((t.kind == KindDpad && unbound(B::Left, B::Right)) || (t.kind == KindLeftStick && unbound(B::LeftStickLeft, B::LeftStickRight)) ||
        (t.kind >= KindRightStick && unbound(B::RightStickUp, B::RightStickDown)))
        return {textPiece(t.original)};
    switch (t.kind)
    {
    case KindL1: return {control(b, B::L1)};
    case KindR1: return {control(b, B::R1)};
    case KindL2: return {control(b, B::L2)};
    case KindR2: return {control(b, B::R2)};
    case KindStart: return {control(b, B::Start)};
    case KindDpad:
        // Steering: a gamepad on its d-pad keeps the words; else the keys.
        if (b.dev.gamepad && b.isDefault(B::Left) && b.isDefault(B::Right))
            return {textPiece(t.original)};
        return {pair(b, B::Left, B::Right, false)};
    case KindLeftStick:
        if (b.dev.gamepad && b.isDefault(B::LeftStickLeft) && b.isDefault(B::LeftStickRight))
            return {textPiece(t.original)};
        return {capPiece(b.name(b.source(B::LeftStickLeft)) + "/" + b.name(b.source(B::LeftStickRight)))};
    case KindRightStick:
    case KindRightStickBack:
    {
        if (b.dev.gamepad && b.isDefault(B::RightStickUp) && b.isDefault(B::RightStickDown))
            return {textPiece(t.original)};
        // English says which way ("back"); the other languages name the stick for both rows.
        if (t.kind == KindRightStickBack)
            return {control(b, B::RightStickDown)};
        if (t.original == "right analog stick")
            return {control(b, B::RightStickUp)};
        return {capPiece(b.name(b.source(B::RightStickUp)) + "/" + b.name(b.source(B::RightStickDown)))};
    }
    default: return {};
    }
}

Pieces pieces(uint8_t code)
{
    if (code >= '0' && code < '0' + KeyCount)
        return keyPieces(code - '0');
    return tokenPieces(code - kTokenBase);
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

constexpr int kFaceWidth = 0x20; // as the PS2's pictures

int pieceWidth(const Piece &p)
{
    switch (p.art)
    {
    case ArtText: return textWidth(p.text);
    case ArtCap: return textWidth(p.text) + 2 * kCapPad + 8;
    default: return kFaceWidth;
    }
}

void drawPiece(int x, int y, const Piece &p)
{
    // The 2D is depth tested (greater): the rim at the PS2 pictures' depth, the face in front of it (the text, at 0,
    // in front of both).
    const ee_float otz = constant(sym::lit4_3275c4), front = otz * ee_float(0.5f);
    switch (p.art)
    {
    case ArtText:
    {
        const fontClass &font = G(fontVarG);
        text(x, y, p.text, font.r, font.g, font.b); // in the line's colour
        return;
    }
    case ArtPs2:
        AMenuDisplayPsxKey(x, y, p.ps2);
        return;
    case ArtFace:
    {
        // A dark round button with its letter in the letter's colour, the size of the PS2's pictures.
        const int cx = x + 15, cy = y + 6;
        ellipse(cx, cy, 13, 7, 0x707070, otz);
        ellipse(cx, cy, 12, 6, 0x181818, front);
        static const uint8_t kColour[4][3] = {{0x20, 0x70, 0x20}, {0x7f, 0x20, 0x18}, {0x20, 0x48, 0x7f}, {0x7f, 0x68, 0x10}};
        const int i = p.letter == 'A' ? 0 : p.letter == 'B' ? 1 : p.letter == 'X' ? 2 : 3;
        const std::string s(1, p.letter);
        text(cx - textWidth(s) / 2, y, s, kColour[i][0], kColour[i][1], kColour[i][2]);
        return;
    }
    case ArtCap:
    {
        // A dark key cap with a light rim and the name in white, as the PS2 pictures look.
        const int w = textWidth(p.text) + 2 * kCapPad;
        cap(x + 1, y - 2, x + 1 + w, y + 13, 3, 0x909090, otz);
        cap(x + 2, y - 1, x + w, y + 12, 2, 0x202020, front);
        text(x + 1 + kCapPad, y, p.text, 0x7f, 0x7f, 0x7f);
        return;
    }
    }
}

std::string describe(const Pieces &ps)
{
    std::string out;
    for (const Piece &p : ps)
        out += (out.empty() ? "" : " + ") + (p.art == ArtPs2 ? "ps2 " + std::to_string(p.ps2) : p.art == ArtFace ? std::string(1, p.letter) : (p.art == ArtCap ? "cap " : "text ") + p.text);
    return out.empty() ? "none" : out;
}

} // namespace

bool replaces(uint8_t code)
{
    if (code >= kTokenBase)
        return true;
    return code >= '0' && code < '0' + KeyCount && settings::prompts() != settings::PromptsPs2;
}

bool dropsSpace(uint8_t code) { return code < kTokenBase && pieces(code).empty(); }

int width(uint8_t code)
{
    ++g_nested;
    int w = 0;
    for (const Piece &p : pieces(code))
        w += pieceWidth(p);
    --g_nested;
    return w;
}

void draw(int x, int y, uint8_t code)
{
    ++g_nested;
    const Pieces ps = pieces(code);
    if (logging())
    {
        static std::set<std::string> seen;
        const std::string line = (code >= kTokenBase && code - kTokenBase < static_cast<int>(g_tokens.size()) ? "\"" + g_tokens[code - kTokenBase].original + "\"" : std::string("}") + static_cast<char>(code)) + " -> " + describe(ps);
        if (seen.insert(line).second)
            std::fprintf(stderr, "[prompts] %s\n", line.c_str());
    }
    for (const Piece &p : ps)
    {
        drawPiece(x, y, p);
        x += pieceWidth(p);
    }
    --g_nested;
}

void rewrite(uint32_t address)
{
    if (g_nested || !address || settings::prompts() == settings::PromptsPs2 || address >= 0x2000000u)
        return;
    char *s = static_cast<char *>(guest_host(address));
    const size_t n = std::strlen(s);
    if (n == 0 || n > 400)
        return;
    // A whole string (or "/" and the rest) naming the d-pad or a stick: the controller layout pages' right column.
    {
        char *at = s[0] == '/' ? s + 1 : s;
        for (const Phrase &ph : kPhrases)
            if (std::strcmp(at, ph.text) == 0)
            {
                const int id = tokenIndex(ph.text, ph.kind);
                if (id >= 0)
                {
                    at[0] = '}';
                    at[1] = static_cast<char>(kTokenBase + id);
                    at[2] = 0;
                }
                return;
            }
    }
    // Button names as words: "R2 button", "Touche L1", "L2-Taste", "the START menu".
    for (size_t i = 0; s[i]; ++i)
    {
        if (i > 0 && (letter(static_cast<uint8_t>(s[i - 1])) || s[i - 1] == '}'))
            continue;
        for (const Word &w : kWords)
        {
            const size_t len = std::strlen(w.text);
            if (std::strncmp(s + i, w.text, len) != 0 || letter(static_cast<uint8_t>(s[i + len])))
                continue;
            const int id = tokenIndex(w.text, w.kind);
            if (id < 0)
                return;
            s[i] = '}';
            s[i + 1] = static_cast<char>(kTokenBase + id);
            if (len > 2)
                std::memmove(s + i + 2, s + i + len, std::strlen(s + i + len) + 1);
            ++i;
            break;
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

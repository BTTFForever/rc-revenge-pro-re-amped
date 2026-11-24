// Widescreen (rc_settings.h: aspect, hud): Hor+ 3D and the 2D layer scaled back to the right shape.
//
// 3D: the game's x projection scales (Rdr_SetupCamera's dValX, shape.cpp's alpha camera, VfxSetupCameraMatrix's
// effects scale) are multiplied by hscale = (4/3) / aspect and the culling planes widened to match (rdr.cpp, display.cpp),
// so the vertical view stays as on the PS2 and the horizontal view widens. The picture is presented at the aspect.
//
// 2D: the game still draws its 640-wide 2D layer, which the wider presentation stretches. A scope around each HUD or
// 2D drawing function scales the x of every vertex it wrote to the 2D packet by hscale about an anchor: the HUD corners
// about the screen's corners (pinned to the edges), menus, text and sprites about the centre (pillarboxed). A UI scope
// leaves full-screen backgrounds and fades (edge to edge, 200+ lines high) filling the screen. Only the outermost scope
// applies. Off in split screen.
//
// HUD and menu scale (rc_settings.h hudScale, menuScale): the same scopes also shrink x and y, the HUD's elements toward
// their anchor's edge or corner (in split screen, of their own half), the menus (and the race's other 2D: the HUD
// scale) toward the centre. Menus' clip rectangles follow (menuScissor).
#pragma once

#include <cstdint>

class PS2Runtime;

namespace rc::widescreen {

// The horizontal half angle (radians) that keeps the vertical view of `half` at 4:3 and widens it to the aspect.
float widenHalfAngle(float half);

class AnchorScope
{
public:
    enum Kind : uint8_t { Hud, Ui };
    // An anchor (render/gfx/tags.h HudAnchor), or kAuto: the HUD scale toward the screen's (or the half's) edge or
    // corner nearest the element, the widescreen scale about the centre.
    static constexpr uint32_t kAuto = 11;
    explicit AnchorScope(uint32_t anchor, Kind kind = Hud);
    ~AnchorScope();
    AnchorScope(const AnchorScope &) = delete;
    AnchorScope &operator=(const AnchorScope &) = delete;

private:
    friend void packetSending();
    friend void packetSent();
    void apply(); // the vertices written since the scope opened (or since the packet was last sent)
    bool m_on = false, m_counted = false, m_widescreen = false, m_split = false;
    int m_menuRow = 0;
    Kind m_kind = Hud;
    float m_scale = 1.0f; // HUD or menu scale
    uint32_t m_anchor = 0, m_buffer = 0;
    int32_t m_tag = -1, m_start = 0;
};

// Menus drawn inside this scale toward the screen's top (-1), centre (0) or bottom (1): the key hints under a menu
// (AMenuDisplayOptions) stay at the bottom.
struct MenuRowScope
{
    explicit MenuRowScope(int row);
    ~MenuRowScope();
    int m_saved;
};

// GfxP2_RenderDmaSendActual: the open scope's vertices are placed before the packet goes, and it continues in the
// next buffer.
void packetSending();
void packetSent();

// AMenuRectClipStart: a menu's clip rectangle (pixels from the menus' 2D origin) moved with its contents; false if
// nothing changes.
bool menuScissor(int32_t &x1, int32_t &y1, int32_t &x2, int32_t &y2);

// The race's full-screen menus (PauseMenuProcess: the pause menu and its pages, the results, the cup ranking, the
// confirm boxes) are drawn inside this: pillarboxed in a split-screen race too, where the rest of the 2D (each half's
// HUD) keeps the original layout.
struct RaceMenuScope
{
    RaceMenuScope();
    ~RaceMenuScope();
};

} // namespace rc::widescreen

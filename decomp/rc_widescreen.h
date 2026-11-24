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
    explicit AnchorScope(uint32_t anchor, Kind kind = Hud);
    ~AnchorScope();
    AnchorScope(const AnchorScope &) = delete;
    AnchorScope &operator=(const AnchorScope &) = delete;

private:
    bool m_on = false, m_counted = false;
    Kind m_kind = Hud;
    float m_scale = 1.0f;
    uint32_t m_buffer = 0, m_anchorX = 0;
    int32_t m_tag = -1, m_start = 0;
};

// The race's full-screen menus (PauseMenuProcess: the pause menu and its pages, the results, the cup ranking, the
// confirm boxes) are drawn inside this: pillarboxed in a split-screen race too, where the rest of the 2D (each half's
// HUD) keeps the original layout.
struct RaceMenuScope
{
    RaceMenuScope();
    ~RaceMenuScope();
};

} // namespace rc::widescreen

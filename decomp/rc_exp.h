// exp/quick-dirty: the experimental PC upgrades (EXPERIMENT.md). Not behaviour-matching: every switch is off by default,
// and with none set each hook is one test of a false flag, so the game runs as on main.
//
//   RC_WIDESCREEN=1        Hor+ widescreen (16:9 unless RC_ASPECT=<w:h or ratio>), 16:9 presentation, wider culling
//   RC_HUD_ANCHOR=1        with RC_WIDESCREEN: HUD corners pinned to the screen corners at the right aspect
//   RC_INTERP=1            interpolated in-between pictures (camera and cars), RC_FPS=<n> (default 120) per second
//   RC_INTERP_PACE=0       do not space the in-between pictures over the tick (present as soon as drawn)
//   RC_INTERP_FORCE=1      keep drawing in-between pictures even when a tick missed its vertical blank
//   RC_INTERP_OBJECTS=0    interpolate the camera only
//   RC_EXP_LOG=1           a line per second of interpolation counters on stderr (session.log)
#pragma once

#include <cstdint>

namespace rc::exp {

struct Config
{
    bool widescreen = false;
    float aspect = 4.0f / 3.0f; // the presented aspect
    float hscale = 1.0f;        // (4/3) / aspect: the projection's x scale (0.75 for 16:9)
    bool hudAnchor = false;
    bool interp = false;
    int perTick = 1;            // pictures per game tick (RC_FPS / field rate, at least 2 when RC_INTERP)
    bool pace = true;
    bool force = false;
    bool objects = true;
    bool log = false;
};

const Config &config();
inline bool widescreen() { return config().widescreen; }
inline float hscale() { return config().hscale; }

// The horizontal half angle (radians) that keeps the vertical view of `half` at 4:3 and widens it to the aspect.
float widenHalfAngle(float half);

// HUD anchoring (RC_HUD_ANCHOR): scales the x of every vertex written to the 2D packet in its scope by hscale about
// the anchor (rc::gfx::HudAnchor: left, right or centre; stretch and the rest untouched).
class HudAnchorScope
{
public:
    explicit HudAnchorScope(uint32_t anchor);
    ~HudAnchorScope();
    HudAnchorScope(const HudAnchorScope &) = delete;
    HudAnchorScope &operator=(const HudAnchorScope &) = delete;

private:
    bool m_on = false;
    uint32_t m_buffer = 0, m_anchorX = 0;
    int32_t m_tag = -1, m_start = 0;
};

// Frame interpolation (RC_INTERP), driven from MainGameProcess.
namespace interp {
void reset();                    // a new race: forget the last tick
void capture();                  // after the tick's logic: last = current, current = the game's state now
bool active(bool paused);        // whether this tick gets in-between pictures
int passes();                    // in-between pictures this tick (perTick - 1)
void apply(float alpha);         // writes the blend of last and current tick into the game's camera and cars
void restore();                  // writes the current tick back
void save2D();                   // keeps the 2D packet and chain the tick wrote before its in-between pictures
void replay2D();                 // writes them again for the tick's own picture
void paceWait(int k);            // waits until k/perTick of the field has passed since the last vertical blank
void endFrameNoVSync();          // Rdr_EndFrame without waiting for the vertical blank (and without texture animation)
void afterRealFrame();           // after the tick's own Rdr_EndFrame: vertical blank time, missed-blank check
} // namespace interp

} // namespace rc::exp

// Vertical split screen (rc_settings.h split=vertical): a two-player race with player 1 on the left half of the screen
// and player 2 on the right, instead of the PS2's player 1 above player 2.
//
// The PS2 draws each half with the full screen's projection, its centre moved up or down by about a quarter of the
// screen (the cameras' vertDrawOffset, the shape packets' y centre) and a scissor for its half. Side by side, each half
// keeps the full screen's centre line, its x centre moves 160 pixels left or right (the shape packets' x centre, the
// word before the y centre), and the scissors are the two halves with a 4-pixel black gap between them.
//
// A half is 320 pixels by the whole height: in 4:3 a 2:3 picture, in 16:9 8:9. Keeping the full screen's scale would
// keep the vertical view and cut the horizontal one to half; scaling the projection by the half's presented aspect
// alone would keep the horizontal view and make the vertical one twice as tall. Each half's projection is instead scaled
// by zoom() = sqrt((aspect / 2) / (4 / 3)) (0.71 in 4:3, 0.82 in 16:9, 0.94 in 21:9; at most 1), so the horizontal
// view narrows by that factor and the vertical one widens by its inverse: the loss is shared equally between them (as
// tangents of the half angles). With the widescreen Hor+ scale on top, square pixels are kept at every aspect.
//
// The 2D HUD (rc_widescreen.cpp) is moved from each horizontal half to its vertical one: player 2's elements to the same
// corners of the right half, player 1's mirrored to the left half (its position and lap panel and its speed at the
// screen's left edge, its weapon at the inner edge), so both players' panels sit at the screen's outer edges and the
// map, which the PS2 put across the line between the halves at the left, sits at the bottom of the line between them.
#pragma once

#include <cstdint>

namespace rc::split {

// The half being drawn: 0 none (full screen, the overlay), 1 player 1's, 2 player 2's (gRdrPolyPlayer, set by
// SetFrameWindow).
int view();
// Side by side for the half being drawn: the setting is on and a half is being drawn.
bool sideBySide();
// The 2D: this is a two-player race drawn side by side (the setting, the split-screen byte, a race running).
bool raceSideBySide();
// The projection's scale (both axes) for a half drawn side by side.
float zoom();
// The GS x of the current view's centre: 2048, or 2048 -/+ 160 for a half drawn side by side.
float centreX();
// For the 3D renderers: the x and y projection scales (the camera's dValX, dValY) of a half drawn side by side, from
// the PS2's (4:3, full-screen) ones; the widescreen Hor+ scale included.
void scaleProjection(float &dValX, float &dValY);
// The camera's side and top/bottom half angles (radians) that cover a half drawn side by side, given its final
// dValX/dValY and the PS2's half angle (which the PS2 uses for both pairs of planes); never narrower than needed.
void halfAngles(float dValX, float dValY, float psHalf, float &sideHalf, float &topHalf);

// View3dInitViewconeReject: the view cone's half width and height (negative, in the window's units: 160 pixels across
// half a 4:3 screen at the projection distance) for a half side by side, from the window's width.
void viewCone(int16_t windowWidth, int16_t &halfX, int16_t &halfY);
// SetFrameWindow: the scissor of a window (0, 1, 2) side by side; returns false if the setting is off or not split.
bool scissor(int window, int &x1, int &y1, int &x2, int &y2);
// Called by SetFrameWindow each time a window is made current: notices the setting changing mid-race (the view cone
// is cached per window and must be rebuilt).
void frameWindow(int window);

} // namespace rc::split

// Button prompts (rc_settings.h prompts): the button pictures in the menus' texts. The game's font draws "}0" to "}7"
// as pictures from the PS2 key sheet through AMenuDisplayPsxKey (FontSpecialCharacterOrColourChange): the arrows,
// square, circle, cross and triangle. "PS2" keeps them. "Match controls" shows what the button is bound to on the
// device the player used last (ps2_input_bindings::promptDevice): a PlayStation-style gamepad keeps the PS2 pictures
// (by its own face labels), other gamepads get a round button with the letter in its colour (A green, B red, X blue,
// Y yellow) or a key cap with the button's name (LB, Start, ...), the keyboard a key cap with the key's name; the
// arrows stay pictures while they are on the d-pad or the arrow keys. "Off": the key-hint line under a menu ("Back",
// "Select", "Move") is not drawn; elsewhere (the controller layout pages' "Accelerate }6 button") a face button's
// picture becomes its name in the line's colour ("Cross button", as the page's "R1 button") and the arrows go, with
// the space after them.
#pragma once

#include <cstdint>

namespace rc::prompts {

// The font's "}<key>" with the menus' picture function: true if this module draws it (not the PS2 picture).
bool replaces(int key);
// Its advance in pixels (the PS2 picture's is 32; Off: 0).
int width(int key);
// Draws it at the pen (x, y): the line's top, as the PS2 picture is placed.
void draw(int x, int y, int key);
// Off: a space after a picture that is left out goes too.
bool dropsSpace(int key);
// AMenuDisplayOptions: the key-hint line (a string with pictures) is not drawn.
bool hidesHints(uint32_t text);

} // namespace rc::prompts

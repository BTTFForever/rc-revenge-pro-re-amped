// Button prompts (rc_settings.h prompts): the button pictures in the menus' texts. The game's font draws "}0" to "}7"
// as pictures from the PS2 key sheet through AMenuDisplayPsxKey (FontSpecialCharacterOrColourChange): the arrows,
// square, circle, cross and triangle. "PS2" keeps them. "Match controls" shows what the button is bound to on the
// device the player used last (ps2_input_bindings::promptDevice): a PlayStation-style gamepad keeps the PS2 pictures
// (by its own face labels), other gamepads get a round button with the letter in its colour (A green, B red, X blue,
// Y yellow) or a key cap with the button's name (LB, Start, ...), the keyboard a key cap with the key's name; the
// arrows stay pictures while they are on the d-pad or the arrow keys. Button names written as words (the controller
// layout pages' "R1 button", "left analog stick"; the Track Editor help's "L2 button", "START button") follow too: the
// bound button or key in its device's style (RB, LT, Start; R1, L2 on a PlayStation pad; a key cap), the d-pad and
// sticks kept in words on a gamepad that uses them. "Off": the key-hint line under a menu ("Back", "Select", "Move") is
// not drawn; elsewhere buttons are named neutrally, by place, in the language's pattern ("bottom button", "Touche du
// bas", "Untere-Taste", "bot\xb3n inferior"; "right trigger button", "the pause menu"), and the arrows go.
#pragma once

#include <cstdint>

namespace rc::prompts {

// A "}" code: '0'-'7' a picture of the PS2 key sheet (with the menus' picture function), 0xe0 and up a button name
// rewrite() found. True if this module draws it.
bool replaces(uint8_t code);
// Its advance in pixels (the PS2 picture's is 32).
int width(uint8_t code);
// Draws it at the pen (x, y): the line's top, as the PS2 picture is placed.
void draw(int x, int y, uint8_t code);
// A picture left out: the space after it goes too.
bool dropsSpace(uint8_t code);
// Before a string is measured or printed (not in PS2 mode): the button names it spells out ("R2 button", "Touche L1",
// "L2-Taste", "the START button", and the controller layout pages' "directional buttons", "left analog stick", ... in
// the four languages) become "}" codes in place (the string only gets shorter), drawn as the setting says; in PS2 mode
// a code draws the original words.
void rewrite(uint32_t address);
// AMenuDisplayOptions: the key-hint line (a string with pictures) is not drawn.
bool hidesHints(uint32_t text);

} // namespace rc::prompts

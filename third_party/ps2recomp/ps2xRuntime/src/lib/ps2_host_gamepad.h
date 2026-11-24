#pragma once
// RC: gamepads through SDL3's gamepad API (no window: SDL runs gamepad-only next to raylib's GLFW window), in up to
// four slots in connection order (a gamepad plugged back in takes the first free slot). Called from the window loop's
// thread (update, the reads) except setRumble (any thread).

#include <cstdint>
#include <string>

namespace ps2_host_gamepad
{
    constexpr int kSlots = 4;

    void update();                  // pumps SDL: hotplug, state, rumble, the test aids' virtual gamepads
    void shutdown();                // closes the gamepads (a Steam Controller's lizard mode back on); at the game's end
    bool present(int slot);
    bool button(int slot, int sdlButton);
    float axis(int slot, int sdlAxis); // sticks -1..1, triggers 0..1
    int count();
    std::string name(int slot);
    std::string buttonLabel(int sdlButton); // the first gamepad's label for a face button ("A", "Cross"), else positional
    int buttonLabelOf(int slot, int sdlButton); // SDL_GamepadButtonLabel of that gamepad's button (0 unknown)
    int type(int slot);                     // SDL_GamepadType (0 none)
    const char *buttonKey(int sdlButton);   // SDL's mapping-string name ("a", "dpup"), nullptr if none
    const char *axisKey(int sdlAxis);       // "leftx", "righttrigger"
    int buttonFromKey(const char *key);     // -1 if unknown
    int axisFromKey(const char *key);
    // The game's motors for a slot: the small one on or off, the large one 0-255.
    void setRumble(int slot, uint8_t small, uint8_t large);

    // Test aid (PS2X_SDL_VIRTUAL_PADS=<n>): n virtual SDL gamepads, pressed through PS2X_PAD_LIVE lines
    // "pad<n> <button or +/-axis> <hold ms> <gap ms>"; "pad<n> unplug" / "pad<n> plug" disconnect and reconnect it.
    // Returns false if the name is unknown.
    bool virtualSet(int index, const char *control, bool down);
}

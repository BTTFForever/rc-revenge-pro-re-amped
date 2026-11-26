**RC Revenge Pro: Re-Amped** is a native PC port of the PlayStation 2 game *RC Revenge Pro* (2000), built from a static recompilation of the PS2 executable with all of the game's own code decompiled into C++. It runs at high frame rates and resolutions, in widescreen, with modern controllers.

**New in v0.2.0: Windows.** This release adds a native Windows build. The game itself is the same as v0.1.0. On Linux, keep using the v0.1.0 AppImage.

## You need your own copy of the game

No game files are included. On the first start the launcher asks for a disc image of **your own** RC Revenge Pro disc:

- **USA** (SLUS-20153) or **Europe** (SLES-50077)
- `.iso`, or `.bin`/`.cue`. A `.bin` is converted to an `.iso` once, which needs about 700 MB of free space. An `.iso` is read where it is.

The launcher checks the image is one of those two releases and copies only the game's executable from it. It keeps that copy with your saves. After the first start it goes straight into the game.

## Requirements

- **Windows 10 or 11, 64-bit.**
- **A CPU with AVX2.** That's most CPUs from about 2013 on: Intel Haswell or AMD Ryzen and newer.
- **OpenGL 4.6 graphics**, which current NVIDIA, AMD and Intel drivers provide. On laptops with hybrid graphics, the dedicated GPU is requested automatically.

## Getting started

1. Download `RC_Revenge_Pro_Re-Amped-v0.2.0-windows-x86_64.zip` and unzip it anywhere.
2. Run `rc-launcher.exe`.
3. Choose your disc image when asked.

To switch to another disc image or release later, run `rc-launcher.exe --choose-disc`.

Windows may show a SmartScreen warning because the program is not signed (More info → Run anyway). Some antivirus programs flag new unsigned programs by heuristics; if yours quarantines a file, restore it and allow the folder.

## What's new compared to the PS2

**Display.** All of these are in **Game Options → Display**:
- Widescreen in 16:9, 16:10 or 21:9. The 3D view widens rather than stretching, and the HUD stays pinned to the screen corners.
- Internal resolution up to 6x, fullscreen or windowed.
- Races at up to 240 fps. Smooth motion draws cars and the camera between the game's physics steps, and game speed is unchanged.
- V-Sync, adaptive V-Sync and variable refresh (FreeSync/G-SYNC).
- HUD and menu scale.
- Button prompts that show PS2 buttons or your own controls, or none at all.

**Controls:**
- Keyboard and gamepads through SDL3, with hot-plugging and rumble.
- Analog sticks, and pressure-sensitive triggers for throttle and handbrake.
- Every control can be rebound for both players, from the game's own Controls menu and from the pause menu.

**Racing:**
- Two-player races with up to 8 cars, up from the PS2's 6.
- Vertical split screen: two players side by side.

**Saves:** memory cards are saved as files and kept between runs. Game saves and Track Editor tracks both work.

## Where things are kept

| What | Where |
|---|---|
| Saves (memory cards) | `%LOCALAPPDATA%\rc-revenge-pro\<usa or eu>\game\mc0`, `mc1` |
| Settings | `%APPDATA%\rc-revenge-pro\settings.ini` |
| Last run's log | `%LOCALAPPDATA%\rc-revenge-pro\<usa or eu>\logs\session.log` |

When reporting a problem, please include `session.log`. If the game crashes, the launcher shows where it is.

## Known issues

- The Windows build has been tested on one machine so far (Windows 11, NVIDIA RTX 2070 SUPER). Reports from AMD and Intel graphics are especially welcome.
- Gamepads and the Steam Controller have not been tried on Windows yet.

## Checksum

SHA-256 of the zip: see the attached `.sha256` file.

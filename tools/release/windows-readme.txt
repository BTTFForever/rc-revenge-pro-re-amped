RC Revenge Pro: Re-Amped (Windows)
==================================

A native PC port of the PlayStation 2 game RC Revenge Pro, built from the decompilation at
https://github.com/BTTFForever/rc-revenge-pro-re-amped

It contains no game data. You need your own copy of the game: a disc image of the USA (SLUS-20153) or
Europe (SLES-50077) release, as a .iso or a raw .bin/.cue dumped from your disc.

Playing
-------
1. Unzip this folder anywhere (keep the files together).
2. Start rc-launcher.exe. On the first run it asks for your disc image, checks it is the USA or Europe
   release, and copies only the game's executable from it. A .bin is converted to an .iso once.
3. Later runs go straight into the game. To use another image or release: rc-launcher.exe --choose-disc
   (or --version usa / --version eu once both are set up).

Settings are in the game's own menus: Game Options > Display (aspect ratio, widescreen, resolution, frame rate,
smooth motion, V-Sync), Game Options > Controls (keyboard and gamepads for each player). F11 toggles fullscreen.

Requirements: Windows 10 or 11 (64-bit), a CPU with AVX2 (about 2013 or later), a graphics card with OpenGL 4.6 (NVIDIA GTX 600 series or later, AMD and Intel with current drivers).

Files
-----
- Settings: %APPDATA%\rc-revenge-pro\ (settings.ini, launcher.ini)
- Saves (memory cards), logs, shader cache: %LOCALAPPDATA%\rc-revenge-pro\usa or \eu
  (game\mc0, game\mc1; logs\session.log, the previous run's in session.prev.log)
If the game stops unexpectedly, the launcher says so; please include logs\session.log when reporting it.

Some antivirus programs flag new, unsigned programs like this one by heuristics. If yours quarantines a file,
you can restore it and allow this folder.

Licence: GPL-3.0 (LICENSE.txt). RC Revenge Pro is a trademark of its owners; this project is not affiliated with them.

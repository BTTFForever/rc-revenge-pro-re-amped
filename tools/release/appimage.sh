#!/bin/bash
# Packs dist/rc-revenge-pro/ (tools/release/build.sh) into one AppImage: dist/RC_Revenge_Pro_Re-Amped-<version>-x86_64.AppImage
#   tools/release/appimage.sh [version]      version: default `git describe --tags --always`
#   APPDIR_ONLY=1 tools/release/appimage.sh  only lays out dist/AppDir (to try it: dist/AppDir/AppRun)
# Needs appimagetool: APPIMAGETOOL=<path> or appimagetool on PATH (https://github.com/AppImage/appimagetool; it runs
# without FUSE through APPIMAGE_EXTRACT_AND_RUN=1). rsvg-convert renders the icon (tools/release/icon.svg).
# Layout: AppRun starts usr/bin/rc-launcher; the games and lib/libSDL3.so.0 sit beside it (rpath $ORIGIN/lib).
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"
dist="$root/dist/rc-revenge-pro"
[ -x "$dist/rc-launcher" ] || { echo "no $dist/rc-launcher: run tools/release/build.sh first" >&2; exit 1; }
tool="${APPIMAGETOOL:-$(command -v appimagetool || true)}"
if [ "${APPDIR_ONLY:-0}" != 1 ] && { [ -z "$tool" ] || [ ! -x "$tool" ]; }; then
    echo "appimagetool not found: set APPIMAGETOOL=<path>" >&2; exit 1
fi
version="${1:-$(git describe --tags --always)}"

app="$root/dist/AppDir"
rm -rf "$app"; mkdir -p "$app/usr/bin" "$app/usr/share/applications" "$app/usr/share/icons/hicolor/256x256/apps"
cp -a "$dist"/. "$app/usr/bin/"
cat > "$app/AppRun" <<'EOF'
#!/bin/sh
# RC Revenge Pro: Re-Amped: the launcher asks for your disc image on the first run, then starts the game.
here="$(dirname "$(readlink -f "$0")")"
exec "$here/usr/bin/rc-launcher" "$@"
EOF
chmod +x "$app/AppRun"
cat > "$app/usr/share/applications/rc-revenge-pro.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=RC Revenge Pro: Re-Amped
Comment=PC port of RC Revenge Pro (PS2); plays from your own disc image
Exec=rc-launcher
Icon=rc-revenge-pro
Categories=Game;
Terminal=false
X-AppImage-Version=$version
EOF
cp "$app/usr/share/applications/rc-revenge-pro.desktop" "$app/"
rsvg-convert -w 256 -h 256 tools/release/icon.svg -o "$app/usr/share/icons/hicolor/256x256/apps/rc-revenge-pro.png"
cp "$app/usr/share/icons/hicolor/256x256/apps/rc-revenge-pro.png" "$app/rc-revenge-pro.png"
ln -s rc-revenge-pro.png "$app/.DirIcon"

[ "${APPDIR_ONLY:-0}" = 1 ] && { echo "laid out $app"; exit 0; }
out="$root/dist/RC_Revenge_Pro_Re-Amped-$version-x86_64.AppImage"
rm -f "$out"
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$tool" --no-appstream "$app" "$out"
ls -l "$out"
sha256sum "$out" | tee "$out.sha256"

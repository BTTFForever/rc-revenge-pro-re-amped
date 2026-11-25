#!/bin/bash
# Builds the release binaries in the container (tools/release/Containerfile: glibc 2.34, GCC 14, SDL3, static FFmpeg)
# and collects them in dist/rc-revenge-pro/:
#   rc-launcher, rc_revenge_pro_usa, rc_revenge_pro_eu (each version whose recompiled code is present), lib/libSDL3.so.0
# Needs podman, and tools/setup.sh run for each version to include (the recompiled code and generated headers come from
# your own disc; the release contains the compiled game code, never the disc's files).
#   tools/release/build.sh [usa] [eu]      default: every version set up
# Build folders: build-release-usa, build-release-eu, build-release-launcher (ignored by git, like every build*).
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"
image=rc-release
versions=("$@")
if [ ${#versions[@]} = 0 ]; then
    [ -d output ] && [ -d decomp/generated/usa ] && versions+=(usa)
    [ -d output_eu ] && [ -d decomp/generated/eu ] && versions+=(eu)
fi
[ ${#versions[@]} -gt 0 ] || { echo "no version set up: run tools/setup.sh with your disc first" >&2; exit 1; }

podman image exists "$image" || podman build -t "$image" -f tools/release/Containerfile tools/release
run() { podman run --rm --userns=keep-id -v "$root:/src" -w /src "$image" bash -c "$1"; }

run "cmake -S launcher -B build-release-launcher -G Ninja >/dev/null && ninja -C build-release-launcher"
for v in "${versions[@]}"; do
    case "$v" in usa) flags="" ;; eu) flags="-DRC_VERSION=eu" ;; *) echo "usa or eu, not $v" >&2; exit 2 ;; esac
    echo "== building $v"
    run "cmake -S . -B build-release-$v -G Ninja $flags >/dev/null && ninja -C build-release-$v rc_revenge_pro"
done

dist="$root/dist/rc-revenge-pro"
rm -rf "$dist"; mkdir -p "$dist/lib"
cp build-release-launcher/rc-launcher "$dist/"
for v in "${versions[@]}"; do cp "build-release-$v/rc_revenge_pro" "$dist/rc_revenge_pro_$v"; done
run "cp -L /opt/deps/lib64/libSDL3.so.0 dist/rc-revenge-pro/lib/ 2>/dev/null || cp -L /opt/deps/lib/libSDL3.so.0 dist/rc-revenge-pro/lib/
     for f in dist/rc-revenge-pro/rc-launcher dist/rc-revenge-pro/rc_revenge_pro_*; do patchelf --set-rpath '\$ORIGIN/lib' \"\$f\"; strip \"\$f\"; done"

echo "== $dist"
ls -la "$dist" "$dist/lib"
echo "== libraries the binaries need from the system"
for f in "$dist"/rc-launcher "$dist"/rc_revenge_pro_*; do objdump -p "$f" | awk '/NEEDED/ {print $2}'; done | sort -u | tr '\n' ' '; echo
echo "== newest glibc symbol version used"
for f in "$dist"/rc-launcher "$dist"/rc_revenge_pro_* "$dist"/lib/*; do objdump -T "$f" | grep -o 'GLIBC_[0-9.]*'; done | sort -uV | tail -1

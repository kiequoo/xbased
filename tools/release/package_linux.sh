#!/bin/bash
# xbased's Linux release: an AppImage of the executable, its fonts and mods,
# the docs and licenses, in dist/xbased-<version>-<arch>.AppImage.
#   tools/release/package_linux.sh [build dir] [out dir] [version]
# The arch is the host's (uname -m: x86_64 or aarch64), or override with $ARCH.
# appimagetool is fetched once into the out dir (or taken from $APPIMAGETOOL).
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${1:-$REPO/../build/linux}
OUT=${2:-$REPO/../dist}
VERSION=${3:-1.0.21}
ARCH=${ARCH:-$(uname -m)}
APPDIR=$OUT/xbased.AppDir
mkdir -p "$OUT"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/256x256/apps" \
         "$APPDIR/usr/share/doc/xbased"

# the program finds its fonts and mods beside itself
install -m 755 "$BUILD/xbased" "$APPDIR/usr/bin/xbased"
strip --strip-debug "$APPDIR/usr/bin/xbased" 2>/dev/null || true
cp -r "$REPO/native/app/ui/fonts" "$APPDIR/usr/bin/fonts"
cp -r "$REPO/mods" "$APPDIR/usr/bin/mods"
mv "$APPDIR/usr/bin/mods/README.md" "$APPDIR/usr/share/doc/xbased/MODS.md"   # how to add patches
README=$REPO/docs/xbased/README.md; [ -f "$README" ] || README=$REPO/README.md
cp "$README" "$REPO/COPYING" "$REPO/LICENSE" "$APPDIR/usr/share/doc/xbased/"
cp -r "$REPO/licenses" "$APPDIR/usr/share/doc/xbased/licenses"

cat > "$APPDIR/usr/share/applications/xbased.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=xbased
GenericName=Xbox Emulator
Comment=Original Xbox emulator for Windows and Linux, developed by crazyriddler
Exec=xbased %f
Icon=xbased
Terminal=false
Categories=Game;Emulator;
Keywords=xbox;emulator;
X-AppImage-Version=$VERSION
EOF
cp "$REPO/native/app/win/icon256.png" "$APPDIR/usr/share/icons/hicolor/256x256/apps/xbased.png"
ln -s usr/share/applications/xbased.desktop "$APPDIR/xbased.desktop"
ln -s usr/share/icons/hicolor/256x256/apps/xbased.png "$APPDIR/xbased.png"
ln -s xbased.png "$APPDIR/.DirIcon"

cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
# settings, saves and states live in ~/.local/share/xbased (the image is read-only)
HERE=$(dirname "$(readlink -f "$0")")
exec "$HERE/usr/bin/xbased" "$@"
EOF
chmod 755 "$APPDIR/AppRun"

TOOL=${APPIMAGETOOL:-$OUT/appimagetool-$ARCH.AppImage}
if [ ! -x "$TOOL" ]; then
  curl -fL -o "$TOOL" "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$ARCH.AppImage"
  chmod 755 "$TOOL"
fi
RUNTIME=${APPIMAGE_RUNTIME:-$OUT/runtime-$ARCH}
if [ ! -s "$RUNTIME" ]; then
  curl -fL -o "$RUNTIME" "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-$ARCH"
fi
IMG=$OUT/xbased-$VERSION-$ARCH.AppImage
rm -f "$IMG"
# extract-and-run: no FUSE needed where it's built
ARCH=$ARCH "$TOOL" --appimage-extract-and-run --no-appstream --runtime-file "$RUNTIME" "$APPDIR" "$IMG"
ls -la "$IMG"

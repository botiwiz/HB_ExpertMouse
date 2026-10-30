#!/bin/sh
# Builds HB_ExpertMouse-<arch>.AppImage. Needs: gcc, pkg-config, libgtk-3-dev, curl.
# GTK3 is not bundled: it ships with practically every Linux desktop, which keeps the AppImage tiny.
set -e
cd "$(dirname "$0")"
ARCH="${ARCH:-$(uname -m)}"
rm -rf build && mkdir -p build/AppDir/usr/bin
gcc -O2 -Wall -std=gnu11 -o build/AppDir/usr/bin/hb_expertmouse src/*.c $(pkg-config --cflags --libs gtk+-3.0)
strip build/AppDir/usr/bin/hb_expertmouse
cp appdir/AppRun appdir/hb_expertmouse.desktop appdir/hb_expertmouse.png build/AppDir/
ln -sf hb_expertmouse.png build/AppDir/.DirIcon

if command -v appimagetool >/dev/null; then
    TOOL=appimagetool
else
    TOOL=build/appimagetool
    curl -fsSL -o "$TOOL" "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$ARCH.AppImage"
    chmod +x "$TOOL"
fi
# extract-and-run: works without FUSE (containers / CI)
APPIMAGE_EXTRACT_AND_RUN=1 ARCH="$ARCH" "$TOOL" --no-appstream build/AppDir "HB_ExpertMouse-$ARCH.AppImage"
echo "Built $(pwd)/HB_ExpertMouse-$ARCH.AppImage"

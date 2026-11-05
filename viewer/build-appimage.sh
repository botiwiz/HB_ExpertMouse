#!/bin/sh
# Builds HB_Viewer-<arch>.AppImage.
# Needs: cmake, g++, extra-cmake-modules, Qt6 and KDE Frameworks 6 dev packages, curl.
# Qt6, KF6 and Dolphin are NOT bundled: the app embeds the system's Dolphin, so it runs on a
# Plasma 6 desktop (or anywhere with the 'dolphin' package installed) and stays tiny.
set -e
cd "$(dirname "$0")"
ARCH="${ARCH:-$(uname -m)}"
cmake -S . -B build/app -DCMAKE_BUILD_TYPE=Release
cmake --build build/app -j"$(nproc)"
strip build/app/hb_viewer
rm -rf build/AppDir && mkdir -p build/AppDir/usr/bin
cp build/app/hb_viewer build/AppDir/usr/bin/
cp appdir/AppRun appdir/hb_viewer.desktop appdir/hb_viewer.png build/AppDir/
chmod +x build/AppDir/AppRun
ln -sf hb_viewer.png build/AppDir/.DirIcon

if command -v appimagetool >/dev/null; then
    TOOL=appimagetool
else
    TOOL=build/appimagetool
    [ -x "$TOOL" ] || curl -fsSL -o "$TOOL" "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$ARCH.AppImage"
    chmod +x "$TOOL"
fi
# extract-and-run: works without FUSE (containers / CI)
APPIMAGE_EXTRACT_AND_RUN=1 ARCH="$ARCH" "$TOOL" --no-appstream build/AppDir "HB_Viewer-$ARCH.AppImage"
echo "Built $(pwd)/HB_Viewer-$ARCH.AppImage"

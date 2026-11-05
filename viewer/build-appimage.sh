#!/bin/sh
# Builds HB_Viewer-<arch>.AppImage. Needs: g++, pkg-config, libgtk-3-dev, curl.
# GTK3 / gdk-pixbuf are not bundled: they ship with practically every Linux desktop,
# which keeps the AppImage tiny. Extra formats (WebP, AVIF, HEIF, JXL, SVG) come from the
# system's gdk-pixbuf loaders when installed.
set -e
cd "$(dirname "$0")"
ARCH="${ARCH:-$(uname -m)}"
./build.sh
strip build/hb_viewer
rm -rf build/AppDir && mkdir -p build/AppDir/usr/bin
cp build/hb_viewer build/AppDir/usr/bin/
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

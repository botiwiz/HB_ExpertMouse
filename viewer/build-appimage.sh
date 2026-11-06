#!/bin/sh
# Builds HB_Viewer-<arch>.AppImage.
# Needs: cmake, g++, extra-cmake-modules, Qt6 and KDE Frameworks 6 dev packages, curl.
# Qt6, KF6 and Dolphin are NOT bundled: the app embeds the system's Dolphin, so it runs on a
# Plasma 6 desktop (or anywhere with the 'dolphin' package installed).
# Optional: F3D_SDK=<extracted f3d release> embeds the f3d 3D viewer; libf3d (with its VTK)
# and f3d's plugins are bundled in usr/lib.
set -e
cd "$(dirname "$0")"
ARCH="${ARCH:-$(uname -m)}"
cmake -S . -B build/app -DCMAKE_BUILD_TYPE=Release ${F3D_SDK:+-DCMAKE_PREFIX_PATH="$F3D_SDK"}
cmake --build build/app -j"$(nproc)"
strip build/app/hb_viewer
rm -rf build/AppDir && mkdir -p build/AppDir/usr/bin
cp build/app/hb_viewer build/AppDir/usr/bin/
if [ -n "$F3D_SDK" ]; then
    # bundle only the SDK libraries hb_viewer really links against (libf3d + its NEEDED
    # closure); the SDK's OSMesa/LLVM software renderer etc. are left out
    mkdir -p build/AppDir/usr/lib
    queue=$(readelf -d build/app/hb_viewer | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
    seen=" "
    while [ -n "$queue" ]; do
        set -- $queue; lib=$1; shift; queue="$*"
        case "$seen" in *" $lib "*) continue ;; esac
        seen="$seen$lib "
        src=$(find "$F3D_SDK/lib" -maxdepth 2 -name "$lib" | head -1)
        [ -n "$src" ] || continue                     # system library: not bundled
        case "$lib" in                                # every desktop has these; bundling could shadow
            libz.so.*|libssl.so.*|libcrypto.so.*|libsqlite3.so.*|libtiff.so.*) continue ;;   # the system's copy for Qt/KIO too
        esac
        cp -L "$src" "build/AppDir/usr/lib/$lib"
        queue="$queue $(readelf -d "$src" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')"
    done
    [ -d "$F3D_SDK/share/f3d" ] && mkdir -p build/AppDir/usr/share && cp -a "$F3D_SDK/share/f3d" build/AppDir/usr/share/
    ls -la build/AppDir/usr/lib
fi
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

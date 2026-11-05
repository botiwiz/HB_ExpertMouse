# HB_Viewer – progress log (for resuming work)

Goal: lightweight, fast **Linux** image viewer shipped as an **AppImage**, strong at huge PSD/PSB.
Reference app: "Imagine" (Windows freeware, user's zip) – used only as feature inspiration.

## Decisions
- C++17 + system GTK3 (not bundled, same as linux/ ExpertMouse app) -> tiny AppImage.
- Decoders: own PSD/PSB reader (merged composite only, skips layers, mmap, parallel row
  bands, box-filtered downscale over memory budget, fast row-sampled preview for >40 MP),
  vendored stb_image (HDR/PIC/PNM/TGA), GdkPixbuf for everything else, RAW = embedded JPEG.
- Image stored in 2048px cairo tiles + mipmaps -> no 32767px limit, fast zoom-out.
- Command registry (main.cpp `register_commands`) = single source for RMB menu, shortcuts,
  Favorites toolbar. Right-click a menu item toggles Favorite (menu stays open).
- UI is always dark: prefer-dark-theme + app CSS (apply_dark_theme in main.cpp) + symbolic icons.
- Settings: hb_viewer.ini next to the AppImage, fallback ~/.config/hb_viewer/.

## Files
- src/image.*   tiled image, pixel conversion, mipmaps
- src/psd.cpp   PSD/PSB decoder
- src/decode.*  format dispatch (pixbuf / stb / RAW preview), extension lists
- src/main.cpp  GTK UI, commands, favorites, loading jobs + cache/preload, --convert CLI
- build.sh, build-appimage.sh, appdir/, CI job `viewer-appimage` in .github/workflows/build.yml

## Phases
1. [x] Image viewer core – done, tested (PSD all modes pixel-exact vs reference, 40000x30000 PSB, GUI under Xvfb)
2. [ ] 3D mesh viewer via libf3d (https://github.com/f3d-app/f3d): embed in a GtkGLArea using
       f3d::engine::createExternal(); bundle prebuilt libf3d (+VTK) in the AppImage.
       Currently: 3D files are listed in the folder, "D" launches external f3d.

## Ideas / TODO
- Re-decode the visible region at full resolution when zooming into a downscaled huge PSD.
- PSD without "Maximize compatibility": composite layers ourselves (normal blend + opacity).
- EXIF panel, histogram, thumbnail browser strip, batch convert dialog, user-editable shortcuts.
- Note: in the build sandbox, first-touch memory is very slow (VM), so full-res decode timings
  there (~6-15 s for 1.2 GP) are not representative; decode work itself was 0.4 s.

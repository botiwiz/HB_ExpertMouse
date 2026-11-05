# HB_Viewer – progress log (for resuming work)

Goal: lightweight, fast Linux image viewer (AppImage), strong at huge PSD/PSB.
Reference app: "Imagine" (Windows freeware, user's zip) – used only as feature inspiration.

## Decisions
- C++17 + system GTK3 (not bundled, same as linux/ ExpertMouse app) -> tiny AppImage.
- Decoders: own PSD/PSB reader (merged composite only, skips layers, mmap, subsampled
  decode for huge files), vendored stb_image (PNG/JPG/BMP/TGA/GIF/HDR/PNM/PIC),
  GdkPixbuf fallback (TIFF, ICO, WebP/AVIF/HEIF/SVG if system loaders exist).
- Image stored in 2048px tiles with mipmaps -> no cairo 32767px limit, fast zoom-out.
- Command registry = single source for RMB menu, shortcuts and Favorites toolbar.
  Right-click an item in the RMB menu toggles it as a Favorite (toolbar button).
- Settings: hb_viewer.ini next to the AppImage, fallback ~/.config/hb_viewer/.

## Phases
1. [ ] Image viewer core (this phase)
2. [ ] 3D mesh viewer via libf3d (https://github.com/f3d-app/f3d) – embed in GtkGLArea
       using f3d::engine::createExternal; bundle prebuilt libf3d in AppImage.

## Status
- started; nothing built yet

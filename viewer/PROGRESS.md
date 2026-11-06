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

## Files (Qt6/KF6 version)
- src/image.*      tiled image (plain buffers, ARGB32 premultiplied), conversion, mipmaps
- src/psd.cpp      PSD/PSB decoder
- src/decode.*     format dispatch (QImageReader / stb / RAW preview), extension lists
- src/cli.*        --convert; src/cli_main.cpp = hb_convert (CLI-only build)
- src/imageview.*  viewer canvas (QPainter, tiles, mips, zoom/rotate, QMovie animation)
- src/mainwindow.* DolphinPart host, Places + KUrlNavigator, command registry, favorites, loading jobs
- src/columnbrowser.* cascade view (QColumnView + KDirModel, preview column)
- src/main.cpp     app, Breeze Dark, --selftest (HBV_SELFTEST_CASCADE=1 tests the cascade)
- tests/viewtest.cpp offscreen render test (CLI-only build)
- Local test build: cmake -S . -B build/cli -DHBV_CLI_ONLY=ON (only Qt6 base needed)
- build-appimage.sh, appdir/, CI job `viewer-appimage` (debian:trixie container) in .github/workflows/build.yml
- Old GTK version (incl. own browser/thumbnailer) is in git history before commit 14e754b.

## DONE (CI: builds on trixie, selftest "dolphinpart: LOADED", 102 commands): Qt6/KF6 + embedded Dolphin (user chose "Embed Dolphin", user runs Plasma 6)
- Browser = DolphinPart (KParts, plugin kf6/parts/dolphinpart) like Konqueror; host handles
  NavigationExtension::openUrlRequest: dir -> part->openUrl, image/3D -> our viewer, else KIO::OpenUrlJob.
  Context menu: NavigationExtension::popupMenu -> KFileItemActions + part edit actions + our commands.
  Places (KFilePlacesView) + KUrlNavigator breadcrumb. Dark: KColorSchemeManager "BreezeDark" + fallback palette.
- Core kept: image (tiles now plain buffers = QImage ARGB32_Premultiplied layout), psd.cpp, stb;
  gdk-pixbuf -> QImageReader (+ KImageFormats on Plasma). GTK browser/thumbs dropped (Dolphin does thumbnails).
- Build: CMake+ECM. Sandbox has no KF6 (mirrors blocked, no docker daemon) -> CI job in debian:trixie
  container builds + runs `--selftest` under xvfb. Locally only the CLI (-DHBV_CLI_ONLY=ON, Qt6 base) for PSD tests.
- AppImage contains only our binary; uses system Qt6/KF6/Dolphin (needs glibc/Qt/KF6 >= trixie's - inferred).

## Phases
1. [x] Image viewer core – done, tested (PSD all modes pixel-exact vs reference, 40000x30000 PSB, GUI under Xvfb)
2. [x] DONE (CI: cube renders, f3d 4.0.1) 3D mesh viewer via libf3d (https://github.com/f3d-app/f3d): embed in a GtkGLArea using
       f3d::engine::createExternal(); bundle prebuilt libf3d (+VTK) in the AppImage.
       Currently: 3D files are listed in the folder, "D" launches external f3d.

## Done: Browser mode (user request: like Imagine, but show ALL file types) - tested under Xvfb
- Plan: src/thumbs.* (thumbnail gen + freedesktop cache ~/.cache/thumbnails/large, worker pool,
  visible-first priority), src/browser.* (folder tree | path bar + GtkIconView of all files,
  folders first; non-images get content-type icons; cached thumbs from file managers reused).
- Tab toggles browser/viewer; double-click image -> viewer; Esc in viewer -> back to browser;
  other files -> default app. Commands act on browser selection (rename/trash/info/open with...).
- Start in browser when launched without a file (or with a folder).

## 3D (f3d) notes
- src/modelview.*: QOpenGLWidget + f3d::engine::createExternal(getProcAddress); Qt input forwarded via
  interactor trigger* API (pattern from f3d examples/libf3d/cpp/qt6). Built only if CMake finds the f3d SDK
  (HBV_HAVE_F3D); CI downloads f3d v4.0.1 release, build-appimage.sh bundles libf3d + NEEDED closure.
- f3d source for reference: git clone --depth 1 https://github.com/f3d-app/f3d (tag v4.0.1).

## DONE (CI: columns=6 for /usr/share/icons/hicolor): Cascade (Miller column) explorer view
- src/columnbrowser.*: QColumnView + KDirModel + KDirSortFilterProxyModel + KFilePreviewGenerator,
  preview column (our decoder for images incl. huge PSD, KIO::PreviewJob for other types) + file info.
- Toggle vs Dolphin view (command "cascade", Ctrl+4); shares Places + KUrlNavigator; same activation rules
  (images/3D -> viewer, other files -> default app); own copy/cut/paste/rename/trash/hidden keys.

## DONE (CI verified: Dolphin actions icons/compact/details/show_preview exist, modes switch): preview side panel + view-mode button
- Branch now: claude/epic-cerf-9rg62w (default; HB_Viewer was merged via PR #1 and deleted).
- src/previewpane.*: shared preview widget (our decoder for images, KIO thumbnails otherwise, file info);
  used by the cascade's preview column AND a toggleable right side panel (command "preview_panel", F12).
- Toolbar "view_mode" button (dropdown + click cycles): Details / Icons / Thumbnails / Cascade.
  Dolphin part actions used: "details", "icons", "show_preview" (names printed by --selftest to verify).

## DONE (CI green, run 42186458573): batch of user fixes
- Double open root cause: KParts NavigationExtension re-emits openUrlRequest as openUrlRequestDelayed
  (QTimer 0); we listened to both. Now only the delayed one. Self-test HBV_SELFTEST_OPEN counts handler calls (=1).
- F11 keeps toolbar/status bar; Ctrl+T and F11 stripped from Dolphin part actions so they always reach us.
- f3d: no setSize on press (unless low-res mode), frames drawn 1:1; HDRI ambient + SSAO on;
  cheatsheet on toolbar (favorites migration flag), shortcut ? / Ctrl+F1 in 3D view scope.

## DONE (CI green, run 42182804067): GPU-only rendering
- f3d: EGL then GLX only, no OSMesa/auto fallback; warn if GL_RENDERER is llvmpipe/softpipe/swrast.
- mpv: hwdec=auto (GPU decode), render API already GPU.
- ImageView -> QOpenGLWidget: lazy per-tile textures of the needed mip level, LRU VRAM budget,
  shader quads with view matrix, checkerboard in shader, nearest/linear per smooth setting.
- Qt widgets stay raster (compositor puts them on GPU); QtQuick rewrite not planned.

## DONE: video player + double-open fix + 3D low-res off
- Video black inside full app (standalone OK): mpv log "after creating texture: OpenGL error INVALID_ENUM",
  "render() not being called or stuck". NOT libf3d (exports no GL symbols). Real cause: mpv's rgba16f
  FBO renders black intermittently on llvmpipe (software GL); rgba8 works 5/5. Fix: detect software GL
  (GL_RENDERER llvmpipe/softpipe/swrast) -> fbo-format=rgba8. Also pass X11/WL display for hwdec interop.
- Double open: onOpenUrlRequest connected to openUrlRequest AND openUrlRequestDelayed -> dedupe same URL <1.5 s.
- Video: src/videoview.* = libmpv (dlopen libmpv.so.2 at runtime, headers from libmpv-dev at build) with
  render API into QOpenGLWidget + controls bar. Missing libmpv -> default app. Videos+audio join files_ list.
- 3D: half-res-while-moving now OFF by default (scaled the f3d overlays); toggle "f3d_lowres" in 3D menu.

## 3D speed (user: offscreen felt slower)
- Mouse moves coalesced (one triggerMousePosition per frame), half-res renders while dragging/scrolling,
  EGL -> GLX -> auto backend; status bar shows backend (OSMesa = software = slow). Needs user feedback.
  Fallback idea if still slow: QOpenGLWindow (default FBO, real double buffering) + createExternal.

## 3D ghosting v2 (user still saw ghosting/stale frames with the FBO-clear fix)
- ModelView is now a plain QWidget: f3d::engine::create(true) renders offscreen in its own GL context,
  frames read back via window.renderToImage() (bottom-up -> flipped), coalesced per event-loop pass.
- Cascade keys v2: Up/Down/Left/Right/Enter all driven from the view's current index (event filter on every
  column), focus follows the new column (deferred). CI sends real key events: Right,Down,Left,Up -> OK.

## 3D ghosting fix v1 (superseded)
- f3d external window composites over the bound FBO (BlitToCurrent + BlitToRenderFramebuffer), so stale frames
  showed through. ModelView::beginGL()/paintGL now clear the FBO before every f3d render. Needs user confirmation.

## Cascade arrow keys (user report) - DONE, CI verified
- Left = select parent folder (focus moves to its column); Right = enter folder, select first item
  (fetchMore + select when rows arrive / listing completes). ColumnBrowser::goLeft/goRight.

## Ideas / TODO
- Browser: preview pane, back/forward history, f3d thumbnails for 3D files (f3d --output), drag files out.
- Re-decode the visible region at full resolution when zooming into a downscaled huge PSD.
- PSD without "Maximize compatibility": composite layers ourselves (normal blend + opacity).
- EXIF panel, histogram, thumbnail browser strip, batch convert dialog, user-editable shortcuts.
- Note: in the build sandbox, first-touch memory is very slow (VM), so full-res decode timings
  there (~6-15 s for 1.2 GP) are not representative; decode work itself was 0.4 s.

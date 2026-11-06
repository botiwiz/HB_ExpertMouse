# HB Viewer (Linux AppImage, KDE Plasma 6)

A fast image viewer for **huge PSD/PSB files** and all common formats. Its file browser is **Dolphin itself**: the app embeds Dolphin's browser component, the same way Konqueror does.

## Requirements

- **KDE Plasma 6**, or any Linux system with **Dolphin** (KF6 version) installed.
- Qt 6, KDE Frameworks 6 and Dolphin are used from your system, not bundled. That way the browser is always your own Dolphin, with your settings and service menus. libf3d (with its own VTK) **is** bundled, for the 3D viewer.
- The CI build runs on Debian 13 "trixie" (Qt 6.8, KF6 6.13, glibc 2.41). It should run on distros at least that new, such as Kubuntu 25.04+, Fedora 42+, openSUSE Tumbleweed, Arch or KDE neon (based on Qt's backward compatibility; not tested on each).

## Get it

- Download the `HB_Viewer-AppImage` artifact from the repo's GitHub Actions "build" run.
- Or build it yourself with `viewer/build-appimage.sh`. On Debian or Ubuntu this needs `cmake g++ extra-cmake-modules qt6-base-dev libkf6parts-dev libkf6kio-dev libkf6xmlgui-dev libkf6colorscheme-dev libkf6configwidgets-dev`.

Then `chmod +x HB_Viewer-x86_64.AppImage` and run it, optionally with a file or folder: `./HB_Viewer-x86_64.AppImage picture.psd`.

## Formats

- **PSD / PSB**: its own decoder. It supports RGB, CMYK, Grayscale, Indexed, Bitmap, Lab and Duotone, at 1, 8, 16 or 32 bits, with RLE or raw data and transparency.
- **Everything Qt can read on your system**: PNG, JPEG, GIF/WebP/APNG (animated), BMP, TIFF, ICO, SVG, and through KDE's KImageFormats also AVIF, HEIF, JPEG XL, EXR, TGA, XCF, KRA/ORA, PCX and more.
- **HDR (Radiance), PIC and PNM** also go through stb_image as a fallback.
- **Camera RAW** (CR2, CR3, NEF, ARW, DNG, ORF, RAF, RW2…) shows the embedded full-size JPEG preview instantly.
- **3D models** open in an **embedded [f3d](https://f3d.app) viewer** (libf3d 4.0.1, bundled in the AppImage). Formats: OBJ, STL, PLY, glTF/GLB, FBX, 3DS, DAE, OFF, VTK formats, STEP/IGES/BREP, USD, Alembic, 3MF, Draco, gaussian splats and more.

## Why it's fast with huge PSDs

- It reads only Photoshop's merged image and skips the layer data, which is usually most of the file.
- The file is memory-mapped, and rows are decoded in parallel on all CPU cores.
- For big files, a quick low-resolution preview appears first, then the full image.
- Pictures are kept in 2048 px tiles with mipmaps. Zooming and panning stay smooth at any size, and there's no 32k pixel limit.
- Images larger than the memory budget are box-filtered down while decoding, and the status bar says so. The budget is about 1/3 of your RAM by default; you can change it with `max_megapixels` in the ini file.

> PSDs saved with **Maximize Compatibility** turned off may not contain a usable merged image, and may show blank. Photoshop turns this option on by default.

## Video and audio player (mpv)

Videos and audio files open inside the window, played by **libmpv**, the engine behind mpv, Haruna and Celluloid. It uses your system's libmpv (install `mpv` or `libmpv2` if it's missing). Without it, those files open in your default player instead.

- Controls: play/pause, seek bar, time, mute, volume and full screen.
- Keys: Space/K play or pause; ←/→ seek 5 s (Shift for 1 s); ↑/↓ seek 1 min; 9/0 volume; M mute; , and . step back/forward one frame; [ and ] change speed (Backspace resets it); L sets an A-B loop; F or Enter toggles full screen.
- Mouse: click to play or pause, double-click for full screen, wheel for volume.
- PgUp/PgDn go to the previous or next file in the folder (pictures, 3D and videos together). Esc goes back to the browser.

## GPU rendering

Pictures, 3D models and video are all drawn on the GPU:

- **Pictures:** the visible tiles are uploaded as textures, within a video-memory budget, and drawn by a shader.
- **3D:** f3d renders off-screen through EGL or GLX, with no software fallback.
- **Video:** mpv decodes on the GPU where your driver supports it (`hwdec=auto`) and shows the frames directly.

If your system only has a software OpenGL (no GPU driver), the status bar says so.

## 3D viewer (f3d)

3D files open inside the window, just like pictures. You can step through a folder with PgUp/PgDn or ←/→, and Esc goes back to the browser.

- **Mouse:** f3d's own controls. Left-drag rotates, middle-drag pans, and the wheel or right-drag zooms. A right-click without dragging opens the command menu.
- **Keyboard:** goes to f3d itself, so all of f3d's keys work (G grid, E edges, X axis, Space animation…). The "f3d keyboard cheatsheet" command lists them.
- **Commands** (menu "3D", all can be pinned as Favorites): Reset camera, Front/Top/Right/Isometric view, Toggle grid, Toggle edges, Toggle axis, Toggle orthographic camera, Play/pause animation, FPS counter, Model info, f3d cheatsheet. Ctrl+D opens the model in the external f3d app.

## View modes and preview panel

Two buttons at the start of the toolbar:

- **View mode**: click it to cycle through the modes, or use its arrow to pick one: **Details** (list with columns), **Icons**, **Thumbnails** (icons showing file previews) and **Cascade** (columns, below). Details, Icons and Thumbnails are Dolphin's own view modes.
- **Preview panel** (also **F12**): shows or hides a panel on the right of the browser. It previews the selected file in any view mode:
  - pictures use HB Viewer's fast decoder (huge PSDs included) and show dimensions and format;
  - other files show your system's thumbnail;
  - every file shows its name, type, size and date.

  You can drag the panel's edge to resize it, and the app remembers whether it's open.

## Cascade view (columns)

Press **Ctrl+4** or **F8**, or use "Cascade (column) view" in the Browser menu, to switch the browser to a cascade (Miller-column) explorer. Press it again to go back to the Dolphin view. The app remembers your choice.

- Each folder you select opens its contents in the next column to the right. The columns start at Home, or at `/` for folders outside Home. They work on network locations too.
- The **last column previews the selected file**:
  - for pictures, a large image made by HB Viewer's own decoder (instant even for multi-GB PSD/PSB files) plus dimensions and format;
  - for other files, your system's thumbnail (videos, PDFs, documents…);
  - name, type, size and date for everything.
- Opening works the same as in Dolphin view: pictures and 3D files open in the viewer, everything else in its default app.
- Keys inside the cascade: Ctrl+C / Ctrl+X / Ctrl+V copy, cut and paste; F2 renames; Delete moves to trash; Alt+Enter shows Properties; Ctrl+H shows hidden files.
- The right-click menu matches Dolphin's: View in HB Viewer, Open With, cut/copy/paste/rename/trash, your service menus, Properties and the HB Viewer commands. Files can be dragged out to other apps.
- Places and the breadcrumb bar are shared with the Dolphin view and follow the selected column.

## Browser = Dolphin

The app opens in the browser at your last folder. Launching it with a folder does the same; launching it with an image file opens the viewer directly.

- **The view is Dolphin's own**: its icon, compact and details views, thumbnails (KIO thumbnailers), sorting, hidden files, selection, drag and drop, cut, copy, paste, rename and trash, plus network locations (smb:/, sftp:/…).
- On the left is Dolphin's **Places** panel (F9 toggles it). On top is Dolphin's **breadcrumb location bar**. Ctrl+L edits the path; Alt+←/→/↑ go back, forward and up.
- **Opening a file:**
  - pictures and 3D files open in HB Viewer;
  - folders open in place;
  - **everything else behaves exactly like in Dolphin**: it opens in its default application, scripts ask whether to run or open, and so on.
- **Right-click** shows Dolphin's menu: "View in HB Viewer", Open With, Dolphin's edit actions, your service menus (Compress, Actions…), Properties, and an "HB Viewer commands" submenu.
- Tab switches between the browser and the viewer. Esc in the viewer comes back to the browser, at that file's folder.

## Commands, menu and Favorites

- **Right-click** in the viewer (or use "HB Viewer commands" in the browser menu, or the ☰ toolbar button) to see every command. They are grouped as File, Navigate, Browser, View, Image, 3D, Favorites, Help and **Dolphin**, which lists all of Dolphin's own actions, like view modes, sorting, hidden files and new folder. Each command shows its shortcut.
- **Right-click a command in that menu** to pin it to the Favorites toolbar, or to unpin it. A ★ marks pinned commands. The menu stays open, so you can pin several at once.
- **Right-click a toolbar button** to remove it, move it left or right, or open **Customize favorites…**. In that dialog you can tick commands and drag them into order.
- **F1** lists every command and shortcut.

Common keys:

| Key | Action |
| --- | --- |
| Tab | Switch between browser and viewer |
| ← → / PgUp PgDn / Space Backspace | Previous / next image in the folder (viewer) |
| Alt+← / Alt+→ / Alt+↑ or Backspace | Back / forward / parent folder (browser) |
| + − / Ctrl+wheel... | Viewer: + − or the wheel zoom (Ctrl+wheel navigates; **M** swaps them). Browser: Ctrl++ / Ctrl+− change the icon size |
| 0 / 1 / W | Fit / 100% / fit width |
| F11, F, Enter, double-click | Full screen |
| L / R / H / V | Rotate left / rotate right / flip horizontal / flip vertical |
| B / T / P | Cycle background / tiled view (seamless texture check) / smooth or pixel view |
| I / F2 / Delete | File info / rename / move to trash (in the browser these are Dolphin's own keys) |
| Ctrl+C / Ctrl+Shift+C | Copy image / copy path |
| E / Ctrl+Shift+O / D | Open with… / show in browser / open in the external f3d app (Ctrl+D inside the 3D view) |
| S | Slideshow |
| Esc | Leave full screen, stop the slideshow, or go back to the browser |
| Ctrl+Q | Quit |

The mouse:
- Drag to pan.
- Middle-click to fit the image.
- The back and forward buttons go to the previous and next image.
- The status bar shows the pixel position and color under the cursor.

## Look

The whole app, including the Dolphin view, uses KDE's **Breeze Dark** colour scheme, whatever your desktop theme is. Without Breeze Dark it falls back to a built-in dark palette.

## Settings

Settings are saved in `hb_viewer.ini` next to the AppImage, or in `~/.config/hb_viewer/` if that folder isn't writable. This includes the favorites, toggles, window size and `max_megapixels`.

## Command line

`hb_viewer --convert <input> <output.png|jpg|webp|tiff|…> [max_megapixels]` converts any supported file, for example to flatten a PSB to PNG.

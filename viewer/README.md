# HB Viewer (Linux AppImage, KDE Plasma 6)

A fast image viewer for **huge PSD/PSB files** and all common formats. Its file browser is **Dolphin itself**: the app embeds Dolphin's browser component, the same way Konqueror does.

## Requirements

- **KDE Plasma 6**, or any Linux system with **Dolphin** (KF6 version) installed.
- Qt 6, KDE Frameworks 6 and Dolphin are used from your system, not bundled. That keeps the AppImage tiny, and the browser is always your own Dolphin, with your settings and service menus.
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
- **3D models** (OBJ, STL, PLY, glTF/GLB, FBX, STEP, USD…): press **D** to open them in [f3d](https://f3d.app). Embedded 3D viewing is the next step.

## Why it's fast with huge PSDs

- It reads only Photoshop's merged image and skips the layer data, which is usually most of the file.
- The file is memory-mapped, and rows are decoded in parallel on all CPU cores.
- For big files, a quick low-resolution preview appears first, then the full image.
- Pictures are kept in 2048 px tiles with mipmaps. Zooming and panning stay smooth at any size, and there's no 32k pixel limit.
- Images larger than the memory budget are box-filtered down while decoding, and the status bar says so. The budget is about 1/3 of your RAM by default; you can change it with `max_megapixels` in the ini file.

> PSDs saved with **Maximize Compatibility** turned off may not contain a usable merged image, and may show blank. Photoshop turns this option on by default.

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
| E / Ctrl+Shift+O / D | Open with… / show in browser / open in f3d |
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

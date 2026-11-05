# HB Viewer (Linux AppImage)

A small, fast image viewer built to open **huge PSD/PSB files** quickly, plus all common formats.

## Get it

- Download the `HB_Viewer-AppImage` artifact from the repo's GitHub Actions "build" run.
- Or build it yourself with `viewer/build-appimage.sh`. This needs `g++`, `pkg-config` and `libgtk-3-dev`.

Then `chmod +x HB_Viewer-x86_64.AppImage` and run it, optionally with a file or folder: `./HB_Viewer-x86_64.AppImage picture.psd`.

## Formats

- **PSD / PSB**: its own decoder. It supports RGB, CMYK, Grayscale, Indexed, Bitmap, Lab and Duotone, at 1, 8, 16 or 32 bits, with RLE or raw data and transparency.
- **PNG, JPEG, GIF (animated), BMP, TIFF, ICO, TGA, PNM, XPM** and more are read through the system's gdk-pixbuf.
- **WebP, AVIF, HEIF, JPEG XL and SVG** open too, if the matching gdk-pixbuf loader is installed. Most desktops have them.
- **HDR (Radiance) and PIC** are read through stb_image.
- **Camera RAW** (CR2, CR3, NEF, ARW, DNG, ORF, RAF, RW2…) shows the embedded full-size JPEG preview.
- **3D models** (OBJ, STL, PLY, glTF/GLB, FBX, STEP, USD…) are listed while browsing. Press **D** to open one in [f3d](https://f3d.app). Embedded 3D viewing is the next step.

## Why it's fast with huge PSDs

- It reads only Photoshop's merged image and skips the layer data, which is usually most of the file.
- The file is memory-mapped, and rows are decoded in parallel on all CPU cores.
- For big files, a quick low-resolution preview appears first, then the full image.
- Pictures are kept in 2048 px tiles with mipmaps. Zooming and panning stay smooth at any size, and there's no 32k pixel limit.
- Images larger than the memory budget are box-filtered down while decoding, and the status bar says so. The budget is about 1/3 of your RAM by default; you can change it with `max_megapixels` in the ini file.

> PSDs saved with **Maximize Compatibility** turned off may not contain a usable merged image, and may show blank. Photoshop turns this option on by default.

## Browser

The app opens in the browser, like Imagine. Launching it with a folder does the same; launching it with an image file opens the viewer directly.

- On the left is a folder tree with Home, Drives and File system. On the right is a path bar (type a path and press Enter), an Up button and a thumbnail grid.
- The grid shows **every file** in the folder, folders first:
  - Images get real thumbnails, including huge PSD/PSB and camera RAW files.
  - Any other file shows the thumbnail your file manager already made for it (videos, PDFs…), or else its file-type icon.
- Thumbnails are made in the background, visible ones first. They are stored in the shared `~/.cache/thumbnails`, so they open instantly next time, and file managers can reuse them too.
- Double-click or press Enter:
  - on an image or 3D file, it opens in the viewer;
  - on a folder, it opens the folder;
  - on any other file, it opens in the file's default app.
- Tab switches between the browser and the viewer. Esc in the viewer goes back to the browser.
- Backspace or Alt+Up goes to the parent folder. +/− changes the thumbnail size. Ctrl+H shows hidden files.
- You can sort by name, date, size or type, and reverse the order. The folder refreshes automatically when files change.
- Rename, Move to trash (works on several selected files), File info, Open with, Copy path and Show in file manager act on the selected file.
- Right-click opens the same command menu, with the same Favorites.

## Commands, menu and Favorites

- **Right-click** anywhere to open a menu with every command, grouped as File, Navigate, View, Image, 3D, Favorites and Help. Each command shows its shortcut.
- **Right-click a command in that menu** to pin it to the Favorites toolbar, or to unpin it. A ★ marks pinned commands. The menu stays open, so you can pin several at once.
- **Right-click a toolbar button** to remove it, move it left or right, or open **Customize favorites…**. In that dialog you can tick commands and drag them into order.
- The **⋮** button at the right end of the toolbar opens the same full menu.
- **F1** lists every command and shortcut.

Common keys:

| Key | Action |
| --- | --- |
| Tab | Switch between browser and viewer |
| ← → / PgUp PgDn / Space Backspace | Previous / next image in the folder (viewer) |
| + − / mouse wheel | Zoom (Ctrl+wheel navigates instead; **M** swaps the two) |
| 0 / 1 / W | Fit / 100% / fit width |
| F11, F, Enter, double-click | Full screen |
| L / R / H / V | Rotate left / rotate right / flip horizontal / flip vertical |
| B / T / P | Cycle background / tiled view (seamless texture check) / smooth or pixel view |
| I / F2 / Delete | File info / rename / move to trash |
| Ctrl+C / Ctrl+Shift+C | Copy image / copy path |
| E / Ctrl+Shift+O / D | Open with… / show in file manager / open in f3d |
| S | Slideshow |
| Esc | Leave full screen, go back to the browser, or quit |

The mouse:
- Drag to pan.
- Middle-click to fit the image.
- The back and forward buttons go to the previous and next image.
- The status bar shows the pixel position and color under the cursor.

## Look

The whole UI is dark: toolbar, menus, dialogs and status bar. It doesn't follow your desktop theme. The only exception is the Open and Save file pickers: on some desktops the system draws these, and they follow the system theme.

## Settings

Settings are saved in `hb_viewer.ini` next to the AppImage, or in `~/.config/hb_viewer/` if that folder isn't writable. This includes the favorites, toggles, window size and `max_megapixels`.

## Command line

`hb_viewer --convert <input> <output.png|jpg|bmp|tiff> [max_megapixels]` converts any supported file, for example to flatten a PSB to PNG.

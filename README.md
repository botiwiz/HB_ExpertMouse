# HB Miscellaneous small apps and extensions

A tiny, portable button remapper for the Kensington Expert Mouse trackball (M01496) on Windows.

- **Only the Expert Mouse is remapped.** Your other mice and touchpads are unchanged.
- **Changes apply instantly.** There's no apply button and no restart.
- **Portable, with no dependencies.** It's one small `.exe` with no installer, driver or service. Settings are saved in `HB_ExpertMouse.ini` next to the exe.
- Built only with the .NET Framework 4.x that already ships with Windows 10/11.

## Get it

- **Build locally:** run `build.bat`. It uses Windows' built-in `csc.exe` and produces `HB_ExpertMouse.exe`.
- **Or** download the `HB_ExpertMouse` artifact from the repo's GitHub Actions "build" run.

## Use

1. Run `HB_ExpertMouse.exe`. The settings window opens and a tray icon appears.
2. Click **Identify...**, then press any button on the Expert Mouse. Its device ID (e.g. `VID_047D&PID_xxxx`) is filled in.
   - The default, `VID_047D`, matches any Kensington device.
3. Press each trackball button. The status line shows which Windows button it reports (Left/Right/Middle/Back/Forward).
4. Choose an action for each button:
   - Default
   - Disabled
   - Left, Right or Middle click
   - Back or Forward
   - Double click
   - Keystroke (click the box and press the combo, e.g. Ctrl+C)

Closing the window hides it to the tray. Right-click the tray icon to toggle **Enabled** or to **Exit**.

To start the app with Windows, put a shortcut to `HB_ExpertMouse.exe /hidden` in `shell:startup`.

## How it works

- A low-level mouse hook can block or replace clicks, but it can't tell which mouse sent them.
- Raw Input knows which device sent input, but Windows delivers it only after the hook has run, and not at all for clicks the hook blocked.
- So each click is credited to the device that most recently sent any input (moved, scrolled or clicked). In practice, that's the device your hand is on.
- Only buttons that you've remapped are affected. Every other button, and all pointer movement, passes through untouched.
- A button release always follows whatever was done for its press, so buttons can't get stuck.

## Limitations

- Suppose you move one device and then click a button on the *other* device without moving it at all. The first click may be credited to the device you moved. Moving the ball or mouse even slightly first avoids this. Making this exact would need a kernel driver.
- Elevated (admin) windows don't receive remapped clicks unless the app is also run as administrator. This is Windows UIPI behavior.

---

# Linux (AppImage)

On Linux, remapping is exact per device rather than guessed:
- The app takes exclusive control of **only** the Expert Mouse's input device (an evdev grab) and re-sends its input through a virtual device, with your remaps applied.
- Other mice are never opened.
- It works on both X11 and Wayland.

## Get it

- Download the `HB_ExpertMouse-AppImage` artifact from the repo's GitHub Actions "build" run.
- Or build it yourself with `linux/build-appimage.sh`. This needs `gcc`, `pkg-config` and `libgtk-3-dev`.

## Use

1. Make it executable and run it: `chmod +x HB_ExpertMouse-x86_64.AppImage`, then `./HB_ExpertMouse-x86_64.AppImage`.
2. The first time, the window may say it has no permission. Click **Grant access** and enter your password once. This installs a small udev rule that lets the logged-in user access Kensington devices and `/dev/uinput`. The app never runs as root.
3. Choose your Expert Mouse in the device list. The default, `047D`, matches any Kensington device.
4. Set an action for each button. Changes apply instantly.

More details:
- Settings are saved in `HB_ExpertMouse.ini` next to the AppImage.
- Closing the window leaves the background remapper running. Use **Stop remapper and quit** to stop it.
- **Start remapper when I log in** adds an autostart entry.
- Turning off **Remapping enabled** releases the trackball, so it works natively again.

## Dependencies

- The remapper itself needs only the standard C library.
- The settings window uses the system's GTK3, which is present on practically every desktop. GTK isn't bundled, which keeps the AppImage small.

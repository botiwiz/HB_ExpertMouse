# HB ExpertMouse

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
- Raw Input knows which device sent a click, but it can't block it.
- The app matches the two streams for each button event:
  - Clicks from the Expert Mouse are replaced with the chosen action.
  - Clicks from other devices pass through.
- Only buttons that you've remapped are inspected. Every other button, and all pointer movement, passes through untouched.
- If Windows delivers the hook event before the Raw Input event, the click is held for a moment:
  - A click from another mouse is re-sent unchanged within a few milliseconds.
  - If no device info arrives within 150 ms, the click is passed on unchanged, so nothing can get stuck.

## Limitations

- If you remap a button that your normal mouse also has (e.g. Left), clicks from that mouse may be re-sent by the app. Re-sent clicks are flagged as synthetic input, and some games with anti-cheat ignore synthetic input. Remapping Middle/Back/Forward avoids this in practice.
- Elevated (admin) windows don't receive remapped clicks unless the app is also run as administrator. This is Windows UIPI behavior.

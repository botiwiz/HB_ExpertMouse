using System;
using System.Collections.Generic;
using System.Windows.Forms;

namespace HBExpertMouse
{
    // Remaps button presses that come from ONE physical device (the Expert Mouse) only.
    //
    // A low-level hook can block clicks but can't tell which mouse sent them; Raw Input knows the
    // device but arrives only after the hook has run, and never arrives for clicks the hook blocked.
    // So each click is credited to the device that most recently sent input (moved, scrolled or
    // clicked) - in practice the one your hand is on. The matching button-up always follows the
    // decision made for its button-down, so buttons can't get stuck.
    // Only buttons that actually have a remap are ever touched; everything else passes straight through.
    public sealed class Remapper : NativeWindow, IDisposable
    {
        readonly Native.LowLevelMouseProc hookProc;
        IntPtr hook;
        readonly Dictionary<IntPtr, bool> isTargetCache = new Dictionary<IntPtr, bool>();
        readonly ButtonAction[] activeAction = new ButtonAction[5];
        IntPtr lastDevice;
        bool lastIsTarget;
        Config config;

        // Fired for button presses: (device path, button, is target).
        public event Action<string, MouseButton, bool> RawButtonPressed;
        // Diagnostic lines; only raised while LogEnabled is set (settings window open).
        public event Action<string> Log;
        public bool LogEnabled;

        public Remapper(Config cfg)
        {
            config = cfg;
            CreateHandle(new CreateParams { Caption = "HB_ExpertMouse_RawInput" });

            var rid = new[] { new Native.RAWINPUTDEVICE {
                usUsagePage = 0x01, usUsage = 0x02,   // generic desktop / mouse
                dwFlags = Native.RIDEV_INPUTSINK | Native.RIDEV_DEVNOTIFY, hwndTarget = Handle } };
            if (!Native.RegisterRawInputDevices(rid, 1, (uint)System.Runtime.InteropServices.Marshal.SizeOf(typeof(Native.RAWINPUTDEVICE))))
                throw new InvalidOperationException("RegisterRawInputDevices failed");

            hookProc = HookProc;
            hook = Native.SetWindowsHookEx(Native.WH_MOUSE_LL, hookProc, Native.GetModuleHandle(null), 0);
            if (hook == IntPtr.Zero)
                hook = Native.SetWindowsHookEx(Native.WH_MOUSE_LL, hookProc, Native.GetModuleHandle("user32.dll"), 0);
            if (hook == IntPtr.Zero)
                throw new InvalidOperationException("SetWindowsHookEx failed, error " + System.Runtime.InteropServices.Marshal.GetLastWin32Error());
        }

        // Apply new settings on the fly.
        public void SetConfig(Config cfg)
        {
            ReleaseActive();
            config = cfg;
            isTargetCache.Clear();
            lastIsTarget = lastDevice != IntPtr.Zero && IsTarget(lastDevice);
        }

        public void Dispose()
        {
            if (hook != IntPtr.Zero) { Native.UnhookWindowsHookEx(hook); hook = IntPtr.Zero; }
            ReleaseActive();
            DestroyHandle();
        }

        // ---------------- low-level hook ----------------

        IntPtr HookProc(int nCode, IntPtr wParam, IntPtr lParam)
        {
            if (nCode >= 0)
            {
                MouseButton button; bool down;
                var info = (Native.MSLLHOOKSTRUCT)System.Runtime.InteropServices.Marshal.PtrToStructure(lParam, typeof(Native.MSLLHOOKSTRUCT));
                // Skip only our own output. Clicks injected by other software (e.g. a vendor mouse
                // utility re-sending the trackball's clicks) are still remapped.
                if (info.dwExtraInfo != Native.OwnInputTag
                    && TryDecodeHook((int)wParam, info.mouseData, out button, out down))
                {
                    bool blocked = HandleButton(button, down);
                    if (LogEnabled && Log != null)
                        Log(string.Format("hook: {0} {1}{2}, last device {3} ({4}) -> {5}",
                            button, down ? "down" : "up",
                            (info.flags & Native.LLMHF_INJECTED) != 0 ? " [injected by other software]" : "",
                            lastDevice == IntPtr.Zero ? "none yet" : SettingsForm.ShortId(Native.GetDeviceName(lastDevice)),
                            lastIsTarget ? "Expert" : "other",
                            blocked ? "REMAPPED to " + (down ? config.Actions[(int)button].ToString() : "release") : "passed through"));
                    if (blocked) return (IntPtr)1;
                }
            }
            return Native.CallNextHookEx(hook, nCode, wParam, lParam);
        }

        // Returns true if the event was remapped and must be blocked.
        bool HandleButton(MouseButton button, bool down)
        {
            if (!down)
            {
                ButtonAction a = activeAction[(int)button];
                if (a == null) return false;     // its down wasn't remapped: pass the up through
                activeAction[(int)button] = null;
                Execute(a, false);
                return true;
            }
            if (!config.Enabled || !lastIsTarget) return false;
            ButtonAction action = config.Actions[(int)button];
            if (action.Kind == ActionKind.Default) return false;

            if (activeAction[(int)button] != null) Execute(activeAction[(int)button], false);
            activeAction[(int)button] = action;
            Execute(action, true);
            if (RawButtonPressed != null) RawButtonPressed(Native.GetDeviceName(lastDevice), button, true);
            return true;
        }

        static bool TryDecodeHook(int msg, int mouseData, out MouseButton b, out bool down)
        {
            b = MouseButton.Left; down = false;
            switch (msg)
            {
                case Native.WM_LBUTTONDOWN: down = true; b = MouseButton.Left; return true;
                case Native.WM_LBUTTONUP: b = MouseButton.Left; return true;
                case Native.WM_RBUTTONDOWN: down = true; b = MouseButton.Right; return true;
                case Native.WM_RBUTTONUP: b = MouseButton.Right; return true;
                case Native.WM_MBUTTONDOWN: down = true; b = MouseButton.Middle; return true;
                case Native.WM_MBUTTONUP: b = MouseButton.Middle; return true;
                case Native.WM_XBUTTONDOWN: down = true; goto case Native.WM_XBUTTONUP;
                case Native.WM_XBUTTONUP:
                    int x = (mouseData >> 16) & 0xFFFF;
                    if (x == 1) { b = MouseButton.X1; return true; }
                    if (x == 2) { b = MouseButton.X2; return true; }
                    return false;
            }
            return false;
        }

        // ---------------- raw input ----------------

        protected override void WndProc(ref Message m)
        {
            if (m.Msg == Native.WM_INPUT) OnRawInput(m.LParam);
            else if (m.Msg == Native.WM_INPUT_DEVICE_CHANGE) isTargetCache.Clear();
            base.WndProc(ref m);
        }

        static readonly int[] RawDownFlags = {
            Native.RI_MOUSE_LEFT_DOWN, Native.RI_MOUSE_RIGHT_DOWN, Native.RI_MOUSE_MIDDLE_DOWN,
            Native.RI_MOUSE_X1_DOWN, Native.RI_MOUSE_X2_DOWN };

        void OnRawInput(IntPtr hRawInput)
        {
            uint size = 0, hdr = (uint)Native.RawHeaderSize;
            Native.GetRawInputData(hRawInput, Native.RID_INPUT, IntPtr.Zero, ref size, hdr);
            if (size == 0) return;
            IntPtr buf = System.Runtime.InteropServices.Marshal.AllocHGlobal((int)size);
            try
            {
                if (Native.GetRawInputData(hRawInput, Native.RID_INPUT, buf, ref size, hdr) != size) return;
                if (System.Runtime.InteropServices.Marshal.ReadInt32(buf, 0) != Native.RIM_TYPEMOUSE) return;
                IntPtr device = System.Runtime.InteropServices.Marshal.ReadIntPtr(buf, 8);
                if (device == IntPtr.Zero) return;                  // injected input (e.g. our own SendInput)

                // Any input (movement, wheel, buttons) marks this device as the one in use.
                if (device != lastDevice)
                {
                    lastDevice = device;
                    lastIsTarget = IsTarget(device);
                }

                int flags = System.Runtime.InteropServices.Marshal.ReadInt16(buf, (int)hdr + 4) & 0xFFFF;
                for (int b = 0; b < 5; b++)
                {
                    if ((flags & RawDownFlags[b]) == 0) continue;
                    if (RawButtonPressed != null) RawButtonPressed(Native.GetDeviceName(device), (MouseButton)b, lastIsTarget);
                    if (LogEnabled && Log != null)
                        Log(string.Format("raw:  {0} down from {1} ({2})", (MouseButton)b,
                            SettingsForm.ShortId(Native.GetDeviceName(device)), lastIsTarget ? "Expert" : "other"));
                }
            }
            finally { System.Runtime.InteropServices.Marshal.FreeHGlobal(buf); }
        }

        bool IsTarget(IntPtr device)
        {
            bool t;
            if (isTargetCache.TryGetValue(device, out t)) return t;
            string match = (config.DeviceMatch ?? "").Trim();
            t = match.Length > 0 && Native.GetDeviceName(device).IndexOf(match, StringComparison.OrdinalIgnoreCase) >= 0;
            isTargetCache[device] = t;
            return t;
        }

        void ReleaseActive()
        {
            for (int b = 0; b < activeAction.Length; b++)
            {
                if (activeAction[b] == null) continue;
                Execute(activeAction[b], false);
                activeAction[b] = null;
            }
        }

        // ---------------- output ----------------

        static void Replay(MouseButton b, bool down)
        {
            switch (b)
            {
                case MouseButton.Left: Native.Send(Native.Mouse(down ? Native.MOUSEEVENTF_LEFTDOWN : Native.MOUSEEVENTF_LEFTUP, 0)); break;
                case MouseButton.Right: Native.Send(Native.Mouse(down ? Native.MOUSEEVENTF_RIGHTDOWN : Native.MOUSEEVENTF_RIGHTUP, 0)); break;
                case MouseButton.Middle: Native.Send(Native.Mouse(down ? Native.MOUSEEVENTF_MIDDLEDOWN : Native.MOUSEEVENTF_MIDDLEUP, 0)); break;
                case MouseButton.X1: Native.Send(Native.Mouse(down ? Native.MOUSEEVENTF_XDOWN : Native.MOUSEEVENTF_XUP, 1)); break;
                case MouseButton.X2: Native.Send(Native.Mouse(down ? Native.MOUSEEVENTF_XDOWN : Native.MOUSEEVENTF_XUP, 2)); break;
            }
        }

        static void Execute(ButtonAction a, bool down)
        {
            switch (a.Kind)
            {
                case ActionKind.Left: Replay(MouseButton.Left, down); break;
                case ActionKind.Right: Replay(MouseButton.Right, down); break;
                case ActionKind.Middle: Replay(MouseButton.Middle, down); break;
                case ActionKind.Back: Replay(MouseButton.X1, down); break;
                case ActionKind.Forward: Replay(MouseButton.X2, down); break;
                case ActionKind.DoubleClick:
                    if (down) Native.Send(
                        Native.Mouse(Native.MOUSEEVENTF_LEFTDOWN, 0), Native.Mouse(Native.MOUSEEVENTF_LEFTUP, 0),
                        Native.Mouse(Native.MOUSEEVENTF_LEFTDOWN, 0), Native.Mouse(Native.MOUSEEVENTF_LEFTUP, 0));
                    break;
                case ActionKind.Keys: SendKeys(a.Keys, down); break;
                // Disabled: swallow
            }
        }

        // Keys are held while the button is held (so e.g. Ctrl as a modifier button works).
        static void SendKeys(Keys k, bool down)
        {
            var vks = new List<ushort>();
            if ((k & Keys.Control) != 0) vks.Add(0x11);
            if ((k & Keys.Shift) != 0) vks.Add(0x10);
            if ((k & Keys.Alt) != 0) vks.Add(0x12);
            if (ButtonAction.IsWinFlagSet(k)) vks.Add(0x5B);
            Keys code = k & Keys.KeyCode;
            if (code != Keys.None) vks.Add((ushort)code);

            var inputs = new List<Native.INPUT>();
            if (down) foreach (ushort vk in vks) inputs.Add(Native.Key(vk, false));
            else for (int i = vks.Count - 1; i >= 0; i--) inputs.Add(Native.Key(vks[i], true));
            Native.Send(inputs.ToArray());
        }
    }
}

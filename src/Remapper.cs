using System;
using System.Collections.Generic;
using System.Windows.Forms;

namespace HBExpertMouse
{
    // Remaps button presses that come from ONE physical device (the Expert Mouse) only.
    //
    // A low-level hook can block clicks but can't tell which mouse sent them; Raw Input knows
    // the device but can't block. So both streams are matched up per button event:
    //  - Raw input arrives first  -> the hook decides instantly (no delay).
    //  - Hook event arrives first -> it is held briefly; when its raw input arrives it is either
    //    remapped (Expert Mouse) or re-sent unchanged (any other mouse). If no raw input shows up
    //    within HoldTimeoutMs it is re-sent unchanged, so other mice can never get stuck.
    // Only buttons that actually have a remap are ever touched; everything else passes straight through.
    public sealed class Remapper : NativeWindow, IDisposable
    {
        const int HoldTimeoutMs = 150;
        const int RawStaleMs = 500;

        struct Pending
        {
            public MouseButton Button;
            public bool Down;
            public bool FromTarget; // raw entries only
            public int Tick;
        }

        readonly Native.LowLevelMouseProc hookProc;
        IntPtr hook;
        readonly List<Pending> heldHook = new List<Pending>();
        readonly List<Pending> earlyRaw = new List<Pending>();
        readonly Dictionary<IntPtr, bool> isTargetCache = new Dictionary<IntPtr, bool>();
        readonly ButtonAction[] activeAction = new ButtonAction[5];
        readonly Timer timer;
        Config config;

        // Fired for every button press from any physical mouse: (device path, button, is target).
        public event Action<string, MouseButton, bool> RawButtonPressed;

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
            if (hook == IntPtr.Zero) throw new InvalidOperationException("SetWindowsHookEx failed");

            timer = new Timer { Interval = 25 };
            timer.Tick += delegate { FlushExpired(); };
        }

        // Apply new settings on the fly.
        public void SetConfig(Config cfg)
        {
            ReleaseActive();
            FlushAll();
            config = cfg;
            isTargetCache.Clear();
        }

        public void Dispose()
        {
            if (hook != IntPtr.Zero) { Native.UnhookWindowsHookEx(hook); hook = IntPtr.Zero; }
            ReleaseActive();
            FlushAll();
            timer.Dispose();
            DestroyHandle();
        }

        bool IsRemapped(MouseButton b)
        {
            return (config.Enabled && config.Actions[(int)b].Kind != ActionKind.Default) || activeAction[(int)b] != null;
        }

        // ---------------- low-level hook ----------------

        IntPtr HookProc(int nCode, IntPtr wParam, IntPtr lParam)
        {
            if (nCode >= 0)
            {
                MouseButton button; bool down;
                var info = (Native.MSLLHOOKSTRUCT)System.Runtime.InteropServices.Marshal.PtrToStructure(lParam, typeof(Native.MSLLHOOKSTRUCT));
                if ((info.flags & Native.LLMHF_INJECTED) == 0          // never touch synthetic input (ours or others')
                    && TryDecodeHook((int)wParam, info.mouseData, out button, out down)
                    && IsRemapped(button))
                {
                    if (HandleHookEvent(button, down)) return (IntPtr)1;
                }
            }
            return Native.CallNextHookEx(hook, nCode, wParam, lParam);
        }

        // Returns true if the event must be blocked.
        bool HandleHookEvent(MouseButton button, bool down)
        {
            int i = earlyRaw.FindIndex(p => p.Button == button && p.Down == down);
            if (i >= 0)
            {
                bool target = earlyRaw[i].FromTarget;
                earlyRaw.RemoveAt(i);
                if (!target) return false;
                Perform(button, down);
                return true;
            }
            heldHook.Add(new Pending { Button = button, Down = down, Tick = Environment.TickCount });
            timer.Enabled = true;
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

        static readonly int[,] RawFlags = {
            { Native.RI_MOUSE_LEFT_DOWN, Native.RI_MOUSE_LEFT_UP },
            { Native.RI_MOUSE_RIGHT_DOWN, Native.RI_MOUSE_RIGHT_UP },
            { Native.RI_MOUSE_MIDDLE_DOWN, Native.RI_MOUSE_MIDDLE_UP },
            { Native.RI_MOUSE_X1_DOWN, Native.RI_MOUSE_X1_UP },
            { Native.RI_MOUSE_X2_DOWN, Native.RI_MOUSE_X2_UP } };

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
                int flags = System.Runtime.InteropServices.Marshal.ReadInt16(buf, (int)hdr + 4) & 0xFFFF;
                if (flags == 0) return;                             // movement / wheel only

                bool target = IsTarget(device);
                for (int b = 0; b < 5; b++)
                {
                    // A single report can carry several transitions; handle downs before ups.
                    if ((flags & RawFlags[b, 0]) != 0) OnRawButton((MouseButton)b, true, target, device);
                    if ((flags & RawFlags[b, 1]) != 0) OnRawButton((MouseButton)b, false, target, device);
                }
            }
            finally { System.Runtime.InteropServices.Marshal.FreeHGlobal(buf); }
        }

        void OnRawButton(MouseButton button, bool down, bool target, IntPtr device)
        {
            if (down && RawButtonPressed != null) RawButtonPressed(Native.GetDeviceName(device), button, target);
            if (!IsRemapped(button)) return;

            int i = heldHook.FindIndex(p => p.Button == button && p.Down == down);
            if (i >= 0)
            {
                heldHook.RemoveAt(i);
                if (target) Perform(button, down);
                else Replay(button, down);
                return;
            }
            earlyRaw.Add(new Pending { Button = button, Down = down, FromTarget = target, Tick = Environment.TickCount });
            timer.Enabled = true;
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

        // ---------------- housekeeping ----------------

        void FlushExpired()
        {
            int now = Environment.TickCount;
            while (heldHook.Count > 0 && now - heldHook[0].Tick >= HoldTimeoutMs)
            {
                Pending p = heldHook[0];
                heldHook.RemoveAt(0);
                Replay(p.Button, p.Down);   // fail open: behave as if no remapper were running
            }
            earlyRaw.RemoveAll(p => now - p.Tick >= RawStaleMs);
            if (heldHook.Count == 0 && earlyRaw.Count == 0) timer.Enabled = false;
        }

        void FlushAll()
        {
            foreach (Pending p in heldHook) Replay(p.Button, p.Down);
            heldHook.Clear();
            earlyRaw.Clear();
            timer.Enabled = false;
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

        void Perform(MouseButton button, bool down)
        {
            if (down)
            {
                ButtonAction a = config.Enabled ? config.Actions[(int)button] : new ButtonAction(ActionKind.Default);
                if (a.Kind == ActionKind.Default) { Replay(button, true); a = new ButtonAction(ActionKind.Default); }
                else Execute(a, true);
                activeAction[(int)button] = a;
            }
            else
            {
                ButtonAction a = activeAction[(int)button];
                activeAction[(int)button] = null;
                if (a == null) return;  // up without a tracked down: swallow
                if (a.Kind == ActionKind.Default) Replay(button, false);
                else Execute(a, false);
            }
        }

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

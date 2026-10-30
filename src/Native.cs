using System;
using System.Runtime.InteropServices;
using System.Text;

namespace HBExpertMouse
{
    // Win32 interop. Kept to C# 5 so it compiles with the csc.exe bundled in Windows.
    internal static class Native
    {
        // ---- Low-level mouse hook ----
        public const int WH_MOUSE_LL = 14;
        public const int WM_LBUTTONDOWN = 0x0201, WM_LBUTTONUP = 0x0202;
        public const int WM_RBUTTONDOWN = 0x0204, WM_RBUTTONUP = 0x0205;
        public const int WM_MBUTTONDOWN = 0x0207, WM_MBUTTONUP = 0x0208;
        public const int WM_XBUTTONDOWN = 0x020B, WM_XBUTTONUP = 0x020C;
        public const int LLMHF_INJECTED = 0x01;
        // Tag on every event we send, so the hook can recognise (and ignore) its own output.
        public static readonly IntPtr OwnInputTag = (IntPtr)0x48424D45;

        public delegate IntPtr LowLevelMouseProc(int nCode, IntPtr wParam, IntPtr lParam);

        [StructLayout(LayoutKind.Sequential)]
        public struct MSLLHOOKSTRUCT
        {
            public int x, y;
            public int mouseData;
            public int flags;
            public int time;
            public IntPtr dwExtraInfo;
        }

        [DllImport("user32.dll", SetLastError = true)]
        public static extern IntPtr SetWindowsHookEx(int idHook, LowLevelMouseProc lpfn, IntPtr hMod, uint dwThreadId);
        [DllImport("user32.dll")]
        public static extern bool UnhookWindowsHookEx(IntPtr hhk);
        [DllImport("user32.dll")]
        public static extern IntPtr CallNextHookEx(IntPtr hhk, int nCode, IntPtr wParam, IntPtr lParam);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        public static extern IntPtr GetModuleHandle(string lpModuleName);

        // ---- Raw input ----
        public const int WM_INPUT = 0x00FF;
        public const int WM_INPUT_DEVICE_CHANGE = 0x00FE;
        public const uint RIDEV_INPUTSINK = 0x00000100;
        public const uint RIDEV_DEVNOTIFY = 0x00002000;
        public const uint RID_INPUT = 0x10000003;
        public const uint RIDI_DEVICENAME = 0x20000007;
        public const int RIM_TYPEMOUSE = 0;

        public const int RI_MOUSE_LEFT_DOWN = 0x0001, RI_MOUSE_LEFT_UP = 0x0002;
        public const int RI_MOUSE_RIGHT_DOWN = 0x0004, RI_MOUSE_RIGHT_UP = 0x0008;
        public const int RI_MOUSE_MIDDLE_DOWN = 0x0010, RI_MOUSE_MIDDLE_UP = 0x0020;
        public const int RI_MOUSE_X1_DOWN = 0x0040, RI_MOUSE_X1_UP = 0x0080;
        public const int RI_MOUSE_X2_DOWN = 0x0100, RI_MOUSE_X2_UP = 0x0200;

        [StructLayout(LayoutKind.Sequential)]
        public struct RAWINPUTDEVICE
        {
            public ushort usUsagePage;
            public ushort usUsage;
            public uint dwFlags;
            public IntPtr hwndTarget;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct RAWINPUTDEVICELIST
        {
            public IntPtr hDevice;
            public uint dwType;
        }

        [DllImport("user32.dll", SetLastError = true)]
        public static extern bool RegisterRawInputDevices(RAWINPUTDEVICE[] devices, uint count, uint cbSize);
        [DllImport("user32.dll")]
        public static extern uint GetRawInputData(IntPtr hRawInput, uint uiCommand, IntPtr pData, ref uint pcbSize, uint cbSizeHeader);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        public static extern uint GetRawInputDeviceInfo(IntPtr hDevice, uint uiCommand, StringBuilder pData, ref uint pcbSize);
        [DllImport("user32.dll")]
        public static extern uint GetRawInputDeviceList([Out] RAWINPUTDEVICELIST[] list, ref uint numDevices, uint cbSize);

        public static int RawHeaderSize { get { return 8 + 2 * IntPtr.Size; } }

        public static string GetDeviceName(IntPtr hDevice)
        {
            uint size = 0;
            GetRawInputDeviceInfo(hDevice, RIDI_DEVICENAME, null, ref size);
            if (size == 0) return "";
            var sb = new StringBuilder((int)size + 1);
            if ((int)GetRawInputDeviceInfo(hDevice, RIDI_DEVICENAME, sb, ref size) < 0) return "";
            return sb.ToString();
        }

        // ---- SendInput ----
        public const int INPUT_MOUSE = 0, INPUT_KEYBOARD = 1;
        public const int MOUSEEVENTF_LEFTDOWN = 0x0002, MOUSEEVENTF_LEFTUP = 0x0004;
        public const int MOUSEEVENTF_RIGHTDOWN = 0x0008, MOUSEEVENTF_RIGHTUP = 0x0010;
        public const int MOUSEEVENTF_MIDDLEDOWN = 0x0020, MOUSEEVENTF_MIDDLEUP = 0x0040;
        public const int MOUSEEVENTF_XDOWN = 0x0080, MOUSEEVENTF_XUP = 0x0100;
        public const int KEYEVENTF_EXTENDEDKEY = 0x0001, KEYEVENTF_KEYUP = 0x0002;

        [StructLayout(LayoutKind.Sequential)]
        public struct MOUSEINPUT
        {
            public int dx, dy;
            public int mouseData;
            public int dwFlags;
            public int time;
            public IntPtr dwExtraInfo;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct KEYBDINPUT
        {
            public ushort wVk;
            public ushort wScan;
            public int dwFlags;
            public int time;
            public IntPtr dwExtraInfo;
        }

        [StructLayout(LayoutKind.Explicit)]
        public struct InputUnion
        {
            [FieldOffset(0)] public MOUSEINPUT mi;
            [FieldOffset(0)] public KEYBDINPUT ki;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct INPUT
        {
            public int type;
            public InputUnion u;
        }

        [DllImport("user32.dll", SetLastError = true)]
        public static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);

        public static INPUT Mouse(int flags, int data)
        {
            var i = new INPUT { type = INPUT_MOUSE };
            i.u.mi.dwFlags = flags;
            i.u.mi.mouseData = data;
            i.u.mi.dwExtraInfo = OwnInputTag;
            return i;
        }

        public static INPUT Key(ushort vk, bool up)
        {
            var i = new INPUT { type = INPUT_KEYBOARD };
            i.u.ki.wVk = vk;
            i.u.ki.dwExtraInfo = OwnInputTag;
            i.u.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (IsExtendedKey(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
            return i;
        }

        public static void Send(params INPUT[] inputs)
        {
            if (inputs.Length > 0)
                SendInput((uint)inputs.Length, inputs, Marshal.SizeOf(typeof(INPUT)));
        }

        static bool IsExtendedKey(ushort vk)
        {
            switch (vk)
            {
                case 0x21: case 0x22: case 0x23: case 0x24:       // PgUp PgDn End Home
                case 0x25: case 0x26: case 0x27: case 0x28:       // arrows
                case 0x2D: case 0x2E:                             // Ins Del
                case 0x5B: case 0x5C: case 0x5D:                  // Win keys, Apps
                case 0x6F: case 0x90: case 0xA3: case 0xA5:       // Num/, NumLock, RCtrl, RAlt
                    return true;
            }
            return vk >= 0xA6 && vk <= 0xB7;                      // browser / media / volume keys
        }
    }
}

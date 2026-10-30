using System;
using System.Collections.Generic;
using System.IO;
using System.Windows.Forms;

namespace HBExpertMouse
{
    // Buttons as Windows reports them. The Expert Mouse's 4 physical buttons map onto some of these.
    public enum MouseButton { Left = 0, Right = 1, Middle = 2, X1 = 3, X2 = 4 }

    public enum ActionKind { Default, Disabled, Left, Right, Middle, Back, Forward, DoubleClick, Keys }

    // What a physical button does. Stored as text: "Default", "Left", ..., "Keys:Control, C".
    public sealed class ButtonAction
    {
        public ActionKind Kind;
        public Keys Keys;

        public ButtonAction(ActionKind kind) { Kind = kind; }
        public ButtonAction(Keys keys) { Kind = ActionKind.Keys; Keys = keys; }

        public static ButtonAction Parse(string s)
        {
            s = (s ?? "").Trim();
            if (s.StartsWith("Keys:", StringComparison.OrdinalIgnoreCase))
            {
                try { return new ButtonAction((Keys)Enum.Parse(typeof(Keys), s.Substring(5), true)); }
                catch (ArgumentException) { return new ButtonAction(ActionKind.Default); }
            }
            ActionKind k;
            if (Enum.TryParse(s, true, out k) && k != ActionKind.Keys) return new ButtonAction(k);
            return new ButtonAction(ActionKind.Default);
        }

        public string Serialize()
        {
            return Kind == ActionKind.Keys ? "Keys:" + Keys : Kind.ToString();
        }

        public override string ToString()
        {
            switch (Kind)
            {
                case ActionKind.Default: return "Default (unchanged)";
                case ActionKind.Disabled: return "Disabled";
                case ActionKind.Left: return "Left click";
                case ActionKind.Right: return "Right click";
                case ActionKind.Middle: return "Middle click";
                case ActionKind.Back: return "Back";
                case ActionKind.Forward: return "Forward";
                case ActionKind.DoubleClick: return "Double click";
                default: return "Keys: " + KeysText(Keys);
            }
        }

        public static string KeysText(Keys k)
        {
            var parts = new List<string>();
            if ((k & Keys.Control) != 0) parts.Add("Ctrl");
            if ((k & Keys.Shift) != 0) parts.Add("Shift");
            if ((k & Keys.Alt) != 0) parts.Add("Alt");
            if (IsWinFlagSet(k)) parts.Add("Win");
            Keys code = k & Keys.KeyCode;
            if (code != Keys.None) parts.Add(code.ToString());
            return string.Join("+", parts);
        }

        // Keys has no Win modifier, so Win+X combos use an otherwise unused bit.
        public const Keys WinFlag = (Keys)0x00080000;
        public static bool IsWinFlagSet(Keys k) { return (k & WinFlag) != 0; }
    }

    public sealed class Config
    {
        public bool Enabled = true;
        // Substring of the raw input device path, e.g. "VID_047D&PID_1020". VID_047D = Kensington.
        public string DeviceMatch = "VID_047D";
        public readonly ButtonAction[] Actions = new ButtonAction[5];

        public Config()
        {
            for (int i = 0; i < Actions.Length; i++) Actions[i] = new ButtonAction(ActionKind.Default);
        }

        public static string FilePath
        {
            get
            {
                // Portable: settings live next to the exe.
                return Path.Combine(Path.GetDirectoryName(Application.ExecutablePath), "HB_ExpertMouse.ini");
            }
        }

        public static Config Load()
        {
            var c = new Config();
            try
            {
                if (!File.Exists(FilePath)) return c;
                foreach (string raw in File.ReadAllLines(FilePath))
                {
                    int eq = raw.IndexOf('=');
                    if (eq < 0 || raw.TrimStart().StartsWith(";")) continue;
                    string key = raw.Substring(0, eq).Trim(), val = raw.Substring(eq + 1).Trim();
                    MouseButton b;
                    if (key.Equals("Enabled", StringComparison.OrdinalIgnoreCase)) c.Enabled = val != "0" && !val.Equals("false", StringComparison.OrdinalIgnoreCase);
                    else if (key.Equals("Device", StringComparison.OrdinalIgnoreCase)) c.DeviceMatch = val;
                    else if (Enum.TryParse(key, true, out b) && Enum.IsDefined(typeof(MouseButton), b)) c.Actions[(int)b] = ButtonAction.Parse(val);
                }
            }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
            return c;
        }

        public void Save()
        {
            try
            {
                var lines = new List<string>();
                lines.Add("; HB ExpertMouse settings");
                lines.Add("Enabled=" + (Enabled ? "1" : "0"));
                lines.Add("Device=" + DeviceMatch);
                foreach (MouseButton b in Enum.GetValues(typeof(MouseButton)))
                    lines.Add(b + "=" + Actions[(int)b].Serialize());
                File.WriteAllLines(FilePath, lines.ToArray());
            }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }
    }
}

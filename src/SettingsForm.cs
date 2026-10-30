using System;
using System.Collections.Generic;
using System.Drawing;
using System.Text.RegularExpressions;
using System.Windows.Forms;

namespace HBExpertMouse
{
    // Settings window. Every change is applied and saved immediately.
    public sealed class SettingsForm : Form
    {
        static readonly ActionKind[] Choices = {
            ActionKind.Default, ActionKind.Disabled, ActionKind.Left, ActionKind.Right, ActionKind.Middle,
            ActionKind.Back, ActionKind.Forward, ActionKind.DoubleClick, ActionKind.Keys };

        static readonly string[] ButtonNames = {
            "Left button", "Right button", "Middle button", "Button 4 (Back)", "Button 5 (Forward)" };

        readonly Config config;
        readonly Action onChanged;
        readonly CheckBox enabledBox = new CheckBox { Text = "Remapping enabled", AutoSize = true };
        readonly ComboBox deviceBox = new ComboBox { Width = 230 };
        readonly Button identifyBtn = new Button { Text = "Identify...", AutoSize = true };
        readonly Label status = new Label { AutoSize = true, ForeColor = SystemColors.GrayText, Text = "Press a button on any mouse to see it here." };
        readonly ComboBox[] actionBoxes = new ComboBox[5];
        readonly TextBox[] keyBoxes = new TextBox[5];
        bool identifying, loading;

        public SettingsForm(Config cfg, Action changed)
        {
            config = cfg;
            onChanged = changed;
            Text = "HB ExpertMouse";
            Icon = Program.AppIcon;
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            AutoSize = true;
            AutoSizeMode = AutoSizeMode.GrowAndShrink;
            Padding = new Padding(10);
            StartPosition = FormStartPosition.CenterScreen;

            var grid = new TableLayoutPanel { ColumnCount = 3, AutoSize = true, Dock = DockStyle.Fill };
            grid.Controls.Add(enabledBox, 0, 0);
            grid.SetColumnSpan(enabledBox, 3);

            grid.Controls.Add(new Label { Text = "Device ID:", AutoSize = true, Anchor = AnchorStyles.Left }, 0, 1);
            grid.Controls.Add(deviceBox, 1, 1);
            grid.Controls.Add(identifyBtn, 2, 1);
            grid.Controls.Add(status, 0, 2);
            grid.SetColumnSpan(status, 3);

            for (int i = 0; i < 5; i++)
            {
                int b = i;
                actionBoxes[i] = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 230 };
                foreach (ActionKind k in Choices)
                    actionBoxes[i].Items.Add(k == ActionKind.Keys ? "Keystroke..." : new ButtonAction(k).ToString());
                keyBoxes[i] = new TextBox { Width = 140, ReadOnly = true, BackColor = SystemColors.Window, ShortcutsEnabled = false };
                keyBoxes[i].KeyDown += (s, e) => CaptureKeys(b, e);
                keyBoxes[i].PreviewKeyDown += (s, e) => e.IsInputKey = true;   // let Tab/arrows be captured too
                actionBoxes[i].SelectedIndexChanged += delegate { OnActionChanged(b); };

                grid.Controls.Add(new Label { Text = ButtonNames[i], AutoSize = true, Anchor = AnchorStyles.Left }, 0, 3 + i);
                grid.Controls.Add(actionBoxes[i], 1, 3 + i);
                grid.Controls.Add(keyBoxes[i], 2, 3 + i);
            }

            var hint = new Label
            {
                AutoSize = true,
                MaximumSize = new Size(520, 0),
                ForeColor = SystemColors.GrayText,
                Text = "Only presses from the device whose ID contains the text above are remapped. " +
                       "Click Identify, then press a button on the Expert Mouse to fill it in. " +
                       "For a keystroke, click the box on the right and press the combination."
            };
            grid.Controls.Add(hint, 0, 8);
            grid.SetColumnSpan(hint, 3);
            Controls.Add(grid);

            enabledBox.CheckedChanged += delegate { if (!loading) { config.Enabled = enabledBox.Checked; onChanged(); } };
            deviceBox.TextChanged += delegate { if (!loading) { config.DeviceMatch = deviceBox.Text.Trim(); onChanged(); } };
            identifyBtn.Click += delegate
            {
                identifying = true;
                status.ForeColor = Color.DarkBlue;
                status.Text = "Press any button on the Expert Mouse now...";
            };

            LoadValues();
        }

        void LoadValues()
        {
            loading = true;
            enabledBox.Checked = config.Enabled;
            deviceBox.Items.Clear();
            foreach (string id in ListMouseIds()) deviceBox.Items.Add(id);
            deviceBox.Text = config.DeviceMatch;
            for (int i = 0; i < 5; i++)
            {
                ButtonAction a = config.Actions[i];
                actionBoxes[i].SelectedIndex = Array.IndexOf(Choices, a.Kind);
                keyBoxes[i].Visible = a.Kind == ActionKind.Keys;
                keyBoxes[i].Text = a.Kind == ActionKind.Keys ? ButtonAction.KeysText(a.Keys) : "";
            }
            loading = false;
        }

        public void RefreshFromConfig() { LoadValues(); }

        void OnActionChanged(int b)
        {
            if (loading) return;
            ActionKind k = Choices[actionBoxes[b].SelectedIndex];
            keyBoxes[b].Visible = k == ActionKind.Keys;
            if (k == ActionKind.Keys)
            {
                // Remap only once a combination has been recorded.
                keyBoxes[b].Text = "click, then press keys";
                keyBoxes[b].Focus();
                return;
            }
            config.Actions[b] = new ButtonAction(k);
            onChanged();
        }

        void CaptureKeys(int b, KeyEventArgs e)
        {
            e.Handled = e.SuppressKeyPress = true;
            Keys k = e.KeyData;
            Keys code = k & Keys.KeyCode;
            if (code == Keys.ControlKey || code == Keys.ShiftKey || code == Keys.Menu) k &= ~Keys.KeyCode;
            if (code == Keys.LWin || code == Keys.RWin) k = (k & ~Keys.KeyCode) | ButtonAction.WinFlag;
            else if ((Control.ModifierKeys & (Keys.LWin | Keys.RWin)) != 0 || IsWinDown()) k |= ButtonAction.WinFlag;
            if (k == Keys.None) return;
            config.Actions[b] = new ButtonAction(k);
            keyBoxes[b].Text = ButtonAction.KeysText(k);
            onChanged();
        }

        static bool IsWinDown()
        {
            return (GetKeyState(0x5B) & 0x8000) != 0 || (GetKeyState(0x5C) & 0x8000) != 0;
        }

        [System.Runtime.InteropServices.DllImport("user32.dll")]
        static extern short GetKeyState(int vk);

        // Called by the remapper for every physical button press.
        public void ShowPress(string devicePath, MouseButton button, bool isTarget)
        {
            string id = ShortId(devicePath);
            if (identifying && id.Length > 0)
            {
                identifying = false;
                deviceBox.Text = id;  // triggers TextChanged -> saved
            }
            status.ForeColor = isTarget ? Color.DarkGreen : SystemColors.GrayText;
            status.Text = string.Format("Last press: {0} from {1} ({2})", ButtonNames[(int)button],
                id.Length > 0 ? id : "unknown device", isTarget ? "Expert Mouse - remapped" : "other mouse - untouched");
        }

        // "\\?\HID#VID_047D&PID_1020&MI_00#..." -> "VID_047D&PID_1020"
        public static string ShortId(string path)
        {
            Match m = Regex.Match(path ?? "", @"VID_[0-9A-F]{4}&PID_[0-9A-F]{4}", RegexOptions.IgnoreCase);
            if (m.Success) return m.Value.ToUpperInvariant();
            m = Regex.Match(path ?? "", @"VID&[0-9A-F]+_PID&[0-9A-F]{4}", RegexOptions.IgnoreCase); // Bluetooth LE style
            return m.Success ? m.Value.ToUpperInvariant() : "";
        }

        static List<string> ListMouseIds()
        {
            var ids = new List<string>();
            uint n = 0, sz = (uint)System.Runtime.InteropServices.Marshal.SizeOf(typeof(Native.RAWINPUTDEVICELIST));
            Native.GetRawInputDeviceList(null, ref n, sz);
            if (n == 0) return ids;
            var list = new Native.RAWINPUTDEVICELIST[n];
            if ((int)Native.GetRawInputDeviceList(list, ref n, sz) < 0) return ids;
            foreach (var d in list)
            {
                if (d.dwType != Native.RIM_TYPEMOUSE) continue;
                string id = ShortId(Native.GetDeviceName(d.hDevice));
                if (id.Length > 0 && !ids.Contains(id)) ids.Add(id);
            }
            return ids;
        }

        protected override void OnFormClosing(FormClosingEventArgs e)
        {
            // Closing the window just hides it; the app keeps running in the tray.
            if (e.CloseReason == CloseReason.UserClosing) { e.Cancel = true; Hide(); }
            base.OnFormClosing(e);
        }
    }
}

using System;
using System.Drawing;
using System.Threading;
using System.Windows.Forms;

namespace HBExpertMouse
{
    static class Program
    {
        public static Icon AppIcon;

        [STAThread]
        static void Main(string[] args)
        {
            bool created;
            using (var mutex = new Mutex(true, "HB_ExpertMouse_SingleInstance", out created))
            {
                if (!created)
                {
                    MessageBox.Show("HB ExpertMouse is already running (see the tray).", "HB ExpertMouse");
                    return;
                }
                Application.EnableVisualStyles();
                Application.SetCompatibleTextRenderingDefault(false);
                AppIcon = MakeIcon();
                Application.Run(new TrayApp(Array.IndexOf(args, "/hidden") < 0));
                GC.KeepAlive(mutex);
            }
        }

        // The icon embedded in the exe (build.bat /win32icon), so no separate resource files are needed.
        static Icon MakeIcon()
        {
            try { return Icon.ExtractAssociatedIcon(Application.ExecutablePath) ?? SystemIcons.Application; }
            catch (Exception) { return SystemIcons.Application; }
        }
    }

    sealed class TrayApp : ApplicationContext
    {
        readonly Config config = Config.Load();
        readonly NotifyIcon tray;
        readonly SettingsForm form;
        readonly Remapper remapper;
        readonly ToolStripMenuItem enabledItem;

        public TrayApp(bool showSettings)
        {
            form = new SettingsForm(config, Changed);
            remapper = Remapper.Start(config);
            // These fire on the remapper thread: hand them to the UI asynchronously so it never waits.
            remapper.RawButtonPressed += (path, button, target) =>
                ToForm(() => form.ShowPress(path, button, target));
            remapper.Log += line => ToForm(() => form.AddLog(line));
            form.VisibleChanged += delegate { remapper.LogEnabled = form.Visible; };

            var menu = new ContextMenuStrip();
            menu.Items.Add("Settings", null, delegate { ShowSettings(); });
            enabledItem = new ToolStripMenuItem("Enabled", null, delegate
            {
                config.Enabled = !config.Enabled;
                form.RefreshFromConfig();
                Changed();
            });
            menu.Items.Add(enabledItem);
            menu.Items.Add(new ToolStripSeparator());
            menu.Items.Add("Exit", null, delegate { ExitThread(); });

            tray = new NotifyIcon { Icon = Program.AppIcon, ContextMenuStrip = menu, Visible = true };
            tray.DoubleClick += delegate { ShowSettings(); };
            UpdateTray();

            if (showSettings) ShowSettings();
        }

        void ToForm(Action a)
        {
            if (!remapper.LogEnabled) return;   // window hidden: nothing to show
            try { form.BeginInvoke(a); }
            catch (InvalidOperationException) { }
        }

        void Changed()
        {
            remapper.ApplyConfig(config);
            config.Save();
            UpdateTray();
        }

        void UpdateTray()
        {
            enabledItem.Checked = config.Enabled;
            tray.Text = "HB ExpertMouse - " + (config.Enabled ? "on" : "off");
        }

        void ShowSettings()
        {
            form.Show();
            form.WindowState = FormWindowState.Normal;
            form.Activate();
        }

        protected override void ExitThreadCore()
        {
            remapper.Shutdown();
            tray.Visible = false;
            tray.Dispose();
            form.Dispose();
            base.ExitThreadCore();
        }
    }
}

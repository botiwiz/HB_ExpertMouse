/* Settings window. Every change is saved and applied immediately (the daemon is told to reload). */
#include "common.h"

#include <gtk/gtk.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static Config cfg;
static int sock = -1;
static guint sock_watch;
static GtkWidget *status_label, *grant_btn, *enabled_chk, *device_combo, *press_label, *autostart_chk;
static GtkWidget *action_combo[NBUTTONS], *keys_btn[NBUTTONS];
static int capturing = -1;      /* button whose keystroke is being recorded */
static int loading;

/* ---------------- daemon connection ---------------- */

static void start_daemon(void)
{
    const char *appimage = getenv("APPIMAGE");
    char exe[4096];
    /* Inside an AppImage, start a fresh instance: this one's mount disappears when the GUI exits. */
    if (appimage && *appimage) snprintf(exe, sizeof exe, "%s", appimage);
    else {
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n <= 0) return;
        exe[n] = 0;
    }
    char *argv[] = { exe, "--daemon", NULL };
    g_spawn_async(NULL, argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL);
}

static void send_cmd(const char *cmd)
{
    if (sock >= 0) send(sock, cmd, strlen(cmd), MSG_NOSIGNAL | MSG_DONTWAIT);
}

static void handle_line(char *line)
{
    if (strncmp(line, "status ", 7) == 0) {
        char *s = line + 7;
        int perm = strncmp(s, "perm:", 5) == 0;
        const char *text = strchr(s, ':') ? strchr(s, ':') + 2 : s;
        char *markup = g_markup_printf_escaped("<b>%s</b> %s",
            strncmp(s, "ok:", 3) == 0 ? "Active:" : strncmp(s, "disabled:", 9) == 0 ? "Off:" : "Not active:", text);
        gtk_label_set_markup(GTK_LABEL(status_label), markup);
        g_free(markup);
        gtk_widget_set_visible(grant_btn, perm);
    } else if (strncmp(line, "press ", 6) == 0) {
        int b = atoi(line + 6);
        if (b >= 0 && b < NBUTTONS) {
            char t[128];
            snprintf(t, sizeof t, "Last press on the Expert Mouse: %s", BUTTON_LABELS[b]);
            gtk_label_set_text(GTK_LABEL(press_label), t);
        }
    }
}

static gboolean try_connect(gpointer unused);

static gboolean on_sock(GIOChannel *ch, GIOCondition cond, gpointer unused)
{
    (void)ch; (void)unused;
    static char buf[2048];
    static size_t len;
    ssize_t n = (cond & G_IO_IN) ? recv(sock, buf + len, sizeof buf - 1 - len, 0) : 0;
    if (n <= 0) {
        close(sock);
        sock = -1;
        len = 0;
        gtk_label_set_text(GTK_LABEL(status_label), "Remapper not running - restarting...");
        g_timeout_add(1000, try_connect, NULL);
        return FALSE;
    }
    len += n;
    buf[len] = 0;
    char *start = buf, *nl;
    while ((nl = strchr(start, '\n'))) { *nl = 0; handle_line(start); start = nl + 1; }
    len = strlen(start);
    memmove(buf, start, len + 1);
    return TRUE;
}

static gboolean try_connect(gpointer unused)
{
    (void)unused;
    static int attempts;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    socket_path(addr.sun_path, sizeof addr.sun_path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) {
        sock = fd;
        attempts = 0;
        GIOChannel *ch = g_io_channel_unix_new(fd);
        sock_watch = g_io_add_watch(ch, G_IO_IN | G_IO_HUP | G_IO_ERR, on_sock, NULL);
        g_io_channel_unref(ch);
        return FALSE;
    }
    close(fd);
    if (attempts++ % 5 == 0) start_daemon();
    return TRUE;   /* retry */
}

/* ---------------- settings ---------------- */

static void changed(void)
{
    if (loading) return;
    if (!config_save(&cfg)) gtk_label_set_text(GTK_LABEL(status_label), "Could not save settings file");
    send_cmd("reload\n");
}

static void refresh_keys_btn(int b)
{
    const Action *a = &cfg.actions[b];
    int keys = gtk_combo_box_get_active(GTK_COMBO_BOX(action_combo[b])) == ACT_KEYS;
    gtk_widget_set_visible(keys_btn[b], keys);
    gtk_button_set_label(GTK_BUTTON(keys_btn[b]),
        capturing == b ? "Press keys..." : (a->kind == ACT_KEYS && a->label[0]) ? a->label : "Set keys...");
}

static void on_action(GtkComboBox *combo, gpointer data)
{
    int b = GPOINTER_TO_INT(data), k = gtk_combo_box_get_active(combo);
    if (loading || k < 0) return;
    if (k == ACT_KEYS) {
        /* remap only once a combination has been recorded */
        capturing = b;
        refresh_keys_btn(b);
        return;
    }
    memset(&cfg.actions[b], 0, sizeof cfg.actions[b]);
    cfg.actions[b].kind = (ActionKind)k;
    if (capturing == b) capturing = -1;
    refresh_keys_btn(b);
    changed();
}

static void on_keys_btn(GtkButton *btn, gpointer data)
{
    (void)btn;
    int prev = capturing;
    capturing = GPOINTER_TO_INT(data);
    if (prev >= 0 && prev != capturing) refresh_keys_btn(prev);
    refresh_keys_btn(capturing);
}

static void record(int b, guint keyval, GdkModifierType mods, int keycode, gboolean is_modifier)
{
    Action *a = &cfg.actions[b];
    memset(a, 0, sizeof *a);
    a->kind = ACT_KEYS;
    if (mods & GDK_CONTROL_MASK) a->keys[a->nkeys++] = KEY_LEFTCTRL;
    if (mods & GDK_SHIFT_MASK)   a->keys[a->nkeys++] = KEY_LEFTSHIFT;
    if (mods & GDK_MOD1_MASK)    a->keys[a->nkeys++] = KEY_LEFTALT;
    if (mods & (GDK_SUPER_MASK | GDK_MOD4_MASK)) a->keys[a->nkeys++] = KEY_LEFTMETA;
    if (!is_modifier) a->keys[a->nkeys++] = keycode;   /* X11/Wayland keycode = evdev code + 8 */
    gchar *label = gtk_accelerator_get_label(is_modifier ? 0 : keyval, mods);
    snprintf(a->label, sizeof a->label, "%s", label);
    g_free(label);
    capturing = -1;
    refresh_keys_btn(b);
    changed();
}

static GdkModifierType modifier_of(guint keyval)
{
    switch (keyval) {
    case GDK_KEY_Control_L: case GDK_KEY_Control_R: return GDK_CONTROL_MASK;
    case GDK_KEY_Shift_L: case GDK_KEY_Shift_R: return GDK_SHIFT_MASK;
    case GDK_KEY_Alt_L: case GDK_KEY_Alt_R: return GDK_MOD1_MASK;
    case GDK_KEY_Super_L: case GDK_KEY_Super_R: return GDK_SUPER_MASK;
    }
    return 0;
}

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer unused)
{
    (void)w; (void)unused;
    if (capturing < 0) return FALSE;
    GdkModifierType mods = ev->state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_MOD1_MASK | GDK_SUPER_MASK | GDK_MOD4_MASK);
    if (ev->type == GDK_KEY_PRESS && !ev->is_modifier)
        record(capturing, gdk_keyval_to_lower(ev->keyval), mods, ev->hardware_keycode - 8, FALSE);
    else if (ev->type == GDK_KEY_RELEASE && ev->is_modifier)   /* a modifier pressed on its own */
        record(capturing, 0, mods | modifier_of(ev->keyval), 0, TRUE);
    return TRUE;
}

static void on_enabled(GtkToggleButton *t, gpointer unused)
{
    (void)unused;
    cfg.enabled = gtk_toggle_button_get_active(t);
    changed();
}

static void on_device(GtkComboBox *combo, gpointer unused)
{
    (void)unused;
    const char *text = gtk_entry_get_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(combo))));
    char id[32];
    if (sscanf(text, "%31s", id) != 1) return;    /* "047D:1020  Kensington Expert Mouse" -> "047D:1020" */
    snprintf(cfg.device, sizeof cfg.device, "%s", id);
    changed();
}

/* Mice from /proc/bus/input/devices (readable without special permissions). */
static void fill_devices(GtkComboBoxText *combo)
{
    FILE *f = fopen("/proc/bus/input/devices", "r");
    if (!f) return;
    char line[512], name[256] = "", entry[320];
    unsigned vendor = 0, product = 0;
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "I: Bus=%*x Vendor=%x Product=%x", &vendor, &product) == 2) continue;
        if (strncmp(line, "N: Name=\"", 9) == 0) {
            snprintf(name, sizeof name, "%.255s", line + 9);
            char *q = strrchr(name, '"');
            if (q) *q = 0;
        } else if (strncmp(line, "H: Handlers=", 12) == 0 && strstr(line, "mouse")) {
            snprintf(entry, sizeof entry, "%04X:%04X  %s", vendor, product, name);
            if (vendor && !g_hash_table_contains(seen, entry)) {
                g_hash_table_add(seen, g_strdup(entry));
                gtk_combo_box_text_append_text(combo, entry);
            }
        }
    }
    g_hash_table_destroy(seen);
    fclose(f);
}

/* ---------------- autostart / permissions ---------------- */

static void autostart_path(char *buf, size_t n)
{
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(buf, n, "%s/autostart/hb-expertmouse.desktop", xdg);
    else snprintf(buf, n, "%s/.config/autostart/hb-expertmouse.desktop", home ? home : ".");
}

static void on_autostart(GtkToggleButton *t, gpointer unused)
{
    (void)unused;
    char path[4096];
    autostart_path(path, sizeof path);
    if (!gtk_toggle_button_get_active(t)) { unlink(path); return; }
    const char *appimage = getenv("APPIMAGE");
    char exe[4096];
    if (appimage && *appimage) snprintf(exe, sizeof exe, "%s", appimage);
    else { ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1); exe[n > 0 ? n : 0] = 0; }
    gchar *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    gchar *quoted = g_shell_quote(exe);
    gchar *content = g_strdup_printf(
        "[Desktop Entry]\nType=Application\nName=HB ExpertMouse\nComment=Expert Mouse button remapper\n"
        "Exec=%s --daemon\nIcon=input-mouse\nTerminal=false\nX-GNOME-Autostart-enabled=true\n", quoted);
    g_file_set_contents(path, content, -1, NULL);
    g_free(content);
    g_free(quoted);
}

/* One-time udev rule so the logged-in user can use the Expert Mouse and /dev/uinput without root. */
static void on_grant(GtkButton *b, gpointer unused)
{
    (void)b; (void)unused;
    const char *script =
        "set -e\n"
        "cat > /etc/udev/rules.d/70-hb-expertmouse.rules <<'EOF'\n"
        "# HB ExpertMouse: let the logged-in user read Kensington devices and create virtual input devices\n"
        "SUBSYSTEM==\"input\", ATTRS{idVendor}==\"047d\", TAG+=\"uaccess\"\n"
        "KERNEL==\"uinput\", SUBSYSTEM==\"misc\", TAG+=\"uaccess\", OPTIONS+=\"static_node=uinput\"\n"
        "EOF\n"
        "echo uinput > /etc/modules-load.d/hb-expertmouse.conf\n"
        "modprobe uinput || true\n"
        "udevadm control --reload-rules\n"
        "udevadm trigger --subsystem-match=input --subsystem-match=misc\n";
    char *argv[] = { "pkexec", "sh", "-c", (char *)script, NULL };
    GError *err = NULL;
    gint exit_status = 0;
    if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &exit_status, &err)) {
        gtk_label_set_text(GTK_LABEL(status_label), err ? err->message : "pkexec failed");
        g_clear_error(&err);
        return;
    }
    gtk_label_set_text(GTK_LABEL(status_label), exit_status == 0
        ? "Access granted - connecting..." : "Access was not granted");
    send_cmd("reload\n");
}

static void on_quit_daemon(GtkButton *b, gpointer win)
{
    (void)b;
    send_cmd("quit\n");
    gtk_widget_destroy(GTK_WIDGET(win));
}

/* ---------------- window ---------------- */

static GtkWidget *label_left(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    return l;
}

int gui_main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    config_load(&cfg);
    loading = 1;

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "HB ExpertMouse");
    /* inside the AppImage the icon sits at the AppDir root */
    const char *appdir = getenv("APPDIR");
    gchar *icon = g_strdup_printf("%s/hb_expertmouse.png", appdir ? appdir : ".");
    if (!appdir || !gtk_window_set_icon_from_file(GTK_WINDOW(win), icon, NULL))
        gtk_window_set_icon_name(GTK_WINDOW(win), "input-mouse");
    g_free(icon);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(win), 12);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(win, "key-press-event", G_CALLBACK(on_key), NULL);
    g_signal_connect(win, "key-release-event", G_CALLBACK(on_key), NULL);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_container_add(GTK_CONTAINER(win), grid);
    int row = 0;

    status_label = label_left("Connecting to remapper...");
    gtk_label_set_line_wrap(GTK_LABEL(status_label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(status_label), 60);
    gtk_grid_attach(GTK_GRID(grid), status_label, 0, row++, 3, 1);
    grant_btn = gtk_button_new_with_label("Grant access (one-time, asks for your password)");
    g_signal_connect(grant_btn, "clicked", G_CALLBACK(on_grant), NULL);
    gtk_grid_attach(GTK_GRID(grid), grant_btn, 0, row++, 3, 1);

    enabled_chk = gtk_check_button_new_with_label("Remapping enabled");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(enabled_chk), cfg.enabled);
    g_signal_connect(enabled_chk, "toggled", G_CALLBACK(on_enabled), NULL);
    gtk_grid_attach(GTK_GRID(grid), enabled_chk, 0, row++, 3, 1);

    gtk_grid_attach(GTK_GRID(grid), label_left("Device (vendor:product):"), 0, row, 1, 1);
    device_combo = gtk_combo_box_text_new_with_entry();
    fill_devices(GTK_COMBO_BOX_TEXT(device_combo));
    gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(device_combo))), cfg.device);
    g_signal_connect(device_combo, "changed", G_CALLBACK(on_device), NULL);
    gtk_grid_attach(GTK_GRID(grid), device_combo, 1, row++, 2, 1);

    press_label = label_left("Press a button on the Expert Mouse to see which one it is.");
    gtk_widget_set_opacity(press_label, 0.7);
    gtk_grid_attach(GTK_GRID(grid), press_label, 0, row++, 3, 1);

    for (int b = 0; b < NBUTTONS; b++, row++) {
        gtk_grid_attach(GTK_GRID(grid), label_left(BUTTON_LABELS[b]), 0, row, 1, 1);
        action_combo[b] = gtk_combo_box_text_new();
        for (int k = 0; k < ACT_COUNT; k++)
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(action_combo[b]), ACTION_LABELS[k]);
        gtk_combo_box_set_active(GTK_COMBO_BOX(action_combo[b]), cfg.actions[b].kind);
        g_signal_connect(action_combo[b], "changed", G_CALLBACK(on_action), GINT_TO_POINTER(b));
        gtk_grid_attach(GTK_GRID(grid), action_combo[b], 1, row, 1, 1);
        keys_btn[b] = gtk_button_new();
        gtk_widget_set_size_request(keys_btn[b], 150, -1);
        g_signal_connect(keys_btn[b], "clicked", G_CALLBACK(on_keys_btn), GINT_TO_POINTER(b));
        gtk_grid_attach(GTK_GRID(grid), keys_btn[b], 2, row, 1, 1);
    }

    char path[4096];
    autostart_path(path, sizeof path);
    autostart_chk = gtk_check_button_new_with_label("Start remapper when I log in");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(autostart_chk), access(path, F_OK) == 0);
    g_signal_connect(autostart_chk, "toggled", G_CALLBACK(on_autostart), NULL);
    gtk_grid_attach(GTK_GRID(grid), autostart_chk, 0, row++, 3, 1);

    GtkWidget *hint = label_left(
        "Only the device above is remapped; other mice are never touched. "
        "Closing this window keeps the remapper running in the background.");
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(hint), 60);
    gtk_widget_set_opacity(hint, 0.7);
    gtk_grid_attach(GTK_GRID(grid), hint, 0, row++, 3, 1);

    GtkWidget *quit = gtk_button_new_with_label("Stop remapper and quit");
    g_signal_connect(quit, "clicked", G_CALLBACK(on_quit_daemon), win);
    gtk_widget_set_halign(quit, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), quit, 0, row++, 3, 1);

    gtk_widget_show_all(win);
    for (int b = 0; b < NBUTTONS; b++) refresh_keys_btn(b);
    gtk_widget_hide(grant_btn);
    loading = 0;

    if (try_connect(NULL)) g_timeout_add(400, try_connect, NULL);
    gtk_main();
    return 0;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--daemon") == 0) return daemon_main();
    return gui_main(argc, argv);
}

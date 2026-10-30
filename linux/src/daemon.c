/* Background remapper.
 *
 * Grabs ONLY the Expert Mouse's evdev node (EVIOCGRAB), so its events no longer reach the desktop
 * directly, and re-emits them through a uinput clone with the buttons remapped. Other mice are
 * never opened. Keystroke actions go out through a separate uinput keyboard.
 *
 * The settings GUI talks to it over a unix socket:
 *   daemon -> gui: "status <text>", "press <button index>"
 *   gui -> daemon: "reload", "quit"
 */
#define _GNU_SOURCE
#include "common.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define MAXCLIENTS 8
#define BITS_LONG (sizeof(long) * 8)
#define NLONGS(x) (((x) + BITS_LONG - 1) / BITS_LONG)
#define TEST_BIT(bit, arr) (((arr)[(bit) / BITS_LONG] >> ((bit) % BITS_LONG)) & 1)

static volatile sig_atomic_t running = 1;
static Config cfg;
static int dev_fd = -1, mouse_fd = -1, kbd_fd = -1, listen_fd = -1;
static int grabbed;
static char dev_path[300], dev_name[256];
static int clients[MAXCLIENTS];
static int nclients;
static char status[512];
static Action active[NBUTTONS];   /* action started by each button's press; used for its release */
static int active_set[NBUTTONS];

static void on_signal(int sig) { (void)sig; running = 0; }

/* ---------------- GUI clients ---------------- */

static void send_line(int fd, const char *line)
{
    size_t n = strlen(line);
    if (send(fd, line, n, MSG_NOSIGNAL | MSG_DONTWAIT) != (ssize_t)n) { /* slow/dead client: dropped on next poll */ }
}

static void broadcast(const char *line)
{
    for (int i = 0; i < nclients; i++) send_line(clients[i], line);
}

static void set_status(const char *fmt, ...)
{
    char s[480];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);
    if (strcmp(s, status) == 0) return;
    snprintf(status, sizeof status, "%s", s);
    char line[520];
    snprintf(line, sizeof line, "status %s\n", status);
    broadcast(line);
    fprintf(stderr, "hb_expertmouse: %s\n", status);
}

/* Returns 0 if another daemon already owns the socket. */
static int open_listen_socket(void)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    socket_path(addr.sun_path, sizeof addr.sun_path);

    int probe = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connect(probe, (struct sockaddr *)&addr, sizeof addr) == 0) { close(probe); return 0; }
    close(probe);
    unlink(addr.sun_path);

    listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(listen_fd, 4) < 0) {
        perror("hb_expertmouse: socket");
        return -1;
    }
    return 1;
}

/* ---------------- uinput output ---------------- */

static void emit(int fd, int type, int code, int value)
{
    struct input_event ev = { .type = type, .code = code, .value = value };
    if (fd >= 0 && write(fd, &ev, sizeof ev) < 0) { /* device gone; recreated on next open */ }
}

static void syn(int fd) { emit(fd, EV_SYN, SYN_REPORT, 0); }

static int create_uinput(const char *name, const struct input_id *id,
                         const unsigned long *keybits, const unsigned long *relbits)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    for (int k = 0; k < KEY_CNT; k++)
        if (TEST_BIT(k, keybits)) ioctl(fd, UI_SET_KEYBIT, k);
    if (relbits) {
        ioctl(fd, UI_SET_EVBIT, EV_REL);
        for (int r = 0; r < REL_CNT; r++)
            if (TEST_BIT(r, relbits)) ioctl(fd, UI_SET_RELBIT, r);
    }
    struct uinput_setup us;
    memset(&us, 0, sizeof us);
    us.id = *id;
    snprintf(us.name, sizeof us.name, "%s", name);
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) { close(fd); return -1; }
    return fd;
}

static int ensure_keyboard(void)
{
    if (kbd_fd >= 0) return 1;
    unsigned long keys[NLONGS(KEY_CNT)];
    memset(keys, 0, sizeof keys);
    for (int k = KEY_ESC; k <= KEY_MICMUTE; k++) keys[k / BITS_LONG] |= 1UL << (k % BITS_LONG);
    struct input_id id = { .bustype = BUS_VIRTUAL, .vendor = 0x4842, .product = 0x4b42, .version = 1 };
    kbd_fd = create_uinput("HB ExpertMouse keyboard (virtual)", &id, keys, NULL);
    return kbd_fd >= 0;
}

/* ---------------- input device ---------------- */

static void close_device(void)
{
    if (dev_fd >= 0) { if (grabbed) ioctl(dev_fd, EVIOCGRAB, 0); close(dev_fd); }
    if (mouse_fd >= 0) { ioctl(mouse_fd, UI_DEV_DESTROY); close(mouse_fd); }
    dev_fd = mouse_fd = -1;
    grabbed = 0;
    memset(active_set, 0, sizeof active_set);
}

static int matches(int fd, int vendor, int product)
{
    struct input_id id;
    unsigned long keys[NLONGS(KEY_CNT)] = { 0 };
    if (ioctl(fd, EVIOCGID, &id) < 0 || id.vendor != vendor) return 0;
    if (product >= 0 && id.product != product) return 0;
    /* pick the pointer interface, not e.g. a consumer-control node of the same device */
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0) return 0;
    return TEST_BIT(BTN_LEFT, keys);
}

static void open_device(void)
{
    int vendor, product, denied = 0;
    if (!config_device_ids(&cfg, &vendor, &product)) { set_status("error: invalid device ID '%s'", cfg.device); return; }

    DIR *d = opendir("/dev/input");
    struct dirent *e;
    while (d && (e = readdir(d)) && dev_fd < 0) {
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        char path[300];
        snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) { if (errno == EACCES) denied = 1; continue; }
        if (matches(fd, vendor, product)) {
            dev_fd = fd;
            snprintf(dev_path, sizeof dev_path, "%s", path);
        } else close(fd);
    }
    if (d) closedir(d);

    if (dev_fd < 0) {
        if (denied) set_status("perm: no permission to read input devices");
        else set_status("nodevice: Expert Mouse (%s) not connected", cfg.device);
        return;
    }
    if (!ensure_keyboard()) {
        close_device();
        set_status("perm: cannot open /dev/uinput");
        return;
    }

    /* Clone the trackball's buttons and axes, plus all mouse buttons for remap targets. */
    unsigned long keys[NLONGS(KEY_CNT)] = { 0 }, rels[NLONGS(REL_CNT)] = { 0 };
    struct input_id id;
    ioctl(dev_fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
    ioctl(dev_fd, EVIOCGBIT(EV_REL, sizeof rels), rels);
    ioctl(dev_fd, EVIOCGID, &id);
    ioctl(dev_fd, EVIOCGNAME(sizeof dev_name), dev_name);
    for (int k = BTN_LEFT; k <= BTN_TASK; k++) keys[k / BITS_LONG] |= 1UL << (k % BITS_LONG);
    /* keep "Trackball" in the name so udev/desktops still treat it as one */
    mouse_fd = create_uinput("HB ExpertMouse Trackball (virtual)", &id, keys, rels);
    if (mouse_fd < 0) { close_device(); set_status("perm: cannot create virtual mouse via /dev/uinput"); return; }
}

/* Grab only while enabled; disabled means the trackball works natively. */
static void update_grab(void)
{
    if (dev_fd < 0) return;
    int want = cfg.enabled;
    if (want != grabbed && ioctl(dev_fd, EVIOCGRAB, want) == 0) grabbed = want;
    if (want && !grabbed) set_status("error: another program has grabbed %s", dev_path);
    else set_status("%s: %s (%s)", grabbed ? "ok" : "disabled", dev_name, dev_path);
}

/* ---------------- remapping ---------------- */

static void press_mouse(int code, int down) { emit(mouse_fd, EV_KEY, code, down); }

static void run_action(const Action *a, int button, int down)
{
    switch (a->kind) {
    case ACT_DEFAULT:  press_mouse(BUTTON_CODES[button], down); break;
    case ACT_LEFT:     press_mouse(BTN_LEFT, down); break;
    case ACT_RIGHT:    press_mouse(BTN_RIGHT, down); break;
    case ACT_MIDDLE:   press_mouse(BTN_MIDDLE, down); break;
    case ACT_BACK:     press_mouse(BTN_SIDE, down); break;
    case ACT_FORWARD:  press_mouse(BTN_EXTRA, down); break;
    case ACT_DOUBLE:
        if (down) for (int i = 0; i < 2; i++) {
            press_mouse(BTN_LEFT, 1); syn(mouse_fd);
            press_mouse(BTN_LEFT, 0); syn(mouse_fd);
        }
        break;
    case ACT_KEYS:     /* held while the button is held */
        if (down) for (int i = 0; i < a->nkeys; i++) emit(kbd_fd, EV_KEY, a->keys[i], 1);
        else for (int i = a->nkeys - 1; i >= 0; i--) emit(kbd_fd, EV_KEY, a->keys[i], 0);
        syn(kbd_fd);
        break;
    default: break;    /* ACT_DISABLED */
    }
}

static void release_all(void)
{
    for (int b = 0; b < NBUTTONS; b++)
        if (active_set[b]) { run_action(&active[b], b, 0); active_set[b] = 0; }
    syn(mouse_fd);
}

static void handle_event(const struct input_event *ev)
{
    if (ev->type == EV_KEY) {
        for (int b = 0; b < NBUTTONS; b++) {
            if (ev->code != BUTTON_CODES[b]) continue;
            if (ev->value == 1) {
                char line[32];
                snprintf(line, sizeof line, "press %d\n", b);
                broadcast(line);
                if (active_set[b]) run_action(&active[b], b, 0);
                active[b] = cfg.actions[b];
                active_set[b] = 1;
                run_action(&active[b], b, 1);
            } else if (ev->value == 0 && active_set[b]) {
                active_set[b] = 0;
                run_action(&active[b], b, 0);
            }
            return;
        }
    }
    if (ev->type == EV_MSC) return;      /* scan codes would describe the original buttons */
    emit(mouse_fd, ev->type, ev->code, ev->value);   /* motion, scroll ring, SYN, other keys */
}

static void read_device(void)
{
    struct input_event evs[64];
    for (;;) {
        ssize_t n = read(dev_fd, evs, sizeof evs);
        if (n < 0 && errno == EAGAIN) return;
        if (n <= 0) {   /* unplugged */
            release_all();
            close_device();
            set_status("nodevice: Expert Mouse disconnected");
            return;
        }
        for (size_t i = 0; i < (size_t)n / sizeof evs[0]; i++) handle_event(&evs[i]);
    }
}

static void reload(void)
{
    Config old = cfg;
    release_all();
    config_load(&cfg);
    if (strcasecmp(old.device, cfg.device) != 0) close_device();
    update_grab();
}

static void read_client(int idx)
{
    char buf[256];
    ssize_t n = recv(clients[idx], buf, sizeof buf - 1, MSG_DONTWAIT);
    if (n <= 0) {
        if (n < 0 && errno == EAGAIN) return;
        close(clients[idx]);
        clients[idx] = clients[--nclients];
        return;
    }
    buf[n] = 0;
    if (strstr(buf, "reload")) reload();
    if (strstr(buf, "quit")) running = 0;
}

int daemon_main(void)
{
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGPIPE, SIG_IGN);

    int s = open_listen_socket();
    if (s == 0) { fprintf(stderr, "hb_expertmouse: already running\n"); return 0; }
    if (s < 0) return 1;

    config_load(&cfg);
    while (running) {
        if (dev_fd < 0) { open_device(); update_grab(); }

        struct pollfd fds[2 + MAXCLIENTS];
        int n = 0;
        fds[n++] = (struct pollfd){ .fd = listen_fd, .events = POLLIN };
        int dev_idx = -1;
        if (dev_fd >= 0) { dev_idx = n; fds[n++] = (struct pollfd){ .fd = dev_fd, .events = POLLIN }; }
        int first_client = n;
        for (int i = 0; i < nclients; i++) fds[n++] = (struct pollfd){ .fd = clients[i], .events = POLLIN };

        /* no device: rescan every 2 s (hotplug / permissions granted) */
        if (poll(fds, n, dev_fd < 0 ? 2000 : -1) < 0) continue;

        if (dev_idx >= 0 && (fds[dev_idx].revents & (POLLIN | POLLERR | POLLHUP))) read_device();
        for (int i = n - 1; i >= first_client; i--)
            if (fds[i].revents & (POLLIN | POLLERR | POLLHUP)) read_client(i - first_client);
        if (fds[0].revents & POLLIN) {
            int c = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
            if (c >= 0 && nclients < MAXCLIENTS) {
                clients[nclients++] = c;
                char line[520];
                snprintf(line, sizeof line, "status %s\n", status);
                send_line(c, line);
            } else if (c >= 0) close(c);
        }
    }

    release_all();
    close_device();
    if (kbd_fd >= 0) { ioctl(kbd_fd, UI_DEV_DESTROY); close(kbd_fd); }
    char path[108];
    socket_path(path, sizeof path);
    unlink(path);
    return 0;
}

#include "common.h"

#include <libgen.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

const char *const BUTTON_KEYS[NBUTTONS] = { "Left", "Right", "Middle", "X1", "X2" };
const char *const BUTTON_LABELS[NBUTTONS] = {
    "Left button", "Right button", "Middle button", "Button 4 (Back)", "Button 5 (Forward)" };
const int BUTTON_CODES[NBUTTONS] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA };

const char *const ACTION_KEYS[ACT_COUNT] = {
    "Default", "Disabled", "Left", "Right", "Middle", "Back", "Forward", "DoubleClick", "Keys" };
const char *const ACTION_LABELS[ACT_COUNT] = {
    "Default (unchanged)", "Disabled", "Left click", "Right click", "Middle click",
    "Back", "Forward", "Double click", "Keystroke..." };

void config_defaults(Config *c)
{
    memset(c, 0, sizeof *c);
    c->enabled = 1;
    strcpy(c->device, "047D");
}

/* Portable: next to the AppImage (or the binary) if writable, else ~/.config. */
void config_path(char *buf, size_t n)
{
    char exe[4096] = "";
    const char *appimage = getenv("APPIMAGE");
    if (appimage && *appimage) snprintf(exe, sizeof exe, "%s", appimage);
    else {
        ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
        exe[len > 0 ? len : 0] = 0;
    }
    if (*exe) {
        char *dir = dirname(exe);
        if (access(dir, W_OK) == 0) { snprintf(buf, n, "%s/HB_ExpertMouse.ini", dir); return; }
    }
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(buf, n, "%s/HB_ExpertMouse.ini", xdg);
    else snprintf(buf, n, "%s/.config/HB_ExpertMouse.ini", home ? home : ".");
}

void socket_path(char *buf, size_t n)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt && *rt) snprintf(buf, n, "%s/hb_expertmouse.sock", rt);
    else snprintf(buf, n, "/tmp/hb_expertmouse-%d.sock", (int)getuid());
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    return s;
}

/* "Keys:29,46;Ctrl+C" -> codes + label */
static void parse_action(Action *a, char *v)
{
    memset(a, 0, sizeof *a);
    if (strncasecmp(v, "Keys:", 5) == 0) {
        char *label = strchr(v, ';');
        if (label) { *label++ = 0; snprintf(a->label, sizeof a->label, "%s", label); }
        for (char *tok = strtok(v + 5, ","); tok && a->nkeys < MAXKEYS; tok = strtok(NULL, ",")) {
            int code = atoi(tok);
            if (code > 0 && code < KEY_MAX) a->keys[a->nkeys++] = code;
        }
        a->kind = a->nkeys ? ACT_KEYS : ACT_DEFAULT;
        return;
    }
    for (int k = 0; k < ACT_COUNT; k++)
        if (k != ACT_KEYS && strcasecmp(v, ACTION_KEYS[k]) == 0) a->kind = (ActionKind)k;
}

void config_load(Config *c)
{
    char path[4200], line[512];
    config_defaults(c);
    config_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == ';' || line[0] == '#') continue;
        *eq = 0;
        char *key = trim(line), *val = trim(eq + 1);
        if (!strcasecmp(key, "Enabled")) c->enabled = strcmp(val, "0") && strcasecmp(val, "false");
        else if (!strcasecmp(key, "Device")) snprintf(c->device, sizeof c->device, "%s", val);
        else for (int b = 0; b < NBUTTONS; b++)
            if (!strcasecmp(key, BUTTON_KEYS[b])) parse_action(&c->actions[b], val);
    }
    fclose(f);
}

int config_save(const Config *c)
{
    char path[4200], tmp[4300];
    config_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return 0;
    fprintf(f, "; HB ExpertMouse settings\nEnabled=%d\nDevice=%s\n", c->enabled ? 1 : 0, c->device);
    for (int b = 0; b < NBUTTONS; b++) {
        const Action *a = &c->actions[b];
        if (a->kind == ACT_KEYS) {
            fprintf(f, "%s=Keys:", BUTTON_KEYS[b]);
            for (int i = 0; i < a->nkeys; i++) fprintf(f, "%s%d", i ? "," : "", a->keys[i]);
            fprintf(f, ";%s\n", a->label);
        } else fprintf(f, "%s=%s\n", BUTTON_KEYS[b], ACTION_KEYS[a->kind]);
    }
    if (fclose(f) != 0) return 0;
    return rename(tmp, path) == 0;   /* atomic: the daemon never sees a half-written file */
}

int config_device_ids(const Config *c, int *vendor, int *product)
{
    unsigned v, p;
    *product = -1;
    if (sscanf(c->device, "%x:%x", &v, &p) == 2) { *vendor = (int)v; *product = (int)p; return 1; }
    if (sscanf(c->device, "%x", &v) == 1) { *vendor = (int)v; return 1; }
    return 0;
}

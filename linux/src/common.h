/* HB ExpertMouse for Linux - shared config and helpers. */
#pragma once
#include <stddef.h>

#define NBUTTONS 5
#define MAXKEYS 8

/* Buttons as the kernel reports them: BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA. */
extern const char *const BUTTON_KEYS[NBUTTONS];   /* ini names: Left Right Middle X1 X2 */
extern const char *const BUTTON_LABELS[NBUTTONS]; /* GUI names */
extern const int BUTTON_CODES[NBUTTONS];

typedef enum {
    ACT_DEFAULT, ACT_DISABLED, ACT_LEFT, ACT_RIGHT, ACT_MIDDLE,
    ACT_BACK, ACT_FORWARD, ACT_DOUBLE, ACT_KEYS, ACT_COUNT
} ActionKind;

extern const char *const ACTION_KEYS[ACT_COUNT];   /* ini names */
extern const char *const ACTION_LABELS[ACT_COUNT]; /* GUI names */

typedef struct {
    ActionKind kind;
    int keys[MAXKEYS];   /* evdev key codes, pressed in order, released in reverse */
    int nkeys;
    char label[64];      /* human-readable combo, e.g. "Ctrl+C" */
} Action;

typedef struct {
    int enabled;
    char device[32];     /* "VVVV" or "VVVV:PPPP" (hex USB vendor[:product]); 047D = Kensington */
    Action actions[NBUTTONS];
} Config;

void config_defaults(Config *c);
void config_path(char *buf, size_t n);
void config_load(Config *c);
int  config_save(const Config *c);
/* Parses c->device into vendor/product; product = -1 when not given. Returns 0 if invalid. */
int  config_device_ids(const Config *c, int *vendor, int *product);

void socket_path(char *buf, size_t n);

int daemon_main(void);
int gui_main(int argc, char **argv);

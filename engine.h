#pragma once
/* Shared window scan / resize logic. */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "x11.h"

typedef struct {
    uint32_t id;
    int x, y, w, h;
    int override;
    int fullscreen;
    int hint_fixed;
    int min_w, min_h, max_w, max_h;
    int pid;
    uint32_t child;
    int child_w, child_h;
    char title[160];
    char cls[80];
} WinInfo;

/* Hit targets for the software-rendered window. */
typedef struct {
    int id, x, y, w, h;
} Hot;

typedef struct {
    uint32_t net_wm_name, utf8, pid, state, fullscreen, moveresize;
    uint32_t mode, server, scale;
    uint32_t protocols, wm_delete;
} Atoms;

typedef struct {
    XConn *x;
    Atoms atom;
    int atoms_ready;
    WinInfo wins[96];
    int nwins;
    int selected;
    int scroll;
    int root_w, root_h;
    uint32_t server_id;
    int have_scale;
    double screen_scale;
    char display[64];
    char displays[8][32];
    int ndisplays;

    char width_buf[8];
    char height_buf[8];
    char screen_w[8];
    char screen_h[8];
    int focus_field; /* -1 none, 0 width, 1 height, 2 screen w, 3 screen h */
    int pin, unfull, do_child, aspect, superres;
    int aspect_w, aspect_h;
    uint32_t pin_id;
    int pin_w, pin_h;

    char status[320];
    int status_kind;

    uint32_t *fb;
    int fb_w, fb_h;
    uint32_t our_win;
    uint32_t gc;
    Hot hots[96];
    int nhots;
    int hover;
    int pressed;
    int mouse_x, mouse_y;
    uint32_t last_click_id;
    uint64_t last_click_ms;
} App;

enum { FR_NPRESETS = 5 };
extern const int FR_PRESETS[FR_NPRESETS][2];

void put32(uint8_t *p, uint32_t v);
uint32_t get32(const uint8_t *p);
void set_status(App *a, int kind, const char *msg);

int ensure_atoms(App *a);
void read_hints(XConn *x, uint32_t win, int *fixed, int *minw, int *minh, int *maxw, int *maxh);
int write_fixed_hints(XConn *x, uint32_t win, int w, int h);
int apply_size(App *a, WinInfo *wi, int w, int h, int clear_fs, int do_child, char *msg, size_t msgsz);
void set_xwayland_mode(App *a, int w, int h, int super, char *msg, size_t msgsz);
void read_root_meta(App *a);
int scan_windows(App *a);
void reapply_pin(App *a);
int find_displays(App *a);

/* Connect a->display. On failure the message is in a->status and a->x is NULL. */
int fr_xconnect(App *a);

#ifdef __cplusplus
}
#endif

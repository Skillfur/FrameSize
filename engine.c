#define _GNU_SOURCE
#include "engine.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const int FR_PRESETS[FR_NPRESETS][2] = {
    {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}
};

void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
void set_status(App *a, int kind, const char *msg) {
    snprintf(a->status, sizeof a->status, "%s", msg);
    a->status_kind = kind;
}
static void copy_clean(char *dst, size_t n, const uint8_t *src, size_t len) {
    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < n; i++) {
        unsigned char c = src[i];
        if (c == 0) break;
        if (c < 32 || c == 127) c = ' ';
        dst[o++] = (char)c;
    }
    dst[o] = 0;
    while (o > 0 && dst[o - 1] == ' ') dst[--o] = 0;
}
int ensure_atoms(App *a) {
    if (a->atoms_ready) return 0;
    a->atom.net_wm_name = x_intern(a->x, "_NET_WM_NAME");
    a->atom.utf8 = x_intern(a->x, "UTF8_STRING");
    a->atom.pid = x_intern(a->x, "_NET_WM_PID");
    a->atom.state = x_intern(a->x, "_NET_WM_STATE");
    a->atom.fullscreen = x_intern(a->x, "_NET_WM_STATE_FULLSCREEN");
    a->atom.moveresize = x_intern(a->x, "_NET_MOVERESIZE_WINDOW");
    a->atom.mode = x_intern(a->x, "GAMESCOPE_XWAYLAND_MODE_CONTROL");
    a->atom.server = x_intern(a->x, "GAMESCOPE_XWAYLAND_SERVER_ID");
    a->atom.scale = x_intern(a->x, "STEAM_SCREEN_SCALE");
    a->atom.protocols = x_intern(a->x, "WM_PROTOCOLS");
    a->atom.wm_delete = x_intern(a->x, "WM_DELETE_WINDOW");
    a->atoms_ready = 1;
    return 0;
}
void read_hints(XConn *x, uint32_t win, int *fixed, int *minw, int *minh, int *maxw, int *maxh) {
    *fixed = 0; *minw = *minh = *maxw = *maxh = 0;
    uint8_t *data = NULL;
    uint32_t n = 0;
    uint8_t fmt = 0;
    uint32_t type = 0;
    if (x_property(x, win, XA_WM_NORMAL_HINTS_, &data, &n, &fmt, &type) < 0 || !data || fmt != 32 || n < 9) {
        free(data);
        return;
    }
    uint32_t flags = get32(data);
    int32_t minw_v = (int32_t)get32(data + 5 * 4);
    int32_t minh_v = (int32_t)get32(data + 6 * 4);
    int32_t maxw_v = (int32_t)get32(data + 7 * 4);
    int32_t maxh_v = (int32_t)get32(data + 8 * 4);
    *minw = minw_v; *minh = minh_v; *maxw = maxw_v; *maxh = maxh_v;
    if ((flags & ((1u << 4) | (1u << 5))) && minw_v && minh_v && minw_v == maxw_v && minh_v == maxh_v)
        *fixed = 1;
    free(data);
}
int write_fixed_hints(XConn *x, uint32_t win, int w, int h) {
    int32_t f[18];
    memset(f, 0, sizeof f);
    uint8_t *data = NULL;
    uint32_t n = 0;
    uint8_t fmt = 0;
    uint32_t type = 0;
    if (x_property(x, win, XA_WM_NORMAL_HINTS_, &data, &n, &fmt, &type) == 0 && data && fmt == 32) {
        uint32_t cnt = n > 18 ? 18 : n;
        for (uint32_t i = 0; i < cnt; i++) f[i] = (int32_t)get32(data + i * 4);
    }
    free(data);
    f[0] |= (1 << 3) | (1 << 4) | (1 << 5) | (1 << 8);
    f[3] = w;
    f[4] = h;
    f[5] = w;
    f[6] = h;
    f[7] = w;
    f[8] = h;
    if (f[9] == 0) f[9] = 1;
    if (f[10] == 0) f[10] = 1;
    f[15] = w;
    f[16] = h;
    uint8_t raw[18 * 4];
    for (int i = 0; i < 18; i++) put32(raw + i * 4, (uint32_t)f[i]);
    return x_change_property(x, win, XA_WM_NORMAL_HINTS_, XA_WM_SIZE_HINTS_, 32, raw, 18);
}
static void send_moveresize(App *a, uint32_t win, int x, int y, int w, int h) {
    if (!a->atom.moveresize) return;
    uint8_t ev[32];
    memset(ev, 0, sizeof ev);
    ev[0] = 33;
    ev[1] = 32;
    put32(ev + 4, win);
    put32(ev + 8, a->atom.moveresize);
    uint32_t flags = 10u | (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) | (2u << 12);
    put32(ev + 12, flags);
    put32(ev + 16, (uint32_t)x);
    put32(ev + 20, (uint32_t)y);
    put32(ev + 24, (uint32_t)w);
    put32(ev + 28, (uint32_t)h);
    x_send_event(a->x, a->x->root, 0x00180000u, ev);
}
static void send_unfullscreen(App *a, uint32_t win) {
    if (!a->atom.state || !a->atom.fullscreen) return;
    uint8_t ev[32];
    memset(ev, 0, sizeof ev);
    ev[0] = 33;
    ev[1] = 32;
    put32(ev + 4, win);
    put32(ev + 8, a->atom.state);
    put32(ev + 12, 0); /* _NET_WM_STATE_REMOVE */
    put32(ev + 16, a->atom.fullscreen);
    put32(ev + 20, 0);
    put32(ev + 24, 2); /* pager */
    x_send_event(a->x, a->x->root, 0x00180000u, ev);
}
int apply_size(App *a, WinInfo *wi, int w, int h, int clear_fs, int do_child, char *msg, size_t msgsz) {
    if (w < 1 || h < 1 || w > 7680 || h > 7680) {
        snprintf(msg, msgsz, "Size must be between 1 and 7680.");
        return -1;
    }
    if (clear_fs) send_unfullscreen(a, wi->id);
    if (write_fixed_hints(a->x, wi->id, w, h) < 0) {
        snprintf(msg, msgsz, "Could not write WM_NORMAL_HINTS (%s).", a->x->err);
        return -1;
    }
    if (x_configure(a->x, wi->id, 0x000c, 0, 0, w, h) < 0) {
        snprintf(msg, msgsz, "ConfigureWindow failed (%s).", a->x->err);
        return -1;
    }
    send_moveresize(a, wi->id, wi->x, wi->y, w, h);
    int touched_child = 0;
    if (do_child && wi->child && wi->child_w > 1 && wi->child_h > 1 &&
        (wi->child_w >= wi->w / 2 || wi->child_w >= 200) && wi->child_h >= 100) {
        write_fixed_hints(a->x, wi->child, w, h);
        x_configure(a->x, wi->child, 0x000f, 0, 0, w, h);
        touched_child = 1;
    }
    int gw = 0, gh = 0, gx = 0, gy = 0;
    int matched = 0;
    for (int i = 0; i < 12; i++) {
        XEvent ev;
        x_poll(a->x, &ev, 40);
        if (x_geometry(a->x, wi->id, &gx, &gy, &gw, &gh) == 0 && gw == w && gh == h) {
            matched = 1;
            break;
        }
    }
    wi->w = gw;
    wi->h = gh;
    wi->x = gx;
    wi->y = gy;
    if (matched) {
        snprintf(msg, msgsz,
                 "Actual buffer is now %dx%d%s.",
                 w, h, touched_child ? " (content child too)" : "");
        return 0;
    }
    snprintf(msg, msgsz,
             "Sent hint + ConfigureWindow, window stayed %dx%d. If it is forced fullscreen, Gamescope snaps it back to the Xwayland screen.",
             gw, gh);
    return 1;
}
void set_xwayland_mode(App *a, int w, int h, int super, char *msg, size_t msgsz) {
    if (w < 1 || h < 1 || w > 7680 || h > 7680) {
        snprintf(msg, msgsz, "Screen size must be between 1 and 7680.");
        return;
    }
    uint8_t raw[16];
    put32(raw + 0, a->server_id);
    put32(raw + 4, (uint32_t)w);
    put32(raw + 8, (uint32_t)h);
    put32(raw + 12, super ? 1u : 0u);
    if (x_change_property(a->x, a->x->root, a->atom.mode, XA_CARDINAL_, 32, raw, 4) < 0) {
        snprintf(msg, msgsz, "Could not set GAMESCOPE_XWAYLAND_MODE_CONTROL (%s).", a->x->err);
        return;
    }
    snprintf(msg, msgsz, "Asked Xwayland server %u to become %dx%d. That is the virtual screen, not one window's scale.",
             a->server_id, w, h);
}
static int window_is_ours(App *a, uint32_t id, int pid) {
    if (id == a->our_win) return 1;
    if (pid > 0 && pid == (int)getpid()) return 1;
    return 0;
}
void read_root_meta(App *a) {
    a->root_w = a->x->root_w;
    a->root_h = a->x->root_h;
    int x, y, w, h;
    if (x_geometry(a->x, a->x->root, &x, &y, &w, &h) == 0) {
        a->root_w = w;
        a->root_h = h;
    }
    a->server_id = 0;
    a->have_scale = 0;
    uint8_t *data = NULL;
    uint32_t n = 0;
    uint8_t fmt = 0;
    uint32_t type = 0;
    if (a->atom.server &&
        x_property(a->x, a->x->root, a->atom.server, &data, &n, &fmt, &type) == 0 &&
        data && fmt == 32 && n >= 1) {
        a->server_id = get32(data);
    }
    free(data);
    data = NULL;
    if (a->atom.scale &&
        x_property(a->x, a->x->root, a->atom.scale, &data, &n, &fmt, &type) == 0 &&
        data && fmt == 32 && n >= 1) {
        uint32_t v = get32(data);
        a->screen_scale = (double)v / (double)0xffffffffu;
        a->have_scale = 1;
    }
    free(data);
}
int scan_windows(App *a) {
    uint32_t keep = 0;
    if (a->selected >= 0 && a->selected < a->nwins) keep = a->wins[a->selected].id;
    uint32_t *ch = NULL;
    int n = 0;
    if (x_query_tree(a->x, a->x->root, &ch, &n) < 0) return -1;
    WinInfo tmp[96];
    int nt = 0;
    for (int i = n - 1; i >= 0 && nt < 96; i--) {
        uint32_t id = ch[i];
        int map_state = 0, override = 0, klass = 0;
        if (x_attributes(a->x, id, &map_state, &override, &klass) < 0) continue;
        if (!getenv("FR_ALL") && (map_state != 2 || klass == 2)) continue;
        if (getenv("FR_ALL")) fprintf(stderr, "child 0x%x map %d class %d\n", id, map_state, klass);
        int x, y, w, h;
        if (x_geometry(a->x, id, &x, &y, &w, &h) < 0) continue;
        if (w < 2 || h < 2) continue;
        uint8_t *data = NULL;
        uint32_t ni = 0;
        uint8_t fmt = 0;
        uint32_t type = 0;
        int pid = 0;
        if (a->atom.pid && x_property(a->x, id, a->atom.pid, &data, &ni, &fmt, &type) == 0 &&
            data && fmt == 32 && ni >= 1) {
            pid = (int)get32(data);
        }
        free(data);
        if (window_is_ours(a, id, pid)) continue;

        WinInfo *wi = &tmp[nt];
        memset(wi, 0, sizeof *wi);
        wi->id = id;
        wi->x = x; wi->y = y; wi->w = w; wi->h = h;
        wi->override = override;
        wi->pid = pid;
        read_hints(a->x, id, &wi->hint_fixed, &wi->min_w, &wi->min_h, &wi->max_w, &wi->max_h);

        data = NULL;
        if (a->atom.net_wm_name &&
            x_property(a->x, id, a->atom.net_wm_name, &data, &ni, &fmt, &type) == 0 &&
            data && fmt == 8 && ni) {
            copy_clean(wi->title, sizeof wi->title, data, ni);
        }
        free(data);
        if (!wi->title[0]) {
            data = NULL;
            if (x_property(a->x, id, XA_WM_NAME_, &data, &ni, &fmt, &type) == 0 && data && fmt == 8 && ni)
                copy_clean(wi->title, sizeof wi->title, data, ni);
            free(data);
        }
        if (!wi->title[0]) snprintf(wi->title, sizeof wi->title, "(untitled)");

        data = NULL;
        if (x_property(a->x, id, XA_WM_CLASS_, &data, &ni, &fmt, &type) == 0 && data && fmt == 8 && ni) {
            const char *inst = (const char *)data;
            size_t L = strnlen(inst, ni);
            const char *klass_s = (L + 1 < ni) ? inst + L + 1 : inst;
            copy_clean(wi->cls, sizeof wi->cls, (const uint8_t *)klass_s, ni);
        }
        free(data);

        data = NULL;
        if (a->atom.state &&
            x_property(a->x, id, a->atom.state, &data, &ni, &fmt, &type) == 0 &&
            data && fmt == 32) {
            for (uint32_t k = 0; k < ni; k++) {
                if (get32(data + k * 4) == a->atom.fullscreen) wi->fullscreen = 1;
            }
        }
        free(data);

        uint32_t *kids = NULL;
        int nk = 0;
        if (x_query_tree(a->x, id, &kids, &nk) == 0 && kids) {
            int best = 0;
            for (int k = 0; k < nk; k++) {
                int ms = 0, ov = 0, kl = 0;
                if (x_attributes(a->x, kids[k], &ms, &ov, &kl) < 0) continue;
                if (kl == 2) continue;
                int cx, cy, cw, chh;
                if (x_geometry(a->x, kids[k], &cx, &cy, &cw, &chh) < 0) continue;
                if ((long)cw * chh > best && cw > 1 && chh > 1) {
                    best = cw * chh;
                    wi->child = kids[k];
                    wi->child_w = cw;
                    wi->child_h = chh;
                }
            }
        }
        free(kids);
        nt++;
    }
    free(ch);
    a->nwins = nt;
    memcpy(a->wins, tmp, (size_t)nt * sizeof(WinInfo));
    a->selected = -1;
    for (int i = 0; i < nt; i++) {
        if (keep && a->wins[i].id == keep) a->selected = i;
    }
    if (a->selected < 0 && nt > 0) a->selected = 0;
    return 0;
}
void reapply_pin(App *a) {
    if (!a->pin || !a->pin_id) return;
    for (int i = 0; i < a->nwins; i++) {
        if (a->wins[i].id != a->pin_id) continue;
        char msg[320];
        int rc = apply_size(a, &a->wins[i], a->pin_w, a->pin_h, a->unfull, a->do_child, msg, sizeof msg);
        if (rc != 0) set_status(a, 2, msg);
        return;
    }
}
int find_displays(App *a) {
    a->ndisplays = 0;
    DIR *d = opendir("/tmp/.X11-unix");
    if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] != 'X') continue;
        char name[32];
        snprintf(name, sizeof name, ":%s", de->d_name + 1);
        if (a->ndisplays < 8) snprintf(a->displays[a->ndisplays++], 32, "%s", name);
    }
    closedir(d);
    /* numeric sort by a cheap bubble */
    for (int i = 0; i < a->ndisplays; i++) {
        for (int j = i + 1; j < a->ndisplays; j++) {
            if (atoi(a->displays[j] + 1) < atoi(a->displays[i] + 1)) {
                char tmp[32];
                memcpy(tmp, a->displays[i], 32);
                memcpy(a->displays[i], a->displays[j], 32);
                memcpy(a->displays[j], tmp, 32);
            }
        }
    }
    return a->ndisplays;
}

int fr_xconnect(App *a) {
    if (a->x) {
        x_disconnect(a->x);
        free(a->x);
        a->x = NULL;
    }
    a->atoms_ready = 0;
    a->our_win = 0;
    a->gc = 0;
    if (!a->display[0]) {
        set_status(a, 2, "No display name.");
        return -1;
    }
    XConn *c = calloc(1, sizeof *c);
    if (!c) {
        set_status(a, 2, "Out of memory.");
        return -1;
    }
    if (x_connect(c, a->display) < 0) {
        set_status(a, 2, c->err[0] ? c->err : "Could not connect to the display.");
        free(c);
        return -1;
    }
    a->x = c;
    ensure_atoms(a);
    read_root_meta(a);
    if (!a->screen_w[0]) snprintf(a->screen_w, sizeof a->screen_w, "%d", a->root_w);
    if (!a->screen_h[0]) snprintf(a->screen_h, sizeof a->screen_h, "%d", a->root_h);
    return 0;
}

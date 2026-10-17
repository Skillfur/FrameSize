#define _GNU_SOURCE
#include "engine.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
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
static void fill_atoms(XConn *c, Atoms *atom) {
    if (!c || c->fd < 0 || c->broken) return;
    atom->net_wm_name = x_intern(c, "_NET_WM_NAME");
    atom->utf8 = x_intern(c, "UTF8_STRING");
    atom->pid = x_intern(c, "_NET_WM_PID");
    atom->state = x_intern(c, "_NET_WM_STATE");
    atom->fullscreen = x_intern(c, "_NET_WM_STATE_FULLSCREEN");
    atom->moveresize = x_intern(c, "_NET_MOVERESIZE_WINDOW");
    atom->mode = x_intern(c, "GAMESCOPE_XWAYLAND_MODE_CONTROL");
    atom->server = x_intern(c, "GAMESCOPE_XWAYLAND_SERVER_ID");
    atom->scale = x_intern(c, "STEAM_SCREEN_SCALE");
    atom->protocols = x_intern(c, "WM_PROTOCOLS");
    atom->wm_delete = x_intern(c, "WM_DELETE_WINDOW");
    atom->wintype = x_intern(c, "_NET_WM_WINDOW_TYPE");
    atom->wintype_normal = x_intern(c, "_NET_WM_WINDOW_TYPE_NORMAL");
}
int ensure_atoms(App *a) {
    if (a->atoms_ready) return 0;
    fill_atoms(a->x, &a->atom);
    a->atoms_ready = 1;
    return 0;
}
/* Connection that scan/resize talks to. The GUI connection stays put. */
static XConn *srv(App *a) {
    return a->ctl ? a->ctl : a->x;
}
static Atoms *atm(App *a) {
    if (a->ctl && a->ctl != a->x) return &a->ctl_atom;
    return &a->atom;
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
    XConn *sx = srv(a);
    Atoms *at = atm(a);
    if (!sx || !at->moveresize) return;
    uint8_t ev[32];
    memset(ev, 0, sizeof ev);
    ev[0] = 33;
    ev[1] = 32;
    put32(ev + 4, win);
    put32(ev + 8, at->moveresize);
    uint32_t flags = 10u | (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) | (2u << 12);
    put32(ev + 12, flags);
    put32(ev + 16, (uint32_t)x);
    put32(ev + 20, (uint32_t)y);
    put32(ev + 24, (uint32_t)w);
    put32(ev + 28, (uint32_t)h);
    x_send_event(sx, sx->root, 0x00180000u, ev);
}
static void send_unfullscreen(App *a, uint32_t win) {
    XConn *sx = srv(a);
    Atoms *at = atm(a);
    if (!sx || !at->state || !at->fullscreen) return;
    uint8_t ev[32];
    memset(ev, 0, sizeof ev);
    ev[0] = 33;
    ev[1] = 32;
    put32(ev + 4, win);
    put32(ev + 8, at->state);
    put32(ev + 12, 0); /* _NET_WM_STATE_REMOVE */
    put32(ev + 16, at->fullscreen);
    put32(ev + 20, 0);
    put32(ev + 24, 2); /* pager */
    x_send_event(sx, sx->root, 0x00180000u, ev);
}
int apply_size(App *a, WinInfo *wi, int w, int h, int clear_fs, int do_child, char *msg, size_t msgsz) {
    XConn *sx = srv(a);
    if (!sx) {
        snprintf(msg, msgsz, "Not connected.");
        return -1;
    }
    if (w < 1 || h < 1 || w > 7680 || h > 7680) {
        snprintf(msg, msgsz, "Size must be between 1 and 7680.");
        return -1;
    }
    if (clear_fs) send_unfullscreen(a, wi->id);
    if (write_fixed_hints(sx, wi->id, w, h) < 0) {
        snprintf(msg, msgsz, "Could not write WM_NORMAL_HINTS (%s).", sx->err);
        return -1;
    }
    if (x_configure(sx, wi->id, 0x000c, 0, 0, w, h) < 0) {
        snprintf(msg, msgsz, "ConfigureWindow failed (%s).", sx->err);
        return -1;
    }
    send_moveresize(a, wi->id, wi->x, wi->y, w, h);
    int touched_child = 0;
    if (do_child && wi->child && wi->child_w > 1 && wi->child_h > 1 &&
        (wi->child_w >= wi->w / 2 || wi->child_w >= 200) && wi->child_h >= 100) {
        write_fixed_hints(sx, wi->child, w, h);
        x_configure(sx, wi->child, 0x000f, 0, 0, w, h);
        touched_child = 1;
    }
    int gw = 0, gh = 0, gx = 0, gy = 0;
    int matched = 0;
    for (int i = 0; i < 12; i++) {
        XEvent ev;
        x_poll(sx, &ev, 40);
        if (x_geometry(sx, wi->id, &gx, &gy, &gw, &gh) == 0 && gw == w && gh == h) {
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
    XConn *sx = srv(a);
    Atoms *at = atm(a);
    if (!sx) {
        snprintf(msg, msgsz, "Not connected.");
        return;
    }
    if (w < 1 || h < 1 || w > 7680 || h > 7680) {
        snprintf(msg, msgsz, "Screen size must be between 1 and 7680.");
        return;
    }
    uint8_t raw[16];
    put32(raw + 0, a->server_id);
    put32(raw + 4, (uint32_t)w);
    put32(raw + 8, (uint32_t)h);
    put32(raw + 12, super ? 1u : 0u);
    if (x_change_property(sx, sx->root, at->mode, XA_CARDINAL_, 32, raw, 4) < 0) {
        snprintf(msg, msgsz, "Could not set GAMESCOPE_XWAYLAND_MODE_CONTROL (%s).", sx->err);
        return;
    }
    snprintf(msg, msgsz, "Asked Xwayland server %u to become %dx%d. That is the virtual screen, not one window's scale.",
             a->server_id, w, h);
}
static int window_is_ours(App *a, uint32_t id, int pid) {
    if (srv(a) == a->x && id == a->our_win) return 1;
    if (pid > 0 && pid == (int)getpid()) return 1;
    return 0;
}
void read_root_meta(App *a) {
    XConn *sx = srv(a);
    Atoms *at = atm(a);
    if (!sx) return;
    a->root_w = sx->root_w;
    a->root_h = sx->root_h;
    int x, y, w, h;
    if (x_geometry(sx, sx->root, &x, &y, &w, &h) == 0) {
        a->root_w = w;
        a->root_h = h;
    }
    a->server_id = 0;
    a->have_scale = 0;
    uint8_t *data = NULL;
    uint32_t n = 0;
    uint8_t fmt = 0;
    uint32_t type = 0;
    if (at->server &&
        x_property(sx, sx->root, at->server, &data, &n, &fmt, &type) == 0 &&
        data && fmt == 32 && n >= 1) {
        a->server_id = get32(data);
    }
    free(data);
    data = NULL;
    if (at->scale &&
        x_property(sx, sx->root, at->scale, &data, &n, &fmt, &type) == 0 &&
        data && fmt == 32 && n >= 1) {
        uint32_t v = get32(data);
        a->screen_scale = (double)v / (double)0xffffffffu;
        a->have_scale = 1;
    }
    free(data);
}
int scan_windows(App *a) {
    XConn *sx = srv(a);
    Atoms *at = atm(a);
    if (!sx || sx->broken || sx->fd < 0) return -1;
    uint32_t keep = 0;
    if (a->selected >= 0 && a->selected < a->nwins) keep = a->wins[a->selected].id;
    uint32_t *ch = NULL;
    int n = 0;
    if (x_query_tree(sx, sx->root, &ch, &n) < 0) return -1;
    WinInfo tmp[96];
    int nt = 0;
    for (int i = n - 1; i >= 0 && nt < 96; i--) {
        uint32_t id = ch[i];
        int map_state = 0, override = 0, klass = 0;
        if (x_attributes(sx, id, &map_state, &override, &klass) < 0) continue;
        if (!getenv("FR_ALL") && (map_state != 2 || klass == 2)) continue;
        if (getenv("FR_ALL")) fprintf(stderr, "child 0x%x map %d class %d\n", id, map_state, klass);
        int x, y, w, h;
        if (x_geometry(sx, id, &x, &y, &w, &h) < 0) continue;
        if (w < 2 || h < 2) continue;
        uint8_t *data = NULL;
        uint32_t ni = 0;
        uint8_t fmt = 0;
        uint32_t type = 0;
        int pid = 0;
        if (at->pid && x_property(sx, id, at->pid, &data, &ni, &fmt, &type) == 0 &&
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
        read_hints(sx, id, &wi->hint_fixed, &wi->min_w, &wi->min_h, &wi->max_w, &wi->max_h);

        data = NULL;
        if (at->net_wm_name &&
            x_property(sx, id, at->net_wm_name, &data, &ni, &fmt, &type) == 0 &&
            data && fmt == 8 && ni) {
            copy_clean(wi->title, sizeof wi->title, data, ni);
        }
        free(data);
        if (!wi->title[0]) {
            data = NULL;
            if (x_property(sx, id, XA_WM_NAME_, &data, &ni, &fmt, &type) == 0 && data && fmt == 8 && ni)
                copy_clean(wi->title, sizeof wi->title, data, ni);
            free(data);
        }
        if (!wi->title[0]) snprintf(wi->title, sizeof wi->title, "(untitled)");

        data = NULL;
        if (x_property(sx, id, XA_WM_CLASS_, &data, &ni, &fmt, &type) == 0 && data && fmt == 8 && ni) {
            const char *inst = (const char *)data;
            size_t L = strnlen(inst, ni);
            const char *klass_s = (L + 1 < ni) ? inst + L + 1 : inst;
            size_t remain = ni - (size_t)(klass_s - inst);
            copy_clean(wi->cls, sizeof wi->cls, (const uint8_t *)klass_s, remain);
        }
        free(data);

        data = NULL;
        if (at->state &&
            x_property(sx, id, at->state, &data, &ni, &fmt, &type) == 0 &&
            data && fmt == 32) {
            for (uint32_t k = 0; k < ni; k++) {
                if (get32(data + k * 4) == at->fullscreen) wi->fullscreen = 1;
            }
        }
        free(data);

        uint32_t *kids = NULL;
        int nk = 0;
        if (x_query_tree(sx, id, &kids, &nk) == 0 && kids) {
            int best = 0;
            for (int k = 0; k < nk; k++) {
                int ms = 0, ov = 0, kl = 0;
                if (x_attributes(sx, kids[k], &ms, &ov, &kl) < 0) continue;
                if (kl == 2) continue;
                int cx, cy, cw, chh;
                if (x_geometry(sx, kids[k], &cx, &cy, &cw, &chh) < 0) continue;
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
    if (a->ctl == a->x) a->ctl = NULL;
    if (a->x) {
        x_disconnect(a->x);
        free(a->x);
        a->x = NULL;
    }
    a->atoms_ready = 0;
    a->our_win = 0;
    a->gc = 0;
    a->pixmap = 0;
    a->pixmap_w = 0;
    a->pixmap_h = 0;
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
        x_disconnect(c);
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

static int connect_once(const char *display, const char *auth, XConn **out, char *err, size_t errn) {
    XConn *c = calloc(1, sizeof *c);
    if (!c) {
        snprintf(err, errn, "Out of memory.");
        return -1;
    }
    int rc = x_connect_auth(c, display, auth);
    if (rc == 0) {
        *out = c;
        return 0;
    }
    snprintf(err, errn, "%s", c->err[0] ? c->err : "Could not connect.");
    x_disconnect(c);
    free(c);
    return -1;
}

static int display_num_of(const char *display) {
    const char *colon = display ? strrchr(display, ':') : NULL;
    if (!colon) return -1;
    return atoi(colon + 1);
}

static int auth_path_ok(const char *p) {
    if (!p || p[0] != '/' || strlen(p) > 400) return 0;
    if (strstr(p, "..")) return 0;
    return 1;
}

/* Nested Plasma on the Frame keeps its cookie in
   $XDG_RUNTIME_DIR/nested_plasma (DISPLAY=:2). A Game Mode process does not
   inherit that environment, so the ambient XAUTHORITY cannot open :2. */
static int proc_authority(const char *display, char *best, size_t bestn) {
    int want = display_num_of(display);
    if (want < 0) return -1;
    DIR *d = opendir("/proc");
    if (!d) return -1;
    char *buf = malloc(131072);
    if (!buf) { closedir(d); return -1; }
    struct dirent *de;
    int score_best = 0;
    int looked = 0;
    best[0] = 0;
    while ((de = readdir(d)) && looked < 600) {
        if (de->d_name[0] < '1' || de->d_name[0] > '9') continue;
        looked++;
        char envpath[320];
        snprintf(envpath, sizeof envpath, "/proc/%s/environ", de->d_name);
        FILE *f = fopen(envpath, "rb");
        if (!f) continue;
        size_t n = fread(buf, 1, 131072, f);
        fclose(f);
        const char *disp = NULL, *auth = NULL;
        size_t i = 0;
        while (i < n) {
            size_t len = strnlen(buf + i, n - i);
            if (i + len >= n) break;
            const char *ent = buf + i;
            if (strncmp(ent, "DISPLAY=", 8) == 0) disp = ent + 8;
            else if (strncmp(ent, "XAUTHORITY=", 11) == 0) auth = ent + 11;
            i += len + 1;
        }
        if (disp && auth && auth_path_ok(auth) && display_num_of(disp) == want &&
            access(auth, R_OK) == 0 && x_authority_has_cookie(auth, display)) {
            int score = strstr(auth, "nested_plasma") ? 2 : 1;
            if (score > score_best) {
                snprintf(best, bestn, "%s", auth);
                score_best = score;
                if (score == 2) break;
            }
        }
    }
    free(buf);
    closedir(d);
    return best[0] ? 0 : -1;
}

static int search_dir_auth(const char *dir, const char *display, XConn **out, char *err, size_t errn) {
    if (!dir || !dir[0]) return -1;
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *de;
    int tried = 0;
    while ((de = readdir(d)) && tried < 16) {
        const char *nm = de->d_name;
        if (!strstr(nm, "xauth") && !strstr(nm, "Xauth") && !strstr(nm, "Xwayland"))
            continue;
        char path[512];
        int nw = snprintf(path, sizeof path, "%s/%s", dir, nm);
        if (nw < 0 || (size_t)nw >= sizeof path) continue;
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (!x_authority_has_cookie(path, display)) continue;
        tried++;
        if (connect_once(display, path, out, err, errn) == 0) {
            closedir(d);
            return 0;
        }
    }
    closedir(d);
    return -1;
}

static int connect_display(const char *display, XConn **out, char *err, size_t errn) {
    err[0] = 0;
    const char *orig = getenv("XAUTHORITY");
    char homeauth[512];
    homeauth[0] = 0;
    const char *home = getenv("HOME");
    if (home && home[0]) snprintf(homeauth, sizeof homeauth, "%s/.Xauthority", home);

    int ambient_has = 0;
    if (orig && orig[0] && x_authority_has_cookie(orig, display)) {
        ambient_has = 1;
        if (connect_once(display, orig, out, err, errn) == 0) return 0;
    } else if ((!orig || !orig[0]) && homeauth[0] && x_authority_has_cookie(homeauth, display)) {
        ambient_has = 1;
        if (connect_once(display, homeauth, out, err, errn) == 0) return 0;
    }

    char procauth[512];
    if (proc_authority(display, procauth, sizeof procauth) == 0) {
        if ((!orig || strcmp(orig, procauth) != 0) &&
            connect_once(display, procauth, out, err, errn) == 0)
            return 0;
    }

    if (homeauth[0] && (!orig || strcmp(orig, homeauth) != 0) &&
        x_authority_has_cookie(homeauth, display) &&
        connect_once(display, homeauth, out, err, errn) == 0)
        return 0;

    char dirs[4][512];
    int nd = 0;
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt && rt[0]) {
        snprintf(dirs[nd++], 512, "%s/nested_plasma", rt);
        snprintf(dirs[nd++], 512, "%s", rt);
    }
    snprintf(dirs[nd++], 512, "/run/user/%u/nested_plasma", (unsigned)getuid());
    for (int i = 0; i < nd; i++) {
        if (search_dir_auth(dirs[i], display, out, err, errn) == 0) return 0;
    }

    /* Gamescope's Xwayland often accepts a local client with no cookie.
       Do this last, and only when nothing we already have names this display,
       so a Game Mode launch does not open KWin :2 with an empty handshake. */
    if (!ambient_has && connect_once(display, NULL, out, err, errn) == 0) return 0;
    if (!err[0]) snprintf(err, errn, "no authority for this display");
    return -1;
}

static void adopt_server(App *a) {
    read_root_meta(a);
    snprintf(a->screen_w, sizeof a->screen_w, "%d", a->root_w > 0 ? a->root_w : 1280);
    snprintf(a->screen_h, sizeof a->screen_h, "%d", a->root_h > 0 ? a->root_h : 720);
}

void fr_drop_foreign(App *a) {
    if (!a) return;
    if (a->x && a->ctl && a->x->yield == a->ctl) a->x->yield = NULL;
    if (a->ctl && a->ctl != a->x) {
        if (a->ctl->yield == a->x) a->ctl->yield = NULL;
        x_disconnect(a->ctl);
        free(a->ctl);
    }
    a->ctl = a->x;
    if (a->x) a->ctl_atom = a->atom;
    else memset(&a->ctl_atom, 0, sizeof a->ctl_atom);
    if (a->home_display[0])
        snprintf(a->display, sizeof a->display, "%s", a->home_display);
}

static void link_yield(App *a) {
    if (!a->x || !a->ctl || a->ctl == a->x) {
        if (a->x) a->x->yield = NULL;
        return;
    }
    a->ctl->yield = a->x;
    a->x->yield = a->ctl;
}

int fr_retarget(App *a, const char *display) {
    if (!a->x || !display || !display[0]) {
        set_status(a, 2, "Not connected.");
        return -1;
    }
    if (a->home_display[0] && strcmp(display, a->home_display) == 0) {
        fr_drop_foreign(a);
        adopt_server(a);
        return 0;
    }
    char err[160];
    err[0] = 0;
    XConn *c = NULL;
    if (connect_display(display, &c, err, sizeof err) < 0) {
        char msg[320];
        snprintf(msg, sizeof msg, "Could not open %s (%s). Staying on %s.",
                 display, err[0] ? err : "connection failed",
                 a->display[0] ? a->display : a->home_display);
        set_status(a, 2, msg);
        return -1;
    }
    if (a->x && a->ctl && a->x->yield == a->ctl) a->x->yield = NULL;
    if (a->ctl && a->ctl != a->x) {
        if (a->ctl->yield == a->x) a->ctl->yield = NULL;
        x_disconnect(a->ctl);
        free(a->ctl);
    }
    a->ctl = c;
    link_yield(a);
    memset(&a->ctl_atom, 0, sizeof a->ctl_atom);
    fill_atoms(c, &a->ctl_atom);
    if (c->broken || c->fd < 0) {
        snprintf(a->display, sizeof a->display, "%s", display);
        fr_drop_foreign(a);
        set_status(a, 2, "That server closed the connection. Staying on the original.");
        return -1;
    }
    snprintf(a->display, sizeof a->display, "%s", display);
    adopt_server(a);
    return 0;
}

static void test_pu16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void test_pu32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static int test_readn(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    size_t off = 0;
    while (off < n) {
        ssize_t r = read(fd, p + off, n - off);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return -1;
        off += (size_t)r;
    }
    return 0;
}
static int test_writen(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, p + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) return -1;
        off += (size_t)w;
    }
    return 0;
}
static int test_send_setup(int fd) {
    uint8_t extra[128];
    memset(extra, 0, sizeof extra);
    test_pu32(extra + 4, 0x00200000);
    test_pu32(extra + 8, 0x001fffff);
    test_pu16(extra + 18, 65535);
    extra[20] = 1;
    extra[21] = 1;
    extra[25] = 32;
    extra[26] = 8;
    extra[27] = 255;
    size_t off = 32;
    extra[off] = 24;
    extra[off + 1] = 32;
    extra[off + 2] = 32;
    off += 8;
    test_pu32(extra + off, 0x123);
    test_pu32(extra + off + 8, 0xffffff);
    test_pu16(extra + off + 20, 1280);
    test_pu16(extra + off + 22, 800);
    test_pu32(extra + off + 32, 0x21);
    extra[off + 38] = 24;
    extra[off + 39] = 1;
    off += 40;
    extra[off] = 24;
    test_pu16(extra + off + 2, 1);
    off += 8;
    test_pu32(extra + off, 0x21);
    extra[off + 4] = 4;
    extra[off + 5] = 32;
    test_pu16(extra + off + 6, 256);
    test_pu32(extra + off + 8, 0x00ff0000);
    test_pu32(extra + off + 12, 0x0000ff00);
    test_pu32(extra + off + 16, 0x000000ff);
    off += 24;
    uint8_t hdr[8];
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 1;
    test_pu16(hdr + 2, 11);
    test_pu16(hdr + 6, (uint16_t)(off / 4));
    if (test_writen(fd, hdr, 8) < 0) return -1;
    return test_writen(fd, extra, off);
}
static void test_reply(int fd, uint16_t seq, uint8_t kind) {
    uint8_t pkt[32];
    memset(pkt, 0, sizeof pkt);
    pkt[0] = 1;
    test_pu16(pkt + 2, seq);
    if (kind == 16) test_pu32(pkt + 8, 1);
    if (kind == 14) {
        test_pu16(pkt + 16, 1280);
        test_pu16(pkt + 18, 800);
    }
    if (kind == 3) {
        test_pu16(pkt + 12, 1);
        pkt[26] = 2;
    }
    test_writen(fd, pkt, 32);
}
static int test_handshake(int fd, const uint8_t *cookie, int require) {
    uint8_t hdr[12];
    if (test_readn(fd, hdr, 12) < 0) return -1;
    uint16_t nlen = (uint16_t)(hdr[6] | (hdr[7] << 8));
    uint16_t dlen = (uint16_t)(hdr[8] | (hdr[9] << 8));
    size_t npad = (4 - (nlen & 3)) & 3;
    size_t dpad = (4 - (dlen & 3)) & 3;
    size_t rest = (size_t)nlen + npad + dlen + dpad;
    uint8_t *body = calloc(1, rest + 1);
    if (!body) return -1;
    if (rest && test_readn(fd, body, rest) < 0) { free(body); return -1; }
    int ok = 1;
    if (require) ok = dlen == 16 && memcmp(body + nlen + npad, cookie, 16) == 0;
    free(body);
    if (!ok) {
        uint8_t pkt[12];
        memset(pkt, 0, sizeof pkt);
        pkt[1] = 4;
        pkt[2] = 11;
        pkt[6] = 1;
        memcpy(pkt + 8, "nope", 4);
        test_writen(fd, pkt, 12);
        return -1;
    }
    return test_send_setup(fd);
}
static void test_serve_conn(int fd, const uint8_t *cookie, int require) {
    if (test_handshake(fd, cookie, require) < 0) return;
    uint16_t seq = 0;
    for (;;) {
        uint8_t hdr[4];
        if (test_readn(fd, hdr, 4) < 0) return;
        uint16_t len = (uint16_t)(hdr[2] | (hdr[3] << 8));
        if (len < 1) return;
        if (len > 1) {
            uint8_t *rest = malloc((size_t)(len - 1) * 4);
            if (!rest) return;
            if (test_readn(fd, rest, (size_t)(len - 1) * 4) < 0) { free(rest); return; }
            free(rest);
        }
        seq++;
        uint8_t op = hdr[0];
        if (op == 16 || op == 14 || op == 3 || op == 15 || op == 20 || op == 101)
            test_reply(fd, seq, op);
    }
}
static int test_listen(int num, int ready_fd) {
    char path[64];
    snprintf(path, sizeof path, "/tmp/.X11-unix/X%d", num);
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un un;
    memset(&un, 0, sizeof un);
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof un.sun_path, "%s", path);
    if (bind(fd, (struct sockaddr *)&un, sizeof un) < 0) { close(fd); return -1; }
    if (listen(fd, 8) < 0) { close(fd); unlink(path); return -1; }
    char ready = 1;
    if (write(ready_fd, &ready, 1) != 1) { close(fd); unlink(path); return -1; }
    return fd;
}
static void test_server_main(int num, const uint8_t *cookie, int require, int silent, int ready_fd) {
    int lfd = test_listen(num, ready_fd);
    if (lfd < 0) _exit(1);
    close(ready_fd);
    if (silent) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd >= 0) {
            for (;;) pause();
        }
        _exit(0);
    }
    for (;;) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            _exit(0);
        }
        test_serve_conn(cfd, cookie, require);
        close(cfd);
    }
}
static pid_t test_spawn(int num, const uint8_t *cookie, int require, int silent) {
    int sp[2];
    if (pipe(sp) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(sp[0]); close(sp[1]); return -1; }
    if (pid == 0) {
        close(sp[0]);
        test_server_main(num, cookie, require, silent, sp[1]);
        _exit(0);
    }
    close(sp[1]);
    char b = 0;
    ssize_t n = read(sp[0], &b, 1);
    close(sp[0]);
    if (n != 1) { kill(pid, SIGTERM); return -1; }
    return pid;
}
static int test_free_display(void) {
    for (int n = 80; n < 99; n++) {
        char path[64];
        snprintf(path, sizeof path, "/tmp/.X11-unix/X%d", n);
        if (access(path, F_OK) != 0) return n;
    }
    return -1;
}
static void test_write_xauth(const char *path, int dpy, const uint8_t cookie[16]) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    unsigned char b[2];
    b[0] = 255; b[1] = 255; fwrite(b, 1, 2, f);
    b[0] = 0; b[1] = 0; fwrite(b, 1, 2, f);
    char num[16];
    snprintf(num, sizeof num, "%d", dpy);
    b[0] = 0; b[1] = (unsigned char)strlen(num); fwrite(b, 1, 2, f);
    fwrite(num, 1, strlen(num), f);
    const char *name = "MIT-MAGIC-COOKIE-1";
    b[0] = 0; b[1] = 18; fwrite(b, 1, 2, f);
    fwrite(name, 1, 18, f);
    b[0] = 0; b[1] = 16; fwrite(b, 1, 2, f);
    fwrite(cookie, 1, 16, f);
    fclose(f);
}
static void test_fail(const char *msg) {
    fprintf(stderr, "foreign-auth: %s\n", msg);
}

int fr_test_foreign(void) {
    int games = test_free_display();
    int desktop = games > 0 ? test_free_display() : -1;
    /* test_free_display returns the first free number, so call again after we
       reserve the socket by spawning. Pick three numbers up front. */
    int n0 = -1, n1 = -1, n2 = -1;
    for (int n = 80; n < 99 && n2 < 0; n++) {
        char path[64];
        snprintf(path, sizeof path, "/tmp/.X11-unix/X%d", n);
        if (access(path, F_OK) == 0) continue;
        if (n0 < 0) n0 = n;
        else if (n1 < 0) n1 = n;
        else n2 = n;
    }
    (void)games; (void)desktop;
    if (n0 < 0 || n1 < 0 || n2 < 0) { test_fail("no free display numbers"); return 1; }

    uint8_t cookie[16];
    for (int i = 0; i < 16; i++) cookie[i] = (uint8_t)(0xA0 + i);
    char tmp[] = "/tmp/fr-auth-XXXXXX";
    if (!mkdtemp(tmp)) { test_fail("mkdtemp"); return 1; }
    char nested[512], authpath[512];
    snprintf(nested, sizeof nested, "%s/nested_plasma", tmp);
    if (mkdir(nested, 0700) < 0) { test_fail("mkdir"); return 1; }
    snprintf(authpath, sizeof authpath, "%s/xauth_kwin", nested);
    test_write_xauth(authpath, n1, cookie);

    char *old_rt = getenv("XDG_RUNTIME_DIR") ? strdup(getenv("XDG_RUNTIME_DIR")) : NULL;
    char *old_auth = getenv("XAUTHORITY") ? strdup(getenv("XAUTHORITY")) : NULL;
    char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    setenv("XDG_RUNTIME_DIR", tmp, 1);
    setenv("HOME", tmp, 1);
    unsetenv("XAUTHORITY");

    pid_t p0 = test_spawn(n0, NULL, 0, 0);
    pid_t p1 = test_spawn(n1, cookie, 1, 0);
    pid_t p2 = test_spawn(n2, NULL, 0, 1);
    int rc = 1;
    App a;
    memset(&a, 0, sizeof a);
    if (p0 < 0 || p1 < 0 || p2 < 0) {
        test_fail("spawn");
        goto done;
    }
    snprintf(a.display, sizeof a.display, ":%d", n0);
    if (fr_xconnect(&a) < 0) {
        fprintf(stderr, "foreign-auth: home connect: %s\n", a.status);
        goto done;
    }
    snprintf(a.home_display, sizeof a.home_display, "%s", a.display);
    a.ctl = a.x;
    a.ctl_atom = a.atom;

    char tgt[16];
    snprintf(tgt, sizeof tgt, ":%d", n1);
    if (fr_retarget(&a, tgt) != 0) {
        fprintf(stderr, "foreign-auth: desktop retarget: %s\n", a.status);
        goto done;
    }
    if (!a.ctl || a.ctl == a.x) { test_fail("ctl was not a second connection"); goto done; }
    if (scan_windows(&a) != 0) { test_fail("scan desktop"); goto done; }
    int gx = 0, gy = 0, gw = 0, gh = 0;
    if (x_geometry(a.x, a.x->root, &gx, &gy, &gw, &gh) != 0 || gw != 1280) {
        test_fail("home connection died while listing desktop");
        goto done;
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    snprintf(tgt, sizeof tgt, ":%d", n2);
    int stalled = fr_retarget(&a, tgt);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    if (stalled == 0) { test_fail("silent server was accepted"); goto done; }
    if (dt > 8.0) {
        fprintf(stderr, "foreign-auth: silent server blocked for %.2fs\n", dt);
        goto done;
    }
    if (x_geometry(a.x, a.x->root, &gx, &gy, &gw, &gh) != 0) {
        test_fail("home connection died after a failed retarget");
        goto done;
    }
    if (strcmp(a.display, a.home_display) == 0) {
        test_fail("failed retarget rewound the listing");
        goto done;
    }
    fr_drop_foreign(&a);
    if (a.ctl != a.x || strcmp(a.display, a.home_display) != 0) {
        test_fail("drop foreign");
        goto done;
    }
    printf("foreign-auth ok (fail path %.2fs)\n", dt);
    rc = 0;
done:
    if (a.ctl && a.ctl != a.x) {
        x_disconnect(a.ctl);
        free(a.ctl);
        a.ctl = NULL;
    }
    if (a.x) { x_disconnect(a.x); free(a.x); a.x = NULL; }
    if (p0 > 0) kill(p0, SIGTERM);
    if (p1 > 0) kill(p1, SIGTERM);
    if (p2 > 0) kill(p2, SIGTERM);
    if (p0 > 0) waitpid(p0, NULL, 0);
    if (p1 > 0) waitpid(p1, NULL, 0);
    if (p2 > 0) waitpid(p2, NULL, 0);
    char sock[64];
    snprintf(sock, sizeof sock, "/tmp/.X11-unix/X%d", n0); unlink(sock);
    snprintf(sock, sizeof sock, "/tmp/.X11-unix/X%d", n1); unlink(sock);
    snprintf(sock, sizeof sock, "/tmp/.X11-unix/X%d", n2); unlink(sock);
    unlink(authpath);
    rmdir(nested);
    rmdir(tmp);
    if (old_rt) { setenv("XDG_RUNTIME_DIR", old_rt, 1); free(old_rt); }
    else unsetenv("XDG_RUNTIME_DIR");
    if (old_auth) { setenv("XAUTHORITY", old_auth, 1); free(old_auth); }
    else unsetenv("XAUTHORITY");
    if (old_home) { setenv("HOME", old_home, 1); free(old_home); }
    return rc;
}

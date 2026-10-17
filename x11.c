#define _GNU_SOURCE
#include "x11.h"

#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>

static void set_err(XConn *c, const char *msg) {
    snprintf(c->err, sizeof c->err, "%s", msg);
}

static int write_full(int fd, const void *buf, size_t n) {
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

static int read_full(int fd, void *buf, size_t n) {
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

/* Pull every byte already queued by the server into c->in. Safe on a blocking
   socket because it polls with timeout 0 before reading. */
static int buffer_readable(XConn *c) {
    for (;;) {
        struct pollfd pfd = { .fd = c->fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 0);
        if (pr < 0) {
            if (errno == EINTR) continue;
            set_err(c, "poll failed");
            return -1;
        }
        if (pr == 0) return 0;
        if (c->in_len + 8192 > c->in_cap) {
            size_t cap = c->in_cap ? c->in_cap * 2 : 16384;
            while (cap < c->in_len + 8192) cap *= 2;
            uint8_t *nbuf = realloc(c->in, cap);
            if (!nbuf) return -1;
            c->in = nbuf;
            c->in_cap = cap;
        }
        ssize_t r = read(c->fd, c->in + c->in_len, c->in_cap - c->in_len);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            set_err(c, "read failed");
            return -1;
        }
        if (r == 0) {
            set_err(c, "display connection closed");
            return -1;
        }
        c->in_len += (size_t)r;
    }
}

/* A blocking PutImage fills the send buffer while Gamescope is still writing
   motion and ConfigureNotify events. If those are not read, both sides stall
   and clicks fall further behind. Drain input between every write. */
static int write_request(XConn *c, const void *buf, size_t n) {
    const uint8_t *p = buf;
    size_t off = 0;
    while (off < n) {
        if (buffer_readable(c) < 0) return -1;
        struct pollfd pfd = { .fd = c->fd, .events = POLLOUT | POLLIN };
        int pr = poll(&pfd, 1, 5000);
        if (pr < 0) {
            if (errno == EINTR) continue;
            set_err(c, "poll failed");
            return -1;
        }
        if (pr == 0) {
            set_err(c, "display write timed out");
            return -1;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            set_err(c, "display connection closed");
            return -1;
        }
        if (pfd.revents & POLLIN) {
            if (buffer_readable(c) < 0) return -1;
        }
        if (!(pfd.revents & POLLOUT)) continue;
        ssize_t w = write(c->fd, p + off, n - off);
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            set_err(c, "write to display failed");
            return -1;
        }
        if (w == 0) {
            set_err(c, "write to display failed");
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

static int ensure_in(XConn *c, size_t need, int timeout_ms) {
    while (c->in_len < need) {
        struct pollfd pfd = { .fd = c->fd, .events = POLLIN };
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            set_err(c, "poll failed");
            return -1;
        }
        if (pr == 0) return 1;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL) && !(pfd.revents & POLLIN)) {
            set_err(c, "display connection closed");
            return -1;
        }
        if (c->in_len + 4096 > c->in_cap) {
            size_t cap = c->in_cap ? c->in_cap * 2 : 8192;
            while (cap < c->in_len + 4096) cap *= 2;
            uint8_t *nbuf = realloc(c->in, cap);
            if (!nbuf) return -1;
            c->in = nbuf;
            c->in_cap = cap;
        }
        ssize_t r = read(c->fd, c->in + c->in_len, c->in_cap - c->in_len);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            set_err(c, "read failed");
            return -1;
        }
        if (r == 0) {
            set_err(c, "display connection closed");
            return -1;
        }
        c->in_len += (size_t)r;
    }
    return 0;
}

static void in_consume(XConn *c, size_t n) {
    if (n >= c->in_len) {
        c->in_len = 0;
        return;
    }
    memmove(c->in, c->in + n, c->in_len - n);
    c->in_len -= n;
}

static int queue_event(XConn *c, const uint8_t raw[32]) {
    if (c->eq_len + 1 > c->eq_cap) {
        int cap = c->eq_cap ? c->eq_cap * 2 : 32;
        XEvent *n = realloc(c->eq, (size_t)cap * sizeof(XEvent));
        if (!n) return -1;
        c->eq = n;
        c->eq_cap = cap;
    }
    XEvent *ev = &c->eq[c->eq_len++];
    memset(ev, 0, sizeof *ev);
    /* SendEvent sets the high bit. Gamescope's close control is a synthetic
       ClientMessage, so the wire type is 33|0x80, not 33. */
    ev->type = raw[0] & 0x7f;
    ev->detail = raw[1];
    ev->seq = (uint16_t)(raw[2] | (raw[3] << 8));
    memcpy(ev->raw, raw, 32);
    return 0;
}

static const char *err_name(uint8_t code) {
    switch (code) {
    case 1: return "BadRequest";
    case 2: return "BadValue";
    case 3: return "BadWindow";
    case 8: return "BadMatch";
    case 10: return "BadAccess";
    case 16: return "BadLength";
    default: return "X error";
    }
}

/* Pull one X packet. timeout_ms < 0 waits forever. Returns 0 packet, 1 timeout, -1 error.
   Reply packets are malloc'd into *reply (caller frees) and *reply_len is the size.
   Events are queued. Errors are recorded. If want_seq != 0, a matching reply/error returns. */
static int pull(XConn *c, uint16_t want_seq, int timeout_ms, uint8_t **reply, size_t *reply_len) {
    for (;;) {
        int st = ensure_in(c, 32, timeout_ms);
        if (st != 0) return st;
        uint8_t type = c->in[0];
        if (type == 1) {
            uint32_t extra = (uint32_t)c->in[4] | ((uint32_t)c->in[5] << 8) |
                             ((uint32_t)c->in[6] << 16) | ((uint32_t)c->in[7] << 24);
            size_t total = 32u + (size_t)extra * 4u;
            st = ensure_in(c, total, timeout_ms < 0 ? 5000 : timeout_ms);
            if (st != 0) return st;
            uint16_t seq = (uint16_t)(c->in[2] | (c->in[3] << 8));
            if (want_seq && seq == want_seq) {
                uint8_t *buf = malloc(total);
                if (!buf) return -1;
                memcpy(buf, c->in, total);
                in_consume(c, total);
                *reply = buf;
                *reply_len = total;
                return 0;
            }
            /* Unexpected reply: drop it. */
            in_consume(c, total);
            continue;
        }
        if (c->in_len < 32) return 1;
        uint8_t raw[32];
        memcpy(raw, c->in, 32);
        in_consume(c, 32);
        uint16_t seq = (uint16_t)(raw[2] | (raw[3] << 8));
        if (type == 0) {
            c->last_error = raw[1];
            c->last_error_seq = seq;
            snprintf(c->last_error_text, sizeof c->last_error_text, "%s (opcode %u)",
                     err_name(raw[1]), raw[10]);
            if (want_seq && seq == want_seq) {
                set_err(c, c->last_error_text);
                return -1;
            }
            continue;
        }
        if (queue_event(c, raw) < 0) return -1;
        if (!want_seq) return 0;
        (void)seq;
    }
}

static int request(XConn *c, uint8_t *req, size_t n, uint8_t **reply, size_t *reply_len, int expect_reply) {
    if (n < 4 || (n & 3)) {
        set_err(c, "bad request size");
        return -1;
    }
    uint16_t len = (uint16_t)(n / 4);
    req[2] = (uint8_t)(len & 255);
    req[3] = (uint8_t)(len >> 8);
    c->seq++;
    uint16_t seq = c->seq;
    if (write_request(c, req, n) < 0) return -1;
    if (!expect_reply) return 0;
    return pull(c, seq, 5000, reply, reply_len);
}

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int parse_display(const char *display, char *host, size_t hostn, int *num) {
    if (!display || !display[0]) display = getenv("DISPLAY");
    if (!display || !display[0]) display = ":0";
    const char *colon = strrchr(display, ':');
    if (!colon) {
        snprintf(host, hostn, "%s", display);
        *num = 0;
        return 0;
    }
    size_t hl = (size_t)(colon - display);
    if (hl >= hostn) hl = hostn - 1;
    memcpy(host, display, hl);
    host[hl] = 0;
    *num = atoi(colon + 1);
    return 0;
}

static int load_cookie(int dpy_num, uint8_t cookie[16]) {
    const char *path = getenv("XAUTHORITY");
    char buf[256];
    if (!path || !path[0]) {
        const char *home = getenv("HOME");
        if (!home) return -1;
        snprintf(buf, sizeof buf, "%s/.Xauthority", home);
        path = buf;
    }
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char num[16];
    snprintf(num, sizeof num, "%d", dpy_num);
    for (;;) {
        unsigned char hdr[4];
        if (fread(hdr, 1, 2, f) != 2) break;
        unsigned family = ((unsigned)hdr[0] << 8) | hdr[1];
        if (fread(hdr, 1, 2, f) != 2) break;
        unsigned alen = ((unsigned)hdr[0] << 8) | hdr[1];
        char *addr = calloc(1, alen + 1);
        if (!addr) { fclose(f); return -1; }
        if (alen && fread(addr, 1, alen, f) != alen) { free(addr); break; }
        if (fread(hdr, 1, 2, f) != 2) { free(addr); break; }
        unsigned nlen = ((unsigned)hdr[0] << 8) | hdr[1];
        char *dnum = calloc(1, nlen + 1);
        if (!dnum) { free(addr); fclose(f); return -1; }
        if (nlen && fread(dnum, 1, nlen, f) != nlen) { free(addr); free(dnum); break; }
        if (fread(hdr, 1, 2, f) != 2) { free(addr); free(dnum); break; }
        unsigned namelen = ((unsigned)hdr[0] << 8) | hdr[1];
        char *name = calloc(1, namelen + 1);
        if (!name) { free(addr); free(dnum); fclose(f); return -1; }
        if (namelen && fread(name, 1, namelen, f) != namelen) {
            free(addr); free(dnum); free(name); break;
        }
        if (fread(hdr, 1, 2, f) != 2) { free(addr); free(dnum); free(name); break; }
        unsigned dlen = ((unsigned)hdr[0] << 8) | hdr[1];
        uint8_t *data = calloc(1, dlen + 1);
        if (!data) { free(addr); free(dnum); free(name); fclose(f); return -1; }
        if (dlen && fread(data, 1, dlen, f) != dlen) {
            free(addr); free(dnum); free(name); free(data); break;
        }
        int match = (strcmp(dnum, num) == 0) &&
                    (family == 256 || family == 65535 || family == 0 || family == 1) &&
                    namelen == 18 && strcmp(name, "MIT-MAGIC-COOKIE-1") == 0 &&
                    dlen == 16;
        if (match) {
            memcpy(cookie, data, 16);
            free(addr); free(dnum); free(name); free(data);
            fclose(f);
            return 0;
        }
        free(addr); free(dnum); free(name); free(data);
    }
    fclose(f);
    return -1;
}

static int connect_tcp(int num, const char *host) {
    unsigned a, b, c0, d;
    const char *h = host;
    if (!h || !h[0] || strcmp(h, "localhost") == 0) h = "127.0.0.1";
    if (sscanf(h, "%u.%u.%u.%u", &a, &b, &c0, &d) != 4) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in in;
    memset(&in, 0, sizeof in);
    in.sin_family = AF_INET;
    in.sin_port = htons((uint16_t)(6000 + num));
    in.sin_addr.s_addr = htonl((a << 24) | (b << 16) | (c0 << 8) | d);
    if (connect(fd, (struct sockaddr *)&in, sizeof in) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int x_connect(XConn *c, const char *display) {
    memset(c, 0, sizeof *c);
    c->fd = -1;
    char host[128];
    int num = 0;
    parse_display(display, host, sizeof host, &num);
    snprintf(c->err, sizeof c->err, "connecting");

    int fd = -1;
    if (!host[0] || strcmp(host, "unix") == 0) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) { set_err(c, "socket failed"); return -1; }
        struct sockaddr_un un;
        memset(&un, 0, sizeof un);
        un.sun_family = AF_UNIX;
        snprintf(un.sun_path, sizeof un.sun_path, "/tmp/.X11-unix/X%d", num);
        if (connect(fd, (struct sockaddr *)&un, sizeof un) < 0) {
            close(fd);
            snprintf(c->err, sizeof c->err, "cannot open /tmp/.X11-unix/X%d", num);
            return -1;
        }
    } else {
        fd = connect_tcp(num, host);
        if (fd < 0) {
            snprintf(c->err, sizeof c->err, "cannot connect to display %s:%d", host, num);
            return -1;
        }
    }
    c->fd = fd;

    uint8_t cookie[16];
    int have = load_cookie(num, cookie) == 0;
    const char *aname = "MIT-MAGIC-COOKIE-1";
    uint16_t nlen = have ? 18 : 0;
    uint16_t dlen = have ? 16 : 0;
    size_t npad = (4 - (nlen & 3)) & 3;
    size_t dpad = (4 - (dlen & 3)) & 3;
    size_t total = 12 + nlen + npad + dlen + dpad;
    uint8_t *hello = calloc(1, total);
    if (!hello) { set_err(c, "out of memory"); return -1; }
    hello[0] = 'l';
    put_u16(hello + 2, 11);
    put_u16(hello + 4, 0);
    put_u16(hello + 6, nlen);
    put_u16(hello + 8, dlen);
    if (have) {
        memcpy(hello + 12, aname, 18);
        memcpy(hello + 12 + nlen + npad, cookie, 16);
    }
    if (write_full(fd, hello, total) < 0) {
        free(hello);
        set_err(c, "handshake write failed");
        return -1;
    }
    free(hello);

    uint8_t hdr[8];
    if (read_full(fd, hdr, 8) < 0) { set_err(c, "handshake read failed"); return -1; }
    if (hdr[0] != 1) {
        uint16_t reason_len = (uint16_t)(hdr[1]);
        (void)reason_len;
        uint16_t add = get_u16(hdr + 6);
        if (add) {
            uint8_t *junk = calloc(1, (size_t)add * 4);
            if (junk) { read_full(fd, junk, (size_t)add * 4); free(junk); }
        }
        set_err(c, hdr[0] == 0 ? "X server refused the connection (auth)" : "unexpected handshake");
        return -1;
    }
    uint16_t add = get_u16(hdr + 6);
    size_t extra_n = (size_t)add * 4;
    uint8_t *extra = calloc(1, extra_n + 1);
    if (!extra) { set_err(c, "out of memory"); return -1; }
    if (read_full(fd, extra, extra_n) < 0) {
        free(extra);
        set_err(c, "setup read failed");
        return -1;
    }
    if (getenv("FR_DEBUG")) {
        fprintf(stderr, "hdr");
        for (int i = 0; i < 8; i++) fprintf(stderr, " %02x", hdr[i]);
        fprintf(stderr, "\nextra %zu:", extra_n);
        size_t m = extra_n < 96 ? extra_n : 96;
        for (size_t i = 0; i < m; i++) {
            if (i % 16 == 0) fprintf(stderr, "\n  ");
            fprintf(stderr, "%02x ", extra[i]);
        }
        fprintf(stderr, "\n");
    }
    if (extra_n < 32) { free(extra); set_err(c, "short setup"); return -1; }
    c->rid_base = get_u32(extra + 4);
    c->rid_mask = get_u32(extra + 8);
    c->rid_next = 1;
    uint16_t vlen = get_u16(extra + 16);
    c->max_req = get_u16(extra + 18);
    uint8_t nroots = extra[20];
    uint8_t nformats = extra[21];
    c->image_lsb = extra[22] == 0;
    c->scanline_pad = extra[25];
    if (c->scanline_pad != 8 && c->scanline_pad != 16 && c->scanline_pad != 32)
        c->scanline_pad = 32;
    c->min_keycode = extra[26];
    c->max_keycode = extra[27];
    size_t off = 32;
    off += vlen;
    off = (off + 3) & ~(size_t)3;
    c->bpp = 32;
    size_t formats_off = off;
    off += (size_t)nformats * 8;
    (void)nroots;

    int screen_index = 0;
    if (display) {
        const char *dot = strrchr(display, '.');
        const char *colon = strrchr(display, ':');
        if (dot && colon && dot > colon) screen_index = atoi(dot + 1);
    }
    int screen_i = 0;
    int found = 0;
    while (off + 40 <= extra_n) {
        uint32_t root = get_u32(extra + off);
        uint32_t cmap = get_u32(extra + off + 4);
        uint32_t white = get_u32(extra + off + 8);
        uint32_t black = get_u32(extra + off + 12);
        (void)cmap;
        uint16_t rw = get_u16(extra + off + 20);
        uint16_t rh = get_u16(extra + off + 22);
        uint32_t visual = get_u32(extra + off + 32);
        uint8_t root_depth = extra[off + 38];
        uint8_t ndepths = extra[off + 39];
        size_t o2 = off + 40;
        uint32_t red = 0xff0000, green = 0x00ff00, blue = 0x0000ff;
        for (uint8_t d = 0; d < ndepths && o2 + 8 <= extra_n; d++) {
            uint8_t dep = extra[o2];
            uint16_t nvis = get_u16(extra + o2 + 2);
            o2 += 8;
            for (uint16_t v = 0; v < nvis && o2 + 24 <= extra_n; v++) {
                uint32_t vid = get_u32(extra + o2);
                if (dep == root_depth && vid == visual) {
                    red = get_u32(extra + o2 + 8);
                    green = get_u32(extra + o2 + 12);
                    blue = get_u32(extra + o2 + 16);
                }
                o2 += 24;
            }
        }
        if (screen_i == screen_index) {
            c->root = root;
            c->white = white;
            c->black = black;
            c->root_w = rw;
            c->root_h = rh;
            c->root_visual = visual;
            c->depth = root_depth;
            c->red_mask = red;
            c->green_mask = green;
            c->blue_mask = blue;
            found = 1;
            off = o2;
            break;
        }
        off = o2;
        screen_i++;
    }
    if (!found) { free(extra); set_err(c, "no screen in setup"); return -1; }

    size_t foff = formats_off;
    for (uint8_t i = 0; i < nformats; i++) {
        if (foff + 8 > extra_n) break;
        if (extra[foff] == c->depth) {
            c->bpp = extra[foff + 1];
            break;
        }
        foff += 8;
    }
    if (c->bpp != 16 && c->bpp != 24 && c->bpp != 32) c->bpp = 32;
    if (getenv("FR_DEBUG")) {
        fprintf(stderr, "setup base=%u mask=%u vlen=%u nfmt=%u depth=%u root=0x%x %ux%u bpp=%u extra=%zu\n",
                c->rid_base, c->rid_mask, vlen, nformats, c->depth, c->root, c->root_w, c->root_h, c->bpp, extra_n);
    }
    free(extra);
    {
        int fl = fcntl(c->fd, F_GETFL, 0);
        if (fl >= 0) fcntl(c->fd, F_SETFL, fl | O_NONBLOCK);
    }
    c->err[0] = 0;
    return 0;
}

void x_disconnect(XConn *c) {
    if (!c) return;
    if (c->fd >= 0) close(c->fd);
    free(c->in);
    free(c->eq);
    free(c->keysyms);
    c->fd = -1;
}

uint32_t x_id(XConn *c) {
    uint32_t id = c->rid_base + c->rid_next;
    c->rid_next++;
    return id;
}

static int expect(XConn *c, uint8_t opcode, uint8_t data, const uint8_t *rest, size_t rest_n, uint8_t **reply, size_t *reply_len) {
    size_t n = 4 + rest_n;
    size_t pad = (4 - (n & 3)) & 3;
    n += pad;
    uint8_t *req = calloc(1, n);
    if (!req) return -1;
    req[0] = opcode;
    req[1] = data;
    if (rest_n) memcpy(req + 4, rest, rest_n);
    int rc = request(c, req, n, reply, reply_len, reply != NULL);
    free(req);
    return rc;
}

uint32_t x_intern(XConn *c, const char *name) {
    size_t len = strlen(name);
    if (len > 65535) len = 65535;
    uint8_t *body = calloc(1, 4 + len);
    if (!body) return 0;
    put_u16(body, (uint16_t)len);
    memcpy(body + 4, name, len);
    uint8_t *reply = NULL;
    size_t rlen = 0;
    int rc = expect(c, 16, 0, body, 4 + len, &reply, &rlen);
    free(body);
    if (rc < 0 || !reply) return 0;
    uint32_t atom = get_u32(reply + 8);
    free(reply);
    return atom;
}

int x_query_tree(XConn *c, uint32_t win, uint32_t **children, int *n) {
    *children = NULL;
    *n = 0;
    uint8_t body[4];
    put_u32(body, win);
    uint8_t *reply = NULL;
    size_t rlen = 0;
    if (expect(c, 15, 0, body, 4, &reply, &rlen) < 0 || !reply) return -1;
    uint16_t nc = get_u16(reply + 16);
    uint32_t *ch = NULL;
    if (nc) {
        ch = calloc(nc, sizeof(uint32_t));
        if (!ch) { free(reply); return -1; }
        for (uint16_t i = 0; i < nc; i++) {
            size_t o = 32 + (size_t)i * 4;
            if (o + 4 <= rlen) ch[i] = get_u32(reply + o);
        }
    }
    free(reply);
    *children = ch;
    *n = (int)nc;
    return 0;
}

int x_geometry(XConn *c, uint32_t win, int *x, int *y, int *w, int *h) {
    uint8_t body[4];
    put_u32(body, win);
    uint8_t *reply = NULL;
    size_t rlen = 0;
    if (expect(c, 14, 0, body, 4, &reply, &rlen) < 0 || !reply || rlen < 20) {
        free(reply);
        return -1;
    }
    *x = (int16_t)get_u16(reply + 12);
    *y = (int16_t)get_u16(reply + 14);
    *w = (int)get_u16(reply + 16);
    *h = (int)get_u16(reply + 18);
    free(reply);
    return 0;
}

int x_attributes(XConn *c, uint32_t win, int *map_state, int *override_redirect, int *klass) {
    uint8_t body[4];
    put_u32(body, win);
    uint8_t *reply = NULL;
    size_t rlen = 0;
    if (expect(c, 3, 0, body, 4, &reply, &rlen) < 0 || !reply || rlen < 28) {
        free(reply);
        return -1;
    }
    *klass = (int)get_u16(reply + 12);
    *map_state = reply[26];
    *override_redirect = reply[27];
    free(reply);
    return 0;
}

int x_property(XConn *c, uint32_t win, uint32_t prop, uint8_t **data, uint32_t *nitems, uint8_t *format, uint32_t *type) {
    *data = NULL;
    *nitems = 0;
    *format = 0;
    *type = 0;
    uint8_t body[20];
    memset(body, 0, sizeof body);
    put_u32(body + 0, win);
    put_u32(body + 4, prop);
    put_u32(body + 8, 0);
    put_u32(body + 12, 0);
    put_u32(body + 16, 0x100000);
    uint8_t *reply = NULL;
    size_t rlen = 0;
    if (expect(c, 20, 0, body, 20, &reply, &rlen) < 0 || !reply || rlen < 32) {
        free(reply);
        return -1;
    }
    *format = reply[1];
    *type = get_u32(reply + 8);
    uint32_t n = get_u32(reply + 16);
    *nitems = n;
    if (*type == 0 || n == 0) {
        free(reply);
        return 0;
    }
    size_t bpi = *format == 32 ? 4 : (*format == 16 ? 2 : 1);
    size_t nbytes = (size_t)n * bpi;
    uint8_t *copy = calloc(1, nbytes + 1);
    if (!copy) { free(reply); return -1; }
    if (rlen > 32) {
        size_t have = rlen - 32;
        if (have > nbytes) have = nbytes;
        memcpy(copy, reply + 32, have);
    }
    free(reply);
    *data = copy;
    return 0;
}

int x_change_property(XConn *c, uint32_t win, uint32_t prop, uint32_t type, uint8_t format, const void *data, uint32_t nitems) {
    size_t bpi = format == 32 ? 4 : (format == 16 ? 2 : 1);
    size_t nbytes = (size_t)nitems * bpi;
    size_t rest = 20 + nbytes;
    uint8_t *body = calloc(1, rest + 4);
    if (!body) return -1;
    put_u32(body + 0, win);
    put_u32(body + 4, prop);
    put_u32(body + 8, type);
    body[12] = format;
    put_u32(body + 16, nitems);
    if (nbytes && data) memcpy(body + 20, data, nbytes);
    int rc = expect(c, 18, 0, body, rest, NULL, NULL);
    free(body);
    return rc;
}

int x_configure(XConn *c, uint32_t win, uint16_t mask, int x, int y, int w, int h) {
    uint8_t body[8 + 16];
    memset(body, 0, sizeof body);
    put_u32(body, win);
    put_u16(body + 4, mask);
    size_t n = 8;
    if (mask & 0x0001) { put_u32(body + n, (uint32_t)(int32_t)x); n += 4; }
    if (mask & 0x0002) { put_u32(body + n, (uint32_t)(int32_t)y); n += 4; }
    if (mask & 0x0004) { put_u32(body + n, (uint32_t)(int32_t)w); n += 4; }
    if (mask & 0x0008) { put_u32(body + n, (uint32_t)(int32_t)h); n += 4; }
    return expect(c, 12, 0, body, n, NULL, NULL);
}

int x_send_event(XConn *c, uint32_t dest, uint32_t mask, const uint8_t event[32]) {
    uint8_t body[40];
    memset(body, 0, sizeof body);
    put_u32(body, dest);
    put_u32(body + 4, mask);
    memcpy(body + 8, event, 32);
    return expect(c, 25, 0, body, 40, NULL, NULL);
}

uint32_t x_create_window(XConn *c, int x, int y, int w, int h, uint32_t event_mask) {
    uint32_t id = x_id(c);
    uint8_t body[36];
    memset(body, 0, sizeof body);
    put_u32(body + 0, id);
    put_u32(body + 4, c->root);
    put_u16(body + 8, (uint16_t)(int16_t)x);
    put_u16(body + 10, (uint16_t)(int16_t)y);
    put_u16(body + 12, (uint16_t)w);
    put_u16(body + 14, (uint16_t)h);
    put_u16(body + 16, 0);
    put_u16(body + 18, 1);
    put_u32(body + 20, 0);
    uint32_t mask = 0x0002 | 0x0800;
    put_u32(body + 24, mask);
    put_u32(body + 28, c->black);
    put_u32(body + 32, event_mask);
    if (expect(c, 1, 0, body, 36, NULL, NULL) < 0) return 0;
    return id;
}

int x_map(XConn *c, uint32_t win) {
    uint8_t body[4];
    put_u32(body, win);
    return expect(c, 8, 0, body, 4, NULL, NULL);
}

int x_destroy(XConn *c, uint32_t win) {
    uint8_t body[4];
    put_u32(body, win);
    return expect(c, 4, 0, body, 4, NULL, NULL);
}

int x_change_attr(XConn *c, uint32_t win, uint32_t mask, uint32_t value) {
    uint8_t body[12];
    put_u32(body, win);
    put_u32(body + 4, mask);
    put_u32(body + 8, value);
    return expect(c, 2, 0, body, 12, NULL, NULL);
}

uint32_t x_create_gc(XConn *c, uint32_t drawable, uint32_t fg) {
    uint32_t id = x_id(c);
    uint8_t body[16];
    put_u32(body, id);
    put_u32(body + 4, drawable);
    put_u32(body + 8, 0x0004);
    put_u32(body + 12, fg);
    if (expect(c, 55, 0, body, 16, NULL, NULL) < 0) return 0;
    return id;
}

static uint32_t spread(uint8_t v, uint32_t mask) {
    if (!mask) return 0;
    unsigned shift = 0;
    while (((mask >> shift) & 1u) == 0) shift++;
    unsigned bits = 0;
    while ((mask >> (shift + bits)) & 1u) bits++;
    uint32_t maxv = (bits >= 31) ? 0xffffffffu : ((1u << bits) - 1u);
    uint32_t scaled = (uint32_t)((v * maxv + 127) / 255);
    return scaled << shift;
}

int x_put_image(XConn *c, uint32_t win, uint32_t gc, int dstx, int dsty, int w, int h, const uint32_t *rgb) {
    if (w <= 0 || h <= 0) return 0;
    unsigned pad_bits = c->scanline_pad ? c->scanline_pad : 32;
    unsigned bpp = c->bpp ? c->bpp : 32;
    size_t stride = ((size_t)w * bpp + pad_bits - 1) / pad_bits * (pad_bits / 8);
    uint32_t max_bytes = c->max_req ? (uint32_t)c->max_req * 4u : 16384u;
    if (max_bytes < 256) max_bytes = 256;
    size_t budget = max_bytes > 64 ? max_bytes - 64 : max_bytes / 2;
    int rows_per = (int)(budget / (stride ? stride : 1));
    if (rows_per < 1) rows_per = 1;
    if (rows_per > h) rows_per = h;

    int fast = c->image_lsb && bpp == 32 &&
               c->red_mask == 0x00ff0000u && c->green_mask == 0x0000ff00u && c->blue_mask == 0x000000ffu &&
               stride == (size_t)w * 4;

    uint8_t *scan = malloc(stride * (size_t)rows_per + 64);
    if (!scan) return -1;

    for (int y0 = 0; y0 < h; ) {
        int rows = h - y0;
        if (rows > rows_per) rows = rows_per;
        memset(scan, 0, stride * (size_t)rows);
        for (int row = 0; row < rows; row++) {
            uint8_t *dst = scan + (size_t)row * stride;
            const uint32_t *src = rgb + (size_t)(y0 + row) * (size_t)w;
            if (fast) {
                memcpy(dst, src, (size_t)w * 4);
                continue;
            }
            for (int col = 0; col < w; col++) {
                uint32_t p = src[col];
                uint32_t pix = spread((p >> 16) & 255, c->red_mask) |
                               spread((p >> 8) & 255, c->green_mask) |
                               spread(p & 255, c->blue_mask);
                uint8_t *px = dst + (size_t)col * (bpp / 8);
                if (bpp == 32) {
                    if (c->image_lsb) put_u32(px, pix);
                    else {
                        px[0] = (uint8_t)(pix >> 24);
                        px[1] = (uint8_t)(pix >> 16);
                        px[2] = (uint8_t)(pix >> 8);
                        px[3] = (uint8_t)pix;
                    }
                } else if (bpp == 24) {
                    if (c->image_lsb) {
                        px[0] = (uint8_t)pix;
                        px[1] = (uint8_t)(pix >> 8);
                        px[2] = (uint8_t)(pix >> 16);
                    } else {
                        px[0] = (uint8_t)(pix >> 16);
                        px[1] = (uint8_t)(pix >> 8);
                        px[2] = (uint8_t)pix;
                    }
                } else if (bpp == 16) {
                    if (c->image_lsb) put_u16(px, (uint16_t)pix);
                    else {
                        px[0] = (uint8_t)(pix >> 8);
                        px[1] = (uint8_t)pix;
                    }
                }
            }
        }
        size_t nbytes = stride * (size_t)rows;
        size_t rest = 20 + nbytes;
        uint8_t *body = calloc(1, rest + 4);
        if (!body) { free(scan); return -1; }
        put_u32(body + 0, win);
        put_u32(body + 4, gc);
        put_u16(body + 8, (uint16_t)w);
        put_u16(body + 10, (uint16_t)rows);
        put_u16(body + 12, (uint16_t)(int16_t)dstx);
        put_u16(body + 14, (uint16_t)(int16_t)(dsty + y0));
        body[16] = 0;
        body[17] = c->depth;
        memcpy(body + 20, scan, nbytes);
        int rc = expect(c, 72, 2, body, rest, NULL, NULL);
        free(body);
        if (rc < 0) { free(scan); return -1; }
        y0 += rows;
    }
    free(scan);
    return 0;
}

int x_keyboard(XConn *c) {
    if (c->max_keycode < c->min_keycode) return 0;
    uint8_t count = (uint8_t)(c->max_keycode - c->min_keycode + 1);
    uint8_t body[4];
    body[0] = c->min_keycode;
    body[1] = count;
    body[2] = 0;
    body[3] = 0;
    uint8_t *reply = NULL;
    size_t rlen = 0;
    if (expect(c, 101, 0, body, 4, &reply, &rlen) < 0 || !reply) return -1;
    c->keysyms_per = reply[1];
    if (!c->keysyms_per) c->keysyms_per = 1;
    uint32_t n = (uint32_t)count * c->keysyms_per;
    c->keysyms = calloc(n ? n : 1, sizeof(uint32_t));
    c->nkeysyms = (int)n;
    if (!c->keysyms) { free(reply); return -1; }
    for (uint32_t i = 0; i < n; i++) {
        size_t o = 32 + (size_t)i * 4;
        if (o + 4 <= rlen) c->keysyms[i] = get_u32(reply + o);
    }
    free(reply);
    return 0;
}

uint32_t x_keysym(XConn *c, uint8_t keycode, int shifted) {
    if (!c->keysyms || keycode < c->min_keycode || keycode > c->max_keycode) return 0;
    uint32_t idx = (uint32_t)(keycode - c->min_keycode) * c->keysyms_per;
    uint32_t base = c->keysyms[idx];
    if (shifted && c->keysyms_per > 1) {
        uint32_t s = c->keysyms[idx + 1];
        if (s) return s;
    }
    return base;
}

int x_flush(XConn *c) {
    (void)c;
    return 0;
}

int x_poll(XConn *c, XEvent *ev, int timeout_ms) {
    if (c->eq_len > 0) {
        *ev = c->eq[0];
        memmove(c->eq, c->eq + 1, (size_t)(c->eq_len - 1) * sizeof(XEvent));
        c->eq_len--;
        return 1;
    }
    int st = pull(c, 0, timeout_ms, NULL, NULL);
    if (c->eq_len > 0) {
        *ev = c->eq[0];
        memmove(c->eq, c->eq + 1, (size_t)(c->eq_len - 1) * sizeof(XEvent));
        c->eq_len--;
        return 1;
    }
    if (st < 0) return -1;
    return 0;
}

int x_roundtrip(XConn *c) {
    int x, y, w, h;
    return x_geometry(c, c->root, &x, &y, &w, &h);
}

#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct XConn XConn;

typedef struct {
    uint8_t type;
    uint8_t detail;
    uint16_t seq;
    uint8_t raw[32];
} XEvent;

struct XConn {
    int fd;
    char err[160];
    uint16_t seq;
    uint32_t rid_base;
    uint32_t rid_mask;
    uint32_t rid_next;
    uint16_t max_req;
    uint8_t image_lsb;
    uint8_t scanline_pad;
    uint8_t bpp;
    uint8_t depth;
    uint8_t min_keycode;
    uint8_t max_keycode;
    uint8_t keysyms_per;
    uint32_t *keysyms;
    int nkeysyms;
    uint32_t root;
    uint32_t root_visual;
    uint32_t black;
    uint32_t white;
    uint32_t red_mask, green_mask, blue_mask;
    uint16_t root_w, root_h;
    uint8_t *in;
    size_t in_len, in_cap;
    XEvent *eq;
    int eq_len, eq_cap;
    int last_error;
    uint16_t last_error_seq;
    char last_error_text[80];
};

int x_connect(XConn *c, const char *display);
void x_disconnect(XConn *c);
uint32_t x_id(XConn *c);

uint32_t x_intern(XConn *c, const char *name);
int x_query_tree(XConn *c, uint32_t win, uint32_t **children, int *n);
int x_geometry(XConn *c, uint32_t win, int *x, int *y, int *w, int *h);
int x_attributes(XConn *c, uint32_t win, int *map_state, int *override_redirect, int *klass);
int x_property(XConn *c, uint32_t win, uint32_t prop, uint8_t **data, uint32_t *nitems, uint8_t *format, uint32_t *type);
int x_change_property(XConn *c, uint32_t win, uint32_t prop, uint32_t type, uint8_t format, const void *data, uint32_t nitems);
int x_configure(XConn *c, uint32_t win, uint16_t mask, int x, int y, int w, int h);
int x_send_event(XConn *c, uint32_t dest, uint32_t mask, const uint8_t event[32]);

uint32_t x_create_window(XConn *c, int x, int y, int w, int h, uint32_t event_mask);
int x_map(XConn *c, uint32_t win);
int x_destroy(XConn *c, uint32_t win);
int x_change_attr(XConn *c, uint32_t win, uint32_t mask, uint32_t value);
uint32_t x_create_gc(XConn *c, uint32_t drawable, uint32_t fg);
uint32_t x_create_pixmap(XConn *c, uint32_t drawable, int w, int h);
int x_free_pixmap(XConn *c, uint32_t pixmap);
int x_copy_area(XConn *c, uint32_t src, uint32_t dst, uint32_t gc, int w, int h);
int x_put_image(XConn *c, uint32_t win, uint32_t gc, int dstx, int dsty, int w, int h, const uint32_t *rgb);
int x_keyboard(XConn *c);
uint32_t x_keysym(XConn *c, uint8_t keycode, int shifted);

int x_flush(XConn *c);
int x_poll(XConn *c, XEvent *ev, int timeout_ms);
int x_roundtrip(XConn *c);

/* Predefined atoms used by this tool. */
enum {
    XA_ATOM_ = 4,
    XA_CARDINAL_ = 6,
    XA_STRING_ = 31,
    XA_WM_NAME_ = 39,
    XA_WM_NORMAL_HINTS_ = 40,
    XA_WM_SIZE_HINTS_ = 41,
    XA_WM_CLASS_ = 67
};

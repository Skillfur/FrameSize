#define _GNU_SOURCE
#include "font.h"
#include "engine.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
    HOT_REFRESH = 1,
    HOT_DISPLAY,
    HOT_WFIELD,
    HOT_HFIELD,
    HOT_APPLY,
    HOT_PIN,
    HOT_UNFULL,
    HOT_CHILD,
    HOT_ASPECT,
    HOT_SCREEN_W,
    HOT_SCREEN_H,
    HOT_SCREEN_APPLY,
    HOT_SUPER,
    HOT_LIST = 100,
    HOT_PRESET = 200,
    HOT_SCREEN_PRESET = 300
};

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static int utf_next(const char **ps) {
    const unsigned char *p = (const unsigned char *)*ps;
    if (!*p) return -1;
    if (*p < 0x80) { *ps += 1; return *p; }
    if ((*p & 0xe0) == 0xc0 && p[1]) { *ps += 2; return '?'; }
    if ((*p & 0xf0) == 0xe0 && p[1] && p[2]) { *ps += 3; return '?'; }
    if ((*p & 0xf8) == 0xf0 && p[1] && p[2] && p[3]) { *ps += 4; return '?'; }
    *ps += 1;
    return '?';
}

static const Glyph *glyph_for(const Glyph *font, int cp) {
    if (cp < FONT_FIRST || cp >= FONT_FIRST + FONT_COUNT) cp = '?';
    return &font[cp - FONT_FIRST];
}

static int text_width(const Glyph *font, const char *s) {
    int w = 0;
    const char *p = s;
    int cp;
    while ((cp = utf_next(&p)) >= 0) w += glyph_for(font, cp)->adv;
    return w;
}

static void blend(uint32_t *px, uint32_t rgb, int alpha) {
    if (alpha <= 0) return;
    if (alpha >= 255) { *px = rgb; return; }
    uint32_t d = *px;
    int dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
    int sr = (rgb >> 16) & 255, sg = (rgb >> 8) & 255, sb = rgb & 255;
    dr += ((sr - dr) * alpha) / 255;
    dg += ((sg - dg) * alpha) / 255;
    db += ((sb - db) * alpha) / 255;
    *px = ((uint32_t)dr << 16) | ((uint32_t)dg << 8) | (uint32_t)db;
}

static void fill_rect(App *a, int x, int y, int w, int h, uint32_t rgb) {
    if (w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > a->fb_w) x1 = a->fb_w;
    int y1 = y + h; if (y1 > a->fb_h) y1 = a->fb_h;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = a->fb + (size_t)yy * (size_t)a->fb_w;
        for (int xx = x0; xx < x1; xx++) row[xx] = rgb;
    }
}

static void stroke_rect(App *a, int x, int y, int w, int h, uint32_t rgb) {
    fill_rect(a, x, y, w, 1, rgb);
    fill_rect(a, x, y + h - 1, w, 1, rgb);
    fill_rect(a, x, y, 1, h, rgb);
    fill_rect(a, x + w - 1, y, 1, h, rgb);
}

static void draw_text(App *a, const Glyph *font, const uint8_t *alpha, int x, int y, uint32_t rgb, const char *s) {
    const char *p = s;
    int cp;
    while ((cp = utf_next(&p)) >= 0) {
        const Glyph *g = glyph_for(font, cp);
        for (int row = 0; row < g->h; row++) {
            int py = y + g->yoff + row;
            if (py < 0 || py >= a->fb_h) continue;
            for (int col = 0; col < g->w; col++) {
                int px = x + g->xoff + col;
                if (px < 0 || px >= a->fb_w) continue;
                uint8_t al = alpha[g->off + (uint32_t)row * g->w + (uint32_t)col];
                if (!al) continue;
                blend(&a->fb[(size_t)py * (size_t)a->fb_w + (size_t)px], rgb, al);
            }
        }
        x += g->adv;
    }
}

static void hot(App *a, int id, int x, int y, int w, int h) {
    if (a->nhots >= (int)(sizeof a->hots / sizeof a->hots[0])) return;
    a->hots[a->nhots++] = (Hot){ id, x, y, w, h };
}

static int hit(App *a, int x, int y) {
    for (int i = a->nhots - 1; i >= 0; i--) {
        Hot *h = &a->hots[i];
        if (x >= h->x && y >= h->y && x < h->x + h->w && y < h->y + h->h) return h->id;
    }
    return 0;
}

static int inside(App *a, int id, int x, int y) {
    for (int i = 0; i < a->nhots; i++) {
        if (a->hots[i].id != id) continue;
        Hot *h = &a->hots[i];
        return x >= h->x && y >= h->y && x < h->x + h->w && y < h->y + h->h;
    }
    return 0;
}

static void fill_fields_from_selection(App *a) {
    if (a->selected < 0 || a->selected >= a->nwins) return;
    WinInfo *w = &a->wins[a->selected];
    snprintf(a->width_buf, sizeof a->width_buf, "%d", w->w);
    snprintf(a->height_buf, sizeof a->height_buf, "%d", w->h);
    a->aspect_w = w->w > 0 ? w->w : 1;
    a->aspect_h = w->h > 0 ? w->h : 1;
}

static char *focused_buf(App *a) {
    if (a->focus_field == 0) return a->width_buf;
    if (a->focus_field == 1) return a->height_buf;
    if (a->focus_field == 2) return a->screen_w;
    if (a->focus_field == 3) return a->screen_h;
    return NULL;
}

static void sync_aspect(App *a, int edited_width) {
    if (!a->aspect || a->aspect_w < 1 || a->aspect_h < 1) return;
    if (edited_width) {
        int w = atoi(a->width_buf);
        if (w < 1) return;
        int h = (int)((w * (long)a->aspect_h + a->aspect_w / 2) / a->aspect_w);
        if (h < 1) h = 1;
        snprintf(a->height_buf, sizeof a->height_buf, "%d", h);
    } else {
        int h = atoi(a->height_buf);
        if (h < 1) return;
        int w = (int)((h * (long)a->aspect_w + a->aspect_h / 2) / a->aspect_h);
        if (w < 1) w = 1;
        snprintf(a->width_buf, sizeof a->width_buf, "%d", w);
    }
}

static void apply_selected(App *a) {
    if (a->selected < 0 || a->selected >= a->nwins) {
        set_status(a, 2, "Select a window first.");
        return;
    }
    int w = atoi(a->width_buf);
    int h = atoi(a->height_buf);
    char msg[320];
    int rc = apply_size(a, &a->wins[a->selected], w, h, a->unfull, a->do_child, msg, sizeof msg);
    set_status(a, rc == 0 ? 1 : 2, msg);
    if (a->pin && rc == 0) {
        a->pin_id = a->wins[a->selected].id;
        a->pin_w = w;
        a->pin_h = h;
    }
    if (!a->pin) a->pin_id = 0;
}

static uint32_t C_BG = 0x101418, C_PANEL = 0x1a2129, C_PANEL2 = 0x222a33, C_LINE = 0x313a45;
static uint32_t C_TEXT = 0xe7eef5, C_MUTED = 0x8b98a6, C_ACCENT = 0x2fbfb0, C_ACCENT2 = 0x143632;
static uint32_t C_WARN = 0xd7a441, C_BAD = 0xd26b6b, C_OK = 0x86c481, C_FIELD = 0x0e1318;

static void button(App *a, int id, int x, int y, int w, int h, const char *label, int primary) {
    int over = (a->hover == id) || inside(a, id, a->mouse_x, a->mouse_y);
    int down = a->pressed == id;
    uint32_t bg = primary ? (down ? 0x249e92 : (over ? 0x39cfc0 : C_ACCENT)) : (down ? 0x1a222b : (over ? 0x2a343f : C_PANEL2));
    uint32_t fg = primary ? 0x06211f : C_TEXT;
    fill_rect(a, x, y, w, h, bg);
    stroke_rect(a, x, y, w, h, primary ? 0x8ee8df : C_LINE);
    int tw = text_width(font_ui, label);
    draw_text(a, font_ui, font_ui_alpha, x + (w - tw) / 2, y + (h - 16) / 2 + 1, fg, label);
    hot(a, id, x, y, w, h);
}

static void checkbox(App *a, int id, int x, int y, const char *label, int on) {
    fill_rect(a, x, y + 2, 16, 16, 0x0c1116);
    stroke_rect(a, x, y + 2, 16, 16, on ? C_ACCENT : 0x7d8b99);
    if (on) fill_rect(a, x + 3, y + 5, 10, 10, C_ACCENT);
    draw_text(a, font_ui, font_ui_alpha, x + 22, y, C_TEXT, label);
    int tw = 22 + text_width(font_ui, label);
    hot(a, id, x, y, tw, 20);
}

static void numfield(App *a, int id, int x, int y, int w, const char *label, const char *value, int which) {
    draw_text(a, font_ui, font_ui_alpha, x, y, C_MUTED, label);
    int focused = a->focus_field == which;
    fill_rect(a, x, y + 18, w, 32, C_FIELD);
    stroke_rect(a, x, y + 18, w, 32, focused ? C_ACCENT : C_LINE);
    draw_text(a, font_mono, font_mono_alpha, x + 8, y + 24, C_TEXT, value[0] ? value : "0");
    if (focused) {
        int cx = x + 8 + text_width(font_mono, value);
        fill_rect(a, cx + 2, y + 24, 2, 14, C_ACCENT);
    }
    hot(a, id, x, y, w, 50);
}

static void ellipsize(char *dst, size_t n, const Glyph *font, const char *src, int max_w) {
    if (text_width(font, src) <= max_w) {
        snprintf(dst, n, "%s", src);
        return;
    }
    snprintf(dst, n, "%s", src);
    size_t len = strlen(dst);
    while (len > 1) {
        dst[len - 1] = 0;
        dst[len - 2] = 0;
        /* walk back a byte and add ellipsis via ASCII */
        len = strlen(dst);
        char tmp[200];
        snprintf(tmp, sizeof tmp, "%s...", dst);
        if (text_width(font, tmp) <= max_w) {
            snprintf(dst, n, "%s", tmp);
            return;
        }
    }
    snprintf(dst, n, "...");
}

static void ui_draw(App *a) {
    a->nhots = 0;
    fill_rect(a, 0, 0, a->fb_w, a->fb_h, C_BG);
    fill_rect(a, 0, 0, a->fb_w, 64, C_PANEL);
    fill_rect(a, 0, 63, a->fb_w, 1, C_LINE);
    draw_text(a, font_bold, font_bold_alpha, 18, 12, C_TEXT, "FrameSize");
    draw_text(a, font_ui, font_ui_alpha, 18, 34, C_MUTED,
              "Writes a fixed size hint and ConfigureWindow.");

    char disp[96];
    snprintf(disp, sizeof disp, "%s", a->display[0] ? a->display : ":0");
    int dw = 118;
    button(a, HOT_DISPLAY, a->fb_w - 18 - dw - 92, 16, dw, 32, disp, 0);
    button(a, HOT_REFRESH, a->fb_w - 18 - 84, 16, 84, 32, "Refresh", 0);

    int top = 80;
    int foot = 52;
    int list_w = a->fb_w * 44 / 100;
    if (list_w < 300) list_w = 300;
    if (a->fb_w - list_w < 340) list_w = a->fb_w - 340;
    if (list_w < 220) list_w = a->fb_w / 2;
    int list_x = 16;
    int list_y = top;
    int list_h = a->fb_h - top - foot - 8;
    fill_rect(a, list_x, list_y, list_w, list_h, C_PANEL);
    stroke_rect(a, list_x, list_y, list_w, list_h, C_LINE);

    char head[64];
    snprintf(head, sizeof head, "Windows  %d", a->nwins);
    draw_text(a, font_bold, font_bold_alpha, list_x + 12, list_y + 10, C_TEXT, head);
    char meta[96];
    if (a->have_scale)
        snprintf(meta, sizeof meta, "Xwayland %dx%d   scale %.2f", a->root_w, a->root_h, a->screen_scale);
    else
        snprintf(meta, sizeof meta, "Xwayland %dx%d", a->root_w, a->root_h);
    draw_text(a, font_ui, font_ui_alpha, list_x + 12, list_y + 30, C_MUTED, meta);

    int row_h = 54;
    int rows_y = list_y + 54;
    int rows_h = list_h - 62;
    int visible = rows_h / row_h;
    if (visible < 1) visible = 1;
    if (a->selected >= 0) {
        if (a->selected < a->scroll) a->scroll = a->selected;
        if (a->selected >= a->scroll + visible) a->scroll = a->selected - visible + 1;
    }
    if (a->scroll < 0) a->scroll = 0;
    if (a->scroll > a->nwins - visible && a->nwins > visible) a->scroll = a->nwins - visible;
    if (a->nwins <= visible) a->scroll = 0;

    if (a->nwins == 0) {
        draw_text(a, font_ui, font_ui_alpha, list_x + 12, rows_y + 8, C_MUTED, "No other viewable windows");
        draw_text(a, font_ui, font_ui_alpha, list_x + 12, rows_y + 28, C_MUTED, "on this Xwayland display.");
    }
    for (int i = 0; i < visible; i++) {
        int idx = a->scroll + i;
        if (idx >= a->nwins) break;
        WinInfo *w = &a->wins[idx];
        int y = rows_y + i * row_h;
        int sel = idx == a->selected;
        if (sel) fill_rect(a, list_x + 6, y, list_w - 12, row_h - 4, C_ACCENT2);
        if (sel) fill_rect(a, list_x + 6, y, 3, row_h - 4, C_ACCENT);
        char title[180];
        ellipsize(title, sizeof title, font_ui, w->title, list_w - 36);
        draw_text(a, font_ui, font_ui_alpha, list_x + 16, y + 4, sel ? 0xf3fffd : C_TEXT, title);
        char sub[160];
        snprintf(sub, sizeof sub, "0x%x   %dx%d", w->id, w->w, w->h);
        if (w->pid) {
            char p[32];
            snprintf(p, sizeof p, "   pid %d", w->pid);
            strncat(sub, p, sizeof sub - strlen(sub) - 1);
        }
        if (w->hint_fixed) strncat(sub, "   fixed", sizeof sub - strlen(sub) - 1);
        if (w->fullscreen) strncat(sub, "   fullscreen", sizeof sub - strlen(sub) - 1);
        if (w->override) strncat(sub, "   override", sizeof sub - strlen(sub) - 1);
        draw_text(a, font_mono, font_mono_alpha, list_x + 16, y + 24, C_MUTED, sub);
        hot(a, HOT_LIST + idx, list_x + 6, y, list_w - 12, row_h - 4);
    }

    int rx = list_x + list_w + 14;
    int rw = a->fb_w - rx - 16;
    int ry = top;
    fill_rect(a, rx, ry, rw, list_h, C_PANEL);
    stroke_rect(a, rx, ry, rw, list_h, C_LINE);

    if (a->selected < 0 || a->selected >= a->nwins) {
        draw_text(a, font_ui, font_ui_alpha, rx + 16, ry + 20, C_MUTED, "Select a window to resize its buffer.");
    } else {
        WinInfo *w = &a->wins[a->selected];
        char title[180];
        ellipsize(title, sizeof title, font_bold, w->title, rw - 32);
        draw_text(a, font_bold, font_bold_alpha, rx + 16, ry + 12, C_TEXT, title);
        char line[160];
        snprintf(line, sizeof line, "Actual buffer   %d x %d", w->w, w->h);
        draw_text(a, font_ui, font_ui_alpha, rx + 16, ry + 36, C_ACCENT, line);
        if (w->hint_fixed)
            snprintf(line, sizeof line, "Gamescope hint min=max %dx%d", w->min_w, w->min_h);
        else
            snprintf(line, sizeof line, "No fixed hint - Gamescope may snap this to the Xwayland screen");
        draw_text(a, font_ui, font_ui_alpha, rx + 16, ry + 56, w->hint_fixed ? C_OK : C_WARN, line);
        if (w->child && w->child_w > 1)
            snprintf(line, sizeof line, "Largest child   %dx%d", w->child_w, w->child_h);
        else
            snprintf(line, sizeof line, "No sizable child window");
        draw_text(a, font_ui, font_ui_alpha, rx + 16, ry + 76, C_MUTED, line);

        int fw = (rw - 48) / 2;
        if (fw > 160) fw = 160;
        numfield(a, HOT_WFIELD, rx + 16, ry + 104, fw, "WIDTH", a->width_buf, 0);
        numfield(a, HOT_HFIELD, rx + 16 + fw + 12, ry + 104, fw, "HEIGHT", a->height_buf, 1);

        int py = ry + 166;
        int pw = 120, ph = 28, gap = 8;
        int cols = (rw - 32 + gap) / (pw + gap);
        if (cols < 1) cols = 1;
        if (cols > FR_NPRESETS) cols = FR_NPRESETS;
        int npre = FR_NPRESETS;
        for (int i = 0; i < npre; i++) {
            int col = i % cols;
            int row = i / cols;
            char lab[32];
            snprintf(lab, sizeof lab, "%dx%d", FR_PRESETS[i][0], FR_PRESETS[i][1]);
            button(a, HOT_PRESET + i, rx + 16 + col * (pw + gap), py + row * (ph + gap), pw, ph, lab, 0);
        }
        int rows_used = (npre + cols - 1) / cols;
        int oy = py + rows_used * (ph + gap) + 6;
        checkbox(a, HOT_ASPECT, rx + 16, oy, "Keep aspect", a->aspect);
        checkbox(a, HOT_UNFULL, rx + 16, oy + 26, "Clear fullscreen state", a->unfull);
        checkbox(a, HOT_CHILD, rx + 16, oy + 52, "Also resize content child", a->do_child);
        checkbox(a, HOT_PIN, rx + 16, oy + 78, "Pin - reapply if the app rewrites the hint", a->pin);
        button(a, HOT_APPLY, rx + 16, oy + 112, rw - 32, 36, "Apply actual size", 1);

        int panel_bottom = ry + list_h;
        int sy = oy + 156;
        int field_y = sy + 30;
        int spw = pw, sph = ph, sgap = gap;
        int scols = (rw - 32 + sgap) / (spw + sgap);
        if (scols < 1) scols = 1;
        if (scols > FR_NPRESETS) scols = FR_NPRESETS;
        int srows = (FR_NPRESETS + scols - 1) / scols;
        int preset_y = field_y + 58;
        int check_y = preset_y + srows * (sph + sgap) + 4;
        int check_bottom = check_y + 22;
        int margin = 16;
        if (check_bottom > panel_bottom - margin) {
            int shift = check_bottom - (panel_bottom - margin);
            sy -= shift;
            field_y -= shift;
            preset_y -= shift;
            check_y -= shift;
            check_bottom -= shift;
        }
        if (sy >= oy + 148 && check_bottom <= panel_bottom - 8) {
            fill_rect(a, rx + 16, sy, rw - 32, 1, C_LINE);
            draw_text(a, font_ui, font_ui_alpha, rx + 16, sy + 10, C_MUTED, "Xwayland screen (every forced-fullscreen window)");
            int sww = 100;
            numfield(a, HOT_SCREEN_W, rx + 16, field_y, sww, "SCREEN W", a->screen_w, 2);
            numfield(a, HOT_SCREEN_H, rx + 16 + sww + 10, field_y, sww, "SCREEN H", a->screen_h, 3);
            button(a, HOT_SCREEN_APPLY, rx + 16 + (sww + 10) * 2, field_y + 18, 120, 32, "Set screen", 0);
            for (int i = 0; i < FR_NPRESETS; i++) {
                int col = i % scols;
                int row = i / scols;
                char lab[32];
                snprintf(lab, sizeof lab, "%dx%d", FR_PRESETS[i][0], FR_PRESETS[i][1]);
                button(a, HOT_SCREEN_PRESET + i,
                       rx + 16 + col * (spw + sgap),
                       preset_y + row * (sph + sgap),
                       spw, sph, lab, 0);
            }
            checkbox(a, HOT_SUPER, rx + 16, check_y, "Allow above panel size", a->superres);
        }
    }

    fill_rect(a, 0, a->fb_h - foot, a->fb_w, foot, C_PANEL);
    fill_rect(a, 0, a->fb_h - foot, a->fb_w, 1, C_LINE);
    uint32_t sc = a->status_kind == 1 ? C_OK : (a->status_kind == 2 ? C_BAD : C_MUTED);
    const char *st = a->status[0] ? a->status : "Enter applies the size. Arrows select a window. R refreshes.";
    draw_text(a, font_ui, font_ui_alpha, 16, a->fb_h - foot + 16, sc, st);
}

static int alloc_fb(App *a, int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > 4096) w = 4096;
    if (h > 4096) h = 4096;
    uint32_t *n = realloc(a->fb, (size_t)w * (size_t)h * sizeof(uint32_t));
    if (!n) return -1;
    a->fb = n;
    a->fb_w = w;
    a->fb_h = h;
    return 0;
}

static int present(App *a) {
    if (!a->x || !a->our_win || !a->gc || !a->fb) return 0;
    if (a->pixmap && (a->pixmap_w != a->fb_w || a->pixmap_h != a->fb_h)) {
        x_free_pixmap(a->x, a->pixmap);
        a->pixmap = 0;
    }
    if (!a->pixmap) {
        a->pixmap = x_create_pixmap(a->x, a->our_win, a->fb_w, a->fb_h);
        if (!a->pixmap) return -1;
        a->pixmap_w = a->fb_w;
        a->pixmap_h = a->fb_h;
    }
    /* Gamescope scans out Xwayland when a blit damages the window. PutImage
       alone updates the pixmap and the Frame stays black. */
    if (x_put_image(a->x, a->pixmap, a->gc, 0, 0, a->fb_w, a->fb_h, a->fb) < 0) return -1;
    return x_copy_area(a->x, a->pixmap, a->our_win, a->gc, a->fb_w, a->fb_h);
}

static void type_digit(App *a, int digit) {
    char *b = focused_buf(a);
    if (!b) return;
    size_t n = strlen(b);
    if (n >= 5) return;
    if (n == 1 && b[0] == '0') n = 0;
    b[n] = (char)('0' + digit);
    b[n + 1] = 0;
    if (a->focus_field == 0) sync_aspect(a, 1);
    if (a->focus_field == 1) sync_aspect(a, 0);
}

static void backspace_field(App *a) {
    char *b = focused_buf(a);
    if (!b || !b[0]) return;
    b[strlen(b) - 1] = 0;
    if (a->focus_field == 0) sync_aspect(a, 1);
    if (a->focus_field == 1) sync_aspect(a, 0);
}

static void bump_field(App *a, int delta) {
    char *b = focused_buf(a);
    if (!b) return;
    int v = atoi(b);
    v += delta;
    if (v < 1) v = 1;
    if (v > 7680) v = 7680;
    snprintf(b, 8, "%d", v);
    if (a->focus_field == 0) sync_aspect(a, 1);
    if (a->focus_field == 1) sync_aspect(a, 0);
}

static void on_hot(App *a, int id) {
    if (id == HOT_REFRESH) {
        scan_windows(a);
        read_root_meta(a);
        set_status(a, 0, "Window list refreshed.");
        return;
    }
    if (id == HOT_DISPLAY) {
        if (a->ndisplays > 1) {
            int cur = 0;
            for (int i = 0; i < a->ndisplays; i++) {
                if (strcmp(a->displays[i], a->display) == 0) cur = i;
            }
            snprintf(a->display, sizeof a->display, "%s", a->displays[(cur + 1) % a->ndisplays]);
            set_status(a, 0, "Display changed - reconnecting.");
            a->pressed = -2; /* signal reconnect */
        }
        return;
    }
    if (id >= HOT_LIST && id < HOT_LIST + 96) {
        int idx = id - HOT_LIST;
        if (idx >= 0 && idx < a->nwins) {
            uint64_t t = now_ms();
            int same = a->wins[idx].id == a->last_click_id && t - a->last_click_ms < 400;
            a->selected = idx;
            a->focus_field = -1;
            fill_fields_from_selection(a);
            a->last_click_id = a->wins[idx].id;
            a->last_click_ms = t;
            if (same) apply_selected(a);
        }
        return;
    }
    if (id >= HOT_PRESET && id < HOT_PRESET + 16) {
        int i = id - HOT_PRESET;
        int npre = FR_NPRESETS;
        if (i >= 0 && i < npre) {
            snprintf(a->width_buf, sizeof a->width_buf, "%d", FR_PRESETS[i][0]);
            snprintf(a->height_buf, sizeof a->height_buf, "%d", FR_PRESETS[i][1]);
            a->aspect = 0;
        }
        return;
    }
    if (id >= HOT_SCREEN_PRESET && id < HOT_SCREEN_PRESET + 16) {
        int i = id - HOT_SCREEN_PRESET;
        if (i >= 0 && i < FR_NPRESETS) {
            snprintf(a->screen_w, sizeof a->screen_w, "%d", FR_PRESETS[i][0]);
            snprintf(a->screen_h, sizeof a->screen_h, "%d", FR_PRESETS[i][1]);
        }
        return;
    }
    if (id == HOT_WFIELD) a->focus_field = 0;
    else if (id == HOT_HFIELD) a->focus_field = 1;
    else if (id == HOT_SCREEN_W) a->focus_field = 2;
    else if (id == HOT_SCREEN_H) a->focus_field = 3;
    else if (id == HOT_APPLY) apply_selected(a);
    else if (id == HOT_PIN) a->pin = !a->pin;
    else if (id == HOT_UNFULL) a->unfull = !a->unfull;
    else if (id == HOT_CHILD) a->do_child = !a->do_child;
    else if (id == HOT_ASPECT) {
        a->aspect = !a->aspect;
        if (a->aspect) {
            a->aspect_w = atoi(a->width_buf);
            a->aspect_h = atoi(a->height_buf);
            if (a->aspect_w < 1) a->aspect_w = 16;
            if (a->aspect_h < 1) a->aspect_h = 9;
        }
    } else if (id == HOT_SUPER) a->superres = !a->superres;
    else if (id == HOT_SCREEN_APPLY) {
        char msg[320];
        set_xwayland_mode(a, atoi(a->screen_w), atoi(a->screen_h), a->superres, msg, sizeof msg);
        set_status(a, 0, msg);
        read_root_meta(a);
    }
    if (!a->pin) a->pin_id = 0;
}

static int write_ui_hints(XConn *x, uint32_t win, int w, int h) {
    int32_t f[18];
    memset(f, 0, sizeof f);
    /* USPosition | USSize | PMinSize | PBaseSize. Not min=max: Gamescope treats
       a locked hint as a game buffer and shows black until that buffer is presented. */
    f[0] = (1 << 0) | (1 << 1) | (1 << 4) | (1 << 8);
    f[1] = 48;
    f[2] = 48;
    f[3] = w;
    f[4] = h;
    f[5] = 640;
    f[6] = 360;
    f[9] = 1;
    f[10] = 1;
    f[15] = w;
    f[16] = h;
    uint8_t raw[18 * 4];
    for (int i = 0; i < 18; i++) put32(raw + i * 4, (uint32_t)f[i]);
    return x_change_property(x, win, XA_WM_NORMAL_HINTS_, XA_WM_SIZE_HINTS_, 32, raw, 18);
}

static int open_gui_window(App *a) {
    int ww = 1280, hh = 720;
    if (alloc_fb(a, ww, hh) < 0) return -1;
    uint32_t mask = 0x1u | 0x4u | 0x8u | 0x40u | 0x8000u | 0x20000u;
    a->our_win = x_create_window(a->x, 48, 48, ww, hh, mask);
    if (!a->our_win) {
        fprintf(stderr, "create window failed: %s\n", a->x->err);
        return -1;
    }
    x_change_attr(a->x, a->our_win, 0x1u, 0);
    const char *title = "FrameSize";
    x_change_property(a->x, a->our_win, XA_WM_NAME_, XA_STRING_, 8, title, (uint32_t)strlen(title));
    if (a->atom.net_wm_name && a->atom.utf8)
        x_change_property(a->x, a->our_win, a->atom.net_wm_name, a->atom.utf8, 8, title, (uint32_t)strlen(title));
    char cls[] = "framesize\0FrameSize";
    x_change_property(a->x, a->our_win, XA_WM_CLASS_, XA_STRING_, 8, cls, (uint32_t)sizeof cls);
    uint8_t pidb[4];
    put32(pidb, (uint32_t)getpid());
    if (a->atom.pid) x_change_property(a->x, a->our_win, a->atom.pid, XA_CARDINAL_, 32, pidb, 1);
    if (a->atom.protocols && a->atom.wm_delete) {
        uint8_t proto[4];
        put32(proto, a->atom.wm_delete);
        x_change_property(a->x, a->our_win, a->atom.protocols, XA_ATOM_, 32, proto, 1);
    }
    if (a->atom.wintype && a->atom.wintype_normal) {
        uint8_t ty[4];
        put32(ty, a->atom.wintype_normal);
        x_change_property(a->x, a->our_win, a->atom.wintype, XA_ATOM_, 32, ty, 1);
    }
    write_ui_hints(a->x, a->our_win, ww, hh);
    a->gc = x_create_gc(a->x, a->our_win, a->x->white);
    if (!a->gc) return -1;
    if (x_map(a->x, a->our_win) < 0) return -1;
    x_roundtrip(a->x);
    /* Do not select SubstructureNotify on the root. Gamescope emits a
       ConfigureNotify for managed windows many times a second, and handling
       those one redraw at a time is what made clicks fall further behind. */
    x_keyboard(a->x);
    return 0;
}

static int gui_connect(App *a) {
    if (fr_xconnect(a) < 0) {
        fprintf(stderr, "%s\n", a->status);
        return -1;
    }
    if (open_gui_window(a) < 0) return -1;
    scan_windows(a);
    fill_fields_from_selection(a);
    if (!a->status[0])
        set_status(a, 0, "Select a window and apply. This changes the buffer, not the compositor scale.");
    return 0;
}

static void handle_key(App *a, uint32_t keysym, int shift) {
    (void)shift;
    if (keysym == 0xff1b) { a->focus_field = -1; return; }
    if (keysym == 0xff09) {
        a->focus_field = (a->focus_field + 1) % 4;
        if (a->focus_field < 0) a->focus_field = 0;
        return;
    }
    if (keysym == 0xff0d || keysym == 0xff8d) {
        if (a->focus_field >= 2) {
            char msg[320];
            set_xwayland_mode(a, atoi(a->screen_w), atoi(a->screen_h), a->superres, msg, sizeof msg);
            set_status(a, 0, msg);
        } else apply_selected(a);
        return;
    }
    if (keysym == 0xff08) { backspace_field(a); return; }
    if (keysym == 0xff52) { /* up */
        if (a->focus_field >= 0) bump_field(a, shift ? 10 : 1);
        else if (a->selected > 0) { a->selected--; fill_fields_from_selection(a); }
        return;
    }
    if (keysym == 0xff54) {
        if (a->focus_field >= 0) bump_field(a, shift ? -10 : -1);
        else if (a->selected + 1 < a->nwins) { a->selected++; fill_fields_from_selection(a); }
        return;
    }
    if (keysym == 'r' || keysym == 'R') {
        scan_windows(a);
        read_root_meta(a);
        set_status(a, 0, "Window list refreshed.");
        return;
    }
    if (keysym >= '0' && keysym <= '9') type_digit(a, (int)(keysym - '0'));
    else if (keysym >= 0xffb0 && keysym <= 0xffb9) type_digit(a, (int)(keysym - 0xffb0));
}

static uint32_t list_sig(const App *a) {
    uint32_t s = 2166136261u;
    s = s * 16777619u ^ (uint32_t)a->nwins;
    s = s * 16777619u ^ (uint32_t)a->root_w;
    s = s * 16777619u ^ (uint32_t)a->root_h;
    s = s * 16777619u ^ (a->have_scale ? (uint32_t)(a->screen_scale * 1000.0) : 0u);
    for (int i = 0; i < a->nwins; i++) {
        const WinInfo *w = &a->wins[i];
        s = s * 16777619u ^ w->id;
        s = s * 16777619u ^ (uint32_t)w->w;
        s = s * 16777619u ^ (uint32_t)w->h;
        s = s * 16777619u ^ (uint32_t)w->fullscreen;
        s = s * 16777619u ^ (uint32_t)w->hint_fixed;
        s = s * 16777619u ^ (uint32_t)w->override;
        s = s * 16777619u ^ (uint32_t)w->pid;
        s = s * 16777619u ^ (uint32_t)w->child_w;
        for (const char *p = w->title; *p; p++) s = s * 16777619u ^ (uint8_t)*p;
    }
    return s;
}

static void motion_at(App *a, int x, int y, int *dirty) {
    a->mouse_x = x;
    a->mouse_y = y;
    int h = hit(a, x, y);
    if (h != a->hover) {
        a->hover = h;
        *dirty = 1;
    }
}

static int handle_gui_event(App *a, const XEvent *ev, int *dirty) {
    int type = ev->type & 0x7f;
    if (type == 4) {
        a->mouse_x = (int16_t)(ev->raw[24] | (ev->raw[25] << 8));
        a->mouse_y = (int16_t)(ev->raw[26] | (ev->raw[27] << 8));
        int button = ev->detail;
        if (button == 4 || button == 5) {
            if (a->mouse_x < a->fb_w / 2) {
                a->scroll += (button == 4) ? -1 : 1;
                if (a->scroll < 0) a->scroll = 0;
                *dirty = 1;
            }
        } else if (button == 1) {
            a->pressed = hit(a, a->mouse_x, a->mouse_y);
            a->hover = a->pressed;
            *dirty = 1;
        }
    } else if (type == 5) {
        a->mouse_x = (int16_t)(ev->raw[24] | (ev->raw[25] << 8));
        a->mouse_y = (int16_t)(ev->raw[26] | (ev->raw[27] << 8));
        if (ev->detail == 1 && a->pressed > 0) {
            int id = hit(a, a->mouse_x, a->mouse_y);
            if (id == a->pressed) on_hot(a, id);
        }
        a->pressed = 0;
        *dirty = 1;
        if (strncmp(a->status, "Display changed", 15) == 0) return 1;
    } else if (type == 6) {
        motion_at(a,
                  (int16_t)(ev->raw[24] | (ev->raw[25] << 8)),
                  (int16_t)(ev->raw[26] | (ev->raw[27] << 8)),
                  dirty);
    } else if (type == 2) {
        uint16_t state = (uint16_t)(ev->raw[28] | (ev->raw[29] << 8));
        uint32_t ks = x_keysym(a->x, ev->detail, (state & 1) || (state & 2));
        handle_key(a, ks, (state & 1) ? 1 : 0);
        *dirty = 1;
    } else if (type == 22) {
        uint32_t win = get32(ev->raw + 8);
        if (win == a->our_win) {
            int nw = (int)(ev->raw[20] | (ev->raw[21] << 8));
            int nh = (int)(ev->raw[22] | (ev->raw[23] << 8));
            if (nw > 0 && nh > 0 && (nw != a->fb_w || nh != a->fb_h)) {
                if (alloc_fb(a, nw, nh) == 0) *dirty = 1;
            }
        }
    } else if (type == 12) {
        uint32_t win = get32(ev->raw + 4);
        if (win == a->our_win) *dirty = 1;
    } else if (type == 33 || type == 17) {
        uint32_t win = get32(ev->raw + (type == 17 ? 8 : 4));
        if (win == a->our_win) {
            if (type == 17) return 2;
            uint32_t typ = get32(ev->raw + 8);
            uint32_t data0 = get32(ev->raw + 12);
            if (typ == a->atom.protocols && data0 == a->atom.wm_delete) return 2;
        }
    }
    return 0;
}

static int run_gui(App *a) {
    find_displays(a);
    if (!a->display[0]) {
        const char *env = getenv("DISPLAY");
        if (env && env[0]) snprintf(a->display, sizeof a->display, "%s", env);
        else if (a->ndisplays) snprintf(a->display, sizeof a->display, "%s", a->displays[0]);
        else snprintf(a->display, sizeof a->display, ":0");
    }
    if (gui_connect(a) < 0) return 1;
    int alive = 1;
    uint64_t last_pin = now_ms();
    uint64_t last_scan = now_ms();
    uint64_t last_present = 0;
    int dirty = 1;
    while (alive) {
        uint64_t t = now_ms();
        if (dirty) {
            int soon = last_present && t - last_present < 16;
            if (!soon) {
                ui_draw(a);
                if (present(a) < 0) {
                    fprintf(stderr, "present failed: %s\n", a->x->err);
                    break;
                }
                last_present = now_ms();
                dirty = 0;
            }
        }
        t = now_ms();
        int until_scan = (int)(2500 - (t - last_scan));
        int until_pin = (a->pin && a->pin_id) ? (int)(1000 - (t - last_pin)) : 2500;
        int until_draw = (dirty && last_present) ? (int)(16 - (now_ms() - last_present)) : 2500;
        int wait = until_scan;
        if (until_pin < wait) wait = until_pin;
        if (until_draw < wait) wait = until_draw;
        if (wait < 0) wait = 0;
        if (wait > 200) wait = 200;

        XEvent ev;
        int got = x_poll(a->x, &ev, wait);
        if (got < 0) break;
        int reconnect = 0;
        while (got > 0) {
            int act = handle_gui_event(a, &ev, &dirty);
            if (act == 2) {
                x_destroy(a->x, a->our_win);
                a->our_win = 0;
                alive = 0;
                break;
            }
            if (act == 1) {
                reconnect = 1;
                break;
            }
            got = x_poll(a->x, &ev, 0);
            if (got < 0) { alive = 0; break; }
        }
        if (!alive) break;
        if (reconnect || strncmp(a->status, "Display changed", 15) == 0) {
            if (gui_connect(a) < 0) return 1;
            set_status(a, 0, "Connected to the other Xwayland.");
            dirty = 1;
            last_scan = now_ms();
            continue;
        }
        t = now_ms();
        if (t - last_scan >= 2500) {
            uint32_t before = list_sig(a);
            scan_windows(a);
            read_root_meta(a);
            last_scan = now_ms();
            if (list_sig(a) != before) dirty = 1;
        }
        if (a->pin && a->pin_id && t - last_pin >= 1000) {
            reapply_pin(a);
            last_pin = now_ms();
        }
    }
    return 0;
}

static void usage(void) {
    fprintf(stderr,
            "FrameSize - set the real buffer size of Xwayland windows inside Gamescope on the Steam Frame\n"
            "\n"
            "  FrameSize [--display :0]\n"
            "  FrameSize --list [--display :0]\n"
            "  FrameSize --resize 0xWINDOW WIDTH HEIGHT [--display :0]\n"
            "  FrameSize --resize-name TITLE WIDTH HEIGHT [--display :0]\n"
            "  FrameSize --xwayland WIDTH HEIGHT [--server N] [--display :0]\n"
            "\n"
            "Writes WM_NORMAL_HINTS with min = max (the size Gamescope keeps as the real buffer)\n"
            "and sends ConfigureWindow, which steamcompmgr applies as a real resize.\n");
}

static int connect_cli(App *a, const char *display) {
    if (display && display[0]) snprintf(a->display, sizeof a->display, "%s", display);
    else if (getenv("DISPLAY")) snprintf(a->display, sizeof a->display, "%s", getenv("DISPLAY"));
    else snprintf(a->display, sizeof a->display, ":0");
    if (fr_xconnect(a) < 0) {
        fprintf(stderr, "%s\n", a->status);
        return 1;
    }
    if (scan_windows(a) < 0) {
        fprintf(stderr, "could not list windows: %s\n", a->x->err);
        return 1;
    }
    return 0;
}

static int cmd_list(App *a) {
    printf("display %s  xwayland %dx%d  server %u", a->display, a->root_w, a->root_h, a->server_id);
    if (a->have_scale) printf("  steam scale %.3f", a->screen_scale);
    printf("\n");
    for (int i = 0; i < a->nwins; i++) {
        WinInfo *w = &a->wins[i];
        printf("0x%x  %4dx%-4d  pid %-6d  %s%s%s  %s\n",
               w->id, w->w, w->h, w->pid,
               w->hint_fixed ? "fixed " : "",
               w->fullscreen ? "fullscreen " : "",
               w->override ? "override " : "",
               w->title);
    }
    if (!a->nwins) printf("(no viewable windows)\n");
    return 0;
}

static WinInfo *find_id(App *a, uint32_t id) {
    for (int i = 0; i < a->nwins; i++) if (a->wins[i].id == id) return &a->wins[i];
    return NULL;
}

static WinInfo *find_name(App *a, const char *sub) {
    WinInfo *found = NULL;
    int hits = 0;
    for (int i = 0; i < a->nwins; i++) {
        if (strcasestr(a->wins[i].title, sub) || strcasestr(a->wins[i].cls, sub)) {
            hits++;
            if (!found) found = &a->wins[i];
            else fprintf(stderr, "also 0x%x %s\n", a->wins[i].id, a->wins[i].title);
        }
    }
    if (hits > 1) fprintf(stderr, "matched %d windows, using 0x%x\n", hits, found ? found->id : 0);
    return found;
}

static int cmd_resize_one(App *a, WinInfo *wi, int w, int h) {
    if (!wi) {
        fprintf(stderr, "window not found\n");
        return 1;
    }
    a->unfull = 1;
    a->do_child = 1;
    char msg[320];
    printf("before %dx%d  %s\n", wi->w, wi->h, wi->title);
    int rc = apply_size(a, wi, w, h, 1, 1, msg, sizeof msg);
    printf("%s\n", msg);
    return rc == 0 ? 0 : 1;
}

static int self_check(const char *display) {
    XConn conn;
    if (x_connect(&conn, display) < 0) {
        fprintf(stderr, "self-check connect: %s\n", conn.err);
        return 1;
    }
    uint32_t mask = 0x20000u;
    uint32_t win = x_create_window(&conn, 10, 10, 400, 300, mask);
    if (!win) {
        fprintf(stderr, "self-check create: %s\n", conn.err);
        x_disconnect(&conn);
        return 1;
    }
    const char *title = "FrameSize-probe";
    if (x_change_property(&conn, win, XA_WM_NAME_, XA_STRING_, 8, title, (uint32_t)strlen(title)) < 0) {
        fprintf(stderr, "self-check name: %s\n", conn.err);
        x_disconnect(&conn);
        return 1;
    }
    if (write_fixed_hints(&conn, win, 640, 480) < 0) {
        fprintf(stderr, "self-check hints: %s\n", conn.err);
        x_disconnect(&conn);
        return 1;
    }
    int fixed = 0, minw = 0, minh = 0, maxw = 0, maxh = 0;
    read_hints(&conn, win, &fixed, &minw, &minh, &maxw, &maxh);
    if (!fixed || minw != 640 || minh != 480 || maxw != 640 || maxh != 480) {
        fprintf(stderr, "self-check hint readback fixed=%d %dx%d .. %dx%d\n", fixed, minw, minh, maxw, maxh);
        x_disconnect(&conn);
        return 1;
    }
    if (x_configure(&conn, win, 0x000c, 0, 0, 640, 480) < 0) {
        fprintf(stderr, "self-check configure: %s\n", conn.err);
        x_disconnect(&conn);
        return 1;
    }
    int x = 0, y = 0, w = 0, h = 0, ok = 0;
    for (int i = 0; i < 20; i++) {
        if (x_geometry(&conn, win, &x, &y, &w, &h) == 0 && w == 640 && h == 480) { ok = 1; break; }
        poll(NULL, 0, 30);
    }
    if (!ok) {
        fprintf(stderr, "self-check geometry stayed %dx%d (%s)\n", w, h, conn.err);
        x_disconnect(&conn);
        return 1;
    }
    uint8_t *data = NULL;
    uint32_t n = 0; uint8_t fmt = 0; uint32_t type = 0;
    if (x_property(&conn, win, XA_WM_NAME_, &data, &n, &fmt, &type) < 0 || !data || strcmp((char *)data, title) != 0) {
        fprintf(stderr, "self-check title readback failed\n");
        free(data);
        x_disconnect(&conn);
        return 1;
    }
    free(data);
    x_disconnect(&conn);
    printf("self-check ok\n");
    return 0;
}

static int probe_window(const char *display) {
    XConn conn;
    if (x_connect(&conn, display) < 0) {
        fprintf(stderr, "probe: %s\n", conn.err);
        return 1;
    }
    uint32_t win = x_create_window(&conn, 30, 30, 400, 300, 0x20000u);
    const char *title = "ProbeTarget";
    x_change_property(&conn, win, XA_WM_NAME_, XA_STRING_, 8, title, (uint32_t)strlen(title));
    x_map(&conn, win);
    fprintf(stderr, "probe 0x%x\n", win);
    for (;;) {
        XEvent ev;
        int got = x_poll(&conn, &ev, -1);
        if (got < 0) break;
        if (got && ev.type == 22) {
            int w = ev.raw[20] | (ev.raw[21] << 8);
            int h = ev.raw[22] | (ev.raw[23] << 8);
            fprintf(stderr, "probe configure %dx%d\n", w, h);
        }
    }
    x_disconnect(&conn);
    return 0;
}

static int test_wm(const char *display) {
    XConn conn;
    if (x_connect(&conn, display) < 0) {
        fprintf(stderr, "test-wm: %s\n", conn.err);
        return 1;
    }
    if (x_change_attr(&conn, conn.root, 0x0800, 0x001A0000u) < 0) {
        fprintf(stderr, "test-wm redirect: %s\n", conn.err);
        x_disconnect(&conn);
        return 1;
    }
    fprintf(stderr, "test-wm listening\n");
    for (;;) {
        XEvent ev;
        int got = x_poll(&conn, &ev, -1);
        if (got < 0) break;
        if (!got) continue;
        if (getenv("FR_DEBUG")) {
            fprintf(stderr, "wm event %u", ev.type);
            for (int i = 0; i < 32; i++) fprintf(stderr, " %02x", ev.raw[i]);
            fprintf(stderr, "\n");
        }
        if (ev.type == 23) {
            uint32_t win = get32(ev.raw + 8);
            int x = (int16_t)(ev.raw[16] | (ev.raw[17] << 8));
            int y = (int16_t)(ev.raw[18] | (ev.raw[19] << 8));
            int w = ev.raw[20] | (ev.raw[21] << 8);
            int h = ev.raw[22] | (ev.raw[23] << 8);
            uint16_t mask = (uint16_t)(ev.raw[26] | (ev.raw[27] << 8));
            x_configure(&conn, win, mask & 0x000f, x, y, w, h);
        } else if (ev.type == 20) {
            uint32_t win = get32(ev.raw + 8);
            x_map(&conn, win);
        }
    }
    x_disconnect(&conn);
    return 0;
}

static int write_ppm(const char *path, const App *a) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", a->fb_w, a->fb_h);
    for (int i = 0; i < a->fb_w * a->fb_h; i++) {
        uint32_t p = a->fb[i];
        unsigned char rgb[3] = { (p >> 16) & 255, (p >> 8) & 255, p & 255 };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return 0;
}

static int screenshot(const char *path) {
    App a;
    memset(&a, 0, sizeof a);
    a.selected = 0;
    a.focus_field = -1;
    a.unfull = 1;
    a.do_child = 1;
    a.superres = 1;
    a.root_w = 2560;
    a.root_h = 1440;
    a.have_scale = 1;
    a.screen_scale = 1.0;
    snprintf(a.display, sizeof a.display, ":0");
    snprintf(a.width_buf, sizeof a.width_buf, "1920");
    snprintf(a.height_buf, sizeof a.height_buf, "1080");
    snprintf(a.screen_w, sizeof a.screen_w, "2560");
    snprintf(a.screen_h, sizeof a.screen_h, "1440");
    set_status(&a, 0, "Select a window and apply. This changes the buffer, not the compositor scale.");
    a.nwins = 4;
    snprintf(a.wins[0].title, sizeof a.wins[0].title, "Half-Life Alyx");
    a.wins[0].id = 0x1a00007; a.wins[0].w = 1280; a.wins[0].h = 720; a.wins[0].pid = 2411;
    a.wins[0].hint_fixed = 1; a.wins[0].min_w = 1280; a.wins[0].min_h = 720;
    a.wins[0].child_w = 1280; a.wins[0].child_h = 720; a.wins[0].child = 0x1a00008;
    snprintf(a.wins[1].title, sizeof a.wins[1].title, "Steam");
    a.wins[1].id = 0x1200004; a.wins[1].w = 1280; a.wins[1].h = 800; a.wins[1].pid = 880; a.wins[1].fullscreen = 1;
    snprintf(a.wins[2].title, sizeof a.wins[2].title, "vkcube");
    a.wins[2].id = 0x2200011; a.wins[2].w = 500; a.wins[2].h = 500; a.wins[2].pid = 3302; a.wins[2].override = 1;
    snprintf(a.wins[3].title, sizeof a.wins[3].title, "portal2.exe");
    a.wins[3].id = 0x32000ab; a.wins[3].w = 1920; a.wins[3].h = 1080; a.wins[3].pid = 4100; a.wins[3].hint_fixed = 1;
    a.wins[3].min_w = 1920; a.wins[3].min_h = 1080;
    if (alloc_fb(&a, 1280, 720) < 0) return 1;
    a.mouse_x = 700; a.mouse_y = 400;
    ui_draw(&a);
    if (write_ppm(path, &a) < 0) return 1;
    printf("wrote %s\n", path);
    free(a.fb);
    return 0;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    const char *display = NULL;
    const char *mode = "gui";
    const char *name = NULL;
    const char *shot = NULL;
    uint32_t wid = 0;
    int have_size = 0, rw = 0, rh = 0;
    uint32_t server = 0;
    int have_server = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        else if (!strcmp(argv[i], "--list")) mode = "list";
        else if (!strcmp(argv[i], "--self-check")) mode = "self-check";
        else if (!strcmp(argv[i], "--probe")) mode = "probe";
        else if (!strcmp(argv[i], "--test-wm")) mode = "test-wm";
        else if (!strcmp(argv[i], "--display") && i + 1 < argc) display = argv[++i];
        else if (!strcmp(argv[i], "--resize") && i + 3 < argc) {
            mode = "resize";
            wid = (uint32_t)strtoul(argv[++i], NULL, 0);
            rw = atoi(argv[++i]); rh = atoi(argv[++i]); have_size = 1;
        } else if (!strcmp(argv[i], "--resize-name") && i + 3 < argc) {
            mode = "resize-name";
            name = argv[++i];
            rw = atoi(argv[++i]); rh = atoi(argv[++i]); have_size = 1;
        } else if (!strcmp(argv[i], "--xwayland") && i + 2 < argc) {
            mode = "xwayland";
            rw = atoi(argv[++i]); rh = atoi(argv[++i]); have_size = 1;
        } else if (!strcmp(argv[i], "--server") && i + 1 < argc) {
            server = (uint32_t)strtoul(argv[++i], NULL, 0);
            have_server = 1;
        } else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) {
            mode = "screenshot";
            shot = argv[++i];
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            usage();
            return 2;
        }
    }
    if (!strcmp(mode, "screenshot")) return screenshot(shot);
    if (!strcmp(mode, "self-check")) return self_check(display ? display : getenv("DISPLAY"));
    if (!strcmp(mode, "probe")) return probe_window(display ? display : getenv("DISPLAY"));
    if (!strcmp(mode, "test-wm")) return test_wm(display ? display : getenv("DISPLAY"));

    App a;
    memset(&a, 0, sizeof a);
    a.selected = -1;
    a.focus_field = -1;
    a.unfull = 1;
    a.do_child = 1;
    a.superres = 1;
    if (!strcmp(mode, "gui")) {
        if (display) snprintf(a.display, sizeof a.display, "%s", display);
        int rc = run_gui(&a);
        if (a.x) x_disconnect(a.x);
        free(a.x);
        free(a.fb);
        return rc;
    }
    if (connect_cli(&a, display)) return 1;
    int rc = 0;
    if (!strcmp(mode, "list")) rc = cmd_list(&a);
    else if (!strcmp(mode, "resize")) {
        if (!have_size) return 2;
        rc = cmd_resize_one(&a, find_id(&a, wid), rw, rh);
    } else if (!strcmp(mode, "resize-name")) {
        rc = cmd_resize_one(&a, find_name(&a, name), rw, rh);
    } else if (!strcmp(mode, "xwayland")) {
        if (have_server) a.server_id = server;
        char msg[320];
        set_xwayland_mode(&a, rw, rh, 1, msg, sizeof msg);
        printf("%s\n", msg);
    }
    x_disconnect(a.x);
    free(a.x);
    return rc;
}

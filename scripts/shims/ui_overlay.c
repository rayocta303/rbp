#define _GNU_SOURCE
#include "ui_overlay.h"
#include "menu_font.h"
#include "net_info.h"
#include "pointsrc.h"
#include "rbp_abi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef RTLD_DEFAULT
#define RTLD_DEFAULT ((void *) 0)
#endif
extern void *dlsym(void *handle, const char *symbol);

#define SHORTCUT_THEME_X0 960
#define SHORTCUT_THEME_X1 1220
#define SHORTCUT_THEME_Y0 8
#define SHORTCUT_THEME_Y1 44

static int s_theme_pressed = 0;

static void ov_set(const struct menu_view *v, int fx, int fy, unsigned int pv)
{
    unsigned char *row;
    if (fx < 0 || fy < 0 || fx >= v->dw || fy >= v->dh)
        return;
    row = (unsigned char *)v->pix + (size_t)fy * v->pitch * (v->bpp / 8);
    if (v->bpp == 32)
        *(unsigned int *)(row + (size_t)fx * 4) = pv;
    else
        *(unsigned short *)(row + (size_t)fx * 2) = (unsigned short)pv;
}

static unsigned int ov_blend(int bpp, unsigned int base, unsigned int ink, int cov)
{
    int br, bg, bb, ir, ig, ib;
    if (cov <= 0) return base;
    if (cov >= 255) return ink;

    if (bpp == 32) {
        br = (int)((base >> 16) & 0xffu);
        bg = (int)((base >> 8) & 0xffu);
        bb = (int)(base & 0xffu);
        ir = (int)((ink >> 16) & 0xffu);
        ig = (int)((ink >> 8) & 0xffu);
        ib = (int)(ink & 0xffu);
        return (unsigned int)(((br * (255 - cov) + ir * cov + 127) / 255) << 16 |
                              ((bg * (255 - cov) + ig * cov + 127) / 255) << 8 |
                              ((bb * (255 - cov) + ib * cov + 127) / 255));
    }
    br = (int)((base >> 11) & 0x1fu) * 255 / 31;
    bg = (int)((base >> 5) & 0x3fu) * 255 / 63;
    bb = (int)(base & 0x1fu) * 255 / 31;
    ir = (int)((ink >> 11) & 0x1fu) * 255 / 31;
    ig = (int)((ink >> 5) & 0x3fu) * 255 / 63;
    ib = (int)(ink & 0x1fu) * 255 / 31;
    return (unsigned int)(((((br * (255 - cov) + ir * cov + 127) / 255) & 0xf8) << 8) |
                          ((((bg * (255 - cov) + ig * cov + 127) / 255) & 0xfc) << 3) |
                          ((((bb * (255 - cov) + ib * cov + 127) / 255) & 0xf8) >> 3));
}

static void ov_fill(const struct menu_view *v, int x0, int y0, int x1, int y1, unsigned int pv)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > v->dw) x1 = v->dw;
    if (y1 > v->dh) y1 = v->dh;

    for (int y = y0; y <= y1; y++) {
        unsigned char *row = (unsigned char *)v->pix + (size_t)y * v->pitch * (v->bpp / 8);
        if (v->bpp == 32) {
            unsigned int *p = (unsigned int *)(row + (size_t)x0 * 4);
            for (int x = x0; x <= x1; x++)
                *p++ = pv;
        } else {
            unsigned short *p = (unsigned short *)(row + (size_t)x0 * 2);
            for (int x = x0; x <= x1; x++)
                *p++ = (unsigned short)pv;
        }
    }
}

static void ov_rect(const struct menu_view *v, int x0, int y0, int x1, int y1,
                    unsigned int fill, unsigned int border)
{
    ov_fill(v, x0, y0, x1, y1, fill);
    ov_fill(v, x0, y0, x1, y0, border);
    ov_fill(v, x0, y1, x1, y1, border);
    ov_fill(v, x0, y0, x0, y1, border);
    ov_fill(v, x1, y0, x1, y1, border);
}

static void ov_glyph(const struct menu_view *v, int pen_fx, int line_fy,
                     unsigned char c, unsigned int base, unsigned int ink,
                     int cx0, int cx1)
{
    const struct menu_glyph *g = menu_font_glyph(c);
    if (!g) return;
    int col, row;

    for (row = 0; row < (int)g->h; row++) {
        for (col = 0; col < (int)g->w; col++) {
            int fx = pen_fx + g->left + col;
            int cov = menu_font_cov(g, col, row);

            if (cov == 0 || fx < cx0 || fx >= cx1)
                continue;
            ov_set(v, fx, line_fy + g->top + row,
                   cov >= 255 ? ink : ov_blend(v->bpp, base, ink, cov));
        }
    }
}

static void ov_text(const struct menu_view *v, int pen_fx, int line_fy,
                    const char *s, unsigned int base, unsigned int ink,
                    int cx0, int cx1)
{
    if (!s) return;
    for (; *s; s++) {
        ov_glyph(v, pen_fx, line_fy, (unsigned char)*s, base, ink, cx0, cx1);
        pen_fx += menu_font_adv((unsigned char)*s, (unsigned char)s[1]);
        if (pen_fx >= cx1) break;
    }
}

static void ov_text_center(const struct menu_view *v, int x0, int y0, int x1, int y1,
                           const char *s, unsigned int base, unsigned int ink)
{
    if (!s) return;
    int w = menu_text_width(s);
    int line_y = y0 + ((y1 - y0) - MENU_FONT_LINE) / 2;
    int pen_x = (x0 + x1 - w) / 2;
    ov_text(v, pen_x, line_y, s, base, ink, x0, x1);
}

typedef int (*rx3_theme_is_light_fn)(void);
typedef void (*rx3_theme_toggle_fn)(void);

static int ov_theme_is_light(void)
{
    rx3_theme_is_light_fn fn = (rx3_theme_is_light_fn)dlsym(RTLD_DEFAULT, "rx3_theme_is_light");
    if (fn) return fn();

    /* Fallback: read /tmp/rx3-theme.state */
    FILE *f = fopen("/tmp/rx3-theme.state", "r");
    if (f) {
        char ch = 0;
        if (fread(&ch, 1, 1, f) == 1) {
            fclose(f);
            return (ch == '1');
        }
        fclose(f);
    }
    return 0;
}

static void ov_theme_toggle(void)
{
    rx3_theme_toggle_fn fn = (rx3_theme_toggle_fn)dlsym(RTLD_DEFAULT, "rx3_theme_toggle");
    if (fn) {
        fn();
        pointsrc_log("ui_overlay: theme toggled via direct function");
    } else {
        FILE *f = fopen("/tmp/rx3-theme.toggle", "w");
        if (f) {
            fputs("1\n", f);
            fclose(f);
            pointsrc_log("ui_overlay: theme toggled via /tmp/rx3-theme.toggle");
        }
    }
}

void ui_overlay_draw(const struct menu_view *v, int browse_mode)
{
    if (!v || !v->pix || v->dw < 1280 || v->dh < 800)
        return;

    char ip[64], host[64], info_str[128];
    get_system_net_info(ip, sizeof(ip), host, sizeof(host));

    unsigned int bg_dark   = 0x0014161a;
    unsigned int ink_green = 0x0044dd66;

    if (browse_mode == BROWSE_MODE_UTILITY) {
        /* UTILITY screen (mode 7): Header bar spans y 0..49.
         * Left has "UTILITY". Right space (x 430..1270, y 8..44) displays IP info. */
        if (host[0])
            snprintf(info_str, sizeof(info_str), "IP: %s  (%s)", ip, host);
        else
            snprintf(info_str, sizeof(info_str), "IP: %s", ip);

        int line_fy = 8 + ((42 - 8) - MENU_FONT_LINE) / 2;
        ov_text(v, 430, line_fy, info_str, bg_dark, ink_green, 430, 1270);
    } else if (browse_mode == 10) {
        /* SHORTCUT screen (mode 10):
         * Place Theme Toggle button at x: 960..1220, y: 8..44 (top right header area).
         */
        int is_light = ov_theme_is_light();
        const char *label = is_light ? "THEME: LIGHT" : "THEME: DARK";

        unsigned int fill_col = is_light ? 0x00dedbde : 0x0020242c;
        unsigned int bdr_col  = s_theme_pressed ? 0x0033ccff : (is_light ? 0x000088cc : 0x0000a0ff);
        unsigned int text_col = is_light ? 0x001c1c1e : 0x00ffffff;

        if (v->bpp == 16) {
            fill_col = is_light ? 0xdedb : 0x2124;
            bdr_col  = s_theme_pressed ? 0x367f : (is_light ? 0x0459 : 0x051f);
            text_col = is_light ? 0x18e3 : 0xffff;
        }

        ov_rect(v, SHORTCUT_THEME_X0, SHORTCUT_THEME_Y0,
                SHORTCUT_THEME_X1, SHORTCUT_THEME_Y1, fill_col, bdr_col);
        ov_text_center(v, SHORTCUT_THEME_X0, SHORTCUT_THEME_Y0,
                       SHORTCUT_THEME_X1, SHORTCUT_THEME_Y1,
                       label, fill_col, text_col);
    }
}

int ui_overlay_touch(int down, int x, int y, int browse_mode)
{
    if (browse_mode == 10) {
        if (x >= SHORTCUT_THEME_X0 && x <= SHORTCUT_THEME_X1 &&
            y >= SHORTCUT_THEME_Y0 && y <= SHORTCUT_THEME_Y1) {
            if (down) {
                s_theme_pressed = 1;
            } else {
                if (s_theme_pressed) {
                    s_theme_pressed = 0;
                    ov_theme_toggle();
                }
            }
            return 1;
        }
        if (!down) {
            s_theme_pressed = 0;
        }
    }
    return 0;
}

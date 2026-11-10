#define _GNU_SOURCE
#include "ui_overlay.h"
#include "menu_font.h"
#include "net_info.h"
#include "pointsrc.h"
#include "rbp_abi.h"
#include "rbp_key.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef RTLD_DEFAULT
#define RTLD_DEFAULT ((void *) 0)
#endif
extern void *dlsym(void *handle, const char *symbol);

#define SHORTCUT_CARD_X0       792
#define SHORTCUT_CARD_Y0        89
#define SHORTCUT_CARD_X1      1259
#define SHORTCUT_CARD_Y1       198

#define SHORTCUT_BTN_DARK_X0    812
#define SHORTCUT_BTN_DARK_X1   1015
#define SHORTCUT_BTN_LIGHT_X0  1035
#define SHORTCUT_BTN_LIGHT_X1  1238
#define SHORTCUT_BTNS_Y0        144
#define SHORTCUT_BTNS_Y1        190

static int s_theme_pressed = 0;

static unsigned int ov_color(int bpp, int r, int g, int b)
{
    if (bpp == 32)
        return (unsigned int)((r << 16) | (g << 8) | b);
    return (unsigned int)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

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
static int s_theme_is_light = 0;

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

    /* Fallback: check /tmp/rx3-theme.off */
    if (access("/tmp/rx3-theme.off", F_OK) == 0) {
        return 0; /* Forced dark */
    }
    return s_theme_is_light;
}

static void ov_theme_toggle(void)
{
    s_theme_is_light = !s_theme_is_light;

    rx3_theme_toggle_fn fn = (rx3_theme_toggle_fn)dlsym(RTLD_DEFAULT, "rx3_theme_toggle");
    if (fn) {
        fn();
        pointsrc_log("ui_overlay: theme toggled via direct function");
    }

    FILE *f = fopen("/tmp/rx3-theme.toggle", "w");
    if (f) {
        fputs("1\n", f);
        fclose(f);
        pointsrc_log("ui_overlay: theme toggled via /tmp/rx3-theme.toggle");
    }

    /* Send native SHIFT + SHORTCUT key sequence to trigger theme-white's toggle */
    send_rx_key(0x4103, OP_PRESS, 1, 0);
    send_rx_key(K_SHORTCUT, OP_PRESS, CH_GLOBAL, 0);
    send_rx_key(K_SHORTCUT, OP_RELEASE, CH_GLOBAL, 0);
    send_rx_key(0x4103, OP_RELEASE, 1, 0);
    pointsrc_log("ui_overlay: theme toggled via SHIFT+SHORTCUT key sequence");

    /* Also toggle /tmp/rx3-theme.off sentinel */
    if (access("/tmp/rx3-theme.off", F_OK) == 0) {
        unlink("/tmp/rx3-theme.off");
        pointsrc_log("ui_overlay: removed /tmp/rx3-theme.off (switch to light)");
    } else {
        FILE *fo = fopen("/tmp/rx3-theme.off", "w");
        if (fo) {
            fputs("off\n", fo);
            fclose(fo);
            pointsrc_log("ui_overlay: created /tmp/rx3-theme.off (switch to dark)");
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
         * Overwrite JOG LCD card at x: 792..1259, y: 89..198 with Theme Toggle card.
         * Matches native Pioneer SHORTCUT card aesthetics:
         *   - Top accent bar (amber/gold) at y: 89..91
         *   - Card background: #212021 (33, 32, 33)
         *   - Card border: #393c39 (57, 60, 57)
         *   - Title: "THEME" at x: 812, y: 108 in #949694 (148, 150, 148)
         *   - Divider line at y: 138
         *   - Segmented buttons: [ DARK ] (812..1015) and [ LIGHT ] (1035..1238) at y: 144..190
         */
        int is_light = ov_theme_is_light();

        unsigned int card_bg   = ov_color(v->bpp, 33, 32, 33);
        unsigned int card_bdr  = ov_color(v->bpp, 57, 60, 57);
        unsigned int card_top  = ov_color(v->bpp, 99, 48, 16);
        unsigned int title_col = ov_color(v->bpp, 148, 150, 148);

        /* Fill card background and border */
        ov_fill(v, SHORTCUT_CARD_X0, SHORTCUT_CARD_Y0 + 3, SHORTCUT_CARD_X1, SHORTCUT_CARD_Y1, card_bg);
        ov_fill(v, SHORTCUT_CARD_X0, SHORTCUT_CARD_Y0, SHORTCUT_CARD_X1, SHORTCUT_CARD_Y0 + 2, card_top);
        ov_rect(v, SHORTCUT_CARD_X0, SHORTCUT_CARD_Y0, SHORTCUT_CARD_X1, SHORTCUT_CARD_Y1, card_bg, card_bdr);

        /* Divider line under title */
        ov_fill(v, SHORTCUT_CARD_X0, 138, SHORTCUT_CARD_X1, 138, card_bdr);

        /* Card Title: THEME */
        ov_text(v, 812, 108, "THEME", card_bg, title_col, 812, 1000);

        /* Segmented buttons: [ DARK ] and [ LIGHT ] */
        unsigned int dark_fill, dark_bdr, dark_text;
        unsigned int light_fill, light_bdr, light_text;

        if (!is_light) {
            /* DARK is active */
            dark_fill  = (s_theme_pressed == 1) ? ov_color(v->bpp, 0, 80, 140) : ov_color(v->bpp, 8, 40, 57);
            dark_bdr   = ov_color(v->bpp, 0, 160, 255);
            dark_text  = ov_color(v->bpp, 255, 255, 255);

            light_fill = (s_theme_pressed == 2) ? ov_color(v->bpp, 40, 40, 40) : ov_color(v->bpp, 24, 24, 24);
            light_bdr  = ov_color(v->bpp, 57, 60, 57);
            light_text = ov_color(v->bpp, 148, 150, 148);
        } else {
            /* LIGHT is active */
            dark_fill  = (s_theme_pressed == 1) ? ov_color(v->bpp, 40, 40, 40) : ov_color(v->bpp, 24, 24, 24);
            dark_bdr   = ov_color(v->bpp, 57, 60, 57);
            dark_text  = ov_color(v->bpp, 148, 150, 148);

            light_fill = (s_theme_pressed == 2) ? ov_color(v->bpp, 0, 120, 180) : ov_color(v->bpp, 0, 136, 204);
            light_bdr  = ov_color(v->bpp, 0, 160, 255);
            light_text = ov_color(v->bpp, 255, 255, 255);
        }

        ov_rect(v, SHORTCUT_BTN_DARK_X0, SHORTCUT_BTNS_Y0,
                SHORTCUT_BTN_DARK_X1, SHORTCUT_BTNS_Y1, dark_fill, dark_bdr);
        ov_text_center(v, SHORTCUT_BTN_DARK_X0, SHORTCUT_BTNS_Y0,
                       SHORTCUT_BTN_DARK_X1, SHORTCUT_BTNS_Y1,
                       "DARK", dark_fill, dark_text);

        ov_rect(v, SHORTCUT_BTN_LIGHT_X0, SHORTCUT_BTNS_Y0,
                SHORTCUT_BTN_LIGHT_X1, SHORTCUT_BTNS_Y1, light_fill, light_bdr);
        ov_text_center(v, SHORTCUT_BTN_LIGHT_X0, SHORTCUT_BTNS_Y0,
                       SHORTCUT_BTN_LIGHT_X1, SHORTCUT_BTNS_Y1,
                       "LIGHT", light_fill, light_text);
    }
}

int ui_overlay_touch(int down, int x, int y, int browse_mode)
{
    if (browse_mode == 10) {
        if (y >= SHORTCUT_BTNS_Y0 && y <= SHORTCUT_BTNS_Y1) {
            if (x >= SHORTCUT_BTN_DARK_X0 && x <= SHORTCUT_BTN_DARK_X1) {
                if (down) {
                    s_theme_pressed = 1;
                } else {
                    if (s_theme_pressed == 1) {
                        s_theme_pressed = 0;
                        if (ov_theme_is_light()) {
                            ov_theme_toggle();
                        }
                    }
                }
                return 1;
            } else if (x >= SHORTCUT_BTN_LIGHT_X0 && x <= SHORTCUT_BTN_LIGHT_X1) {
                if (down) {
                    s_theme_pressed = 2;
                } else {
                    if (s_theme_pressed == 2) {
                        s_theme_pressed = 0;
                        if (!ov_theme_is_light()) {
                            ov_theme_toggle();
                        }
                    }
                }
                return 1;
            }
        }
        if (!down) {
            s_theme_pressed = 0;
        }
    }
    return 0;
}

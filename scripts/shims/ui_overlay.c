#define _GNU_SOURCE
#include "ui_overlay.h"
#include "menu_font.h"
#include "net_info.h"
#include "pointsrc.h"
#include "rbp_abi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void ov_glyph(const struct menu_view *v, int pen_fx, int line_fy,
                     unsigned char c, unsigned int base, unsigned int ink,
                     int cx0, int cx1)
{
    const struct menu_glyph *g = menu_font_glyph(c);
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
    if (!s)
        return;
    for (; *s; s++) {
        ov_glyph(v, pen_fx, line_fy, (unsigned char)*s, base, ink, cx0, cx1);
        pen_fx += menu_font_adv((unsigned char)*s, (unsigned char)s[1]);
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
    }
}

int ui_overlay_touch(int down, int x, int y, int browse_mode)
{
    (void)down;
    (void)x;
    (void)y;
    (void)browse_mode;
    return 0;
}

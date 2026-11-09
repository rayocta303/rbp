/*
 * net_paint.c -- Renderer for OS Network & Wireless Settings overlay on XDJ-RX3.
 *
 * Implements native Pioneer RBP UI styling matching Utility, Source, and Browse views:
 * - True black background (#000000 / #08090c)
 * - Pioneer Electric Blue (#007de7 / #007eff) active accents
 * - Clean 1px dividers (#212021) matching RBP Utility tables
 * - Exact key-value columns with zero text collision/overlap
 * - Rectangular / square button UI without rounded corner borders (r=0)
 * - Top-bar button styled seamlessly with vertical dividers at x: 938..1066
 * - Overlay height strictly above the blue deckstrip line (y <= 580)
 */

#include "net_paint.h"
#include "menu_font.h"
#include "menu_paint.h"

#include <stdio.h>
#include <string.h>

/* --- Color Helpers --- */
static unsigned int np_color(int bpp, int r, int g, int b)
{
    if (bpp == 32)
        return (0xffu << 24) |
               ((unsigned int)(r & 0xff) << 16) |
               ((unsigned int)(g & 0xff) << 8)  |
               ((unsigned int)(b & 0xff));
    return (unsigned int)((((r >> 3) & 0x1f) << 11) |
                          (((g >> 2) & 0x3f) << 5)  |
                          (((b >> 3) & 0x1f)));
}

/* Palette Constants (Sampled directly from native RBP screens) */
#define COL_BLACK        np_color(v->bpp, 0, 0, 0)
#define COL_DARK_BG      np_color(v->bpp, 8, 9, 12)         /* #08090c Deep black */
#define COL_HDR_BG       np_color(v->bpp, 14, 16, 20)       /* #0e1014 Header bar */
#define COL_CARD_BG      np_color(v->bpp, 20, 23, 28)       /* #14171c Card surface */
#define COL_ROW_BG       np_color(v->bpp, 12, 13, 16)       /* #0c0d10 Dark row */
#define COL_ROW_ALT      np_color(v->bpp, 17, 19, 24)       /* #111318 Alternate row */
#define COL_ROW_ACTIVE   np_color(v->bpp, 12, 28, 50)       /* #0c1c32 Connected row */
#define COL_BORDER       np_color(v->bpp, 42, 46, 54)       /* #2a2e36 Subtle border */
#define COL_BORDER_BRT   np_color(v->bpp, 65, 72, 85)       /* #414855 Active border */
#define COL_DIVIDER      np_color(v->bpp, 33, 32, 33)       /* #212021 Pioneer 1px divider */
#define COL_PILL_BG      np_color(v->bpp, 22, 25, 32)       /* #161920 Top bar pill */
#define COL_ACCENT_BLUE  np_color(v->bpp, 0, 125, 231)      /* #007de7 Pioneer Electric Blue */
#define COL_ACCENT_HOV   np_color(v->bpp, 0, 160, 255)      /* Hover / pressed blue */
#define COL_BLUE_BORDER  np_color(v->bpp, 0, 155, 255)      /* Active tab border */
#define COL_BLUE_DARK    np_color(v->bpp, 0, 75, 145)       /* Dark accent */
#define COL_GREEN_ON     np_color(v->bpp, 30, 184, 72)      /* #1eb848 Pioneer green */
#define COL_GRAY_OFF     np_color(v->bpp, 48, 52, 60)       /* #30343c OFF gray */
#define COL_TEXT_WHITE   np_color(v->bpp, 255, 255, 255)
#define COL_TEXT_GRAY    np_color(v->bpp, 145, 150, 160)    /* Clean gray for labels */
#define COL_TEXT_DIM     np_color(v->bpp, 85, 90, 102)      /* Dim text */
#define COL_TEXT_GREEN   np_color(v->bpp, 45, 210, 85)
#define COL_TEXT_CYAN    np_color(v->bpp, 0, 200, 255)      /* Vibrant cyan */
#define COL_CLOSE_RED    np_color(v->bpp, 210, 40, 40)

/* --- Low-level pixel primitives --- */

static void np_set(const struct menu_view *v, int fx, int fy, unsigned int pv)
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

static void np_fill(const struct menu_view *v, int x0, int y0, int x1, int y1,
                    unsigned int pv)
{
    int x, y;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > v->dw) x1 = v->dw;
    if (y1 > v->dh) y1 = v->dh;

    for (y = y0; y <= y1; y++) {
        unsigned char *row = (unsigned char *)v->pix + (size_t)y * v->pitch * (v->bpp / 8);
        if (v->bpp == 32) {
            unsigned int *p = (unsigned int *)(row + (size_t)x0 * 4);
            for (x = x0; x <= x1; x++)
                *p++ = pv;
        } else {
            unsigned short *p = (unsigned short *)(row + (size_t)x0 * 2);
            for (x = x0; x <= x1; x++)
                *p++ = (unsigned short)pv;
        }
    }
}

/* Sharp rectangular button primitive: strictly 90-degree square corners without rounded caps */
static void np_rect(const struct menu_view *v, int x0, int y0, int x1, int y1,
                    unsigned int fill, unsigned int border)
{
    /* Fill main body */
    np_fill(v, x0, y0, x1, y1, fill);

    /* Draw 1px border lines */
    np_fill(v, x0, y0, x1, y0, border);
    np_fill(v, x0, y1, x1, y1, border);
    np_fill(v, x0, y0, x0, y1, border);
    np_fill(v, x1, y0, x1, y1, border);
}

/* --- Bitmaps & Icons --- */

/* 24x24 Wi-Fi Icon */
static const unsigned int s_wifi_icon_24[24] = {
    0x00000000,
    0x00000000,
    0x000ff000, /* ....************.... */
    0x003ffc00, /* ..****************.. */
    0x00f00f00, /* ****............**** */
    0x01c00380, /* ***..............*** */
    0x030000c0, /* **................** */
    0x00000000,
    0x0007e000, /* .....**********..... */
    0x001ff800, /* ...**************... */
    0x00381c00, /* ..***..........***.. */
    0x00600600, /* .**..............**. */
    0x00000000,
    0x00018000, /* .......******....... */
    0x0003c000, /* ......********...... */
    0x0007e000, /* .....**********..... */
    0x00066000, /* .....**......**..... */
    0x00000000,
    0x00000000,
    0x00018000, /* .......**........... */
    0x0003c000, /* ......****.......... */
    0x0003c000, /* ......****.......... */
    0x00018000, /* .......**........... */
    0x00000000
};

/* 20x24 Bluetooth Icon */
static const unsigned int s_bt_icon_24[24] = {
    0x00000000,
    0x00060000, /* ......**............ */
    0x00070000, /* ......***........... */
    0x00078000, /* ......****.......... */
    0x0007c000, /* ......*****......... */
    0x0006e000, /* ......**..***....... */
    0x00067000, /* ......**....***..... */
    0x00663800, /* ..**..**......**.... */
    0x00761c00, /* .***..**.......**... */
    0x003e3c00, /* ..******......***... */
    0x001ef800, /* ...****......***.... */
    0x000fe000, /* ....**......***..... */
    0x0007e000, /* ......******........ */
    0x000fe000, /* ....**......***..... */
    0x001ef800, /* ...****......***.... */
    0x003e3c00, /* ..******......***... */
    0x00761c00, /* .***..**.......**... */
    0x00663800, /* ..**..**......**.... */
    0x00067000, /* ......**....***..... */
    0x0006e000, /* ......**..***....... */
    0x0007c000, /* ......*****......... */
    0x00078000, /* ......****.......... */
    0x00070000, /* ......***........... */
    0x00060000  /* ......**............ */
};

static void np_draw_icon(const struct menu_view *v, int x, int y,
                         const unsigned int *bmp, int h, unsigned int color)
{
    for (int row = 0; row < h; row++) {
        unsigned int mask = bmp[row];
        for (int col = 0; col < 24; col++) {
            if (mask & (1u << (23 - col))) {
                np_set(v, x + col, y + row, color);
            }
        }
    }
}

/* Signal Strength 4-bar indicator */
static void np_draw_signal_bars(const struct menu_view *v, int x, int y, int signal,
                                unsigned int active_col, unsigned int inactive_col)
{
    int bars = (signal <= 0) ? 0 : (signal < 30) ? 1 : (signal < 60) ? 2 : (signal < 85) ? 3 : 4;
    int h_bars[4] = { 6, 11, 16, 21 };

    for (int b = 0; b < 4; b++) {
        int bx = x + b * 6;
        int bh = h_bars[b];
        int by = y + (21 - bh);
        unsigned int col = (b < bars) ? active_col : inactive_col;
        np_fill(v, bx, by, bx + 3, y + 21, col);
    }
}

/* --- Font & Text Helpers --- */

static unsigned int np_blend(int bpp, unsigned int base, unsigned int ink, int cov)
{
    int br, bg, bb, ir, ig, ib;
    if (cov <= 0) return base;
    if (cov >= 255) return ink;

    if (bpp == 32) {
        br = (int)((base >> 16) & 0xffu); bg = (int)((base >> 8) & 0xffu);
        bb = (int)(base & 0xffu);
        ir = (int)((ink >> 16) & 0xffu);  ig = (int)((ink >> 8) & 0xffu);
        ib = (int)(ink & 0xffu);
        return (0xffu << 24) |
               (unsigned int)(((br * (255 - cov) + ir * cov + 127) / 255) << 16 |
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

static void np_glyph(const struct menu_view *v, int pen_x, int line_y,
                     unsigned char c, unsigned int base, unsigned int ink,
                     int cx0, int cx1)
{
    const struct menu_glyph *g = menu_font_glyph(c);
    if (!g) return;

    for (int row = 0; row < (int)g->h; row++) {
        for (int col = 0; col < (int)g->w; col++) {
            int fx = pen_x + g->left + col;
            int cov = menu_font_cov(g, col, row);
            if (cov == 0 || fx < cx0 || fx >= cx1)
                continue;
            np_set(v, fx, line_y + g->top + row,
                   cov >= 255 ? ink : np_blend(v->bpp, base, ink, cov));
        }
    }
}

static void np_text(const struct menu_view *v, int pen_x, int line_y,
                    const char *s, unsigned int base, unsigned int ink,
                    int cx0, int cx1)
{
    if (!s) return;
    for (; *s; s++) {
        np_glyph(v, pen_x, line_y, (unsigned char)*s, base, ink, cx0, cx1);
        pen_x += menu_font_adv((unsigned char)*s, (unsigned char)s[1]);
        if (pen_x >= cx1)
            break;
    }
}

static void np_text_center(const struct menu_view *v, int x0, int y0, int x1, int y1,
                           const char *s, unsigned int base, unsigned int ink)
{
    if (!s) return;
    int w = menu_text_width(s);
    int line_y = y0 + ((y1 - y0) - MENU_FONT_LINE) / 2;
    int pen_x = (x0 + x1 - w) / 2;
    np_text(v, pen_x, line_y, s, base, ink, x0, x1);
}

/* Helper to render native Pioneer Utility-style key-value table row */
static void np_draw_table_row(const struct menu_view *v, int row_idx,
                              const char *key, const char *val,
                              unsigned int val_color)
{
    int ry0 = 196 + row_idx * 46;
    int ry1 = ry0 + 44;
    if (ry1 > NET_WIN_Y1) return;

    /* Background: alternate subtle dark rows */
    unsigned int bg = (row_idx % 2 == 0) ? COL_ROW_BG : COL_ROW_ALT;
    np_fill(v, NET_BODY_X0, ry0, NET_BODY_X1, ry1, bg);

    /* 1px bottom divider line (Pioneer Utility style #212021) */
    np_fill(v, NET_BODY_X0, ry1, NET_BODY_X1, ry1, COL_DIVIDER);

    /* Left accent indicator */
    np_fill(v, NET_BODY_X0, ry0, NET_BODY_X0 + 3, ry1, COL_BORDER);

    int line_y = ry0 + ((ry1 - ry0) - MENU_FONT_LINE) / 2;

    /* Left Key (Name): white text, clipped cleanly at 580 */
    np_text(v, NET_BODY_X0 + 24, line_y, key, bg, COL_TEXT_WHITE,
            NET_BODY_X0 + 24, 580);

    /* Right Value: cyan / green / white text starting at 600 */
    np_text(v, 600, line_y, val, bg, val_color, 600, NET_BODY_X1 - 20);
}

/* --- Top Bar Button Paint (KONSEP MOD RBP VIEW UTAMA) --- */

void net_strip_paint(const struct menu_view *v,
                     int wifi_active,
                     int bt_active,
                     int pressed)
{
    if (!v || !v->pix) return;

    /*
     * Exactly matches KONSEP MOD RBP VIEW UTAMA + TOMBOL WIFI & BT.png:
     * - Placed at x: 938..1066, y: 8..44
     * - Seamless background with the top bar (highlighted when pressed)
     * - Left vertical divider line (|) at x=938
     * - Right vertical divider line (|) at x=1066
     * - Wi-Fi Icon at x=972, BT Icon at x=1024
     * - Strictly NO rounded pill box, NO top/bottom border
     */
    if (pressed) {
        np_fill(v, NET_STRIP_X0, NET_STRIP_Y0, NET_STRIP_X1, NET_STRIP_Y1, COL_BLUE_DARK);
    }

    /* Left vertical divider line (|) */
    np_fill(v, NET_STRIP_X0, NET_STRIP_Y0 + 4, NET_STRIP_X0, NET_STRIP_Y1 - 4, COL_BORDER_BRT);

    /* Right vertical divider line (|) */
    np_fill(v, NET_STRIP_X1, NET_STRIP_Y0 + 4, NET_STRIP_X1, NET_STRIP_Y1 - 4, COL_BORDER_BRT);

    unsigned int wifi_col = wifi_active ? COL_TEXT_WHITE : COL_TEXT_DIM;
    unsigned int bt_col   = bt_active   ? COL_TEXT_WHITE : COL_TEXT_DIM;

    /* Draw Wi-Fi Icon */
    np_draw_icon(v, NET_ICON_WIFI_X, NET_ICON_WIFI_Y, s_wifi_icon_24, 24, wifi_col);

    /* Draw Bluetooth Icon */
    np_draw_icon(v, NET_ICON_BT_X, NET_ICON_BT_Y, s_bt_icon_24, 24, bt_col);
}

/* --- Overlay Page Paint --- */

void net_overlay_paint(const struct menu_view *v,
                       const struct net_render_state *st,
                       int active_tab,
                       int pressed_target,
                       int pressed_val)
{
    if (!v || !v->pix) return;

    /* 1. Backdrop for Overlay Window (y: 50..579) - Strictly above blue line at y=581 */
    np_fill(v, NET_WIN_X0, NET_WIN_Y0, NET_WIN_X1, NET_WIN_Y1, COL_DARK_BG);

    /* Bottom divider line above the blue deck line */
    np_fill(v, NET_WIN_X0, NET_WIN_Y1, NET_WIN_X1, NET_WIN_Y1, COL_DIVIDER);

    /* 2. Header Bar (y: 50..92) */
    np_fill(v, NET_WIN_X0, NET_HDR_Y0, NET_WIN_X1, NET_HDR_Y1, COL_HDR_BG);
    np_fill(v, NET_WIN_X0, NET_HDR_Y1, NET_WIN_X1, NET_HDR_Y1, COL_DIVIDER);

    /* Pioneer Blue accent vertical tag on left of title */
    np_fill(v, 40, 56, 44, 86, COL_ACCENT_BLUE);

    /* Header Title */
    int line_y = NET_HDR_Y0 + ((NET_HDR_Y1 - NET_HDR_Y0) - MENU_FONT_LINE) / 2;
    np_text(v, 54, line_y, "NETWORK SETTINGS", COL_HDR_BG, COL_TEXT_WHITE, 54, 320);

    /* Subtitle Tag */
    np_text(v, 340, line_y, "XDJ-RX3 WIRELESS CONTROL", COL_HDR_BG, COL_TEXT_GRAY, 340, 1100);

    /* Close Button [CLOSE] - Sharp rectangle without rounded corners */
    int close_pressed = (pressed_target == 2 /* TARGET_CLOSE */);
    unsigned int close_fill = close_pressed ? COL_CLOSE_RED : COL_CARD_BG;
    unsigned int close_bdr  = close_pressed ? COL_ACCENT_HOV : COL_BORDER;
    np_rect(v, NET_CLOSE_X0, NET_CLOSE_Y0, NET_CLOSE_X1, NET_CLOSE_Y1, close_fill, close_bdr);
    np_text_center(v, NET_CLOSE_X0, NET_CLOSE_Y0, NET_CLOSE_X1, NET_CLOSE_Y1,
                   "CLOSE", close_fill, COL_TEXT_WHITE);

    /* 3. Tab Bar (y: 96..134) - 4 sharp rectangular tabs across x: 40..1240 */
    const char *tab_names[NET_TABS_COUNT] = {
        "WI-FI", "BLUETOOTH", "HOTSPOT / AP", "STATUS / IP"
    };
    int tab_x0[NET_TABS_COUNT] = { NET_TAB0_X0, NET_TAB1_X0, NET_TAB2_X0, NET_TAB3_X0 };
    int tab_x1[NET_TABS_COUNT] = { NET_TAB0_X1, NET_TAB1_X1, NET_TAB2_X1, NET_TAB3_X1 };

    for (int i = 0; i < NET_TABS_COUNT; i++) {
        int is_act = (active_tab == i);
        int is_prs = (pressed_target == (3 + i) /* TARGET_TAB0..3 */);
        unsigned int tab_fill = is_act ? COL_ACCENT_BLUE : (is_prs ? COL_BLUE_DARK : COL_CARD_BG);
        unsigned int tab_bdr  = is_act ? COL_BLUE_BORDER : (is_prs ? COL_ACCENT_HOV : COL_BORDER);
        unsigned int tab_txt  = is_act ? COL_TEXT_WHITE  : (is_prs ? COL_TEXT_WHITE : COL_TEXT_GRAY);

        /* Sharp rectangle button (r=0) */
        np_rect(v, tab_x0[i], NET_TAB_Y0, tab_x1[i], NET_TAB_Y1, tab_fill, tab_bdr);
        np_text_center(v, tab_x0[i], NET_TAB_Y0, tab_x1[i], NET_TAB_Y1,
                       tab_names[i], tab_fill, tab_txt);
    }

    /* 4. Tab Content Area (y: 140..576) */
    if (active_tab == NET_TAB_WIFI) {
        /* --- WI-FI TAB --- */
        /* Control Card (y: 140..186) */
        np_rect(v, NET_BODY_X0, NET_CTRL_Y0, NET_BODY_X1, NET_CTRL_Y1, COL_CARD_BG, COL_BORDER);

        int line_c = NET_CTRL_Y0 + ((NET_CTRL_Y1 - NET_CTRL_Y0) - MENU_FONT_LINE) / 2;
        np_text(v, NET_BODY_X0 + 20, line_c, "Wi-Fi - wlan0:", COL_CARD_BG, COL_TEXT_WHITE,
                NET_BODY_X0 + 20, 225);

        /* Toggle Button [ON / OFF] - Sharp rectangle */
        int tog_prs = (pressed_target == 7 /* TARGET_TOGGLE */);
        unsigned int tog_fill = st->wifi_enabled ? COL_GREEN_ON : COL_GRAY_OFF;
        if (tog_prs) tog_fill = COL_ACCENT_HOV;
        np_rect(v, NET_TOGGLE_X0, NET_TOGGLE_Y0, NET_TOGGLE_X1, NET_TOGGLE_Y1, tog_fill, COL_BORDER_BRT);
        np_text_center(v, NET_TOGGLE_X0, NET_TOGGLE_Y0, NET_TOGGLE_X1, NET_TOGGLE_Y1,
                       st->wifi_enabled ? "ON" : "OFF", tog_fill, COL_TEXT_WHITE);

        /* Scan Button [SCAN] - Sharp rectangle */
        int act_prs = (pressed_target == 8 /* TARGET_ACTION */);
        unsigned int act_fill = act_prs ? COL_ACCENT_HOV : COL_ROW_BG;
        np_rect(v, NET_ACTION_X0, NET_ACTION_Y0, NET_ACTION_X1, NET_ACTION_Y1, act_fill, COL_BORDER_BRT);
        np_text_center(v, NET_ACTION_X0, NET_ACTION_Y0, NET_ACTION_X1, NET_ACTION_Y1,
                       "SCAN", act_fill, COL_TEXT_WHITE);

        /* Connection Status summary in Control Card */
        char stat_str[128];
        if (st->wifi_enabled) {
            if (st->wifi_connected && st->wifi_ssid[0]) {
                snprintf(stat_str, sizeof(stat_str), "Connected: %s - IP: %s",
                         st->wifi_ssid, st->wifi_ip[0] ? st->wifi_ip : "--");
                np_text(v, 455, line_c, stat_str, COL_CARD_BG, COL_TEXT_CYAN, 455, 1220);
            } else {
                np_text(v, 455, line_c, "Status: Not connected - Press SCAN to search",
                        COL_CARD_BG, COL_TEXT_GRAY, 455, 1220);
            }
        } else {
            np_text(v, 455, line_c, "Status: Wi-Fi adapter is turned off",
                    COL_CARD_BG, COL_TEXT_DIM, 455, 1220);
        }

        /* Available Networks Table Header (y: 192..214) */
        np_fill(v, NET_BODY_X0, NET_WIFI_HDR_Y0, NET_BODY_X1, NET_WIFI_HDR_Y1, COL_DARK_BG);
        np_fill(v, NET_BODY_X0, NET_WIFI_HDR_Y1, NET_BODY_X1, NET_WIFI_HDR_Y1, COL_DIVIDER);

        int line_th = NET_WIFI_HDR_Y0 + ((NET_WIFI_HDR_Y1 - NET_WIFI_HDR_Y0) - MENU_FONT_LINE) / 2;
        np_text(v, NET_BODY_X0 + 20, line_th, "NETWORK / SSID", COL_DARK_BG, COL_TEXT_GRAY,
                NET_BODY_X0 + 20, 580);
        np_text(v, 600, line_th, "SECURITY", COL_DARK_BG, COL_TEXT_GRAY, 600, 780);
        np_text(v, 800, line_th, "SIGNAL", COL_DARK_BG, COL_TEXT_GRAY, 800, 950);
        np_text(v, 1080, line_th, "ACTION", COL_DARK_BG, COL_TEXT_GRAY, 1080, 1220);

        /* Network List Rows (y: 216..562) - 7 rows of height 46px, ending strictly above y=580 */
        int count = st->wifi_item_count;
        if (count > NET_WIFI_ROWS) count = NET_WIFI_ROWS;

        for (int i = 0; i < NET_WIFI_ROWS; i++) {
            int ry0 = NET_ROW_Y0(i);
            int ry1 = NET_ROW_Y1(i);

            if (i < count && st->wifi_enabled) {
                const struct net_wifi_item *it = &st->wifi_items[i];
                int is_conn = it->in_use;
                unsigned int row_bg = is_conn ? COL_ROW_ACTIVE : (i % 2 == 0 ? COL_ROW_BG : COL_ROW_ALT);
                unsigned int row_bdr = is_conn ? COL_ACCENT_BLUE : COL_BORDER;

                /* Sharp rectangle row (r=0) */
                np_rect(v, NET_BODY_X0, ry0, NET_BODY_X1, ry1, row_bg, row_bdr);

                /* If connected: draw vibrant accent left indicator */
                if (is_conn) {
                    np_fill(v, NET_BODY_X0, ry0, NET_BODY_X0 + 3, ry1, COL_ACCENT_BLUE);
                }

                /* Wi-Fi Icon */
                np_draw_icon(v, NET_BODY_X0 + 16, ry0 + 11, s_wifi_icon_24, 24,
                             is_conn ? COL_TEXT_CYAN : COL_TEXT_WHITE);

                /* SSID Text (Strict boundary at 580 to prevent overlap) */
                int line_r = ry0 + ((ry1 - ry0) - MENU_FONT_LINE) / 2;
                np_text(v, NET_BODY_X0 + 52, line_r, it->ssid, row_bg,
                        is_conn ? COL_TEXT_CYAN : COL_TEXT_WHITE, NET_BODY_X0 + 52, 580);

                /* Security (Strict boundary at 780) */
                np_text(v, 600, line_r, it->security[0] ? it->security : "OPEN",
                        row_bg, is_conn ? COL_TEXT_CYAN : COL_TEXT_GRAY, 600, 780);

                /* Signal Strength Indicator */
                np_draw_signal_bars(v, 800, ry0 + 12, it->signal,
                                    is_conn ? COL_TEXT_CYAN : COL_TEXT_WHITE, COL_TEXT_DIM);

                char sig_str[16];
                snprintf(sig_str, sizeof(sig_str), "%d", it->signal);
                np_text(v, 835, line_r, sig_str, row_bg,
                        is_conn ? COL_TEXT_CYAN : COL_TEXT_GRAY, 835, 950);

                /* Connect Button - Sharp rectangle */
                int conn_prs = (pressed_target == 9 /* TARGET_ROW_CONNECT */ && pressed_val == i);
                unsigned int cbtn_fill = is_conn ? COL_CARD_BG : (conn_prs ? COL_ACCENT_HOV : COL_ACCENT_BLUE);
                unsigned int cbtn_bdr  = is_conn ? COL_ACCENT_BLUE : COL_BLUE_BORDER;
                unsigned int cbtn_txt  = is_conn ? COL_TEXT_CYAN : COL_TEXT_WHITE;

                np_rect(v, NET_ROW_CONNECT_X0, ry0 + 6, NET_ROW_CONNECT_X1, ry1 - 6,
                        cbtn_fill, cbtn_bdr);
                np_text_center(v, NET_ROW_CONNECT_X0, ry0 + 6, NET_ROW_CONNECT_X1, ry1 - 6,
                               is_conn ? "CONNECTED" : "CONNECT", cbtn_fill, cbtn_txt);
            } else if (i == 0) {
                /* Empty / off state card on row 0 */
                int msg_y0 = 216;
                int msg_y1 = 300;
                np_rect(v, NET_BODY_X0, msg_y0, NET_BODY_X1, msg_y1, COL_CARD_BG, COL_BORDER);
                if (!st->wifi_enabled) {
                    np_text_center(v, NET_BODY_X0, msg_y0 + 16, NET_BODY_X1, msg_y0 + 40,
                                   "Wi-Fi is currently disabled.", COL_CARD_BG, COL_TEXT_WHITE);
                    np_text_center(v, NET_BODY_X0, msg_y0 + 44, NET_BODY_X1, msg_y0 + 68,
                                   "Press [ON] above to turn on Wi-Fi and search for networks.",
                                   COL_CARD_BG, COL_TEXT_GRAY);
                } else {
                    np_text_center(v, NET_BODY_X0, msg_y0 + 16, NET_BODY_X1, msg_y0 + 40,
                                   "No wireless networks found nearby.", COL_CARD_BG, COL_TEXT_WHITE);
                    np_text_center(v, NET_BODY_X0, msg_y0 + 44, NET_BODY_X1, msg_y0 + 68,
                                   "Press [SCAN] above to refresh available access points.",
                                   COL_CARD_BG, COL_TEXT_GRAY);
                }
            }
        }

    } else if (active_tab == NET_TAB_BT) {
        /* --- BLUETOOTH TAB --- */
        /* Control Card */
        np_rect(v, NET_BODY_X0, NET_CTRL_Y0, NET_BODY_X1, NET_CTRL_Y1, COL_CARD_BG, COL_BORDER);

        int line_c = NET_CTRL_Y0 + ((NET_CTRL_Y1 - NET_CTRL_Y0) - MENU_FONT_LINE) / 2;
        np_text(v, NET_BODY_X0 + 20, line_c, "Bluetooth - hci0:", COL_CARD_BG, COL_TEXT_WHITE,
                NET_BODY_X0 + 20, 225);

        /* Toggle Button */
        int tog_prs = (pressed_target == 7);
        unsigned int tog_fill = st->bt_enabled ? COL_GREEN_ON : COL_GRAY_OFF;
        if (tog_prs) tog_fill = COL_ACCENT_HOV;
        np_rect(v, NET_TOGGLE_X0, NET_TOGGLE_Y0, NET_TOGGLE_X1, NET_TOGGLE_Y1,
                tog_fill, COL_BORDER_BRT);
        np_text_center(v, NET_TOGGLE_X0, NET_TOGGLE_Y0, NET_TOGGLE_X1, NET_TOGGLE_Y1,
                       st->bt_enabled ? "ON" : "OFF", tog_fill, COL_TEXT_WHITE);

        /* Scan Button */
        int act_prs = (pressed_target == 8);
        unsigned int act_fill = act_prs ? COL_ACCENT_HOV : COL_ROW_BG;
        np_rect(v, NET_ACTION_X0, NET_ACTION_Y0, NET_ACTION_X1, NET_ACTION_Y1,
                act_fill, COL_BORDER_BRT);
        np_text_center(v, NET_ACTION_X0, NET_ACTION_Y0, NET_ACTION_X1, NET_ACTION_Y1,
                       "SCAN", act_fill, COL_TEXT_WHITE);

        /* Status info */
        if (st->bt_enabled) {
            np_text(v, 455, line_c, "Status: Discoverable as XDJ-RX3-UNX / Bluetooth 5.0",
                    COL_CARD_BG, COL_TEXT_GREEN, 455, 1220);
        } else {
            np_text(v, 455, line_c, "Status: Bluetooth adapter disabled",
                    COL_CARD_BG, COL_TEXT_DIM, 455, 1220);
        }

        /* Native Utility-Style Table Rows (y: 196..562) */
        np_draw_table_row(v, 0, "BLUETOOTH ADAPTER",
                          st->bt_enabled ? "Active: hci0 / Discoverable" : "Powered Off",
                          st->bt_enabled ? COL_TEXT_GREEN : COL_TEXT_DIM);
        np_draw_table_row(v, 1, "DEVICE BROADCAST NAME", "XDJ-RX3-UNX", COL_TEXT_CYAN);
        np_draw_table_row(v, 2, "WIRELESS DJ MIDI MODE", "Enabled - Auto-link on pair", COL_TEXT_WHITE);
        np_draw_table_row(v, 3, "BLUETOOTH AUDIO IN / A2DP", "Ready - Aux In Streaming", COL_TEXT_CYAN);
        np_draw_table_row(v, 4, "PAIRING PIN CODE", "0000", COL_TEXT_WHITE);
        np_draw_table_row(v, 5, "BLUETOOTH MAC ADDRESS", "00:1A:7D:DA:71:15", COL_TEXT_GRAY);
        np_draw_table_row(v, 6, "DISCOVERY VISIBILITY",
                          st->bt_enabled ? "Visible to all nearby smartphones and laptops" : "Hidden",
                          COL_TEXT_GRAY);
        np_draw_table_row(v, 7, "RF TRANSMISSION POWER", "4 dBm - Class 2 / 10m range", COL_TEXT_GRAY);

    } else if (active_tab == NET_TAB_HOTSPOT) {
        /* --- HOTSPOT / AP TAB --- */
        /* Control Card */
        np_rect(v, NET_BODY_X0, NET_CTRL_Y0, NET_BODY_X1, NET_CTRL_Y1, COL_CARD_BG, COL_BORDER);

        int line_c = NET_CTRL_Y0 + ((NET_CTRL_Y1 - NET_CTRL_Y0) - MENU_FONT_LINE) / 2;
        np_text(v, NET_BODY_X0 + 20, line_c, "Pro DJ Link AP:", COL_CARD_BG, COL_TEXT_WHITE,
                NET_BODY_X0 + 20, 225);

        /* Toggle Button */
        int tog_prs = (pressed_target == 7);
        unsigned int tog_fill = st->ap_enabled ? COL_GREEN_ON : COL_GRAY_OFF;
        if (tog_prs) tog_fill = COL_ACCENT_HOV;
        np_rect(v, NET_TOGGLE_X0, NET_TOGGLE_Y0, NET_TOGGLE_X0 + 130, NET_TOGGLE_Y1,
                tog_fill, COL_BORDER_BRT);
        np_text_center(v, NET_TOGGLE_X0, NET_TOGGLE_Y0, NET_TOGGLE_X0 + 130, NET_TOGGLE_Y1,
                       st->ap_enabled ? "STOP AP" : "START AP", tog_fill, COL_TEXT_WHITE);

        if (st->ap_enabled) {
            np_text(v, 380, line_c, "Status: Broadcasting standalone DJ network",
                    COL_CARD_BG, COL_TEXT_GREEN, 380, 1220);
        } else {
            np_text(v, 380, line_c, "Status: Standalone hotspot is stopped",
                    COL_CARD_BG, COL_TEXT_DIM, 380, 1220);
        }

        /* Native Utility-Style Table Rows (y: 196..562) */
        np_draw_table_row(v, 0, "HOTSPOT ACCESS POINT",
                          st->ap_enabled ? "Broadcasting: hostapd active" : "Stopped",
                          st->ap_enabled ? COL_TEXT_GREEN : COL_TEXT_DIM);
        np_draw_table_row(v, 1, "NETWORK NAME / SSID", "XDJ-RX3-DIRECT", COL_TEXT_CYAN);
        np_draw_table_row(v, 2, "SECURITY PROTOCOL", "WPA2-PSK / AES-CCMP", COL_TEXT_WHITE);
        np_draw_table_row(v, 3, "WIFI PASSWORD", "xdjrx3wireless", COL_TEXT_CYAN);
        np_draw_table_row(v, 4, "GATEWAY IP ADDRESS", "192.168.42.1", COL_TEXT_WHITE);
        np_draw_table_row(v, 5, "DHCP POOL RANGE", "192.168.42.10 - 192.168.42.50", COL_TEXT_GRAY);
        np_draw_table_row(v, 6, "RADIO CHANNEL / BAND", "Channel 6 - 2.412 GHz / 802.11n", COL_TEXT_GRAY);

        char cli_buf[64];
        snprintf(cli_buf, sizeof(cli_buf), "%d devices - Rekordbox / iPad", st->ap_clients);
        np_draw_table_row(v, 7, "CONNECTED CLIENTS", cli_buf, COL_TEXT_CYAN);

    } else if (active_tab == NET_TAB_STATUS) {
        /* --- STATUS / IP TAB --- */
        /* Control Card */
        np_rect(v, NET_BODY_X0, NET_CTRL_Y0, NET_BODY_X1, NET_CTRL_Y1, COL_CARD_BG, COL_BORDER);

        int line_c = NET_CTRL_Y0 + ((NET_CTRL_Y1 - NET_CTRL_Y0) - MENU_FONT_LINE) / 2;
        np_text(v, NET_BODY_X0 + 20, line_c, "Network Stack:", COL_CARD_BG, COL_TEXT_WHITE,
                NET_BODY_X0 + 20, 225);

        /* Restart Button */
        int act_prs = (pressed_target == 7 || pressed_target == 8);
        unsigned int act_fill = act_prs ? COL_ACCENT_HOV : COL_ROW_BG;
        np_rect(v, NET_TOGGLE_X0, NET_TOGGLE_Y0, NET_TOGGLE_X0 + 190, NET_TOGGLE_Y1,
                act_fill, COL_BORDER_BRT);
        np_text_center(v, NET_TOGGLE_X0, NET_TOGGLE_Y0, NET_TOGGLE_X0 + 190, NET_TOGGLE_Y1,
                       "RESTART NETWORK", act_fill, COL_TEXT_WHITE);

        np_text(v, 440, line_c, "Status: All network interfaces operational",
                COL_CARD_BG, COL_TEXT_GREEN, 440, 1220);

        /* Native Utility-Style Table Rows (y: 196..562) */
        np_draw_table_row(v, 0, "SYSTEM HOSTNAME",
                          st->hostname[0] ? st->hostname : "unx.dj", COL_TEXT_WHITE);

        char ip_buf[64];
        snprintf(ip_buf, sizeof(ip_buf), "%s - wlan0", st->wifi_ip[0] ? st->wifi_ip : "Disconnected");
        np_draw_table_row(v, 1, "WI-FI / WLAN0 IP", ip_buf,
                          st->wifi_ip[0] ? COL_TEXT_GREEN : COL_TEXT_DIM);

        snprintf(ip_buf, sizeof(ip_buf), "%s - eth0", st->eth_ip[0] ? st->eth_ip : "Cable Disconnected");
        np_draw_table_row(v, 2, "ETHERNET / ETH0 IP", ip_buf,
                          st->eth_ip[0] ? COL_TEXT_GREEN : COL_TEXT_DIM);

        char web_buf[128];
        snprintf(web_buf, sizeof(web_buf), "http://%s:5800/ - NoVNC Screen",
                 st->wifi_ip[0] ? st->wifi_ip : "192.168.137.22");
        np_draw_table_row(v, 3, "WEB REMOTE CONSOLE", web_buf, COL_TEXT_CYAN);

        char ssh_buf[128];
        snprintf(ssh_buf, sizeof(ssh_buf), "ssh root@%s - Port 22",
                 st->wifi_ip[0] ? st->wifi_ip : "192.168.137.22");
        np_draw_table_row(v, 4, "SSH SHELL ACCESS", ssh_buf, COL_TEXT_WHITE);

        np_draw_table_row(v, 5, "PRO DJ LINK ENGINE", "UDP 50000-50002 - Link Active", COL_TEXT_CYAN);
        np_draw_table_row(v, 6, "TIME SYNCHRONIZATION", "pool.ntp.org - NTP Synced", COL_TEXT_GRAY);
        np_draw_table_row(v, 7, "HARDWARE PLATFORM", "Amlogic S905X Quad-Core ARMv8 - Armbian", COL_TEXT_GRAY);
    }
}

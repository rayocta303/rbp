/* SPDX-License-Identifier: MPL-2.0 */
/* The X-PAD FX window as one component: layout, touch and painting.
 *
 * What is drawn: the LED-dot window (rx3_rmx_dots.h), five small length
 * chips, ROLL, four sounds (KICK SNARE CLAP H.HAT), OVERDUB, DELETE, HOLD and
 * MUTE. Original work: the shapes, colours and lettering are drawn here from
 * scratch (a 5x7 dot font, rounded rectangles); no artwork or name of any
 * product is used.
 *
 * What it does not own. It owns no state of the controls: every press goes
 * to the shared machine of rx3_rmx_state.h with the source RMXS_TOUCH, and
 * every lit button is read back from that machine's view, so a pad pressed on
 * the console lights the button here and a touch lights the pad (one state,
 * two surfaces). It owns only what is purely a matter of the screen: which
 * length chip is chosen (the length the ROLL button will use), which finger
 * holds which button, and the DELETE / MUTE latch (see below).
 *
 * One finger. The console reports one touch point at a time (the player's
 * TouchStatus carries a single x, y), so "hold DELETE and tap a pad" cannot be
 * done with two fingers on the screen. DELETE and MUTE are therefore latches
 * here: a tap arms one (it stays lit, owned by the touch source in the shared
 * machine, so pads press and touches alike delete or mute) and a second tap
 * disarms it.
 *
 * Coordinates are those of a canvas the caller owns, origin at its top-left:
 * the caller subtracts the canvas' place on the screen. Two canvas sizes are
 * solved: the PAGE (about 892 x 444, the area of the two waveforms) and the
 * BAND (892 x 222, one deck's waveform area). Nothing else is assumed; see
 * docs/RMX1000_PIANO.md for what the core lets a module do with them.
 *
 * Sizes are chosen for a finger on a ~10 inch, 1280 x 800 screen
 * (0.17 mm per pixel): every touchable box is at least RMXX_MIN_TOUCH (48 px,
 * 8 mm) on both sides, the chips are small to look at (about 70 x 34) with a
 * bigger box around them to hit.
 *
 * Pure code, no allocation, no libc, no libm, no signed division (the hook
 * has no __aeabi_idiv): every divide below is of unsigned values. Tested on
 * the computer by tests/test_rmx_xpad.py; not tried on a console.
 */
#ifndef RX3_RMX_XPAD_H
#define RX3_RMX_XPAD_H

#include "rx3_rmx_dots.h"
#include "rx3_rmx_state.h"

#define RMXX_MIN_TOUCH 48
#define RMXX_NONE 0xffu

/* The items, in hit-test order. Items 0..3 are sounds 0..3 of the state. */
enum rmxx_item {
    RMXX_KICK = 0, RMXX_SNARE, RMXX_CLAP, RMXX_HHAT,
    RMXX_ROLL, RMXX_OVERDUB, RMXX_DELETE, RMXX_HOLD, RMXX_MUTE,
    RMXX_LEN0, RMXX_LEN1, RMXX_LEN2, RMXX_LEN3, RMXX_LEN4,      /* 1/8 1/4 1/2 1/1 2/1 */
    RMXX_DOTS,                                                   /* the X-PAD itself */
    RMXX_ITEMS
};

enum rmxx_phase { RMXX_UP = 0, RMXX_DOWN = 1, RMXX_MOVE = 2 };

struct rmxx_rect { int x, y, w, h; };

struct rmxx_layout {
    unsigned int width, height;          /* the canvas */
    unsigned int pitch;                  /* dot cell, = sprite size */
    unsigned int scale;                  /* lettering scale of the buttons (chips use 2) */
    struct rmxx_rect frame;              /* the dark window around the dots */
    struct rmxx_rect dots;               /* the matrix: 35 x pitch by 9 x pitch */
    struct rmxx_rect face[RMXX_ITEMS];   /* what is drawn */
    struct rmxx_rect hit[RMXX_ITEMS];    /* what a finger can hit, never smaller than face */
};

struct rmxx_ui {
    struct rmxs *st;                     /* the shared state, owned by the caller */
    unsigned char len;                   /* chosen length chip 0..4, used by ROLL */
    unsigned char latch;                 /* RMXS_DELETE, RMXS_MUTE or RMXX_NONE */
    unsigned char held;                  /* item under the finger or RMXX_NONE */
    unsigned char dragging;              /* the finger holds the X-PAD */
    unsigned int shown_lit, shown_muted; /* what the chrome last painted */
    unsigned char shown_len, shown_zone, chrome_valid;
    struct rmxd_field field;
    struct rmxd_shown shown;
};

/* ---- layout ---- */

static inline struct rmxx_rect rmxx_r(int x, int y, int w, int h)
{
    struct rmxx_rect r; r.x = x; r.y = y; r.w = w; r.h = h; return r;
}

static inline struct rmxx_rect rmxx_grow(struct rmxx_rect r, int by)
{
    return rmxx_r(r.x - by, r.y - by, r.w + 2 * by, r.h + 2 * by);
}

static inline int rmxx_inside(struct rmxx_rect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static inline int rmxx_overlap(struct rmxx_rect a, struct rmxx_rect b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

/* A button's box in a row or grid: the hit box reaches half a gap around the face. */
static inline void rmxx_place(struct rmxx_layout *L, unsigned int item, int x, int y, int w, int h, int gap)
{
    L->face[item] = rmxx_r(x, y, w, h);
    L->hit[item] = rmxx_grow(L->face[item], gap / 2);
}

/* Solves a canvas. Height >= 400: the PAGE (dots above, chips to their right,
   two rows of buttons below). Height 200..399: the BAND (dots and chips to the
   left, a 3 x 3 grid of buttons to the right). Returns 0 when it does not fit. */
static inline int rmxx_layout_solve(struct rmxx_layout *L, unsigned int width, unsigned int height)
{
    const int W = (int)width, H = (int)height;
    unsigned int i;
    if (width < 640u || width > 4096u || height < 200u || height > 4096u) return 0;
    for (i = 0; i < RMXX_ITEMS; i++) { L->face[i] = rmxx_r(0, 0, 0, 0); L->hit[i] = rmxx_r(0, 0, 0, 0); }
    L->width = width; L->height = height;
    if (height >= 400u) {
        const int m = 16, gap = 12, pad = 14, chip_w = 84, chip_h = RMXX_MIN_TOUCH, col_gap = 12;
        int avail = W - 2 * m - chip_w - col_gap - 2 * pad;
        unsigned int pitch = avail > 0 ? (unsigned int)avail / RMXD_COLS : 0u;
        if (pitch > RMXD_SPRITE_MAX) pitch = RMXD_SPRITE_MAX;
        if (pitch < 8u) return 0;
        L->pitch = pitch; L->scale = 3;
        const int dw = (int)(RMXD_COLS * pitch), dh = (int)(RMXD_ROWS * pitch);
        L->frame = rmxx_r(m, m, dw + 2 * pad, dh + 2 * pad);
        L->dots = rmxx_r(m + pad, m + pad, dw, dh);
        const int col_x = m + L->frame.w + col_gap, col_w = W - m - col_x;
        if (col_w < RMXX_MIN_TOUCH) return 0;
        const int top_h = L->frame.h > 5 * chip_h ? L->frame.h : 5 * chip_h;
        for (i = 0; i < 5u; i++) {
            L->hit[RMXX_LEN0 + i] = rmxx_r(col_x, m + (int)i * chip_h, col_w, chip_h);
            /* The face is a small chip in the middle of its box. */
            const int fw = col_w > 70 ? 70 : col_w - 8, fh = 34;
            L->face[RMXX_LEN0 + i] = rmxx_r(col_x + (col_w - fw) / 2, m + (int)i * chip_h + (chip_h - fh) / 2, fw, fh);
        }
        const int y0 = m + top_h + gap + 2;
        const int rows_h = H - m - y0;
        const int row_h = (rows_h - gap) / 2;
        if (row_h < RMXX_MIN_TOUCH) return 0;
        {   /* Row A: ROLL twice as wide as each sound. */
            const int total = W - 2 * m - 4 * gap, unit = total / 6;
            const int roll_w = unit * 2;
            int x = m;
            rmxx_place(L, RMXX_ROLL, x, y0, roll_w, row_h, gap); x += roll_w + gap;
            for (i = 0; i < 4u; i++) {
                const int w = i == 3u ? W - m - x : unit;
                rmxx_place(L, RMXX_KICK + i, x, y0, w, row_h, gap); x += w + gap;
            }
        }
        {   /* Row B: OVERDUB DELETE HOLD MUTE, equal. */
            static const unsigned char order[4] = { RMXX_OVERDUB, RMXX_DELETE, RMXX_HOLD, RMXX_MUTE };
            const int unit = (W - 2 * m - 3 * gap) / 4;
            int x = m;
            for (i = 0; i < 4u; i++) {
                const int w = i == 3u ? W - m - x : unit;
                rmxx_place(L, order[i], x, y0 + row_h + gap, w, row_h, gap); x += w + gap;
            }
        }
    } else {
        const int m = 10, gap = 8, pad = 8, chip_h = RMXX_MIN_TOUCH;
        unsigned int pitch = ((unsigned int)W * 53u / 100u - 2u * (unsigned int)pad) / RMXD_COLS;
        if (pitch > RMXD_SPRITE_MAX) pitch = RMXD_SPRITE_MAX;
        while (pitch >= 8u && 2 * m + (int)(RMXD_ROWS * pitch) + 2 * pad + 6 + chip_h > H) pitch--;
        if (pitch < 8u) return 0;
        L->pitch = pitch; L->scale = 2;
        const int dw = (int)(RMXD_COLS * pitch), dh = (int)(RMXD_ROWS * pitch);
        L->frame = rmxx_r(m, m, dw + 2 * pad, dh + 2 * pad);
        L->dots = rmxx_r(m + pad, m + pad, dw, dh);
        const int chips_y = m + L->frame.h + 6;
        for (i = 0; i < 5u; i++) {
            const int cx = L->frame.x + (int)(i * (unsigned int)L->frame.w / 5u);
            const int cw = L->frame.x + (int)((i + 1u) * (unsigned int)L->frame.w / 5u) - cx;
            L->hit[RMXX_LEN0 + i] = rmxx_r(cx, chips_y, cw, chip_h);
            const int fw = cw > 76 ? 70 : cw - 8, fh = 30;
            L->face[RMXX_LEN0 + i] = rmxx_r(cx + (cw - fw) / 2, chips_y + (chip_h - fh) / 2, fw, fh);
        }
        const int gx = m + L->frame.w + 14, gw = W - m - gx;
        const int cell_w = (gw - 2 * gap) / 3, cell_h = (H - 2 * m - 2 * gap) / 3;
        if (cell_w < RMXX_MIN_TOUCH || cell_h < RMXX_MIN_TOUCH) return 0;
        static const unsigned char grid[9] = { RMXX_ROLL, RMXX_KICK, RMXX_SNARE, RMXX_CLAP, RMXX_HHAT,
                                               RMXX_OVERDUB, RMXX_DELETE, RMXX_HOLD, RMXX_MUTE };
        for (i = 0; i < 9u; i++) {
            const int c = (int)(i % 3u), r = (int)(i / 3u);
            const int w = c == 2 ? W - m - (gx + 2 * (cell_w + gap)) : cell_w;
            rmxx_place(L, grid[i], gx + c * (cell_w + gap), m + r * (cell_h + gap), w, cell_h, gap);
        }
    }
    L->face[RMXX_DOTS] = L->frame;
    L->hit[RMXX_DOTS] = L->frame;
    return 1;
}

static inline unsigned int rmxx_hit(const struct rmxx_layout *L, int x, int y)
{
    for (unsigned int i = 0; i < RMXX_ITEMS; i++)
        if (rmxx_inside(L->hit[i], x, y)) return i;
    return RMXX_NONE;
}

/* A point on the dots as the X-PAD's 0..255 pair; y 255 is the top row. Points
   outside the matrix are clamped to its edge (a dragging finger may leave it). */
static inline void rmxx_pad_value(const struct rmxx_layout *L, int px, int py, unsigned int *x, unsigned int *y)
{
    int rx = px - L->dots.x, ry = py - L->dots.y;
    if (rx < 0) rx = 0;
    if (ry < 0) ry = 0;
    if (rx > L->dots.w - 1) rx = L->dots.w - 1;
    if (ry > L->dots.h - 1) ry = L->dots.h - 1;
    *x = (unsigned int)rx * 255u / (unsigned int)(L->dots.w - 1);
    *y = 255u - (unsigned int)ry * 255u / (unsigned int)(L->dots.h - 1);
}

/* ---- touch ---- */

static inline void rmxx_init(struct rmxx_ui *ui, struct rmxs *st)
{
    ui->st = st;
    ui->len = 2;                          /* 1/2 */
    ui->latch = RMXX_NONE;
    ui->held = RMXX_NONE;
    ui->dragging = 0;
    ui->shown_lit = 0xffffffffu; ui->shown_muted = 0xffffu;
    ui->shown_len = 0xffu; ui->shown_zone = 0xffu; ui->chrome_valid = 0;
    rmxd_shown_reset(&ui->shown);
}

/* The state control an item presses, or RMXX_NONE for chips and the dots. */
static inline unsigned int rmxx_control_of(unsigned int item)
{
    static const unsigned char map[RMXX_ITEMS] = {
        0, 1, 2, 3, RMXS_ROLL, RMXS_OVERDUB, RMXS_DELETE, RMXS_HOLD, RMXS_MUTE,
        RMXX_NONE, RMXX_NONE, RMXX_NONE, RMXX_NONE, RMXX_NONE, RMXX_NONE
    };
    return item < RMXX_ITEMS ? map[item] : RMXX_NONE;
}

static inline void rmxx_unlatch(struct rmxx_ui *ui)
{
    if (ui->latch != RMXX_NONE) (void)rmxs_release(ui->st, RMXS_TOUCH, ui->latch);
    ui->latch = RMXX_NONE;
}

/* A finger event at canvas (x, y). Returns 1 when the component takes it: a
   DOWN inside any hit box, and then every event of that gesture, wherever the
   finger goes, up to the lift. */
static inline int rmxx_touch(struct rmxx_ui *ui, const struct rmxx_layout *L, int x, int y, unsigned int phase)
{
    struct rmxs *st = ui->st;
    if (phase == RMXX_DOWN) {
        unsigned int item = rmxx_hit(L, x, y), control;
        if (item == RMXX_NONE) return 0;
        if (ui->held != RMXX_NONE) return 1;               /* a gesture is already running */
        ui->held = (unsigned char)item;
        if (item == RMXX_DOTS) {
            unsigned int px, py;
            rmxx_pad_value(L, x, y, &px, &py);
            if (rmxs_xpad(st, RMXS_TOUCH, px, py, 1) != RMXS_BUSY) { ui->dragging = 1; ui->len = st->zone; }
        } else if (item >= RMXX_LEN0 && item <= RMXX_LEN4) {
            ui->len = (unsigned char)(item - RMXX_LEN0);
            (void)rmxs_roll_zone(st, RMXS_TOUCH, ui->len);  /* a roll this finger's source holds follows */
        } else if (item == RMXX_DELETE || item == RMXX_MUTE) {
            control = rmxx_control_of(item);
            if (ui->latch == control) rmxx_unlatch(ui);
            else {
                rmxx_unlatch(ui);
                if (rmxs_press(st, RMXS_TOUCH, control, 0) == RMXS_TAKEN) ui->latch = (unsigned char)control;
            }
        } else {
            control = rmxx_control_of(item);
            (void)rmxs_press(st, RMXS_TOUCH, control, ui->len);
        }
        return 1;
    }
    if (ui->held == RMXX_NONE) return 0;
    if (phase == RMXX_MOVE) {
        if (ui->dragging) {
            unsigned int px, py;
            rmxx_pad_value(L, x, y, &px, &py);
            (void)rmxs_xpad(st, RMXS_TOUCH, px, py, 1);
            ui->len = st->zone;
        }
        return 1;
    }
    /* The lift. Toggles and latches have nothing to let go of. */
    if (ui->dragging) {
        (void)rmxs_xpad(st, RMXS_TOUCH, st->x, st->y, 0);
    } else if (ui->held < RMXX_LEN0 && ui->held != RMXX_OVERDUB && ui->held != RMXX_DELETE &&
               ui->held != RMXX_HOLD && ui->held != RMXX_MUTE) {
        (void)rmxs_release(st, RMXS_TOUCH, rmxx_control_of(ui->held));
    }
    ui->held = RMXX_NONE;
    ui->dragging = 0;
    return 1;
}

/* The panel went away or the finger is lost: everything this source held lets go. */
static inline void rmxx_lost(struct rmxx_ui *ui)
{
    rmxs_source_lost(ui->st, RMXS_TOUCH);
    ui->latch = RMXX_NONE;
    ui->held = RMXX_NONE;
    ui->dragging = 0;
}

/* ---- what the dots show ---- */

static inline const char *rmxx_len_label(unsigned int len)
{
    static const char *const label[RMXS_ZONES] = { "1/8", "1/4", "1/2", "1/1", "2/1" };
    return label[len < RMXS_ZONES ? len : RMXS_ZONES - 1u];
}

static inline const char *rmxx_word(const struct rmxx_ui *ui)
{
    if (ui->latch == RMXS_DELETE) return "DEL";
    if (ui->latch == RMXS_MUTE) return "MUTE";
    if (ui->st->overdub) return "OVR";
    return rmxx_len_label(ui->st->roll_on ? ui->st->zone : ui->len);
}

static inline void rmxx_field_update(struct rmxx_ui *ui)
{
    struct rmxd_field *f = &ui->field;
    const struct rmxs *st = ui->st;
    if (st->roll_on) {
        /* The roll's zone rings; the peak follows the finger (or stays where HOLD kept it). */
        rmxd_field_xpad(f, RMXD_ZONE, st->x, st->y, 255u);
        return;
    }
    unsigned int sounding = 0, n = 0, sum = 0;
    for (unsigned int c = 0; c < 4u; c++)
        if (st->owner[c] != RMXS_FREE) { n++; sum += 32u + c * 64u; }
    if (n) {                                              /* a sound hit: a soft bell, in its quarter */
        rmxd_field_xpad(f, RMXD_BELLS, sum / n, 128u, 170u);
        sounding = 1;
    }
    if (!sounding) {
        rmxd_field_xpad(f, RMXD_OFF, 0, 0, 0);
        const char *w = rmxx_word(ui);
        unsigned int len = 0; while (w[len]) len++;
        unsigned int width = len * 6u - 1u;
        rmxd_text(f, 1u, (RMXD_COLS - width) / 2u, w, 200u);
    }
}

/* ---- painting into an RGB565 canvas ---- */

#define RMXX_BG        0x0b0e11u
#define RMXX_FRAME_BG  0x06080au
#define RMXX_FRAME_EDGE 0x2b333bu
#define RMXX_FACE      0x1b2127u
#define RMXX_CHIP      0x14191eu
#define RMXX_INK_DARK  0x0f1317u
#define RMXX_TINT      0xffb040u

static inline unsigned int rmxx_colour_of(unsigned int item)
{
    switch (item) {
    case RMXX_KICK: return 0xe5484du;
    case RMXX_SNARE: return 0xf5a524u;
    case RMXX_CLAP: return 0xa37cf0u;
    case RMXX_HHAT: return 0x35c2e8u;
    case RMXX_ROLL: return 0xff8a1fu;
    case RMXX_OVERDUB: return 0xff4d9au;
    case RMXX_DELETE: return 0xd5dadfu;
    case RMXX_HOLD: return 0x3a9bffu;
    case RMXX_MUTE: return 0x8d96a0u;
    default: return 0xff8a1fu;
    }
}

static inline const char *rmxx_name_of(unsigned int item)
{
    static const char *const name[RMXX_ITEMS] = {
        "KICK", "SNARE", "CLAP", "H.HAT", "RMX", "OVERDUB", "DELETE", "HOLD", "MUTE",
        "1/8", "1/4", "1/2", "1/1", "2/1", "" };
    return item < RMXX_ITEMS ? name[item] : "";
}

static inline unsigned int rmxx_dim(unsigned int rgb, unsigned int k256)
{
    return ((((rgb >> 16) & 255u) * k256 >> 8) << 16) | ((((rgb >> 8) & 255u) * k256 >> 8) << 8) | ((rgb & 255u) * k256 >> 8);
}

static inline uint16_t rmxx_565(unsigned int rgb)
{
    return rmxd_rgb565((rgb >> 16) & 255u, (rgb >> 8) & 255u, rgb & 255u);
}

static inline void rmxx_fill(uint16_t *fb, unsigned int stride, const struct rmxx_layout *L,
                             int x, int y, int w, int h, uint16_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)L->width) w = (int)L->width - x;
    if (y + h > (int)L->height) h = (int)L->height - y;
    for (int r = 0; r < h; r++) {
        uint16_t *p = fb + (unsigned int)(y + r) * stride + (unsigned int)x;
        for (int i = 0; i < w; i++) p[i] = c;
    }
}

/* Rounded corner: the number of pixels cut from a row `k` rows below the top, for radius r. */
static inline int rmxx_cut(int r, int k)
{
    if (k >= r) return 0;
    const int dy = 2 * (r - k) - 1;
    for (int x = 0; x < r; x++) {
        const int dx = 2 * (r - x) - 1;
        if (dx * dx + dy * dy <= 4 * r * r) return x;
    }
    return r;
}

static inline void rmxx_round(uint16_t *fb, unsigned int stride, const struct rmxx_layout *L,
                              struct rmxx_rect b, int r, uint16_t c)
{
    if (r * 2 > b.h) r = b.h / 2;
    if (r * 2 > b.w) r = b.w / 2;
    for (int k = 0; k < b.h; k++) {
        const int from_bottom = b.h - 1 - k;
        const int cut = rmxx_cut(r, k < from_bottom ? k : from_bottom);
        rmxx_fill(fb, stride, L, b.x + cut, b.y + k, b.w - 2 * cut, 1, c);
    }
}

static inline unsigned int rmxx_text_width(const char *s, unsigned int scale)
{
    unsigned int n = 0; while (s[n]) n++;
    return n ? n * 6u * scale - scale : 0u;
}

static inline void rmxx_text(uint16_t *fb, unsigned int stride, const struct rmxx_layout *L,
                             int x, int y, unsigned int scale, const char *s, uint16_t c)
{
    for (; *s; s++, x += (int)(6u * scale)) {
        const uint8_t *g = rmxd_font[rmxd_glyph(*s)];
        for (unsigned int r = 0; r < 7u; r++)
            for (unsigned int k = 0; k < 5u; k++)
                if ((g[r] >> (4u - k)) & 1u)
                    rmxx_fill(fb, stride, L, x + (int)(k * scale), y + (int)(r * scale), (int)scale, (int)scale, c);
    }
}

static inline void rmxx_text_centred(uint16_t *fb, unsigned int stride, const struct rmxx_layout *L,
                                     struct rmxx_rect box, unsigned int scale, const char *s, uint16_t c)
{
    const int w = (int)rmxx_text_width(s, scale), h = (int)(7u * scale);
    rmxx_text(fb, stride, L, box.x + (box.w - w) / 2, box.y + (box.h - h) / 2, scale, s, c);
}

/* One button: unlit = dark face, coloured edge and lettering; lit = the colour filled, dark lettering. */
static inline void rmxx_button(uint16_t *fb, unsigned int stride, const struct rmxx_layout *L,
                               unsigned int item, int lit, int muted, int selected)
{
    const struct rmxx_rect b = L->face[item];
    const unsigned int colour = rmxx_colour_of(item);
    const int chip = item >= RMXX_LEN0 && item <= RMXX_LEN4;
    const int radius = chip ? 8 : 12;
    unsigned int edge, face, ink;
    if (lit) { edge = colour; face = colour; ink = RMXX_INK_DARK; }
    else if (chip && selected) { edge = colour; face = rmxx_dim(colour, 70u); ink = 0xffffffu; }
    else if (chip) { edge = 0x2b333bu; face = RMXX_CHIP; ink = 0x9aa5b0u; }
    else { edge = rmxx_dim(colour, 150u); face = RMXX_FACE; ink = colour; }
    rmxx_round(fb, stride, L, b, radius, rmxx_565(edge));
    rmxx_round(fb, stride, L, rmxx_r(b.x + 2, b.y + 2, b.w - 4, b.h - 4), radius > 2 ? radius - 2 : radius, rmxx_565(face));
    rmxx_text_centred(fb, stride, L, b, chip ? 2u : L->scale, rmxx_name_of(item), rmxx_565(ink));
    if (muted)                                           /* a muted layer: a bar along the bottom */
        rmxx_fill(fb, stride, L, b.x + b.w / 4, b.y + b.h - 10, b.w / 2, 4, rmxx_565(0xff5a4a));
}

/* The state behind the picture, as the buttons need it. */
struct rmxx_look { unsigned int lit, muted; unsigned char len, zone; };

static inline struct rmxx_look rmxx_look_of(const struct rmxx_ui *ui)
{
    const struct rmxs_view v = rmxs_view_of(ui->st);
    struct rmxx_look k;
    k.lit = v.lit; k.muted = v.muted;
    k.len = ui->len; k.zone = v.roll_on ? v.zone : (unsigned char)ui->len;
    return k;
}

static inline unsigned int rmxx_item_lit(const struct rmxx_look *k, unsigned int item)
{
    const unsigned int control = rmxx_control_of(item);
    return control == RMXX_NONE ? 0u : (k->lit >> control) & 1u;
}

/* Paints the component into the canvas `fb` (RGB565, `stride` pixels per line).
 *
 *  full != 0: everything is repainted, the background included. This is what a
 *  surface that is cleared by its owner needs (the player clears the waveform
 *  buffer before every redraw), and the dots are all copied again.
 *  full == 0: for a surface that keeps what was painted: the chrome is
 *  repainted only when a button's state changed, and only the dots whose
 *  brightness step changed are copied.
 *
 * `sprites` holds RMXD_LEVELS tiles of pitch x pitch built by rmxd_sprites() with
 * the background RMXX_FRAME_BG and the tint RMXX_TINT. Returns the number of dots copied. */
static inline unsigned int rmxx_paint(uint16_t *fb, unsigned int stride, const struct rmxx_layout *L,
                                      struct rmxx_ui *ui, const uint16_t *sprites, unsigned int full)
{
    const struct rmxx_look k = rmxx_look_of(ui);
    unsigned int i;
    if (full) { ui->chrome_valid = 0; rmxd_shown_reset(&ui->shown); }
    if (!ui->chrome_valid || k.lit != ui->shown_lit || k.muted != ui->shown_muted || k.len != ui->shown_len ||
        k.zone != ui->shown_zone) {
        if (!ui->chrome_valid) {
            rmxx_fill(fb, stride, L, 0, 0, (int)L->width, (int)L->height, rmxx_565(RMXX_BG));
            rmxx_round(fb, stride, L, L->frame, 14, rmxx_565(RMXX_FRAME_EDGE));
            rmxx_round(fb, stride, L, rmxx_r(L->frame.x + 2, L->frame.y + 2, L->frame.w - 4, L->frame.h - 4), 12,
                       rmxx_565(RMXX_FRAME_BG));
            rmxd_shown_reset(&ui->shown);
        }
        for (i = 0; i < RMXX_DOTS; i++) {
            const int chip = i >= RMXX_LEN0 && i <= RMXX_LEN4;
            if (chip) {
                /* The chosen length is marked; a sounding roll lights its own zone's chip. */
                const unsigned int on = k.zone == i - RMXX_LEN0 && (ui->st->roll_on || (ui->st->owner[RMXS_ROLL] != RMXS_FREE));
                rmxx_button(fb, stride, L, i, (int)on, 0, k.len == i - RMXX_LEN0);
            } else {
                rmxx_button(fb, stride, L, i, (int)rmxx_item_lit(&k, i), i < 4u ? (int)((k.muted >> i) & 1u) : 0, 0);
            }
        }
        ui->shown_lit = k.lit; ui->shown_muted = k.muted; ui->shown_len = k.len; ui->shown_zone = k.zone;
        ui->chrome_valid = 1;
    }
    rmxx_field_update(ui);
    return rmxd_draw(fb + (unsigned int)L->dots.y * stride + (unsigned int)L->dots.x, stride, L->pitch, sprites,
                     L->pitch, &ui->field, &ui->shown);
}

#endif

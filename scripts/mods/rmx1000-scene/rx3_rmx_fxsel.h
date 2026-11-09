/* SPDX-License-Identifier: MPL-2.0 */
/* The effect chooser of BEAT FX position 4 (the old HELIX): which effect that
 * position plays, and the one row of touch buttons that shows and changes it.
 * Pure logic the computer can test: no allocation, no I/O, no libc, no threads
 * (the caller serialises its calls). Original work.
 *
 * What it models.
 *  - A short registry of the effects position 4 can play. Entry 0 is HELIX, the
 *    player's own effect, always selectable and the default. The others are the
 *    SCENE FX family of the RMX-1000. The ids are the same as in
 *    rmx1000-roll/rx3_rmx_slots.h (a module may not include a sibling's header,
 *    so the rows are repeated here).
 *  - One list, no pages: the effects are buttons on one row of eight cells (the core
 *    allows no more), the one that plays is lit, a tap changes it (at once, or at the
 *    next OFF when switch=off), and the last button, X, closes the row. SPIRAL and
 *    REVERB share a cell each for their UP and DOWN: tap again to flip.
 *  - What leaving the position does: `reset` puts the effect back to the start
 *    effect, `keep` leaves the chosen one where it is.
 *  - The captions, kinds and widths of that row, as data, so a test and the
 *    preview picture read the same thing as the screen.
 */
#ifndef RX3_RMX_FXSEL_H
#define RX3_RMX_FXSEL_H

#include "../core/api/rx3_platform.h"
#include "../core/api/rx3_palette.h"

#define FXS_ENTRIES 9u
#define FXS_NONE 0xffu
#define FXS_POSITION 4u                 /* BEAT FX selector position 4 = HELIX */
#define FXS_CAPTION_MAX 24u

/* group: 0 the player's own, 1 builds up (red), 2 breaks down (blue). */
struct fxs_entry { const char *id; const char *name; unsigned int written; unsigned int group; };

/* The order on the screen, left to right. NOISE and plain ECHO are not here: the player has them
   natively. The names are short on purpose: a caption that does not fit its button is not drawn. */
static const struct fxs_entry fxs_registry[FXS_ENTRIES] = {
    {"helix",       "HELIX",     1, 0},
    {"bpf-echo",    "BPF ECHO",  1, 1},
    {"hpf-echo",    "HPF ECHO",  1, 2},
    {"lpf-echo",    "LPF ECHO",  1, 2},
    {"crush-echo",  "CRUSH",     1, 2},
    {"spiral-up",   "SPIRAL UP", 1, 1},
    {"spiral-down", "SPIRAL DN", 1, 2},
    {"reverb-up",   "REVERB UP", 1, 1},
    {"reverb-down", "REVERB DN", 1, 2}
};

enum fxs_entry_id { FXS_HELIX = 0, FXS_BPF_ECHO, FXS_HPF_ECHO, FXS_LPF_ECHO, FXS_CRUSH_ECHO,
                    FXS_SPIRAL_UP, FXS_SPIRAL_DOWN, FXS_REVERB_UP, FXS_REVERB_DOWN };
#define FXS_RED_565 0xe186u
#define FXS_BLUE_565 RX3_PAL_ACCENT_DEFAULT_565   /* the selection blue of the palette */

/* The row. The core accepts at most eight widgets in a row, so SPIRAL and REVERB are one button each
   that flips between UP and DOWN when it is tapped again: HELIX, BPF, HPF, LPF | CRUSH, SPIRAL, REVERB, X.
   All cells are the same width, so the seam of the screen falls between the fourth and the fifth. */
#define FXS_W_SPIRAL 5u
#define FXS_W_REVERB 6u
#define FXS_L_CLOSE 7u
#define FXS_WIDGETS 8u
static const unsigned char fxs_weight[FXS_WIDGETS] = {1, 1, 1, 1, 1, 1, 1, 1};

/* What the model asks of the owner after a call: open the panel, or dismiss it. */
enum fxs_request { FXS_REQ_NONE = 0, FXS_REQ_OPEN = 1, FXS_REQ_CLOSE = 2 };

struct fxs_config {
    unsigned char start;                /* entry played on arrival after a reset, and at boot */
    unsigned char reset;                /* 1 leaving the position resets, 0 keeps */
    unsigned char immediate;            /* 1 a choice takes over at once, 0 at the next OFF */
    unsigned char autoopen;             /* 1 the row opens by itself on position 4 */
};

static inline void fxs_defaults(struct fxs_config *c)
{
    c->start = FXS_HELIX; c->reset = 1; c->immediate = 1; c->autoopen = 1;
}

static inline int fxs_streq(const char *a, const char *b)
{
    unsigned int i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}

/* The registry entry named `id` (NUL terminated), or FXS_NONE. */
static inline unsigned int fxs_find(const char *id)
{
    if (!id) return FXS_NONE;
    for (unsigned int i = 0; i < FXS_ENTRIES; i++) if (fxs_streq(id, fxs_registry[i].id)) return i;
    return FXS_NONE;
}

struct fxs {
    struct fxs_config cfg;
    unsigned char current, pending;     /* the effect now; one chosen while ON, waiting */
    unsigned char shown;                /* the row is the panel on screen */
    unsigned char spiral_down, reverb_down;   /* the direction the SPIRAL / REVERB button shows */
    unsigned char on;                   /* the effect is sounding (set by the owner) */
    unsigned char request;              /* enum fxs_request, taken by the owner */
    unsigned int position;              /* the selector as last seen; FXS_NONE until seen */
    unsigned int generation;            /* grows with every change the screen should show */
};

static inline void fxs_init(struct fxs *m, const struct fxs_config *cfg)
{
    m->cfg = *cfg;
    if (m->cfg.start >= FXS_ENTRIES || !fxs_registry[m->cfg.start].written) m->cfg.start = FXS_HELIX;
    m->current = m->cfg.start; m->pending = FXS_NONE;
    m->shown = 0; m->on = 0; m->request = FXS_REQ_NONE;
    m->spiral_down = m->current == FXS_SPIRAL_DOWN; m->reverb_down = m->current == FXS_REVERB_DOWN;
    m->position = FXS_NONE; m->generation = 0;
}

/* The effect the audio plays on position 4 now. */
static inline unsigned int fxs_active(const struct fxs *m) { return m->current; }

/* The selector moved (or was seen for the first time). */
static inline void fxs_selector(struct fxs *m, unsigned int position)
{
    if (position == m->position) return;
    unsigned int old = m->position;
    m->position = position;
    if (old == FXS_POSITION) {
        if (m->cfg.reset) m->current = m->cfg.start;
        m->pending = FXS_NONE;
        if (m->shown) m->request = FXS_REQ_CLOSE;
    }
    if (position == FXS_POSITION && m->cfg.autoopen) m->request = FXS_REQ_OPEN;
    m->generation++;
}

/* Choose an entry. 0 when it does not exist. */
static inline int fxs_choose(struct fxs *m, unsigned int entry)
{
    if (entry >= FXS_ENTRIES || !fxs_registry[entry].written) return 0;
    if (entry == FXS_SPIRAL_UP) m->spiral_down = 0; else if (entry == FXS_SPIRAL_DOWN) m->spiral_down = 1;
    if (entry == FXS_REVERB_UP) m->reverb_down = 0; else if (entry == FXS_REVERB_DOWN) m->reverb_down = 1;
    if (m->on && !m->cfg.immediate && entry != m->current) m->pending = (unsigned char)entry;
    else { m->current = (unsigned char)entry; m->pending = FXS_NONE; }
    m->generation++;
    return 1;
}

/* The effect went ON or OFF; a choice that waited takes over at OFF. */
static inline void fxs_set_on(struct fxs *m, unsigned int on)
{
    on = on != 0;
    if (on == m->on) return;
    m->on = (unsigned char)on;
    if (!on && m->pending != FXS_NONE) {
        m->current = m->pending; m->pending = FXS_NONE;
        m->generation++;
    }
}

/* The row became (1) or stopped being (0) the panel on screen. */
static inline void fxs_activate(struct fxs *m, unsigned int active)
{
    m->shown = (unsigned char)(active != 0);
    m->generation++;
}

/* The owner asks what to do about the panel; the request is consumed. */
static inline unsigned int fxs_take_request(struct fxs *m)
{
    unsigned int r = m->request;
    m->request = FXS_REQ_NONE;
    return r;
}

/* ---- the touch row, as data ------------------------------------------------ */

/* The widget kinds, the values of rx3_panel_api.h (RX3_PAD_*): 0 button, 1 toggle. */
#define FXS_KIND_BUTTON 0u
#define FXS_KIND_TOGGLE 1u

/* The effect a widget stands for now (the direction of SPIRAL and REVERB is the one it shows), or FXS_NONE. */
static inline unsigned int fxs_widget_entry(const struct fxs *m, unsigned int widget)
{
    if (widget == FXS_W_SPIRAL) return m->spiral_down ? FXS_SPIRAL_DOWN : FXS_SPIRAL_UP;
    if (widget == FXS_W_REVERB) return m->reverb_down ? FXS_REVERB_DOWN : FXS_REVERB_UP;
    if (widget < FXS_W_SPIRAL) return widget;            /* HELIX, BPF, HPF, LPF, CRUSH are entries 0..4 */
    return FXS_NONE;
}

static inline unsigned int fxs_kind(unsigned int widget)
{
    return widget < FXS_L_CLOSE ? FXS_KIND_TOGGLE : FXS_KIND_BUTTON;
}

/* Lit: the effect that plays now, or the one that waits for the next OFF. */
static inline int fxs_is_on(const struct fxs *m, unsigned int widget)
{
    unsigned int entry = fxs_widget_entry(m, widget);
    if (entry == FXS_NONE) return 0;
    unsigned int now = m->pending != FXS_NONE ? m->pending : m->current;
    if (widget == FXS_W_SPIRAL) return now == FXS_SPIRAL_UP || now == FXS_SPIRAL_DOWN;
    if (widget == FXS_W_REVERB) return now == FXS_REVERB_UP || now == FXS_REVERB_DOWN;
    return entry == now;
}

static inline unsigned int fxs_put(uint16_t *out, unsigned int at, const char *text)
{
    while (*text && at < FXS_CAPTION_MAX - 1u) out[at++] = (uint16_t)(unsigned char)*text++;
    return at;
}

/* The caption of a widget, as UTF-16 code units (ASCII), NUL terminated. Returns its length. */
static inline unsigned int fxs_caption(const struct fxs *m, unsigned int widget, uint16_t *out)
{
    unsigned int n = 0;
    unsigned int entry = fxs_widget_entry(m, widget);
    if (entry != FXS_NONE) n = fxs_put(out, n, fxs_registry[entry].name);
    else if (widget == FXS_L_CLOSE) n = fxs_put(out, n, "X");
    out[n] = 0;
    return n;
}

/* The accent of a widget (RGB565): red for the ones that build up, blue for the ones that break
   down, the default (0) for HELIX and the X. */
static inline unsigned int fxs_colour(const struct fxs *m, unsigned int widget)
{
    unsigned int entry = fxs_widget_entry(m, widget);
    unsigned int group = entry != FXS_NONE ? fxs_registry[entry].group : 0u;
    return group == 1u ? FXS_RED_565 : group == 2u ? FXS_BLUE_565 : 0u;
}

/* A touch on a widget. SPIRAL and REVERB flip direction when tapped while they play. The X asks to close. */
static inline void fxs_fire(struct fxs *m, unsigned int widget)
{
    if (widget == FXS_L_CLOSE) { m->request = FXS_REQ_CLOSE; return; }
    unsigned int entry = fxs_widget_entry(m, widget);
    if (entry == FXS_NONE) return;
    if (fxs_is_on(m, widget) && (widget == FXS_W_SPIRAL || widget == FXS_W_REVERB))
        entry = widget == FXS_W_SPIRAL ? (entry == FXS_SPIRAL_UP ? FXS_SPIRAL_DOWN : FXS_SPIRAL_UP)
                                       : (entry == FXS_REVERB_UP ? FXS_REVERB_DOWN : FXS_REVERB_UP);
    (void)fxs_choose(m, entry);
}

#endif

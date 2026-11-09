/* SPDX-License-Identifier: MPL-2.0 */
#ifndef RX3_SAMPLES_PANEL_H
#define RX3_SAMPLES_PANEL_H

#include "../core/api/rx3_palette.h"

/* STATUS tab: the ROLL squares, the sound and HOLD, eleven equal cells.
   Six squares, 1/1 on the left to 1/32 on the right, one continuous slider:
   a finger down rolls on the beat grid, sliding moves to the next square in
   time with no restart, lifting stops (HOLD keeps it rolling until HOLD is
   tapped off). KICK SNARE CLAP HAT pick the sound, also while it rolls.
   The roll volume is the AUX LEVEL knob on PORTABLE. */
#define ROLL_LINE_MAX 600u
#define ROLL_LINE_ZONES 6u
enum { PANEL_ROLL_LINE, PANEL_ROLL_KICK, PANEL_ROLL_SNARE, PANEL_ROLL_CLAP, PANEL_ROLL_HAT,
       PANEL_ROLL_HOLD, PANEL_WIDGETS };

static volatile unsigned int roll_line_value; /* under the finger + 1, 0 when idle */

static unsigned int roll_line_division(unsigned int value)
{
    unsigned int zone = value * ROLL_LINE_ZONES / (ROLL_LINE_MAX + 1u);
    return 1u << (zone < ROLL_LINE_ZONES ? zone : ROLL_LINE_ZONES - 1u);
}

static const uint16_t *samples_caption(unsigned int deck, unsigned int widget,
                                       unsigned int part)
{
    (void)deck;
    static uint16_t square[ROLL_LINE_ZONES + 1u][6];
    static const uint16_t hold[] = {'H','O','L','D',0};
    static const uint16_t sounds[ROLL_SOUNDS][6] = {
        {'K','I','C','K',0}, {'S','N','A','R','E',0}, {'C','L','A','P',0}, {'H','A','T',0}
    };
    static const uint16_t none[] = {0};
    if (widget == PANEL_ROLL_HOLD) return hold;
    if (widget >= PANEL_ROLL_KICK && widget <= PANEL_ROLL_HAT) return sounds[widget - PANEL_ROLL_KICK];
    if (!part || part > ROLL_LINE_ZONES) return none;
    unsigned int division = 1u << (part - 1u), at = 0u;
    uint16_t *text = square[part];
    text[at++] = '1'; text[at++] = '/';
    if (division >= 10u) text[at++] = (uint16_t)('0' + division / 10u);
    text[at++] = (uint16_t)('0' + division % 10u);
    text[at] = 0;
    return text;
}

static int samples_is_on(unsigned int deck, unsigned int widget,
                         unsigned int part)
{
    (void)deck; (void)part;
    if (widget == PANEL_ROLL_HOLD) return __atomic_load_n(&roll.hold, __ATOMIC_SEQ_CST) != 0u;
    if (widget >= PANEL_ROLL_KICK && widget <= PANEL_ROLL_HAT)
        return __atomic_load_n(&roll.sound, __ATOMIC_SEQ_CST) == widget - PANEL_ROLL_KICK;
    return __atomic_load_n(&roll.line, __ATOMIC_SEQ_CST) != 0u;
}

static void samples_fire(unsigned int deck, unsigned int widget,
                         unsigned int part)
{
    (void)deck; (void)part;
    if (widget >= PANEL_ROLL_KICK && widget <= PANEL_ROLL_HAT) {
        /* The next hit plays it; the one ringing is not cut. */
        __atomic_store_n(&roll.sound, widget - PANEL_ROLL_KICK, __ATOMIC_SEQ_CST);
        return;
    }
    if (widget != PANEL_ROLL_HOLD) return;
    unsigned int hold = !__atomic_load_n(&roll.hold, __ATOMIC_SEQ_CST);
    __atomic_store_n(&roll.hold, hold, __ATOMIC_SEQ_CST);
    /* HOLD off with no finger on the squares ends a held roll. */
    if (!hold && !roll_line_value) roll_line_touch(0u);
}

static unsigned int samples_slider_max(unsigned int deck, unsigned int widget)
{
    (void)deck; (void)widget;
    return ROLL_LINE_MAX;
}

/* The square that is rolling, as a value inside it; 0 when idle. */
static unsigned int samples_slider_get(unsigned int deck, unsigned int widget)
{
    (void)deck; (void)widget;
    unsigned int line = __atomic_load_n(&roll.line, __ATOMIC_SEQ_CST), zone = 0u;
    if (!line) return 0u;
    while (line > 1u) { line >>= 1u; zone++; }
    return (zone * 2u + 1u) * ROLL_LINE_MAX / (ROLL_LINE_ZONES * 2u);
}

static void samples_slider_set(unsigned int deck, unsigned int widget,
                               unsigned int value, unsigned int committed)
{
    (void)deck; (void)widget;
    roll_line_value = committed ? 0u : value + 1u;
    if (committed && __atomic_load_n(&roll.hold, __ATOMIC_SEQ_CST)) return;
    roll_line_touch(committed ? 0u : roll_line_division(value));
}

/* Swipe up on the held squares: high pass; down: low pass; 120 px is all
   the way. Lifting the finger opens the filter again. */
static void samples_vertical(unsigned int deck, unsigned int widget, int offset)
{
    (void)deck;
    if (widget != PANEL_ROLL_LINE) return;
    int amount = offset * 100 / 120;
    if (amount > 100) amount = 100;
    if (amount < -100) amount = -100;
    __atomic_store_n(&roll.filter, amount, __ATOMIC_SEQ_CST);
}

static void samples_panel_activate(unsigned int active)
{
    /* Leaving the tab with a finger still down must not leave a roll running;
       a held roll goes on. */
    if (!active && roll_line_value) {
        roll_line_value = 0u;
        if (!__atomic_load_n(&roll.hold, __ATOMIC_SEQ_CST)) roll_line_touch(0u);
    }
}

/* Render thread, every frame the tab is up: keeps a long roll on the grid. */
static int samples_panel_needs_refresh(void)
{
    roll_resync();
    static unsigned int seen = 0xffffffffu;
    unsigned int now = __atomic_load_n(&roll.line, __ATOMIC_SEQ_CST) |
        __atomic_load_n(&roll.hold, __ATOMIC_SEQ_CST) << 8u |
        __atomic_load_n(&roll.sound, __ATOMIC_SEQ_CST) << 9u;
    int changed = seen != now;
    seen = now;
    return changed;
}

/* One share per cell: the six squares, four sounds and HOLD come out equal. */
static const struct rx3_pad_widget samples_widgets[PANEL_WIDGETS] = {
    { RX3_PAD_SLIDER, 6 }, { RX3_PAD_TOGGLE, 1 }, { RX3_PAD_TOGGLE, 1 },
    { RX3_PAD_TOGGLE, 1 }, { RX3_PAD_TOGGLE, 1 }, { RX3_PAD_TOGGLE, 1 }
};

/* Panel 3 is the status slot: it replaces the native STATUS tab. The sample
   pads play on RELEASE FX whatever panel is up. */
static const struct rx3_pad_row samples_row = {
    .panel_id = 3u, .tab_image = 0u, .scope = RX3_PAD_SCOPE_SCREEN,
    .count = PANEL_WIDGETS, .widgets = samples_widgets,
    .caption = samples_caption, .is_on = samples_is_on, .fire = samples_fire,
    .slider_max = samples_slider_max, .slider_get = samples_slider_get,
    .slider_set = samples_slider_set, .needs_refresh = samples_panel_needs_refresh,
    .activate = samples_panel_activate, .segments = ROLL_LINE_ZONES,
    .vertical = samples_vertical
};

#include "rx3_samples_fxrelease.h"

/* Panel 6 takes the BEAT FX tab: the sampler's volume, then which bands the
   BEAT FX works on (LOW, MID, HIGH). With the RMX-1000 Roll module on, three
   held buttons follow them (BRAKE, ECHO, SPIN: rx3_samples_fxrelease.h). */
#define VOLUME_WIDGETS 4u
#define FX_WIDGETS 7u
static const uint16_t *samples_volume_caption(unsigned int deck, unsigned int widget,
                                              unsigned int part)
{
    (void)deck; (void)part;
    static const uint16_t volume[] = {'S','A','M','P','L','E',' ','V','O','L',0};
    static const uint16_t low[] = {'L','O','W',0};
    static const uint16_t mid[] = {'M','I','D',0};
    static const uint16_t high[] = {'H','I','G','H',0};
    static const uint16_t brake[] = {'B','R','A','K','E',0};
    static const uint16_t echo[] = {'E','C','H','O',0};
    static const uint16_t spin[] = {'S','P','I','N',0};
    static const uint16_t *const text[FX_WIDGETS] = {volume, low, mid, high, brake, echo, spin};
    return widget < FX_WIDGETS ? text[widget] : volume;
}

static int samples_volume_on(unsigned int deck, unsigned int widget, unsigned int part)
{
    (void)deck; (void)part;
    if (widget >= FXREL_FIRST_WIDGET && widget < FX_WIDGETS)
        return fxrel_lit(widget - FXREL_FIRST_WIDGET);
    return widget >= 1u && widget < VOLUME_WIDGETS && fxbands_original &&
           fxbands_on(widget - 1u);
}

static void samples_volume_fire(unsigned int deck, unsigned int widget, unsigned int part)
{
    (void)deck; (void)part;
    if (widget >= 1u && widget < VOLUME_WIDGETS && fxbands_original) fxbands_toggle(widget - 1u);
}

static unsigned int samples_volume_max(unsigned int deck, unsigned int widget)
{
    (void)deck; (void)widget;
    return 100u;
}

static unsigned int samples_volume_get(unsigned int deck, unsigned int widget)
{
    (void)deck;
    if (widget) return 0u;
    unsigned int volume = __atomic_load_n(&samples_volume, __ATOMIC_SEQ_CST);
    return volume > 100u ? 100u : volume;
}

static void samples_volume_set(unsigned int deck, unsigned int widget,
                               unsigned int value, unsigned int committed)
{
    (void)deck;
    /* BRAKE, ECHO, SPIN are held: the landing and every move say the finger is
       down, the lift (committed) lets go, wherever on the screen it ends. */
    if (widget >= FXREL_FIRST_WIDGET && widget < FX_WIDGETS) {
        fxrel_touch(widget - FXREL_FIRST_WIDGET, committed);
        return;
    }
    if (widget) return;
    if (value > 100u) value = 100u;
    __atomic_store_n(&samples_volume_touched, 1u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&samples_volume, value, __ATOMIC_SEQ_CST);
    if (committed) log_number("sample volume = ", value);
}

static int samples_volume_refresh(void)
{
    static unsigned int seen = 0xffffffffu;
    unsigned int now = __atomic_load_n(&samples_volume, __ATOMIC_SEQ_CST) |
        (__atomic_load_n(&fxbands_mask, __ATOMIC_SEQ_CST) << 8);
    int changed = seen != now;
    seen = now;
    return changed;
}

static const struct rx3_pad_widget samples_volume_widgets[VOLUME_WIDGETS] = {
    { RX3_PAD_SLIDER, 4 }, { RX3_PAD_TOGGLE, 2 }, { RX3_PAD_TOGGLE, 2 }, { RX3_PAD_TOGGLE, 2 }
};

static const struct rx3_pad_row samples_volume_row = {
    .panel_id = 6u, .tab_image = 0u, .scope = RX3_PAD_SCOPE_SCREEN,
    .count = VOLUME_WIDGETS, .widgets = samples_volume_widgets,
    .caption = samples_volume_caption, .is_on = samples_volume_on, .fire = samples_volume_fire,
    .slider_max = samples_volume_max, .slider_get = samples_volume_get,
    .slider_set = samples_volume_set, .needs_refresh = samples_volume_refresh
};

/* The FX row, with the RMX-1000 Roll module on: the same four controls and three held buttons.
   BRAKE, ECHO, SPIN must see the finger land and lift, which only a SLIDER reports, and a slider
   cannot be drawn as a button by the core; so the row paints all seven itself (rx3_pad_brush).
   Cells from the core's solver (gap 9, 1259 px from x 11; the seam of the two screens at x 640 lies
   in the gap between MID and HIGH, so no word crosses it): SAMPLE VOL 319 | LOW 145 | MID 145 |
   HIGH 146 | BRAKE 150 | ECHO 150 | SPIN 150. Words at HelveticaNeue 22: SAMPLE VOL 134 px, LOW 49,
   MID 40, HIGH 55, BRAKE 72, ECHO 62, SPIN 50: every one fits with 78 px or more to spare. */
static const uint32_t fx_release_rgb[FXREL_COUNT] = {0xff4d9au, 0x35c2e8u, 0xf5a524u};   /* BRAKE, ECHO, SPIN */

static uint32_t fx_dim(uint32_t rgb, unsigned int k256)
{
    return ((((rgb >> 16) & 255u) * k256 >> 8) << 16) | ((((rgb >> 8) & 255u) * k256 >> 8) << 8) |
           ((rgb & 255u) * k256 >> 8);
}

/* A frame, a face (neutral, or the accent when lit), the word. `level` 0..100 draws the gauge strip of a
   slider under the word; level < 0 a thin accent line when the button is not lit. */
static void fx_button(const struct rx3_pad_brush *b, int x1, int x2, uint32_t accent, int lit,
                      int level, const char *word)
{
    const int top = RX3_PAD_BRUSH_TOP, bottom = RX3_PAD_BRUSH_BOTTOM;
    if (x2 - x1 < 6) return;
    b->box(x1, top, x2, bottom, lit ? accent : b->frame);
    b->box(x1 + 1, top + 1, x2 - 1, bottom - 1, lit ? accent : b->face);
    if (level >= 0) {
        int left = x1 + 8, right = x2 - 8;
        if (right > left) {
            b->box(left, bottom - 5, right, bottom - 3,
                   b->light ? RX3_PAL_LIGHT_TRACK : RX3_PAL_DARK_TRACK);
            int filled = left + (int)((unsigned int)(right - left + 1) * (unsigned int)level / 100u) - 1;
            if (filled >= left)
                b->box(left, bottom - 5, filled, bottom - 3,
                       b->light ? RX3_PAL_LIGHT_HANDLE : RX3_PAL_DARK_HANDLE);
        }
    } else if (!lit) {
        b->box(x1 + 6, bottom - 3, x2 - 6, bottom - 2, fx_dim(accent, 140u));
    }
    b->text(x1, x2, word, lit);
}

static void samples_fx_paint(unsigned int deck, unsigned int widget, int x1, int x2,
                             const struct rx3_pad_brush *b)
{
    (void)deck;
    static const char *const word[FX_WIDGETS] = {"SAMPLE VOL", "LOW", "MID", "HIGH", "BRAKE", "ECHO", "SPIN"};
    if (widget >= FX_WIDGETS) return;
    if (widget == 0u) {
        unsigned int volume = __atomic_load_n(&samples_volume, __ATOMIC_SEQ_CST);
        if (volume > 100u) volume = 100u;
        fx_button(b, x1, x2, RX3_PAL_SELECT, volume != 0u, (int)volume, word[0]);
    } else if (widget < VOLUME_WIDGETS) {
        fx_button(b, x1, x2, RX3_PAL_SELECT, samples_volume_on(0u, widget, 0u), -1, word[widget]);
    } else {
        fx_button(b, x1, x2, fx_release_rgb[widget - FXREL_FIRST_WIDGET],
                  fxrel_lit(widget - FXREL_FIRST_WIDGET), -1, word[widget]);
    }
}

/* The tab closing (or any other panel taking over) with a finger down: the effect is let go. */
static void samples_fx_activate(unsigned int active)
{
    if (!active) fxrel_let_go();
}

static int samples_fx_refresh(void)
{
    static unsigned int seen = 0xffffffffu;
    unsigned int now = __atomic_load_n(&fxrel_held, __ATOMIC_SEQ_CST) |
        (__atomic_load_n(&fxrel_live, __ATOMIC_SEQ_CST) << 4);
    int changed = seen != now;
    seen = now;
    return samples_volume_refresh() | changed;
}

static const struct rx3_pad_widget samples_fx_widgets[FX_WIDGETS] = {
    { RX3_PAD_SLIDER, 11 }, { RX3_PAD_TOGGLE, 5 }, { RX3_PAD_TOGGLE, 5 }, { RX3_PAD_TOGGLE, 5 },
    { RX3_PAD_SLIDER, 5 }, { RX3_PAD_SLIDER, 5 }, { RX3_PAD_SLIDER, 5 }
};

static const struct rx3_pad_row samples_fx_row = {
    .panel_id = 6u, .tab_image = 0u, .scope = RX3_PAD_SCOPE_SCREEN,
    .count = FX_WIDGETS, .widgets = samples_fx_widgets,
    .caption = samples_volume_caption, .is_on = samples_volume_on, .fire = samples_volume_fire,
    .slider_max = samples_volume_max, .slider_get = samples_volume_get,
    .slider_set = samples_volume_set, .needs_refresh = samples_fx_refresh,
    .activate = samples_fx_activate, .paint = samples_fx_paint
};

/* The row panel 6 gets: the FX row with the RMX-1000 Roll module, the stock one without. */
static const struct rx3_pad_row *samples_volume_registered;
static const struct rx3_pad_row *samples_panel6_row(void)
{
    return fxrel_wanted() ? &samples_fx_row : &samples_volume_row;
}

#endif /* RX3_SAMPLES_PANEL_H */

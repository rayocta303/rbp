/* SPDX-License-Identifier: MPL-2.0 */
#ifndef RX3_STEMS_PANEL_H
#define RX3_STEMS_PANEL_H

#include "rx3_stems_audio.h"
#include "rx3_stems_iso.h"
#include "../core/api/rx3_palette.h"

static const uint16_t stems_text_harmony[] = {'H','A','R','M','O','N','Y',0};
static const uint16_t stems_text_vocal[] = {'V','O','C','E',0};
static const uint16_t stems_text_drums[] = {'D','R','U','M','S',0};
static const uint16_t stems_text_bass[] = {'B','A','S','S',0};
static const uint16_t stems_text_none[] = {'N','O',' ','S','T','E','M','S',0};
static const uint16_t stems_text_failed[] = {'S','T','E','M','S',' ','E','R','R',0};
static const uint16_t stems_text_make[] = {'F','A','I',' ','S','T','E','M','S',0};
static const uint16_t stems_text_queued[] = {'S','U','L',' ','M','A','C','.','.','.',0};
/* MAKE STEMS from the deck: rx3_stems_remote.h. */
static volatile unsigned int stems_remote_state[2];
static int stems_remote_available(void);
static int stems_remote_button(void);
static void stems_remote_request(unsigned int deck);
/* Last committed full-volume adjustment, in the core's millisecond clock. */
static unsigned int stems_slider_until[2][4];

/* SLIP LOOP pads 1 to 4 and the touch controls, in order: VOCE, HARMONY
   (the residual: the mix minus every stored role, so it holds the bass when
   the package has no BASS), DRUMS, BASS. Pads 5 to 8 sit under them in the
   same colours. A role the package lacks is skipped on screen. */
static const unsigned int stems_display_order[4] = {2u,1u,4u,8u};
struct stems_rgb { uint8_t red, green, blue; };
static const struct stems_rgb stems_pad_colour[4] = {
    {0u,255u,0u}, {255u,0u,0u}, {120u,200u,255u}, {160u,0u,255u}
};

static unsigned int stems_control_count(unsigned int available)
{
    unsigned int count = 0u;
    for (unsigned int i = 0; i < 4u; i++) if (available & (1u << i)) count++;
    return count < 2u ? 2u : count;
}

static unsigned int stems_control_bit(unsigned int available, unsigned int control)
{
    for (unsigned int i = 0; i < 4u; i++)
        if (available & stems_display_order[i]) {
            if (!control) return stems_display_order[i];
            control--;
        }
    return 0u;
}

/* How many controls this deck is showing. A deck with nothing loaded shows one,
   which carries the reason rather than a stem name. */
static unsigned int stems_live_count(unsigned int deck)
{
    struct stems_deck_context *context = &stems_decks[deck];
    unsigned int status = __atomic_load_n(&context->status, __ATOMIC_SEQ_CST);
    if (status != 1u && status != 2u) return 1u;
    return stems_control_count(stems_available(context));
}

/* The part's name only: "VOCE HI" and "HARMONY MID" did not fit the pad
   (RX3, 2026-10-15). The knobs stay as they are. */
static const uint16_t *stems_caption(unsigned int deck, unsigned int widget,
                                     unsigned int part)
{
    (void)part;
    struct stems_deck_context *context = &stems_decks[deck];
    unsigned int status = __atomic_load_n(&context->status, __ATOMIC_SEQ_CST);
    if (status != 1u && status != 2u) {
        if (status == 3u) return stems_text_failed;
        if (stems_remote_button() && context->reader)
            return __atomic_load_n(&stems_remote_state[deck], __ATOMIC_SEQ_CST) == 1u
                ? stems_text_queued : stems_text_make;
        return stems_text_none;
    }
    switch (stems_control_bit(stems_available(context), widget)) {
    case 8u: return stems_text_bass;
    case 4u: return stems_text_drums;
    case 2u: return stems_text_vocal;
    default: return stems_text_harmony;
    }
}

/* Lit while the stem is playing; dim while the file is still being read. */
static int stems_is_on(unsigned int deck, unsigned int widget, unsigned int part)
{
    (void)part;
    struct stems_deck_context *context = &stems_decks[deck];
    unsigned int status = __atomic_load_n(&context->status, __ATOMIC_SEQ_CST);
    if (status != 1u && status != 2u) return 0;
    if (!context->reader || !context->armed) return 0;
    unsigned int available = stems_available(context);
    unsigned int selected = stems_selected(context);
    if (!context->payloads[0].data) {
#ifdef RX3_OVERCUE_PROTOTYPE
        if(!context->overcue)
#endif
        return 0;   /* loading: shown dim and steady, no blinking (RX3, 2026-10-15) */
    }
    return (selected & stems_control_bit(available, widget)) != 0u;
}

static void stems_fire(unsigned int deck, unsigned int widget, unsigned int part)
{
    (void)part;
    struct stems_deck_context *context = &stems_decks[deck];
    unsigned int status = __atomic_load_n(&context->status, __ATOMIC_SEQ_CST);
    if (status == 0u && context->reader && stems_remote_button()) {
        stems_remote_request(deck);
        return;
    }
    if (status != 2u || !context->reader || !context->armed) return;
    __atomic_store_n(&stems_slider_until[deck][widget], 0u, __ATOMIC_SEQ_CST);
    unsigned int bit = stems_control_bit(stems_available(context), widget);
    /* As on the pads: muting or enabling a part ends its echo tail. */
    __atomic_fetch_and(&stems_echo[deck].tails, ~bit, __ATOMIC_SEQ_CST);
    stems_toggle(context, bit);
}

static int stems_slider_visible(unsigned int deck, unsigned int widget)
{
    struct stems_deck_context *context = &stems_decks[deck];
    if (context->status != 2u) return 0;
    unsigned int value = stems_level(context, stems_control_bit(stems_available(context), widget));
    unsigned int until = __atomic_load_n(&stems_slider_until[deck][widget], __ATOMIC_SEQ_CST);
    return value && (value < 100u || (until && (int)(until - now_ms()) > 0));
}

/* Watcher thread. While a deck loads, the row is redrawn on each blink phase
   change and nothing else, so the toggles keep the LEDs' parity; the pending
   changes land with the next phase. */
static int stems_panel_needs_refresh(void)
{
    static unsigned int seen[2][5];
    static int last_phase = -1;
    int changed = 0;
    for (unsigned int deck = 0; deck < 2u; deck++) {
#ifdef RX3_OVERCUE_PROTOTYPE
        if(stems_decks[deck].overcue)
            __atomic_store_n(&stems_decks[deck].status,rx3_overcue_status(deck),__ATOMIC_SEQ_CST);
#endif
        unsigned int status = __atomic_load_n(&stems_decks[deck].status, __ATOMIC_SEQ_CST) |
            (__atomic_load_n(&stems_remote_state[deck], __ATOMIC_SEQ_CST) << 8);
        unsigned int selection = __atomic_load_n(&stems_decks[deck].selection, __ATOMIC_SEQ_CST);
        unsigned int levels = __atomic_load_n(&stems_decks[deck].levels, __ATOMIC_SEQ_CST);
        unsigned int mode = 0u;
        for (unsigned int w = 0; w < 4u; w++)
            if (stems_slider_visible(deck, w)) mode |= 1u << w;
        if (seen[deck][2] != levels || seen[deck][3] != mode) changed = 1;
        seen[deck][2] = levels; seen[deck][3] = mode;
        if (seen[deck][0] != status || seen[deck][1] != selection) changed = 1;
        seen[deck][0] = status; seen[deck][1] = selection;
        /* ISO <-> STEMS changes the captions. */
        unsigned int iso = (__atomic_load_n(&stems_channel_stems, __ATOMIC_SEQ_CST) >> deck) & 1u;
        if (seen[deck][4] != iso) changed = 1;
        seen[deck][4] = iso;
    }
    if (stems_any_deck_loading()) {
        int phase = blink_phase_is_on();
        changed = phase != last_phase;
        last_phase = phase;
    } else {
        last_phase = -1;
        if (!changed) stems_blink_idle();
    }
    return changed;
}

static unsigned int stems_widget_kind(unsigned int deck, unsigned int widget)
{
    (void)widget;
#ifdef RX3_OVERCUE_PROTOTYPE
    if(stems_decks[deck].overcue)return RX3_PAD_BUTTON;
#endif
    return stems_decks[deck].status == 2u ? RX3_PAD_TOGGLE_SLIDER : RX3_PAD_BUTTON;
}
static uint16_t stems_widget_colour(unsigned int deck, unsigned int widget)
{
    switch(stems_control_bit(stems_available(&stems_decks[deck]),widget)) {
    case 8u: return RX3_PAL_STEM_BASS_565;
    case 4u: return RX3_PAL_STEM_DRUMS_565;
    case 1u: return RX3_PAL_STEM_HARMONY_565;
    case 2u: return RX3_PAL_STEM_VOCAL_565;
    default: return RX3_PAL_STEM_NONE_565;
    }
}
static unsigned int stems_slider_max(unsigned int deck, unsigned int widget)
{
    (void)deck; (void)widget; return 100u;
}
static unsigned int stems_slider_get(unsigned int deck, unsigned int widget)
{
    struct stems_deck_context *context=&stems_decks[deck];
    return stems_level(context,stems_control_bit(stems_available(context),widget));
}
static void stems_slider_set(unsigned int deck, unsigned int widget,
                             unsigned int value, unsigned int committed)
{
    struct stems_deck_context *context=&stems_decks[deck];
    if(context->status!=2u || !context->reader || !context->armed) return;
    stems_set_level(context,stems_control_bit(stems_available(context),widget),value);
    __atomic_store_n(&stems_slider_until[deck][widget],
                     committed && value >= 100u ? now_ms() + 2000u : 0u, __ATOMIC_SEQ_CST);
}
static const struct rx3_pad_widget stems_widgets[4] = {
    {RX3_PAD_TOGGLE_SLIDER,1}, {RX3_PAD_TOGGLE_SLIDER,1},
    {RX3_PAD_TOGGLE_SLIDER,1}, {RX3_PAD_TOGGLE_SLIDER,1}
};
static void stems_activate(unsigned int active)
{
    __atomic_store_n(&stems_panel_shown, active ? 1u : 0u, __ATOMIC_SEQ_CST);
}
static const struct rx3_pad_row stems_row = {
    .panel_id=2u, .tab_image=TAB_IMAGE_STEMS, .scope=RX3_PAD_SCOPE_DECK,
    .count=4u, .widgets=stems_widgets, .live_count=stems_live_count,
    .caption=stems_caption, .is_on=stems_is_on, .fire=stems_fire,
    .slider_max=stems_slider_max, .slider_get=stems_slider_get, .slider_set=stems_slider_set,
    .needs_refresh=stems_panel_needs_refresh, .kind=stems_widget_kind, .colour=stems_widget_colour,
    .slider_visible=stems_slider_visible, .activate=stems_activate, .level_style=1u
};
#endif /* RX3_STEMS_PANEL_H */

/* SPDX-License-Identifier: MPL-2.0 */
/* RMX-1000 style Roll on the MASTER bus (rbp 1.20). Developer (unstable) build only.
 *
 * ENGINE: the sound of the MASTER output (the sum of the decks, after the mixer,
 * not one channel and not the BFX CH choice) starts repeating at the instant of
 * the press, at a note value of the MASTER deck's tempo, and the music comes back
 * on the release. No beat grid is read or snapped to. Audio is rx3_rmx_roll.h
 * (tested on the computer) on the core's second master client (claim_master_after).
 *
 * THE BEAT FX IS NEVER TOUCHED (Marco's rule): ON/OFF, BEAT < >, the type selector
 * and BFX CH are not read, taken, moved or injected. The LEVEL/DEPTH knob is NOT read either (Marco: BEAT FX untouched). The roll is commanded only by
 *   - the physical pads (BEAT LOOP page in Roll mode, configurable),
 *   - the touch screen (the RMX window, panel 3, the STATUS slot),
 *   - the AUX knob as the roll level.
 * Roll mode (mode=aux): 0x4409 AUX LINE/PORTABLE switch, read and never taken (its own job
 * continues): PORTABLE = Roll mode, LINE = everything native. 0x440a AUX knob: in Roll mode the
 * roll level, taken so the AUX trim stays where it was, handed back when Roll mode ends.
 * mode=always is armed all the time (AUX knob untouched); mode=off does nothing.
 *
 * Pads (BEAT LOOP page, configurable): deck 1 pads 1..7 ROLL at 1/16 1/8 1/4 1/2 1 2 4, pad 8 HOLD;
 * deck 2 pads 1..4 KICK SNARE CLAP H.HAT, 5 OVERDUB, 6 DELETE (held), 7 unused, 8 HOLD. Lights
 * follow the shared state through the unit-wide refresh (RX3_LIGHTS_PADS), only on a deck whose
 * last pad press was on that page (the refresh does not say which page is shown).
 * Screen (panel 3, one row of 39 px across the screen, painted by this module through the
 * core's brush): five length chips, the X-PAD as a window of LED dots (5 rows, 35 columns), ROLL,
 * the four sounds, OVERDUB DELETE HOLD. The core has no free canvas, so this is a strip.
 * OVERDUB writes the sounds into a one-bar loop that plays again each pass (rx3_rmx_layers.h);
 * DELETE (or its latch on the screen) + a sound removes that sound's layer, DELETE alone removes all.
 * Pads and screen share one state machine (rx3_rmx_state.h) behind one spin lock.
 *
 * The key and pad handlers run on the panel's input thread, the touch callbacks on the touch
 * thread, the audio callback on the audio thread (reads a few atomics only).
 */
#include "../core/api/rx3_module_api.h"
#include "rx3_rmx_roll.h"
#include "rx3_rmx_tempo.h"
#include "rx3_rmx_state.h"
#include "rx3_rmx_dots.h"
#include "rx3_rmx_layers.h"
#include "rx3_rmx_filter.h"   /* only rmxf_exp2 is used (a copy of the RMX scene module's: modules may not include each other) */
#include "rx3_rmx_osc.h"      /* the X-PAD oscillator (OSC) */

#define RMX_KEY_AUX_SWITCH 0x4409u
#define RMX_KEY_AUX_LEVEL 0x440au
#define RMX_KEY_MODE_FIRST 0x4113u       /* the pad-mode keys: HOT CUE .. BEAT JUMP */
#define RMX_KEY_MODE_LAST 0x4116u
#define RMX_PAD_FIRST 0x4117u
#define RMX_PAD_LAST 0x411eu
#define RMX_OP_PRESS 0u
#define RMX_OP_RELEASE 2u
#define RMX_OP_VALUE 5u
#define RMX_KNOB_MAX 1023u
#define RMX_RATE 44100u
#define RMX_PLAY_BPM ((unsigned long)0x000fd1fcu)
#define RMX_PANEL_ID 3u   /* the STATUS slot: the tab reads RMX (artwork from the samples module), FX stays beside it */
#define RMX_PAD_HOLD 7u                  /* pad 8 */
#define RMX_PAGE_UNKNOWN 0xffu
#define RMX_SQUARE_MAX 600u              /* the slider's own units, as the sampler's ROLL squares */
#define RMX_DEFAULT_PORTABLE_RAW 2u      /* the AUX selector reports OFF 0, LINE 1, PORTABLE 2 (sampler) */

enum rmx_mode { RMX_MODE_OFF, RMX_MODE_AUX, RMX_MODE_ALWAYS };
/* Where the length pads live: a pad mode and its SHIFT page, or none. */
enum rmx_pads { RMX_PADS_OFF, RMX_PADS_BEAT_LOOP, RMX_PADS_BEAT_LOOP_2, RMX_PADS_SLIP_LOOP };

static const uint8_t rmx_play_bpm_guard[8] __attribute__((unused)) = {0x08, 0x40, 0x2d, 0xe9, 0x00, 0x10, 0xa0, 0xe1};
typedef int (*rmx_bpm_fn)(unsigned int deck);

struct rmx_settings {
    unsigned int mode, capture_before, in_ms, out_ms, start_length, fallback_bpm, tempo_auto;
    unsigned int portable_raw, pads, pad_lights, panel, master_roll;
};

/* Pad n (0..6) is ROLL at this length index; pad 7 is HOLD. */
static const unsigned char rmx_pad_length[RMX_PAD_HOLD] = {0u, 1u, 2u, 3u, 5u, 6u, 7u};
/* The squares and the log say 1/16 1/8 1/4 1/2 3/4 1 2 4 8 16. */
static const char *const rmx_length_name[RMXS_LENGTHS] __attribute__((unused)) =
    {"1/16", "1/8", "1/4", "1/2", "3/4", "1", "2", "4", "8", "16"};

static const struct rx3_services *framework;
static struct rmx_settings rmx_set;
static rmx_bpm_fn rmx_bpm;
static unsigned int rmx_registered, rmx_claimed, rmx_lights_owned, rmx_panel_registered;
static const unsigned char rmx_owner;

/* Everything below up to "Published" is guarded by rmx_lock. */
static volatile unsigned int rmx_lock;
static struct rmxs rmx_s;
static unsigned int rmx_armed_now;
static volatile unsigned int rmx_panel_shown;
static unsigned int rmx_switch_known, rmx_switch_raw;
static unsigned int rmx_aux_known, rmx_aux_raw;
static float rmx_aux_value;
static unsigned int rmx_pad_down;        /* deck 2: bit 16 + pad of each held function pad */
static unsigned int rmx_roll_pad;        /* 1 + deck * 8 + pad of the pad that holds the roll, 0 none */
static unsigned int rmx_injecting;
static unsigned char rmx_deck_page[2];   /* pad page last pressed on each deck (RMX_PAGE_UNKNOWN) */
static unsigned int rmx_raw_logged, rmx_lines;
static unsigned int rmx_aux_moved;                  /* the AUX knob has moved since Roll mode began: until then the level is 0 */
static volatile unsigned int rmx_sound_level_milli;   /* 0..1000: the AUX knob, 0 = muted (the switch is not PORTABLE, or the knob is untouched) */
/* Published to the audio thread and to the lights, the screen and the log. */
static volatile unsigned int rmx_armed, rmx_hold, rmx_press_seq, rmx_loop_frames, rmx_gain_milli;
static volatile unsigned int rmx_pub_roll, rmx_pub_hold, rmx_pub_length, rmx_pub_touch;
static volatile unsigned int rmx_rate_milli = 1000u;   /* speed of the recorded loop, set by the dot matrix zone: 1000 .. 4000 */
/* With a loop recorded the zones 1 1/2 1/4 1/8 1/16 set its speed: the finer the division, the faster the whole line. */
static const unsigned short rmx_zone_rate[5] = {1000u, 1500u, 2000u, 3000u, 4000u};
static volatile unsigned int rmx_matrix_down;          /* a finger is on the dot matrix (the pitch is live) */
/* OSC: a latching button. While it is on, the dot matrix plays an oscillator (rx3_rmx_osc.h) instead of the roll:
   rmx_osc_on is the button, rmx_osc_gate a finger is on the matrix; x and y are rmx_pub_xy. */
static volatile unsigned int rmx_osc_on, rmx_osc_gate;
static struct rmxo rmx_osc;                           /* audio thread only */
static volatile unsigned int rmx_native_valid;     /* the firmware's ROLL label place is known */
static volatile uint16_t rmx_native_box[4];
/* Audio thread only. */
static struct rmxr_state rmx_state;
static struct rmxt_state rmx_tempo_state;     /* listens to the master before the roll; see rx3_rmx_tempo.h */
static float rmx_ring[RMXR_RING_FRAMES * 2u];
/* The sounds and the layers: commands go in through the lock, the audio thread takes them off a ring. */
static struct rmxy rmx_y;
static struct rmxy_sound rmx_sounds[RMXY_SOUNDS];
static void *rmx_sound_block[RMXY_SOUNDS];
static size_t rmx_sound_size[RMXY_SOUNDS];
static volatile unsigned int rmx_muted, rmx_bar_frames, rmx_pub_layers, rmx_pub_gen, rmx_pub_lit, rmx_pub_xy;
static volatile unsigned int rmx_pub_overdub, rmx_pub_sounds;
static volatile unsigned int rmx_selected = 1u;      /* the sound the roll repeats: KICK 0 SNARE 1 CLAP 2 H.HAT 3 */
static unsigned int rmx_delete_used, rmx_loader_claimed;
static unsigned int rmx_touch_zone = 0xffu;  /* sound cell under the finger, 0xff none */

/* The controls work while Roll mode is on OR the RMX window is open (the window is entered by touch). */
static unsigned int rmx_engaged(void) { return rmx_armed_now || __atomic_load_n(&rmx_panel_shown, __ATOMIC_SEQ_CST); }

static unsigned int rmx_aux_mode(void) { return rmx_set.mode == RMX_MODE_AUX; }

static void rmx_lock_take(void)
{
    while (__atomic_exchange_n(&rmx_lock, 1u, __ATOMIC_ACQUIRE)) { }
}
static void rmx_lock_drop(void) { __atomic_store_n(&rmx_lock, 0u, __ATOMIC_RELEASE); }

static unsigned int rmx_number(const char *text, unsigned int fallback, unsigned int limit)
{
    if (!text || text[0] < '0' || text[0] > '9') return fallback;
    unsigned int value = 0;
    for (const char *c = text; *c; c++) {
        if (*c < '0' || *c > '9') return fallback;
        value = value * 10u + (unsigned int)(*c - '0');
        if (value > limit) return fallback;
    }
    return value;
}

static void rmx_load_settings(struct rmx_settings *set)
{
    const char *mode = getenv("RX3_RMX_ROLL_MODE");
    set->mode = !mode ? RMX_MODE_AUX : mode[0] == 'a' && mode[1] == 'l' ? RMX_MODE_ALWAYS
              : mode[0] == 'o' ? RMX_MODE_OFF : RMX_MODE_AUX;
    const char *capture = getenv("RX3_RMX_ROLL_CAPTURE");
    set->capture_before = capture && capture[0] == 'b';
    set->in_ms = rmx_number(getenv("RX3_RMX_ROLL_IN_MS"), 3u, 200u);
    set->out_ms = rmx_number(getenv("RX3_RMX_ROLL_OUT_MS"), 8u, 2000u);
    set->start_length = rmx_number(getenv("RX3_RMX_ROLL_LENGTH"), RMXS_DEFAULT_LENGTH, RMXS_LENGTHS - 1u);
    { const char *t = getenv("RX3_RMX_ROLL_TEMPO"); set->tempo_auto = !(t && t[0] == 'd'); }  /* auto (default) or deck */
    set->fallback_bpm = rmx_number(getenv("RX3_RMX_ROLL_BPM"), 120u, 300u);
    if (set->fallback_bpm < 40u) set->fallback_bpm = 120u;
    set->portable_raw = rmx_number(getenv("RX3_RMX_ROLL_PORTABLE_RAW"), RMX_DEFAULT_PORTABLE_RAW, RMX_KNOB_MAX);
    const char *pads = getenv("RX3_RMX_ROLL_PADS");
    unsigned int pads_end = 0;
    while (pads && pads[pads_end]) pads_end++;
    set->pads = !pads ? RMX_PADS_BEAT_LOOP : pads[0] == 'o' ? RMX_PADS_OFF : pads[0] == 's' ? RMX_PADS_SLIP_LOOP
              : pads[0] == 'b' && pads_end > 1u && pads[pads_end - 1u] == '2' ? RMX_PADS_BEAT_LOOP_2
              : RMX_PADS_BEAT_LOOP;                      /* off, beat-loop, beat-loop-2, slip-loop */
    set->pad_lights = rmx_number(getenv("RX3_RMX_ROLL_PAD_LIGHTS"), 1u, 1u);
    set->panel = rmx_number(getenv("RX3_RMX_ROLL_PANEL"), 1u, 1u);
    /* The roll repeats the SELECTED SOUND (like the first test build). Repeating the MASTER is an
       experiment, off unless RX3_RMX_ROLL_MASTER=1. */
    set->master_roll = rmx_number(getenv("RX3_RMX_ROLL_MASTER"), 0u, 1u);
}

static unsigned int rmx_pad_mode(void)
{
    return rmx_set.pads == RMX_PADS_SLIP_LOOP ? RX3_PAD_SLIP_LOOP : RX3_PAD_BEAT_LOOP;
}
static unsigned int rmx_pad_page(void) { return rmx_set.pads == RMX_PADS_BEAT_LOOP_2 ? 1u : 0u; }
static unsigned int rmx_page_id(unsigned int mode, unsigned int page) { return mode * 2u + (page ? 1u : 0u); }

/* Beats per minute x 100, and where it came from (*from_player: 2 the audio of the master, locked;
   1 the MASTER deck's BPM; 3 TAP; 0 the configured tempo). The audio wins once the tracker has
   locked on the track (rx3_rmx_tempo.h); until then, and with tempo=deck, the player's BPM. */
static unsigned int rmx_tempo(unsigned int *from_player)
{
    unsigned int deck_bpm100 = 0u;
    *from_player = 0u;
    if (rmx_bpm && framework->browse && framework->browse->reference) {
        struct rx3_harmonic_reference master = framework->browse->reference();
        if (master.deck == 0 || master.deck == 1) {
            int bpm = rmx_bpm((unsigned int)master.deck);
            if (bpm >= 4000 && bpm <= 30000) deck_bpm100 = (unsigned int)bpm;
        }
    }
    if (rmx_set.tempo_auto) {
        unsigned int source = RMXT_NONE;
        rmxt_hint(&rmx_tempo_state, deck_bpm100);
        unsigned int bpm100 = rmxt_pick(&rmx_tempo_state, deck_bpm100, &source);
        if (source != RMXT_NONE) {
            *from_player = source == RMXT_AUTO ? 2u : source == RMXT_DECK ? 1u : 3u;
            return bpm100;
        }
    } else if (deck_bpm100) { *from_player = 1u; return deck_bpm100; }
    return rmx_set.fallback_bpm * 100u;
}

/* The level of the sounds and of the roll is the AUX knob over its whole travel, and the AUX selector is
   the mute switch: anything but PORTABLE (OFF, LINE) is silence; PORTABLE starts at 0 and the level
   follows the knob from its first move. Without a switch to read (mode always) the level starts low. Lock held. */
static void rmx_update_level(void)
{
    unsigned int level = 0u;
    if (rmx_armed_now) {
        if (!rmx_aux_mode()) level = 220u;
        else if (rmx_aux_moved) level = (rmx_aux_raw > RMX_KNOB_MAX ? RMX_KNOB_MAX : rmx_aux_raw) * 1000u / RMX_KNOB_MAX;
    }
    __atomic_store_n(&rmx_sound_level_milli, level, __ATOMIC_SEQ_CST);
}

/* (Re)start the roll at the state's length: the instant of this call is the loop's start. */
static void rmx_trigger(void)
{
    unsigned int from_player = 0;
    unsigned int bpm100 = rmx_tempo(&from_player);
    unsigned int length = rmx_s.length;
    unsigned int frames = rmxr_loop_frames(bpm100, rmxr_sixteenths[length], RMX_RATE);
    __atomic_store_n(&rmx_loop_frames, frames, __ATOMIC_SEQ_CST);
    __atomic_store_n(&rmx_hold, 1u, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&rmx_press_seq, 1u, __ATOMIC_SEQ_CST);      /* only the master roll (off by default) reads these */
    if (frames) (void)rmxy_post(&rmx_y, RMXY_C_ROLL_ON, (__atomic_load_n(&rmx_selected, __ATOMIC_SEQ_CST) << 24) | frames);
    if (rmx_lines < 200u) {
        rmx_lines++;
        framework->log_number("rmx1000-roll: roll, tempo x 100 (1 from the player) = ",
                              bpm100 * 10u + from_player);
        framework->log_number("rmx1000-roll: length index = ", length);
        framework->log_number("rmx1000-roll: loop frames = ", frames);
    }
}

/* The roll lengths the five X-PAD zones stand for, left to right: 1 1/2 1/4 1/8 1/16 of a beat. */
static const unsigned char rmx_zone_length[RMXS_ZONES] = {5u, 3u, 2u, 1u, 0u};   /* 1, 1/2, 1/4, 1/8, 1/16 beat, left to right */
static const char *const rmx_sound_name[RMXY_SOUNDS] = {"KICK", "SNARE", "CLAP", "H.HAT"};

/* Frames of one bar (four beats) at the MASTER tempo, for a new overdub loop. */
static unsigned int rmx_bar(void)
{
    unsigned int from_player = 0;
    unsigned int frames = rmxr_loop_frames(rmx_tempo(&from_player), 64u, RMX_RATE);
    return frames ? frames : 2u * RMX_RATE;
}

/* Drain what the state machine queued for the engine, and publish the view. Lock held. */
static void rmx_apply(void)
{
    struct rmxs_event e;
    while (rmxs_take(&rmx_s, &e)) {
        switch (e.kind) {
        case RMXS_E_ROLL_ON: case RMXS_E_ROLL_ZONE: case RMXS_E_ROLL_LENGTH:
            if (rmx_engaged()) rmx_trigger();
            break;
        case RMXS_E_ROLL_OFF:
            __atomic_store_n(&rmx_hold, 0u, __ATOMIC_SEQ_CST);
            (void)rmxy_post(&rmx_y, RMXY_C_ROLL_OFF, 0u);
            break;
        case RMXS_E_SOUND_ON:
            /* OVERDUB on: the hit plays and is written. OVERDUB off: the touch only SELECTS the sound
               (the roll repeats it); nothing sounds. */
            if (e.arg < RMXY_SOUNDS) {
                __atomic_store_n(&rmx_selected, (unsigned int)e.arg, __ATOMIC_SEQ_CST);
                if (rmx_engaged() && rmx_s.overdub) (void)rmxy_post(&rmx_y, RMXY_C_TRIGGER, e.arg);
                /* A roll is going (kept by HOLD, or under a finger): it changes to the new sound NOW, the new sound hits at the
                   touch (no waiting for the next strike) and the roll goes on from there. With OVERDUB the hit was posted above. */
                if (rmx_engaged() && rmx_s.roll_on)
                    (void)rmxy_post(&rmx_y, RMXY_C_ROLL_SOUND, (unsigned int)e.arg | (rmx_s.overdub ? 0x100u : 0u));
            }
            break;
        case RMXS_E_OVERDUB:
            if (rmx_lines < 400u) { rmx_lines++; framework->log_number("rmx1000-roll: OVERDUB = ", e.arg ? 1u : 0u); }
            if (e.arg) __atomic_store_n(&rmx_bar_frames, rmx_bar(), __ATOMIC_SEQ_CST);
            (void)rmxy_post(&rmx_y, RMXY_C_OVERDUB, e.arg);
            break;
        case RMXS_E_DELETE_PAD:
            if (e.arg < RMXY_SOUNDS) { rmx_delete_used = 1u; (void)rmxy_post(&rmx_y, RMXY_C_DELETE, e.arg); }
            break;
        case RMXS_E_HOLD:
            if (rmx_lines < 400u) { rmx_lines++; framework->log_number("rmx1000-roll: H (hold) = ", e.arg ? 1u : 0u); }
            break;
        case RMXS_E_MUTE_PAD:
            break;
        default: break;
        }
    }
    __atomic_store_n(&rmx_muted, rmx_s.muted & 15u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&rmx_pub_roll, rmx_s.roll_on, __ATOMIC_SEQ_CST);
    __atomic_store_n(&rmx_pub_hold, rmx_s.hold, __ATOMIC_SEQ_CST);
    __atomic_store_n(&rmx_pub_length, rmx_s.length, __ATOMIC_SEQ_CST);
    __atomic_store_n(&rmx_pub_touch, rmx_s.owner[RMXS_ROLL] == RMXS_TOUCH, __ATOMIC_SEQ_CST);
    __atomic_store_n(&rmx_pub_overdub, rmx_s.overdub, __ATOMIC_SEQ_CST);
    __atomic_store_n(&rmx_pub_lit, rmxs_view_of(&rmx_s).lit, __ATOMIC_SEQ_CST);
    __atomic_store_n(&rmx_pub_xy, (unsigned int)rmx_s.x | (unsigned int)rmx_s.y << 8, __ATOMIC_SEQ_CST);
    __atomic_store_n(&rmx_pub_gen, rmx_s.generation, __ATOMIC_SEQ_CST);
}

/* DELETE held or latched ends: if no sound was deleted meanwhile, every layer goes. Lock held. */
static void rmx_delete_ended(void)
{
    if (!rmx_delete_used) (void)rmxy_post(&rmx_y, RMXY_C_DELETE_ALL, 0u);
    rmx_delete_used = 0u;
}
static void rmx_delete_begun(void) { rmx_delete_used = 0u; }

/* Straight to the native sendKey (the core runs no handler for an injection). */
static void rmx_send(unsigned int key, unsigned int operation, unsigned int raw, float value)
{
    if (!framework->input || !framework->input->send_key) return;
    rmx_injecting = 1u;
    (void)framework->input->send_key(key, operation, 0u, raw, value);
    rmx_injecting = 0u;
}

static void rmx_set_gain(unsigned int raw)
{
    if (raw > RMX_KNOB_MAX) raw = RMX_KNOB_MAX;
    __atomic_store_n(&rmx_gain_milli, raw * 1000u / RMX_KNOB_MAX, __ATOMIC_SEQ_CST);
}


/* Roll mode begins. Lock held. */
static void rmx_enter(void)
{
    rmx_armed_now = 1u;
    __atomic_store_n(&rmx_armed, 1u, __ATOMIC_SEQ_CST);
    rmx_aux_moved = 0u;                 /* always starts muted: the knob has to be moved */
    rmx_update_level();
    if (rmx_aux_mode()) rmx_set_gain(0u);
}

/* Roll mode ends: every source lets go, HOLD is dropped, the player gets back what the
   hardware shows. Lock held. */
static void rmx_leave(void)
{
    rmx_armed_now = 0u;
    for (unsigned int source = 0; source < RMXS_SOURCES; source++) rmxs_source_lost(&rmx_s, source);
    if (rmx_s.hold) (void)rmxs_press(&rmx_s, RMXS_KEY, RMXS_HOLD, 0u);
    rmx_apply();
    __atomic_store_n(&rmx_armed, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&rmx_hold, 0u, __ATOMIC_SEQ_CST);
    rmx_pad_down = 0u;
    rmx_roll_pad = 0u;
    if (rmx_aux_mode() && rmx_aux_known)
        rmx_send(RMX_KEY_AUX_LEVEL, RMX_OP_VALUE, rmx_aux_raw, rmx_aux_value);
    rmx_aux_moved = 0u;
    rmx_update_level();                 /* muted; the window, if open, stays */
}

static unsigned int rmx_wanted(void)
{
    switch (rmx_set.mode) {
    case RMX_MODE_ALWAYS: return 1u;
    case RMX_MODE_AUX: return rmx_switch_known && rmx_switch_raw == rmx_set.portable_raw;
    default: return 0u;
    }
}

static void rmx_update(void)
{
    unsigned int wanted = rmx_wanted();
    if (wanted && !rmx_armed_now) rmx_enter();
    else if (!wanted && rmx_armed_now) rmx_leave();
}

static int rmx_key(const struct rx3_key_event *event)
{
    if (rmx_injecting) return 0;
    if (event->key >= RMX_KEY_MODE_FIRST && event->key <= RMX_KEY_MODE_LAST) {
        /* A pad-mode key was pressed: which page the pads show is unknown until the next pad press. */
        if (event->operation == RMX_OP_PRESS && event->channel >= 1u && event->channel <= 2u)
            rmx_deck_page[event->channel - 1u] = RMX_PAGE_UNKNOWN;
        return 0;
    }
    if (event->key != RMX_KEY_AUX_SWITCH && event->key != RMX_KEY_AUX_LEVEL) {
        /* "Always" starts on the first event the panel sends; nothing else is read or taken. */
        if (rmx_set.mode == RMX_MODE_ALWAYS && !rmx_armed_now) {
            rmx_lock_take();
            if (!rmx_armed_now) rmx_enter();
            rmx_lock_drop();
        }
        return 0;
    }
    if (event->channel != 0u || event->operation != RMX_OP_VALUE) return 0;
    int taken = 0;
    rmx_lock_take();
    if (event->key == RMX_KEY_AUX_SWITCH) {
        /* Read, never taken: the native LINE/PORTABLE attenuator keeps working. */
        if (!rmx_raw_logged || rmx_switch_raw != event->raw) {
            rmx_raw_logged = 1u;
            framework->log_number("rmx1000-roll: AUX selector raw (PORTABLE is the configured one) = ", event->raw);
        }
        rmx_switch_known = 1u;
        rmx_switch_raw = event->raw;
        rmx_update();
    } else {
        rmx_aux_known = 1u;
        rmx_aux_raw = event->raw;
        rmx_aux_value = event->value;
        if (rmx_armed_now && rmx_aux_mode()) {
            rmx_aux_moved = 1u;
            rmx_set_gain(event->raw);
            rmx_update_level();
            taken = 1;
        }
    }
    rmx_lock_drop();
    return taken;
}

/* Deck 2's pads: 0..3 the sounds, then OVERDUB, DELETE (held), (unused), HOLD. */
static const unsigned char rmx_func_control[8] = {
    0u, 1u, 2u, 3u, RMXS_OVERDUB, RMXS_DELETE, 0xffu, RMXS_HOLD   /* pad 7: unused (no MUTE) */
};

static unsigned int rmx_pad_deck_is_functions(unsigned int deck) { return deck == 1u; }

/* The physical pads: on the configured page in Roll mode, deck 1 is ROLL (lengths and HOLD),
   deck 2 is the sounds and the functions. */
static int rmx_pad(const struct rx3_pad_event *event)
{
    if (event->code < RMX_PAD_FIRST || event->code > RMX_PAD_LAST) return 0;
    unsigned int pad = event->code - RMX_PAD_FIRST;
    unsigned int deck = event->deck < 2u ? event->deck : 0u;
    unsigned int page = rmx_page_id(event->pad_mode, event->pad_page);
    int taken = 0;
    rmx_lock_take();
    if (event->deck < 2u && event->operation == RMX_OP_PRESS) rmx_deck_page[event->deck] = (unsigned char)page;
    if (rmx_armed_now && rmx_set.pads != RMX_PADS_OFF &&
        event->pad_mode == rmx_pad_mode() && event->pad_page == rmx_pad_page()) {
        unsigned int id = 1u + deck * 8u + pad;
        if (rmx_pad_deck_is_functions(deck)) {
            unsigned int control = rmx_func_control[pad];
            if (control == 0xffu) { rmx_lock_drop(); return 0; }   /* the pad stays the player's */
            if (event->operation == RMX_OP_PRESS) {
                taken = 1;
                if (control == RMXS_DELETE) rmx_delete_begun();
                if (rmxs_press(&rmx_s, RMXS_PAD, control, 0u) == RMXS_TAKEN &&
                    control != RMXS_HOLD && control != RMXS_OVERDUB) rmx_pad_down |= 1u << (16u + pad);
                rmx_apply();
            } else if ((event->operation & ~1u) == RMX_OP_RELEASE) {
                taken = 1;
                if (rmx_pad_down & (1u << (16u + pad))) {
                    rmx_pad_down &= ~(1u << (16u + pad));
                    (void)rmxs_release(&rmx_s, RMXS_PAD, control);
                    if (control == RMXS_DELETE) rmx_delete_ended();
                    rmx_apply();
                }
            }
        } else if (event->operation == RMX_OP_PRESS) {
            taken = 1;
            if (pad == RMX_PAD_HOLD) (void)rmxs_press(&rmx_s, RMXS_PAD, RMXS_HOLD, 0u);
            else if (rmxs_press_roll(&rmx_s, RMXS_PAD, rmx_pad_length[pad]) == RMXS_TAKEN) rmx_roll_pad = id;
            rmx_apply();
        } else if ((event->operation & ~1u) == RMX_OP_RELEASE) {
            taken = 1;
            if (pad != RMX_PAD_HOLD && rmx_roll_pad == id) {
                rmx_roll_pad = 0u;
                (void)rmxs_release(&rmx_s, RMXS_PAD, RMXS_ROLL);
                rmx_apply();
            }
        }
    }
    rmx_lock_drop();
    return taken;
}

/* Pad lights, from the unit-wide refresh. Deck 1: the length pad of the rolling length is lit, the
   others dim, HOLD lit while on. Deck 2: each function as the screen shows it. Only in Roll mode,
   only for a deck whose last pad press was on the configured page, never for SHIFT. Input thread;
   reads atomics only. */
static void rmx_light(unsigned int deck, unsigned int control, struct rx3_light *light)
{
    if (!__atomic_load_n(&rmx_armed, __ATOMIC_SEQ_CST) || rmx_set.pads == RMX_PADS_OFF || deck >= 2u ||
        control > RMX_PAD_HOLD ||
        rmx_deck_page[deck] != rmx_page_id(rmx_pad_mode(), rmx_pad_page()))
        return;
    if (rmx_pad_deck_is_functions(deck)) {
        static const uint32_t colour[8] = {0xe5484du, 0xf5a524u, 0xa37cf0u, 0x35c2e8u,
                                           0xff4d9au, 0xd5dadfu, 0x8d96a0u, 0x2e9bffu};
        unsigned int lit = __atomic_load_n(&rmx_pub_lit, __ATOMIC_SEQ_CST);
        unsigned int c = rmx_func_control[control];
        if (c == 0xffu) return;
        int on = (lit >> c) & 1u;
        if (control < RMXY_SOUNDS && ((__atomic_load_n(&rmx_pub_layers, __ATOMIC_SEQ_CST) >> control) & 1u)) on = 1;
        if (control < RMXY_SOUNDS && ((__atomic_load_n(&rmx_muted, __ATOMIC_SEQ_CST) >> control) & 1u))
            on = 0;
        if (control < RMXY_SOUNDS && __atomic_load_n(&rmx_selected, __ATOMIC_SEQ_CST) == control) on = 1;
        light->state = on ? RX3_LIGHT_ON : RX3_LIGHT_DIM;
        light->rgb = colour[control];
        return;
    }
    if (control == RMX_PAD_HOLD) {
        light->state = __atomic_load_n(&rmx_pub_hold, __ATOMIC_SEQ_CST) ? RX3_LIGHT_ON : RX3_LIGHT_DIM;
        light->rgb = 0x2e9bffu;
        return;
    }
    int rolling = __atomic_load_n(&rmx_pub_roll, __ATOMIC_SEQ_CST) &&
                  __atomic_load_n(&rmx_pub_length, __ATOMIC_SEQ_CST) == rmx_pad_length[control];
    light->state = rolling ? RX3_LIGHT_ON : RX3_LIGHT_DIM;
    light->rgb = 0xff7a00u;
}

/* ---- the screen: one row, panel 3 ---------------------------------------------------
 * Five widgets, all painted here with the core's brush:
 *   0 LEN    five chips 1/8 1/4 1/2 1 2: a tap chooses the length (slider, so a finger reaches it)
 *   1 XPAD   the LED-dot window; a finger on it rolls at the zone's length, slides to change it,
 *            lifts to stop; its height (relative drag) is the dots' second parameter
 *   2 ROLL   held = roll at the chosen length
 *   3 SOUNDS KICK SNARE CLAP H.HAT: held = the sound plays (and is written when OVERDUB is on)
 *   4 FUNCS  OSC | OVERDUB | X (4:7:3 of the cell): a tap flips OSC or OVERDUB, X clears the layers. With OSC on the
 *            X-PAD plays an oscillator while the finger is on it (rx3_rmx_osc.h) instead of rolling.
 *            (the console reports one finger, so "hold DELETE and tap a sound" cannot be done by touch)
 * Everything reads and writes the shared state (rx3_rmx_state.h) under rmx_lock. */

enum { RMX_W_HOLD, RMX_W_XPAD, RMX_W_SOUNDS, RMX_W_FUNCS, RMX_W_COUNT };   /* H | X-PAD | sounds | OSC OVERDUB X, left to right */
#define RMX_SLIDER_MAX 1000u
/* The right-hand group, in the order on the screen: OSC (a latch: the X-PAD plays an oscillator), OVERDUB, and X = clear
   everything recorded (a tap, never a latch). H (HOLD) has its own cell on the left. The three share the one cell in
   the proportions 4 : 7 : 3 (OVERDUB is the widest word), in the painter and in the touch alike. */
#define RMX_FUNCS 3u
#define RMX_FUNC_SUM 14u
static const unsigned char rmx_func_ctl[RMX_FUNCS] = {0xffu, RMXS_OVERDUB, RMXS_DELETE};
/* cumulative: the buttons' edges in 14ths. OSC is shelved for now (RX3_RMX_OSC=1 brings it back): without it there is no OSC
   button and OVERDUB and X share the cell 10 : 4. */
static unsigned char rmx_func_share[RMX_FUNCS + 1u] = {0u, 0u, 10u, 14u};
static unsigned int rmx_osc_enabled;
static const char *const rmx_func_name[RMX_FUNCS] = {"OSC", "OVERDUB", "X"};
static volatile unsigned int rmx_x_down;          /* a finger is on X (it lights while touched) */
static const uint32_t rmx_sound_rgb[RMXY_SOUNDS] = {0xe5484du, 0xf5a524u, 0xa37cf0u, 0x35c2e8u};
static const uint32_t rmx_func_rgb[4] = {0xff4d9au, 0xd5dadfu, 0x2e9bffu, 0x2ecc71u};   /* OVERDUB, X, H, OSC */
static const char *const rmx_chip_name[RMXS_ZONES] = {"1", "1/2", "1/4", "1/8", "1/16"};
#define RMX_ROLL_RGB 0xff8a1fu
#define RMX_DOT_PITCH 7             /* rows of the dot window, in px */
static int rmx_xpad_dy;                  /* the finger's height offset, set by the core before slider_set */
static unsigned char rmx_down[RMX_W_COUNT];   /* a finger is down on this widget (landing already seen) */

static const uint16_t *rmx_caption(unsigned int deck, unsigned int widget, unsigned int part)
{
    (void)deck; (void)widget; (void)part;
    return 0;                            /* every widget paints its own lettering */
}

static int rmx_is_on(unsigned int deck, unsigned int widget, unsigned int part)
{
    (void)deck; (void)widget; (void)part;
    return 0;
}

static void rmx_panel_fire(unsigned int deck, unsigned int widget, unsigned int part)
{
    (void)deck; (void)widget; (void)part;
}

static unsigned int rmx_slider_max(unsigned int deck, unsigned int widget)
{
    (void)deck; (void)widget;
    return RMX_SLIDER_MAX;
}

/* The length chip of the shown length, as a position inside it (the core only asks for repaint
   decisions: the painter reads the published state itself). */
static unsigned int rmx_slider_get(unsigned int deck, unsigned int widget)
{
    (void)deck; (void)widget;
    return 0u;
}

static unsigned int rmx_zone_of_value(unsigned int value, unsigned int count)
{
    unsigned int z = value * count / (RMX_SLIDER_MAX + 1u);
    return z >= count ? count - 1u : z;
}

/* Which of OSC, OVERDUB, X a position 0..1000 of the group's cell is on (the same 4 : 7 : 3 as the painter). */
static unsigned int rmx_func_of_value(unsigned int value)
{
    unsigned int slot = value * RMX_FUNC_SUM / (RMX_SLIDER_MAX + 1u);
    return slot < rmx_func_share[1] ? 0u : slot < rmx_func_share[2] ? 1u : 2u;
}

static unsigned int rmx_chip_of_length(unsigned int length)
{
    for (unsigned int z = 0; z < RMXS_ZONES; z++) if (rmx_zone_length[z] == length) return z;
    return 0xffu;
}

/* Lock held. */
static void rmx_touch_up(unsigned int widget)
{
    if (widget == RMX_W_XPAD) {
        __atomic_store_n(&rmx_matrix_down, 0u, __ATOMIC_SEQ_CST);
        __atomic_store_n(&rmx_osc_gate, 0u, __ATOMIC_SEQ_CST);
        (void)rmxs_release(&rmx_s, RMXS_TOUCH, RMXS_ROLL);
    } else if (widget == RMX_W_SOUNDS && rmx_touch_zone < RMXY_SOUNDS) {
        (void)rmxs_release(&rmx_s, RMXS_TOUCH, rmx_touch_zone);
        rmx_touch_zone = 0xffu;
    } else if (widget == RMX_W_FUNCS && __atomic_load_n(&rmx_x_down, __ATOMIC_SEQ_CST)) {
        __atomic_store_n(&rmx_x_down, 0u, __ATOMIC_SEQ_CST);
        rmx_s.generation++;                       /* repaint: X lets go */
    }
}

/* The core calls this on the finger's landing and on every real move (committed 0) and on the
   lift (committed 1), for each of the touch widgets. */
static void rmx_slider_set(unsigned int deck, unsigned int widget, unsigned int value, unsigned int committed)
{
    (void)deck;
    if (widget >= RMX_W_COUNT) return;
    rmx_lock_take();
    unsigned int landing = !rmx_down[widget];
    if (committed) {
        rmx_down[widget] = 0u;
        rmx_touch_up(widget);
    } else if (rmx_engaged()) {
        rmx_down[widget] = 1u;
        switch (widget) {
        case RMX_W_XPAD: {
            /* While OVERDUB is on the X-PAD is off: recording is done with the pads, so the matrix does nothing (no roll,
               no pitch, no loop speed). The window shows OVERDUB in it. */
            if (rmx_s.overdub) break;
            unsigned int x = value * 255u / RMX_SLIDER_MAX;
            int y = 128 - rmx_xpad_dy * 3;
            unsigned int zone = rmxs_zone_of(x);
            __atomic_store_n(&rmx_matrix_down, 1u, __ATOMIC_SEQ_CST);
            if (__atomic_load_n(&rmx_osc_on, __ATOMIC_SEQ_CST)) {
                /* OSC: no roll, no loop speed, no sound; the oscillator speaks while the finger is on the matrix. */
                __atomic_store_n(&rmx_osc_gate, 1u, __ATOMIC_SEQ_CST);
                rmx_s.x = (unsigned short)x;
                rmx_s.y = (unsigned short)(y < 0 ? 0 : y > 255 ? 255 : y);
                rmx_s.generation++;
                break;
            }
            /* The finger's zone sets the roll's length AND, when a loop is recorded, its speed (1 = 1x, 1/2 = 1.5x,
               1/4 = 2x, 1/8 = 3x, 1/16 = 4x: the whole line speeds up, as on the RMX-1000). Both at once: the X-PAD
               never goes dead because OVERDUB is on or a loop exists. With OVERDUB on, the roll is written too. */
            if (__atomic_load_n(&rmx_pub_layers, __ATOMIC_SEQ_CST))
                __atomic_store_n(&rmx_rate_milli, rmx_zone_rate[zone], __ATOMIC_SEQ_CST);
            if (landing) {
                (void)rmxs_press_roll(&rmx_s, RMXS_TOUCH, rmx_zone_length[zone]);
            } else if (rmx_s.owner[RMXS_ROLL] == RMXS_TOUCH && rmx_s.length != rmx_zone_length[zone]) {
                (void)rmxs_press_roll(&rmx_s, RMXS_TOUCH, rmx_zone_length[zone]);
            }
            rmx_s.x = (unsigned short)x;
            rmx_s.y = (unsigned short)(y < 0 ? 0 : y > 255 ? 255 : y);
            rmx_s.generation++;
            break;
        }
        case RMX_W_SOUNDS: {
            unsigned int zone = rmx_zone_of_value(value, RMXY_SOUNDS);
            if (landing || rmx_touch_zone != zone) {
                if (rmx_touch_zone < RMXY_SOUNDS) (void)rmxs_release(&rmx_s, RMXS_TOUCH, rmx_touch_zone);
                rmx_touch_zone = rmxs_press(&rmx_s, RMXS_TOUCH, zone, 0u) == RMXS_TAKEN &&
                                 rmx_s.owner[zone] == RMXS_TOUCH ? zone : 0xffu;
            }
            break;
        }
        case RMX_W_HOLD:
            if (landing) (void)rmxs_press(&rmx_s, RMXS_TOUCH, RMXS_HOLD, 0u);
            break;
        case RMX_W_FUNCS:
            if (landing) {
                unsigned int c = rmx_func_ctl[rmx_func_of_value(value)];
                if (c == 0xffu) {
                    /* OSC: flips and stays. Switching it off lets the oscillator go (it fades out). */
                    unsigned int on = !__atomic_load_n(&rmx_osc_on, __ATOMIC_SEQ_CST);
                    __atomic_store_n(&rmx_osc_on, on, __ATOMIC_SEQ_CST);
                    if (!on) __atomic_store_n(&rmx_osc_gate, 0u, __ATOMIC_SEQ_CST);
                    if (rmx_lines < 400u) { rmx_lines++; framework->log_number("rmx1000-roll: OSC = ", on); }
                    rmx_s.generation++;
                } else if (c == RMXS_DELETE) {
                    /* X: a tap clears every layer and the loop; it lights only while the finger is on it. */
                    (void)rmxy_post(&rmx_y, RMXY_C_DELETE_ALL, 0u);
                    if (rmx_lines < 400u) { rmx_lines++; framework->log_line("rmx1000-roll: X pressed, every layer cleared"); }
                    __atomic_store_n(&rmx_x_down, 1u, __ATOMIC_SEQ_CST);
                    __atomic_store_n(&rmx_rate_milli, 1000u, __ATOMIC_SEQ_CST);
                    rmx_s.generation++;
                } else {
                    (void)rmxs_press(&rmx_s, RMXS_TOUCH, c, 0u);
                }
            }
            break;
        default: break;
        }
    }
    rmx_apply();
    rmx_lock_drop();
}

static void rmx_vertical(unsigned int deck, unsigned int widget, int offset)
{
    (void)deck;
    if (widget == RMX_W_XPAD) rmx_xpad_dy = offset;
}

/* Leaving the tab with a finger still down must not leave a roll or a sound held; a held roll goes on. */
static void rmx_panel_activate(unsigned int active)
{
    __atomic_store_n(&rmx_panel_shown, active ? 1u : 0u, __ATOMIC_SEQ_CST);
    if (active) return;
    rmx_lock_take();
    __atomic_store_n(&rmx_matrix_down, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&rmx_osc_gate, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&rmx_x_down, 0u, __ATOMIC_SEQ_CST);
    for (unsigned int w = 0; w < RMX_W_COUNT; w++) rmx_down[w] = 0u;
    rmx_touch_zone = 0xffu;
    rmxs_source_lost(&rmx_s, RMXS_TOUCH);
    rmx_apply();
    rmx_lock_drop();
}

/* Render thread, while the tab is up: repaint when the shared state moved. */
static int rmx_panel_needs_refresh(void)
{
    static unsigned int seen_a = 0xffffffffu, seen_b = 0xffffffffu;
    unsigned int a = __atomic_load_n(&rmx_pub_gen, __ATOMIC_SEQ_CST);
    unsigned int b = __atomic_load_n(&rmx_pub_layers, __ATOMIC_SEQ_CST) |
        __atomic_load_n(&rmx_gain_milli, __ATOMIC_SEQ_CST) << 4u;
    int changed = seen_a != a || seen_b != b;
    seen_a = a;
    seen_b = b;
    return changed;
}

static uint16_t rmx_colour(unsigned int deck, unsigned int widget)
{
    (void)deck; (void)widget;
    return 0xfc60u;
}

static unsigned int rmx_mix_rgb(unsigned int rgb, unsigned int k256)
{
    return ((((rgb >> 16) & 255u) * k256 >> 8) << 16) | ((((rgb >> 8) & 255u) * k256 >> 8) << 8) | ((rgb & 255u) * k256 >> 8);
}

/* One button: a frame, a face (neutral or the accent when lit), a small layer bar, the word. */
static void rmx_button(const struct rx3_pad_brush *b, int x1, int x2, uint32_t accent, int lit, int dim, int bar,
                       const char *word)
{
    const int top = RX3_PAD_BRUSH_TOP, bottom = RX3_PAD_BRUSH_BOTTOM;
    if (x2 - x1 < 6) return;
    b->box(x1, top, x2, bottom, lit ? accent : b->frame);
    b->box(x1 + 1, top + 1, x2 - 1, bottom - 1, lit ? accent : b->face);
    if (bar) b->box(x1 + 6, bottom - 5, x2 - 6, bottom - 3, dim ? rmx_mix_rgb(accent, 90u) : accent);
    else if (!lit) b->box(x1 + 6, bottom - 3, x2 - 6, bottom - 2, rmx_mix_rgb(accent, 140u));
    b->text(x1, x2, word, lit);
}

/* The dot window, for the shown state (the same logic as rx3_rmx_xpad.h's rmxx_field_update). */
static void rmx_field(struct rmxd_field *f, unsigned int *tint, const char **word)
{
    unsigned int lit = __atomic_load_n(&rmx_pub_lit, __ATOMIC_SEQ_CST);
    unsigned int xy = __atomic_load_n(&rmx_pub_xy, __ATOMIC_SEQ_CST);
    unsigned int x = xy & 255u, y = (xy >> 8) & 255u;
    unsigned int length = __atomic_load_n(&rmx_pub_length, __ATOMIC_SEQ_CST);
    unsigned int chip = rmx_chip_of_length(length);
    unsigned int selected = __atomic_load_n(&rmx_selected, __ATOMIC_SEQ_CST);
    *tint = selected < RMXY_SOUNDS ? rmx_sound_rgb[selected] : RMX_ROLL_RGB;   /* the matrix wears the chosen sound's colour, always */
    *word = "";
    if (((lit >> RMXS_ROLL) & 1u) || __atomic_load_n(&rmx_matrix_down, __ATOMIC_SEQ_CST)) {
        unsigned int zone = ((lit >> RMXS_ROLL) & 1u) && chip < RMXS_ZONES ? chip : x * RMXS_ZONES / 256u;
        rmxd_field_xpad(f, RMXD_ZONE, zone * 51u + 25u, y, 255u);
        return;
    }
    unsigned int n = 0, sum = 0, first = 0;
    for (unsigned int c = 0; c < RMXY_SOUNDS; c++)
        if ((lit >> c) & 1u) { if (!n) first = c; n++; sum += 32u + c * 64u; }
    if (n) {
        rmxd_field_xpad(f, RMXD_BELLS, sum / n, 128u, 190u);
        *tint = rmx_sound_rgb[first];
        return;
    }
    rmxd_field_xpad(f, RMXD_OFF, 0, 0, 0);
    *word = __atomic_load_n(&rmx_pub_overdub, __ATOMIC_SEQ_CST) ? "OVERDUB" :
            chip < RMXS_ZONES ? rmx_chip_name[chip] : "RMX";
}

static void rmx_paint_xpad(const struct rx3_pad_brush *b, int x1, int x2)
{
    const int top = RX3_PAD_BRUSH_TOP, bottom = RX3_PAD_BRUSH_BOTTOM;
    struct rmxd_field f;
    unsigned int tint;
    const char *word;
    rmx_field(&f, &tint, &word);
    b->box(x1, top, x2, bottom, b->frame);
    b->box(x1 + 1, top + 1, x2 - 1, bottom - 1, b->light ? 0x20252au : 0x06080au);
    /* 35 columns across the whole window, 5 rows 7 px apart (the matrix's rows 0 2 4 6 8). */
    int pitch_x = (x2 - x1 + 1 - 4) / (int)RMXD_COLS, pitch_y = RMX_DOT_PITCH;
    if (pitch_x < 3) return;
    int left = x1 + 2 + (x2 - x1 + 1 - 4 - (int)RMXD_COLS * pitch_x) / 2;
    int origin = top + 2 + (bottom - top - 3 - 5 * pitch_y) / 2;
    static const unsigned char row_of[5] = {0u, 2u, 4u, 6u, 8u};
    for (unsigned int r = 0; r < 5u; r++)
        for (unsigned int c = 0; c < RMXD_COLS; c++) {
            unsigned int level = rmxd_level(f.v[row_of[r]][c]);   /* 0 .. 15 */
            int cx = left + (int)c * pitch_x, cy = origin + (int)r * pitch_y;
            int size = level ? 2 + (int)(level * 4u / 15u) : 2;   /* 2 .. 6 px round-ish LEDs */
            int ix = (pitch_x - size) / 2, iy = (pitch_y - size) / 2;
            unsigned int rgb = level ? rmx_mix_rgb(tint, 90u + level * 11u) : (b->light ? 0x38414au : 0x1c2228u);
            b->box(cx + ix, cy + iy, cx + ix + size - 1, cy + iy + size - 1, rgb);
        }
    (void)word;
}

static void rmx_paint(unsigned int deck, unsigned int widget, int x1, int x2, const struct rx3_pad_brush *b)
{
    (void)deck;
    unsigned int lit = __atomic_load_n(&rmx_pub_lit, __ATOMIC_SEQ_CST);
    int width = x2 - x1 + 1;
    switch (widget) {
    case RMX_W_HOLD:
        rmx_button(b, x1, x2, rmx_func_rgb[2], (lit >> RMXS_HOLD) & 1u, 0, 0, "H");
        break;
    case RMX_W_XPAD:
        rmx_paint_xpad(b, x1, x2);
        break;
    case RMX_W_SOUNDS: {
        unsigned int layers = __atomic_load_n(&rmx_pub_layers, __ATOMIC_SEQ_CST);
        unsigned int muted = __atomic_load_n(&rmx_muted, __ATOMIC_SEQ_CST);
        unsigned int selected = __atomic_load_n(&rmx_selected, __ATOMIC_SEQ_CST);
        for (unsigned int k = 0; k < RMXY_SOUNDS; k++) {
            int a = x1 + width * (int)k / (int)RMXY_SOUNDS, e = x1 + width * (int)(k + 1u) / (int)RMXY_SOUNDS - 3;
            rmx_button(b, a, e, rmx_sound_rgb[k], ((lit >> k) & 1u) || k == selected, (muted >> k) & 1u, ((layers >> k) & 1u) || ((muted >> k) & 1u),
                       rmx_sound_name[k]);
        }
        break;
    }
    case RMX_W_FUNCS:
        for (unsigned int k = 0; k < RMX_FUNCS; k++) {
            if (k == 0u && !rmx_osc_enabled) continue;                   /* OSC is shelved: no button */
            int a = x1 + width * (int)rmx_func_share[k] / (int)RMX_FUNC_SUM, e = x1 + width * (int)rmx_func_share[k + 1u] / (int)RMX_FUNC_SUM - 3;
            unsigned int on = k == 0u ? __atomic_load_n(&rmx_osc_on, __ATOMIC_SEQ_CST) :
                              k == 1u ? (lit >> RMXS_OVERDUB) & 1u : __atomic_load_n(&rmx_x_down, __ATOMIC_SEQ_CST);
            rmx_button(b, a, e, k == 0u ? rmx_func_rgb[3] : rmx_func_rgb[k == 1u ? 0u : 1u], on, 0, 0, rmx_func_name[k]);
        }
        break;
    default: break;
    }
}

static const struct rx3_pad_widget rmx_widgets[RMX_W_COUNT] = {
    /* The dot window takes over half the screen (no lettering in it, so the seam at x 640 does it no harm); the
       buttons, which carry words, all lie to the right of the seam: the core draws a letter whole or not at all.
       Widths in the core's 1259 px strip, gap 9: H 82 | X-PAD 574 | sounds 328 (buttons 79 px, widest word SNARE 72) | OSC OVERDUB X 248,
       split 4:7:3 inside (buttons 68, 122, 52 px; words OSC 47, OVERDUB 104, X 13 px). Words measured at HelveticaNeue 22. */
    { RX3_PAD_SLIDER, 1 }, { RX3_PAD_SLIDER, 7 }, { RX3_PAD_SLIDER, 4 }, { RX3_PAD_SLIDER, 3 }
};

static const struct rx3_pad_row rmx_row = {
    .panel_id = RMX_PANEL_ID, .tab_image = 0u, .scope = RX3_PAD_SCOPE_SCREEN,
    .count = RMX_W_COUNT, .widgets = rmx_widgets,
    .caption = rmx_caption, .is_on = rmx_is_on, .fire = rmx_panel_fire,
    .slider_max = rmx_slider_max, .slider_get = rmx_slider_get, .slider_set = rmx_slider_set,
    .needs_refresh = rmx_panel_needs_refresh, .colour = rmx_colour,
    .activate = rmx_panel_activate, .vertical = rmx_vertical, .paint = rmx_paint
};

/* The X-PAD's pitch on the module's own sounds. Audio thread only. `pos` 0..255: 128 is the sound as it is, a finger
   above it plays the sounds higher (up to an octave), below it lower (down to an octave); the ratio follows
   2^((pos - 128) / 128), smoothed block by block so a moving finger bends the pitch without steps. */
#define RMX_BLOCK 1024u
static float rmx_scratch[RMX_BLOCK * 2u];
static double rmx_pitch_now = 1.0;
static float rmx_pitch_next(unsigned int pos)
{
    double target = rmxf_exp2(((double)pos - 128.0) * (1.0 / 128.0));
    rmx_pitch_now += (target - rmx_pitch_now) * 0.35;
    if (rmx_pitch_now > 0.998 && rmx_pitch_now < 1.002) rmx_pitch_now = 1.0;    /* flat is exactly flat */
    return (float)rmx_pitch_now;
}

/* Audio thread: the master bus, after the first client. */
static void rmx_master(struct rx3_stereo *output, unsigned int frames)
{
    struct rmxr_params p;
    p.armed = __atomic_load_n(&rmx_armed, __ATOMIC_SEQ_CST);
    p.hold = __atomic_load_n(&rmx_hold, __ATOMIC_SEQ_CST);
    p.press_seq = __atomic_load_n(&rmx_press_seq, __ATOMIC_SEQ_CST);
    p.loop_frames = __atomic_load_n(&rmx_loop_frames, __ATOMIC_SEQ_CST);
    p.capture_before = rmx_set.capture_before;
    p.in_frames = rmx_set.in_ms * RMX_RATE / 1000u;
    p.out_frames = rmx_set.out_ms * RMX_RATE / 1000u;
    p.gain = (float)__atomic_load_n(&rmx_gain_milli, __ATOMIC_SEQ_CST) * 0.001f;
    if (rmx_set.tempo_auto) rmxt_process(&rmx_tempo_state, (const float *)output, frames);   /* before the roll: it hears the music, not the loop */
    rmx_y.level_target = (float)__atomic_load_n(&rmx_sound_level_milli, __ATOMIC_SEQ_CST) * 0.001f;
    if (rmx_set.master_roll) rmxr_process(&rmx_state, rmx_ring, (float *)output, frames, &p);
    /* The four sounds and the layers, on top of the roll. They run while Roll mode is on, the RMX window is
       open, or a loop or a roll exists (a loop never stops because a switch moved). They are made in a scratch
       block, played at the pitch of the finger on the X-PAD (up = higher, down = lower, middle = as recorded)
       and added to the music; then the limiter. */
    unsigned int osc_on = __atomic_load_n(&rmx_osc_on, __ATOMIC_SEQ_CST);
    rmxo_set(&rmx_osc, osc_on && __atomic_load_n(&rmx_osc_gate, __ATOMIC_SEQ_CST),
             __atomic_load_n(&rmx_pub_xy, __ATOMIC_SEQ_CST) & 255u, (__atomic_load_n(&rmx_pub_xy, __ATOMIC_SEQ_CST) >> 8) & 255u);
    if (p.armed || __atomic_load_n(&rmx_panel_shown, __ATOMIC_SEQ_CST) || rmx_y.bar || rmx_y.roll_on || rmxo_active(&rmx_osc)) {
        if (!rmx_y.bar) __atomic_store_n(&rmx_rate_milli, 1000u, __ATOMIC_SEQ_CST);   /* no loop: back to 1x */
        rmx_y.rate = (float)__atomic_load_n(&rmx_rate_milli, __ATOMIC_SEQ_CST) * 0.001f;
        unsigned int muted = __atomic_load_n(&rmx_muted, __ATOMIC_SEQ_CST), bar = __atomic_load_n(&rmx_bar_frames, __ATOMIC_SEQ_CST);
        /* A loop is on and the tracker has locked on the audio of the master: the bar follows it, so another song
           (or a tempo reading that settled after the first hits) stretches the loop onto the new beat grid. */
        if (rmx_set.tempo_auto && rmx_y.bar) {
            unsigned int source = RMXT_NONE;
            unsigned int bpm100 = rmxt_pick(&rmx_tempo_state, rmx_tempo_state.hint_bpm100, &source);
            if (source == RMXT_AUTO) { unsigned int f = rmxr_loop_frames(bpm100, 64u, RMX_RATE); if (f) bar = f; }
        }
        unsigned int lit = __atomic_load_n(&rmx_pub_lit, __ATOMIC_SEQ_CST);
        unsigned int y_pos = (__atomic_load_n(&rmx_pub_xy, __ATOMIC_SEQ_CST) >> 8) & 255u;
        float *io = (float *)output;
        unsigned int done = 0;
        while (done < frames) {
            unsigned int n = frames - done > RMX_BLOCK ? RMX_BLOCK : frames - done;
            for (unsigned int i = 0; i < n * 2u; i++) rmx_scratch[i] = 0.0f;
            /* With OSC on the finger's height is the oscillator's pitch, and the sounds of the pads stay as they are. */
            rmx_y.pitch = rmx_pitch_next(!osc_on && (((lit >> RMXS_ROLL) & 1u) || __atomic_load_n(&rmx_matrix_down, __ATOMIC_SEQ_CST)) ? y_pos : 128u);
            float level_from = rmx_y.level;
            rmxy_process(&rmx_y, rmx_sounds, muted, bar, rmx_scratch, n);
            rmxo_process(&rmx_osc, rmx_scratch, n, level_from, rmx_y.level);   /* the same level glide as the sounds */
            for (unsigned int i = 0; i < n * 2u; i++) io[done * 2u + i] += rmx_scratch[i];
            done += n;
        }
        __atomic_store_n(&rmx_pub_layers, rmxy_layers(&rmx_y), __ATOMIC_SEQ_CST);
        rmxr_limit(io, frames);                            /* nothing leaves the soft limiter */
    }
}

/* ---- the sounds: kick.wav snare.wav clap.wav hat.wav, from the sampler Roll's own folder ---------
   RX3_ROLL_FILE (set by the samples module when RX3_RUNTIME/roll/ has roll.ini and snare.wav) names
   that folder; RX3_RMX_SOUNDS names another. A missing file is a silent sound: the state works
   all the same. Loaded on the loader thread, before any pad can reach the audio. */
static const char *const rmx_sound_file[RMXY_SOUNDS] = {"kick.wav", "snare.wav", "clap.wav", "hat.wav"};
#define RMX_SOUND_MAX_BYTES (8u * 44100u * 4u + 4096u)

static unsigned int rmx_text_length(const char *t) { unsigned int n = 0; while (t && t[n]) n++; return n; }

static int rmx_load_one(unsigned int i, const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    off_t length = lseek(fd, 0, SEEK_END);
    if (length < 44 || (unsigned long)length > RMX_SOUND_MAX_BYTES) { close(fd); return 0; }
    void *wave = mmap(0, (size_t)length, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (wave == MAP_FAILED) return 0;
    unsigned int bytes;
    const uint8_t *audio = rmxy_wave_data((const uint8_t *)wave, (unsigned long)length, &bytes);
    size_t allocation = (bytes + 4095u) & ~(size_t)4095u;
    void *block = MAP_FAILED;
    if (audio && framework->memory && framework->memory->reserve(&rmx_owner, allocation, 0u)) {
        block = mmap(0, allocation, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (block == MAP_FAILED) framework->memory->abandon(&rmx_owner, allocation);
        else { memcpy(block, audio, bytes); framework->memory->settle(&rmx_owner, allocation); }
    }
    munmap(wave, (size_t)length);
    if (block == MAP_FAILED) return 0;
    rmx_sound_block[i] = block;
    rmx_sound_size[i] = allocation;
    rmx_sounds[i].length = bytes / 4u;
    __atomic_store_n(&rmx_sounds[i].frames, (const int16_t *)block, __ATOMIC_SEQ_CST);
    return 1;
}

static void rmx_load_job(void *context)
{
    (void)context;
    const char *folder = getenv("RX3_RMX_SOUNDS");
    const char *file = getenv("RX3_ROLL_FILE");
    char name[512];
    unsigned int length = 0, loaded = 0;
    const char *source = folder && folder[0] ? folder : file;
    unsigned int n = rmx_text_length(source);
    if (!source || !source[0]) { framework->log_line("rmx1000-roll: no sound folder (RX3_RUNTIME/roll/), the sounds stay silent"); return; }
    length = n;
    if (!(folder && folder[0])) while (length && source[length - 1u] != '/') length--;   /* the folder of snare.wav */
    if (!length || length + 16u > sizeof(name)) return;
    memcpy(name, source, length);
    if (name[length - 1u] != '/') name[length++] = '/';
    for (unsigned int i = 0; i < RMXY_SOUNDS; i++) {
        if (framework->loader && framework->loader->stopping(&rmx_owner)) return;
        unsigned int k = rmx_text_length(rmx_sound_file[i]);
        memcpy(name + length, rmx_sound_file[i], k + 1u);
        if (rmx_load_one(i, name)) loaded |= 1u << i;
    }
    __atomic_store_n(&rmx_pub_sounds, loaded, __ATOMIC_SEQ_CST);
    framework->log_number("rmx1000-roll: sounds loaded (1 kick, 2 snare, 4 clap, 8 hat) = ", loaded);
}

static int configured(void)
{
    const char *value = getenv("RX3_RMX_ROLL");
    return value && value[0] == '1';
}

static void rmx_reset_state(void)
{
    rmxs_reset(&rmx_s);
    rmx_s.length = (unsigned char)rmx_set.start_length;
    rmx_armed_now = 0u;
    rmx_switch_known = rmx_aux_known = 0u;
    rmx_switch_raw = rmx_aux_raw = 0u;
    rmx_aux_value = 0.0f;
    rmx_pad_down = 0u;
    rmx_roll_pad = 0u;
    rmx_injecting = 0u;
    rmx_deck_page[0] = rmx_deck_page[1] = RMX_PAGE_UNKNOWN;
    rmx_raw_logged = rmx_lines = 0u;
    __atomic_store_n(&rmx_matrix_down, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&rmx_rate_milli, 1000u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&rmx_native_valid, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&rmx_hold, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&rmx_press_seq, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&rmx_loop_frames, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&rmx_gain_milli, 0u, __ATOMIC_SEQ_CST);
    rmx_aux_moved = 0u;
    __atomic_store_n(&rmx_sound_level_milli, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&rmx_selected, 1u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&rmx_armed, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&rmx_panel_shown, 0u, __ATOMIC_SEQ_CST);
    rmxy_reset(&rmx_y);
    rmx_y.level = rmx_y.level_target = 0.0f;       /* starts muted */
    rmxo_reset(&rmx_osc);
    {
        const char *osc = getenv("RX3_RMX_OSC");
        rmx_osc_enabled = osc && osc[0] == '1';
        rmx_func_share[1] = rmx_osc_enabled ? 4u : 0u;          /* with OSC: 4 : 7 : 3 of the cell, without: OVERDUB 10, X 4 */
        rmx_func_share[2] = rmx_osc_enabled ? 11u : 10u;
    }
    __atomic_store_n(&rmx_osc_on, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&rmx_osc_gate, 0u, __ATOMIC_SEQ_CST);
    rmx_delete_used = 0u;
    rmx_touch_zone = 0xffu;
    for (unsigned int w = 0; w < RMX_W_COUNT; w++) rmx_down[w] = 0u;
    __atomic_store_n(&rmx_muted, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&rmx_bar_frames, 2u * RMX_RATE, __ATOMIC_SEQ_CST);
    __atomic_store_n(&rmx_pub_layers, 0u, __ATOMIC_SEQ_CST);
    rmx_apply();
}

static int start(const struct rx3_services *services)
{
    framework = services;
    rmx_load_settings(&rmx_set);
    rmxr_reset(&rmx_state);
    rmxt_reset(&rmx_tempo_state, RMX_RATE);
    rmx_registered = rmx_claimed = rmx_lights_owned = rmx_panel_registered = rmx_loader_claimed = 0u;
    rmx_lock = 0u;
    rmx_reset_state();
#if defined(__arm__)
    rmx_bpm = 0;
    if (!memcmp((const void *)RMX_PLAY_BPM, rmx_play_bpm_guard, 8)) rmx_bpm = (rmx_bpm_fn)RMX_PLAY_BPM;
    else services->log_line("rmx1000-roll: UiGetPlayBpm prologue differs, fixed tempo");
#endif
    if (rmx_set.mode == RMX_MODE_OFF) {
        services->log_line("rmx1000-roll: mode off, every key stays native");
        return 1;
    }
    if (!services->input || !services->input->register_key || !services->audio ||
        !services->audio->claim_master_after) {
        services->log_line("rmx1000-roll: services missing, ROLL stays native");
        return 0;
    }
    if (!services->input->register_key(&rmx_owner, 24u, rmx_key)) {
        services->log_line("rmx1000-roll: key handler refused");
        return 0;
    }
    rmx_registered = 1u;
    if (!services->audio->claim_master_after(&rmx_owner, rmx_master)) {
        services->log_line("rmx1000-roll: master bus refused, ROLL stays native");
        services->input->unregister_owner(&rmx_owner);
        rmx_registered = 0u;
        return 0;
    }
    rmx_claimed = 1u;
    /* The pads, their lights and the screen are extras: a refusal costs only that piece. */
    if (rmx_set.pads != RMX_PADS_OFF) {
        if (!services->input->register_pad || !services->input->register_pad(&rmx_owner, 9u, rmx_pad))
            services->log_line("rmx1000-roll: pad handler refused, no ROLL on the pads");
        else if (rmx_set.pad_lights && services->input->register_lights) {
            if (services->input->register_lights(&rmx_owner, RX3_LIGHTS_PADS, rmx_light)) rmx_lights_owned = 1u;
            else services->log_line("rmx1000-roll: pad lights refused, the pads work dark");
        }
    }
    if (rmx_set.panel) {
        if (services->panels && services->panels->register_row && services->panels->register_row(&rmx_row))
            rmx_panel_registered = 1u;
        else services->log_line("rmx1000-roll: screen row refused, no ROLL on the screen");
    }
    /* The four sounds: a refusal costs only the sounds. */
    if (services->loader && services->loader->claim && services->loader->submit) {
        if (services->loader->claim(&rmx_owner)) {
            struct rx3_load_job job = { rmx_load_job, 0, 0 };
            if (services->loader->submit(&rmx_owner, &job)) rmx_loader_claimed = 1u;
            else { services->loader->release(&rmx_owner); services->log_line("rmx1000-roll: loader queue full, the sounds stay silent"); }
        } else services->log_line("rmx1000-roll: loader refused, the sounds stay silent");
    }
    services->log_number("rmx1000-roll: mode (1 AUX selector, 2 always) = ", rmx_set.mode);
    services->log_number("rmx1000-roll: AUX PORTABLE raw = ", rmx_set.portable_raw);
    services->log_number("rmx1000-roll: capture before (1) or after (0) = ", rmx_set.capture_before);
    services->log_number("rmx1000-roll: pads (0 off, 1 BEAT LOOP, 2 BEAT LOOP 2, 3 SLIP LOOP) = ", rmx_set.pads);
    return 1;
}

/* Must also accept a partially completed start. The audio claim goes first: it drains the
   callback, so nothing reads the state while the handlers go. Nothing is re-injected here:
   the player goes away with its own state. */
static void stop(void)
{
    if (rmx_claimed && framework && framework->audio && framework->audio->release_master_after)
        framework->audio->release_master_after(&rmx_owner);
    rmx_claimed = 0u;
    if (rmx_loader_claimed && framework && framework->loader && framework->loader->release)
        framework->loader->release(&rmx_owner);
    rmx_loader_claimed = 0u;
    for (unsigned int i = 0; i < RMXY_SOUNDS; i++) {
        __atomic_store_n(&rmx_sounds[i].frames, (const int16_t *)0, __ATOMIC_SEQ_CST);
        if (rmx_sound_block[i]) {
            munmap(rmx_sound_block[i], rmx_sound_size[i]);
            if (framework && framework->memory) framework->memory->release(&rmx_owner, rmx_sound_size[i]);
            rmx_sound_block[i] = 0;
        }
    }
    if (rmx_panel_registered && framework && framework->panels && framework->panels->unregister_row)
        framework->panels->unregister_row(&rmx_row);
    rmx_panel_registered = 0u;
    if (rmx_registered && framework && framework->input && framework->input->unregister_owner)
        framework->input->unregister_owner(&rmx_owner);
    rmx_registered = rmx_lights_owned = 0u;
    __atomic_store_n(&rmx_hold, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&rmx_armed, 0u, __ATOMIC_SEQ_CST);
    rmx_armed_now = 0u;
}

/* ---- the native ROLL on the BEAT FX touch screen -----------------------------------------------
 * In Roll mode (armed) a tap on the firmware's own ROLL label on the BEAT FX screen opens THIS window
 * (dots, numbered lengths, sounds, OVERDUB/DELETE/HOLD) instead of the old drawing; the tap is taken
 * from the player, so the native ROLL does not start. Only the touch screen is read: no physical BEAT FX
 * button is touched. DEFENSIVE: the label's place is learnt from the text the renderer draws (text_observed,
 * "ROLL", box x1 y1 x2 y2 taken as screen pixels); until it has been seen, or after another text has been drawn
 * over it, or when not armed, or when this window is already up, nothing is taken and the native ROLL behaves
 * as before. Not provable off the console: see the README. */
#define RMX_NATIVE_MARGIN 14

static void rmx_text_observed(const struct rx3_text_observation *t)
{
    if (!t || !t->text) return;
    if (__atomic_load_n(&rmx_panel_shown, __ATOMIC_SEQ_CST)) { __atomic_store_n(&rmx_native_valid, 0u, __ATOMIC_SEQ_CST); return; }
    const uint16_t *c = t->text;
    if (t->length == 4u && (c[0] | 0x20u) == 'r' && (c[1] | 0x20u) == 'o' && (c[2] | 0x20u) == 'l' && (c[3] | 0x20u) == 'l') {
        if (t->box[2] <= t->box[0] || t->box[3] <= t->box[1] || t->box[2] > 1280u || t->box[3] > 800u) return;
        __atomic_store_n(&rmx_native_valid, 0u, __ATOMIC_SEQ_CST);
        for (unsigned int i = 0; i < 4u; i++) rmx_native_box[i] = t->box[i];
        __atomic_store_n(&rmx_native_valid, 1u, __ATOMIC_SEQ_CST);
        return;
    }
    if (__atomic_load_n(&rmx_native_valid, __ATOMIC_SEQ_CST)) {
        /* Another text over the label's centre: the label is gone. */
        unsigned int cx = ((unsigned int)rmx_native_box[0] + rmx_native_box[2]) / 2u, cy = ((unsigned int)rmx_native_box[1] + rmx_native_box[3]) / 2u;
        if (cx >= t->box[0] && cx <= t->box[2] && cy >= t->box[1] && cy <= t->box[3])
            __atomic_store_n(&rmx_native_valid, 0u, __ATOMIC_SEQ_CST);
    }
}

static int rmx_native_touch(int x, int y)
{
    if (rmx_set.mode == RMX_MODE_OFF || __atomic_load_n(&rmx_panel_shown, __ATOMIC_SEQ_CST) ||
        !__atomic_load_n(&rmx_native_valid, __ATOMIC_SEQ_CST) || !rmx_set.panel || !rmx_panel_registered ||
        !framework->panels || !framework->panels->open)
        return 0;
    if (x < (int)rmx_native_box[0] - RMX_NATIVE_MARGIN || x > (int)rmx_native_box[2] + RMX_NATIVE_MARGIN ||
        y < (int)rmx_native_box[1] - RMX_NATIVE_MARGIN || y > (int)rmx_native_box[3] + RMX_NATIVE_MARGIN)
        return 0;
    framework->log_line("rmx1000-roll: native ROLL touched, RMX window opened");
    return framework->panels->open(RMX_PANEL_ID) ? 1 : 0;
}

const struct rx3_module rx3_rmx1000_roll_module = {
    .version = RX3_MODULE_API_VERSION, .size = sizeof(struct rx3_module),
    .name = "rmx1000-roll", .configured = configured, .start = start, .stop = stop,
    .text_observed = rmx_text_observed, .beatfx_touch = rmx_native_touch
};

/* SPDX-License-Identifier: MPL-2.0 */
/* One shared state for the RMX controls, read by the physical pads and by the
 * touchscreen alike, and written out to the pad LEDs and to the graphics.
 * A control (a sound pad, ROLL, HOLD, OVERDUB, DELETE, MUTE) has ONE state
 * whoever presses it: a pad press lights the button on the screen and a touch
 * lights the pad, because both read the same `lit` mask of rmxs_view().
 *
 * Pure logic: no allocation, no I/O, no libc, no threads. The caller (the
 * input thread, or the touch thread behind one lock) feeds it presses and
 * releases with the SOURCE they came from, and drains the events it queues
 * for the audio engine. Original work.
 *
 * Rules (the conflicts the plan names):
 *  - Each control has at most one OWNER, the source that pressed it first. A
 *    press of the same control from another source is refused while it is
 *    owned (RMXS_BUSY); only the owner releases it; a release from anybody
 *    else is ignored.
 *  - Different controls never conflict: a sound pad on the pad and ROLL on the
 *    touch X-PAD sound together.
 *  - HOLD and OVERDUB are toggles: a press from either source flips them.
 *  - DELETE and MUTE are held modifiers: while one is held, a sound pad press
 *    deletes or mutes that pad instead of playing it.
 *  - ROLL is one voice: its owner can slide the zone (the length); a second
 *    source cannot take it over. While HOLD is on, releasing ROLL keeps the roll
 *    going until HOLD is turned off.
 *  - When a source goes away (the pad page is left, the touch panel is closed)
 *    everything it owned is released.
 */
#ifndef RX3_RMX_STATE_H
#define RX3_RMX_STATE_H

#define RMXS_SOUNDS 8u
#define RMXS_ZONES 5u
#define RMXS_LENGTHS 10u              /* ROLL note values: 1/16 1/8 1/4 1/2 3/4 1 2 4 8 16 beats */
#define RMXS_DEFAULT_LENGTH 5u        /* 1 beat */
#define RMXS_FREE 0xffu
#define RMXS_QUEUE 16u

enum rmxs_source { RMXS_PAD = 0, RMXS_TOUCH = 1, RMXS_KEY = 2, RMXS_SOURCES = 3 };
/* Controls 0..7 are the sound pads. */
enum rmxs_control {
    RMXS_ROLL = 8, RMXS_HOLD = 9, RMXS_OVERDUB = 10, RMXS_DELETE = 11, RMXS_MUTE = 12,
    RMXS_CONTROLS = 13
};
enum rmxs_result { RMXS_TAKEN = 0, RMXS_BUSY = 1, RMXS_IGNORED = 2 };
enum rmxs_event_kind {
    RMXS_E_NONE = 0,
    RMXS_E_SOUND_ON, RMXS_E_SOUND_OFF,          /* arg: pad 0..7 */
    RMXS_E_ROLL_ON, RMXS_E_ROLL_ZONE, RMXS_E_ROLL_OFF,   /* arg: zone 0..4 */
    RMXS_E_HOLD,                                 /* arg: 1 on, 0 off */
    RMXS_E_OVERDUB,                              /* arg: 1 on, 0 off (off deletes the loop) */
    RMXS_E_MUTE_PAD,                             /* arg: pad, the layer is now muted or not: see `muted` */
    RMXS_E_DELETE_PAD,                           /* arg: pad */
    RMXS_E_XPAD,                                 /* arg: unused; x and y are in the state */
    RMXS_E_LENGTH,                               /* arg: the new ROLL length 0..9, nothing is rolling */
    RMXS_E_ROLL_LENGTH                           /* arg: the new length 0..9 while a roll sounds: start it again */
};
struct rmxs_event { unsigned char kind, arg; };

struct rmxs {
    unsigned char owner[RMXS_CONTROLS];   /* RMXS_FREE or a source */
    unsigned char zone;                   /* ROLL length zone, 0 .. RMXS_ZONES - 1 (the X-PAD's five zones) */
    unsigned char length;                 /* ROLL note value, 0 .. RMXS_LENGTHS - 1: BEAT < >, pads and touch all move it */
    unsigned char roll_on;                /* a roll is sounding (owned, or kept by HOLD) */
    unsigned char hold, overdub;
    unsigned short muted;                 /* pad layers muted */
    unsigned short x, y;                  /* the X-PAD, 0..255, last touch */
    unsigned int generation;              /* grows with every change: repaint when it moves */
    struct rmxs_event queue[RMXS_QUEUE];
    unsigned char queue_head, queue_count, dropped;
};

struct rmxs_view {
    unsigned int lit;                     /* bit i: sound pad i sounding; then bit (1 << control) for the others */
    unsigned char zone, roll_on, hold, overdub, length;
    unsigned short muted;
};

static inline void rmxs_reset(struct rmxs *s)
{
    for (unsigned int i = 0; i < RMXS_CONTROLS; i++) s->owner[i] = RMXS_FREE;
    s->zone = 0; s->length = RMXS_DEFAULT_LENGTH; s->roll_on = 0; s->hold = 0; s->overdub = 0; s->muted = 0;
    s->x = 128; s->y = 128; s->generation = 0;
    s->queue_head = 0; s->queue_count = 0; s->dropped = 0;
}

static inline void rmxs_emit(struct rmxs *s, unsigned int kind, unsigned int arg)
{
    s->generation++;
    if (s->queue_count >= RMXS_QUEUE) { s->dropped++; return; }
    unsigned int at = (s->queue_head + s->queue_count) % RMXS_QUEUE;
    s->queue[at].kind = (unsigned char)kind;
    s->queue[at].arg = (unsigned char)arg;
    s->queue_count++;
}

/* The next event for the engine, oldest first; 0 when there is none. */
static inline int rmxs_take(struct rmxs *s, struct rmxs_event *out)
{
    if (!s->queue_count) return 0;
    *out = s->queue[s->queue_head];
    s->queue_head = (unsigned char)((s->queue_head + 1u) % RMXS_QUEUE);
    s->queue_count--;
    return 1;
}

static inline unsigned int rmxs_zone_of(unsigned int x)
{
    unsigned int z = (x > 255u ? 255u : x) * RMXS_ZONES / 256u;
    return z >= RMXS_ZONES ? RMXS_ZONES - 1u : z;
}

/* A press. `arg` is the ROLL zone for RMXS_ROLL, ignored otherwise. */
static inline int rmxs_press(struct rmxs *s, unsigned int source, unsigned int control, unsigned int arg)
{
    if (source >= RMXS_SOURCES || control >= RMXS_CONTROLS) return RMXS_IGNORED;
    if (control == RMXS_HOLD) {
        s->hold = !s->hold;
        rmxs_emit(s, RMXS_E_HOLD, s->hold);
        if (!s->hold && s->roll_on && s->owner[RMXS_ROLL] == RMXS_FREE) {
            s->roll_on = 0;                                   /* the kept roll ends with HOLD */
            rmxs_emit(s, RMXS_E_ROLL_OFF, s->zone);
        }
        return RMXS_TAKEN;
    }
    if (control == RMXS_OVERDUB) {
        s->overdub = !s->overdub;
        rmxs_emit(s, RMXS_E_OVERDUB, s->overdub);
        return RMXS_TAKEN;
    }
    if (s->owner[control] != RMXS_FREE) return s->owner[control] == source ? RMXS_IGNORED : RMXS_BUSY;
    if (control < RMXS_SOUNDS) {
        if (s->owner[RMXS_DELETE] != RMXS_FREE) { rmxs_emit(s, RMXS_E_DELETE_PAD, control); return RMXS_TAKEN; }
        if (s->owner[RMXS_MUTE] != RMXS_FREE) {
            s->muted ^= (unsigned short)(1u << control);
            rmxs_emit(s, RMXS_E_MUTE_PAD, control);
            return RMXS_TAKEN;
        }
        s->owner[control] = (unsigned char)source;
        rmxs_emit(s, RMXS_E_SOUND_ON, control);
        return RMXS_TAKEN;
    }
    if (control == RMXS_ROLL) {
        s->owner[RMXS_ROLL] = (unsigned char)source;
        s->zone = (unsigned char)(arg >= RMXS_ZONES ? RMXS_ZONES - 1u : arg);
        if (!s->roll_on) { s->roll_on = 1; rmxs_emit(s, RMXS_E_ROLL_ON, s->zone); }
        else rmxs_emit(s, RMXS_E_ROLL_ZONE, s->zone);         /* HOLD kept it: the new press takes it over */
        return RMXS_TAKEN;
    }
    s->owner[control] = (unsigned char)source;                 /* DELETE, MUTE: held modifiers */
    s->generation++;
    return RMXS_TAKEN;
}

/* The owner moves the ROLL zone (the finger slides, or another zone pad is hit while held). */
static inline int rmxs_roll_zone(struct rmxs *s, unsigned int source, unsigned int zone)
{
    if (source >= RMXS_SOURCES || s->owner[RMXS_ROLL] != source) return RMXS_IGNORED;
    if (zone >= RMXS_ZONES) zone = RMXS_ZONES - 1u;
    if (zone == s->zone) return RMXS_IGNORED;
    s->zone = (unsigned char)zone;
    rmxs_emit(s, RMXS_E_ROLL_ZONE, zone);
    return RMXS_TAKEN;
}

/* The ROLL length, 0 .. RMXS_LENGTHS - 1 (BEAT < > on the panel, a length pad, a square on the screen).
   Free to anyone while nobody holds the roll, and to the holder while somebody does; a roll kept
   by HOLD (nobody holds it) follows the change by starting again at the new length. Anyone else
   is refused while a source holds the roll: the new length is for the next press. */
static inline int rmxs_set_length(struct rmxs *s, unsigned int source, unsigned int length)
{
    if (source >= RMXS_SOURCES) return RMXS_IGNORED;
    if (length >= RMXS_LENGTHS) length = RMXS_LENGTHS - 1u;
    if (s->owner[RMXS_ROLL] != RMXS_FREE && s->owner[RMXS_ROLL] != source) return RMXS_BUSY;
    if (length == s->length) return RMXS_IGNORED;
    s->length = (unsigned char)length;
    rmxs_emit(s, s->roll_on ? RMXS_E_ROLL_LENGTH : RMXS_E_LENGTH, length);
    return RMXS_TAKEN;
}

/* A press of ROLL at a given length (a length pad, the touch squares). The owner pressing
   again is a slide to the new length; another source is refused; a roll kept by HOLD is
   taken over at the new length. */
static inline int rmxs_press_roll(struct rmxs *s, unsigned int source, unsigned int length)
{
    if (source >= RMXS_SOURCES) return RMXS_IGNORED;
    if (s->owner[RMXS_ROLL] != RMXS_FREE) {
        if (s->owner[RMXS_ROLL] != source) return RMXS_BUSY;
        (void)rmxs_set_length(s, source, length);
        return RMXS_TAKEN;
    }
    if (length >= RMXS_LENGTHS) length = RMXS_LENGTHS - 1u;
    s->length = (unsigned char)length;
    return rmxs_press(s, source, RMXS_ROLL, s->zone);
}

static inline int rmxs_release(struct rmxs *s, unsigned int source, unsigned int control)
{
    if (source >= RMXS_SOURCES || control >= RMXS_CONTROLS || control == RMXS_HOLD || control == RMXS_OVERDUB)
        return RMXS_IGNORED;
    if (s->owner[control] != source) return RMXS_IGNORED;
    s->owner[control] = RMXS_FREE;
    if (control < RMXS_SOUNDS) rmxs_emit(s, RMXS_E_SOUND_OFF, control);
    else if (control == RMXS_ROLL) {
        if (!s->hold) { s->roll_on = 0; rmxs_emit(s, RMXS_E_ROLL_OFF, s->zone); }
        else s->generation++;                                   /* kept by HOLD */
    } else s->generation++;
    return RMXS_TAKEN;
}

/* The X-PAD on the screen: a touch is a ROLL press whose zone follows the finger's x,
   and y is the second parameter. `touching` 0 is the lift. */
static inline int rmxs_xpad(struct rmxs *s, unsigned int source, unsigned int x, unsigned int y, unsigned int touching)
{
    if (!touching) return rmxs_release(s, source, RMXS_ROLL);
    unsigned int zone = rmxs_zone_of(x);
    int r;
    if (s->owner[RMXS_ROLL] == RMXS_FREE) r = rmxs_press(s, source, RMXS_ROLL, zone);
    else if (s->owner[RMXS_ROLL] == source) r = rmxs_roll_zone(s, source, zone);
    else return RMXS_BUSY;                                      /* another source holds the roll */
    s->x = (unsigned short)(x > 255u ? 255u : x);
    s->y = (unsigned short)(y > 255u ? 255u : y);
    rmxs_emit(s, RMXS_E_XPAD, 0);
    return r == RMXS_IGNORED ? RMXS_TAKEN : r;
}

/* A source is gone (page left, panel closed): everything it owned lets go. */
static inline void rmxs_source_lost(struct rmxs *s, unsigned int source)
{
    for (unsigned int c = 0; c < RMXS_CONTROLS; c++)
        if (s->owner[c] == source) (void)rmxs_release(s, source, c);
}

static inline struct rmxs_view rmxs_view_of(const struct rmxs *s)
{
    struct rmxs_view v;
    v.lit = 0;
    for (unsigned int c = 0; c < RMXS_SOUNDS; c++) if (s->owner[c] != RMXS_FREE) v.lit |= 1u << c;
    if (s->roll_on) v.lit |= 1u << RMXS_ROLL;
    if (s->hold) v.lit |= 1u << RMXS_HOLD;
    if (s->overdub) v.lit |= 1u << RMXS_OVERDUB;
    if (s->owner[RMXS_DELETE] != RMXS_FREE) v.lit |= 1u << RMXS_DELETE;
    if (s->owner[RMXS_MUTE] != RMXS_FREE) v.lit |= 1u << RMXS_MUTE;
    v.zone = s->zone; v.length = s->length; v.roll_on = s->roll_on; v.hold = s->hold; v.overdub = s->overdub; v.muted = s->muted;
    return v;
}

#endif

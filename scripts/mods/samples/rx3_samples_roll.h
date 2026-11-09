/* SPDX-License-Identifier: MPL-2.0 */
#ifndef RX3_SAMPLES_ROLL_H
#define RX3_SAMPLES_ROLL_H
/* Roll on the ROLL line of the STATUS tab: a drum sound from the drive
 * retriggered at 1/1 .. 1/32 of a beat of the MASTER deck, on the master
 * bus. BEAT FX and the AUX trim stay stock.
 *
 * RX3_RUNTIME/roll/ on the drive (written by the app, docs/ROLL_INI.md):
 *   kick.wav snare.wav clap.wav hat.wav  44.1 kHz PCM16 stereo, at most 8 s
 * KICK SNARE CLAP HAT on the line pick the sound; snare.wav is required, a
 * missing one plays the snare.
 *
 * Timing (roll_mix): the hits are placed on the audio thread, sample by
 * sample, where the MASTER deck's grid lines cross its playback stream
 * (framework->audio->playhead, the TimeStretch stage of the same cycle). The
 * grid (a beat and its length) and the deck's speed are read off the native
 * engine on touch and every 200 ms, never the moment of a hit. Without a
 * grid or a moving deck, a timer at the deck's BPM (UiGetPlayBpm @0xfd1fc). */
#define ROLL_SOUNDS 4u
#define ROLL_SNARE 1u
static const char *const roll_sound_files[ROLL_SOUNDS] = {"kick.wav", "snare.wav", "clap.wav", "hat.wav"};
#define ROLL_DIVISION_MAX 5u
#define ROLL_DEFAULT_BEAT_FRAMES 22050u /* 120 BPM without a reading */
#define ROLL_PLAY_BPM ((unsigned long)0x000fd1fc)
static const uint8_t roll_play_bpm_guard[8] = {
    0x08, 0x40, 0x2d, 0xe9, 0x00, 0x10, 0xa0, 0xe1
};
typedef int (*roll_bpm_fn)(unsigned int deck);

/* The beat grid, as the quantized Beat FX reads it
   (mixerengine::QuantizedBeatEffect::searchNearestBeat @0x99168, rbp 1.20):
   BeatEffect::getPlayEngine() @0xb5e94 (static in effect: `this` unused),
   PlayEngine::getBeatPosInfo(channel) @0x5f5bc -> common::BeatPosition*,
   PlayEngine::getPlayingPosition(channel) @0x5d950 (samples at 44.1 kHz),
   PlayEngine::getTempo(channel, bool) @0x5dd24 (fraction, float in r0).
   BeatPosition: +0x24 grid present, +0x28 grid offset in ms, +0x34/+0x38
   the beats, 8 bytes each: beat in bar and BPM x 100 (two halves), then
   the beat's time in ms from the offset. */
#define ROLL_GET_ENGINE ((unsigned long)0x000b5e94)
#define ROLL_BEAT_INFO ((unsigned long)0x0005f5bc)
#define ROLL_POSITION ((unsigned long)0x0005d950)
#define ROLL_TEMPO ((unsigned long)0x0005dd24)
static const uint8_t roll_engine_guard[8] = {0xf8,0x40,0x2d,0xe9,0x9c,0x40,0x9f,0xe5};
static const uint8_t roll_info_guard[8] = {0x0c,0x30,0x90,0xe5,0x10,0x20,0x90,0xe5};
static const uint8_t roll_position_guard[8] = {0x02,0x00,0x51,0xe3,0x10,0x40,0x2d,0xe9};
static const uint8_t roll_tempo_guard[8] = {0x08,0x40,0x2d,0xe9,0x0c,0x30,0x90,0xe5};
typedef void *(*roll_engine_fn)(void *unused);
typedef const uint8_t *(*roll_info_fn)(void *engine, unsigned int channel);
typedef long (*roll_position_fn)(void *engine, unsigned int channel);
typedef float (*roll_tempo_fn)(void *engine, unsigned int channel, int flag);

struct roll_sound {
    const int16_t *frames;
    unsigned int length;
    void *block;
    size_t block_size;
};

static struct {
    /* The snare's, for "a roll is possible". */
    const int16_t *frames;
    struct roll_sound sounds[ROLL_SOUNDS];
    volatile unsigned int sound;         /* the one KICK..HAT selects */
    unsigned int divisions[ROLL_DIVISION_MAX], division_count;
    roll_bpm_fn bpm;
    /* Input thread writes, audio thread reads. */
    volatile unsigned int active, interval, volume, division;
    /* The timer's beat in frames: its hits are the 1/division lines of it. */
    volatile unsigned int beat;
    /* Touch line: the division under the finger, 0 when no finger is down. */
    volatile unsigned int line;
    /* Output frames from the start to the first hit, on the grid. */
    volatile unsigned int wait;
    /* Render thread -> audio: frames to the next grid hit + 1, 0 = none. Moves
       the timer onto the grid without cutting the hit that is ringing. */
    volatile unsigned int resync;
    volatile unsigned int hold;          /* HOLD: lifting the finger keeps it */
    /* Filter from the finger's height on the squares: -100 (swiped up, high
       pass) .. 0 (open) .. +100 (swiped down, low pass). */
    volatile int filter;
    float svf[2][2];                     /* audio thread: filter state, per channel */
    unsigned int grid;   /* the grid functions passed their guards */
    unsigned int volume_key;             /* AUX LEVEL (RX3_ROLL_VOLUME_KEY overrides) */
    unsigned int portable_raw;           /* AUX selector value for PORTABLE */
    unsigned int advance;                /* output frames the hits come early */
    unsigned int switch_raw, switch_known;
    unsigned int keys;                   /* knob watcher registered */
    unsigned int resync_ms;
    /* The MASTER deck's grid for the audio thread, written by the input and
       render threads under `grid_seq` (odd while writing): a beat's reader
       frame, a beat's length in reader frames, and reader frames per output
       frame (1 + tempo). */
    volatile unsigned int grid_seq;
    volatile int grid_anchor;
    volatile unsigned int grid_beat, grid_deck;
    volatile float grid_rate;
    unsigned int playhead;               /* framework->audio->use_playhead() held */
    /* Audio thread only. */
    /* Timer: frames into the beat, the next hit's place in it, and the
       division it was worked out for. */
    unsigned int phase, next, timed;
    unsigned int hit, playing;           /* `playing`: the sound the hit restarted */
    unsigned int seen_serial, stale_blocks, locked;
    /* Reader frame at the next output block's start, moved on by each block.
       The deck reads its file in its own blocks, so the last read position
       can stand still for several output blocks and then jump: following it
       saw a quarter of the grid lines (RX3, 17/10: 1/1 hit once in 4 beats). */
    double cursor;                       /* double: mid-track a float loses samples */
    unsigned int cursor_valid;
    int last_hit;                        /* line of the last grid hit, in the hits' own frames (see loop_shift) */
    /* A loop the master deck plays: the read position jumps back by the same
       amount every pass. The first jump is taken for a cue and puts the roll
       on the grid; the second, from and to the same places, is a loop, and
       from then on the roll's lines go on across the jump (`loop_shift` is
       what the passes added up to, mod one beat) instead of starting over at
       each pass (RX3, 25/10: "the roll always starts over in a loop"). */
    double loop_end, loop_start, loop_len, loop_shift;
    unsigned int loop_confirmed;
} roll;

static unsigned int roll_parse_divisions(const char *text, unsigned int *out)
{
    unsigned int count = 0u, value = 0u, digits = 0u;
    for (const char *c = text ? text : "";; c++) {
        if (*c >= '0' && *c <= '9' && digits < 3u) { value = value * 10u + (unsigned int)(*c - '0'); digits++; continue; }
        if (*c != ',' && *c != '\0') return 0u;
        if (!digits || (value != 1u && value != 2u && value != 4u && value != 8u && value != 16u))
            return 0u;
        if (count == ROLL_DIVISION_MAX) return 0u;
        out[count++] = value;
        value = digits = 0u;
        if (*c == '\0') return count;
    }
}

/* One beat of the MASTER deck in frames; the input thread only. */
static unsigned int roll_beat_frames(void)
{
    if (roll.bpm && framework->browse && framework->browse->reference) {
        struct rx3_harmonic_reference master = framework->browse->reference();
        if (master.deck == 0 || master.deck == 1) {
            int bpm = roll.bpm((unsigned int)master.deck);
            if (bpm >= 4000 && bpm <= 30000)
                return (44100u * 60u * 100u + (unsigned int)bpm / 2u) / (unsigned int)bpm;
        }
    }
    return ROLL_DEFAULT_BEAT_FRAMES;
}

static void roll_retime(void)
{
    unsigned int line = __atomic_load_n(&roll.line, __ATOMIC_SEQ_CST);
    unsigned int division = line ? line : roll.divisions[roll.division % roll.division_count];
    unsigned int beat = roll_beat_frames();
    unsigned int interval = beat / division;
    __atomic_store_n(&roll.beat, beat, __ATOMIC_SEQ_CST);
    __atomic_store_n(&roll.interval, interval ? interval : 1u, __ATOMIC_SEQ_CST);
}

/* Audio thread: the timer's next 1/division line of the beat after `phase`.
   Lines of every division fall on the same beat, so changing the division
   keeps time (RX3, 17/10: 1/1, 1/2, 1/1 came back off the beat when the
   timer counted from the last hit). At most `beat`, the next beat's start. */
static unsigned int roll_timer_next(unsigned int phase, unsigned int beat, unsigned int division)
{
    unsigned int slot = phase * division / beat;      /* < 132300 * 32: no overflow */
    unsigned int next = (slot + 1u) * beat / division;
    /* Lines are rounded down, so `phase` can sit on line slot + 1 itself. */
    if (next <= phase) next = (slot + 2u) * beat / division;
    return next;
}

static void roll_start(void)
{
    roll_retime();
    __atomic_store_n(&roll.active, 2u, __ATOMIC_SEQ_CST); /* 2: restart at the next block */
}

/* Why the last grid reading failed, for the log: 0 found, else a reason. */
static unsigned int roll_grid_reason;

/* The MASTER deck's beat at or before its play position, all in samples at
   44.1 kHz. The grid is in milliseconds: Quantize::calcSampleFromBeat
   @0xb4e00 takes (beat + offset) * 44.1, and QuantizedBeatEffect::
   calcPlayingTime @0x990a4 divides the play position by 44.1 before
   searchNearestBeat. Reading the beats as samples put every play position
   past the last beat (RX3, 16/10: reason 6, the roll never on the grid). */
struct roll_beat {
    long played;            /* getPlayingPosition */
    long anchor;            /* where the beat starts */
    unsigned int beat;      /* its length */
    unsigned int channel;
    float tempo;
};
static int roll_grid_locate(struct roll_beat *out)
{
#if defined(__arm__)
    roll_grid_reason = 1u;
    if (!roll.grid || !framework->browse || !framework->browse->reference) return 0;
    struct rx3_harmonic_reference master = framework->browse->reference();
    roll_grid_reason = 2u;
    if (master.deck != 0 && master.deck != 1) return 0;
    unsigned int channel = (unsigned int)master.deck;
    void *engine = ((roll_engine_fn)ROLL_GET_ENGINE)(0);
    roll_grid_reason = 3u;
    if (!engine) return 0;
    const uint8_t *info = ((roll_info_fn)ROLL_BEAT_INFO)(engine, channel);
    roll_grid_reason = 4u;
    if (!info || !*(const uint32_t *)(info + 0x24)) return 0;
    long offset = *(const long *)(info + 0x28);
    const uint32_t *first = *(const uint32_t *const *)(info + 0x34);
    const uint32_t *last = *(const uint32_t *const *)(info + 0x38);
    if (!first || last <= first + 2) return 0;
    unsigned long count = (unsigned long)(last - first) / 2u;
    long played = ((roll_position_fn)ROLL_POSITION)(engine, channel);
    /* The raw grid next to the play position, a few times per power-on. */
    static unsigned int dumps;
    if (dumps < 3u) {
        dumps++;
        log_number("roll grid: deck = ", channel + 1u);
        log_number("roll grid: beats = ", count);
        log_number("roll grid: play position = ", (unsigned long)played);
        log_number("roll grid: offset ms = ", (unsigned long)offset);
        log_number("roll grid: first beat ms = ", first[1]);
        log_number("roll grid: second beat ms = ", first[3]);
        log_number("roll grid: last beat ms = ", first[(count - 1u) * 2u + 1u]);
    }
    double ms = (double)played * (1.0 / 44.1) - (double)offset;
    roll_grid_reason = 5u;
    if (ms < (double)first[1]) return 0;
    unsigned long low = 0u, high = count - 1u;
    while (low < high) {
        unsigned long middle = (low + high + 1u) / 2u;
        if ((double)first[middle * 2u + 1u] <= ms) low = middle; else high = middle - 1u;
    }
    roll_grid_reason = 6u;
    if (low + 1u >= count) return 0;
    /* The beat's length from its BPM x 100 (the upper half of its first word;
       Bpm::getBeatWidthMsec @0xaa140 takes 6000000 / it), finer than the
       whole milliseconds between two beats; those when it is missing. */
    unsigned int bpm = first[low * 2u] >> 16;
    double beat = bpm >= 2000u && bpm <= 60000u
                      ? 264600000.0 / (double)bpm
                      : ((double)first[(low + 1u) * 2u + 1u] - (double)first[low * 2u + 1u]) * 44.1;
    roll_grid_reason = 7u;
    if (beat < 4410.0 || beat > 132300.0) return 0;          /* 20..600 BPM */
    roll_grid_reason = 0u;
    float tempo = ((roll_tempo_fn)ROLL_TEMPO)(engine, channel, 0);
    if (!(tempo > -0.95f && tempo < 3.0f)) tempo = 0.0f;
    out->played = played;
    out->anchor = (long)(((double)first[low * 2u + 1u] + (double)offset) * 44.1 + 0.5);
    out->beat = (unsigned int)(beat + 0.5);
    out->channel = channel;
    out->tempo = tempo;
    return 1;
#else
    (void)out;
    return 0;
#endif
}

/* Output frames until the next 1/division of the MASTER deck's beat grid,
   or 0 (start now) without a grid or a sane reading. Input thread. */
static unsigned int roll_grid_wait(unsigned int division)
{
    struct roll_beat at;
    if (!division || !roll_grid_locate(&at)) return 0u;
    /* Unsigned: rbp exports no signed division helper. */
    unsigned long step = at.beat / division;
    if (!step) return 0u;
    unsigned long into = at.played > at.anchor ? (unsigned long)(at.played - at.anchor) % step : 0u;
    unsigned long ahead = into ? step - into : 0u;
    unsigned int wait = (unsigned int)((float)ahead / (1.0f + at.tempo));
    unsigned int period = (unsigned int)((float)step / (1.0f + at.tempo));
    /* The deck reaches the output later than its play position says: the
       snare came in behind the beat (RX3, 2026-10-15). Hit that much early. */
    unsigned int advance = roll.advance % (period ? period : 1u);
    wait = wait >= advance ? wait - advance : wait + period - advance;
    return wait ? wait : 1u;
}

/* Publish the MASTER deck's grid for the audio thread. Input and render
   threads; returns 0 when there is no usable grid. */
static int roll_grid_refresh(void)
{
    struct roll_beat at;
    if (!roll_grid_locate(&at)) return 0;
    unsigned int seq = __atomic_load_n(&roll.grid_seq, __ATOMIC_SEQ_CST);
    __atomic_store_n(&roll.grid_seq, seq | 1u, __ATOMIC_SEQ_CST);
    roll.grid_anchor = (int)at.anchor;
    roll.grid_beat = at.beat;
    roll.grid_deck = at.channel;
    roll.grid_rate = 1.0f + at.tempo;
    __atomic_store_n(&roll.grid_seq, (seq | 1u) + 1u, __ATOMIC_SEQ_CST);
    /* How far the stream the hits follow is from the engine's play position:
       the latency the old early hits made up for. A few lines per power-on. */
    static unsigned int reported;
    struct rx3_playhead head;
    if (reported < 12u && framework->audio->playhead(at.channel, &head)) {
        reported++;
        long gap = (long)head.position - at.played;
        log_number(gap < 0 ? "roll: stream behind play position, frames = "
                           : "roll: stream ahead of play position, frames = ",
                   (unsigned long)(gap < 0 ? -gap : gap));
        log_number("roll: deck block frames = ", head.frames);
    }
    return 1;
}

/* The ROLL line on the screen (STATUS tab): a finger down starts the roll at
   the division under it, sliding changes the division on the same grid, and
   lifting it stops the roll. BEAT FX stays the stock effect. */
static void roll_line_touch(unsigned int division)
{
    if (!__atomic_load_n(&roll.frames, __ATOMIC_SEQ_CST)) return;
    unsigned int was = __atomic_load_n(&roll.line, __ATOMIC_SEQ_CST);
    __atomic_store_n(&roll.line, division, __ATOMIC_SEQ_CST);
    if (!division) {
        __atomic_store_n(&roll.active, 0u, __ATOMIC_SEQ_CST);
        return;
    }
    roll_retime();
    if (!was) {
        int ok = roll_grid_refresh();
        /* What the roll started with, for the first touches of a power-on. */
        static unsigned int told;
        if (told < 40u) {
            told++;
            log_number("roll: start at 1/", division);
            log_number("roll: sound (0 kick, 1 snare, 2 clap, 3 hat) = ", __atomic_load_n(&roll.sound, __ATOMIC_SEQ_CST));
            log_number(ok ? "roll: on the grid, beat frames = "
                          : "roll: no grid, BPM timer; reason (1 guards, 2 no MASTER, 3 engine, 4 no grid, "
                            "5 before first beat, 6 after last beat, 7 odd beat) = ",
                       ok ? roll.grid_beat : roll_grid_reason);
        }
    }
    if (!was) {
        /* First touch: the first hit waits for the grid. */
        __atomic_store_n(&roll.wait, roll_grid_wait(division), __ATOMIC_SEQ_CST);
        roll_start();
    } else if (was != division) {
        /* Another square: same roll, next hit on the new division's grid, and
           the hit already ringing is not cut, so squares join seamlessly. */
        __atomic_store_n(&roll.resync, roll_grid_wait(division) + 1u, __ATOMIC_SEQ_CST);
    }
}

/* Render thread: every 200 ms of a roll, put its timer back on the grid, so a
   long roll does not drift from the beat (the BPM reading is rounded). */
static void roll_resync(void)
{
    unsigned int division = __atomic_load_n(&roll.line, __ATOMIC_SEQ_CST);
    if (!division || !__atomic_load_n(&roll.active, __ATOMIC_SEQ_CST)) return;
    struct { long s, us; } now = {0, 0};
    gettimeofday((void *)&now, 0);
    /* Unsigned: rbp exports no signed division helper. */
    unsigned int ms = (unsigned int)((unsigned long)now.s * 1000u + (unsigned long)now.us / 1000u);
    if (ms - roll.resync_ms < 200u) return;
    roll.resync_ms = ms;
    roll_retime();
    roll_grid_refresh();
    unsigned int wait = roll_grid_wait(division);
    if (wait) __atomic_store_n(&roll.resync, wait + 1u, __ATOMIC_SEQ_CST);
}

/* The roll volume is AUX LEVEL, but only with the AUX selector on PORTABLE:
   on LINE the knob stays the external input's (RX3, 2026-10-15). MIC 2 LEVEL
   never reached the software, so it cannot be used. The knob keeps its own
   job either way. The selector reports 0, 1 or 2; which one is PORTABLE is
   read from the log line below and set with portable-raw= in roll.ini. */
#define ROLL_AUX_SWITCH 0x4409u
#define ROLL_AUX_LEVEL 0x440au
static int roll_knob(const struct rx3_key_event *event)
{
    if (event->key == ROLL_AUX_SWITCH) {
        if (!roll.switch_known || roll.switch_raw != event->raw) {
            roll.switch_raw = event->raw;
            roll.switch_known = 1u;
            log_number("AUX selector raw = ", event->raw);
            log_number(event->raw == roll.portable_raw ? "  PORTABLE: AUX LEVEL is the roll volume, value "
                                                        : "  not PORTABLE: AUX LEVEL left alone, PORTABLE is ",
                       roll.portable_raw);
        }
        return 0;
    }
    if (event->operation != 5u || (event->key & 0xff00u) != 0x4400u) return 0;
    /* The mixer reports every knob at start, so log real moves: a change of
       a quarter turn or more, a limited number of lines. */
    static uint16_t last[256];
    static uint8_t known[256];
    static unsigned int lines;
    unsigned int index = event->key & 0xffu, raw = event->raw > 1023u ? 1023u : event->raw;
    if (!known[index]) { known[index] = 1u; last[index] = (uint16_t)raw; }
    else if (lines < 80u && (raw > last[index] + 255u || last[index] > raw + 255u)) {
        lines++;
        last[index] = (uint16_t)raw;
        log_number("mixer knob turned, code = ", event->key);
        log_number("  to raw = ", raw);
    }
    if (roll.volume_key && event->key == roll.volume_key &&
        (roll.volume_key != ROLL_AUX_LEVEL ||
         (roll.switch_known && roll.switch_raw == roll.portable_raw)))
        __atomic_store_n(&roll.volume, event->raw > 1023u ? 100u : event->raw * 100u / 1023u,
                         __ATOMIC_SEQ_CST);
    return 0;
}

/* The swipe filter: a resonant state-variable filter (topology-preserving
   transform, two integrators per channel). Down: low pass from 12 kHz to
   150 Hz; up: high pass from 30 Hz to 4 kHz; the resonance (Q 0.7 to ~4.5)
   rises with the swipe, so the cutoff sings as it moves. The 101 settings
   per side are worked out once at install: no libm on the player. */
struct roll_svf { float a1, a2, a3, k, gain; };
static struct roll_svf roll_svf_low[101], roll_svf_high[101];

/* tan(x) for 0 <= x <= 1.4 from sine and cosine series. */
static float roll_tan(float x)
{
    float x2 = x * x, sine = x, cosine = 1.0f, term_s = x, term_c = 1.0f;
    for (int n = 1; n <= 9; n++) {
        term_s *= -x2 / (float)((2 * n) * (2 * n + 1));
        term_c *= -x2 / (float)((2 * n - 1) * (2 * n));
        sine += term_s;
        cosine += term_c;
    }
    return sine / cosine;
}

static void roll_svf_table(struct roll_svf *table, float from_hz, float ratio_per_step)
{
    float hz = from_hz;
    for (unsigned int i = 0; i <= 100u; i++, hz *= ratio_per_step) {
        float amount = (float)i * 0.01f;
        float q = 0.707f + 3.8f * amount * amount;
        float g = roll_tan(3.14159265f * hz / 44100.0f);
        float k = 1.0f / q;
        table[i].k = k;
        table[i].a1 = 1.0f / (1.0f + g * (g + k));
        table[i].a2 = g * table[i].a1;
        table[i].a3 = g * table[i].a2;
        /* Keep the resonant peak from jumping out of the mix. */
        table[i].gain = 1.0f / (1.0f + 0.18f * (q - 0.707f));
    }
}

static void roll_svf_install(void)
{
    /* Per step of 1%: 12000 -> 150 Hz is x0.957126, 30 -> 4000 Hz x1.050145. */
    roll_svf_table(roll_svf_low, 12000.0f, 0.957126f);
    roll_svf_table(roll_svf_high, 30.0f, 1.050145f);
}

/* Audio thread: the frames of this block where a grid line of the MASTER
   deck passes, at most `capacity` of them, or -1 when the roll cannot follow
   the grid now (no grid, a stopped deck) and the timer keeps time. */
/* Reader frames the deck's read position may sit from the running cursor
   before it counts as a jump: a few of the deck's read blocks. */
#define ROLL_CURSOR_SLACK 4096.0
#define ROLL_CURSOR_PULL 0.02
/* How far the end and the start of a loop's second pass may be from the
   first's (reader frames) and still be the same loop. */
#define ROLL_LOOP_TOLERANCE 1536
/* Reader frames looked back for a line the last grid reading moved behind
   the block: more than the whole-millisecond rounding (22 frames). */
#define ROLL_LOOKBACK 256.0f
static int roll_grid_hits(unsigned int frames, unsigned int division, unsigned int *hits,
                          unsigned int capacity)
{
    unsigned int seq = __atomic_load_n(&roll.grid_seq, __ATOMIC_SEQ_CST);
    if (!seq || (seq & 1u) || !framework->audio || !framework->audio->playhead) return -1;
    int anchor = roll.grid_anchor;
    unsigned int beat = roll.grid_beat, deck = roll.grid_deck;
    float rate = roll.grid_rate;
    if (__atomic_load_n(&roll.grid_seq, __ATOMIC_SEQ_CST) != seq) return -1;
    struct rx3_playhead head;
    if (!framework->audio->playhead(deck, &head)) return -1;
    /* A deck that stopped moving: its stream is asked for the same block. */
    int fresh = head.serial != roll.seen_serial;
    if (fresh) { roll.seen_serial = head.serial; roll.stale_blocks = 0u; }
    else if (++roll.stale_blocks > 8u) { roll.cursor_valid = 0u; roll.loop_len = roll.loop_shift = 0.0; roll.loop_confirmed = 0u; return -1; }
    if (!(rate > 0.05f) || !beat || !division) return -1;
    float step = (float)beat / (float)division;
    /* Where the deck is at this block's first frame: the running cursor, put
       back on the deck's read position only after a real jump (cue, loop,
       beat jump), in reader frames from the anchor beat, moved on by the
       early-hit setting. */
    double gap = (double)head.position - roll.cursor;
    int jumped = 0;
    if (!roll.cursor_valid || gap > ROLL_CURSOR_SLACK) {
        roll.cursor = (double)head.position;
        roll.cursor_valid = 1u;
        roll.loop_len = roll.loop_shift = 0.0;
        roll.loop_confirmed = 0u;
        jumped = 1;
    } else if (gap < -ROLL_CURSOR_SLACK) {
        double end = roll.cursor, begin = (double)head.position;
        double near = (double)ROLL_LOOP_TOLERANCE;
        if (roll.loop_len > 0.0 && end - roll.loop_end < near && roll.loop_end - end < near &&
            begin - roll.loop_start < near && roll.loop_start - begin < near) {
            /* The same loop again: the cursor goes round it, and the roll's
               lines (cursor + loop_shift) do not notice the jump. */
            roll.cursor -= roll.loop_len;
            roll.loop_shift += roll.loop_len;
            roll.loop_confirmed = 1u;
            /* Only the part of the shift under one beat matters to the lines. */
            double whole = (double)beat * (double)(long)(roll.loop_shift / (double)beat);
            roll.loop_shift -= whole;
            roll.last_hit -= (int)whole;
        } else {
            /* A cue, a beat jump, or the first pass of a loop: the grid. */
            roll.loop_len = end - begin;
            roll.loop_end = end;
            roll.loop_start = begin;
            roll.loop_shift = 0.0;
            roll.loop_confirmed = 0u;
            roll.cursor = begin;
            jumped = 1;
        }
    } else {
        if (roll.loop_confirmed && roll.cursor - roll.loop_end > ROLL_CURSOR_SLACK) {
            /* Past the loop's end with no jump back: the loop is off, the
               roll goes back to the grid of the track. */
            roll.loop_len = roll.loop_shift = 0.0;
            roll.loop_confirmed = 0u;
            jumped = 1;
        }
        if (fresh) {
            /* A tempo reading a little off would walk the cursor away: lean it
               towards each new read position, too slowly to hear. */
            roll.cursor += gap * ROLL_CURSOR_PULL;
        }
    }
    double here = roll.cursor;
    roll.cursor += (double)frames * (double)rate;
    float start = (float)(here + roll.loop_shift - (double)anchor) + (float)roll.advance * rate;
    float end = start + (float)frames * rate;
    /* The grid is read again every 200 ms from whole milliseconds, so it can
       step back a few frames: a line just ahead would then sit behind this
       block and never play (RX3, 17/10: 1/4 skipped now and then). While
       locked, look a little back and play such a line at once. */
    float from = roll.locked ? start - ROLL_LOOKBACK * rate : start;
    /* The roll's first block on the grid, or the first after the deck jumped (a
       loop closing, a cue, a beat jump): lines before it are not owed. Keeping the
       old last_hit across a jump made the first line after a short loop's wrap look
       like a repeat of the one just played, so a loop of up to two roll steps
       (a 1-beat loop under a 1/1 or 1/2 roll) lost a hit at every pass (RX3, 23/10). */
    if (!roll.locked || jumped) roll.last_hit = anchor + (int)(start - step * 0.5f);
    float ratio = from / step;
    int k = (int)ratio;
    if ((float)k < ratio) k++;           /* the first line at or after `from` */
    unsigned int count = 0u;
    for (float line = (float)k * step; line < end && count < capacity; line += step) {
        int at = anchor + (int)line;
        /* A line already played (overlap, or the look back): one hit. Far
           behind the last hit is a jump back (cue, loop): play it. */
        if (roll.locked && at - roll.last_hit < (int)(step * 0.5f) &&
            roll.last_hit - at < (int)(step * 2.0f))
            continue;
        roll.last_hit = at;
        unsigned int frame = line > start ? (unsigned int)((line - start) / rate) : 0u;
        hits[count++] = frame < frames ? frame : frames - 1u;
    }
    return (int)count;
}

/* Master bus: the chosen sound restarts on every hit while the roll is on;
   a new hit chokes the one ringing, as a roll does. On the grid when the
   MASTER deck plays one, else on a timer. */
static void roll_mix(struct rx3_stereo *output, unsigned int frames)
{
    unsigned int active = __atomic_load_n(&roll.active, __ATOMIC_SEQ_CST);
    if (!__atomic_load_n(&roll.frames, __ATOMIC_SEQ_CST) || !output) return;
    unsigned int interval = __atomic_load_n(&roll.interval, __ATOMIC_SEQ_CST);
    unsigned int resync = __atomic_exchange_n(&roll.resync, 0u, __ATOMIC_SEQ_CST);
    if (active == 2u) {
        /* The first hit lands `wait` frames from now on the timer, or on the
           next grid line when the deck is followed. */
        unsigned int wait = __atomic_load_n(&roll.wait, __ATOMIC_SEQ_CST);
        unsigned int beat = __atomic_load_n(&roll.beat, __ATOMIC_SEQ_CST);
        /* The first hit starts a beat. */
        roll.phase = wait && beat && wait < beat ? beat - wait : 0u;
        roll.next = roll.phase ? beat : 0u;
        roll.timed = 0u;
        roll.hit = 0xffffffffu;
        roll.locked = 0u;
        roll.cursor_valid = 0u;
        roll.loop_len = roll.loop_shift = 0.0;
        roll.loop_confirmed = 0u;
        roll.last_hit = -0x40000000;
        __atomic_store_n(&roll.active, 1u, __ATOMIC_SEQ_CST);
    } else if (!active) {
        roll.hit = 0xffffffffu; /* let nothing ring on after the release */
        roll.locked = 0u;
        return;
    }
    unsigned int line = __atomic_load_n(&roll.line, __ATOMIC_SEQ_CST);
    unsigned int division = line ? line : roll.divisions[roll.division % roll.division_count];
    unsigned int hits[8];
    int grid = roll_grid_hits(frames, division, hits, 8u);
    if (grid >= 0) {
        roll.locked = 1u;
    } else {
        unsigned int beat = __atomic_load_n(&roll.beat, __ATOMIC_SEQ_CST);
        if (!beat) beat = ROLL_DEFAULT_BEAT_FRAMES;
        if (roll.phase >= beat) {
            /* A block that ended on a beat, or a slower reading: from its start. */
            roll.phase %= beat;
            roll.next = roll.phase ? roll_timer_next(roll.phase, beat, division) : 0u;
        }
        if (roll.locked) {
            /* Leaving the grid: the timer goes on from the last hit. */
            roll.locked = 0u;
            roll.phase = 0u;
            roll.next = roll_timer_next(0u, beat, division);
            roll.timed = division;
        }
        if (division != roll.timed) {
            /* Another square: its next line in the same beat. */
            if (roll.timed) roll.next = roll_timer_next(roll.phase, beat, division);
            roll.timed = division;
        }
        if (resync) {
            unsigned int wait = resync - 1u;
            /* The next line is `wait` away. Within a frame block of a hit
               already: leave it. */
            if (wait < interval && wait > 64u && interval - wait > 64u && roll.next >= wait)
                roll.phase = roll.next - wait;
        }
        interval = beat;                 /* the loop below wraps the beat */
    }
    unsigned int selected = __atomic_load_n(&roll.sound, __ATOMIC_SEQ_CST);
    if (selected >= ROLL_SOUNDS || !roll.sounds[selected].frames) selected = ROLL_SNARE;
    unsigned int volume = __atomic_load_n(&roll.volume, __ATOMIC_SEQ_CST);
    float level = (float)volume * 0.01f;
    level = level * level * (1.0f / 32768.0f);
    /* Swipe down: low pass; up: high pass; both resonant (roll_svf_*). */
    int filter = __atomic_load_n(&roll.filter, __ATOMIC_SEQ_CST);
    unsigned int amount = (unsigned int)(filter < 0 ? -filter : filter);
    if (amount > 100u) amount = 100u;
    const struct roll_svf *svf = filter > 0 ? &roll_svf_low[amount] : &roll_svf_high[amount];
    int filtering = amount != 0u;
    unsigned int next = 0u;
    for (unsigned int i = 0; i < frames; i++) {
        int start = 0;
        if (grid >= 0) {
            while (next < (unsigned int)grid && hits[next] <= i) { start = hits[next] == i; next++; }
        } else {
            if (roll.phase >= interval) { roll.phase -= interval; roll.next = 0u; }
            if (roll.phase >= roll.next) {
                roll.next = roll_timer_next(roll.phase, interval, division);
                start = 1;
            }
            roll.phase++;
        }
        if (start) { roll.hit = 0u; roll.playing = selected; }
        float left = 0.0f, right = 0.0f;
        const struct roll_sound *sound = &roll.sounds[roll.playing < ROLL_SOUNDS ? roll.playing : ROLL_SNARE];
        if (sound->frames && roll.hit < sound->length) {
            left = (float)sound->frames[roll.hit * 2u] * level;
            right = (float)sound->frames[roll.hit * 2u + 1u] * level;
            roll.hit++;
        }
        if (filtering) {
            float in[2] = {left, right}, out[2];
            for (unsigned int c = 0; c < 2u; c++) {
                float *state = roll.svf[c];
                float v3 = in[c] - state[1];
                float v1 = svf->a1 * state[0] + svf->a2 * v3;
                float v2 = state[1] + svf->a2 * state[0] + svf->a3 * v3;
                state[0] = 2.0f * v1 - state[0];
                state[1] = 2.0f * v2 - state[1];
                out[c] = (filter > 0 ? v2 : in[c] - svf->k * v1 - v2) * svf->gain;
            }
            left = out[0];
            right = out[1];
        } else {
            roll.svf[0][0] = roll.svf[0][1] = roll.svf[1][0] = roll.svf[1][1] = 0.0f;
        }
        output[i].left += left;
        output[i].right += right;
    }
}

/* The RMX-1000 Roll module (rmx1000-roll, RX3_RMX_ROLL=1) takes the place of this Snare Roll: its ROLL line on
   the STATUS tab, its touch, its logic and its volume knob are all off, and the STATUS tab stays native. It
   still reads its drum files from RX3_RUNTIME/roll (RX3_ROLL_FILE), which is the module's concern, not ours. */
static int roll_superseded(void)
{
    const char *rmx = getenv("RX3_RMX_ROLL");
    return rmx && rmx[0] == '1' && rmx[1] == '\0';
}

static int roll_configured(void)
{
    const char *file = getenv("RX3_ROLL_FILE");
    return file && file[0] != '\0' && !roll_superseded();
}

/* Loader thread: one sound into reserved, read-only memory. */
static int roll_load_sound(const char *path, struct roll_sound *sound)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    off_t length = lseek(fd, 0, SEEK_END);
    if (length < 12 || (unsigned long)length > SAMPLES_BANK_MAX_BYTES) { close(fd); return 0; }
    void *wave = mmap(0, (size_t)length, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (wave == MAP_FAILED) return 0;
    unsigned int bytes;
    const uint8_t *audio = samples_wave_data(wave, (size_t)length, &bytes);
    size_t allocation = (bytes + 4095u) & ~4095u;
    void *block = MAP_FAILED;
    if (audio && framework->memory->reserve(sound, allocation, 0u)) {
        block = mmap(0, allocation, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (block == MAP_FAILED) framework->memory->abandon(sound, allocation);
        else { memcpy(block, audio, bytes); framework->memory->settle(sound, allocation); }
    }
    munmap(wave, (size_t)length);
    if (block == MAP_FAILED) return 0;
    sound->length = bytes / SAMPLES_FRAME_BYTES;
    sound->block = block;
    sound->block_size = allocation;
    __atomic_store_n(&sound->frames, (const int16_t *)block, __ATOMIC_SEQ_CST);
    return 1;
}

static size_t roll_length(const char *text)
{
    size_t n = 0u;
    while (text && text[n]) n++;
    return n;
}

/* Loader thread: the kit, from the directory RX3_ROLL_FILE (snare.wav) is in. */
static void roll_load(void)
{
    const char *path = getenv("RX3_ROLL_FILE");
    size_t length = roll_length(path), cut = length;
    while (cut && path[cut - 1u] != '/') cut--;
    char name[512];
    if (!cut || cut + 16u > sizeof(name)) { log_line("snare roll: snare.wav missing"); return; }
    memcpy(name, path, cut);
    unsigned int loaded = 0u;
    for (unsigned int i = 0; i < ROLL_SOUNDS; i++) {
        size_t n = roll_length(roll_sound_files[i]);
        memcpy(name + cut, roll_sound_files[i], n + 1u);
        if (roll_load_sound(name, &roll.sounds[i])) loaded |= 1u << i;
    }
    if (!(loaded & (1u << ROLL_SNARE))) {
        log_line("snare roll: snare.wav rejected or missing (44.1 kHz PCM16 stereo)");
        return;
    }
    log_number("roll: sounds loaded (1 kick, 2 snare, 4 clap, 8 hat) = ", loaded);
    __atomic_store_n(&roll.frames, roll.sounds[ROLL_SNARE].frames, __ATOMIC_SEQ_CST);
}

/* A number in `base` (10 or 16), at most four digits; else `fallback`. */
static unsigned int roll_parse(const char *text, unsigned int base, unsigned int fallback)
{
    unsigned int value = 0u, digits = 0u;
    for (; text && *text; text++, digits++) {
        unsigned int d = *text >= '0' && *text <= '9' ? (unsigned int)(*text - '0') :
                         (*text | 32) >= 'a' && (*text | 32) <= 'f' ? (unsigned int)((*text | 32) - 'a' + 10) : 99u;
        if (d >= base || digits >= 4u) return fallback;
        value = value * base + d;
    }
    return digits ? value : fallback;
}

/* Startup thread, before any key can reach roll_key. */
static int roll_install(void)
{
    if (!roll_configured()) return 1;
    roll.division_count = roll_parse_divisions(getenv("RX3_ROLL_DIVISIONS"), roll.divisions);
    if (!roll.division_count) {
        static const unsigned int all[ROLL_DIVISION_MAX] = {1u, 2u, 4u, 8u, 16u};
        for (unsigned int i = 0; i < ROLL_DIVISION_MAX; i++) roll.divisions[i] = all[i];
        roll.division_count = ROLL_DIVISION_MAX;
    }
    roll.division = roll.division_count > 2u ? 2u % roll.division_count : 0u;
    roll.volume = 70u;
    roll.active = 0u;
    roll.bpm = 0;
#if defined(__arm__)
    if (!memcmp((const void *)ROLL_PLAY_BPM, roll_play_bpm_guard, 8))
        roll.bpm = (roll_bpm_fn)ROLL_PLAY_BPM;
    else
        log_line("snare roll: UiGetPlayBpm prologue differs, roll at 120 BPM");
    roll.grid = !memcmp((const void *)ROLL_GET_ENGINE, roll_engine_guard, 8) &&
                !memcmp((const void *)ROLL_BEAT_INFO, roll_info_guard, 8) &&
                !memcmp((const void *)ROLL_POSITION, roll_position_guard, 8) &&
                !memcmp((const void *)ROLL_TEMPO, roll_tempo_guard, 8);
    if (!roll.grid) log_line("snare roll: beat grid functions differ, roll starts on the touch");
#endif
    roll_retime();
    roll.volume_key = roll_parse(getenv("RX3_ROLL_VOLUME_KEY"), 16u, ROLL_AUX_LEVEL);
    roll.portable_raw = roll_parse(getenv("RX3_ROLL_PORTABLE_RAW"), 10u, 2u);
    roll.advance = roll_parse(getenv("RX3_ROLL_ADVANCE_MS"), 10u, 0u) * 441u / 10u;
    log_number("snare roll: hits early by ms = ", roll.advance * 10u / 441u);  /* OFF 0, LINE 1, PORTABLE 2 */
    roll.keys = framework->input->register_key(&roll, 5u, roll_knob) != 0;
    roll_svf_install();
    roll.sound = ROLL_SNARE;
    roll.playing = ROLL_SNARE;
    roll.playhead = framework->audio && framework->audio->use_playhead && framework->audio->use_playhead();
    if (!roll.playhead) log_line("roll: deck stream unavailable, hits on the BPM timer");
    log_line("snare roll on the ROLL squares (STATUS tab), on the beat grid");
    return 1;
}

static void roll_remove(void)
{
    if (roll.keys) framework->input->unregister_owner(&roll);
    roll.keys = 0u;
    __atomic_store_n(&roll.active, 0u, __ATOMIC_SEQ_CST);
    if (roll.playhead) framework->audio->drop_playhead();
    roll.playhead = 0u;
    __atomic_store_n(&roll.frames, (const int16_t *)0, __ATOMIC_SEQ_CST);
    for (unsigned int i = 0; i < ROLL_SOUNDS; i++) {
        struct roll_sound *sound = &roll.sounds[i];
        __atomic_store_n(&sound->frames, (const int16_t *)0, __ATOMIC_SEQ_CST);
        if (sound->block) {
            munmap(sound->block, sound->block_size);
            framework->memory->release(sound, sound->block_size);
            sound->block = 0;
        }
    }
}
#endif /* RX3_SAMPLES_ROLL_H */

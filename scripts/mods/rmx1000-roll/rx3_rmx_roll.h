/* SPDX-License-Identifier: MPL-2.0 */
/* A Roll in the style of the RMX-1000 X-PAD ROLL, as a plain DSP kernel: it
 * records the signal it is given into a ring and, while the roll is held,
 * plays one length of it over and over. Nothing here knows about the beat
 * grid: the loop starts the instant the roll is pressed, and its length comes
 * from a tempo and a note value only. No allocation, no I/O, no libm, no
 * integer division. The caller owns the ring and the state.
 *
 * Original work. The idea (record from the touch, repeat at a note value of
 * the tempo) is in the RMX-1000's public manual; the code is ours.
 *
 * Two ways to take the loop, `capture`:
 *   after  (the RMX-1000's way) the loop is the `length` frames that follow
 *          the press. They play live as they are recorded, so the press
 *          sounds like nothing happened for one length; then the same frames
 *          come round again, and again, until the release.
 *   before the loop is the `length` frames that came just before the press,
 *          and it repeats at once (a stutter of what was just heard), with a
 *          short fade-in from the live sound.
 *
 * One mixer does it all: out = live + m * (gain * loop - live), where m is the
 * roll's engagement: it rises over `in_frames` at the press and falls over
 * `out_frames` at the release, so neither the entry nor the exit clicks. The
 * live signal keeps running underneath, so leaving the roll comes back to the
 * music as if it had never been interrupted (a slip). At the seam of the loop
 * the last RMXR_SEAM frames fade into the audio that came just before the
 * loop's first frame, which is what the first frame follows on from, so the
 * wrap is continuous too.
 */
#ifndef RX3_RMX_ROLL_H
#define RX3_RMX_ROLL_H

#define RMXR_RING_FRAMES 262144u        /* power of two: 5.94 s at 44.1 kHz */
#define RMXR_RING_MASK (RMXR_RING_FRAMES - 1u)
#define RMXR_SEAM 64u                   /* frames of seam fade at the wrap */
#define RMXR_MAX_LOOP (RMXR_RING_FRAMES - 2u * RMXR_SEAM)
#define RMXR_MIN_LOOP 16u
#define RMXR_LENGTHS 10u

/* Note values in sixteenths of a beat: 1/16 1/8 1/4 1/2 3/4 1 2 4 8 16 beats. */
static const unsigned int rmxr_sixteenths[RMXR_LENGTHS] = {1u, 2u, 4u, 8u, 12u, 16u, 32u, 64u, 128u, 256u};

struct rmxr_params {
    unsigned int armed;          /* record into the ring (needed for `before`, and for `after` once pressed) */
    unsigned int hold;           /* the roll is held now */
    unsigned int press_seq;      /* grows with each press, so a tap shorter than a block is not lost */
    unsigned int loop_frames;    /* length of the loop for the next press, 0 = none */
    unsigned int capture_before; /* 0 after, 1 before */
    unsigned int in_frames;      /* engagement ramp up, frames (>= 1) */
    unsigned int out_frames;     /* engagement ramp down, frames (>= 1) */
    float gain;                  /* roll level, 0 .. 1 */
};

struct rmxr_state {
    unsigned int write;          /* next ring frame to be written */
    unsigned int seen_press;     /* the last press_seq taken */
    unsigned int running;        /* a loop exists (playing or fading out) */
    unsigned int start;          /* ring frame of the loop's first frame */
    unsigned int length;         /* frames in this loop */
    unsigned int pos;            /* frame within the loop */
    unsigned int hold_left;      /* a press counts as held for at least the ramp-up, so a tap is heard */
    float engage;                /* m, 0 .. 1 */
    float level;                 /* the gain, smoothed */
};

/* Frames in `sixteenths`/16 of a beat at bpm100 / 100 beats per minute, at the
   sample rate, rounded to the nearest frame, and halved while it does not fit
   the ring. 0 for a tempo or rate that makes no sense. */
static inline unsigned int rmxr_loop_frames(unsigned int bpm100, unsigned int sixteenths,
                                            unsigned int rate)
{
    if (bpm100 < 2000u || bpm100 > 40000u || !sixteenths || rate < 8000u || rate > 192000u) return 0u;
    double frames = (double)rate * 6000.0 * (double)sixteenths / ((double)bpm100 * 16.0);
    while (frames > (double)RMXR_MAX_LOOP) frames *= 0.5;
    unsigned int whole = (unsigned int)(frames + 0.5);
    return whole < RMXR_MIN_LOOP ? RMXR_MIN_LOOP : whole;
}

static inline void rmxr_reset(struct rmxr_state *s)
{
    s->write = 0u; s->seen_press = 0u; s->running = 0u; s->start = 0u; s->length = 0u;
    s->pos = 0u; s->hold_left = 0u; s->engage = 0.0f; s->level = 1.0f;
}

/* Process `frames` interleaved stereo frames in place. `ring` holds
   RMXR_RING_FRAMES * 2 floats. */
static inline void rmxr_process(struct rmxr_state *s, float *ring, float *io, unsigned int frames,
                                const struct rmxr_params *p)
{
    const float up = 1.0f / (float)(p->in_frames ? p->in_frames : 1u);
    const float down = 1.0f / (float)(p->out_frames ? p->out_frames : 1u);
    const float glide = 0.002f;
    int pressed = p->press_seq != s->seen_press;
    for (unsigned int i = 0; i < frames; i++) {
        float live_l = io[i * 2u], live_r = io[i * 2u + 1u];
        if (p->armed || s->running || pressed) {
            ring[(s->write & RMXR_RING_MASK) * 2u] = live_l;
            ring[(s->write & RMXR_RING_MASK) * 2u + 1u] = live_r;
        }
        if (pressed) {
            /* A press takes effect on the frame it is seen at. */
            pressed = 0;
            s->seen_press = p->press_seq;
            if (p->loop_frames >= RMXR_MIN_LOOP && p->loop_frames <= RMXR_MAX_LOOP) {
                s->length = p->loop_frames;
                s->start = p->capture_before ? s->write - s->length : s->write;
                s->pos = 0u;
                s->hold_left = p->in_frames ? p->in_frames : 1u;
                if (!s->running) s->level = p->gain;      /* no glide from a stale level */
                s->running = 1u;
            }
        }
        float out_l = live_l, out_r = live_r;
        if (s->running) {
            s->level += (p->gain - s->level) * glide;
            int held = p->hold || s->hold_left;
            if (s->hold_left) s->hold_left--;
            if (held) { s->engage += up; if (s->engage > 1.0f) s->engage = 1.0f; }
            else { s->engage -= down; if (s->engage < 0.0f) s->engage = 0.0f; }
            unsigned int at = (s->start + s->pos) & RMXR_RING_MASK;
            float loop_l = ring[at * 2u], loop_r = ring[at * 2u + 1u];
            const unsigned int seam = s->length / 2u < RMXR_SEAM ? s->length / 2u : RMXR_SEAM;
            if (s->pos + seam >= s->length) {
                /* The seam: fade toward the audio before the loop's first
                   frame, at the same offset, so the wrap runs on. */
                unsigned int back = (at - s->length) & RMXR_RING_MASK;
                float w = (float)(s->pos + seam + 1u - s->length) * (1.0f / (float)(seam + 1u));
                loop_l += (ring[back * 2u] - loop_l) * w;
                loop_r += (ring[back * 2u + 1u] - loop_r) * w;
            }
            float m = s->engage;
            out_l = live_l + m * (s->level * loop_l - live_l);
            out_r = live_r + m * (s->level * loop_r - live_r);
            if (++s->pos >= s->length) s->pos = 0u;
            if (!held && s->engage <= 0.0f) s->running = 0u;
        }
        s->write++;
        io[i * 2u] = out_l;
        io[i * 2u + 1u] = out_r;
    }
}

/* Safety: a soft limiter for the module's output, and a ceiling for the roll's own level.
   Below RMXR_KNEE the signal is untouched; above it the excess is bent smoothly toward
   RMXR_CEIL (a rational curve, no libm), so nothing the roll or the layers add can leave
   +-RMXR_CEIL. NaN is taken to silence. */
#define RMXR_KNEE 0.90f
#define RMXR_CEIL 1.0f      /* 0 dBFS: transparent below the knee, never above full scale */

static inline float rmxr_soft(float x)
{
    if (!(x == x)) return 0.0f;
    float a = x < 0.0f ? -x : x;
    if (a <= RMXR_KNEE) return x;
    float over = (a - RMXR_KNEE) * (1.0f / (RMXR_CEIL - RMXR_KNEE));
    if (over > 1.0e6f) over = 1.0e6f;
    float y = RMXR_KNEE + (RMXR_CEIL - RMXR_KNEE) * over / (1.0f + over);
    return x < 0.0f ? -y : y;
}

static inline void rmxr_limit(float *io, unsigned int frames)
{
    for (unsigned int i = 0; i < frames * 2u; i++) io[i] = rmxr_soft(io[i]);
}

#endif

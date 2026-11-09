/* SPDX-License-Identifier: MPL-2.0 */
/* The three RELEASE FX of the RMX-1000 on the master bus: VINYL BRAKE, ECHO
 * (echo-out) and BACK SPIN. Copy of rmx1000-roll/rx3_rmx_release.h (a module cannot include another
 * module's header), renamed smplr_ so the two never share a symbol, and with the constants rescaled
 * for the master bus of the player: float, 1.0 = full scale (the original is PCM16 scale).
 *
 * Pure code: no allocation, no I/O, no libc, no libm, no threads. The caller
 * owns the state (about 1 MiB, static) and calls smplr_process on the audio
 * thread for every block of the master (interleaved stereo float, 1.0 = full scale,
 * 44.1 kHz). The core keeps a ring of the last ~3 s of the master; while an
 * effect is on it reads from that ring instead of the live audio. Original work.
 *
 * Idle (smplr_active == 0): the block is copied into the ring and left untouched,
 * bit for bit. smplr_reset clears the ring (1 MiB of stores): not on the audio
 * path.
 *
 * BRAKE  the sound goes on but slows down like a platter stopping: the read
 *        speed falls from 1 to 0 in 1.2 s along a smooth curve (the pitch drops),
 *        then silence until the release.
 * SPIN   the ring is read backwards from the live point, fast (3x) and slowing
 *        down to 0 in 0.9 s (about the last second of sound), then silence.
 * ECHO   the live sound is taken away and only its tail is left: the last
 *        `delay` frames are repeated every `delay` frames, each time at 0.55
 *        of the level before, until the release.
 *
 * Entry and exit are 20 ms crossfades between the live sound and the effect, so
 * there is no click. smplr_begin is ignored unless the unit is idle (end, wait
 * ~20 ms, begin again). The delay is read when the effect begins.
 * The output of an active effect is always finite and within +-SMPLR_LIMIT (4.0: well above a hot master, so a normal signal is never touched).
 */
#ifndef RX3_SAMPLES_RELEASE_H
#define RX3_SAMPLES_RELEASE_H

#define SMPLR_RING_BITS 17u
#define SMPLR_RING (1u << SMPLR_RING_BITS)             /* frames of the master kept: 131072 = 2.97 s */
#define SMPLR_MASK (SMPLR_RING - 1u)
#define SMPLR_XFADE 882u                              /* frames of the entry and exit crossfades: 20 ms */
#define SMPLR_BRAKE_FRAMES 52920u                     /* 1.2 s */
#define SMPLR_SPIN_FRAMES 39690u                      /* 0.9 s */
#define SMPLR_SPIN_SPEED 3.0f                         /* back-spin speed at the start (x live) */
#define SMPLR_DELAY_DEFAULT 16537u                    /* 0.375 s */
#define SMPLR_DELAY_MIN 32u
#define SMPLR_DELAY_MAX (SMPLR_RING - 8u)
#define SMPLR_FEEDBACK 0.55f
#define SMPLR_LIMIT 4.0f                              /* +12 dBFS: only a guard against runaway values */
#define SMPLR_DENORMAL 1.0e-9f                        /* the echo tail below this is cut to 0 */

enum smplr_type { SMPLR_BRAKE = 0, SMPLR_ECHO, SMPLR_SPIN };
enum smplr_state { SMPLR_IDLE = 0, SMPLR_ON, SMPLR_RELEASING };

struct smplr {
    float ring[SMPLR_RING * 2];            /* interleaved stereo */
    unsigned int wr;                      /* index (mod SMPLR_RING) of the next frame to write */
    unsigned int state, type;
    unsigned int t;                       /* frames since the effect began (stops at the end of the curve) */
    unsigned int delay, d;                /* echo delay asked for / in use, frames */
    unsigned int xf;                      /* crossfade position 0 (live) .. SMPLR_XFADE (effect), counted so that both ends are exact */
    double lag;                           /* how far behind the newest frame the read point is (brake, spin) */
};

static inline float smplr_clean(float x)
{
    if (!(x == x)) return 0.0f;           /* NaN */
    if (x > SMPLR_LIMIT) return SMPLR_LIMIT;
    if (x < -SMPLR_LIMIT) return -SMPLR_LIMIT;
    return x;
}

static inline void smplr_reset(struct smplr *s)
{
    for (unsigned int i = 0; i < SMPLR_RING * 2u; i++) s->ring[i] = 0.0f;
    s->wr = 0; s->state = SMPLR_IDLE; s->type = SMPLR_BRAKE; s->t = 0;
    s->delay = SMPLR_DELAY_DEFAULT; s->d = SMPLR_DELAY_DEFAULT; s->xf = 0; s->lag = 0.0;
}

/* The echo delay in frames (clamped); read when the next effect begins. */
static inline void smplr_set_delay(struct smplr *s, unsigned int frames)
{
    if (frames < SMPLR_DELAY_MIN) frames = SMPLR_DELAY_MIN;
    if (frames > SMPLR_DELAY_MAX) frames = SMPLR_DELAY_MAX;
    s->delay = frames;
}

/* Starts an effect. Returns 1, or 0 when the type is unknown or the unit is not idle. */
static inline int smplr_begin(struct smplr *s, unsigned int type)
{
    if (type > SMPLR_SPIN || s->state != SMPLR_IDLE) return 0;
    s->type = type; s->state = SMPLR_ON; s->t = 0; s->xf = 0; s->lag = 0.0; s->d = s->delay;
    return 1;
}

/* The release: back to the live sound over 20 ms. */
static inline void smplr_end(struct smplr *s)
{
    if (s->state == SMPLR_ON) s->state = SMPLR_RELEASING;
}

static inline int smplr_active(const struct smplr *s) { return s->state != SMPLR_IDLE; }

/* The read at `lag` frames behind the newest frame written (linear between two frames). */
static inline void smplr_read(const struct smplr *s, double lag, float *l, float *r)
{
    if (lag < 0.0) lag = 0.0;
    if (lag > (double)(SMPLR_RING - 4u)) lag = (double)(SMPLR_RING - 4u);
    unsigned int whole = (unsigned int)lag;
    float frac = (float)(lag - (double)whole);
    unsigned int newer = (s->wr - 1u - whole) & SMPLR_MASK;
    unsigned int older = (newer - 1u) & SMPLR_MASK;
    *l = s->ring[older * 2u] * frac + s->ring[newer * 2u] * (1.0f - frac);
    *r = s->ring[older * 2u + 1u] * frac + s->ring[newer * 2u + 1u] * (1.0f - frac);
}

/* io: `frames` interleaved stereo frames of the live master. They go into the
   ring; while an effect is on they are replaced by its output. */
static inline void smplr_process(struct smplr *s, float *io, unsigned int frames)
{
    for (unsigned int i = 0; i < frames; i++) {
        float *p = io + (unsigned long)i * 2u;
        if (s->state == SMPLR_IDLE) {
            s->ring[(s->wr & SMPLR_MASK) * 2u] = smplr_clean(p[0]);
            s->ring[(s->wr & SMPLR_MASK) * 2u + 1u] = smplr_clean(p[1]);
            s->wr = (s->wr + 1u) & SMPLR_MASK;
            continue;
        }
        float live_l = smplr_clean(p[0]), live_r = smplr_clean(p[1]);
        float fx_l = 0.0f, fx_r = 0.0f;
        if (s->type == SMPLR_ECHO) {
            unsigned int at = (s->wr - s->d) & SMPLR_MASK;
            fx_l = s->ring[at * 2u]; fx_r = s->ring[at * 2u + 1u];
            float fb_l = fx_l * SMPLR_FEEDBACK, fb_r = fx_r * SMPLR_FEEDBACK;
            if (fb_l < SMPLR_DENORMAL && fb_l > -SMPLR_DENORMAL) fb_l = 0.0f;
            if (fb_r < SMPLR_DENORMAL && fb_r > -SMPLR_DENORMAL) fb_r = 0.0f;
            s->ring[s->wr * 2u] = fb_l;                       /* the live sound is not recorded: only the tail */
            s->ring[s->wr * 2u + 1u] = fb_r;
            s->wr = (s->wr + 1u) & SMPLR_MASK;
        } else {
            s->ring[s->wr * 2u] = live_l; s->ring[s->wr * 2u + 1u] = live_r;
            s->wr = (s->wr + 1u) & SMPLR_MASK;
            unsigned int length = s->type == SMPLR_BRAKE ? SMPLR_BRAKE_FRAMES : SMPLR_SPIN_FRAMES;
            if (s->t < length) {
                float u = (float)s->t / (float)length, speed, shape;
                if (s->type == SMPLR_BRAKE) {
                    shape = 1.0f - u * u * (3.0f - 2.0f * u);           /* smooth 1 -> 0 */
                    speed = shape;
                } else {
                    shape = (1.0f - u) * (1.0f - u);
                    speed = SMPLR_SPIN_SPEED * shape;
                }
                float gain = shape * 5.0f; if (gain > 1.0f) gain = 1.0f;   /* no DC when the speed reaches 0 */
                smplr_read(s, s->lag, &fx_l, &fx_r);
                fx_l *= gain; fx_r *= gain;
                s->lag += s->type == SMPLR_BRAKE ? (double)(1.0f - speed) : (double)(1.0f + speed);
                s->t++;
            }                                                          /* past the end of the curve: silence */
        }
        float w = (float)s->xf / (float)SMPLR_XFADE;
        w = w * w * (3.0f - 2.0f * w);
        p[0] = smplr_clean(live_l * (1.0f - w) + fx_l * w);
        p[1] = smplr_clean(live_r * (1.0f - w) + fx_r * w);
        if (s->state == SMPLR_ON) { if (s->xf < SMPLR_XFADE) s->xf++; }
        else if (s->xf > 0u) s->xf--;
        else s->state = SMPLR_IDLE;
    }
}

#endif

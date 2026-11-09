/* SPDX-License-Identifier: MPL-2.0 */
/* The X-PAD oscillator of the RMX window (OSC on): pure DSP, no allocation, no I/O, no libm, no integer
 * division. One voice, mono, played while a finger is on the dot matrix.
 *
 *   height (y 0..255, 255 = top of the matrix): the pitch, continuous and logarithmic, 110 Hz at the bottom to
 *     1760 Hz at the top (four octaves); the log-pitch glides with a ~15 ms time constant, so a moving finger
 *     bends the note and a jump is a short glissando, never a step.
 *   across (x 0..255): the wave shape. Left = triangle, centre = smoothed square, right = filtered noise,
 *     crossfaded continuously (the weights glide too). The noise is a one-pole low-pass of a xorshift
 *     generator whose cut-off follows the pitch, so the right-hand edge is brighter up high.
 *   envelope: ~5 ms attack, ~40 ms release, shaped by a smoothstep (no corner, no click, exactly 0 at the end).
 *   level: about -12 dBFS at the peak (RMXO_PEAK) times the module's level (the AUX knob), given as the level
 *     at the start and at the end of each block and interpolated between. The caller adds the result to the
 *     bus and runs its limiter.
 * rmxf_exp2 (rx3_rmx_filter.h) must be visible: it is the only function used from outside.
 */
#ifndef RX3_RMX_OSC_H
#define RX3_RMX_OSC_H

#define RMXO_RATE 44100.0f
#define RMXO_F_LOW 110.0f
#define RMXO_OCTAVES 4.0f                   /* 110 Hz .. 1760 Hz */
#define RMXO_PEAK 0.2512f                   /* -12 dBFS */
#define RMXO_ATTACK_STEP (1.0f / 221.0f)    /* 5 ms */
#define RMXO_RELEASE_STEP (1.0f / 1764.0f)  /* 40 ms */
#define RMXO_GLIDE (1.0f / 662.0f)          /* 15 ms time constant on the log pitch */
#define RMXO_MORPH (1.0f / 441.0f)          /* 10 ms on the wave-shape weights */
#define RMXO_STEP 16u                       /* frames per exp2 evaluation */

struct rmxo {
    float phase;                 /* 0 .. 1 */
    float env;                   /* 0 .. 1, the linear ramp; the sound uses its smoothstep */
    float log2f;                 /* smoothed log2(f / 110), 0 .. 4 */
    float w_sq, w_noise;         /* smoothed weights of the square and of the noise (the triangle is the rest) */
    float noise_lp;
    unsigned int rng;
    /* the controls, written by the audio thread from what the lock published */
    unsigned int gate, x, y;
};

static inline void rmxo_reset(struct rmxo *o)
{
    o->phase = 0.25f; o->env = 0.0f; o->log2f = 2.0f; o->w_sq = 0.0f; o->w_noise = 0.0f; o->noise_lp = 0.0f;
    o->rng = 2463534242u; o->gate = 0u; o->x = 0u; o->y = 128u;
}

/* The sound is on, or still dying away: the caller must keep calling rmxo_process. */
static inline int rmxo_active(const struct rmxo *o) { return o->gate || o->env > 0.0f; }

static inline void rmxo_set(struct rmxo *o, unsigned int gate, unsigned int x, unsigned int y)
{
    o->gate = gate ? 1u : 0u;
    o->x = x > 255u ? 255u : x;
    o->y = y > 255u ? 255u : y;
}

static inline float rmxo_clamp1(float v) { return v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v; }

/* Adds `frames` frames of the oscillator to the interleaved stereo `io`. lvl0 and lvl1: the level at the first
   and at the last frame (0 .. 1). */
static inline void rmxo_process(struct rmxo *o, float *io, unsigned int frames, float lvl0, float lvl1)
{
    if (!rmxo_active(o)) return;
    /* A new note begins on a zero of the triangle at the pitch of the finger: no glide in from an old note. */
    const float log_target = (float)o->y * (RMXO_OCTAVES / 255.0f);
    if (o->env <= 0.0f && o->gate) { o->log2f = log_target; o->phase = 0.25f; o->noise_lp = 0.0f; }
    /* Wave shape from x: 0 .. 128 triangle -> square, 128 .. 255 square -> noise. */
    float t_sq, t_noise;
    if (o->x <= 128u) { t_sq = (float)o->x * (1.0f / 128.0f); t_noise = 0.0f; }
    else { t_noise = (float)(o->x - 128u) * (1.0f / 127.0f); t_sq = 1.0f - t_noise; }
    const float slope = frames ? (lvl1 - lvl0) * (1.0f / (float)frames) : 0.0f;
    const float env_step = o->gate ? RMXO_ATTACK_STEP : -RMXO_RELEASE_STEP;
    unsigned int i = 0;
    float inc = 0.0f, lp_a = 0.0f, noise_gain = 0.0f;
    while (i < frames) {
        /* The pitch-dependent constants are re-evaluated every RMXO_STEP frames. */
        float hz = RMXO_F_LOW * (float)rmxf_exp2((double)o->log2f);
        inc = hz * (1.0f / RMXO_RATE);
        lp_a = hz * 0.00057f;                         /* cut-off about four times the pitch */
        if (lp_a > 0.9f) lp_a = 0.9f;
        noise_gain = 3.5f - 3.0f * lp_a;               /* the darker the noise, the more gain */
        unsigned int end = i + RMXO_STEP < frames ? i + RMXO_STEP : frames;
        for (; i < end; i++) {
            o->log2f += (log_target - o->log2f) * RMXO_GLIDE;
            o->w_sq += (t_sq - o->w_sq) * RMXO_MORPH;
            o->w_noise += (t_noise - o->w_noise) * RMXO_MORPH;
            o->env += env_step;
            if (o->env > 1.0f) o->env = 1.0f;
            if (o->env < 0.0f) { o->env = 0.0f; }
            o->phase += inc;
            if (o->phase >= 1.0f) o->phase -= 1.0f;
            float d = o->phase - 0.5f;
            float tri = 1.0f - 4.0f * (d < 0.0f ? -d : d);             /* -1 .. 1 */
            float sq = rmxo_clamp1(3.0f * tri);                         /* a square with sloped edges */
            o->rng ^= o->rng << 13; o->rng ^= o->rng >> 17; o->rng ^= o->rng << 5;
            float white = (float)(o->rng >> 8) * (2.0f / 16777216.0f) - 1.0f;
            o->noise_lp += (white - o->noise_lp) * lp_a;
            float noise = rmxo_clamp1(o->noise_lp * noise_gain);
            float w_tri = 1.0f - o->w_sq - o->w_noise;
            if (w_tri < 0.0f) w_tri = 0.0f;
            float wave = rmxo_clamp1(tri * w_tri + sq * o->w_sq + noise * o->w_noise);
            float e = o->env;
            float shaped = e * e * (3.0f - 2.0f * e);
            float v = wave * shaped * RMXO_PEAK * (lvl0 + slope * (float)i);
            io[i * 2u] += v;
            io[i * 2u + 1u] += v;
            if (o->env <= 0.0f && !o->gate) { i++; break; }          /* the release ended inside the block */
        }
        if (o->env <= 0.0f && !o->gate) break;
    }
}

#endif

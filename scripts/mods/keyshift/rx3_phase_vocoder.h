/* SPDX-License-Identifier: MPL-2.0
 *
 * Frequency-domain key shift for the RX3 performance hook.
 *
 * Design after the approach Geraint Luff describes for Signalsmith Stretch
 * (MIT; ADC talk "Four ways to write a pitch-shifter"). Only the ideas are
 * used; every line here is written for this module:
 *
 *  - STFT with a long block (default 5120 frames, 116 ms at 44.1 kHz) and a
 *    quarter-block interval (1280 frames, 29 ms). Zero-phase Hann analysis
 *    and synthesis windows. Both channels share one complex FFT (L + iR).
 *  - Frequency map: output bin j reads the input at bin j/ratio. Magnitudes
 *    are interpolated linearly between the two neighbouring input bins.
 *    Nothing is resampled and nothing is spliced, so the input and output
 *    clocks are the same and the latency is constant.
 *  - Phase from two predictions: the previous frame (the measured input
 *    frequency, times the ratio) and the neighbouring output bin (the input's
 *    phase step between bins, which carries the time position of the energy).
 *    Bins are visited loudest first; each prediction is weighted by the
 *    energy of the bin it comes from, so peaks fix their own phase and their
 *    skirts follow them.
 *  - Transient reset: on an onset the previous frame is ignored and the
 *    frame is rebuilt from the input phases and the vertical step alone.
 *  - Stereo: one rotation per output bin, applied to both channels, so the
 *    phase difference between channels survives the shift.
 *
 * Real-time shape. The work of one frame is cut into small jobs and paced
 * over the next interval, so one audio callback never pays for a whole FFT.
 * That costs one interval of latency: rx3_pv_latency() = block + interval.
 *
 * Self-contained: no libc, no libm, no allocation in the audio path. FFT,
 * sine/cosine, atan2 and square root are below. The caller owns the per-deck
 * memory (rx3_pv_bytes()); the plan (tables) is shared by every deck.
 */

#ifndef RX3_PHASE_VOCODER_H
#define RX3_PHASE_VOCODER_H

#define RX3_PV_MAX_N 5120u
#define RX3_PV_MAX_M 4096u
#define RX3_PV_MAX_BINS (RX3_PV_MAX_N / 2u + 1u)
/* Power of two above block + interval + the largest callback slice. */
#define RX3_PV_RING 8192u
#define RX3_PV_RING_MASK (RX3_PV_RING - 1u)
/* Onset: share of the frame's energy in bins that rose by more than 3 dB,
   with the whole frame at least this much louder than the previous one. */
#define RX3_PV_ONSET_SHARE 0.60f
#define RX3_PV_ONSET_RISE 1.30f

#define RX3_PV_PI 3.14159265358979f
#define RX3_PV_TWO_PI 6.28318530717959f

typedef struct { float re, im; } rx3_pv_complex;

/* Tables for one block size. Built outside the audio path. */
/* One step of a frame: what it does, on which range, and what it costs. */
#define RX3_PV_MAX_JOBS 320u
struct rx3_pv_job { unsigned char kind, pass; unsigned short a, b; unsigned short weight; };

struct rx3_pv_plan {
    unsigned int n, m, factor, bits, hop, bins, jobs, weight;
    struct rx3_pv_job job[RX3_PV_MAX_JOBS];
    float window[RX3_PV_MAX_N];
    rx3_pv_complex twiddle_m[RX3_PV_MAX_M / 2u];   /* e^{-2 pi i q / M} */
    rx3_pv_complex twiddle_n[RX3_PV_MAX_N];        /* e^{-2 pi i q / N} */
    unsigned short reverse[RX3_PV_MAX_M];
};

struct rx3_pv {
    const struct rx3_pv_plan *plan;
    /* Working memory, carved from the caller's block. */
    float *in;                     /* RX3_PV_RING stereo frames */
    float *out;                    /* RX3_PV_RING stereo frames, overlap-add */
    rx3_pv_complex *z, *spec;      /* N each */
    float *mag_l, *mag_r, *energy, *energy_prev, *freq;   /* input bins */
    rx3_pv_complex *ref, *ref_prev, *rel_l, *rel_r, *step;
    float *out_l, *out_r, *out_energy, *out_energy_prev;  /* output bins */
    rx3_pv_complex *phase, *phase_prev, *predict;
    unsigned short *source;        /* nearest input bin of each output bin */
    unsigned char *assigned;
    /* Clocks. */
    unsigned int written;          /* input frames received */
    unsigned int frame_start;      /* input index of the current frame */
    unsigned int job;              /* next job of the current frame */
    unsigned int pace;             /* job credit, in 1/hop units */
    unsigned int primed;           /* a frame has been captured */
    float ratio, frame_ratio;
    float gain, target_gain;       /* make-up gain: per frame, and asked for */
    int have_previous;
    int onset, force_onset;
    unsigned int analysed;         /* input bins analysed by the previous frame */
    /* Diagnostics. */
    unsigned int frames, onsets;
};

/* ------------------------------------------------------------- maths */

/* Table building only: double series, any argument. */
static double rx3_pv_dsin(double x)
{
    const double two_pi = 6.283185307179586476925;
    x -= two_pi * (double)(int)(x / two_pi);   /* table arguments stay far below 2^31 turns */
    if (x > 3.141592653589793238463) x -= two_pi;
    if (x < -3.141592653589793238463) x += two_pi;
    double term = x, sum = x, square = x * x;
    for (unsigned int k = 1; k < 16u; k++) {
        term *= -square / (double)((2u * k) * (2u * k + 1u));
        sum += term;
    }
    return sum;
}

static double rx3_pv_dcos(double x) { return rx3_pv_dsin(x + 1.570796326794896619231); }

/* Unit phasor of 2 pi * turns. Accurate to about 2e-7. */
static rx3_pv_complex rx3_pv_turn(float turns)
{
    float t = turns - (float)(int)turns;
    if (t < 0.0f) t += 1.0f;
    /* Quadrant, then a polynomial on [-pi/4, pi/4]. */
    float scaled = t * 4.0f;
    int quadrant = (int)(scaled + 0.5f);
    float x = (scaled - (float)quadrant) * (RX3_PV_PI * 0.5f);
    float x2 = x * x;
    float s = x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f +
                  x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f)))));
    float c = 1.0f + x2 * (-0.5f + x2 * (1.0f / 24.0f + x2 * (-1.0f / 720.0f +
                  x2 * (1.0f / 40320.0f))));
    rx3_pv_complex r;
    switch (quadrant & 3) {
    case 0: r.re = c; r.im = s; break;
    case 1: r.re = -s; r.im = c; break;
    case 2: r.re = -c; r.im = -s; break;
    default: r.re = s; r.im = -c; break;
    }
    return r;
}

/* atan2 with one division; absolute error below 1e-5 rad. */
static float rx3_pv_atan2(float y, float x)
{
    float ax = x < 0.0f ? -x : x, ay = y < 0.0f ? -y : y;
    if (ax == 0.0f && ay == 0.0f) return 0.0f;
    int swap = ay > ax;
    float a = swap ? ax / ay : ay / ax;          /* [0, 1] */
    float a2 = a * a;
    float r = a * (0.99997726f + a2 * (-0.33262347f + a2 * (0.19354346f +
                  a2 * (-0.11643287f + a2 * (0.05265332f + a2 * -0.01172120f)))));
    if (swap) r = 1.5707963268f - r;
    if (x < 0.0f) r = RX3_PV_PI - r;
    return y < 0.0f ? -r : r;
}

/* 1/sqrt by the bit estimate and two Newton steps: about 5e-6 relative. */
static float rx3_pv_rsqrt(float x)
{
    union { float f; unsigned int u; } v;
    v.f = x;
    v.u = 0x5f375a86u - (v.u >> 1);
    float y = v.f;
    for (int i = 0; i < 2; i++) y = y * (1.5f - 0.5f * x * y * y);
    return y;
}


static rx3_pv_complex rx3_pv_mul(rx3_pv_complex a, rx3_pv_complex b)
{
    rx3_pv_complex r = {a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re};
    return r;
}

static rx3_pv_complex rx3_pv_mulconj(rx3_pv_complex a, rx3_pv_complex b)
{
    rx3_pv_complex r = {a.re * b.re + a.im * b.im, a.im * b.re - a.re * b.im};
    return r;
}

/* Unit phasor of a, or `fallback` when a is (almost) zero. */
static rx3_pv_complex rx3_pv_unit(rx3_pv_complex a, rx3_pv_complex fallback)
{
    float p = a.re * a.re + a.im * a.im;
    if (p < 1.0e-30f) return fallback;
    float s = rx3_pv_rsqrt(p);
    rx3_pv_complex r = {a.re * s, a.im * s};
    return r;
}

/* ------------------------------------------------------------- plan */

enum {
    RX3_PV_JOB_GATHER, RX3_PV_JOB_STAGE, RX3_PV_JOB_COMBINE, RX3_PV_JOB_ANALYSE,
    RX3_PV_JOB_ONSET, RX3_PV_JOB_MAP, RX3_PV_JOB_PEAKS, RX3_PV_JOB_RISING,
    RX3_PV_JOB_FALLING, RX3_PV_JOB_SYNTH, RX3_PV_JOB_OVERLAP
};
/* Bins per chunk of the per-bin jobs: small enough that no single job
   dominates an audio callback. */
#define RX3_PV_CHUNK 160u

static void rx3_pv_add_job(struct rx3_pv_plan *plan, unsigned int kind, unsigned int pass,
                           unsigned int a, unsigned int b, unsigned int weight)
{
    if (plan->jobs >= RX3_PV_MAX_JOBS) return;
    struct rx3_pv_job *j = &plan->job[plan->jobs++];
    j->kind = (unsigned char)kind; j->pass = (unsigned char)pass;
    j->a = (unsigned short)a; j->b = (unsigned short)b;
    j->weight = (unsigned short)(weight ? weight : 1u);
    plan->weight += j->weight;
}

/* Ranged jobs over [0, count) in chunks. Weights are in units of 10 ns as
   measured on the host (`cost` is per element, in hundredths of a unit);
   only their ratios matter to the scheduler. */
static void rx3_pv_add_range(struct rx3_pv_plan *plan, unsigned int kind, unsigned int count,
                             unsigned int chunk, unsigned int cost)
{
    for (unsigned int from = 0; from < count; from += chunk) {
        unsigned int to = from + chunk > count ? count : from + chunk;
        rx3_pv_add_job(plan, kind, 0u, from, to, (to - from) * cost / 100u);
    }
}

/* The fixed sequence of one frame: transform, analysis, onset, map, peaks,
   flanks, synthesis, inverse transform, overlap-add. The frame's ratio is
   latched by its first job. Weights are relative costs; the scheduler pays
   them out evenly over the interval. */
static void rx3_pv_plan_jobs(struct rx3_pv_plan *plan)
{
    unsigned int f = plan->factor, b = plan->bins;
    plan->jobs = 0; plan->weight = 0;
    for (unsigned int pass = 0; pass < 2u; pass++) {
        if (pass == 1u) {
            rx3_pv_add_range(plan, RX3_PV_JOB_SYNTH, b, RX3_PV_CHUNK, 50u);
        } else {
            for (unsigned int r = 0; r < f; r++)
                rx3_pv_add_job(plan, RX3_PV_JOB_GATHER, 0u, r, 0u, plan->m * 127u / 1000u);
        }
        if (pass == 1u)
            for (unsigned int r = 0; r < f; r++)
                rx3_pv_add_job(plan, RX3_PV_JOB_GATHER, 1u, r, 0u, plan->m * 80u / 1000u);
        for (unsigned int r = 0; r < f; r++)
            for (unsigned int s = 0; s < plan->bits; s++)
                rx3_pv_add_job(plan, RX3_PV_JOB_STAGE, pass, r, s, plan->m * 68u / 1000u);
        if (f > 1u)
            for (unsigned int q = 0; q < 16u; q++)
                rx3_pv_add_job(plan, RX3_PV_JOB_COMBINE, pass, q * plan->m / 16u,
                               (q + 1u) * plan->m / 16u, plan->m * 26u / 160u);
        if (pass == 0u) {
            rx3_pv_add_range(plan, RX3_PV_JOB_ANALYSE, b, RX3_PV_CHUNK / 2u, 370u);
            rx3_pv_add_job(plan, RX3_PV_JOB_ONSET, 0u, 0u, 0u, b * 26u / 100u);
            rx3_pv_add_range(plan, RX3_PV_JOB_MAP, b, RX3_PV_CHUNK, 180u);
            rx3_pv_add_range(plan, RX3_PV_JOB_PEAKS, b, 4u * RX3_PV_CHUNK, 20u);
            rx3_pv_add_range(plan, RX3_PV_JOB_RISING, b, 2u * RX3_PV_CHUNK, 60u);
            rx3_pv_add_range(plan, RX3_PV_JOB_FALLING, b, 2u * RX3_PV_CHUNK, 60u);
        }
    }
    rx3_pv_add_range(plan, RX3_PV_JOB_OVERLAP, plan->n, plan->n / 8u, 25u);
}

/* Supported blocks: 2048, 4096 (radix 2) and 5120 (5 x 1024). The interval
   is always a quarter block, where Hann squared sums to a flat 1.5. */
static int rx3_pv_plan_init(struct rx3_pv_plan *plan, unsigned int n)
{
    const double two_pi = 6.283185307179586476925;
    unsigned int factor = n == 5120u ? 5u : 1u, m = n / factor, bits = 0;
    if (n != 2048u && n != 4096u && n != 5120u) return 0;
    while ((1u << bits) < m) bits++;
    plan->n = n; plan->m = m; plan->factor = factor; plan->bits = bits;
    plan->hop = n / 4u; plan->bins = n / 2u + 1u;
    for (unsigned int i = 0; i < n; i++)
        plan->window[i] = (float)(0.5 - 0.5 * rx3_pv_dcos(two_pi * i / n));
    for (unsigned int q = 0; q < m / 2u; q++) {
        plan->twiddle_m[q].re = (float)rx3_pv_dcos(two_pi * q / m);
        plan->twiddle_m[q].im = (float)-rx3_pv_dsin(two_pi * q / m);
    }
    for (unsigned int q = 0; q < n; q++) {
        plan->twiddle_n[q].re = (float)rx3_pv_dcos(two_pi * q / n);
        plan->twiddle_n[q].im = (float)-rx3_pv_dsin(two_pi * q / n);
    }
    for (unsigned int i = 0; i < m; i++) {
        unsigned int r = 0;
        for (unsigned int b = 0; b < bits; b++)
            if (i & (1u << b)) r |= 1u << (bits - 1u - b);
        plan->reverse[i] = (unsigned short)r;
    }
    rx3_pv_plan_jobs(plan);
    return 1;
}

static unsigned int rx3_pv_latency(const struct rx3_pv_plan *plan)
{
    return plan->n + plan->hop;
}

/* ------------------------------------------------------------- state */

#define RX3_PV_ALIGN(x) (((x) + 15u) & ~15u)

static unsigned long rx3_pv_bytes(const struct rx3_pv_plan *plan)
{
    unsigned long n = plan->n, b = plan->bins, total = 0;
    total += RX3_PV_ALIGN(RX3_PV_RING * 2u * sizeof(float)) * 2u;      /* in, out */
    total += RX3_PV_ALIGN(n * sizeof(rx3_pv_complex)) * 2u;            /* z, spec */
    total += RX3_PV_ALIGN(b * sizeof(float)) * 9u;
    total += RX3_PV_ALIGN(b * sizeof(rx3_pv_complex)) * 8u;
    total += RX3_PV_ALIGN(b * sizeof(unsigned short));
    total += RX3_PV_ALIGN(b);
    return total;
}

static void rx3_pv_clear(struct rx3_pv *pv)
{
    const struct rx3_pv_plan *p = pv->plan;
    for (unsigned int i = 0; i < RX3_PV_RING * 2u; i++) { pv->in[i] = 0.0f; pv->out[i] = 0.0f; }
    for (unsigned int k = 0; k < p->bins; k++) {
        pv->energy_prev[k] = 0.0f; pv->out_energy_prev[k] = 0.0f;
        pv->ref_prev[k].re = 1.0f; pv->ref_prev[k].im = 0.0f;
        pv->phase_prev[k].re = 1.0f; pv->phase_prev[k].im = 0.0f;
    }
    pv->written = 0; pv->frame_start = 0; pv->job = p->jobs; pv->pace = 0;
    pv->primed = 0; pv->have_previous = 0; pv->onset = 0; pv->force_onset = 0;
    pv->analysed = p->bins;
}

/* `memory` holds rx3_pv_bytes(plan) bytes, 16-byte aligned. */
static void rx3_pv_init(struct rx3_pv *pv, const struct rx3_pv_plan *plan, void *memory)
{
    unsigned char *at = memory;
    unsigned long n = plan->n, b = plan->bins;
#define RX3_PV_TAKE(field, type, count) \
    do { pv->field = (type *)(void *)at; at += RX3_PV_ALIGN((count) * sizeof(type)); } while (0)
    pv->plan = plan;
    RX3_PV_TAKE(in, float, RX3_PV_RING * 2u);
    RX3_PV_TAKE(out, float, RX3_PV_RING * 2u);
    RX3_PV_TAKE(z, rx3_pv_complex, n);
    RX3_PV_TAKE(spec, rx3_pv_complex, n);
    RX3_PV_TAKE(mag_l, float, b); RX3_PV_TAKE(mag_r, float, b);
    RX3_PV_TAKE(energy, float, b); RX3_PV_TAKE(energy_prev, float, b);
    RX3_PV_TAKE(freq, float, b);
    RX3_PV_TAKE(out_l, float, b); RX3_PV_TAKE(out_r, float, b);
    RX3_PV_TAKE(out_energy, float, b); RX3_PV_TAKE(out_energy_prev, float, b);
    RX3_PV_TAKE(ref, rx3_pv_complex, b); RX3_PV_TAKE(ref_prev, rx3_pv_complex, b);
    RX3_PV_TAKE(rel_l, rx3_pv_complex, b); RX3_PV_TAKE(rel_r, rx3_pv_complex, b);
    RX3_PV_TAKE(step, rx3_pv_complex, b);
    RX3_PV_TAKE(phase, rx3_pv_complex, b); RX3_PV_TAKE(phase_prev, rx3_pv_complex, b);
    RX3_PV_TAKE(predict, rx3_pv_complex, b);
    RX3_PV_TAKE(source, unsigned short, b);
    RX3_PV_TAKE(assigned, unsigned char, b);
#undef RX3_PV_TAKE
    pv->ratio = 1.0f; pv->frame_ratio = 1.0f; pv->gain = 1.0f; pv->target_gain = 1.0f;
    pv->frames = 0; pv->onsets = 0;
    rx3_pv_clear(pv);
}

/* Takes effect at the next frame. */
static void rx3_pv_set_ratio(struct rx3_pv *pv, float ratio)
{
    if (ratio < 0.5f) ratio = 0.5f;
    if (ratio > 2.0f) ratio = 2.0f;
    pv->ratio = ratio;
}

/* Equal temperament, -12 to +12. */
static const float rx3_pv_semitone_ratio[25] = {
    0.500000f, 0.529732f, 0.561231f, 0.594604f, 0.629961f, 0.667420f,
    0.707107f, 0.749154f, 0.793701f, 0.840896f, 0.890899f, 0.943874f,
    1.000000f,
    1.059463f, 1.122462f, 1.189207f, 1.259921f, 1.334840f, 1.414214f,
    1.498307f, 1.587401f, 1.681793f, 1.781797f, 1.887749f, 2.000000f
};

/* Level make-up per semitone. Interpolating magnitudes along the frequency
   map narrows a lobe when lowering pitch and widens it when raising, and
   lowering also drops everything above ratio * Nyquist. Measured with
   tests/keyshift/bench_keyshift.c as the mean of a tone and white noise:
   about -0.42 dB per semitone down, about -0.25 dB in total up.
   gain = 10^(dB/20) with dB = 0.42 * |st| down, 0.25 up. */
static const float rx3_pv_semitone_gain[25] = {
    1.786488f, 1.702159f, 1.621810f, 1.545254f, 1.472313f, 1.402814f,
    1.336596f, 1.273503f, 1.213389f, 1.156112f, 1.101539f, 1.049542f,
    1.000000f, 1.029201f, 1.029201f, 1.029201f, 1.029201f, 1.029201f,
    1.029201f, 1.029201f, 1.029201f, 1.029201f, 1.029201f, 1.029201f,
    1.029201f
};

static void rx3_pv_set_semitones(struct rx3_pv *pv, int semitones)
{
    if (semitones < -12) semitones = -12;
    if (semitones > 12) semitones = 12;
    rx3_pv_set_ratio(pv, rx3_pv_semitone_ratio[semitones + 12]);
    pv->target_gain = rx3_pv_semitone_gain[semitones + 12];
}

/* A transport jump: the next frames are rebuilt from the input phases. The
   pipeline keeps running, so the deck stays exactly rx3_pv_latency() late. */
static void rx3_pv_discontinuity(struct rx3_pv *pv) { pv->force_onset = 2; }

/* ------------------------------------------------------------- FFT */

/* Gather sub-sequence r (x[factor*i + r]) of `from` into block r of `to`,
   bit-reversed. With `window` the source is the input ring, windowed and
   rotated to zero phase; otherwise it is a complex spectrum. */
static void rx3_pv_gather(struct rx3_pv *pv, unsigned int r, int from_ring)
{
    const struct rx3_pv_plan *p = pv->plan;
    unsigned int n = p->n, m = p->m, f = p->factor, half = n / 2u;
    rx3_pv_complex *block = pv->z + r * m;
    for (unsigned int i = 0; i < m; i++) {
        unsigned int index = f * i + r;
        rx3_pv_complex v;
        if (from_ring) {
            /* Zero phase: sample index of the frame is index + N/2. */
            unsigned int t = index + half; if (t >= n) t -= n;
            unsigned int slot = (pv->frame_start + t) & RX3_PV_RING_MASK;
            float w = p->window[t];
            v.re = pv->in[slot * 2u] * w;
            v.im = pv->in[slot * 2u + 1u] * w;
        } else {
            v = pv->spec[index];
        }
        block[p->reverse[i]] = v;
    }
}

/* One radix-2 decimation-in-time stage on block r. */
static void rx3_pv_stage(struct rx3_pv *pv, unsigned int r, unsigned int stage)
{
    const struct rx3_pv_plan *p = pv->plan;
    unsigned int m = p->m, size = 2u << stage, half = size / 2u, stride = m / size;
    rx3_pv_complex *x = pv->z + r * m;
    for (unsigned int start = 0; start < m; start += size) {
        for (unsigned int k = 0; k < half; k++) {
            rx3_pv_complex w = p->twiddle_m[k * stride];
            rx3_pv_complex *a = x + start + k, *b = a + half;
            rx3_pv_complex t = rx3_pv_mul(*b, w);
            b->re = a->re - t.re; b->im = a->im - t.im;
            a->re += t.re; a->im += t.im;
        }
    }
}

/* Radix-5 recombination of the five length-M transforms, output bins
   [from, to) of the first M; each writes its five aliases k + qM. */
static void rx3_pv_combine(struct rx3_pv *pv, unsigned int from, unsigned int to)
{
    const struct rx3_pv_plan *p = pv->plan;
    unsigned int m = p->m, n = p->n;
    /* e^{-2 pi i q / 5}. */
    const rx3_pv_complex *w5 = p->twiddle_n;   /* index q*M */
    for (unsigned int k = from; k < to; k++) {
        rx3_pv_complex y[5];
        y[0] = pv->z[k];
        for (unsigned int r = 1; r < 5u; r++)
            y[r] = rx3_pv_mul(pv->z[r * m + k], p->twiddle_n[(r * k) % n]);
        for (unsigned int q = 0; q < 5u; q++) {
            rx3_pv_complex s = y[0];
            for (unsigned int r = 1; r < 5u; r++) {
                rx3_pv_complex t = rx3_pv_mul(y[r], w5[((r * q) % 5u) * m]);
                s.re += t.re; s.im += t.im;
            }
            pv->spec[k + q * m] = s;
        }
    }
}

/* The transform result: spec after recombination, z without. */
static rx3_pv_complex *rx3_pv_result(struct rx3_pv *pv)
{
    return pv->plan->factor > 1u ? pv->spec : pv->z;
}

/* ------------------------------------------------------------- analysis */

/* Input bins the map reads at this frame's ratio: above it, raising pitch
   discards them, so they are neither analysed nor kept. */
static unsigned int rx3_pv_input_limit(const struct rx3_pv *pv)
{
    const unsigned int last = pv->plan->bins - 1u;
    if (pv->frame_ratio <= 1.0f) return last + 1u;
    unsigned int limit = (unsigned int)((float)last / pv->frame_ratio) + 3u;
    return limit > last + 1u ? last + 1u : limit;
}

static void rx3_pv_analyse(struct rx3_pv *pv, unsigned int from, unsigned int to)
{
    unsigned int limit = rx3_pv_input_limit(pv);
    if (to > limit) to = limit;
    const struct rx3_pv_plan *p = pv->plan;
    const rx3_pv_complex *zf = rx3_pv_result(pv);
    unsigned int n = p->n;
    /* Expected advance per hop of bin k is k/4 turns (hop = N/4). */
    const float to_bins = (float)n / (RX3_PV_TWO_PI * (float)p->hop);
    const rx3_pv_complex one = {1.0f, 0.0f};
    for (unsigned int k = from; k < to; k++) {
        rx3_pv_complex a = zf[k], bc = zf[k ? n - k : 0u];
        bc.im = -bc.im;
        rx3_pv_complex xl = {(a.re + bc.re) * 0.5f, (a.im + bc.im) * 0.5f};
        rx3_pv_complex xr = {(a.im - bc.im) * 0.5f, -(a.re - bc.re) * 0.5f};
        float pl = xl.re * xl.re + xl.im * xl.im, pr = xr.re * xr.re + xr.im * xr.im;
        float il = pl > 1.0e-30f ? rx3_pv_rsqrt(pl) : 0.0f;
        float ir = pr > 1.0e-30f ? rx3_pv_rsqrt(pr) : 0.0f;
        pv->mag_l[k] = pl * il;
        pv->mag_r[k] = pr * ir;
        pv->energy[k] = pl + pr;
        rx3_pv_complex sum = {xl.re + xr.re, xl.im + xr.im};
        float ps = sum.re * sum.re + sum.im * sum.im;
        /* Anti-phase channels cancel in the sum: fall back to the louder one. */
        rx3_pv_complex ref = ps > 0.05f * (pl + pr) ? sum : (pl >= pr ? xl : xr);
        rx3_pv_complex u = rx3_pv_unit(ref, one);
        pv->ref[k] = u;
        /* Each channel's phase relative to the reference; silent: in phase. */
        rx3_pv_complex rl = rx3_pv_mulconj(xl, u), rr = rx3_pv_mulconj(xr, u);
        if (il > 0.0f) { rl.re *= il; rl.im *= il; } else rl = one;
        if (ir > 0.0f) { rr.re *= ir; rr.im *= ir; } else rr = one;
        pv->rel_l[k] = rl;
        pv->rel_r[k] = rr;
        /* Measured frequency, in bins. */
        rx3_pv_complex d = rx3_pv_mulconj(u, pv->ref_prev[k]);
        float advance = rx3_pv_atan2(d.im, d.re);
        /* hop = N/4: bin k is expected to advance k quarter turns. */
        float expected = (RX3_PV_PI * 0.5f) * (float)(k & 3u);
        float deviation = advance - expected;
        deviation -= RX3_PV_TWO_PI * (float)(int)(deviation * (1.0f / RX3_PV_TWO_PI) +
                                                   (deviation >= 0.0f ? 0.5f : -0.5f));
        pv->freq[k] = (float)k + deviation * to_bins;
        if (!pv->have_previous) pv->freq[k] = (float)k;
    }
}

/* Vertical step: the input's phase change from bin k to k+1. */
static void rx3_pv_steps(struct rx3_pv *pv)
{
    unsigned int limit = rx3_pv_input_limit(pv);
    for (unsigned int k = 0; k + 1u < limit; k++)
        pv->step[k] = rx3_pv_mulconj(pv->ref[k + 1u], pv->ref[k]);
    pv->step[limit - 1u] = pv->step[limit - 2u];
}

static void rx3_pv_detect_onset(struct rx3_pv *pv)
{
    const struct rx3_pv_plan *p = pv->plan;
    float total = 0.0f, previous = 0.0f, rising = 0.0f;
    unsigned int limit = rx3_pv_input_limit(pv);
    (void)p;
    for (unsigned int k = 1; k < limit; k++) {
        float e = pv->energy[k];
        total += e;
        previous += pv->energy_prev[k];
        if (e > 2.0f * pv->energy_prev[k]) rising += e;
    }
    int onset = pv->have_previous && total > 1.0e-12f &&
                rising > RX3_PV_ONSET_SHARE * total && total > RX3_PV_ONSET_RISE * previous;
    if (pv->force_onset) { onset = 1; pv->force_onset--; }
    /* Bins the previous frame skipped have no history: start them afresh. */
    if (limit > pv->analysed) onset = 1;
    if (!pv->have_previous) onset = 1;
    pv->onset = onset;
    if (onset) pv->onsets++;
}

/* Output bins [from, to): magnitudes and the time prediction. */
static void rx3_pv_map(struct rx3_pv *pv, unsigned int from, unsigned int to)
{
    const struct rx3_pv_plan *p = pv->plan;
    float r = pv->frame_ratio, inverse = 1.0f / r;
    unsigned int last = p->bins - 1u;
    /* Advance per hop of a frequency of `f` bins is f/4 turns. */
    float turns_per_bin = (float)p->hop / (float)p->n;
    for (unsigned int j = from; j < to; j++) {
        float k = (float)j * inverse;
        if (k > (float)last) {
            pv->out_l[j] = pv->out_r[j] = pv->out_energy[j] = 0.0f;
            pv->source[j] = (unsigned short)last;
            pv->predict[j] = pv->phase_prev[j];
            continue;
        }
        unsigned int k0 = (unsigned int)k, k1 = k0 < last ? k0 + 1u : last;
        float t = k - (float)k0;
        pv->out_l[j] = pv->mag_l[k0] + (pv->mag_l[k1] - pv->mag_l[k0]) * t;
        pv->out_r[j] = pv->mag_r[k0] + (pv->mag_r[k1] - pv->mag_r[k0]) * t;
        pv->out_energy[j] = pv->out_l[j] * pv->out_l[j] + pv->out_r[j] * pv->out_r[j];
        pv->source[j] = (unsigned short)(t < 0.5f ? k0 : k1);
        float f = pv->freq[k0] + (pv->freq[k1] - pv->freq[k0]) * t;
        pv->predict[j] = rx3_pv_mul(pv->phase_prev[j], rx3_pv_turn(f * r * turns_per_bin));
    }
}

/* ------------------------------------------------------------- phase */

/* Order of output bins: louder first, the lower index on a tie. A bin louder
   than both neighbours is a peak; any other bin has exactly one louder
   neighbour, and following louder neighbours always ends at a peak. */
static int rx3_pv_louder(const struct rx3_pv *pv, unsigned int a, unsigned int b)
{
    float ea = pv->out_energy[a], eb = pv->out_energy[b];
    return ea > eb || (ea == eb && a < b);
}

/* Prediction for output bin j from the already phased bin n = j +/- 1.
   The output keeps the input's phase step per bin, whatever the ratio: that
   step encodes where in the block the energy sits, so keeping it keeps
   transients in place instead of pulling them towards the block centre. */
static rx3_pv_complex rx3_pv_vertical(const struct rx3_pv *pv, unsigned int n, unsigned int j)
{
    if (j > n) return rx3_pv_mul(pv->phase[n], pv->step[pv->source[n]]);
    return rx3_pv_mulconj(pv->phase[n], pv->step[pv->source[j]]);
}

/* Peaks first: their own time prediction (on an onset, the input phase). */
static void rx3_pv_peaks(struct rx3_pv *pv, unsigned int from, unsigned int to)
{
    const unsigned int last = pv->plan->bins - 1u;
    for (unsigned int j = from; j < to; j++) {
        int peak = (j == 0u || rx3_pv_louder(pv, j, j - 1u)) &&
                   (j == last || rx3_pv_louder(pv, j, j + 1u));
        pv->assigned[j] = (unsigned char)peak;
        if (peak) pv->phase[j] = pv->onset ? pv->ref[pv->source[j]] : pv->predict[j];
    }
}

/* Then the neighbours, walking down each flank away from its peak. Each bin
   blends the vertical prediction from its louder neighbour with its own time
   prediction, weighted by the energy each comes from. Rising flanks are
   walked upwards (direction +1) and falling ones downwards (-1); a chunk
   continues where the previous one stopped. */
static void rx3_pv_flank(struct rx3_pv *pv, unsigned int from, unsigned int to, int direction)
{
    const unsigned int last = pv->plan->bins - 1u;
    for (unsigned int i = from; i < to; i++) {
        unsigned int j = direction > 0 ? i : last - i;
        if (pv->assigned[j]) continue;
        unsigned int n;
        if (direction > 0) {
            if (j == 0u || (j < last && rx3_pv_louder(pv, j + 1u, j - 1u))) continue;
            n = j - 1u;
        } else {
            if (j == last || (j > 0u && rx3_pv_louder(pv, j - 1u, j + 1u))) continue;
            n = j + 1u;
        }
        rx3_pv_complex v = rx3_pv_vertical(pv, n, j);
        float wv = pv->out_energy[n];
        rx3_pv_complex sum = {v.re * wv, v.im * wv};
        if (!pv->onset) {
            float wt = pv->out_energy_prev[j];
            sum.re += pv->predict[j].re * wt; sum.im += pv->predict[j].im * wt;
        }
        pv->phase[j] = rx3_pv_unit(sum, pv->onset ? v : pv->predict[j]);
        pv->assigned[j] = 1u;
    }
}

/* ------------------------------------------------------------- synthesis */

/* Packed conjugate spectrum of the two output channels, bins [from, to). */
static void rx3_pv_synthesise(struct rx3_pv *pv, unsigned int from, unsigned int to)
{
    const struct rx3_pv_plan *p = pv->plan;
    unsigned int n = p->n, last = p->bins - 1u;
    for (unsigned int j = from; j < to; j++) {
        unsigned int k = pv->source[j];
        rx3_pv_complex yl = rx3_pv_mul(pv->phase[j], pv->rel_l[k]);
        rx3_pv_complex yr = rx3_pv_mul(pv->phase[j], pv->rel_r[k]);
        yl.re *= pv->out_l[j]; yl.im *= pv->out_l[j];
        yr.re *= pv->out_r[j]; yr.im *= pv->out_r[j];
        if (j == 0u || j == last) { yl.im = 0.0f; yr.im = 0.0f; }
        /* P[j] = YL + i YR, P[N-j] = conj(YL) + i conj(YR); store conj(P). */
        rx3_pv_complex pj = {yl.re - yr.im, yl.im + yr.re};
        pv->spec[j].re = pj.re; pv->spec[j].im = -pj.im;
        if (j != 0u && j != last) {
            rx3_pv_complex pn = {yl.re + yr.im, -yl.im + yr.re};
            pv->spec[n - j].re = pn.re; pv->spec[n - j].im = -pn.im;
        }
    }
}

/* Overlap-add samples [from, to) of the frame. */
static void rx3_pv_overlap(struct rx3_pv *pv, unsigned int from, unsigned int to)
{
    const struct rx3_pv_plan *p = pv->plan;
    const rx3_pv_complex *y = rx3_pv_result(pv);
    unsigned int n = p->n, half = n / 2u;
    /* 1/N for the inverse, 1/1.5 for Hann squared at a quarter hop. */
    float scale = pv->gain / (1.5f * (float)n);
    for (unsigned int t = from; t < to; t++) {
        unsigned int index = t + half; if (index >= n) index -= n;
        float w = p->window[t] * scale;
        unsigned int slot = (pv->frame_start + t) & RX3_PV_RING_MASK;
        pv->out[slot * 2u] += y[index].re * w;
        pv->out[slot * 2u + 1u] -= y[index].im * w;
    }
}

static void rx3_pv_finish_frame(struct rx3_pv *pv)
{
    const struct rx3_pv_plan *p = pv->plan;
    for (unsigned int k = 0; k < p->bins; k++) {
        pv->energy_prev[k] = pv->energy[k];
        pv->ref_prev[k] = pv->ref[k];
        pv->out_energy_prev[k] = pv->out_energy[k];
        pv->phase_prev[k] = pv->phase[k];
    }
    pv->analysed = rx3_pv_input_limit(pv);
    pv->have_previous = 1;
    pv->frames++;
}

/* ------------------------------------------------------------- jobs */

static void rx3_pv_run_job(struct rx3_pv *pv, unsigned int index)
{
    const struct rx3_pv_plan *p = pv->plan;
    const struct rx3_pv_job *j = &p->job[index];
    switch (j->kind) {
    case RX3_PV_JOB_GATHER:
        /* The ratio is fixed for the whole frame from its first job. */
        if (j->pass == 0u && j->a == 0u) { pv->frame_ratio = pv->ratio; pv->gain = pv->target_gain; }
        rx3_pv_gather(pv, j->a, j->pass == 0u);
        break;
    case RX3_PV_JOB_STAGE: rx3_pv_stage(pv, j->a, j->b); break;
    case RX3_PV_JOB_COMBINE: rx3_pv_combine(pv, j->a, j->b); break;
    case RX3_PV_JOB_ANALYSE: rx3_pv_analyse(pv, j->a, j->b); break;
    case RX3_PV_JOB_ONSET: rx3_pv_steps(pv); rx3_pv_detect_onset(pv); break;
    case RX3_PV_JOB_MAP: rx3_pv_map(pv, j->a, j->b); break;
    case RX3_PV_JOB_PEAKS: rx3_pv_peaks(pv, j->a, j->b); break;
    case RX3_PV_JOB_RISING: rx3_pv_flank(pv, j->a, j->b, 1); break;
    case RX3_PV_JOB_FALLING: rx3_pv_flank(pv, j->a, j->b, -1); break;
    case RX3_PV_JOB_SYNTH: rx3_pv_synthesise(pv, j->a, j->b); break;
    default:
        rx3_pv_overlap(pv, j->a, j->b);
        if (j->b == p->n) rx3_pv_finish_frame(pv);
        break;
    }
}

static void rx3_pv_flush_jobs(struct rx3_pv *pv)
{
    while (pv->job < pv->plan->jobs) rx3_pv_run_job(pv, pv->job++);
}

/* ------------------------------------------------------------- stream */

/* Shift `frames` interleaved stereo frames in place. The output is the
   shifted input delayed by exactly rx3_pv_latency() frames. */
static void rx3_pv_process(struct rx3_pv *pv, float *io, unsigned int frames)
{
    const struct rx3_pv_plan *p = pv->plan;
    const unsigned int hop = p->hop, latency = rx3_pv_latency(p);
    while (frames) {
        unsigned int phase = pv->written % hop, n = hop - phase;
        if (n > frames) n = frames;
        for (unsigned int i = 0; i < n; i++) {
            unsigned int slot = (pv->written + i) & RX3_PV_RING_MASK;
            pv->in[slot * 2u] = io[i * 2u];
            pv->in[slot * 2u + 1u] = io[i * 2u + 1u];
            unsigned int read = (pv->written + i - latency) & RX3_PV_RING_MASK;
            io[i * 2u] = pv->out[read * 2u];
            io[i * 2u + 1u] = pv->out[read * 2u + 1u];
            pv->out[read * 2u] = 0.0f;
            pv->out[read * 2u + 1u] = 0.0f;
        }
        /* Pace the current frame's jobs over its interval. */
        /* Pay the frame's cost out evenly over its interval. */
        pv->pace += n * p->weight;
        while (pv->job < p->jobs && pv->pace >= p->job[pv->job].weight * hop) {
            pv->pace -= p->job[pv->job].weight * hop;
            rx3_pv_run_job(pv, pv->job++);
        }
        pv->written += n;
        io += n * 2u;
        frames -= n;
        if (pv->written % hop == 0u && pv->written >= p->n) {
            rx3_pv_flush_jobs(pv);
            pv->frame_start = pv->written - p->n;
            pv->job = 0;
            pv->pace = 0;
            pv->primed = 1;
        }
    }
}

#endif /* RX3_PHASE_VOCODER_H */

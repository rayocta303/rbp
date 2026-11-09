/* SPDX-License-Identifier: MPL-2.0 */
/* Sliding filters in the style of the RMX-1000 SCENE FX (HPF, LPF, BPF), 256
 * steps each, as plain DSP kernels: no allocation, no I/O, no libm, no
 * hidden state. The caller owns every buffer and every state.
 *
 * Original work. The filters are the standard RBJ "Audio EQ Cookbook"
 * biquads (Robert Bristow-Johnson) at the 48 kHz of the unit; the sweep laws
 * below are our own parametrisation of what was MEASURED on the real unit
 * (cut-off and Q of each step, recovered by inverting the RBJ forms). No
 * table, code or byte of that unit is in this repository: what follows is a
 * dozen parameters of closed laws, and every step is computed from them.
 *
 * The five series of the unit (index = series base + step 0..255):
 *   HPF    (hpf_a)  steps 0..3 straight pass; then f0 = 60 Hz * 1.017001^(k-4),
 *                   rounded to a whole Hz; Q = 1/sqrt 2 (Butterworth). 60 Hz up
 *                   to 4129 Hz.
 *   LPF             f0 = 17330 Hz * 0.983423^(k-4) from step 4 (down to 261 Hz);
 *                   Butterworth. The unit holds steps 0..4 at 17330 Hz; here
 *                   steps 0..3 are a straight pass instead, so that the knob
 *                   at rest is dry for every type (a deliberate choice: the
 *                   17 kHz filter would take a little off the top of the mix).
 *   BPF    (bpf_1)  steps 0..3 straight pass; band-pass with a 0 dB peak whose
 *                   ln f0 and ln Q are smooth (Chebyshev) functions of the step,
 *                   in two pieces (4..26 and 27..255). Q from 0.01 (an almost
 *                   flat, very wide band) to about 2.8.
 *   HPF_B  (hpf_b)  a second high-pass: 54 Hz * 1.019149^(k-4), up to 6310 Hz;
 *                   steps 0..4 frozen at 54 Hz; Butterworth.
 *   BPF_2  (bpf_2)  a second band-pass, ln f0 and ln Q again Chebyshev
 *                   (steps 0..3 constant, 4..255 degree 5); the centre goes
 *                   from about 1.3 kHz down to about 870 Hz while Q rises.
 * The first three are what the SCENE knob of this module plays; the other two
 * are in the kernel and tested but not wired to the knob (which series the
 * unit's own knob uses is not known until it is measured on the console).
 * Fit against the unit's own table (checked outside the repository): at most
 * 0.24 dB of difference in magnitude for the HPF, 0.09 dB for the BPF, none
 * for the LPF, 0.04 dB for HPF_B, 0.11 dB for BPF_2.
 *
 * Every step is computed for the sample rate in use, so the cut-offs are in
 * Hz whatever the rate. The knob of this module is ONE knob read as a
 * fractional step: the laws are defined between whole steps too (the rounded
 * HPF by interpolating between the neighbouring whole-Hz values, the BPF by
 * interpolating ln f0 and ln Q), so a gliding knob moves the cut-off
 * smoothly and passes through exactly the unit's values at whole steps.
 *
 * For the caller: the step is a double on purpose. Glide the knob (a one-pole
 * smoother of about 10 ms on the step, then rmxf_step_design every few
 * frames) before it reaches rmxf_step_design: a knob turned fast and passed
 * through raw makes the coefficients jump and the output zip. The BEAT FX
 * module does exactly that.
 */
#ifndef RX3_RMX_FILTER_H
#define RX3_RMX_FILTER_H

#define RMXF_STEPS 256u
#define RMXF_TOP (RMXF_STEPS - 1u)
/* The knob at rest is a straight pass: steps 0..3 of the HPF and of the BPF
   series do nothing on the unit, and the LPF is made to do the same here.
   (HPF_B and the second BPF are not wired to the knob and keep the law: their
   first steps are frozen at the first value.) */
#define RMXF_PASS_STEPS 4.0
/* The sweeps start at step 4 and the geometric ones count from there. */
#define RMXF_FIRST_STEP 4.0

#define RMXF_PI 3.14159265358979323846
#define RMXF_BUTTERWORTH_Q 0.70710678118654752440

/* Which series: the first three are what the module's SCENE knob plays. */
enum rmxf_type { RMXF_HPF = 0, RMXF_LPF = 1, RMXF_BPF = 2, RMXF_HPF_B = 3, RMXF_BPF_2 = 4, RMXF_TYPES = 5 };
/* The RBJ form of a series. */
enum rmxf_shape { RMXF_SHAPE_HPF = 0, RMXF_SHAPE_LPF = 1, RMXF_SHAPE_BPF = 2 };

/* Normalised so that a0 is 1: y = b0 x + z1; z1 = b1 x - a1 y + z2; z2 = b2 x - a2 y. */
struct rmxf_coef { double b0, b1, b2, a1, a2; };
struct rmxf_state { double z1, z2; };

/* The geometric series: first cut-off (Hz) at step 4, ratio per step, and the
   same ratio as log2 so that a step is a single 2^x. */
#define RMXF_HPF_FIRST_HZ 60.0
#define RMXF_HPF_LOG2_RATIO 0.024321458595887094     /* ratio 1.017001 */
#define RMXF_LPF_FIRST_HZ 17330.0
#define RMXF_LPF_LOG2_RATIO (-0.024115848754890975)  /* ratio 0.983423 */
#define RMXF_HPF_B_FIRST_HZ 54.0
#define RMXF_HPF_B_LOG2_RATIO 0.027364688397236974   /* ratio 1.019149 */

/* The band-pass series: ln f0 and ln Q as sums of Chebyshev polynomials of the
   step, in pieces. t = 2 (k - first) / (last - first) - 1 on each piece. */
struct rmxf_piece {
    unsigned int first, last;
    unsigned int n_f, n_q;
    const double *ln_f, *ln_q;
};
static const double rmxf_bpf1_f_a[4] = {5.381250968964206, 0.020264033518853013, 0.0011911563770387594, 4.607428248539188e-05};
static const double rmxf_bpf1_q_a[4] = {-4.605132021893931, 8.63133528717617e-05, 5.195288880671205e-05, 5.46759058200389e-05};
static const double rmxf_bpf1_f_b[12] = {6.612929884624089, 1.552479737265681, 0.33984425248371214,
    -0.022358364187908007, -0.016211298133362126, 0.002793898242021543, 0.0012070426267267297,
    -0.0003492400912274428, -9.515654028961496e-05, 4.3141146310199225e-05, 7.2556028715858135e-06,
    -6.370691647575968e-06};
static const double rmxf_bpf1_q_b[12] = {-0.965106547540008, 2.648042368178632, -0.7799842958111113,
    0.25862006482747457, -0.0675294660995641, -0.0043383265506752025, 0.026024135234454623,
    -0.028485760276127692, 0.0225923924683399, -0.016452575460686052, 0.00942047253773485,
    -0.005800219515668794};
static const struct rmxf_piece rmxf_bpf1[2] = {
    {4u, 26u, 4u, 4u, rmxf_bpf1_f_a, rmxf_bpf1_q_a},
    {27u, 255u, 12u, 12u, rmxf_bpf1_f_b, rmxf_bpf1_q_b},
};
static const double rmxf_bpf2_f_a[1] = {7.182742827404047};
static const double rmxf_bpf2_q_a[1] = {-3.1963544145703047};
static const double rmxf_bpf2_f_b[6] = {6.8934419980523245, -0.17489007999816755, 0.06698330462676039,
    -0.030794199401955862, 0.011821724409166633, -0.004811520915671737};
static const double rmxf_bpf2_q_b[6] = {-1.687861471476911, 1.4513108875976977, 0.016746421877919557,
    0.06467200545847994, -0.0008333713869780595, 0.008423100528157762};
static const struct rmxf_piece rmxf_bpf2[2] = {
    {0u, 3u, 1u, 1u, rmxf_bpf2_f_a, rmxf_bpf2_q_a},
    {4u, 255u, 6u, 6u, rmxf_bpf2_f_b, rmxf_bpf2_q_b},
};

static inline double rmxf_clamp(double x, double low, double high)
{
    return x < low ? low : x > high ? high : x;
}

/* 2^x for |x| < 60, about 1e-8 relative: seven terms of exp(x ln 2) on the
   fraction taken from the NEAREST whole number (|f| <= 0.5; from the floor the
   error would be 1e-6 near f = 1), then a power of two by repeated
   multiplication. */
static inline double rmxf_exp2(double x)
{
    int whole = (int)(x >= 0.0 ? x + 0.5 : x - 0.5);
    double f = x - (double)whole;
    double p = 1.0 + f * (0.6931471805599453 + f * (0.2402265069591007 + f * (0.0555041086648216 +
               f * (0.0096181291076285 + f * (0.0013333558146428 + f * (0.0001540353039338 +
               f * 0.0000152527338040))))));
    /* 2^whole by multiplying, no ldexp and no 64-bit bit tricks. */
    double scale = 1.0;
    if (whole >= 0) for (int i = 0; i < whole; i++) scale *= 2.0;
    else for (int i = 0; i > whole; i--) scale *= 0.5;
    return p * scale;
}

/* cos(pi y) for |y| <= 0.5 (error below 1e-8). */
static inline double rmxf_cos_pi(double y)
{
    double x = RMXF_PI * y, x2 = x * x;
    return 1.0 - x2 * (0.5 - x2 * (1.0 / 24.0 - x2 * (1.0 / 720.0 - x2 * (1.0 / 40320.0 -
           x2 * (1.0 / 3628800.0 - x2 * (1.0 / 479001600.0))))));
}

/* cos and sin of w = pi y for y in [0, 1]. */
static inline double rmxf_cos_w(double y)
{
    return y <= 0.5 ? rmxf_cos_pi(y) : -rmxf_cos_pi(1.0 - y);
}
static inline double rmxf_sin_w(double y)
{
    return rmxf_cos_pi(0.5 - y);
}

/* Sum of Chebyshev polynomials c[0..n-1] at t in [-1, 1] (Clenshaw). */
static inline double rmxf_cheb(const double *c, unsigned int n, double t)
{
    double b1 = 0.0, b2 = 0.0;
    for (unsigned int i = n - 1u; i > 0u; i--) {
        double b0 = c[i] + 2.0 * t * b1 - b2;
        b2 = b1;
        b1 = b0;
    }
    return c[0] + t * b1 - b2;
}

/* ln f0 and ln Q of a band-pass series at a WHOLE step. */
static inline void rmxf_bpf_at(const struct rmxf_piece *pieces, unsigned int k, double *ln_f, double *ln_q)
{
    const struct rmxf_piece *p = k >= pieces[1].first ? &pieces[1] : &pieces[0];
    double t = 2.0 * (double)(k - p->first) / (double)(p->last - p->first) - 1.0;
    *ln_f = rmxf_cheb(p->ln_f, p->n_f, t);
    *ln_q = rmxf_cheb(p->ln_q, p->n_q, t);
}

/* A band-pass series at a fractional step: ln f0 and ln Q are interpolated
   between the two whole steps around it, so it agrees with the law exactly at
   whole steps and moves smoothly between them (also across the two pieces). */
static inline void rmxf_bpf_curve(const struct rmxf_piece *pieces, double step, double *hz, double *q)
{
    unsigned int k0 = (unsigned int)step;
    unsigned int k1 = k0 < RMXF_TOP ? k0 + 1u : k0;
    double frac = step - (double)k0, f0, q0, f1, q1;
    rmxf_bpf_at(pieces, k0, &f0, &q0);
    rmxf_bpf_at(pieces, k1, &f1, &q1);
    *hz = rmxf_exp2((f0 + (f1 - f0) * frac) * 1.4426950408889634);   /* e^x = 2^(x log2 e) */
    *q = rmxf_exp2((q0 + (q1 - q0) * frac) * 1.4426950408889634);
}

/* The rounded first series: whole Hz at whole steps, straight line between. */
static inline double rmxf_hpf_hz(unsigned int k)
{
    double hz = RMXF_HPF_FIRST_HZ * rmxf_exp2((double)(k - 4u) * RMXF_HPF_LOG2_RATIO);
    return (double)(unsigned int)(hz + 0.5);
}

/* The cut-off (Hz) and Q of a series at a position in steps, 0 .. 255
   (fractions are fine: the caller smooths the knob). Where the unit passes
   the signal straight (HPF and BPF below step 4) there is no filter: the
   function returns 0 and the caller treats the stage as a pass. */
static inline int rmxf_curve(unsigned int type, double step, double *hz, double *q)
{
    if (type >= RMXF_TYPES) return 0;
    step = rmxf_clamp(step, 0.0, (double)RMXF_TOP);
    if (type == RMXF_HPF || type == RMXF_LPF || type == RMXF_BPF) {
        if (step < RMXF_PASS_STEPS) return 0;
    }
    if (type == RMXF_BPF) {
        rmxf_bpf_curve(rmxf_bpf1, step, hz, q);
        return 1;
    }
    if (type == RMXF_BPF_2) {
        rmxf_bpf_curve(rmxf_bpf2, step, hz, q);
        return 1;
    }
    *q = RMXF_BUTTERWORTH_Q;
    if (type == RMXF_HPF) {
        unsigned int k0 = (unsigned int)step;
        unsigned int k1 = k0 < RMXF_TOP ? k0 + 1u : k0;
        double lo = rmxf_hpf_hz(k0);
        *hz = lo + (rmxf_hpf_hz(k1) - lo) * (step - (double)k0);
        return 1;
    }
    double x = step > RMXF_FIRST_STEP ? step - RMXF_FIRST_STEP : 0.0;   /* frozen before step 4 */
    if (type == RMXF_LPF) *hz = RMXF_LPF_FIRST_HZ * rmxf_exp2(x * RMXF_LPF_LOG2_RATIO);
    else *hz = RMXF_HPF_B_FIRST_HZ * rmxf_exp2(x * RMXF_HPF_B_LOG2_RATIO);
    return 1;
}

/* The RBJ form of a series. */
static inline unsigned int rmxf_shape(unsigned int type)
{
    return type == RMXF_LPF ? RMXF_SHAPE_LPF
         : (type == RMXF_BPF || type == RMXF_BPF_2) ? RMXF_SHAPE_BPF : RMXF_SHAPE_HPF;
}

/* RBJ coefficients for a cut-off in Hz at a sample rate; the cut-off is kept
   under 0.45 of the rate so a low rate cannot push the pole out of the circle. */
static inline struct rmxf_coef rmxf_design(unsigned int type, double hz, double q, double rate)
{
    struct rmxf_coef c = {1.0, 0.0, 0.0, 0.0, 0.0};
    if (!(rate > 0.0) || !(q > 0.0) || type >= RMXF_TYPES) return c;
    double y = rmxf_clamp(2.0 * hz / rate, 1e-4, 0.9);   /* w0 = pi y */
    double cs = rmxf_cos_w(y), sn = rmxf_sin_w(y);
    double alpha = sn / (2.0 * q);
    double a0 = 1.0 + alpha;
    double inv = 1.0 / a0;
    c.a1 = -2.0 * cs * inv;
    c.a2 = (1.0 - alpha) * inv;
    switch (rmxf_shape(type)) {
    case RMXF_SHAPE_HPF:
        c.b0 = (1.0 + cs) * 0.5 * inv; c.b1 = -(1.0 + cs) * inv; c.b2 = c.b0;
        break;
    case RMXF_SHAPE_LPF:
        c.b0 = (1.0 - cs) * 0.5 * inv; c.b1 = (1.0 - cs) * inv; c.b2 = c.b0;
        break;
    default:   /* band-pass, constant 0 dB peak gain */
        c.b0 = alpha * inv; c.b1 = 0.0; c.b2 = -c.b0;
        break;
    }
    return c;
}

/* The coefficients of a step; a pass (unity) below the pass steps. */
static inline struct rmxf_coef rmxf_step_design(unsigned int type, double step, double rate)
{
    double hz = 0.0, q = 0.0;
    if (!rmxf_curve(type, step, &hz, &q)) {
        struct rmxf_coef pass = {1.0, 0.0, 0.0, 0.0, 0.0};
        return pass;
    }
    return rmxf_design(type, hz, q, rate);
}

/* Transposed direct form II, one sample. */
static inline double rmxf_tick(const struct rmxf_coef *c, struct rmxf_state *s, double x)
{
    double y = c->b0 * x + s->z1;
    s->z1 = c->b1 * x - c->a1 * y + s->z2;
    s->z2 = c->b2 * x - c->a2 * y;
    return y;
}

#endif

/* SPDX-License-Identifier: MPL-2.0 */
/* RMX-1000 style SCENE FX on the BEAT FX (rbp 1.20): sliding HPF, LPF or BPF
 * of 256 steps, with an optional tempo echo after it. Developer (unstable)
 * build only.
 *
 * Original work: standard RBJ biquads (rx3_rmx_filter.h) and a plain delay
 * line. Nothing of the RMX-1000's firmware or of any other product's code is
 * used; the curves were tuned against the RMX-1000's measured cut-offs (see
 * docs/RMX1000_PIANO.md).
 *
 * Where it plays. The BEAT FX selector position 4 plays
 * mixerengine::BeatEffectHelix. Here that position plays the scene instead,
 * on whatever the BFX CH selector sends to the effect (a channel or the
 * master), and the controls keep their meaning:
 *   Lv/Depth knob   the filter position, 0..1 onto steps 0..255 (fully
 *                   counter-clockwise = no filter, as the SCENE FX knob of the
 *                   RMX-1000 at rest)
 *   ON/OFF          the effect in and out, with the stock effect's own fade
 *   BEAT < >        the echo time, when the echo is on (the stock time of the
 *                   effect, in time with the MASTER BPM)
 *
 * How it hooks (mixerengine::BeatEffectHelix, rbp 1.20, symbols present):
 *   execute(this, const Float2 *in, Float2 *out, int frames)   @0x00092e7c
 * The stock execute still runs on every block, into a scratch buffer, so the
 * effect keeps its own bookkeeping (smoothers, flags, end of the effect);
 * its output is dropped and this module writes `out`. The hook point, the
 * guard and the object fields below come from the project's own static
 * analysis of that function; none of them has been checked on a console from
 * this module.
 *
 * Object fields read (offsets in bytes):
 *   +0x04 sample rate (Hz)   +0x20 Lv/Depth (0..1)   +0x8c cycle time, ms
 *   +0x74 and +0x78: linear smoothers (+0x10 float current value); their
 *   product is the stock fade in and out, used here as the amount of effect.
 * If either smoother is missing the block is left to the stock effect.
 *
 * The chooser. With the chooser on (the default, when the core offers its touch
 * panels and key service) position 4 is no longer one fixed scene: it plays the
 * effect picked on the screen. Entry HELIX is the player's own effect, the
 * default, and runs the stock code untouched. HPF, LPF and BPF ECHO are the
 * filter and the echo; CRUSH, SPIRAL UP/DN and REVERB UP/DN are the other SCENE FX
 * (rx3_rmx_fxsel.h). Turning the selector to position 4 opens one row of touch
 * buttons, one per effect (the one that plays is lit) and an X to close it. The
 * physical BEAT FX buttons and the knob are only read, never taken: this
 * handler always lets the key through. Opening and closing the row is a request
 * to the core's panel service, applied on its render thread. With the chooser
 * off (selector=off) the module plays the one scene of the configuration, as
 * before. Not tried on a console.
 *
 *   0x448b BeatEffectSW (type)   read: raw is the position 0..13
 *
 * Audio thread rules: no allocation, I/O or waiting. The delay line is static
 * memory (512 KiB of zero-initialised data, mapped lazily). Calculations in
 * double; no libm and no integer division, so the core's imports do not grow.
 */
#include "../core/api/rx3_module_api.h"
#include "rx3_rmx_filter.h"
#include "rx3_rmx_fxsel.h"

#define SCENE_EXECUTE ((unsigned long)0x00092e7cu)
#define SCENE_CHUNK 512u
#define SCENE_SUB 16u                  /* frames between two coefficient updates */
#define SCENE_ECHO_FRAMES 65536u       /* power of two: 1.49 s at 44.1 kHz */
#define SCENE_ECHO_MASK (SCENE_ECHO_FRAMES - 1u)
#define SCENE_SMOOTH_SECONDS 0.012     /* the knob's glide */
#define SCENE_ECHO_SLEW 0.0004         /* delay-time glide per frame, fraction of the gap */
#define SCENE_DEFAULT_FEEDBACK 45u     /* percent */
#define SCENE_DEFAULT_LEVEL 70u        /* percent */
#define SCENE_MAX_FEEDBACK 90u

/* A pointer field of the effect object (4 bytes on the player). The host
   tests, where a pointer is 8 bytes, define their own. */
#ifndef SCENE_POINTER_AT
#define SCENE_POINTER_AT(object, offset) (*(uint8_t *const *)((object) + (offset)))
#endif

static const uint8_t scene_guard[8] = {0xf8, 0x4f, 0x2d, 0xe9, 0x04, 0x8b, 0x2d, 0xed};

typedef void (*scene_execute_fn)(void *effect, const float *in, float *out, int frames);

struct scene_settings {
    unsigned int type;       /* enum rmxf_type */
    unsigned int echo;       /* 0 filter only, 1 filter and echo */
    unsigned int feedback;   /* percent */
    unsigned int level;      /* percent */
    unsigned int filter;     /* 0 the filter is bypassed (the echo alone), 1 it plays */
    unsigned int selector;   /* 1 the chooser is on */
    unsigned int mode;       /* enum scene_mode */
};

/* What the effect does besides (or instead of) the filter. */
enum scene_mode { SCENE_FILTER = 0, SCENE_CRUSH, SCENE_SPIRAL_UP, SCENE_SPIRAL_DOWN, SCENE_REVERB_UP, SCENE_REVERB_DOWN };

#define SCENE_KEY_TYPE 0x448bu
#define SCENE_OP_VALUE 5u
#define SCENE_PANEL_LIST 8u             /* the touch row of the chooser; 1, 2, 3, 5 and 6 are taken by other modules */
#define SCENE_LEGACY 0xfeu              /* "no chooser": the one scene of the configuration */

static const struct rx3_services *framework;
static scene_execute_fn scene_original;
static struct installed_hook scene_hook;
static volatile unsigned int scene_active;
static struct scene_settings scene_set;
static struct scene_settings scene_cur;          /* what this block plays */
static const unsigned char scene_owner;
static unsigned int scene_input_registered, scene_rows_registered;

/* The chooser. Its state changes on the input thread (the selector key), the
   touch thread (the buttons) and the render thread (the fade), each for a few
   instructions under one spin lock; the audio thread reads two published words. */
static struct fxs scene_fxs;
static volatile unsigned int scene_lock;
static volatile unsigned int scene_effect = SCENE_LEGACY;   /* entry played now, published */
static volatile unsigned int scene_fx_on;                    /* the audio thread: the effect is sounding */
static unsigned int scene_last_effect = 0xffu;               /* audio thread only */

/* The audio thread only, after start(). */
static struct rmxf_state scene_state[2];
static struct rmxf_coef scene_coef;
static double scene_position;          /* smoothed step; negative = idle, jump to the target */
static double scene_coef_step = -1.0;
static double scene_echo_len;          /* smoothed delay, frames; 0 = not started */
static unsigned int scene_echo_pos;
static unsigned int scene_echo_dirty;
static float scene_echo[2][SCENE_ECHO_FRAMES];
static float scene_scratch[SCENE_CHUNK * 2u];
/* Level keeping: running mean squares of the signal before and after the filter, and the
   make-up gain that brings the filtered sound back to the level of the dry one. */
static double scene_ms_in, scene_ms_filtered, scene_makeup = 1.0, scene_makeup_goal = 1.0;
static unsigned int scene_blocks_told;

#define SCENE_LEVEL_SECONDS 0.15       /* window of the level measurement */
#define SCENE_MAKEUP_MAX 4.0           /* +12 dB at most: a band-pass that narrow is the effect itself */
#define SCENE_KNEE 0.90f

/* The other SCENE FX (original work, plain textbook blocks, tuned by ear on the host only):
 *  CRUSH   the knob lowers the bit depth (15 to 4 bits) and the sample rate (hold 1 to 6 frames)
 *          of what goes into the echo and of what is heard with it.
 *  SPIRAL  the echo repeats and each repeat is shifted in pitch, up or down by a step the knob picks
 *          (1 to 12 semitones): two crossfaded read taps of a short ring buffer.
 *  REVERB  four combs and two all-passes per side (the classic Schroeder layout); the knob moves a
 *          high-pass (UP) or a low-pass (DOWN) over the tail, the same curves as the filters. */
#define SCENE_PS_LEN 2048u
#define SCENE_PS_MASK (SCENE_PS_LEN - 1u)
#define SCENE_PS_WINDOW 1024.0
#define REV_COMBS 4u
#define REV_COMB_MAX 1400u
#define REV_AP_MAX 600u
#define REV_SPREAD 23u
#define REV_FEEDBACK 0.84f
#define REV_DAMP 0.2f
#define REV_INPUT 0.015f
#define REV_WET 3.0f
static const unsigned scene_rev_comb_len[REV_COMBS] = {1116u, 1188u, 1277u, 1356u};
static const unsigned scene_rev_ap_len[2] = {556u, 441u};
/* 2^(n/12), n = 1..12: the pitch step of a spiral repeat. */
static const double scene_semitone[12] = {1.0594630943592953, 1.122462048309373, 1.189207115002721,
    1.2599210498948732, 1.3348398541700344, 1.4142135623730951, 1.4983070768766815, 1.5874010519681994,
    1.681792830507429, 1.7817974362806785, 1.887748625363387, 2.0};

static float scene_ps[2][SCENE_PS_LEN];
static unsigned int scene_ps_write;
static double scene_ps_phase;
static unsigned int scene_hold_count;
static float scene_hold[2];
static float scene_rev_comb[2][REV_COMBS][REV_COMB_MAX], scene_rev_lp[2][REV_COMBS];
static unsigned int scene_rev_ci[2][REV_COMBS];
static float scene_rev_ap[2][2][REV_AP_MAX];
static unsigned int scene_rev_ai[2][2];
static struct rmxf_state scene_rev_filter[2];
static unsigned int scene_fx_dirty, scene_noise;

static void scene_clear_fx(void)
{
    memset(scene_ps, 0, sizeof(scene_ps));
    memset(scene_rev_comb, 0, sizeof(scene_rev_comb));
    memset(scene_rev_lp, 0, sizeof(scene_rev_lp));
    memset(scene_rev_ci, 0, sizeof(scene_rev_ci));
    memset(scene_rev_ap, 0, sizeof(scene_rev_ap));
    memset(scene_rev_ai, 0, sizeof(scene_rev_ai));
    memset(scene_rev_filter, 0, sizeof(scene_rev_filter));
    scene_ps_write = 0u; scene_ps_phase = 0.0; scene_hold_count = 0u;
    scene_hold[0] = scene_hold[1] = 0.0f;
    scene_fx_dirty = 0u;
}

/* Bit and sample-rate reduction. `amount` 0..1; the held value is renewed every `hold` frames. */
static float scene_crush(float x, unsigned int channel, unsigned int fresh, unsigned int bits)
{
    if (fresh) {
        float levels = (float)(1u << (bits - 1u));
        float v = x * levels;
        v += v < 0.0f ? -0.5f : 0.5f;
        float q = (float)(int)v / levels;
        scene_hold[channel] = q;
    }
    return scene_hold[channel];
}

/* One read tap of the pitch ring buffer, `delay` frames behind the write position. */
static float scene_ps_tap(unsigned int channel, double delay)
{
    double at = (double)scene_ps_write - delay;
    if (at < 0.0) at += (double)SCENE_PS_LEN;
    unsigned int i0 = (unsigned int)at & SCENE_PS_MASK;
    unsigned int i1 = (i0 + 1u) & SCENE_PS_MASK;
    float frac = (float)(at - (double)(unsigned int)at);
    return scene_ps[channel][i0] + (scene_ps[channel][i1] - scene_ps[channel][i0]) * frac;
}

/* Shift a stereo frame in pitch by `ratio` (>1 up). Two taps half a window apart, crossfaded with a
   triangle so the jump of a tap, where its gain is zero, is never heard. */
static void scene_pitch(float *l, float *r, double ratio)
{
    scene_ps[0][scene_ps_write] = *l;
    scene_ps[1][scene_ps_write] = *r;
    double pa = scene_ps_phase, pb = pa + 0.5;
    if (pb >= 1.0) pb -= 1.0;
    double ga = 1.0 - (pa < 0.5 ? 1.0 - 2.0 * pa : 2.0 * pa - 1.0);
    double gb = 1.0 - (pb < 0.5 ? 1.0 - 2.0 * pb : 2.0 * pb - 1.0);
    double da = 2.0 + pa * SCENE_PS_WINDOW, db = 2.0 + pb * SCENE_PS_WINDOW;
    *l = (float)(ga * scene_ps_tap(0, da) + gb * scene_ps_tap(0, db));
    *r = (float)(ga * scene_ps_tap(1, da) + gb * scene_ps_tap(1, db));
    scene_ps_write = (scene_ps_write + 1u) & SCENE_PS_MASK;
    scene_ps_phase += (1.0 - ratio) / SCENE_PS_WINDOW;
    if (scene_ps_phase < 0.0) scene_ps_phase += 1.0;
    if (scene_ps_phase >= 1.0) scene_ps_phase -= 1.0;
}

/* The reverb of one side. `x` is the mono input already scaled. */
static float scene_reverb_side(unsigned int c, float x)
{
    float sum = 0.0f;
    for (unsigned int k = 0; k < REV_COMBS; k++) {
        unsigned int len = scene_rev_comb_len[k] + (c ? REV_SPREAD : 0u);
        unsigned int i = scene_rev_ci[c][k];
        float y = scene_rev_comb[c][k][i];
        float lp = y * (1.0f - REV_DAMP) + scene_rev_lp[c][k] * REV_DAMP;
        scene_rev_lp[c][k] = lp;
        scene_rev_comb[c][k][i] = x + lp * REV_FEEDBACK;
        if (++i >= len) i = 0u;
        scene_rev_ci[c][k] = i;
        sum += y;
    }
    for (unsigned int a = 0; a < 2u; a++) {
        unsigned int len = scene_rev_ap_len[a] + (c ? REV_SPREAD : 0u);
        unsigned int i = scene_rev_ai[c][a];
        float buf = scene_rev_ap[c][a][i];
        float out = buf - sum;
        scene_rev_ap[c][a][i] = sum + buf * 0.5f;
        if (++i >= len) i = 0u;
        scene_rev_ai[c][a] = i;
        sum = out;
    }
    return sum;
}

/* Safety limiter, 0 dBFS, transparent: the dry sample passes untouched, and only what the effect
   adds beyond max(knee, dry) is bent toward full scale (a rational curve, no libm). Never lowers
   a signal that stays under the knee. NaN is taken to silence. */
static float scene_limit(float x, float dry)
{
    if (!(x == x)) return 0.0f;
    float a = x < 0.0f ? -x : x, d = dry < 0.0f ? -dry : dry;
    float knee = d > SCENE_KNEE ? d : SCENE_KNEE;
    if (a <= knee) return x;
    if (knee >= 0.9999f) return x < 0.0f ? -knee : knee;     /* the dry sample is already at full scale */
    float over = (a - knee) * (1.0f / (1.0f - knee));
    if (over > 1.0e6f) over = 1.0e6f;
    float y = knee + (1.0f - knee) * over / (1.0f + over);
    return x < 0.0f ? -y : y;
}

static unsigned int scene_number(const char *text, unsigned int fallback, unsigned int limit)
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

/* "hpf", "lpf" or "bpf": the first letter decides, anything else is the HPF. */
static unsigned int scene_parse_type(const char *text)
{
    if (text && text[0] == 'l') return RMXF_LPF;
    if (text && text[0] == 'b') return RMXF_BPF;
    return RMXF_HPF;
}

static void scene_load_settings(struct scene_settings *set)
{
    set->type = scene_parse_type(getenv("RX3_RMX_SCENE_TYPE"));
    set->echo = scene_number(getenv("RX3_RMX_SCENE_ECHO"), 0u, 1u);
    set->feedback = scene_number(getenv("RX3_RMX_SCENE_FEEDBACK"), SCENE_DEFAULT_FEEDBACK,
                                 SCENE_MAX_FEEDBACK);
    set->level = scene_number(getenv("RX3_RMX_SCENE_LEVEL"), SCENE_DEFAULT_LEVEL, 100u);
    set->filter = 1u;
    set->selector = scene_number(getenv("RX3_RMX_SCENE_SELECTOR"), 1u, 1u);
    set->mode = SCENE_FILTER;
}

static void scene_load_chooser(struct fxs_config *cfg)
{
    fxs_defaults(cfg);
    const char *text = getenv("RX3_RMX_SCENE_START");
    unsigned int entry = fxs_find(text);
    if (entry != FXS_NONE && fxs_registry[entry].written) cfg->start = (unsigned char)entry;
    text = getenv("RX3_RMX_SCENE_POLICY");
    if (text && text[0] == 'k') cfg->reset = 0u;
    text = getenv("RX3_RMX_SCENE_SWITCH");
    if (text && text[0] == 'o') cfg->immediate = 0u;
    cfg->autoopen = (unsigned char)scene_number(getenv("RX3_RMX_SCENE_AUTOOPEN"), 1u, 1u);
}

static void scene_lock_take(void)
{
    while (__atomic_exchange_n(&scene_lock, 1u, __ATOMIC_ACQUIRE)) { }
}

static void scene_lock_drop(void)
{
    __atomic_store_n(&scene_lock, 0u, __ATOMIC_RELEASE);
}

/* The settings of an entry of the chooser; the filter kind comes from its name. */
static void scene_effect_settings(unsigned int effect, struct scene_settings *out)
{
    *out = scene_set;
    out->filter = 1u; out->echo = 1u; out->mode = SCENE_FILTER;
    if (effect == FXS_LPF_ECHO) out->type = RMXF_LPF;
    else if (effect == FXS_BPF_ECHO) out->type = RMXF_BPF;
    else if (effect == FXS_HPF_ECHO) out->type = RMXF_HPF;
    else if (effect == FXS_CRUSH_ECHO) { out->filter = 0u; out->mode = SCENE_CRUSH; }
    else if (effect == FXS_SPIRAL_UP) { out->filter = 0u; out->mode = SCENE_SPIRAL_UP; }
    else if (effect == FXS_SPIRAL_DOWN) { out->filter = 0u; out->mode = SCENE_SPIRAL_DOWN; }
    else if (effect == FXS_REVERB_UP) { out->filter = 0u; out->echo = 0u; out->mode = SCENE_REVERB_UP; out->type = RMXF_HPF; }
    else if (effect == FXS_REVERB_DOWN) { out->filter = 0u; out->echo = 0u; out->mode = SCENE_REVERB_DOWN; out->type = RMXF_LPF; }
}

/* Under the lock: what the audio thread plays follows the state. */
static void scene_publish(void)
{
    __atomic_store_n(&scene_effect, (unsigned int)fxs_active(&scene_fxs), __ATOMIC_SEQ_CST);
}

/* After the lock is dropped: the open and close requests the state made. */
static void scene_apply_request(void)
{
    scene_lock_take();
    unsigned int request = fxs_take_request(&scene_fxs);
    scene_lock_drop();
    if (!framework || !framework->panels) return;
    if (request == FXS_REQ_OPEN && framework->panels->open)
        (void)framework->panels->open(SCENE_PANEL_LIST);
    else if (request == FXS_REQ_CLOSE && framework->panels->close)
        (void)framework->panels->close();
}

/* Input thread. The selector key is only read: it always goes on to the player. */
static int scene_key(const struct rx3_key_event *event)
{
    if (event->key != SCENE_KEY_TYPE || event->channel != 0u || event->operation != SCENE_OP_VALUE)
        return 0;
    scene_lock_take();
    fxs_selector(&scene_fxs, event->raw);
    scene_publish();
    scene_lock_drop();
    scene_apply_request();
    return 0;
}

static float scene_smoother(const uint8_t *object, unsigned int offset, int *present)
{
    const uint8_t *smoother = SCENE_POINTER_AT(object, offset);
    if (!smoother) { *present = 0; return 0.0f; }
    return *(const float *)(smoother + 0x10u);
}

static float scene_clamp01(float g)
{
    return g < 0.0f ? 0.0f : g > 1.0f ? 1.0f : g;
}

/* Nothing left to add: forget the filter and the echo so the next start is clean. */
static void scene_rest(void)
{
    scene_state[0].z1 = scene_state[0].z2 = scene_state[1].z1 = scene_state[1].z2 = 0.0;
    scene_ms_in = scene_ms_filtered = 0.0; scene_makeup = scene_makeup_goal = 1.0;
    scene_position = -1.0;
    scene_coef_step = -1.0;
    scene_echo_len = 0.0;
    scene_echo_pos = 0u;
    if (scene_echo_dirty) {
        memset(scene_echo, 0, sizeof(scene_echo));
        scene_echo_dirty = 0u;
    }
    if (scene_fx_dirty) scene_clear_fx();
}

/* Delay in frames for a cycle time, halved until the line holds it. */
static double scene_echo_target(unsigned int time_ms, double rate)
{
    if (time_ms < 10u) time_ms = 10u;
    if (time_ms > 4000u) time_ms = 4000u;
    double frames = (double)time_ms * 0.001 * rate;
    while (frames > (double)(SCENE_ECHO_FRAMES - 8u)) frames *= 0.5;
    return frames < 1.0 ? 1.0 : frames;
}

/* One chunk of the effect, `count` <= SCENE_CHUNK frames: `src` in, `dst` out,
   the amount of effect going from g0 to g1 across the chunk. `target_step` is
   the knob in steps (0..255) and `time_ms` the echo time. The scene is added
   by interpolating between the dry and the processed signal, so the stock
   fade makes ON and OFF smooth. */
static void scene_process(const float *src, float *dst, unsigned int count, double rate,
                          double target_step, unsigned int time_ms, float g0, float g1)
{
    const double glide = (double)SCENE_SUB / (SCENE_SMOOTH_SECONDS * rate);
    const double slew = SCENE_ECHO_SLEW;
    const float feedback = (float)scene_set.feedback * 0.01f;
    const float level = (float)scene_set.level * 0.01f;
    const double echo_goal = scene_cur.echo ? scene_echo_target(time_ms, rate) : 0.0;
    float gain = g0;
    const float gain_step = (g1 - g0) / (float)count;
    if (scene_position < 0.0) scene_position = target_step;      /* each start at the knob */
    if (scene_cur.echo && scene_echo_len <= 0.0) scene_echo_len = echo_goal;
    const double ms_a = 1.0 / (SCENE_LEVEL_SECONDS * rate);
    /* The knob (0..1) as the amount of the effects that have no filter curve of their own. */
    const double amount = target_step / (double)RMXF_TOP;
    unsigned int crush_bits = 15u - (unsigned int)(amount * 11.99);
    unsigned int crush_hold = 1u + (unsigned int)(amount * 5.99);
    double spiral_ratio = scene_semitone[(unsigned int)(amount * 11.99)];
    if (scene_cur.mode == SCENE_SPIRAL_DOWN) spiral_ratio = 1.0 / spiral_ratio;
    for (unsigned int base = 0; base < count; base += SCENE_SUB) {
        unsigned int n = count - base < SCENE_SUB ? count - base : SCENE_SUB;
        if (scene_cur.filter) {
            /* The make-up goal is sqrt(dry power / filtered power), at least 1: the filter never
               makes the sound louder than it came in, and what it takes away is given back up to +12 dB.
               Newton steps, no libm; the gain itself glides, so it never modulates the sound. */
            double ratio = scene_ms_filtered > 1e-9 ? scene_ms_in / scene_ms_filtered : 1.0;
            if (ratio > SCENE_MAKEUP_MAX * SCENE_MAKEUP_MAX) ratio = SCENE_MAKEUP_MAX * SCENE_MAKEUP_MAX;
            for (unsigned int k = 0; k < 2u; k++) scene_makeup_goal = 0.5 * (scene_makeup_goal + ratio / scene_makeup_goal);
            if (scene_makeup_goal < 1.0) scene_makeup_goal = 1.0;
            scene_makeup += (scene_makeup_goal - scene_makeup) * 0.004;
        }
        scene_position += (target_step - scene_position) * (glide < 1.0 ? glide : 1.0);
        if (scene_position < 0.0) scene_position = 0.0;
        double moved = scene_position - scene_coef_step;
        if (scene_coef_step < 0.0 || moved > 0.002 || moved < -0.002) {
            scene_coef = rmxf_step_design(scene_cur.type, scene_position, rate);
            scene_coef_step = scene_position;
        }
        for (unsigned int i = base; i < base + n; i++) {
            float l = src[i * 2u], r = src[i * 2u + 1u];
            float fl = l, fr = r;
            if (scene_cur.filter) {
                fl = (float)rmxf_tick(&scene_coef, &scene_state[0], (double)l);
                fr = (float)rmxf_tick(&scene_coef, &scene_state[1], (double)r);
                scene_ms_in += ((double)l * l + (double)r * r) * 0.5 * ms_a - scene_ms_in * ms_a;
                scene_ms_filtered += ((double)fl * fl + (double)fr * fr) * 0.5 * ms_a - scene_ms_filtered * ms_a;
                fl *= (float)scene_makeup; fr *= (float)scene_makeup;
            }
            float pl = fl, pr = fr;
            const unsigned int mode = scene_cur.mode;
            if (mode == SCENE_REVERB_UP || mode == SCENE_REVERB_DOWN) {
                float in = (fl + fr) * REV_INPUT;
                scene_noise++;
                in += (scene_noise & 1u) ? 1.0e-15f : -1.0e-15f;           /* keeps the tail out of denormals */
                float wl = scene_reverb_side(0u, in) * REV_WET, wr = scene_reverb_side(1u, in) * REV_WET;
                wl = (float)rmxf_tick(&scene_coef, &scene_rev_filter[0], (double)wl);
                wr = (float)rmxf_tick(&scene_coef, &scene_rev_filter[1], (double)wr);
                scene_fx_dirty = 1u;
                pl = fl + wl; pr = fr + wr;
            } else if (scene_cur.echo) {
                float el = fl, er = fr;                                    /* what goes into the line */
                if (mode == SCENE_CRUSH) {
                    unsigned int fresh = scene_hold_count == 0u;
                    el = scene_crush(fl, 0u, fresh, crush_bits);
                    er = scene_crush(fr, 1u, fresh, crush_bits);
                    if (++scene_hold_count >= crush_hold) scene_hold_count = 0u;
                    fl = el; fr = er; pl = el; pr = er;                    /* the crushed sound is the one heard */
                    scene_fx_dirty = 1u;
                }
                scene_echo_len += (echo_goal - scene_echo_len) * slew;
                double at = (double)scene_echo_pos - scene_echo_len;
                if (at < 0.0) at += (double)SCENE_ECHO_FRAMES;
                unsigned int i0 = (unsigned int)at & SCENE_ECHO_MASK;
                unsigned int i1 = (i0 + 1u) & SCENE_ECHO_MASK;
                float frac = (float)(at - (double)(unsigned int)at);
                float dl = scene_echo[0][i0] + (scene_echo[0][i1] - scene_echo[0][i0]) * frac;
                float dr = scene_echo[1][i0] + (scene_echo[1][i1] - scene_echo[1][i0]) * frac;
                float wl = fl + feedback * dl, wr = fr + feedback * dr;
                if (mode == SCENE_SPIRAL_UP || mode == SCENE_SPIRAL_DOWN) {
                    scene_pitch(&wl, &wr, spiral_ratio);                    /* every repeat a step higher or lower */
                    scene_fx_dirty = 1u;
                }
                scene_echo[0][scene_echo_pos] = wl;
                scene_echo[1][scene_echo_pos] = wr;
                scene_echo_pos = (scene_echo_pos + 1u) & SCENE_ECHO_MASK;
                scene_echo_dirty = 1u;
                pl = pl + level * dl;
                pr = pr + level * dr;
            }
            gain += gain_step;
            dst[i * 2u] = scene_limit(l + gain * (pl - l), l);
            dst[i * 2u + 1u] = scene_limit(r + gain * (pr - r), r);
        }
    }
    /* A decaying state never reaches zero by itself; flush it before it
       becomes denormal and slows the audio thread. */
    for (unsigned int c = 0; c < 2u; c++) {
        if (scene_state[c].z1 < 1e-24 && scene_state[c].z1 > -1e-24) scene_state[c].z1 = 0.0;
        if (scene_state[c].z2 < 1e-24 && scene_state[c].z2 > -1e-24) scene_state[c].z2 = 0.0;
    }
}

static void scene_execute(void *effect, const float *in, float *out, int frames)
{
    __atomic_add_fetch(&scene_active, 1u, __ATOMIC_SEQ_CST);
    const uint8_t *object = (const uint8_t *)effect;
    int present = 1;
    float g_start = 0.0f, g_end = 0.0f;
    if (object && in && out && frames > 0) {
        /* Probe once: a missing smoother leaves the block to the stock effect. */
        (void)scene_smoother(object, 0x74u, &present);
        (void)scene_smoother(object, 0x78u, &present);
    }
    if (!object || !in || !out || frames <= 0 || !present) {
        if (scene_original) scene_original(effect, in, out, frames);
        __atomic_sub_fetch(&scene_active, 1u, __ATOMIC_SEQ_CST);
        return;
    }
    double rate = (double)*(const uint32_t *)(object + 4u);
    float depth = *(const float *)(object + 0x20u);
    unsigned int time_ms = *(const uint32_t *)(object + 0x8cu);
    if (rate < 8000.0 || rate > 200000.0) rate = 44100.0;
    if (!(depth > 0.0f)) depth = 0.0f;
    if (depth > 1.0f) depth = 1.0f;
    double target = (double)depth * (double)RMXF_TOP;
    unsigned int entry = __atomic_load_n(&scene_effect, __ATOMIC_SEQ_CST);
    if (entry == FXS_HELIX) {
        /* HELIX is the player's own effect: the stock code, untouched. The fade
           is read only to tell the chooser whether the effect is sounding. */
        scene_original(effect, in, out, frames);
        int unused = 1;
        float g = scene_clamp01(scene_smoother(object, 0x74u, &unused) * scene_smoother(object, 0x78u, &unused));
        __atomic_store_n(&scene_fx_on, g > 0.0f, __ATOMIC_SEQ_CST);
        if (scene_last_effect != entry) { scene_rest(); scene_last_effect = entry; }
        __atomic_sub_fetch(&scene_active, 1u, __ATOMIC_SEQ_CST);
        return;
    }
    if (entry != scene_last_effect) {
        /* Another effect: its filter starts clean; the echo line keeps what it holds. */
        scene_state[0].z1 = scene_state[0].z2 = scene_state[1].z1 = scene_state[1].z2 = 0.0;
        scene_ms_in = scene_ms_filtered = 0.0; scene_makeup = scene_makeup_goal = 1.0;
        scene_position = -1.0;
        scene_coef_step = -1.0;
        scene_clear_fx();
        scene_last_effect = entry;
    }
    if (entry == SCENE_LEGACY) scene_cur = scene_set;
    else scene_effect_settings(entry, &scene_cur);
    for (unsigned int done = 0; done < (unsigned int)frames;) {
        unsigned int count = (unsigned int)frames - done;
        if (count > SCENE_CHUNK) count = SCENE_CHUNK;
        const float *src = in + done * 2u;
        float *dst = out + done * 2u;
        int unused = 1;
        g_start = scene_clamp01(scene_smoother(object, 0x74u, &unused) *
                                scene_smoother(object, 0x78u, &unused));
        scene_original(effect, src, scene_scratch, (int)count);        /* keeps the stock bookkeeping */
        g_end = scene_clamp01(scene_smoother(object, 0x74u, &unused) *
                              scene_smoother(object, 0x78u, &unused));
        __atomic_store_n(&scene_fx_on, g_start > 0.0f || g_end > 0.0f, __ATOMIC_SEQ_CST);
        if (g_start <= 0.0f && g_end <= 0.0f) {
            if (dst != src) memcpy(dst, src, count * 2u * sizeof(float));
            scene_rest();
        } else {
            scene_process(src, dst, count, rate, target, time_ms, g_start, g_end);
        }
        if (scene_blocks_told < 12u && (g_start > 0.0f || g_end > 0.0f)) {
            scene_blocks_told++;
            framework->log_number("rmx1000-scene: block (rate Hz) = ", (unsigned long)rate);
            framework->log_number("rmx1000-scene: time ms = ", time_ms);
            framework->log_number("rmx1000-scene: depth x 1000 = ", (unsigned long)(depth * 1000.0f));
            framework->log_number("rmx1000-scene: fade x 1000, start and end = ",
                                  (unsigned long)(g_start * 1000.0f) * 10000ul +
                                  (unsigned long)(g_end * 1000.0f));
        }
        done += count;
    }
    __atomic_sub_fetch(&scene_active, 1u, __ATOMIC_SEQ_CST);
}

/* ---- the touch row of the chooser (render and touch threads) ---------------- */

static uint16_t scene_caption_text[FXS_WIDGETS][FXS_CAPTION_MAX];

static const uint16_t *scene_row_caption(unsigned int d, unsigned int widget, unsigned int part)
{
    (void)d; (void)part;
    if (widget >= FXS_WIDGETS) widget = 0;
    scene_lock_take();
    (void)fxs_caption(&scene_fxs, widget, scene_caption_text[widget]);
    scene_lock_drop();
    return scene_caption_text[widget];
}

static unsigned int scene_row_kind(unsigned int d, unsigned int widget)
{
    (void)d;
    return fxs_kind(widget);
}

static int scene_row_is_on(unsigned int d, unsigned int widget, unsigned int part)
{
    (void)d; (void)part;
    scene_lock_take();
    int on = fxs_is_on(&scene_fxs, widget);
    scene_lock_drop();
    return on;
}

static unsigned int scene_row_live(unsigned int d)
{
    (void)d;
    return FXS_WIDGETS;
}

static void scene_row_fire(unsigned int d, unsigned int widget, unsigned int part)
{
    (void)d; (void)part;
    scene_lock_take();
    fxs_fire(&scene_fxs, widget);
    scene_publish();
    scene_lock_drop();
    scene_apply_request();
}

static uint16_t scene_row_colour(unsigned int d, unsigned int widget)
{
    (void)d;
    scene_lock_take();
    unsigned int c = fxs_colour(&scene_fxs, widget);
    scene_lock_drop();
    return (uint16_t)(c ? c : RX3_PAL_ACCENT_DEFAULT_565);
}

static void scene_row_activate(unsigned int active)
{
    scene_lock_take();
    fxs_activate(&scene_fxs, active);
    scene_lock_drop();
}

/* Render thread, every frame the row is up: tells the chooser whether the effect
   sounds (a choice that waited takes over at OFF) and whether to repaint. */
static int scene_refresh(void)
{
    static unsigned int seen = 0xffffffffu;
    scene_lock_take();
    fxs_set_on(&scene_fxs, __atomic_load_n(&scene_fx_on, __ATOMIC_SEQ_CST));
    scene_publish();
    unsigned int now = scene_fxs.generation;
    scene_lock_drop();
    int changed = seen != now;
    seen = now;
    return changed;
}

/* One share per weight of rx3_rmx_fxsel.h: the effects, then the X (a row holds eight widgets at most). */
static const struct rx3_pad_widget scene_row_widgets[FXS_WIDGETS] = {
    { RX3_PAD_TOGGLE, 1 }, { RX3_PAD_TOGGLE, 1 }, { RX3_PAD_TOGGLE, 1 }, { RX3_PAD_TOGGLE, 1 },
    { RX3_PAD_TOGGLE, 1 }, { RX3_PAD_TOGGLE, 1 }, { RX3_PAD_TOGGLE, 1 }, { RX3_PAD_BUTTON, 1 }
};

static const struct rx3_pad_row scene_list_row = {
    .panel_id = SCENE_PANEL_LIST, .tab_image = 0u, .scope = RX3_PAD_SCOPE_SCREEN,
    .count = FXS_WIDGETS, .widgets = scene_row_widgets,
    .live_count = scene_row_live, .caption = scene_row_caption, .is_on = scene_row_is_on,
    .fire = scene_row_fire, .needs_refresh = scene_refresh, .kind = scene_row_kind, .colour = scene_row_colour,
    .activate = scene_row_activate
};

/* The chooser needs the key and panel services; without them HELIX plays the
   one scene of the configuration, as before. Returns 1 when it is on. */
static int scene_chooser_start(const struct rx3_services *services)
{
    scene_input_registered = scene_rows_registered = 0u;
    __atomic_store_n(&scene_effect, SCENE_LEGACY, __ATOMIC_SEQ_CST);
    __atomic_store_n(&scene_fx_on, 0u, __ATOMIC_SEQ_CST);
    scene_last_effect = 0xffu;
    if (!scene_set.selector) return 0;
    if (!services->input || !services->input->register_key || !services->panels ||
        !services->panels->register_row) {
        services->log_line("rmx1000-scene: chooser services missing, the one scene plays");
        return 0;
    }
    struct fxs_config cfg;
    scene_load_chooser(&cfg);
    fxs_init(&scene_fxs, &cfg);
    scene_publish();
    if (!services->panels->register_row(&scene_list_row)) {
        services->log_line("rmx1000-scene: touch row refused (panel 8 taken), the one scene plays");
        if (services->panels->unregister_row) services->panels->unregister_row(&scene_list_row);
        __atomic_store_n(&scene_effect, SCENE_LEGACY, __ATOMIC_SEQ_CST);
        return 0;
    }
    scene_rows_registered = 1u;
    if (!services->input->register_key(&scene_owner, 25u, scene_key)) {
        services->log_line("rmx1000-scene: key handler refused, the one scene plays");
        services->panels->unregister_row(&scene_list_row);
        scene_rows_registered = 0u;
        __atomic_store_n(&scene_effect, SCENE_LEGACY, __ATOMIC_SEQ_CST);
        return 0;
    }
    scene_input_registered = 1u;
    services->log_number("rmx1000-scene: chooser on, start entry = ", cfg.start);
    return 1;
}

static void scene_chooser_stop(void)
{
    if (scene_input_registered && framework && framework->input && framework->input->unregister_owner)
        framework->input->unregister_owner(&scene_owner);
    scene_input_registered = 0u;
    if (scene_rows_registered && framework && framework->panels && framework->panels->unregister_row)
        framework->panels->unregister_row(&scene_list_row);
    scene_rows_registered = 0u;
}

static int configured(void)
{
    const char *value = getenv("RX3_RMX_SCENE");
    return value && value[0] == '1';
}

static int start(const struct rx3_services *services)
{
    framework = services;
    memset(scene_state, 0, sizeof(scene_state));
    scene_echo_dirty = 1u;           /* a restart clears the line once */
    scene_rest();
    scene_blocks_told = 0u;
    scene_load_settings(&scene_set);
    if (!services->install_hook) {
        services->log_line("rmx1000-scene: services missing, HELIX stays native");
        return 0;
    }
    scene_original = (scene_execute_fn)services->install_hook(&scene_hook, SCENE_EXECUTE,
                                                              scene_guard, scene_execute);
    if (!scene_original) {
        services->log_line("rmx1000-scene: BeatEffectHelix::execute guard rejected, HELIX stays native");
        return 0;
    }
    services->log_number("rmx1000-scene: type (0 HPF, 1 LPF, 2 BPF) = ", scene_set.type);
    services->log_number("rmx1000-scene: echo = ", scene_set.echo);
    if (scene_chooser_start(services))
        services->log_line("rmx1000-scene: BEAT FX position 4 plays the effect chosen on the screen (HELIX by default)");
    else
        services->log_line("rmx1000-scene: BEAT FX position 4 (was HELIX) plays the scene");
    return 1;
}

/* Must also accept a partially completed start. */
static void stop(void)
{
    scene_chooser_stop();
    if (scene_original && framework->detach_hook && framework->detach_hook(&scene_hook)) {
        while (__atomic_load_n(&scene_active, __ATOMIC_SEQ_CST)) usleep(1000u);
        usleep(10000u);
        framework->release_hook(&scene_hook);
    }
    scene_original = 0;
}

const struct rx3_module rx3_rmx1000_scene_module = {
    .version = RX3_MODULE_API_VERSION, .size = sizeof(struct rx3_module),
    .name = "rmx1000-scene", .configured = configured, .start = start, .stop = stop
};

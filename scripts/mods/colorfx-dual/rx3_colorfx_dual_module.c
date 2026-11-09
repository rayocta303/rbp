/* SPDX-License-Identifier: MPL-2.0 */
/* Up to six SOUND COLOR FX at once (rbp 1.20).
 *
 * Stock: a SOUND COLOR FX button turns its effect on and the other off.
 * Here the buttons are a set: a tap on an effect that is off adds it, a tap
 * on one that is on takes it away, a long tap (CFX_HOLD_ALL_MS) on one that
 * is on takes all of them away. The COLOR knob drives every effect.
 *
 *   on now      gesture                  result
 *   -           tap A                    A            (stock)
 *   A           tap B                    A + B        (B is ours)
 *   A + B       tap C                    A + B + C    (up to all six)
 *   A + B       tap B                    A
 *   A + B       tap A                    B            (handed to the stock engine)
 *   A + B + C   hold any lit button      -
 *
 * The stock engine plays one effect (the primary); the others are `extras`
 * played by this module. Taking the primary away hands the lowest remaining
 * effect to the stock engine.
 *
 * Every mixer channel has a SoundColorFxManager holding one instance of each
 * effect (+0x4 Off, +0x8 FILTER, +0xc NOISE, +0x10 SWEEP, +0x14 DUB ECHO,
 * +0x18 SPACE, +0x1c CRUSH: +4 + 4 * type), +0x0 the one playing, +0x20 its
 * type and +0x24 the type asked for (constructor @0xbb970, operate
 * @0xbbc68). An extra effect is the channel's own idle instance, run after
 * operate() on the same block and faded in and out over CFX_FADE frames.
 *
 * Effect vtable: +0x8 execute(in, out, frames), +0xc adjustParameter (7 the
 * COLOR, 8 the parameter, 0..1), +0x10 initialize, +0x20 status on.
 * Instance: +0x1c the COLOR, +0x20 the parameter.
 *
 * Buttons (SEND_KEY, operation 0 press, 2/3 release): 0x50a1 SPACE, 0x50a2
 * DUB ECHO, 0x50a3 SWEEP, 0x50a4 NOISE, 0x50a5 CRUSH, 0x50a6 FILTER, the
 * order of ui::MixerChInnards::onEv_ColorFxSW @0x2d3684, which maps them to
 * the types 5, 4, 3, 2, 6, 1.
 *
 * Lights: ui::Mixer::checkLedStat @0x2d0874 lights each SOUND COLOR FX
 * button from its own bit of the first channel status (Mixer+0xb8, written
 * by MixerChInnards::getStat @0x2d59f4): byte 8 bit 3 FILTER, 6 NOISE,
 * 4 SWEEP, 5 DUB ECHO, 7 SPACE, byte 9 bit 0 CRUSH. Before it runs, the six
 * bits are set to the stock type's and the extras'.
 *
 * Not done: the extra effects play where the first one does in the channel,
 * before or after the fader, whatever their own position. They run one after
 * the other, lowest type first.
 */
#include "../core/api/rx3_module_api.h"

#define CFX_KEY_FIRST 0x50a1u
#define CFX_BUTTONS 6u
#define CFX_TYPES 7u                   /* 0 off, 1..6 */
#define CFX_OPERATE ((unsigned long)0x000bbc68u)
#define CFX_GET_TYPE ((unsigned long)0x0004e37cu)
#define CFX_CHECK_LEDS ((unsigned long)0x002d0874u)
#define CFX_WORD_MIXER_STATUS 46u         /* Mixer+0xb8 */
#define CFX_CHUNK 512u
#define CFX_FADE 1024u                 /* frames, ~23 ms */
#define CFX_LANES 4u
#define CFX_HOLD_ALL_MS 700u            /* held this long: all off */
#define CFX_SETTLE_MS 500u              /* the player takes this long to follow a key */
#define CFX_PARAM_COLOR 7
#define CFX_PARAM_PARAMETER 8
/* Vtable slots and manager words, in pointers (4 bytes on the player). */
#define CFX_SLOT_EXECUTE 2u
#define CFX_SLOT_ADJUST 3u
#define CFX_SLOT_INITIALIZE 4u
#define CFX_SLOT_STATUS_ON 8u
#define CFX_WORD_TYPE_NOW 8u
#define CFX_WORD_TYPE_ASKED 9u

static const unsigned int cfx_button_type[CFX_BUTTONS] = {5u, 4u, 3u, 2u, 6u, 1u};
static const uint8_t cfx_operate_guard[8] = {0xf8, 0x45, 0x2d, 0xe9, 0x02, 0x8b, 0x2d, 0xed};
static const uint8_t cfx_type_guard[8] = {0xf8, 0x40, 0x2d, 0xe9, 0x01, 0x50, 0xa0, 0xe1};
static const uint8_t cfx_leds_guard[8] = {0xf0, 0x4f, 0x2d, 0xe9, 0x00, 0x50, 0xa0, 0xe1};
/* Per type 1..6: the status byte (8 or 9) and bit of its button light. */
static const uint8_t cfx_light_byte[CFX_TYPES] = {0, 8, 8, 8, 8, 8, 9};
static const uint8_t cfx_light_bit[CFX_TYPES] = {0, 0x08, 0x40, 0x10, 0x20, 0x80, 0x01};

typedef void (*cfx_operate_fn)(void *manager, float *buffer, int frames);
typedef void (*cfx_leds_fn)(void *mixer, void *led_stat);
typedef void (*cfx_execute_fn)(void *effect, const float *in, float *out, int frames);
typedef void (*cfx_adjust_fn)(void *effect, int which, float value);
typedef void (*cfx_call_fn)(void *effect);
typedef unsigned int (*cfx_type_fn)(void);

/* One mixer channel's extra effects, one slot per type; the audio thread only. */
struct cfx_lane {
    void *manager;
    unsigned int live;                 /* bit per type: started, in the channel */
    unsigned int mix[CFX_TYPES];       /* 0..CFX_FADE */
    float color[CFX_TYPES], parameter[CFX_TYPES];
};

struct cfx_timeval { long seconds, microseconds; };
static unsigned int cfx_now_ms(void)
{
    struct cfx_timeval value;
    if (gettimeofday(&value, 0)) return 0;
    return (unsigned int)(((uint64_t)(unsigned long)value.seconds * 1000000u +
                           (uint64_t)(unsigned long)value.microseconds) / 1000u);
}
static unsigned int (*cfx_clock)(void) = cfx_now_ms;

static const struct rx3_services *framework;
static const unsigned char cfx_owner;
static cfx_operate_fn cfx_original;
static struct installed_hook cfx_hook;
static cfx_leds_fn cfx_leds_original;
static struct installed_hook cfx_leds_hook;
static unsigned int cfx_registered;
/* Input thread writes, audio thread reads: the extras, a bit per type. */
static volatile unsigned int cfx_wanted;
static struct cfx_lane cfx_lanes[CFX_LANES];
static float cfx_wet[CFX_CHUNK * 2u];
/* Input thread only. */
static unsigned int cfx_taken;
static unsigned int cfx_sel;            /* effects on, a bit per type */
static unsigned int cfx_primary;        /* the one the stock engine plays, 0 none */
static unsigned int cfx_changed_ms;     /* when a key last went to the player */
static unsigned int cfx_pending;        /* buttons held that were on: act on release */
static unsigned int cfx_dirty;          /* the stock engine still has to follow cfx_primary */
static unsigned int cfx_retries;
static unsigned int cfx_channels_any;   /* channels any button was reported on */
static unsigned int cfx_pressed_ms[CFX_BUTTONS];
static struct { unsigned int channel, raw; float value; } cfx_last[CFX_BUTTONS];

/* The stock type on CH1: DjEngineIF::getSoundColorFxType ignores `this`
   and reads the mixer engine's singleton. */
static unsigned int cfx_native_type(void)
{
    return ((unsigned int (*)(void *, unsigned int))CFX_GET_TYPE)(0, 0u) & 0xffu;
}
static cfx_type_fn cfx_stock_type = cfx_native_type;

static void *cfx_vfunc(void *object, unsigned int slot)
{
    return (*(void *const *const *)object)[slot];
}

static void *cfx_effect(void *manager, unsigned int type)
{
    return ((void *const *)manager)[1u + type];
}

static unsigned int cfx_word(void *manager, unsigned int word)
{
    return *(const unsigned int *)((void *const *)manager + word);
}

static struct cfx_lane *cfx_lane(void *manager)
{
    for (unsigned int i = 0; i < CFX_LANES; i++)
        if (cfx_lanes[i].manager == manager) return &cfx_lanes[i];
    for (unsigned int i = 0; i < CFX_LANES; i++)
        if (!cfx_lanes[i].manager) { cfx_lanes[i].manager = manager; return &cfx_lanes[i]; }
    return 0;
}

/* The COLOR and parameter knobs reach only the stock instance: copy them. */
static void cfx_follow_knobs(struct cfx_lane *lane, unsigned int type, void *effect, int force)
{
    const uint8_t *playing = *(const uint8_t *const *)lane->manager;
    if (!playing) return;
    float color = *(const float *)(playing + 0x1c), parameter = *(const float *)(playing + 0x20);
    cfx_adjust_fn adjust = (cfx_adjust_fn)cfx_vfunc(effect, CFX_SLOT_ADJUST);
    if (force || color != lane->color[type]) { lane->color[type] = color; adjust(effect, CFX_PARAM_COLOR, color); }
    if (force || parameter != lane->parameter[type]) {
        lane->parameter[type] = parameter;
        adjust(effect, CFX_PARAM_PARAMETER, parameter);
    }
}

static void cfx_operate(void *manager, float *buffer, int frames)
{
    cfx_original(manager, buffer, frames);
    if (!manager || !buffer || frames <= 0) return;
    struct cfx_lane *lane = cfx_lane(manager);
    if (!lane) return;
    unsigned int wanted = __atomic_load_n(&cfx_wanted, __ATOMIC_RELAXED);
    unsigned int now = cfx_word(manager, CFX_WORD_TYPE_NOW), asked = cfx_word(manager, CFX_WORD_TYPE_ASKED);
    unsigned int goal[CFX_TYPES];
    for (unsigned int type = 1u; type < CFX_TYPES; type++) {
        unsigned int bit = 1u << type;
        /* The stock engine taking that effect over (A + B, tap A): it is the
           same instance, which must never run twice in a block. */
        int stock_has = now == type || asked == type;
        if ((lane->live & bit) && stock_has) { lane->live &= ~bit; lane->mix[type] = 0u; }
        /* Nothing under it (the stock effect went off another way): stop too. */
        goal[type] = (wanted & bit) && asked != 0u && !stock_has ? CFX_FADE : 0u;
        if (lane->live & bit) continue;
        if (!goal[type]) continue;
        void *effect = cfx_effect(manager, type);
        if (!effect) { goal[type] = 0u; continue; }
        cfx_follow_knobs(lane, type, effect, 1);
        ((cfx_call_fn)cfx_vfunc(effect, CFX_SLOT_INITIALIZE))(effect);
        ((cfx_call_fn)cfx_vfunc(effect, CFX_SLOT_STATUS_ON))(effect);
        lane->live |= bit;
        lane->mix[type] = 0u;
    }
    if (!lane->live) return;
    for (unsigned int type = 1u; type < CFX_TYPES; type++)
        if (lane->live & (1u << type)) cfx_follow_knobs(lane, type, cfx_effect(manager, type), 0);
    for (unsigned int done = 0; done < (unsigned int)frames;) {
        unsigned int count = (unsigned int)frames - done;
        if (count > CFX_CHUNK) count = CFX_CHUNK;
        float *dry = buffer + done * 2u;
        for (unsigned int type = 1u; type < CFX_TYPES; type++) {
            if (!(lane->live & (1u << type))) continue;
            void *effect = cfx_effect(manager, type);
            cfx_execute_fn execute = (cfx_execute_fn)cfx_vfunc(effect, CFX_SLOT_EXECUTE);
            execute(effect, dry, cfx_wet, (int)count);
            for (unsigned int i = 0; i < count; i++) {
                if (lane->mix[type] < goal[type]) lane->mix[type]++;
                else if (lane->mix[type] > goal[type]) lane->mix[type]--;
                float mix = (float)lane->mix[type] * (1.0f / (float)CFX_FADE);
                dry[i * 2u] += (cfx_wet[i * 2u] - dry[i * 2u]) * mix;
                dry[i * 2u + 1u] += (cfx_wet[i * 2u + 1u] - dry[i * 2u + 1u]) * mix;
            }
        }
        done += count;
    }
    for (unsigned int type = 1u; type < CFX_TYPES; type++)
        if ((lane->live & (1u << type)) && !goal[type] && !lane->mix[type]) lane->live &= ~(1u << type);
}

/* UI thread: every button lit whose effect plays. */
static void cfx_leds(void *mixer, void *led_stat)
{
    uint8_t *status = mixer ? ((uint8_t *const *)mixer)[CFX_WORD_MIXER_STATUS] : 0;
    if (status) {
        unsigned int stock = cfx_stock_type(), extras = __atomic_load_n(&cfx_wanted, __ATOMIC_RELAXED);
        for (unsigned int type = 1u; type < CFX_TYPES; type++)
            status[cfx_light_byte[type]] &= (uint8_t)~cfx_light_bit[type];
        if (stock < CFX_TYPES) status[cfx_light_byte[stock]] |= cfx_light_bit[stock];
        for (unsigned int type = 1u; stock && type < CFX_TYPES; type++)
            if (extras & (1u << type)) status[cfx_light_byte[type]] |= cfx_light_bit[type];
    }
    cfx_leds_original(mixer, led_stat);
    /* For the log, a little after each change: did the lights hook run, and
       are the extras playing on the channels (type x 10000 + fade)? */
    static unsigned int told, last = 0xffffffffu, countdown;
    unsigned int second = __atomic_load_n(&cfx_wanted, __ATOMIC_RELAXED);
    if (second != last) { last = second; countdown = 30u; }
    if (countdown && !--countdown && told < 40u) {
        told++;
        framework->log_number("colorfx-dual: lights, status byte 8 + 9 x 256 = ",
                              status ? (unsigned long)status[8] | (unsigned long)status[9] << 8 : 0xfffffu);
        for (unsigned int i = 0; i < CFX_LANES; i++)
            for (unsigned int type = 1u; cfx_lanes[i].manager && type < CFX_TYPES; type++)
                if (cfx_lanes[i].live & (1u << type))
                    framework->log_number("colorfx-dual: channel playing (type x 10000 + fade of 1024) = ",
                                          type * 10000u + cfx_lanes[i].mix[type]);
    }
}

static unsigned int cfx_button_of(unsigned int type)
{
    for (unsigned int i = 0; i < CFX_BUTTONS; i++)
        if (cfx_button_type[i] == type) return i;
    return CFX_BUTTONS;
}

/* The mixer reports a SOUND COLOR FX button once per channel it serves
   (0x50xx are channel keys; the player toggles each channel's effect on its
   own press). The first press of a button decides; the same press from the
   other channel follows that decision, or a press seen twice would be undone
   by its own second report (RX3, 17/10: only A blinked). */
#define CFX_CHANNELS 4u
static unsigned int cfx_down[CFX_CHANNELS];      /* per channel: buttons held */
static unsigned int cfx_seen[CFX_BUTTONS];       /* channels that reported it */

static unsigned int cfx_any_down(void)
{
    unsigned int all = 0u;
    for (unsigned int c = 0; c < CFX_CHANNELS; c++) all |= cfx_down[c];
    return all;
}

/* The extras the audio thread plays: what is on, minus the stock engine's. */
static void cfx_publish(void)
{
    unsigned int extras = cfx_primary ? cfx_sel & ~(1u << cfx_primary) : 0u;
    if (__atomic_exchange_n(&cfx_wanted, extras, __ATOMIC_SEQ_CST) != extras)
        framework->log_number("colorfx-dual: extra effects (bit per type) = ", extras);
}

/* As if the button of `type` had been pressed alone, on every channel it was
   seen on, in the order the mixer reports them (all presses, then all
   releases): the stock engine switches to it, or off if it is the one playing. */
static void cfx_press_for_player(unsigned int type)
{
    unsigned int b = cfx_button_of(type);
    if (b >= CFX_BUTTONS) return;
    unsigned int channels = cfx_seen[b] ? cfx_seen[b] : cfx_channels_any ? cfx_channels_any : 1u << cfx_last[b].channel;
    for (unsigned int operation = 0u; operation <= 2u; operation += 2u)
        for (unsigned int c = 0; c < CFX_CHANNELS; c++)
            if (channels & (1u << c))
                framework->input->send_key(CFX_KEY_FIRST + b, operation, c, cfx_last[b].raw, cfx_last[b].value);
    cfx_changed_ms = cfx_clock();
}

/* The stock engine follows what the buttons decided with one key, once no
   SOUND COLOR FX button is held: a key sent while the DJ still holds that
   button was ignored (RX3 27/10: two buttons released together left one on). */
static void cfx_sync_player(void)
{
    if (!cfx_dirty || cfx_any_down()) return;
    unsigned int stock = cfx_stock_type();
    if (stock >= CFX_TYPES) stock = 0u;
    if (stock == cfx_primary) { cfx_dirty = 0u; cfx_retries = 0u; return; }
    cfx_press_for_player(cfx_primary ? cfx_primary : stock);   /* none wanted: the playing one, off */
}

/* The stock engine is the truth for the first effect: if it is not what was
   told, and the player had time to follow, another path changed it (the
   effect went off, the console reset): start again from what it plays. */
static void cfx_resync(void)
{
    unsigned int stock = cfx_stock_type();
    if (stock >= CFX_TYPES) stock = 0u;
    if (stock == cfx_primary) { cfx_dirty = 0u; cfx_retries = 0u; return; }
    if ((unsigned int)(cfx_clock() - cfx_changed_ms) < CFX_SETTLE_MS) return;
    if (cfx_dirty && cfx_retries < 2u) {      /* the key was lost: once more */
        cfx_retries++;
        cfx_sync_player();
        return;
    }
    cfx_dirty = cfx_retries = 0u;
    cfx_primary = stock;
    cfx_sel = stock ? 1u << stock : 0u;
    cfx_publish();
}

/* A button that was on, released: off, or with a long hold, all off. */
static void cfx_released(unsigned int index)
{
    unsigned int type = cfx_button_type[index], bit = 1u << type;
    if (!(cfx_sel & bit)) return;
    if ((unsigned int)(cfx_clock() - cfx_pressed_ms[index]) >= CFX_HOLD_ALL_MS) {
        cfx_sel = 0u;
        cfx_primary = 0u;
        cfx_dirty = 1u;
        cfx_retries = 0u;
        cfx_publish();
        return;
    }
    cfx_sel &= ~bit;
    if (type != cfx_primary) { cfx_publish(); return; }
    /* The stock engine's own: the lowest of the rest takes its place. */
    unsigned int next = 0u;
    for (unsigned int t = 1u; !next && t < CFX_TYPES; t++)
        if (cfx_sel & (1u << t)) next = t;
    cfx_primary = next;
    cfx_dirty = 1u;
    cfx_retries = 0u;
    cfx_publish();
}

static int cfx_key(const struct rx3_key_event *event)
{
    if (event->key < CFX_KEY_FIRST || event->key >= CFX_KEY_FIRST + CFX_BUTTONS) return 0;
    unsigned int index = event->key - CFX_KEY_FIRST, bit = 1u << index;
    unsigned int type = cfx_button_type[index];
    unsigned int channel = event->channel < CFX_CHANNELS ? event->channel : CFX_CHANNELS - 1u;
    static unsigned int traced;
    if (traced < 32u) {
        traced++;
        framework->log_number("colorfx-dual: button (code, op, channel) = ",
                              event->key * 10000u + (event->operation & 0xffu) * 100u + channel);
    }
    if ((event->operation & ~1u) == 2u) {
        cfx_down[channel] &= ~bit;
        int taken = (cfx_taken & bit) != 0u;     /* its press never reached the player */
        if (!(cfx_any_down() & bit)) {
            cfx_taken &= ~bit;
            if (cfx_pending & bit) { cfx_pending &= ~bit; cfx_released(index); }
        }
        cfx_sync_player();
        return taken;
    }
    if (event->operation != 0u) return 0;
    if (cfx_any_down() & bit) {
        /* The same press from another channel: as decided. */
        cfx_down[channel] |= bit;
        cfx_seen[index] |= 1u << channel;
        return (cfx_taken & bit) != 0u;
    }
    cfx_last[index].channel = channel;
    cfx_last[index].raw = event->raw;
    cfx_last[index].value = event->value;
    cfx_seen[index] = 1u << channel;
    cfx_channels_any |= 1u << channel;
    cfx_resync();
    cfx_down[channel] |= bit;
    if (cfx_sel & (1u << type)) {
        /* On already: it goes off when released (a long hold: all off). */
        cfx_pending |= bit;
        cfx_pressed_ms[index] = cfx_clock();
        cfx_taken |= bit;
        return 1;
    }
    if (!cfx_sel) {                       /* the first one: stock turns it on */
        cfx_sel = 1u << type;
        cfx_primary = type;
        cfx_changed_ms = cfx_clock();
        cfx_publish();
        return 0;
    }
    cfx_sel |= 1u << type;                /* one more, played by this module */
    cfx_publish();
    cfx_taken |= bit;
    return 1;
}

static int configured(void)
{
    const char *value = getenv("RX3_COLORFX_DUAL");
    return value && value[0] == '1';
}

static int start(const struct rx3_services *services)
{
    framework = services;
    cfx_wanted = 0u;
    cfx_taken = 0u;
    cfx_sel = cfx_primary = cfx_pending = cfx_dirty = cfx_retries = cfx_channels_any = 0u;
    memset(cfx_down, 0, sizeof(cfx_down));
    memset(cfx_seen, 0, sizeof(cfx_seen));
    memset(cfx_lanes, 0, sizeof(cfx_lanes));
    if (!services->input || !services->input->register_key || !services->input->send_key ||
        !services->install_hook) {
        services->log_line("colorfx-dual: services missing, SOUND COLOR FX stay native");
        return 0;
    }
#if defined(__arm__)
    if (memcmp((const void *)CFX_GET_TYPE, cfx_type_guard, sizeof(cfx_type_guard))) {
        services->log_line("colorfx-dual: getSoundColorFxType guard rejected, SOUND COLOR FX stay native");
        return 0;
    }
#endif
    cfx_original = (cfx_operate_fn)services->install_hook(&cfx_hook, CFX_OPERATE, cfx_operate_guard,
                                                          cfx_operate);
    if (!cfx_original) {
        services->log_line("colorfx-dual: operate guard rejected, SOUND COLOR FX stay native");
        return 0;
    }
    if (!services->input->register_key(&cfx_owner, 30u, cfx_key)) {
        services->log_line("colorfx-dual: key handler refused");
        return 0;
    }
    cfx_registered = 1u;
    /* Without the lights the two effects still play. */
    cfx_leds_original = (cfx_leds_fn)services->install_hook(&cfx_leds_hook, CFX_CHECK_LEDS,
                                                            cfx_leds_guard, cfx_leds);
    if (!cfx_leds_original)
        services->log_line("colorfx-dual: checkLedStat guard rejected, the added buttons stay dark");
    services->log_line("colorfx-dual: tap SOUND COLOR FX to add or remove, hold a lit one to clear all");
    return 1;
}

/* Must also accept a partially completed start. */
static void stop(void)
{
    if (cfx_registered && framework->input->unregister_owner)
        framework->input->unregister_owner(&cfx_owner);
    cfx_registered = 0u;
    cfx_sel = cfx_primary = cfx_pending = 0u;
    __atomic_store_n(&cfx_wanted, 0u, __ATOMIC_SEQ_CST);
    if (cfx_original && framework->detach_hook && framework->detach_hook(&cfx_hook))
        framework->release_hook(&cfx_hook);
    cfx_original = 0;
    if (cfx_leds_original && framework->detach_hook && framework->detach_hook(&cfx_leds_hook))
        framework->release_hook(&cfx_leds_hook);
    cfx_leds_original = 0;
}

const struct rx3_module rx3_colorfx_dual_module = {
    .version = RX3_MODULE_API_VERSION, .size = sizeof(struct rx3_module),
    .name = "colorfx-dual", .configured = configured, .start = start, .stop = stop
};

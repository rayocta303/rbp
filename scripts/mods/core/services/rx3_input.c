/* SPDX-License-Identifier: MPL-2.0 */
/* Native input broker. One owner for each shared input hook; features only
 * register handlers. Registration happens on the serial startup thread; the
 * dispatch paths read a fixed table and never allocate, log or wait. */
#include "../api/rx3_input_api.h"
#include "rx3_input.h"
#include "rx3_hooks.h"
#include "rx3_log.h"

#define ON_KEY_PAD     ((unsigned long)0x003060e8)
#define SEND_KEY       ((unsigned long)0x0037ad64)
#define CHECK_SLIP_LED ((unsigned long)0x002fcc04)
#define CHECK_LED_STAT ((unsigned long)0x002f6318)
/* ui::Player::checkSimulatorLedState (RELEASE FX) and
   ui::Player::checkBeatJumpLoopMoveLedState, analysis 01-05-06 §0.2. */
#define CHECK_SIMULATOR_LED ((unsigned long)0x002fd9f8)
#define CHECK_BEAT_JUMP_LED ((unsigned long)0x002fe544)
#define PHYSICAL_KEY   ((unsigned long)0x00306b78)
#define SET_LED_COLOR  ((unsigned long)0x0033e4f8)
#define SET_LED_STATE  ((unsigned long)0x0033e3f4)

static const uint8_t pad_guard[8] = {0xb8,0x30,0xd1,0xe1,0xf0,0x4f,0x2d,0xe9};
static const uint8_t send_key_guard[8] = {0xf0,0x4f,0x2d,0xe9,0x0c,0xd0,0x4d,0xe2};
/* This prologue contains a PC-relative ldr and requires literal relocation. */
static const uint8_t slip_led_guard[8] = {0xb4,0x3d,0x9f,0xe5,0xf0,0x4f,0x2d,0xe9};
static const uint8_t simulator_led_guard[8] = {0x38,0x3b,0x9f,0xe5,0xf0,0x4f,0x2d,0xe9};
static const uint8_t beat_jump_led_guard[8] = {0x14,0x37,0x9f,0xe5,0xf0,0x4f,0x2d,0xe9};
static const uint8_t led_stat_guard[8] = {0xf0,0x4f,0x2d,0xe9,0x00,0x40,0xa0,0xe1};
static const uint8_t physical_guard[8] = {0xb8,0x30,0xd1,0xe1,0x00,0xc0,0xa0,0xe3};

typedef int (*on_key_pad_fn)(void *, const void *);
typedef int (*send_key_fn)(void *, unsigned int, unsigned int, unsigned int,
                           unsigned int, unsigned int, unsigned int);
typedef void (*led_refresh_fn)(void *, void *);
/* uif::Led::setState(State, period_ms, started_at_ms, long, BrightnessState).
   With State 2 the panel runs the blink itself. */
typedef void (*set_led_state_fn)(void *, int, unsigned int, unsigned int, long, int);
typedef void (*set_led_color_fn)(void *, int, int, const void *);

/* stems, samples, theme-white and keyshift already take four key slots;
   unregister_owner() does not compact the table, so keep headroom. */
#define CLIENT_LIMIT 8u
#define KEY_CHANNELS 4u

struct pad_client { const void *owner; unsigned int priority; rx3_pad_handler handler; };
struct key_client { const void *owner; unsigned int priority; rx3_key_handler handler; };
struct shared_hook {
    struct installed_hook hook;
    void *original;
    unsigned int active;
};
/* The native LED list a refresh hands over: a count at +4 and 44-byte
   entries at +8, each with its LED ID and channel. */
#define LED_STRIDE 44u
#define LED_LIMIT 256u
/* uif::Led::State. With BLINK the panel runs the blink itself from a
   half-period and an origin in juce::Time::currentTimeMillis. */
#define LED_ON 1
#define LED_BLINK 2
#define BLINK_PERIOD_MS 500u

static struct pad_client pad_clients[CLIENT_LIMIT];
static struct key_client key_clients[CLIENT_LIMIT];
static unsigned int pad_client_count, key_client_count;
/* One native LED refresh per RX3_LIGHTS_* value, one owner each. */
#define LIGHT_REFRESHES (RX3_LIGHTS_BEAT_JUMP + 1u)
struct light_refresh {
    struct shared_hook hook;
    const void *owner;
    rx3_light_fn light;
};
static struct light_refresh light_refreshes[LIGHT_REFRESHES];
static const void *mode_key_owners[CLIENT_LIMIT];
static volatile unsigned int blink_origin, blink_valid;
static struct shared_hook pad_hook, key_hook, mode_key_hook;
static void (*mode_key_action)(void);
static uint8_t shift_held[KEY_CHANNELS];
/* The KeyManager the panel last sent through: send_key() injects there. */
static void *volatile key_target;
static unsigned int key_trace_limit, key_traced, key_trace_pressed;
/* 0 traces every key; otherwise only this code (RX3_KEY_TRACE). */
static unsigned int key_trace_only;
/* The firmware's LED writers. */
static set_led_state_fn led_state_writer = (set_led_state_fn)SET_LED_STATE;
static set_led_color_fn led_colour_writer = (set_led_color_fn)SET_LED_COLOR;

static void enter(struct shared_hook *hook) { __atomic_add_fetch(&hook->active, 1u, __ATOMIC_SEQ_CST); }
static void leave(struct shared_hook *hook) { __atomic_sub_fetch(&hook->active, 1u, __ATOMIC_SEQ_CST); }

/* A handler cleared before this returns can no longer be entered: every path
   that reaches it increments the counter first. */
static void drain(struct shared_hook *hook)
{
    for (;;) {
        while (__atomic_load_n(&hook->active, __ATOMIC_SEQ_CST)) usleep(1000u);
        usleep(10000u);
        if (!__atomic_load_n(&hook->active, __ATOMIC_SEQ_CST)) return;
    }
}

static int acquire(struct shared_hook *hook, unsigned long address, const uint8_t guard[8],
                   void *replacement, int literal)
{
    if (hook->original) return 1;
    hook->original = literal ? install_pc_ldr_hook(&hook->hook, address, guard, replacement)
                             : install_hook(&hook->hook, address, guard, replacement);
    return hook->original != 0;
}

/* The ABI is softfp: sendKey's float travels in an integer slot. */
union key_value { float value; unsigned int bits; };

/* Detach, drain, then release the trampoline. A failed detach retains the
   trampoline and the original entry: the replacement stays reachable and
   passes every event through while nobody is registered. */
static void relinquish(struct shared_hook *hook)
{
    if (!hook->original) return;
    int detached = detach_hook(&hook->hook);
    drain(hook);
    if (!detached) {
        log_line("input: native hook retained after restore failure");
        return;
    }
    (void)release_hook(&hook->hook);
    hook->original = 0;
}

/* The SHIFT page flag of each pad mode, a byte of PlayerInnards (analysis
   01-05-06 §0.1). HOT CUE has none here: its GATE CUE flag is +0x78 or +0x79
   and which is not settled (DA_FIXARE/14), so it reports page 0. */
static unsigned int pad_page_of(const uint8_t *innards, unsigned int pad_mode)
{
    switch (pad_mode) {
    case RX3_PAD_BEAT_LOOP: return innards[0x7au] != 0u;
    case RX3_PAD_SLIP_LOOP: return innards[0x7bu] != 0u;
    case RX3_PAD_BEAT_JUMP: return innards[0x7cu] != 0u;
    default: return 0u;
    }
}

/* The first presses of the eight performance pads, once, for the log: what the player says the
   mode and the SHIFT page are when a pad page does not answer (the sampler on RELEASE FX had no
   validation on the console). One number, digits D M a b c P: deck (9 = none), pad mode, the
   three page bytes at +0x7a +0x7b +0x7c, and the pad 0 to 7. */
static unsigned int pad_traced;
static void trace_pad(const struct rx3_pad_event *e, const uint8_t *innards)
{
    if (e->operation != 0u || e->code < 0x4117u || e->code > 0x411eu || pad_traced >= 24u) return;
    pad_traced++;
    unsigned int deck = e->deck < 9u ? e->deck : 9u, mode = e->pad_mode < 9u ? e->pad_mode : 9u;
    unsigned int a = innards[0x7au] < 9u ? innards[0x7au] : 9u, b = innards[0x7bu] < 9u ? innards[0x7bu] : 9u;
    unsigned int c = innards[0x7cu] < 9u ? innards[0x7cu] : 9u;
    rx3_log_number("pad deck mode p7a p7b p7c pad =", deck * 100000u + mode * 10000u + a * 1000u +
                   b * 100u + c * 10u + (e->code - 0x4117u));
}

static int pad_hooked(void *player, const void *input)
{
    enter(&pad_hook);
    const uint8_t *event = input;
    const uint8_t *innards = player;
    struct rx3_pad_event e;
    e.code = event[8] | ((unsigned int)event[9] << 8u);
    e.operation = event[11] & 15u;
    e.channel = event[10];
    unsigned int number = e.channel >= 1u && e.channel <= 2u ? e.channel : innards[0x26u];
    e.deck = number >= 1u && number <= 2u ? number - 1u : RX3_NO_DECK;
    e.pad_mode = *(const uint32_t *)(innards + 0x74u);
    e.pad_page = pad_page_of(innards, e.pad_mode);
    trace_pad(&e, innards);
    int result = 0;
    for (unsigned int i = 0; i < pad_client_count && !result; i++) {
        rx3_pad_handler handler = __atomic_load_n(&pad_clients[i].handler, __ATOMIC_SEQ_CST);
        if (handler) result = handler(&e);
    }
    if (!result) result = ((on_key_pad_fn)pad_hook.original)(player, input);
    leave(&pad_hook);
    return result;
}

/* Diagnostic only, off unless the startup asked for it: each traced event
   is two lines, the code/operation/channel triple and then its raw value. */
static void trace_key(unsigned int key, unsigned int operation, unsigned int channel,
                      unsigned int raw)
{
    if (key_traced >= key_trace_limit) return;
    /* At start-up both panels report every button's state (operation 2):
       a whole trace budget of nothing. Tracing every key starts at the
       first real press. */
    if (!key_trace_only && !key_trace_pressed) {
        if (operation != 0u) return;
        key_trace_pressed = 1u;
    }
    if (key_trace_only && key != key_trace_only) return;
    /* Tracing every key: knobs and faders (operation 5) would fill the
       budget in a second, and the buttons are what a trace is asked for. */
    if (!key_trace_only && (operation & 0xffu) == 5u) return;
    key_traced++;
    rx3_log_number("key kkkkooocc =", (key & 0xffffu) * 100000u +
               (operation & 0xffu) * 100u + (channel & 0xffu));
    rx3_log_number("key raw =", raw);
}

static int key_hooked(void *target, unsigned int key, unsigned int operation,
                      unsigned int channel, unsigned int a, unsigned int b, unsigned int c)
{
    enter(&key_hook);
    if (target) __atomic_store_n(&key_target, target, __ATOMIC_SEQ_CST);
    trace_key(key, operation, channel, a);
    unsigned int slot = channel < KEY_CHANNELS ? channel : 0u;
    if (key == RX3_KEY_SHIFT) {
        if (operation == 0u) shift_held[slot] = 1u;
        else if ((operation & ~1u) == 2u) shift_held[slot] = 0u;
    }
    union key_value value;
    value.bits = b;
    struct rx3_key_event e = {key, operation, channel, a, value.value};
    int consumed = 0;
    for (unsigned int i = 0; i < key_client_count && !consumed; i++) {
        rx3_key_handler handler = __atomic_load_n(&key_clients[i].handler, __ATOMIC_SEQ_CST);
        if (handler) consumed = handler(&e);
    }
    int result = consumed ? 0 : ((send_key_fn)key_hook.original)(target, key, operation,
                                                                 channel, a, b, c);
    leave(&key_hook);
    return result;
}

/* Straight to the native sendKey: the handlers are skipped, so a handler
   may inject without re-entering itself. */
static int send_key(unsigned int key, unsigned int operation, unsigned int channel,
                    unsigned int raw, float value)
{
    enter(&key_hook);
    void *target = __atomic_load_n(&key_target, __ATOMIC_SEQ_CST);
    void *original = __atomic_load_n(&key_hook.original, __ATOMIC_SEQ_CST);
    int sent = 0;
    if (target && original) {
        union key_value v;
        v.value = value;
        (void)((send_key_fn)original)(target, key, operation, channel, raw, v.bits, 0u);
        sent = 1;
    }
    leave(&key_hook);
    return sent;
}

struct rx3_timeval { long seconds, microseconds; };
/* juce::Time::currentTimeMillis is gettimeofday reduced to milliseconds, so
   the panel's LED blink and the on-screen controls share one time domain. */
static unsigned int now_ms(void)
{
    struct rx3_timeval value;
    if (gettimeofday(&value, 0)) return 0;
    return (unsigned int)(((uint64_t)(unsigned long)value.seconds * 1000000u +
                           (uint64_t)(unsigned long)value.microseconds) / 1000u);
}

static unsigned int blink_origin_ms(void)
{
    if (!__atomic_load_n(&blink_valid, __ATOMIC_SEQ_CST)) {
        __atomic_store_n(&blink_origin, now_ms(), __ATOMIC_SEQ_CST);
        __atomic_store_n(&blink_valid, 1u, __ATOMIC_SEQ_CST);
    }
    return __atomic_load_n(&blink_origin, __ATOMIC_SEQ_CST);
}

static int blink_on(void)
{
    return (((now_ms() - blink_origin_ms()) / BLINK_PERIOD_MS) & 1u) == 0u;
}

static void blink_restart(void) { __atomic_store_n(&blink_valid, 0u, __ATOMIC_SEQ_CST); }

/* One declared light onto one native LED. The SLIP LOOP refresh sets its own
   states, so a steady light there changes only the colour; the unit-wide
   refresh leaves states to whoever colours the pad. */
static void apply_light(uint8_t *led, const struct rx3_light *light, int set_state)
{
    const uint8_t colour[3] = {(uint8_t)(light->rgb >> 16u), (uint8_t)(light->rgb >> 8u),
                               (uint8_t)light->rgb};
    if (light->state == RX3_LIGHT_BLINK) {
        led_state_writer(led, LED_BLINK, BLINK_PERIOD_MS, blink_origin_ms(), 0, 0);
        led_colour_writer(led, LED_BLINK, 0, colour);
        return;
    }
    if (set_state) led_state_writer(led, LED_ON, 0u, 0u, 0, 0);
    led_colour_writer(led, LED_ON, light->state == RX3_LIGHT_DIM, colour);
}

static int led_list(void *list, uint8_t **entries, unsigned int *count)
{
    if (!list) return 0;
    *entries = *(uint8_t **)((uint8_t *)list + 8u);
    *count = *(const uint16_t *)((const uint8_t *)list + 4u);
    return *entries && *count <= LED_LIMIT;
}

/* One deck's pads after a per-deck native refresh. The deck is the player's
   number (+0x26); only entries on that channel are asked. The pads carry IDs
   18 to 25; with `any_base` a list that has none of those carries them at
   1 to 8 instead, as in the unit-wide refresh. */
static void light_deck_pads(struct light_refresh *refresh, void *player, void *led_stat,
                            int any_base, int set_state)
{
    enter(&refresh->hook);
    ((led_refresh_fn)refresh->hook.original)(player, led_stat);
    rx3_light_fn light_of = __atomic_load_n(&refresh->light, __ATOMIC_SEQ_CST);
    unsigned int number = *(const uint8_t *)((const uint8_t *)player + 0x26u);
    uint8_t *entries;
    unsigned int count;
    if (light_of && number >= 1u && number <= 2u && led_list(led_stat, &entries, &count)) {
        unsigned int base = 18u;
        if (any_base) {
            base = 1u;
            for (unsigned int i = 0; i < count; i++) {
                unsigned int id = *(const uint32_t *)(entries + i * LED_STRIDE);
                if (id >= 18u && id <= 25u) base = 18u;
            }
        }
        for (unsigned int i = 0; i < count; i++) {
            uint8_t *led = entries + i * LED_STRIDE;
            unsigned int id = *(const uint32_t *)led;
            if (*(const uint32_t *)(led + 4u) != number || id < base || id >= base + 8u) continue;
            /* The colour the native refresh just set (uif::Led +0x28, RGB),
               so a client can keep it while it changes only the state. */
            struct rx3_light light = {RX3_LIGHT_NATIVE,
                (uint32_t)led[0x28] << 16u | (uint32_t)led[0x29] << 8u | led[0x2a]};
            light_of(number - 1u, id - base, &light);
            if (light.state != RX3_LIGHT_NATIVE) apply_light(led, &light, set_state);
        }
    }
    leave(&refresh->hook);
}

/* SLIP LOOP, one deck: that deck's pads carry IDs 18 to 25. */
static void slip_led_hooked(void *player, void *led_stat)
{
    light_deck_pads(&light_refreshes[RX3_LIGHTS_SLIP_LOOP], player, led_stat, 0, 0);
}

/* RELEASE FX and BEAT JUMP (both pages), one deck each. These refreshes are
   newer than SLIP LOOP's and not yet seen on the console, so a declared
   steady light also sets the LED state (DA_FIXARE/15). */
static void release_fx_led_hooked(void *player, void *led_stat)
{
    light_deck_pads(&light_refreshes[RX3_LIGHTS_RELEASE_FX], player, led_stat, 1, 1);
}

static void beat_jump_led_hooked(void *player, void *led_stat)
{
    light_deck_pads(&light_refreshes[RX3_LIGHTS_BEAT_JUMP], player, led_stat, 1, 1);
}

/* Every pad mode: the pads are IDs 18 to 25 when the list carries them there,
   else 1 to 8; SHIFT is ID 14. */
static void pad_led_hooked(void *self, void *state)
{
    struct light_refresh *refresh = &light_refreshes[RX3_LIGHTS_PADS];
    enter(&refresh->hook);
    ((led_refresh_fn)refresh->hook.original)(self, state);
    rx3_light_fn light_of = __atomic_load_n(&refresh->light, __ATOMIC_SEQ_CST);
    uint8_t *entries;
    unsigned int count;
    if (light_of && led_list(state, &entries, &count) && count) {
        unsigned int base = 1u;
        for (unsigned int i = 0; i < count; i++) {
            unsigned int id = *(const uint32_t *)(entries + i * LED_STRIDE);
            if (id >= 18u && id <= 25u) base = 18u;
        }
        for (unsigned int i = 0; i < count; i++) {
            uint8_t *led = entries + i * LED_STRIDE;
            unsigned int id = *(const uint32_t *)led;
            unsigned int control = id == 14u ? RX3_LIGHT_SHIFT :
                                   id >= base && id < base + 8u ? id - base : RX3_NO_DECK;
            if (control == RX3_NO_DECK) continue;
            unsigned int channel = *(const uint32_t *)(led + 4u);
            struct rx3_light light = {RX3_LIGHT_NATIVE, 0u};
            light_of(channel >= 1u && channel <= 2u ? channel - 1u : RX3_NO_DECK, control, &light);
            if (light.state != RX3_LIGHT_NATIVE) apply_light(led, &light, 1);
        }
    }
    leave(&refresh->hook);
}

/* Where each refresh lives, indexed by RX3_LIGHTS_*. The per-deck prologues
   start with a PC-relative ldr and need literal relocation. */
static const struct {
    unsigned long address;
    const uint8_t *guard;
    void (*replacement)(void *, void *);
    int literal;
} refresh_sites[LIGHT_REFRESHES] = {
    {CHECK_SLIP_LED, slip_led_guard, slip_led_hooked, 1},
    {CHECK_LED_STAT, led_stat_guard, pad_led_hooked, 0},
    {CHECK_SIMULATOR_LED, simulator_led_guard, release_fx_led_hooked, 1},
    {CHECK_BEAT_JUMP_LED, beat_jump_led_guard, beat_jump_led_hooked, 1},
};

/* 1.19 inlines SLIP LOOP in onPhysicalKey. Never consume a native mode event
   or add a page to its cycle: the core only closes its own panels. */
static int mode_key_hooked(void *player, const void *input)
{
    enter(&mode_key_hook);
    const uint8_t *event = input;
    unsigned int code = event[8] | ((unsigned int)event[9] << 8u);
    void (*action)(void) = __atomic_load_n(&mode_key_action, __ATOMIC_SEQ_CST);
    if (action && code >= 0x4113u && code <= 0x4116u && !(event[11] & 0x0fu)) action();
    int result = ((on_key_pad_fn)mode_key_hook.original)(player, input);
    leave(&mode_key_hook);
    return result;
}

static int register_pad(const void *owner, unsigned int priority, rx3_pad_handler handler)
{
    if (!owner || !handler || pad_client_count >= CLIENT_LIMIT) return 0;
    for (unsigned int i = 0; i < pad_client_count; i++)
        if (pad_clients[i].owner == owner && pad_clients[i].handler) return 0;
    if (!acquire(&pad_hook, ON_KEY_PAD, pad_guard, (void *)pad_hooked, 0)) return 0;
    unsigned int at = pad_client_count;
    while (at && pad_clients[at - 1u].priority > priority) {
        pad_clients[at] = pad_clients[at - 1u];
        at--;
    }
    pad_clients[at].owner = owner;
    pad_clients[at].priority = priority;
    pad_clients[at].handler = handler;
    __atomic_store_n(&pad_client_count, pad_client_count + 1u, __ATOMIC_SEQ_CST);
    return 1;
}

static int register_key(const void *owner, unsigned int priority, rx3_key_handler handler)
{
    if (!owner || !handler || key_client_count >= CLIENT_LIMIT) return 0;
    for (unsigned int i = 0; i < key_client_count; i++)
        if (key_clients[i].owner == owner && key_clients[i].handler) return 0;
    if (!acquire(&key_hook, SEND_KEY, send_key_guard, (void *)key_hooked, 0)) return 0;
    unsigned int at = key_client_count;
    while (at && key_clients[at - 1u].priority > priority) {
        key_clients[at] = key_clients[at - 1u];
        at--;
    }
    key_clients[at].owner = owner;
    key_clients[at].priority = priority;
    key_clients[at].handler = handler;
    __atomic_store_n(&key_client_count, key_client_count + 1u, __ATOMIC_SEQ_CST);
    return 1;
}

static int register_lights(const void *owner, unsigned int refresh, rx3_light_fn light)
{
    if (!owner || !light || refresh >= LIGHT_REFRESHES) return 0;
    struct light_refresh *r = &light_refreshes[refresh];
    if (r->owner ||
        !acquire(&r->hook, refresh_sites[refresh].address, refresh_sites[refresh].guard,
                 (void *)refresh_sites[refresh].replacement, refresh_sites[refresh].literal))
        return 0;
    r->owner = owner;
    __atomic_store_n(&r->light, light, __ATOMIC_SEQ_CST);
    return 1;
}

static int claim_mode_keys(const void *owner)
{
    if (!owner) return 0;
    unsigned int free_slot = CLIENT_LIMIT;
    for (unsigned int i = 0; i < CLIENT_LIMIT; i++) {
        if (mode_key_owners[i] == owner) return 0;
        if (!mode_key_owners[i] && free_slot == CLIENT_LIMIT) free_slot = i;
    }
    if (free_slot == CLIENT_LIMIT ||
        !acquire(&mode_key_hook, PHYSICAL_KEY, physical_guard, (void *)mode_key_hooked, 0))
        return 0;
    mode_key_owners[free_slot] = owner;
    return 1;
}

static void unregister_owner(const void *owner)
{
    if (!owner) return;
    unsigned int pads = 0, keys = 0, modes = 0;
    for (unsigned int i = 0; i < pad_client_count; i++) {
        if (pad_clients[i].owner == owner) __atomic_store_n(&pad_clients[i].handler, 0, __ATOMIC_SEQ_CST);
        if (pad_clients[i].handler) pads++;
    }
    for (unsigned int i = 0; i < key_client_count; i++) {
        if (key_clients[i].owner == owner) __atomic_store_n(&key_clients[i].handler, 0, __ATOMIC_SEQ_CST);
        if (key_clients[i].handler) keys++;
    }
    for (unsigned int i = 0; i < LIGHT_REFRESHES; i++) {
        if (light_refreshes[i].owner != owner) continue;
        __atomic_store_n(&light_refreshes[i].light, 0, __ATOMIC_SEQ_CST);
        light_refreshes[i].owner = 0;
    }
    for (unsigned int i = 0; i < CLIENT_LIMIT; i++) {
        if (mode_key_owners[i] == owner) mode_key_owners[i] = 0;
        if (mode_key_owners[i]) modes++;
    }
    /* Handlers the owner left behind can still be running; wait them out
       before its code or state may be released. */
    drain(&pad_hook); drain(&key_hook); drain(&mode_key_hook);
    for (unsigned int i = 0; i < LIGHT_REFRESHES; i++) drain(&light_refreshes[i].hook);
    if (!pads) { relinquish(&pad_hook); if (!pad_hook.original) pad_client_count = 0; }
    if (!keys) {
        relinquish(&key_hook);
        if (!key_hook.original) {
            key_client_count = 0;
            __atomic_store_n(&key_target, 0, __ATOMIC_SEQ_CST);
        }
    }
    for (unsigned int i = 0; i < LIGHT_REFRESHES; i++)
        if (!light_refreshes[i].owner) relinquish(&light_refreshes[i].hook);
    if (!modes) relinquish(&mode_key_hook);
}

static int shift_is_held(unsigned int channel)
{
    if (channel != RX3_NO_DECK) return channel < KEY_CHANNELS && shift_held[channel];
    for (unsigned int i = 0; i < KEY_CHANNELS; i++) if (shift_held[i]) return 1;
    return 0;
}

void rx3_input_bind_mode_keys(void (*action)(void))
{
    __atomic_store_n(&mode_key_action, action, __ATOMIC_SEQ_CST);
}

void rx3_input_trace_keys(unsigned int limit)
{
    key_trace_limit = limit;
}

void rx3_input_trace_only(unsigned int key)
{
    key_trace_only = key;
}

unsigned int rx3_input_count(void)
{
    unsigned int count = pad_client_count + key_client_count;
    for (unsigned int i = 0; i < LIGHT_REFRESHES; i++) count += light_refreshes[i].owner != 0;
    return count;
}

const struct rx3_input_service rx3_input = {
    register_pad, register_key, register_lights, claim_mode_keys,
    unregister_owner, shift_is_held, blink_on, blink_restart, send_key
};

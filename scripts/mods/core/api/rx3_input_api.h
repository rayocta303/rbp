/* SPDX-License-Identifier: MPL-2.0 */
#ifndef RX3_INPUT_API_H
#define RX3_INPUT_API_H
#include "rx3_platform.h"
/* The core owns every native input hook: pad presses, SEND_KEY, the pad LED
 * refreshes and the physical pad-mode dispatcher. Modules register handlers;
 * they never install, remove or chain those hooks themselves.
 *
 * Handlers run on the player's input thread. They must return promptly and
 * must not allocate, perform I/O or wait. unregister_owner() closes admission
 * and drains in-flight handlers before it returns, so it must not be called
 * from a handler. A shared native hook is removed with its last client.
 */
#define RX3_NO_DECK 0xffffffffu

/* One native pad event. `operation` is the low nibble of the native operation
 * byte (0 press, 2 or 3 release). `channel` is the native channel byte
 * unchanged. `deck` is resolved from the channel, else from the player that
 * dispatched it, else RX3_NO_DECK. `pad_mode` is the player's pad mode
 * (PlayerInnards+0x74): 0 HOT CUE, 1 BEAT LOOP, 2 SLIP LOOP, 3 BEAT JUMP.
 *
 * `pad_page` is the page of that mode, 0 normal, 1 SHIFT page, read from the
 * dispatching player: mode 1 +0x7a (BEAT LOOP 2), mode 2 +0x7b (RELEASE FX),
 * mode 3 +0x7c (BEAT JUMP 2). For HOT CUE (mode 0) it is always 0: the GATE
 * CUE flag is +0x78 or +0x79 and which one is not yet known (DA_FIXARE/14).
 * A handler that owns a page tests both fields, e.g. SLIP LOOP proper is
 * `pad_mode == 2 && pad_page == 0`. */
struct rx3_pad_event {
    unsigned int code, operation, channel, deck, pad_mode;
    unsigned int pad_page;
};
#define RX3_PAD_HOT_CUE   0u
#define RX3_PAD_BEAT_LOOP 1u
#define RX3_PAD_SLIP_LOOP 2u
#define RX3_PAD_BEAT_JUMP 3u

/* Return nonzero to consume the event. Lower priority values run first;
 * the original player handler runs exactly once when nobody consumes. */
typedef int (*rx3_pad_handler)(const struct rx3_pad_event *);

/* SEND_KEY, uif::IKeyManager::sendKey(key, op, channel, raw, value, extra):
 * key code, native operation (0 press, 2 or 3 release, 5 analogue value) and
 * the reporting channel (1 or 2 for a mixer channel, 0 for the master
 * section). Knobs and faders arrive here too, as operation 5 with `raw` the
 * 10-bit position 0..1023 and `value` = raw / 1023.0f; buttons carry the
 * panel's raw and value unchanged (usually 0). Analysis 03-11 §0.
 * SHIFT state is updated before handlers run. Return nonzero to consume the
 * key: the player never sees it. */
struct rx3_key_event {
    unsigned int key, operation, channel;
    unsigned int raw;
    float value;
};
typedef int (*rx3_key_handler)(const struct rx3_key_event *);
#define RX3_KEY_SHIFT 0x4103u

/* Pad lights, declared rather than driven. When the player refreshes its pad
 * lights, the core asks the owner of that refresh what each control should
 * show and writes the native LEDs itself; modules never see an LED ID.
 * Controls are the eight performance pads, 0 (pad 1) to 7, and SHIFT.
 * Answering RX3_LIGHT_NATIVE leaves the player's own light. Callbacks run on
 * the input thread: no allocation, I/O or waiting. */
#define RX3_LIGHT_NATIVE 0u
#define RX3_LIGHT_ON     1u  /* the colour at full brightness */
#define RX3_LIGHT_DIM    2u  /* the colour at its idle brightness */
#define RX3_LIGHT_BLINK  3u  /* the colour, on the shared blink clock */
#define RX3_LIGHT_SHIFT  8u
/* In the per-deck refreshes `rgb` arrives holding the colour the native
   refresh set, so leaving it keeps the stock colour. */
struct rx3_light { unsigned int state; uint32_t rgb; /* 0xRRGGBB */ };
typedef void (*rx3_light_fn)(unsigned int deck, unsigned int control, struct rx3_light *);
/* The native refreshes a module can own, one owner each. The unit-wide
 * refresh covers every pad mode and SHIFT, with deck RX3_NO_DECK when the
 * player does not name one. The others run per deck, after the native one,
 * while their page is shown, and only ask about that deck's eight pads:
 *   SLIP_LOOP   ui::Player::checkSlipBeatLoopLedState (SLIP LOOP page 0)
 *   RELEASE_FX  ui::Player::checkSimulatorLedState    (SLIP LOOP page 1)
 *   BEAT_JUMP   ui::Player::checkBeatJumpLoopMoveLedState (BEAT JUMP, both
 *               pages: the callback tells them apart itself if it must)
 * In SLIP_LOOP a steady light changes only the colour (the native refresh
 * sets the state); in RELEASE_FX and BEAT_JUMP it also sets the state.
 * A module that takes a page's pads registers that page's refresh, so the
 * sampler and a beat jump module never compete for RX3_LIGHTS_PADS. */
#define RX3_LIGHTS_SLIP_LOOP  0u
#define RX3_LIGHTS_PADS       1u
#define RX3_LIGHTS_RELEASE_FX 2u
#define RX3_LIGHTS_BEAT_JUMP  3u

struct rx3_input_service {
    int (*register_pad)(const void *owner, unsigned int priority, rx3_pad_handler);
    int (*register_key)(const void *owner, unsigned int priority, rx3_key_handler);
    /* One owner per refresh; a second registration fails without replacing it. */
    int (*register_lights)(const void *owner, unsigned int refresh, rx3_light_fn);
    /* Ask the core to close its custom panels when a physical pad-mode key
       is pressed on the firmware path that inlines SLIP LOOP. */
    int (*claim_mode_keys)(const void *owner);
    void (*unregister_owner)(const void *owner);
    /* SHIFT as last reported on a channel; RX3_NO_DECK means any channel. */
    int (*shift_held)(unsigned int channel);
    /* The shared blink clock: nonzero in its on half. Any caller starts it
       at the on phase; blink_restart() makes the next start fresh. The pad
       lights and on-screen controls share it, so they cannot drift apart. */
    int (*blink_on)(void);
    void (*blink_restart)(void);
    /* Inject one SEND_KEY into the player, as the panel would: the native
       sendKey runs on the last KeyManager seen by the SEND_KEY hook, with
       extra = 0, and queues the event for the UI thread, in order with the
       panel's own. No handler sees it (no recursion), and SHIFT tracking is
       untouched. For the Isolator in STEMS mode (flatten or restore a knob:
       key 0x501a..0x501c, operation 5, raw 0..1023, value raw / 1023.0f) and
       for driving the Beat FX. Returns 1 when sent, 0 when no SEND_KEY has
       been seen yet or the hook is not installed (register_key first).
       The native sendKey allocates its message: call it from a key or pad
       handler or the startup thread, never from the audio thread, and not
       while unregister_owner() runs. */
    int (*send_key)(unsigned int key, unsigned int operation, unsigned int channel,
                    unsigned int raw, float value);
};
extern const struct rx3_input_service rx3_input;
#endif

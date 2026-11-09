/* SPDX-License-Identifier: MPL-2.0 */
/* Custom Beat Jump: the 16 pads of BEAT JUMP and BEAT JUMP 2 jump by what the
 * app wrote in RX3_RUNTIME/beatjump/beatjump.ini, up to 64 bars.
 *
 * How (analysis docs/analisi/01-05-06-pad-modes.md §3, firmware 1.20):
 * - the two pad -> EnBeatJumpType tables in rbp's rodata are rewritten, under
 *   guard, so that each configured pad sends a type of its own, 13..20 for
 *   BEAT JUMP and 21..28 for BEAT JUMP 2 (the player knows 1..12);
 * - playengine::Player::isBeatJumpEnable(EnBeatJumpType, float *) is hooked:
 *   types 1..12 go to the original, 13..28 answer from the configuration,
 *   checked against Player::getJumpableNumOfBeats(); the jump itself is then
 *   done by the player with the float written here;
 * - the BEAT JUMP pad lights are ON when the configured jump is possible and
 *   DIM when it is not, asked through DjEngineIF::isBeatJumpEnable.
 * A file that does not parse leaves the deck exactly stock: no table is
 * touched and no hook is installed. Everything lives in RAM. */
#include "../core/api/rx3_module_api.h"
#include "rx3_beatjump_custom_config.h"

/* playengine::Player::isBeatJumpEnable(EnBeatJumpType, float *) const.
   Prologue: sub r1, r1, #1; push {r4, r5, lr}. No PC-relative load. */
#define BEATJUMP_IS_ENABLE ((unsigned long)0x00068044)
/* playengine::Player::getJumpableNumOfBeats() const: half beats, the high
   halfword backwards, the low halfword forwards. */
#define BEATJUMP_JUMPABLE ((unsigned long)0x00067d48)
/* getJumpableNumOfBeats counts only the audio already in the decoder buffer
   around the play position (RX3 test 2026-10-03: 32 and 64 bar pads stayed
   dark and did nothing). The jump itself, Loop::playBeatJump, places the
   point on the beat grid and calls Player::setPlayingPosition, a real seek,
   so a long jump only needs its availability measured against the track:
   Player::getTotalLength @0x63a30 (samples), Bpm::getBeatWidthSample(Player
   const&) @0xaa310 (samples per beat, 0 without a grid), and the position
   getJumpableNumOfBeats itself reads: +0xe78 when the byte +0xe74 is set,
   else +0xe70 (rbp 1.20 @0x67e0c). */
#define BEATJUMP_TOTAL_LENGTH ((unsigned long)0x00063a30)
#define BEATJUMP_BEAT_WIDTH ((unsigned long)0x000aa310)
#define BEATJUMP_POSITION_SELECT 0xe74u
#define BEATJUMP_POSITION_ALT 0xe78u
#define BEATJUMP_POSITION 0xe70u
/* DjEngineIF::isBeatJumpEnable(channel, type): ignores `this`, uses the global
   PlayEngine; channel is the deck, 0 or 1. */
#define BEATJUMP_DJ_ENABLE ((unsigned long)0x00049da8)
/* Pad -> type tables, eight 32-bit words each (vaddr; file offset -0x8000). */
#define BEATJUMP_TABLE_PAGE1 ((unsigned long)0x004d3c78) /* BEAT JUMP   */
#define BEATJUMP_TABLE_PAGE2 ((unsigned long)0x004d3c58) /* BEAT JUMP 2 */
#define BEATJUMP_FIRST_VIRTUAL 13u
#define BEATJUMP_LAST_VIRTUAL (BEATJUMP_FIRST_VIRTUAL + BEATJUMP_PAD_COUNT - 1u)

static const uint8_t beatjump_guard[8] = {0x01,0x10,0x41,0xe2,0x30,0x40,0x2d,0xe9};
/* The stock tables: BEAT JUMP -1 +1 -2 +2 -4 +4 -8 +8, BEAT JUMP 2
   -1/2 +1/2 -2 +2 -4 +4 -16 +16 (EnBeatJumpType values). */
static const uint32_t beatjump_stock_types[BEATJUMP_PAD_COUNT] = {
    3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u,
    1u, 2u, 5u, 6u, 7u, 8u, 11u, 12u
};

/* ABI softfp: `out` is a pointer, so the float travels through memory and no
   VFP argument register is involved. The bool comes back in r0. */
typedef int (*beatjump_enable_fn)(const void *player, unsigned int type, float *out);
typedef uint32_t (*beatjump_jumpable_fn)(const void *player);
typedef long (*beatjump_length_fn)(const void *player);
typedef unsigned long (*beatjump_width_fn)(const void *player);
typedef int (*beatjump_dj_enable_fn)(const void *engine, unsigned int channel,
                                     unsigned int type);

/* Pad graphics (rbp 1.20 .data, scanned for the stock image IDs): each pad
 * has an OFF and an ON image object with its image ID at +0xc; OFF objects
 * step down by 0x3c from pad 1, the ON object sits 0x18 below its OFF one.
 * A configured pad gets a label composed by the core from stock records:
 * the frame and arrow of a stock BEAT JUMP image (back 5086/5087, forward
 * 5088/5089) and digits cut from the BEAT LOOP images. */
#define BEATJUMP_OBJECT_PAGE1 ((unsigned long)0x0052e72c)
#define BEATJUMP_OBJECT_PAGE2 ((unsigned long)0x0052e544)
#define BEATJUMP_OBJECT_STEP 0x3cu
#define BEATJUMP_OBJECT_ON 0x18u
#define BEATJUMP_OBJECT_IMAGE 0x0cu
#define BEATJUMP_FRAME_BACK 5086u
#define BEATJUMP_FRAME_FORWARD 5088u
#define BEATJUMP_HALF_SOURCE 5117u /* "1/2": glyphs 1, /, 2 */
static const uint32_t beatjump_stock_images[BEATJUMP_PAD_COUNT] = {
    5092u, 5090u, 5098u, 5100u, 5104u, 5102u, 5106u, 5108u,
    5096u, 5094u, 5098u, 5100u, 5104u, 5102u, 5086u, 5088u
};
/* OFF digit images 1..9 of BEAT LOOP and BEAT LOOP 2; ON is the next ID. */
static const unsigned int beatjump_digit_images[10] = {
    0u, 5115u, 5121u, 5125u, 5131u, 5133u, 5135u, 5137u, 5139u, 5141u
};

static const struct rx3_services *framework;
static const unsigned char beatjump_owner;
static struct installed_hook beatjump_hook;
static beatjump_enable_fn beatjump_original;
/* Firmware entry points, variables so host tests can stand in for them. */
static beatjump_jumpable_fn beatjump_jumpable = (beatjump_jumpable_fn)BEATJUMP_JUMPABLE;
static beatjump_length_fn beatjump_length = (beatjump_length_fn)BEATJUMP_TOTAL_LENGTH;
static beatjump_width_fn beatjump_width = (beatjump_width_fn)BEATJUMP_BEAT_WIDTH;
static beatjump_dj_enable_fn beatjump_dj_enable = (beatjump_dj_enable_fn)BEATJUMP_DJ_ENABLE;
/* Written by start() before any hook exists, read-only afterwards. */
static struct beatjump_config beatjump_config;
static uint32_t beatjump_patched;   /* pads whose table word holds our type */
static unsigned int beatjump_input; /* pad observer and lights registered */
static unsigned int beatjump_calls; /* callers inside the hook */
static uint32_t beatjump_labelled; /* bit 2*pad (+1 for ON): object holds our label */
static uint32_t beatjump_label_ids[BEATJUMP_PAD_COUNT * 2u];
/* The page each deck last showed a BEAT JUMP pad press on: the light refresh
   covers both pages and does not say which one is up. */
static unsigned int beatjump_page[2];

static unsigned long beatjump_table_address(unsigned int pad)
{
    return (pad < 8u ? BEATJUMP_TABLE_PAGE1 : BEATJUMP_TABLE_PAGE2) + (pad & 7u) * 4u;
}

/* The availability rule: `half` half beats (negative = back) against the
   packed answer of getJumpableNumOfBeats. */
static int beatjump_allowed(int half, uint32_t jumpable)
{
    if (!half) return 0;
    uint32_t need = (uint32_t)(half < 0 ? -half : half);
    uint32_t room = half < 0 ? jumpable >> 16u : jumpable & 0xffffu;
    return need <= room;
}

/* The room the track itself leaves, in the same packed half beats as
   getJumpableNumOfBeats, saturated at 16 bits. Zero from the player means a
   state that forbids any jump (loop reservation, slip, no grid): kept. */
static uint32_t beatjump_track_room(const void *player, uint32_t buffered)
{
    if (!buffered) return 0u;
    unsigned long width = beatjump_width(player);
    if (!width) return buffered;
    long total = beatjump_length(player);
    if (total <= 0) return buffered;
    const uint8_t *base = player;
    long position;
    memcpy(&position, base + (base[BEATJUMP_POSITION_SELECT] ? BEATJUMP_POSITION_ALT
                                                               : BEATJUMP_POSITION), 4u);
    if (position < 0 || position >= total) return buffered;
    unsigned long back = (unsigned long)position / width * 2u;
    unsigned long ahead = (unsigned long)(total - 1 - position) / width * 2u;
    if (back > 0xffffu) back = 0xffffu;
    if (ahead > 0xffffu) ahead = 0xffffu;
    return (uint32_t)back << 16 | (uint32_t)ahead;
}

static int beatjump_is_enable(const void *player, unsigned int type, float *out)
{
    __atomic_add_fetch(&beatjump_calls, 1u, __ATOMIC_SEQ_CST);
    int result;
    if (type < BEATJUMP_FIRST_VIRTUAL || type > BEATJUMP_LAST_VIRTUAL) {
        result = beatjump_original(player, type, out);
    } else {
        int half = beatjump_config.half[type - BEATJUMP_FIRST_VIRTUAL];
        uint32_t buffered = beatjump_jumpable(player);
        /* The track's own room decides when it is known: the decoded buffer
           alone lit BEAT JUMP 2 near the start and the end of a track, where
           the jump cannot happen (RX3, 2026-10-14). */
        result = half && beatjump_allowed(half, beatjump_track_room(player, buffered));
        if (result && out) *out = (float)half * 0.5f;
    }
    __atomic_sub_fetch(&beatjump_calls, 1u, __ATOMIC_SEQ_CST);
    return result;
}

/* Watches, never takes: the player still runs every BEAT JUMP pad. */
static int beatjump_pad(const struct rx3_pad_event *event)
{
    if (event->pad_mode == RX3_PAD_BEAT_JUMP && event->deck < 2u)
        __atomic_store_n(&beatjump_page[event->deck], event->pad_page ? 1u : 0u,
                         __ATOMIC_SEQ_CST);
    return 0;
}

/* The colour the player gives BEAT JUMP (page 1), per deck, read from its
   own refresh: BEAT JUMP 2 lights the same (RX3, 2026-10-15: the 32 and 64
   pads blinked and never went dark, only 8 and 16 followed the track). */
static uint32_t beatjump_native_rgb[2];


static void beatjump_light(unsigned int deck, unsigned int control, struct rx3_light *light)
{
    if (deck > 1u || control > 7u) return;

    unsigned int slot = __atomic_load_n(&beatjump_page[deck], __ATOMIC_SEQ_CST) * 8u + control;
    if (!beatjump_config.half[slot]) {
        /* A stock pad keeps its stock light; remember its colour. */
        if (slot < 8u && light->rgb) beatjump_native_rgb[deck] = light->rgb;
        return;
    }
    /* Our pads: ON when the jump fits in the track, DIM when it does not,
       always steady, in the BEAT JUMP colour (or led_color when set). */
    /* No colour known yet (BEAT JUMP not shown since start): stay native
       rather than light a pad black. */
    if (!beatjump_config.custom_led && !beatjump_native_rgb[deck] && !light->rgb) return;
    int possible = beatjump_dj_enable(0, deck, BEATJUMP_FIRST_VIRTUAL + slot);
    light->state = possible ? RX3_LIGHT_ON : RX3_LIGHT_DIM;
    if (beatjump_config.custom_led) light->rgb = beatjump_config.rgb;
    else if (beatjump_native_rgb[deck]) light->rgb = beatjump_native_rgb[deck];
}

/* CUE/LOOP CALL < (1/2X) and > (2X) with no loop running: a 4 beat and an
   8 beat loop, as LOOP IN held does (onKey_LoopIn @0x303cc4: setAutoBeatLoop
   with {beats, 1}, 3, false, false). With a loop running they stay stock:
   halve and double. Both DjEngineIF calls use the global PlayEngine and
   ignore `this`. */
#define BEATJUMP_CALL_NEXT 0x4322u
#define BEATJUMP_CALL_PREV 0x4323u
#define BEATJUMP_AUTO_LOOP ((unsigned long)0x00048f40)
#define BEATJUMP_IS_LOOPING ((unsigned long)0x000482e4)
static const uint8_t beatjump_auto_loop_guard[8] = {0xf0,0x4f,0x2d,0xe9,0x04,0xd0,0x4d,0xe2};
static const uint8_t beatjump_is_looping_guard[8] = {0xf8,0x40,0x2d,0xe9,0x01,0x50,0xa0,0xe1};
struct beatjump_auto_loop { int16_t beats, unit; };
typedef void (*beatjump_auto_loop_fn)(const void *, unsigned int, const struct beatjump_auto_loop *,
                                      int, int, int);
typedef int (*beatjump_is_looping_fn)(const void *, unsigned int);
static unsigned int beatjump_call_ready;
static volatile unsigned int beatjump_injecting;
#define BEATJUMP_LOOP_IN 0x410cu
static uint8_t beatjump_call_taken[2];

/* Codes come from the firmware's name table and are unconfirmed on the RX3
   (2026-10-15: CALL did nothing and no 0x4322/0x4323 reached the log). Every
   deck button seen for the first time is logged, so one test names them;
   RX3_CALL_PREV_KEY / RX3_CALL_NEXT_KEY (hex) then override. */
static unsigned int beatjump_call_next = BEATJUMP_CALL_NEXT, beatjump_call_prev = BEATJUMP_CALL_PREV;

static unsigned int beatjump_hex(const char *text, unsigned int fallback)
{
    unsigned int value = 0u, digits = 0u;
    for (; text && *text; text++, digits++) {
        unsigned int d = *text >= '0' && *text <= '9' ? (unsigned int)(*text - '0') :
                         (*text | 32) >= 'a' && (*text | 32) <= 'f' ? (unsigned int)((*text | 32) - 'a' + 10) : 16u;
        if (d > 15u || digits >= 4u) return fallback;
        value = value * 16u + d;
    }
    return digits ? value : fallback;
}

/* Why playengine::Player::setAutoBeatLoop @0x6e618 declines (RX3 2026-10-15:
   LOOP IN held sent, no loop): the fields its early exits read, logged once
   per press. The players are the PlayEngine's vector at +0xc (getBeatPosInfo
   @0x5f5bc reads it the same way); the engine is BeatEffect::getPlayEngine. */
#define BEATJUMP_GET_ENGINE ((unsigned long)0x000b5e94)
static const uint8_t beatjump_engine_guard[8] = {0xf8,0x40,0x2d,0xe9,0x9c,0x40,0x9f,0xe5};
static void beatjump_loop_diagnose(unsigned int deck)
{
#if defined(__arm__)
    if (memcmp((const void *)BEATJUMP_GET_ENGINE, beatjump_engine_guard, 8)) return;
    const uint8_t *engine = ((const uint8_t *(*)(void *))BEATJUMP_GET_ENGINE)(0);
    if (!engine) return;
    const uint8_t *const *first = *(const uint8_t *const *const *)(engine + 0xc);
    const uint8_t *const *last = *(const uint8_t *const *const *)(engine + 0x10);
    if (!first || first + deck >= last || !first[deck]) return;
    const uint8_t *player = first[deck];
    const uint8_t *p;
    framework->log_number("loop check e5e (must be 1) = ", player[0xe5e]);
    framework->log_number("loop check e98 = ", player[0xe98]);
    framework->log_number("loop check e5f (must be 0) = ", player[0xe5f]);
    memcpy(&p, player + 0xea0, 4u);
    framework->log_number("loop check ea0+8 (must be 0) = ", p ? p[8] : 255u);
    memcpy(&p, player + 0xef0, 4u);
    framework->log_number("loop check ef0+54 = ", p ? p[0x54] : 255u);
    memcpy(&p, player + 0xf14, 4u);
    framework->log_number("loop check f14+c (must be 0) = ", p ? p[0xc] : 255u);
    framework->log_number("loop check f43 (must be 0) = ", player[0xf43]);
    memcpy(&p, player + 0xc8, 4u);
    uint32_t mode = 0;
    if (p) memcpy(&mode, p + 0xb0, 4u);
    framework->log_number("loop check c8+b0 = ", mode);
#else
    (void)deck;
#endif
}

static int beatjump_call_key(const struct rx3_key_event *event)
{
    if (event->operation == 0u && event->channel >= 1u && event->channel <= 2u) {
        static uint16_t seen[48];
        static unsigned int seen_count;
        unsigned int known = 0u;
        for (unsigned int i = 0; i < seen_count; i++) known |= seen[i] == event->key;
        if (!known && seen_count < 48u) {
            seen[seen_count++] = (uint16_t)event->key;
            framework->log_number("deck button first pressed, code = ", event->key);
        }
    }
    /* LOOP IN as the player gets it, the real button's and ours, to learn the
       sequence a hold makes (RX3 2026-10-15: our hold alone made no loop). */
    if (event->key == BEATJUMP_LOOP_IN) {
        static unsigned int traced;
        if (traced++ < 24u)
            framework->log_number(beatjump_injecting ? "LOOP IN injected, operation = "
                                                     : "LOOP IN button, operation = ", event->operation);
    }
    if (beatjump_injecting) return 0;   /* our own LOOP IN / 2X go to the player */
    if (event->key != beatjump_call_next && event->key != beatjump_call_prev) return 0;
    if (event->channel < 1u || event->channel > 2u) return 0;
    unsigned int deck = event->channel - 1u;
    if ((event->operation & ~1u) == 2u) {           /* release */
        if (!beatjump_call_taken[deck]) return 0;
        /* The hand lets LOOP IN go too. */
        beatjump_injecting = 1u;
        framework->input->send_key(BEATJUMP_LOOP_IN, 2u, event->channel, 0u, 0.0f);
        beatjump_injecting = 0u;
        /* 8 beats: double the 4 beat loop once it runs. Never 2X without a
           loop: there it is CALL, which jumps to the next cue. */
        if (beatjump_call_taken[deck] == 2u) {
            if (((beatjump_is_looping_fn)BEATJUMP_IS_LOOPING)(0, deck)) {
                beatjump_injecting = 1u;
                framework->input->send_key(beatjump_call_next, 0u, event->channel, 0u, 0.0f);
                framework->input->send_key(beatjump_call_next, 2u, event->channel, 0u, 0.0f);
                beatjump_injecting = 0u;
                framework->log_line("CALL >: loop doubled to 8 beats");
            } else {
                framework->log_line("CALL >: no loop yet on release, left at 4 beats");
            }
        }
        beatjump_call_taken[deck] = 0u;
        return 1;
    }
    if (event->operation != 0u || !beatjump_call_ready) return 0;
    if (((beatjump_is_looping_fn)BEATJUMP_IS_LOOPING)(0, deck)) return 0;
    /* The direct setAutoBeatLoop call ran but made no loop on the RX3
       (2026-10-15), so do what the hand does: LOOP IN held is the 4 beat
       loop; for 8, 2X doubles it as soon as it runs. */
    int eight = event->key == beatjump_call_next;
    beatjump_injecting = 1u;
    /* A held button is a press, then the hold; the hold alone made nothing. */
    int sent = framework->input->send_key(BEATJUMP_LOOP_IN, 0u, event->channel, 0u, 0.0f) &&
               framework->input->send_key(BEATJUMP_LOOP_IN, 1u, event->channel, 0u, 0.0f);
    beatjump_injecting = 0u;
    framework->log_number("CALL without a loop: LOOP IN held sent, beats = ", eight ? 8u : 4u);
    beatjump_loop_diagnose(deck);
    framework->log_number("  sent = ", (unsigned long)sent);
    beatjump_call_taken[deck] = eight ? 2u : 1u;
    return 1;
}

/* Called on the startup thread only. */
static int beatjump_read_config(const char *path, struct beatjump_config *into)
{
    beatjump_config_defaults(into);
    int fd = path && path[0] ? open(path, O_RDONLY) : -1;
    if (fd < 0) {
        framework->log_line("beat jump: no beatjump.ini, stock pads");
        return 0;
    }
    off_t length = lseek(fd, 0, SEEK_END);
    int valid = length > 0 && (unsigned long)length <= BEATJUMP_CONFIG_MAX_BYTES &&
                lseek(fd, 0, SEEK_SET) == 0;
    char buffer[BEATJUMP_CONFIG_MAX_BYTES];
    size_t total = 0u;
    while (valid && total < (size_t)length) {
        ssize_t got = read(fd, buffer + total, (size_t)length - total);
        if (got <= 0) {
            valid = 0;
            break;
        }
        total += (size_t)got;
    }
    close(fd);
    if (!valid || !beatjump_parse_config(buffer, total, into)) {
        framework->log_line("beat jump: beatjump.ini rejected, stock pads");
        return 0;
    }
    return 1;
}

static unsigned long beatjump_object(unsigned int pad, unsigned int on)
{
    unsigned long off = (pad < 8u ? BEATJUMP_OBJECT_PAGE1 : BEATJUMP_OBJECT_PAGE2) -
                        (pad & 7u) * BEATJUMP_OBJECT_STEP;
    return (on ? off - BEATJUMP_OBJECT_ON : off) + BEATJUMP_OBJECT_IMAGE;
}

/* The glyphs of one value: "1/2" or the decimal digits of whole beats. Zero
   when a digit has no stock source (0): the pad keeps its stock image. */
static unsigned int beatjump_label_glyphs(int half, unsigned int on,
                                          unsigned int *source, uint8_t *index)
{
    unsigned int beats = (unsigned int)(half < 0 ? -half : half);
    if (beats == 1u) {
        for (unsigned int g = 0; g < 3u; g++) { source[g] = BEATJUMP_HALF_SOURCE + on; index[g] = (uint8_t)g; }
        return 3u;
    }
    if (beats & 1u) return 0u;
    beats /= 2u;
    unsigned int digits[4], count = 0;
    while (beats && count < 4u) { digits[count++] = beats % 10u; beats /= 10u; }
    if (beats) return 0u;
    for (unsigned int g = 0; g < count; g++) {
        unsigned int digit = digits[count - 1u - g];
        if (!beatjump_digit_images[digit]) return 0u;
        source[g] = beatjump_digit_images[digit] + on;
        index[g] = 0u;
    }
    return count;
}

/* Best effort: a pad whose label cannot be registered or written keeps the
   stock picture; the jump itself does not depend on it. */
static void beatjump_install_labels(void)
{
    if (!framework->images || !framework->images->register_label) return;
    for (unsigned int pad = 0; pad < BEATJUMP_PAD_COUNT; pad++) {
        int half = beatjump_config.half[pad];
        if (!half) continue;
        for (unsigned int on = 0; on < 2u; on++) {
            unsigned int source[4];
            uint8_t index[4];
            unsigned int glyphs = beatjump_label_glyphs(half, on, source, index);
            if (!glyphs) continue;
            unsigned int frame = (half < 0 ? BEATJUMP_FRAME_BACK : BEATJUMP_FRAME_FORWARD) + on;
            uint32_t id = framework->images->register_label(&beatjump_owner, frame, source,
                                                            index, glyphs);
            uint32_t stock = beatjump_stock_images[pad] + on;
            if (!id || !framework->write_guarded(beatjump_object(pad, on), &stock, &id, 4u))
                continue;
            beatjump_label_ids[pad * 2u + on] = id;
            beatjump_labelled |= 1u << (pad * 2u + on);
        }
    }
}

static void beatjump_restore_labels(void)
{
    for (unsigned int slot = 0; slot < BEATJUMP_PAD_COUNT * 2u; slot++) {
        if (!(beatjump_labelled & (1u << slot))) continue;
        uint32_t stock = beatjump_stock_images[slot / 2u] + (slot & 1u);
        if (framework->write_guarded(beatjump_object(slot / 2u, slot & 1u),
                                     &beatjump_label_ids[slot], &stock, 4u))
            beatjump_labelled &= ~(1u << slot);
    }
    if (!beatjump_labelled && framework->images && framework->images->unregister_owner)
        framework->images->unregister_owner(&beatjump_owner);
}

static int beatjump_patch_tables(void)
{
    for (unsigned int pad = 0; pad < BEATJUMP_PAD_COUNT; pad++) {
        if (!beatjump_config.half[pad]) continue;
        uint32_t wanted = BEATJUMP_FIRST_VIRTUAL + pad;
        if (!framework->write_guarded(beatjump_table_address(pad),
                                      &beatjump_stock_types[pad], &wanted, 4u)) {
            framework->log_number("beat jump: unexpected table word, pad = ", pad + 1u);
            return 0;
        }
        beatjump_patched |= 1u << pad;
    }
    return 1;
}

/* Each word goes back only while it still holds our type. */
static void beatjump_restore_tables(void)
{
    for (unsigned int pad = 0; pad < BEATJUMP_PAD_COUNT; pad++) {
        if (!(beatjump_patched & (1u << pad))) continue;
        uint32_t ours = BEATJUMP_FIRST_VIRTUAL + pad;
        if (framework->write_guarded(beatjump_table_address(pad), &ours,
                                     &beatjump_stock_types[pad], 4u))
            beatjump_patched &= ~(1u << pad);
        else
            framework->log_number("beat jump: table word not restored, pad = ", pad + 1u);
    }
}

static int configured(void)
{
    const char *value = getenv("RX3_BEATJUMP_CONFIG");
    return value && value[0] != '\0';
}

static int start(const struct rx3_services *services)
{
    framework = services;
    if (!services->install_hook || !services->detach_hook || !services->release_hook ||
        !services->write_guarded || !services->input || !services->log_line ||
        !services->log_number)
        return 0;
    beatjump_page[0] = beatjump_page[1] = 0u;
    if (!beatjump_read_config(getenv("RX3_BEATJUMP_CONFIG"), &beatjump_config))
        return 1; /* stock deck, nothing installed */
    unsigned int count = 0u;
    for (unsigned int pad = 0; pad < BEATJUMP_PAD_COUNT; pad++)
        count += beatjump_config.half[pad] != 0;
    if (!count) {
        services->log_line("beat jump: no pad configured, stock pads");
        return 1;
    }
    /* The hook first: a table word only ever names a type somebody answers. */
    beatjump_original = (beatjump_enable_fn)services->install_hook(
        &beatjump_hook, BEATJUMP_IS_ENABLE, beatjump_guard, (void *)beatjump_is_enable);
    if (!beatjump_original) {
        services->log_line("beat jump: unexpected isBeatJumpEnable prologue");
        return 0;
    }
    if (!beatjump_patch_tables()) return 0;
    if (!services->input->register_pad(&beatjump_owner, 1u, beatjump_pad) ||
        !services->input->register_lights(&beatjump_owner, RX3_LIGHTS_BEAT_JUMP,
                                          beatjump_light)) {
        beatjump_input = 1u; /* whatever got registered is released by stop */
        services->log_line("beat jump: pad lights unavailable");
        return 0;
    }
    beatjump_input = 1u;
#if defined(__arm__)
    beatjump_call_ready = !memcmp((const void *)BEATJUMP_AUTO_LOOP, beatjump_auto_loop_guard, 8) &&
                          !memcmp((const void *)BEATJUMP_IS_LOOPING, beatjump_is_looping_guard, 8);
#endif
    beatjump_call_prev = beatjump_hex(getenv("RX3_CALL_PREV_KEY"), BEATJUMP_CALL_PREV);
    beatjump_call_next = beatjump_hex(getenv("RX3_CALL_NEXT_KEY"), BEATJUMP_CALL_NEXT);
    if (beatjump_call_ready && services->input->register_key(&beatjump_owner, 6u, beatjump_call_key))
        services->log_line("CALL < > without a loop: 4 and 8 beat loop");
    beatjump_install_labels();
    services->log_number("beat jump: custom pads = ", count);
    return 1;
}

/* Accepts a partial start. The tables go back before the hook: while a word
   still names a custom type the hook stays, so the player never meets a type
   it does not know. */
static void stop(void)
{
    if (!framework) return;
    if (beatjump_input) {
        framework->input->unregister_owner(&beatjump_owner);
        beatjump_input = 0u;
    }
    beatjump_restore_labels();
    beatjump_restore_tables();
    if (beatjump_original) {
        if (beatjump_patched) {
            framework->log_line("beat jump: hook retained, a table word is still ours");
            return;
        }
        if (!framework->detach_hook(&beatjump_hook)) {
            framework->log_line("beat jump: hook retained after restore failure");
            return;
        }
        while (__atomic_load_n(&beatjump_calls, __ATOMIC_SEQ_CST)) usleep(1000u);
        framework->release_hook(&beatjump_hook);
        beatjump_original = 0;
    }
    beatjump_config_defaults(&beatjump_config);
}

const struct rx3_module rx3_beatjump_custom_module = {
    .version = RX3_MODULE_API_VERSION, .size = sizeof(struct rx3_module),
    .name = "beatjump-custom", .configured = configured, .start = start, .stop = stop
};

/* SPDX-License-Identifier: MPL-2.0 */
#ifndef RX3_STEMS_AUDIO_H
#define RX3_STEMS_AUDIO_H
#include "../core/api/rx3_dsp.h"
#include "../core/api/rx3_audio_api.h"

/* Bits: 1 HARMONY (the residual: mix minus the stored roles), 2 VOCAL,
   4 DRUMS, 8 BASS. A package stores 1 to 3 PCM roles, so up to 15. */
static unsigned int stems_available(const struct stems_deck_context *context)
{
    return (__atomic_load_n(&context->selection, __ATOMIC_SEQ_CST) >> 4u) & 15u;
}

static void stems_set_mask(struct stems_deck_context *context, unsigned int mask)
{
    uint32_t levels=0;
    for(unsigned int i=0;i<4u;i++)
        if(mask & (1u<<i)) levels |= 100u << (i*7u);
    __atomic_store_n(&context->levels, levels, __ATOMIC_SEQ_CST);
}
static unsigned int stems_selected(const struct stems_deck_context *context)
{
    uint32_t levels=__atomic_load_n(&context->levels, __ATOMIC_SEQ_CST);
    unsigned int selected=0;
    for(unsigned int i=0;i<4u;i++) if((levels>>(i*7u))&127u) selected |= 1u<<i;
    return selected & stems_available(context);
}
static unsigned int stems_level(const struct stems_deck_context *context, unsigned int bit)
{
    uint32_t levels=__atomic_load_n(&context->levels, __ATOMIC_SEQ_CST);
    for(unsigned int i=0;i<4u;i++) if(bit==(1u<<i)) return (levels>>(i*7u))&127u;
    return 0;
}
/* Both physical pads and touch clients edit this same atomic word. */
static void stems_change_levels(struct stems_deck_context *context, unsigned int bits,
                               unsigned int value, int toggle)
{
    bits &= stems_available(context);
    if(!bits) return;
    if(value>100u) value=100u;
    uint32_t old=__atomic_load_n(&context->levels, __ATOMIC_SEQ_CST);
    for(;;) {
        uint32_t next=old;
        for(unsigned int i=0;i<4u;i++) if(bits & (1u<<i)) {
            unsigned int shift=i*7u;
            unsigned int level=toggle ? (((old>>shift)&127u) ? 0u : 100u) : value;
            next=(next & ~(127u<<shift)) | (level<<shift);
        }
        if(__atomic_compare_exchange_n(&context->levels,&old,next,0,
                                      __ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST)) return;
    }
}
static void stems_toggle(struct stems_deck_context *context, unsigned int bits)
{
    stems_change_levels(context,bits,0,1);
}
static void stems_set_level(struct stems_deck_context *context, unsigned int bit,
                           unsigned int value)
{
    stems_change_levels(context,bit,value,0);
}

static void stems_reset_mix(struct stems_deck_context *context)
{
    for (unsigned int i = 0; i < 4u; i++)
        context->gain[i] = context->from[i] = context->target[i] = 1.0f;
    context->transition_cursor = 256u;
    stems_set_mask(context, context->selection & 15u);
}

/* Diagnostics, read by the watcher thread: samples the echo line clipped,
   output frames over full scale while a tail plays, and the delay used. */
static volatile unsigned int stems_echo_clipped, stems_echo_hot, stems_echo_last_delay;

/* The echo line keeps 6 dB of headroom (half scale): a role plus its feedback
   went over full scale hundreds of times a second (log 2026-10-22) and the
   clipped samples came back in every repeat as crackle. */
static int16_t stems_echo_sample(float value)
{
    float scaled = value * 16384.0f;
    if (scaled >= 32767.0f) { stems_echo_clipped++; return 32767; }
    if (scaled <= -32768.0f) { stems_echo_clipped++; return -32768; }
    return (int16_t)scaled;
}

/* The deck's echo lines, or null when they were never allocated. */
static struct stems_echo_deck *stems_echo_of(const struct stems_deck_context *context)
{
    unsigned int deck = (unsigned int)(context - stems_decks);
    if (deck >= 2u || !stems_echo[deck].buffer[0]) return 0;
    return &stems_echo[deck];
}

/* The lines of the stream being mixed: the deck's own, or its second stream's. */
static struct stems_echo_deck *stems_echo_lines(const struct stems_deck_context *context)
{
    struct stems_echo_deck *control = stems_echo_of(context);
    if (!control || !stems_stream_index) return control;
    struct stems_echo_deck *alt = &stems_echo_alt[control - stems_echo];
    return alt->buffer[0] ? alt : 0;
}

/* Audio thread: whether a tail still needs blocks, silent ones included. */
static int stems_echo_busy(const struct stems_deck_context *context)
{
    const struct stems_echo_deck *echo = stems_echo_of(context);
    return echo && (echo->running || stems_echo_alt[echo - stems_echo].running ||
                    __atomic_load_n(&echo->tails, __ATOMIC_SEQ_CST));
}

/* Audio thread, once per block: start the tails asked for, fade out the
   ones taken back. */
static void stems_echo_begin(struct stems_echo_deck *echo, struct stems_echo_deck *control,
                             unsigned int count)
{
    unsigned int tails = __atomic_load_n(&control->tails, __ATOMIC_SEQ_CST);
    echo->running = 0u;
    for (unsigned int i = 0; i <= count; i++) {
        struct stems_echo_line *line = &echo->line[i];
        int wanted = (tails >> i) & 1u;
        if (wanted && line->state != 1u) {
            if (!line->state) {
                line->low_left = line->low_right = 0.0f;
                line->repeats = 0u;
            }
            line->state = 1u;
            line->fade = 1.0f;
            line->peak = 0.0f;
        } else if (!wanted && line->state == 1u) {
            line->state = 2u;
        }
        if (line->state) echo->running++;
    }
}

/* One output frame through every line. `in` is what each role plays now
   (its gain applied). Returns the tails' sum to add to the output. */
static struct rx3_stereo stems_echo_frame(struct stems_echo_deck *echo, struct stems_echo_deck *control,
                                          unsigned int count, unsigned int delay,
                                          const struct rx3_stereo *in)
{
    struct rx3_stereo wet = {0.0f, 0.0f};
    unsigned int write = echo->cursor;
    unsigned int read = write >= delay ? write - delay : write + STEMS_ECHO_FRAMES - delay;
    for (unsigned int i = 0; i <= count; i++) {
        Short2 *buffer = echo->buffer[i];
        struct stems_echo_line *line = &echo->line[i];
        float left = in[i].left, right = in[i].right;
        if (line->state) {
            float y_left = (float)buffer[read].left * (1.0f / 16384.0f);
            float y_right = (float)buffer[read].right * (1.0f / 16384.0f);
            wet.left += y_left * STEMS_ECHO_WET * line->fade;
            wet.right += y_right * STEMS_ECHO_WET * line->fade;
            line->low_left += (y_left - line->low_left) * STEMS_ECHO_DAMPING;
            line->low_right += (y_right - line->low_right) * STEMS_ECHO_DAMPING;
            left += line->low_left * STEMS_ECHO_FEEDBACK;
            right += line->low_right * STEMS_ECHO_FEEDBACK;
            float a = y_left < 0.0f ? -y_left : y_left, b = y_right < 0.0f ? -y_right : y_right;
            if (a > line->peak) line->peak = a;
            if (b > line->peak) line->peak = b;
            if (line->state == 2u) {
                line->fade -= 1.0f / (float)TRANSITION_FRAMES;
                if (line->fade <= 0.0f) {
                    line->state = 0u;
                    line->fade = 0.0f;
                    echo->running--;
                }
            }
        }
        buffer[write].left = stems_echo_sample(left);
        buffer[write].right = stems_echo_sample(right);
    }
    echo->cursor = write + 1u >= STEMS_ECHO_FRAMES ? 0u : write + 1u;
    /* Once per repeat: a tail whose whole repeat stayed silent is over. */
    if (++echo->window >= delay) {
        echo->window = 0u;
        for (unsigned int i = 0; i <= count; i++) {
            struct stems_echo_line *line = &echo->line[i];
            if (line->state == 1u && (line->peak < STEMS_ECHO_SILENT ||
                                      ++line->repeats >= STEMS_ECHO_REPEATS)) {
                line->state = 0u;
                echo->running--;
                __atomic_fetch_and(&control->tails, ~(1u << i), __ATOMIC_SEQ_CST);
            }
            line->peak = 0.0f;
        }
    }
    return wet;
}

#define STEMS_SOFT_KNEE 0.95f
/* Soft knee above STEMS_SOFT_KNEE of full scale, for a signal the module itself changed: a
   sum of separated parts can reach 0 dBFS (log 2026-10-22, peak 1000/1000) and a
   hard clip there is heard as crackle. It is never applied to a block whose
   gains are all at unity: that block is the player's own audio, bit for bit.
   (Applied to every block it flattened the peaks of any loud track as soon as
   its stems were loaded, which was heard as constant crackle.) */
#ifdef RX3_STEMS_NO_LIMITER /* host tests that compare the mix with its exact arithmetic */
static inline float stems_soft(float x) { return x; }
#else
static inline float stems_soft(float x)
{
    float a = x < 0.0f ? -x : x;
    if (a <= STEMS_SOFT_KNEE) return x;
    float over = a - STEMS_SOFT_KNEE, room = 1.0f - STEMS_SOFT_KNEE;
    float y = STEMS_SOFT_KNEE + room * over / (over + room);
    return x < 0.0f ? -y : y;
}
#endif

/* A request this small is the player scrubbing or crawling (log 2026-10-23: a paused deck at
   0.04x was asked for 1 frame, 42,000 times a second, and each call ran the whole mixer
   setup: the console went over budget, the screen slowed, the audio crackled). */
#define STEMS_TINY_FRAMES 32u

/* Every part at full level, no fade running, no echo tail: the block is the player's own. */
static int stems_unity(const struct stems_deck_context *context)
{
    unsigned int count = context->payload_count;
    if (!count || count > 3u || context->transition_cursor < 256u) return 0;
    uint32_t levels = __atomic_load_n(&context->levels, __ATOMIC_SEQ_CST);
    for (unsigned int i = 0; i <= count; i++)
        if (((levels >> (i * 7u)) & 127u) != 100u) return 0;
    return !stems_echo_busy(context);
}

/* With MASTER TEMPO off the player's scratch stretch reads the source in windows of 1 to 25
   frames that slide by about one frame per output sample: on the order of 44,000 requests a
   second per deck, each overlapping the one before. Mixing every window afresh is 25 times the
   work of the same audio in blocks, and with two stem decks it is more than the audio thread
   has (crackle, slow screen). What a window adds to the player's own samples depends only on
   its position and the levels: sum over the roles of (level - residual) * role. That part is
   computed once per segment and kept, and each window is then its own samples times the
   residual plus a lookup. */
#define STEMS_DELTA_SEGMENT 512u
#define STEMS_DELTA_SLOTS 4u
struct stems_delta_slot {
    const void *data;
    uint32_t levels, generation;
    int number;
    unsigned int frames;
    struct rx3_stereo delta[STEMS_DELTA_SEGMENT];
};
static struct stems_delta_slot stems_delta_slots[2][STEMS_DELTA_SLOTS];

static const struct stems_delta_slot *stems_delta_get(unsigned int deck, struct stems_deck_context *context,
                                                      int number, uint32_t levels, uint64_t limit,
                                                      unsigned int count, const float *scale,
                                                      const Short2 *const *base)
{
    struct stems_delta_slot *slot = &stems_delta_slots[deck][(unsigned int)number % STEMS_DELTA_SLOTS];
    uint64_t first = (uint64_t)(unsigned int)number * STEMS_DELTA_SEGMENT;
    uint64_t want = limit > first ? limit - first : 0u;
    if (want > STEMS_DELTA_SEGMENT) want = STEMS_DELTA_SEGMENT;
    uint32_t generation = __atomic_load_n(&context->generation, __ATOMIC_SEQ_CST);
    if (slot->data == (const void *)base[0] && slot->generation == generation && slot->levels == levels &&
        slot->number == number &&
        slot->frames >= (unsigned int)want) return slot;
    float residual = context->gain[0];
    float gain[3];
    for (unsigned int j = 0; j < count; j++)
        gain[j] = (context->gain[j + 1u] - residual) * (1.0f / 32768.0f);
    for (unsigned int i = 0; i < (unsigned int)want; i++) {
        float left = 0.0f, right = 0.0f;
        for (unsigned int j = 0; j < count; j++) {
            const Short2 *pcm = base[j] + (unsigned int)first + i;
            left += (float)pcm->left * scale[j] * gain[j];
            right += (float)pcm->right * scale[j] * gain[j];
        }
        slot->delta[i].left = left;
        slot->delta[i].right = right;
    }
    slot->data = (const void *)base[0];
    slot->levels = levels;
    slot->generation = generation;
    slot->number = number;
    slot->frames = (unsigned int)want;
    return slot;
}

static void stems_mix(struct stems_deck_context *context, int position,
                        struct rx3_stereo *output, unsigned int frames)
{
#ifdef RX3_OVERCUE_PROTOTYPE
    if(context->overcue) {
        if(position>=0)rx3_overcue_render((unsigned int)(context-stems_decks),
            (unsigned int)position,output,frames,stems_selected(context));
        return;
    }
#endif
    unsigned int count = context->payload_count;
    if (!output || !frames || !count || count > 3u || !context->payloads[0].data) return;
    /* The stems are read at the very position the player asked for, in the same
       domain as the audio in `output` (this stage sits in front of the stretch
       that follows), so a block that does not follow the last one needs no
       treatment: the mix and the stems jump together. A fade toward where the
       stems "would have gone" (the old seam fade) put the stems out of step with
       the audio for 64 frames at every such block, which with a pitch or tempo
       change is almost every block. */
    uint32_t levels = __atomic_load_n(&context->levels, __ATOMIC_SEQ_CST);
    float target[4];
    int changed = 0, all = 1;
    for (unsigned int i = 0; i <= count; i++) {
        /* Role i+1 is payload i; BASS (payload 2) has its own level. */
        target[i] = (float)((levels >> (i * 7u)) & 127u) * 0.01f;
        if (target[i] != context->target[i]) changed = 1;
        if (target[i] != 1.0f) all = 0;
    }
    /* With echo lines the loop always runs: a line records its role even
       at full mix, so the last beat is there when a pad asks for it. */
    struct stems_echo_deck *control = stems_echo_of(context);
    struct stems_echo_deck *echo = stems_echo_lines(context);
    /* Scrub-sized requests do not feed the echo lines (their beat is not a beat); a tail that
       is already playing still needs every frame, so it keeps the full path. */
    if (frames < STEMS_TINY_FRAMES && echo && !stems_echo_busy(context)) echo = 0;
    if (changed) {
        for (unsigned int i = 0; i <= count; i++) {
            context->from[i] = context->gain[i];
            context->target[i] = target[i];
        }
        context->transition_cursor = 0u;
    } else if (context->transition_cursor >= 256u) {
        for (unsigned int i = 0; i <= count; i++) context->gain[i] = target[i];
        if (all && !echo) return;
    }
    unsigned int delay = 0u;
    if (echo) {
        stems_echo_begin(echo, control, count);
        delay = __atomic_load_n(&control->delay, __ATOMIC_SEQ_CST);
        if (delay < 1u || delay >= STEMS_ECHO_FRAMES) delay = STEMS_ECHO_DEFAULT_DELAY;
        stems_echo_last_delay = delay;
    }
    /* Gains away from unity, or still moving: the block is the module's, not the player's. */
    int touched = !all || context->transition_cursor < 256u;
    uint64_t limit = context->payloads[0].frames;
    for (unsigned int i = 1; i < count; i++)
        if (context->payloads[i].frames < limit) limit = context->payloads[i].frames;
    if (__atomic_load_n(&context->streaming, __ATOMIC_ACQUIRE)) {
        unsigned int loaded = __atomic_load_n(&context->loaded, __ATOMIC_ACQUIRE);
        if ((uint64_t)loaded < limit) limit = loaded;
    }
    /* Per-payload constants, hoisted out of the per-frame loop: the format
       scale and the PCM base pointer do not change inside a block. */
    float scale[3];
    const Short2 *base[3];
    for (unsigned int j = 0; j < count; j++) {
        scale[j] = context->payloads[j].format == FORMAT_S16_GAIN ? context->payloads[j].pcm_gain : 1.0f;
        base[j] = (const Short2 *)context->payloads[j].data;
    }
    /* Scratch-sized window, gains settled, no echo: from the per-segment sum (see above). */
    if (frames <= STEMS_TINY_FRAMES && !echo && context->transition_cursor >= 256u &&
        context->gain[0] == target[0] && context->payloads[0].data) {
        unsigned int deck = (unsigned int)(context - stems_decks);
        float residual = context->gain[0];
        for (unsigned int i = 0; i < frames; i++) {
            int index = (int)((uint32_t)position + i);
            if (index < 0 || (uint64_t)(unsigned int)index >= limit ||
                (output[i].left == 0.0f && output[i].right == 0.0f)) continue;
            const struct stems_delta_slot *slot = stems_delta_get(deck, context,
                index / (int)STEMS_DELTA_SEGMENT, levels, limit, count, scale, base);
            const struct rx3_stereo *delta = &slot->delta[(unsigned int)index % STEMS_DELTA_SEGMENT];
            output[i].left = stems_soft(output[i].left * residual + delta->left);
            output[i].right = stems_soft(output[i].right * residual + delta->right);
        }
        return;
    }
    for (unsigned int i = 0; i < frames; i++) {
        struct rx3_stereo in[4] = {{0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}};
        int index = (int)((uint32_t)position + i);
        int dry = index >= 0 && (uint64_t)(unsigned int)index < limit &&
            (output[i].left != 0.0f || output[i].right != 0.0f);
        if (!dry && !echo) continue;
        struct rx3_stereo mixed = output[i];
        if (dry) {
            if (context->transition_cursor < 256u) {
                float alpha = (float)++context->transition_cursor * (1.0f / 256.0f);
                for (unsigned int j = 0; j <= count; j++)
                    context->gain[j] = rx3_dsp_lerp(context->from[j], context->target[j], alpha);
            }
            float residual = context->gain[0];
            struct rx3_stereo rest = output[i];
            mixed.left = output[i].left * residual;
            mixed.right = output[i].right * residual;
            for (unsigned int j = 0; j < count; j++) {
                const Short2 *pcm = base[j] + (unsigned int)index;
                float gain = (context->gain[j + 1u] - residual) * (1.0f / 32768.0f);
                float left = (float)pcm->left * scale[j], right = (float)pcm->right * scale[j];
                mixed.left += left * gain;
                mixed.right += right * gain;
                if (echo) {
                    /* What this role plays now, for its line. */
                    float level = context->gain[j + 1u] * (1.0f / 32768.0f);
                    in[j + 1u].left = left * level;
                    in[j + 1u].right = right * level;
                    rest.left -= left * (1.0f / 32768.0f);
                    rest.right -= right * (1.0f / 32768.0f);
                }
            }
            in[0].left = rest.left * residual;
            in[0].right = rest.right * residual;
            if (context->transition_cursor >= 256u)
                for (unsigned int j = 0; j <= count; j++) context->gain[j] = context->target[j];
        }
        if (echo) {
            /* Exactly zero while no tail plays. */
            struct rx3_stereo wet = stems_echo_frame(echo, control, count, delay, in);
            mixed.left += wet.left;
            mixed.right += wet.right;
            if (echo->running && (mixed.left > 1.0f || mixed.left < -1.0f ||
                                  mixed.right > 1.0f || mixed.right < -1.0f))
                stems_echo_hot++;
        }
        if (touched || (echo && echo->running)) {
            mixed.left = stems_soft(mixed.left);
            mixed.right = stems_soft(mixed.right);
            output[i] = mixed;
        }
    }
}

#endif /* RX3_STEMS_AUDIO_H */

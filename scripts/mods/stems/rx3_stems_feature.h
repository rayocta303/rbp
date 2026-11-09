/* SPDX-License-Identifier: MPL-2.0 */
#ifndef RX3_STEMS_FEATURE_H
#define RX3_STEMS_FEATURE_H

#include "rx3_stems_loader.h"
#include "../core/api/rx3_thread.h"
#include "rx3_stems_monitor.h"
#include "rx3_stems_remote.h"

static int block_is_silent(const struct rx3_stereo *output, unsigned int frames)
{
    for (unsigned int i = 0; i < frames; i++)
        if (output[i].left != 0.0f || output[i].right != 0.0f)
            return 0;
    return 1;
}

/* The last block each deck mixed, kept to answer a repeat of it. */
#define STEMS_REPEAT_FRAMES 2048u
struct stems_repeat {
    const void *reader;
    int position;
    unsigned int frames;
    uint32_t levels;
    struct rx3_stereo block[STEMS_REPEAT_FRAMES];
};
static struct stems_repeat stems_repeats[4];   /* per deck and stream (deck * 2 + stream) */

static void stems_repeat_store(unsigned int deck, const struct stems_deck_context *context,
                               const void *reader, int position,
                               const struct rx3_stereo *output, unsigned int frames)
{
    struct stems_repeat *r = &stems_repeats[deck];
    r->frames = 0u;
    if (frames > STEMS_REPEAT_FRAMES) return;
    for (unsigned int i = 0; i < frames; i++) r->block[i] = output[i];
    r->reader = reader;
    r->position = position;
    r->levels = __atomic_load_n(&context->levels, __ATOMIC_SEQ_CST);
    r->frames = frames;
}

/* A repeat reads the same PCM as the block before it, so with the levels
   unchanged its mix is the stored one. */
static int stems_repeat_serve(unsigned int deck, const struct stems_deck_context *context,
                              const void *reader, int position,
                              struct rx3_stereo *output, unsigned int frames)
{
    const struct stems_repeat *r = &stems_repeats[deck];
    if (!r->frames || r->frames != frames || r->reader != reader || r->position != position ||
        r->levels != __atomic_load_n(&context->levels, __ATOMIC_SEQ_CST)) return 0;
    for (unsigned int i = 0; i < frames; i++) output[i] = r->block[i];
    return 1;
}

/* Which of a deck's two interleaved streams a block belongs to (see stems_echo_alt). The two share
   the player object, so the only thing that tells them apart is where each continues: a block
   that starts where a stream's last one ended is that stream's. A block that continues neither
   is a seek of the one stream, or the first block of a second one: the next block says which (it
   continues the old position in the second case). Once there are two, a block that continues
   neither goes to the stream that did not ask last. A stream not seen for STEMS_STREAM_STALE
   blocks is gone. */
#define STEMS_STREAM_STALE 8u
struct stems_stream_track {
    const void *reader;
    int next[2], prev_next0, suspect_end;
    unsigned int valid[2], age[2], last_slot, suspect;
};
static struct stems_stream_track stems_tracks[2];

static unsigned int stems_stream_slot(unsigned int deck, const void *reader, int position,
                                      unsigned int frames)
{
    struct stems_stream_track *t = &stems_tracks[deck];
    if (t->reader != reader) {
        memset(t, 0, sizeof(*t));
        t->reader = reader;
    }
    int end = position + (int)frames;
    unsigned int match = 2u, slot;
    if (t->valid[0] && position == t->next[0]) match = 0u;
    else if (t->valid[1] && position == t->next[1]) match = 1u;
    t->age[0]++;
    t->age[1]++;
    if (match == 2u && t->suspect && position == t->prev_next0) {
        /* The block before was another stream's, not a seek: now there are two. */
        t->valid[1] = 1u;
        t->next[1] = t->suspect_end;
        t->age[1] = 0u;
        t->suspect = 0u;
        slot = 0u;
    } else if (match != 2u) {
        t->suspect = 0u;
        slot = match;
    } else if (t->valid[1] && t->age[1] <= STEMS_STREAM_STALE) {
        slot = 1u - t->last_slot;
    } else {
        t->suspect = t->valid[0];
        t->prev_next0 = t->next[0];
        t->suspect_end = end;
        t->valid[1] = 0u;
        slot = 0u;
    }
    t->next[slot] = end;
    t->age[slot] = 0u;
    t->valid[slot] = 1u;
    t->last_slot = slot;
    if (t->valid[1] && t->age[1] > STEMS_STREAM_STALE) t->valid[1] = 0u;
    return slot;
}

/* Audio thread, after the deck's TimeStretch stage. The reader check keeps a
   stale request from mixing into the next track. */
static void stems_stream(unsigned int deck, const void *reader, int position,
                         struct rx3_stereo *output, unsigned int frames)
{
    if (deck >= 2u || !__atomic_load_n(&stems_callbacks_enabled, __ATOMIC_SEQ_CST)) return;
    struct stems_deck_context *context = &stems_decks[deck];
    __sync_add_and_fetch(&context->readers_active, 1u);
    /* A silent block (pause, cue) still runs while an echo tail plays. */
    if (__atomic_load_n(&context->reader, __ATOMIC_SEQ_CST) == reader) {
        unsigned int stream = stems_stream_slot(deck, reader, position, frames), slot = deck * 2u + stream;
        stems_stream_index = stream;
        /* A tiny request on a deck whose parts are all up: nothing to do, and no bookkeeping
           either (it was most of the console's CPU with a paused deck, see stems_unity). */
        if (frames < STEMS_TINY_FRAMES && stems_unity(context)) {
            stems_monitor_tiny(deck);
            __sync_sub_and_fetch(&context->readers_active, 1u);
            return;
        }
        int silent = block_is_silent(output, frames);
        /* A paused deck's stream is asked again and again for the same
           position (about 44,000 times a second on the RX3, log 2026-10-15),
           against some 700 blocks of playback, and with the pitch away from 0
           the time stretch asks for a block again while playing. Mixing each
           repeat used the audio thread's whole budget; passing it through
           unmixed let the muted parts back in for that block (the crackle
           with pitch/MASTER TEMPO). A repeat gets the stored mix instead; a
           silent one still feeds a playing echo tail. Each stream has its own. */
        if (stems_monitor_repeat(deck, stream, reader, position, frames) && !silent &&
            stems_repeat_serve(slot, context, reader, position, output, frames)) {
            stems_monitor_skipped(deck);
        } else if (!silent || stems_echo_busy(context)) {
            if (stems_echo_busy(context)) stems_monitor_echo(deck, stream, position, frames);
            uint64_t started = stems_monitor_block(deck, stream, reader, position, frames);
            stems_mix(context, position, output, frames);
            stems_monitor_mixed(deck, stream, started);
            stems_repeat_store(slot, context, reader, position, output, frames);
        }
    }
    __sync_sub_and_fetch(&context->readers_active, 1u);
}

/* Half a beat of this deck, in output frames, from its BPM (the repeats run
   together as a smooth "ooo" instead of a spoken "ciao ciao"); 250 ms without
   one. Input thread only: the accessor is the player's UI-side reader. */
static unsigned int stems_echo_delay_for(unsigned int deck)
{
    stems_bpm_fn reader = stems_bpm_reader;
    if (reader) {
        int bpm = reader(deck);
        if (bpm >= 4000 && bpm <= 30000)
            return (44100u * 30u * 100u + (unsigned int)bpm / 2u) / (unsigned int)bpm;
    }
    return STEMS_ECHO_DEFAULT_DELAY;
}

/* Echo Out on a role: mute it with its tail, or bring it back. Without
   echo lines (no memory at install) the pad is a plain mute. */
static void stems_echo_pad(unsigned int deck, unsigned int bit)
{
    struct stems_deck_context *context = &stems_decks[deck];
    struct stems_echo_deck *echo = &stems_echo[deck];
    if (!(stems_available(context) & bit)) return;
    if (!echo->buffer[0]) {
        stems_toggle(context, bit);
        return;
    }
    unsigned int tails = __atomic_load_n(&echo->tails, __ATOMIC_SEQ_CST);
    if (stems_level(context, bit) && !(tails & bit)) {
        /* A playing tail keeps the delay it started with. */
        if (!tails) __atomic_store_n(&echo->delay, stems_echo_delay_for(deck), __ATOMIC_SEQ_CST);
        __atomic_fetch_or(&echo->tails, bit, __ATOMIC_SEQ_CST);
        stems_set_level(context, bit, 0u);
    } else {
        __atomic_fetch_and(&echo->tails, ~bit, __ATOMIC_SEQ_CST);
        stems_set_level(context, bit, 100u);
    }
}

/* SLIP LOOP, normal page, once the set is resident (stems_display_order):
   pads 1 to 4 mute and enable VOCAL, HARMONY, DRUMS, BASS; pads 5 to 8 are
   Echo Out of the part above. Muting or enabling a part from its upper pad
   also ends its tail. On a deck with stems all eight pads are taken, even
   for a role the package lacks, so a stem deck never fires a native slip
   loop by surprise. On the SHIFT page (RELEASE FX) the pads stay native.
   A captured press also takes its release, even if the page changed since,
   so the native loop never sees half of a gesture. */
#define STEMS_PAD_FIRST 0x4117u
#define STEMS_PAD_LAST  0x411eu
static int stems_deck_ready(const struct stems_deck_context *context)
{
    return __atomic_load_n(&context->status, __ATOMIC_SEQ_CST) == 2u &&
        __atomic_load_n(&context->reader, __ATOMIC_SEQ_CST) &&
        __atomic_load_n(&context->armed, __ATOMIC_SEQ_CST);
}

static int stems_pad(const struct rx3_pad_event *event)
{
    unsigned int code = event->code;
    if (!__atomic_load_n(&stems_callbacks_enabled, __ATOMIC_SEQ_CST) ||
        code < STEMS_PAD_FIRST || code > STEMS_PAD_LAST || event->deck >= 2u) return 0;
    unsigned int operation = event->operation;
    unsigned int deck = event->deck, pad = code - STEMS_PAD_FIRST, captured = 1u << pad;
    struct stems_deck_context *context = &stems_decks[deck];
    if (!operation && event->pad_mode == RX3_PAD_SLIP_LOOP && event->pad_page == 0u &&
        stems_deck_ready(context)) {
        unsigned int bit = stems_display_order[pad & 3u];
        __atomic_fetch_or(&captured_pad_mask[deck], captured, __ATOMIC_SEQ_CST);
        if (pad < 4u) {
            __atomic_fetch_and(&stems_echo[deck].tails, ~bit, __ATOMIC_SEQ_CST);
            stems_toggle(context, bit);
        } else {
            stems_echo_pad(deck, bit);
        }
        return 1;
    }
    if (__atomic_load_n(&captured_pad_mask[deck], __ATOMIC_SEQ_CST) & captured) {
        if (operation == 2u || operation == 3u)
            __atomic_fetch_and(&captured_pad_mask[deck], ~captured, __ATOMIC_SEQ_CST);
        return 1;
    }
    return 0;
}

#define STEMS_KEY_CUE 0x4102u
/* The channel's headphone CUE on the mixer: what "SHIFT + CUE of the
   channel" means on the RX3 (user, 2026-10-03). Channel 1/2 as the deck. */
#define STEMS_KEY_CHANNEL_CUE 0x5020u
#define STEMS_PANEL_ID 2u

/* Set at install when the core offers send_key: without it the native
   Isolator cannot be made flat, and SHIFT+CUE only opens the panel. */
static unsigned int stems_iso_enabled;
/* The STEMS panel was opened by entering STEMS on a channel, so leaving
   STEMS on the last channel closes it again; opening it by hand clears it. */
static volatile unsigned int stems_panel_auto;
static unsigned int stems_cue_down_ms[4];
#define STEMS_LONG_PRESS_MS 500u
#define STEMS_NOTICE_ID 0x150u

/* A deck whose stems are loaded or loading: its channel can be STEMS. */
static int stems_deck_has_stems(unsigned int deck)
{
    const struct stems_deck_context *context = &stems_decks[deck];
    unsigned int status = __atomic_load_n(&context->status, __ATOMIC_SEQ_CST);
    return (status == 1u || status == 2u) &&
        __atomic_load_n(&context->reader, __ATOMIC_SEQ_CST) &&
        __atomic_load_n(&context->armed, __ATOMIC_SEQ_CST);
}

static void stems_send_knob(unsigned int key, unsigned int channel, unsigned int raw)
{
    framework->input->send_key(key, 5u, channel, raw, (float)raw * (1.0f / 1023.0f));
}

/* Key thread only. Brings the native Isolator of a channel in line with
   its mode: flat while the channel is STEMS on a deck with stems, at the
   knobs' last positions otherwise. Runs on every key of that channel, so a
   new track on a STEMS channel settles at its first knob, CUE or PLAY. */
static void stems_iso_reconcile(unsigned int channel)
{
    unsigned int deck = channel - 1u, bit = 1u << deck;
    int want = (__atomic_load_n(&stems_channel_stems, __ATOMIC_SEQ_CST) & bit) &&
        stems_deck_has_stems(deck);
    int flat = (__atomic_load_n(&stems_channel_flat, __ATOMIC_SEQ_CST) & bit) != 0u;
    if (want && !flat) {
        for (unsigned int k = 0; k < 3u; k++)
            stems_send_knob(STEMS_KNOB_HI + k, channel, STEMS_KNOB_FLAT);
        __atomic_fetch_or(&stems_channel_flat, bit, __ATOMIC_SEQ_CST);
        stems_apply_knobs(deck, (1u << STEMS_KNOB_COUNT) - 1u);
    } else if (!want && flat) {
        for (unsigned int k = 0; k < 3u; k++)
            stems_send_knob(STEMS_KNOB_HI + k, channel, stems_knob_position(deck, k));
        /* COLOR was taken only with the COLOR = DRUMS option, and only once
           it had moved: give the player where it is now. */
        if (stems_iso_color &&
            (__atomic_load_n(&stems_knob_raw[deck][3], __ATOMIC_SEQ_CST) & STEMS_KNOB_KNOWN))
            stems_send_knob(STEMS_KNOB_COLOR, channel, stems_knob_position(deck, 3));
        __atomic_fetch_and(&stems_channel_flat, ~bit, __ATOMIC_SEQ_CST);
        stems_change_levels(&stems_decks[deck], 15u, 100u, 0);
    }
}

/* "CH1 STEMS" / "CH1 ISO" on that deck's half of the screen. */
static void stems_iso_notice(unsigned int channel, int stems, int ready)
{
    if (!framework->notices || !framework->notices->post) return;
    static uint16_t text[2][40];
    static const char suffix[] = " - NO STEMS, EQ ON";
    uint16_t *out = text[channel - 1u];
    unsigned int n = 0u;
    out[n++] = 'C'; out[n++] = 'H'; out[n++] = (uint16_t)('0' + channel); out[n++] = ' ';
    const char *mode = stems ? "STEMS" : "ISO";
    for (unsigned int i = 0; mode[i]; i++) out[n++] = (uint16_t)mode[i];
    if (stems && !ready)
        for (unsigned int i = 0; suffix[i]; i++) out[n++] = (uint16_t)suffix[i];
    out[n] = 0;
    const struct rx3_notice notice = {stems_cue_down_ms, STEMS_NOTICE_ID + channel,
                                      channel - 1u, RX3_NOTICE_INFO, 1500u, out};
    (void)framework->notices->post(&notice);
}

static void stems_toggle_panel(void)
{
    __atomic_store_n(&stems_panel_auto, 0u, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&stems_panel_shown, __ATOMIC_SEQ_CST)) framework->panels->close();
    else framework->panels->open(STEMS_PANEL_ID);
}

/* SHIFT+CUE, short: this channel ISOLATOR <-> STEMS. */
static void stems_iso_toggle(unsigned int channel)
{
    unsigned int deck = channel - 1u, bit = 1u << deck;
    unsigned int now = __atomic_xor_fetch(&stems_channel_stems, bit, __ATOMIC_SEQ_CST) & bit;
    stems_iso_reconcile(channel);
    stems_iso_notice(channel, now != 0u, stems_deck_has_stems(deck));
    if (now) {
        if (!__atomic_load_n(&stems_panel_shown, __ATOMIC_SEQ_CST) &&
            framework->panels->open(STEMS_PANEL_ID))
            __atomic_store_n(&stems_panel_auto, 1u, __ATOMIC_SEQ_CST);
    } else if (!__atomic_load_n(&stems_channel_stems, __ATOMIC_SEQ_CST) &&
               __atomic_exchange_n(&stems_panel_auto, 0u, __ATOMIC_SEQ_CST) &&
               __atomic_load_n(&stems_panel_shown, __ATOMIC_SEQ_CST)) {
        framework->panels->close();
    }
}

/* SHIFT+CUE (analysis 03-11 §1.6): released before 500 ms it switches the
   deck's channel between ISOLATOR and STEMS (entering STEMS opens the STEMS
   panel, leaving closes it if that opened it); held 500 ms or longer it
   opens or closes the STEMS panel, as SHIFT+CUE did before. The press and
   its release are both taken, so the deck never sets or jumps to a cue.
   Both the deck CUE (0x4102) and the channel's headphone CUE (0x5020) are
   taken with SHIFT; which one the RX3 sends is read from the button trace
   (DA_FIXARE/05). */
static int stems_shift_cue(const struct rx3_key_event *event)
{
    unsigned int channel = event->channel, bit = 1u << (channel & 3u);
    if (event->operation == 0u) {
        /* The channel CUE sits on the mixer: SHIFT of either deck counts. */
        int shifted = framework->input->shift_held(channel) ||
            (event->key == STEMS_KEY_CHANNEL_CUE && framework->input->shift_held(RX3_NO_DECK));
        if (!shifted) return 0;
        __atomic_fetch_or(&stems_cue_captured, bit, __ATOMIC_SEQ_CST);
        stems_cue_down_ms[channel & 3u] = now_ms();
        return 1;
    }
    if (!(__atomic_load_n(&stems_cue_captured, __ATOMIC_SEQ_CST) & bit)) return 0;
    if (event->operation == 2u || event->operation == 3u) {
        __atomic_fetch_and(&stems_cue_captured, ~bit, __ATOMIC_SEQ_CST);
        unsigned int held = now_ms() - stems_cue_down_ms[channel & 3u];
        if (stems_iso_enabled && channel >= 1u && channel <= 2u && held < STEMS_LONG_PRESS_MS)
            stems_iso_toggle(channel);
        else
            stems_toggle_panel();
    }
    return 1;
}

/* Every key on the panel thread: SHIFT+CUE, and the knobs of a channel.
   Knob positions are always remembered; a channel in STEMS takes its knob
   events and turns them into stem levels. A channel whose deck has no
   stems is left to the native Isolator (its EQ is restored first). */
static int stems_key(const struct rx3_key_event *event)
{
    if (!__atomic_load_n(&stems_callbacks_enabled, __ATOMIC_SEQ_CST)) return 0;
    if (event->key == STEMS_KEY_CUE || event->key == STEMS_KEY_CHANNEL_CUE)
        return stems_shift_cue(event);
    unsigned int channel = event->channel;
    if (!stems_iso_enabled || channel < 1u || channel > 2u) return 0;
    unsigned int deck = channel - 1u;
    int knob = stems_knob_of(event->key);
    int moved = knob >= 0 && event->operation == 5u;
    if (moved)
        __atomic_store_n(&stems_knob_raw[deck][knob], (event->raw & 1023u) | STEMS_KNOB_KNOWN,
                         __ATOMIC_SEQ_CST);
    /* Solo quando si muove una manopola dell'Isolator: farlo su ogni tasto del
       canale mandava tre comandi di manopola prima del PLAY dopo un cambio
       brano, e la canzone partiva in ritardo (RX3, 2026-10-14). */
    if (moved) stems_iso_reconcile(channel);
    if (!moved || (knob == 3 && !stems_iso_color) ||
        !(__atomic_load_n(&stems_channel_flat, __ATOMIC_SEQ_CST) & (1u << deck))) return 0;
    stems_apply_knobs(deck, 1u << (unsigned int)knob);
    return 1;
}

/* SLIP LOOP pads 1 to 4 blink in their role colour while the set loads,
   then hold it, dimmed when the role is off. Pads 5 to 8 take the colour of
   the part above: blinking while its echo tail plays, dimmed at rest. A
   role the package lacks is dark on a stem deck. Without stems every pad
   stays native. */
static void stems_light(unsigned int deck, unsigned int control, struct rx3_light *light)
{
    if (deck >= 2u || control > 7u) return;
    struct stems_deck_context *context = &stems_decks[deck];
    if (!__atomic_load_n(&stems_callbacks_enabled, __ATOMIC_SEQ_CST) ||
        !context->reader || !context->armed) return;
    unsigned int pad = control & 3u, bit = stems_display_order[pad];
    const struct stems_rgb *c = &stems_pad_colour[pad];
    light->rgb = ((uint32_t)c->red << 16u) | ((uint32_t)c->green << 8u) | c->blue;
    if (!context->payloads[0].data)
        light->state = control < 4u && (stems_available(context) & bit) ? RX3_LIGHT_DIM
                                                                          : RX3_LIGHT_NATIVE;
    else if (!(stems_available(context) & bit)) {
        light->state = RX3_LIGHT_ON;
        light->rgb = 0u;
    } else if (control >= 4u)
        light->state = __atomic_load_n(&stems_echo[deck].tails, __ATOMIC_SEQ_CST) & bit
            ? RX3_LIGHT_BLINK : RX3_LIGHT_DIM;
    else {
        /* Isteresi: con una manopola vicino al minimo il livello oscilla tra 0
           e pochi punti, e un LED "acceso se > 0" lampeggiava (RX3, 2026-10-14). */
        static unsigned char lit[2][4] = {{1, 1, 1, 1}, {1, 1, 1, 1}};
        unsigned int level = stems_level(context, bit);
        if (level >= 8u) lit[deck][pad] = 1u;
        else if (level <= 2u) lit[deck][pad] = 0u;
        light->state = lit[deck][pad] ? RX3_LIGHT_ON : RX3_LIGHT_DIM;
    }
    if (!stems_any_deck_loading()) stems_blink_idle();
}

/* Publish values through the sharing framework, never the PCM context. */
static struct rx3_mix_state stems_mix_state(unsigned int deck)
{
    const struct stems_deck_context *context = &stems_decks[deck];
    struct rx3_mix_state state = {
        stems_selected(context), stems_available(context),
        __atomic_load_n(&context->reader, __ATOMIC_SEQ_CST) &&
        __atomic_load_n(&context->armed, __ATOMIC_SEQ_CST) &&
        __atomic_load_n(&context->payloads[0].data, __ATOMIC_SEQ_CST),
        __atomic_load_n(&context->levels, __ATOMIC_SEQ_CST)
    };
    return state;
}

/* Copy under the same reader barrier as PCM. Loading/unloading never exposes
   stale package pointers to the separate Stemwave module. */
static unsigned int stems_waveform(unsigned int deck, unsigned int mask,
                                    unsigned int format, uint8_t *out, unsigned int count)
{
    if(deck>=2u || format>3u || !out) return 0;
    struct stems_deck_context *context=&stems_decks[deck];
    unsigned int copied=0, stride=format>=2u?3u:format+1u;
    __sync_add_and_fetch(&context->readers_active,1u);
    if (!__atomic_load_n(&context->reader,__ATOMIC_SEQ_CST) ||
        !context->armed) goto done;
    /* Audio without prepared waveforms must retain the native display. */
    if(context->payloads[0].data && !context->payloads[0].wave) {
        copied=0xfffffffeu; goto done;
    }
    if(!context->payloads[0].wave) goto done;
    const uint8_t *wave=context->payloads[0].wave;
    const struct rx3_wave_header *header=(const void *)wave;
    if(header->version==4u && (stride!=header->stride || format==2u)) {
        copied=0xfffffffeu; goto done;
    }
    if(count!=header->count) { copied=0xffffffffu; goto done; }
    if(mask & ~stems_available(context)) goto done;
    if(!mask) { memset(out,0,count*stride); copied=count; goto done; }
    const struct rx3_wave_entry *entries=(const void *)(wave+128u);
    if(header->version==4u) {
        if(mask>header->roles) goto done;
        memcpy(out,wave+(unsigned int)entries[mask-1u].offset,count*stride);
        copied=count;
    } else if(header->version>=2u) {
        if((format==3u && header->version!=3u) || (format==2u && header->version!=2u)) goto done;
        if(mask>header->roles) goto done;
        const uint8_t *data=wave+(unsigned int)entries[mask-1u].offset;
        unsigned int offset=format==0u?0u:format==1u?1u:3u;
        for(unsigned int i=0;i<count;i++)
            memcpy(out+i*stride,data+i*6u+offset,stride);
        copied=count;
    } else {
        /* Old files only contain single-role curves in one display format.
           Never pass an envelope sum off as the waveform of a mixed signal. */
        if(stride!=header->stride || format>=2u || (mask & (mask-1u))) goto done;
        unsigned int role=mask==2u?0u:mask==1u?1u:mask==4u?2u:3u;
        if(role>=header->roles || (mask==1u && header->roles==4u)) goto done;
        memcpy(out,wave+(unsigned int)entries[role].offset,count*stride);
        copied=count;
    }
done:
    __sync_sub_and_fetch(&context->readers_active,1u);
    return copied;
}

/* The echo lines, once, before the audio stage is claimed. Refused memory
   leaves Echo Out off: pads 5 to 8 then mute like pads 1 to 4. */
static int stems_echo_install(void)
{
    size_t bytes = (size_t)STEMS_ECHO_FRAMES * sizeof(Short2) * 16u;   /* two streams per deck */
    const struct rx3_memory_service *memory = framework->memory;
    if (!memory->reserve(&stems_echo, bytes, RX3_STEMS_PLAYER_RESERVE_KIB)) return 0;
    void *block = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (block == MAP_FAILED) {
        memory->abandon(&stems_echo, bytes);
        return 0;
    }
    memset(block, 0, bytes); /* resident before it is counted */
    memory->settle(&stems_echo, bytes);
    for (unsigned int deck = 0; deck < 2u; deck++) {
        memset(&stems_echo[deck], 0, sizeof(stems_echo[deck]));
        stems_echo[deck].delay = STEMS_ECHO_DEFAULT_DELAY;
        for (unsigned int i = 0; i < 4u; i++)
            stems_echo[deck].buffer[i] = (Short2 *)block + (deck * 4u + i) * STEMS_ECHO_FRAMES;
        memset(&stems_echo_alt[deck], 0, sizeof(stems_echo_alt[deck]));
        for (unsigned int i = 0; i < 4u; i++)
            stems_echo_alt[deck].buffer[i] = (Short2 *)block + (8u + deck * 4u + i) * STEMS_ECHO_FRAMES;
    }
    stems_echo_block = block;
    stems_echo_bytes = bytes;
    return 1;
}

/* After the audio stage is released and drained. */
static void stems_echo_remove(void)
{
    if (!stems_echo_block) return;
    for (unsigned int deck = 0; deck < 2u; deck++) {
        memset(&stems_echo[deck], 0, sizeof(stems_echo[deck]));
        memset(&stems_echo_alt[deck], 0, sizeof(stems_echo_alt[deck]));
    }
    munmap(stems_echo_block, stems_echo_bytes);
    framework->memory->release(&stems_echo, stems_echo_bytes);
    stems_echo_block = 0;
    stems_echo_bytes = 0;
}

static int stems_feature_install(void)
{
#ifdef RX3_OVERCUE_PROTOTYPE
    if(getenv("RX3_OVERCUE_ROOT")&&!rx3_overcue_start())return 0;
#endif
    for (unsigned int i = 0; i < 2u; i++) {
        stems_decks[i].selection = 0u;
        stems_decks[i].transition_cursor = TRANSITION_FRAMES;
    }
    stems_bpm_reader = 0;
    stems_iso_enabled = framework->input->send_key != 0;
    {
        const char *colour = getenv("RX3_STEMS_ISO_COLOR");
        stems_iso_color = colour && colour[0] == '1' && !colour[1];
    }
#if defined(__arm__)
    if (!memcmp((const void *)STEMS_PLAY_BPM, stems_play_bpm_guard, 8))
        stems_bpm_reader = (stems_bpm_fn)STEMS_PLAY_BPM;
    else
        log_line("stems echo out: UiGetPlayBpm prologue differs, echo uses 500 ms");
#endif
    if (!stems_echo_block && !stems_echo_install())
        log_line("stems echo out: no memory for the echo lines, pads 5-8 mute only");
    if (!framework->audio->claim_deck_stream(&stems_decks, stems_stream) ||
        !framework->input->register_pad(&stems_decks, 20u, stems_pad) ||
        !framework->input->register_key(&stems_decks, 20u, stems_key) ||
        !framework->input->register_lights(&stems_decks, RX3_LIGHTS_SLIP_LOOP, stems_light) ||
        !framework->loader->claim(&stems_decks)) return 0;
    if (!framework->provide_mix(stems_mix_state) ||
        !framework->provide_waveform(stems_waveform)) return 0;
    __atomic_store_n(&stems_callbacks_enabled, 1u, __ATOMIC_SEQ_CST);
    return framework->panels->register_row(&stems_row);
}

/* The core detaches and drains the shared stream, pad and LED hooks before
   these calls return; the deck drains below cover the waveform readers. */
static void stems_feature_remove(void)
{
    framework->withdraw_waveform(stems_waveform);
    framework->withdraw_mix(stems_mix_state);
    __atomic_store_n(&stems_callbacks_enabled, 0u, __ATOMIC_SEQ_CST);
    framework->panels->unregister_row(&stems_row);
    __atomic_store_n(&stems_panel_shown, 0u, __ATOMIC_SEQ_CST);
    framework->audio->release_deck_stream(&stems_decks);
    framework->input->unregister_owner(&stems_decks);
    stems_echo_remove();
    stems_bpm_reader = 0;
    /* The player is going away with the mod: nothing is injected here. */
    stems_iso_enabled = 0u;
    __atomic_store_n(&stems_channel_stems, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&stems_channel_flat, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&stems_panel_auto, 0u, __ATOMIC_SEQ_CST);
    /* Queued loads are dropped and a running one finishes before this
       returns; pending requests are released below. */
    framework->loader->release(&stems_decks);
    for (unsigned int deck = 0; deck < 2u; deck++) {
        stems_release_request(stems_pending_loads[deck]);
        stems_pending_loads[deck] = 0;
        __atomic_store_n(&stems_decks[deck].reader, 0, __ATOMIC_SEQ_CST);
        stems_drain(&stems_decks[deck]);
        for (unsigned int i = 0; i < 3u; i++) {
            if (stems_decks[deck].pending_fds[i] >= 0) close(stems_decks[deck].pending_fds[i]);
            stems_decks[deck].pending_fds[i] = -1;
            release_payload(&stems_decks[deck].payloads[i]);
        }
        stems_decks[deck].payload_count = 0u;
    }
#ifdef RX3_OVERCUE_PROTOTYPE
    rx3_overcue_stop();
#endif
}

#endif /* RX3_STEMS_FEATURE_H */

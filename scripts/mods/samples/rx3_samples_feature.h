/* SPDX-License-Identifier: MPL-2.0
 * Sample bank behaviour, private to rx3_samples_module.c: the banks (one per
 * deck), their voices, the pad policy on the RELEASE FX page, and its LEDs.
 */

#ifndef RX3_SAMPLES_FEATURE_H
#define RX3_SAMPLES_FEATURE_H

static int samples_enabled;

#include "rx3_samples_config.h"
#include "rx3_samples_audio.h"
#include "rx3_samples_roll.h"
#include "rx3_samples_fxbands.h"
#include "rx3_samples_panel.h"
#include "rx3_samples_page.h"

/* Every setting the pads read, not only the colours: a bank's modes, trims
   and shift.silence used to stop at the parser, so every pad played once at
   unity whatever settings.ini said. The volume of deck 1's bank is the
   starting level of the one volume control. */
static void samples_publish_config(unsigned int deck, const struct samples_config *wanted)
{
    struct samples_config *config = &samples_config[deck];
    for (unsigned int pad = 0; pad < SAMPLES_PAD_COUNT; pad++) {
        __atomic_store_n(&config->colour[pad], wanted->colour[pad], __ATOMIC_SEQ_CST);
        __atomic_store_n(&config->mode[pad], wanted->mode[pad], __ATOMIC_SEQ_CST);
        __atomic_store_n(&config->gain[pad], wanted->gain[pad], __ATOMIC_SEQ_CST);
    }
    /* Written by the loader only; the sampler page reads it to title the pads
       (rx3_samples_page.h). */
    memcpy(config->label, wanted->label, sizeof(config->label));
    __atomic_store_n(&config->shift_silence, wanted->shift_silence, __ATOMIC_SEQ_CST);
    __atomic_store_n(&config->volume, wanted->volume, __ATOMIC_SEQ_CST);
    if (deck == 0u && !__atomic_load_n(&samples_volume_touched, __ATOMIC_SEQ_CST))
        __atomic_store_n(&samples_volume, wanted->volume, __ATOMIC_SEQ_CST);
}

static void samples_read_config(const char *path, struct samples_config *into)
{
    samples_config_defaults(into);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        log_line("sample: no settings.ini, compiled defaults active");
        return;
    }
    off_t length = lseek(fd, 0, SEEK_END);
    int valid = length > 0 && (unsigned long)length <= SAMPLES_CONFIG_MAX_BYTES &&
        lseek(fd, 0, SEEK_SET) == 0;
    char buffer[SAMPLES_CONFIG_MAX_BYTES];
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
    if (!valid || !samples_parse_config(buffer, total, into)) {
        log_line("sample: settings.ini rejected, compiled defaults active");
        return;
    }
    log_line("sample: settings.ini version 1 active");
}

/* The banks load as one job on the core's shared loader. */
static int samples_loader_stopping(void)
{
    return framework->loader->stopping(&samples_enabled);
}

/* A shared slot (deck 2 on deck 1's bank) has no block: it only forgets. */
static void samples_release_slot(unsigned int deck, unsigned int pad)
{
    struct sample_bank_slot *slot = &samples_bank[deck][pad];
    struct sample_slot *voice = &samples_slots[deck][pad];
    __atomic_store_n(&slot->frames, 0, __ATOMIC_SEQ_CST);
    voice->position = SAMPLE_SLOT_IDLE;
    voice->frames = 0;
    voice->length = 0u;
    voice->mode = SAMPLE_MODE_ONCE;
    voice->gain = SAMPLES_GAIN_UNITY;
    if (slot->block) {
        munmap(slot->block, slot->block_size);
        framework->memory->release(&samples_enabled, slot->block_size);
    }
    memset(slot, 0, sizeof(*slot));
}

/* One deck's bank: its settings and the numbered WAV files of `directory`. */
static void samples_load_deck(unsigned int deck, const char *directory, const char *config)
{
    struct samples_config wanted;
    samples_read_config(config, &wanted);
    samples_publish_config(deck, &wanted);
    size_t prefix = directory ? str_length(directory) : 0u;
    char path[1024];
    unsigned int total = 0u, loaded = 0u;
    if (!prefix || prefix + 7u > sizeof(path)) {
        log_line(deck ? "sample: no folder for the deck 2 bank" : "sample: no folder for the bank");
        return;
    }
    memcpy(path, directory, prefix);
    memcpy(path + prefix, "/1.wav", 7u);
    for (unsigned int pad = 0; pad < SAMPLES_PAD_COUNT; pad++) {
        if (samples_loader_stopping()) break;
        samples_release_slot(deck, pad);
        path[prefix + 1u] = (char)('1' + pad);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        off_t length = lseek(fd, 0, SEEK_END);
        if (length < 12 || (unsigned long)length > SAMPLES_BANK_MAX_BYTES) {
            close(fd);
            continue;
        }
        void *wave = mmap(0, (size_t)length, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (wave == MAP_FAILED) continue;
        unsigned int bytes;
        const uint8_t *audio = samples_wave_data(wave, (size_t)length, &bytes);
        if (!audio || bytes > SAMPLES_BANK_MAX_BYTES - total) {
            munmap(wave, (size_t)length);
            continue;
        }
        size_t allocation = (bytes + 4095u) & ~4095u;
        /* Recorded in the shared ledger; each bank's own 16 MiB ceiling is
           the limit, as it always was. */
        const struct rx3_memory_service *memory = framework->memory;
        void *block = memory->reserve(&samples_enabled, allocation, 0u)
            ? mmap(0, allocation, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)
            : MAP_FAILED;
        if (block != MAP_FAILED) memcpy(block, audio, bytes);
        munmap(wave, (size_t)length);
        if (block == MAP_FAILED) {
            memory->abandon(&samples_enabled, allocation);
            continue;
        }
        if (mprotect(block, allocation, PROT_READ)) {
            munmap(block, allocation);
            memory->abandon(&samples_enabled, allocation);
            continue;
        }
        memory->settle(&samples_enabled, allocation);
        samples_bank[deck][pad].length = bytes / SAMPLES_FRAME_BYTES;
        samples_bank[deck][pad].block = block;
        samples_bank[deck][pad].block_size = allocation;
        __atomic_store_n(&samples_bank[deck][pad].frames, block, __ATOMIC_SEQ_CST);
        total += bytes;
        loaded++;
        log_number(deck ? "sample loaded on deck 2, pad = " : "sample loaded on deck 1, pad = ",
                   pad + 1u);
    }
    log_number(deck ? "sample: sounds in the deck 2 bank =" : "sample: sounds in the deck 1 bank =", loaded);
}

/* One bank on both decks: deck 2 plays deck 1's audio with its own voices. */
static void samples_share_deck(void)
{
    samples_publish_config(1u, &samples_config[0]);
    for (unsigned int pad = 0; pad < SAMPLES_PAD_COUNT; pad++) {
        samples_release_slot(1u, pad);
        const int16_t *frames = __atomic_load_n(&samples_bank[0][pad].frames, __ATOMIC_SEQ_CST);
        if (!frames) continue;
        samples_bank[1][pad].length = samples_bank[0][pad].length;
        __atomic_store_n(&samples_bank[1][pad].frames, frames, __ATOMIC_SEQ_CST);
    }
}

static void samples_load_bank(void *unused)
{
    (void)unused;
    if (roll_configured()) roll_load();
    const char *config = getenv("RX3_SAMPLES_CONFIG");
    /* Deck 2's slots may point into deck 1's blocks: forget them first. */
    for (unsigned int pad = 0; pad < SAMPLES_PAD_COUNT; pad++) samples_release_slot(1u, pad);
    samples_load_deck(0u, getenv("RX3_SAMPLES_DIR"), config ? config : "");
    const char *second = getenv("RX3_SAMPLES_DIR2");
    size_t prefix = second ? str_length(second) : 0u;
    char path[1024];
    if (prefix && prefix + 14u <= sizeof(path)) {
        memcpy(path, second, prefix);
        memcpy(path + prefix, "/settings.ini", 14u);
        samples_load_deck(1u, second, path);
    } else if (!samples_loader_stopping()) {
        samples_share_deck();
    }
}

/* The master bus already contains the player; the core calls this before its
   talkover attenuator runs. */
static void samples_master(struct rx3_stereo *output, unsigned int frames)
{
    if (!__atomic_load_n(&samples_callbacks_enabled, __ATOMIC_SEQ_CST)) return;
    unsigned int volume = __atomic_load_n(&samples_volume, __ATOMIC_SEQ_CST);
    for (unsigned int deck = 0; deck < SAMPLES_DECKS; deck++)
        samples_mix(samples_slots[deck], output, (int)frames, volume);
    roll_mix(output, frames);
    /* BRAKE, ECHO, SPIN (RMX-1000 Roll on): last, so they take the player and the samples together. */
    fxrel_master(output, frames);
}

/* Explicit silence for one deck, independent of panels and pad modes. */
static void samples_stop(unsigned int deck)
{
    for (unsigned int pad = 0; pad < SAMPLES_PAD_COUNT; pad++)
        samples_voice_command(&samples_slots[deck][pad], SAMPLE_SLOT_IDLE);
}

static unsigned int samples_active_voices(void)
{
    unsigned int count = 0u;
    for (unsigned int deck = 0; deck < SAMPLES_DECKS; deck++)
        for (unsigned int pad = 0; pad < SAMPLES_PAD_COUNT; pad++)
            if (samples_voice_position(&samples_slots[deck][pad]) != SAMPLE_SLOT_IDLE)
                count++;
    return count;
}

/* A pad hit on the sampler page. SHIFT held with shift.silence stops every
   sound of that deck instead of starting one. */
static void samples_hit(unsigned int deck, unsigned int pad)
{
    struct samples_config *config = &samples_config[deck];
    struct sample_slot *voice = &samples_slots[deck][pad];
    if (__atomic_load_n(&config->shift_silence, __ATOMIC_SEQ_CST) &&
        (framework->input->shift_held(1u) || framework->input->shift_held(2u))) {
        samples_stop(deck);
        return;
    }
    unsigned int mode = __atomic_load_n(&config->mode[pad], __ATOMIC_SEQ_CST);
    unsigned int position = samples_voice_position(voice);
    const int16_t *data = __atomic_load_n(&samples_bank[deck][pad].frames, __ATOMIC_SEQ_CST);
    if ((mode == SAMPLE_MODE_LOOP || mode == SAMPLE_MODE_LATCH) &&
        position != SAMPLE_SLOT_IDLE) {
        /* Both modes that hold are stopped by the pad that started them.
           The mixer tells them apart, not this: only a loop wraps, and a
           latch runs out on its own. */
        samples_voice_command(voice, SAMPLE_SLOT_IDLE);
    } else if (data && samples_bank[deck][pad].length &&
               (position != SAMPLE_SLOT_IDLE ||
                samples_active_voices() < SAMPLES_MAX_VOICES)) {
        voice->frames = data;
        voice->length = samples_bank[deck][pad].length;
        /* The mode lands before the cursor does, so the mixer never sees a
           sounding voice carrying the previous mode. */
        __atomic_store_n(&voice->mode, mode, __ATOMIC_SEQ_CST);
        __atomic_store_n(&voice->gain, __atomic_load_n(&config->gain[pad], __ATOMIC_SEQ_CST),
                         __ATOMIC_SEQ_CST);
        samples_voice_command(voice, 0u);
    }
}

/* RELEASE FX (SLIP LOOP, SHIFT page) is the sampler: the eight pads of a
   deck play that deck's bank. Every event of that page is taken, press and
   release, empty pads too, so the player never reaches simulator_*On or
   simulatorExit and none of its effects (Vinyl Brake, Rewind...) starts.
   A captured press also takes its release after the page or the mode has
   changed. Every other page goes on to the next handler and the player. */
static int samples_pad(const struct rx3_pad_event *event)
{
    unsigned int code = event->code, operation = event->operation, deck = event->deck;
    if (!__atomic_load_n(&samples_callbacks_enabled, __ATOMIC_SEQ_CST) ||
        code < 0x4117u || code > 0x411eu || deck >= SAMPLES_DECKS)
        return 0;
    unsigned int pad = code - 0x4117u, bit = 1u << pad;
    int release_fx = event->pad_mode == RX3_PAD_SLIP_LOOP && event->pad_page == 1u;
    if (!operation && release_fx) {
        samples_captured[deck] |= bit;
        samples_hit(deck, pad);
        samples_page_changed();
        return 1;
    }
    if ((operation & 0x0eu) == 2u && (samples_captured[deck] & bit)) {
        samples_captured[deck] &= ~bit;
        struct sample_slot *voice = &samples_slots[deck][pad];
        if (__atomic_load_n(&voice->mode, __ATOMIC_SEQ_CST) == SAMPLE_MODE_HOLD) {
            samples_voice_command(voice, SAMPLE_SLOT_IDLE);
            samples_page_changed();
        }
        return 1;
    }
    /* Anything else on the page (a repeat, a release whose press came before
       the sampler was up) is taken as well: no simulator call may run here. */
    return release_fx;
}

/* RELEASE FX lights, one deck at a time: each pad in its bank colour, dimmed
   while idle and full while it sounds. A pad with no sound is dark rather
   than native, because the native light would be an effect that is gone. */
static void samples_light(unsigned int deck, unsigned int control, struct rx3_light *light)
{
    if (!__atomic_load_n(&samples_callbacks_enabled, __ATOMIC_SEQ_CST) ||
        deck >= SAMPLES_DECKS || control >= SAMPLES_PAD_COUNT) return;
    if (!__atomic_load_n(&samples_bank[deck][control].frames, __ATOMIC_SEQ_CST)) {
        light->state = RX3_LIGHT_ON;
        light->rgb = 0u;
        return;
    }
    light->rgb = __atomic_load_n(&samples_config[deck].colour[control], __ATOMIC_SEQ_CST);
    light->state = samples_voice_position(&samples_slots[deck][control]) == SAMPLE_SLOT_IDLE
        ? RX3_LIGHT_DIM : RX3_LIGHT_ON;
}

/* The core detaches, drains and releases every shared hook before these
   calls return, so no callback can reach the payloads released after them. */
static void samples_feature_remove(void)
{
    __atomic_store_n(&samples_callbacks_enabled, 0u, __ATOMIC_SEQ_CST);
    if (framework->panels->unregister_page) framework->panels->unregister_page(&samples_page);
    if (!roll_superseded()) framework->panels->unregister_row(&samples_row);
    if (samples_volume_registered) framework->panels->unregister_row(samples_volume_registered);
    samples_volume_registered = 0;
    framework->audio->release_master(&samples_enabled);
    fxrel_stop();
    framework->input->unregister_owner(&samples_enabled);
    framework->loader->release(&samples_enabled);
    roll_remove();
    fxbands_remove();
    /* Deck 2 first: its slots may point into deck 1's blocks. */
    for (unsigned int deck = SAMPLES_DECKS; deck-- > 0u;) {
        samples_captured[deck] = 0u;
        for (unsigned int pad = 0; pad < SAMPLES_PAD_COUNT; pad++)
            samples_release_slot(deck, pad);
    }
}

static int samples_feature_install(void)
{
    const struct rx3_input_service *input = framework->input;
    if (!samples_enabled) return 0;
    struct samples_config defaults;
    samples_config_defaults(&defaults);
    for (unsigned int deck = 0; deck < SAMPLES_DECKS; deck++)
        samples_publish_config(deck, &defaults);
    /* Closes the SAMPLES volume panel when a pad-mode key is pressed. The
       pads no longer depend on it: without it the panel only stays up. */
    if (!input->claim_mode_keys(&samples_enabled))
        log_line("sample panel: physical pad-mode dispatcher guard rejected");
    /* The 1 MiB of the release effects is cleared here, before the audio thread can reach it. */
    fxrel_prepare();
    if (!framework->audio->claim_master(&samples_enabled, samples_master)) return 0;
    /* Ahead of Stems, which leaves the RELEASE FX page alone anyway. */
    if (!input->register_pad(&samples_enabled, 10u, samples_pad)) return 0;
    if (!input->register_lights(&samples_enabled, RX3_LIGHTS_RELEASE_FX, samples_light))
        log_line("sample LEDs unavailable: RELEASE FX refresh guard rejected");
    for (unsigned int deck = 0; deck < SAMPLES_DECKS; deck++) {
        samples_captured[deck] = 0u;
        for (unsigned int pad = 0; pad < SAMPLES_PAD_COUNT; pad++) {
            samples_slots[deck][pad].position = SAMPLE_SLOT_IDLE;
            samples_slots[deck][pad].gain = SAMPLES_GAIN_UNITY;
        }
    }
    if (!roll_install()) return 0;
    fxbands_install();
    const struct rx3_load_job bank = {samples_load_bank, 0, 0};
    if (!framework->loader->claim(&samples_enabled) ||
        !framework->loader->submit(&samples_enabled, &bank)) return 0;
    __atomic_store_n(&samples_callbacks_enabled, 1u, __ATOMIC_SEQ_CST);
    /* Panel 3 (the STATUS tab with the Snare Roll line) is not registered when the RMX Roll supersedes it. */
    if (!roll_superseded() && !framework->panels->register_row(&samples_row)) return 0;
    if (framework->panels->register_row(samples_panel6_row()))
        samples_volume_registered = samples_panel6_row();
    else
        log_line("sample volume: BEAT FX tab stays native");
    if (!framework->panels->register_page || !framework->panels->register_page(&samples_page))
        log_line("sample titles unavailable: RELEASE FX keeps the native pictures");
    log_line("sample playback installed on RELEASE FX; hardware validation pending");
    return 1;
}

#endif /* RX3_SAMPLES_FEATURE_H */

/* SPDX-License-Identifier: MPL-2.0 */
/* The four RMX sounds (KICK SNARE CLAP H.HAT) as one-shots, and the layers of
 * OVERDUB: a loop of one bar into which the hits are written, and played again
 * on every pass, until DELETE takes a sound's layer (or all of them) away.
 *
 * Pure code: no allocation, no I/O, no libc, no libm, no integer division by a
 * variable that is not unsigned. The caller owns the state and the sounds. Not
 * on the audio thread: the commands (rmxy_post) may come from any thread, the
 * processing (rmxy_process) runs on the audio thread only and takes them off a
 * small ring of atomics, so nothing here waits. Original work.
 *
 * Layers. The first hit while OVERDUB is on starts the loop: it is written at
 * frame 0 of a loop of `bar` frames (one bar at the master tempo, fixed when
 * the loop starts) and the loop runs from then on, whether OVERDUB stays on or
 * not. Each further hit while OVERDUB is on is written at the place of the loop
 * it fell on. A muted sound's layer keeps running silently. DELETE of a sound
 * removes its hits; DELETE ALL removes every hit and stops the loop.
 */
#ifndef RX3_RMX_LAYERS_H
#define RX3_RMX_LAYERS_H

#define RMXY_SOUNDS 4u
#define RMXY_VOICES 8u
#define RMXY_HITS 256u
#define RMXY_RING 32u                 /* commands in flight */
#define RMXY_MAX_BAR (44100u * 12u)
/* The master bus is float with full scale 1.0 (the sampler scales PCM16 by 1/32768), so a
   PCM16 hit must be scaled down to that before it is added. The level (y->level, 0..1) is the
   level knob over its whole travel (0 = muted); no other ceiling. The ends of every hit are faded. */
#define RMXY_SCALE (1.0f / 32768.0f)
#define RMXY_FADE_IN 32u
#define RMXY_FADE_OUT 128u

/* RMXY_C_ROLL_ON: arg = sound << 24 | interval in frames (the selected sound is struck again every
   interval, from the press, until RMXY_C_ROLL_OFF; the strikes in flight ring out with their own fade). */
enum rmxy_cmd { RMXY_C_NONE = 0, RMXY_C_TRIGGER, RMXY_C_OVERDUB, RMXY_C_DELETE, RMXY_C_DELETE_ALL,
                RMXY_C_ROLL_ON, RMXY_C_ROLL_OFF, RMXY_C_ROLL_SOUND };

struct rmxy_sound { const int16_t *frames; unsigned int length; };   /* interleaved stereo PCM16, 44.1 kHz */

#define RMXY_CHOKE 165u   /* frames (3.7 ms) a voice takes to fade out when the same sound strikes again */
struct rmxy_voice { unsigned char sound, active, choked; unsigned int pos, delay, choke, release; float frac; };   /* frac: the fraction of a frame past pos when the sound is pitched */

struct rmxy {
    /* audio thread only */
    struct rmxy_voice voice[RMXY_VOICES];
    unsigned int hit_at[RMXY_HITS];
    unsigned char hit_sound[RMXY_HITS];
    float pitch;                                                   /* playback speed of the sounds (0.5 .. 2, 1 = as recorded): the finger height on the X-PAD */
    float rate, phase_frac;                                        /* loop speed (0.5 .. 4) and the remainder of the phase */
    float level, level_target;                                     /* 0..1 (the AUX knob; 0 = muted): level glides to the target over ~25 ms */
    unsigned int roll_on, roll_sound, roll_interval, roll_next;   /* the sample roll */
    unsigned int hits, bar, phase, recording, dropped, fresh;   /* fresh: 1 + index of the hit written in this block */
    /* the command ring: producer = any thread (one at a time under the caller's lock), consumer = audio */
    volatile unsigned int head, tail;
    unsigned char ring_cmd[RMXY_RING];
    unsigned int ring_arg[RMXY_RING];
};

/* What the screen and the lights may read (published by the audio thread). */
struct rmxy_view { unsigned int layers; unsigned int recording; unsigned int running; };

static inline void rmxy_reset(struct rmxy *y)
{
    for (unsigned int i = 0; i < RMXY_VOICES; i++) { y->voice[i].active = 0; y->voice[i].sound = 0; y->voice[i].pos = 0; y->voice[i].delay = 0; y->voice[i].frac = 0.0f; y->voice[i].choked = 0; y->voice[i].release = 0; y->voice[i].choke = 0; }
    y->hits = 0; y->bar = 0; y->phase = 0; y->recording = 0; y->dropped = 0; y->fresh = 0;
    y->level = 1.0f; y->level_target = 1.0f; y->pitch = 1.0f; y->rate = 1.0f; y->phase_frac = 0.0f;
    y->roll_on = 0; y->roll_sound = 0; y->roll_interval = 0; y->roll_next = 0;
    __atomic_store_n(&y->head, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&y->tail, 0u, __ATOMIC_SEQ_CST);
}

/* Queue a command. Returns 0 when the ring is full (the command is lost). */
static inline int rmxy_post(struct rmxy *y, unsigned int cmd, unsigned int arg)
{
    unsigned int head = __atomic_load_n(&y->head, __ATOMIC_SEQ_CST);
    unsigned int tail = __atomic_load_n(&y->tail, __ATOMIC_SEQ_CST);
    if (head - tail >= RMXY_RING) return 0;
    y->ring_cmd[head % RMXY_RING] = (unsigned char)cmd;
    y->ring_arg[head % RMXY_RING] = arg;
    __atomic_store_n(&y->head, head + 1u, __ATOMIC_SEQ_CST);
    return 1;
}

static inline void rmxy_start_voice(struct rmxy *y, unsigned int sound, unsigned int delay)
{
    unsigned int pick = 0, oldest = 0;
    /* Like a drum machine: a sound that strikes again chokes its own earlier voice with a short fade (a roll of one sound
       never piles up its long tail on itself, which made it distort), instead of being added on top of it. */
    for (unsigned int i = 0; i < RMXY_VOICES; i++) {
        struct rmxy_voice *old = &y->voice[i];
        if (old->active && !old->choked && old->sound == sound && old->delay <= delay) {
            old->choked = 1u; old->release = RMXY_CHOKE; old->choke = delay;
        }
    }
    for (unsigned int i = 0; i < RMXY_VOICES; i++) {
        if (!y->voice[i].active) { pick = i; oldest = 0xffffffffu; break; }
        if (y->voice[i].pos >= oldest || i == 0) { oldest = y->voice[i].pos; pick = i; }
    }
    y->voice[pick].sound = (unsigned char)sound;
    y->voice[pick].pos = 0;
    y->voice[pick].frac = 0.0f;
    y->voice[pick].choked = 0u; y->voice[pick].release = 0u; y->voice[pick].choke = 0u;
    y->voice[pick].delay = delay;
    y->voice[pick].active = 1;
}

static inline void rmxy_remove(struct rmxy *y, unsigned int sound)
{
    unsigned int to = 0;
    for (unsigned int i = 0; i < y->hits; i++) {
        if (y->hit_sound[i] == sound) continue;
        y->hit_at[to] = y->hit_at[i];
        y->hit_sound[to] = y->hit_sound[i];
        to++;
    }
    y->hits = to;
    if (!y->hits) { y->bar = 0; y->phase = 0; }
}

/* OVERDUB is always on the grid: a hit is written at the nearest sixteenth of the bar (one bar = 16 steps), so
   whatever the hand did, the layers stay in time. The live hit still sounds when it was struck. */
#define RMXY_GRID 16u
static inline unsigned int rmxy_snap(unsigned int at, unsigned int bar)
{
    unsigned int step = bar / RMXY_GRID;
    if (step < 2u) return at;
    at = (at + step / 2u) / step * step;
    return at >= bar ? 0u : at;
}

/* The bar a loop was recorded at no longer matches the master's (another song, another tempo, or the tempo
   reading settled after the first hits): the loop is stretched to the new bar so its hits stay on the new
   beat grid. A change under 6 % is the tempo reading's own wobble and is left alone. */
static inline void rmxy_retime(struct rmxy *y, unsigned int bar_frames)
{
    if (!y->bar || !y->hits || bar_frames < 1024u) return;
    unsigned int target = bar_frames > RMXY_MAX_BAR ? RMXY_MAX_BAR : bar_frames;
    unsigned int diff = target > y->bar ? target - y->bar : y->bar - target;
    if ((float)diff * 100.0f < (float)y->bar * 6.0f) return;
    float k = (float)target / (float)y->bar;
    for (unsigned int i = 0; i < y->hits; i++) {
        unsigned int at = (unsigned int)((float)y->hit_at[i] * k);
        y->hit_at[i] = at >= target ? 0u : at;
    }
    y->phase = (unsigned int)((float)y->phase * k);
    if (y->phase >= target) y->phase = 0u;
    y->bar = target;
}

static inline unsigned int rmxy_layers(const struct rmxy *y)
{
    unsigned int mask = 0;
    for (unsigned int i = 0; i < y->hits; i++) mask |= 1u << y->hit_sound[i];
    return mask;
}

/* `bar_frames`: the loop length a new loop would take (frames of one bar at the
   master tempo), `muted` a bit per sound. `io` is interleaved stereo FLOATS in the
   player's own scale (PCM16 scale), the sounds are added to it. */
static inline void rmxy_process(struct rmxy *y, const struct rmxy_sound sounds[RMXY_SOUNDS], unsigned int muted,
                                unsigned int bar_frames, float *io, unsigned int frames)
{
    for (;;) {
        unsigned int tail = __atomic_load_n(&y->tail, __ATOMIC_SEQ_CST);
        if (tail == __atomic_load_n(&y->head, __ATOMIC_SEQ_CST)) break;
        unsigned int cmd = y->ring_cmd[tail % RMXY_RING], arg = y->ring_arg[tail % RMXY_RING];
        __atomic_store_n(&y->tail, tail + 1u, __ATOMIC_SEQ_CST);
        if (cmd == RMXY_C_TRIGGER && arg < RMXY_SOUNDS) {
            if (!((muted >> arg) & 1u)) rmxy_start_voice(y, arg, 0u);
            if (y->recording) {
                if (!y->bar && bar_frames >= 1024u) { y->bar = bar_frames > RMXY_MAX_BAR ? RMXY_MAX_BAR : bar_frames; y->phase = 0; }
                if (y->bar && y->hits < RMXY_HITS) { y->hit_at[y->hits] = rmxy_snap(y->phase, y->bar); y->hit_sound[y->hits] = (unsigned char)arg; y->hits++; y->fresh = y->hits; }
                else y->dropped++;
            }
        } else if (cmd == RMXY_C_ROLL_ON) {
            unsigned int interval = arg & 0xffffffu, sound = arg >> 24;
            if (sound < RMXY_SOUNDS && interval >= 64u) {
                y->roll_on = 1u; y->roll_sound = sound; y->roll_interval = interval; y->roll_next = 0u;   /* the first strike is now */
            }
        } else if (cmd == RMXY_C_ROLL_OFF) {
            y->roll_on = 0u;
        } else if (cmd == RMXY_C_ROLL_SOUND) {
            /* The running roll changes to another sound at once: bit 8 of `arg` = the hit is already made (OVERDUB). The next
               strike falls one interval after this touch, so the roll goes on from the moment the player hit. */
            unsigned int sound = arg & 0xffu;
            if (y->roll_on && sound < RMXY_SOUNDS && y->roll_interval >= 64u) {
                y->roll_sound = sound;
                if (!(arg & 0x100u)) rmxy_start_voice(y, sound, 0u);
                y->roll_next = y->roll_interval;
            }
        } else if (cmd == RMXY_C_OVERDUB) {
            y->recording = arg ? 1u : 0u;
            /* Recording starts at the touch: the loop's clock starts now, not at the first hit. */
            if (arg && !y->bar && bar_frames >= 1024u) { y->bar = bar_frames > RMXY_MAX_BAR ? RMXY_MAX_BAR : bar_frames; y->phase = 0; y->phase_frac = 0.0f; }
        } else if (cmd == RMXY_C_DELETE && arg < RMXY_SOUNDS) {
            rmxy_remove(y, arg);
        } else if (cmd == RMXY_C_DELETE_ALL) {
            y->hits = 0; y->bar = 0; y->phase = 0; y->phase_frac = 0.0f;
        }
    }
    rmxy_retime(y, bar_frames);
    /* The loop moves by `rate` loop frames per frame (0.5 .. 4, 1 = the tempo it was recorded at): the
       speed of what has been made, on the BEAT FX level knob. phase_frac keeps the remainder. */
    float rate = y->rate < 0.5f ? 0.5f : y->rate > 4.0f ? 4.0f : y->rate;
    float advf = (float)frames * rate + y->phase_frac;
    unsigned int adv = (unsigned int)advf;
    /* The layers: the hits that fall inside this block start a voice at their offset. */
    if (y->bar && y->hits) {
        for (unsigned int i = 0; i < y->hits; i++) {
            if ((muted >> y->hit_sound[i]) & 1u || i + 1u == y->fresh) continue;   /* the hit just played live */
            unsigned int d = y->hit_at[i] >= y->phase ? y->hit_at[i] - y->phase : y->hit_at[i] + y->bar - y->phase;
            if (d < adv) {
                unsigned int offset = rate == 1.0f ? d : (unsigned int)((float)d / rate);
                if (offset < frames) rmxy_start_voice(y, y->hit_sound[i], offset);
            }
        }
    }
    y->fresh = 0;
    /* The roll: the selected sound again at every interval, at its own frame; while OVERDUB records, each
       strike is written into the loop as well (after the layers above, so it does not sound twice now).
       Not muted by MUTE (the roll is the player's own hand). */
    if (y->roll_on) {
        while (y->roll_next < frames) {
            rmxy_start_voice(y, y->roll_sound, y->roll_next);
            if (y->recording) {
                if (!y->bar && bar_frames >= 1024u) { y->bar = bar_frames > RMXY_MAX_BAR ? RMXY_MAX_BAR : bar_frames; y->phase = 0; }
                if (y->bar && y->hits < RMXY_HITS) {
                    unsigned int at = y->phase + (unsigned int)((float)y->roll_next * rate);
                    while (at >= y->bar) at -= y->bar;
                    y->hit_at[y->hits] = rmxy_snap(at, y->bar); y->hit_sound[y->hits] = (unsigned char)y->roll_sound; y->hits++;
                } else y->dropped++;
            }
            y->roll_next += y->roll_interval;
        }
        y->roll_next -= frames;
    }
    if (y->bar) { y->phase += adv; while (y->phase >= y->bar) y->phase -= y->bar; }
    y->phase_frac = advf - (float)adv;
    /* The level glides from where it was toward the target by at most one 25 ms step per block, and is
       interpolated frame by frame inside the block, so a mute or an unmute never clicks. */
    float lvl0 = y->level, lvl1 = y->level_target;
    {
        float step = (float)frames * (1.0f / 1100.0f);
        if (lvl1 > lvl0 + step) lvl1 = lvl0 + step;
        else if (lvl1 < lvl0 - step) lvl1 = lvl0 - step;
    }
    const float lvl_slope = frames ? (lvl1 - lvl0) / (float)frames : 0.0f;
    const float pitch = y->pitch < 0.5f ? 0.5f : y->pitch > 2.0f ? 2.0f : y->pitch;
    for (unsigned int v = 0; v < RMXY_VOICES; v++) {
        struct rmxy_voice *voice = &y->voice[v];
        if (!voice->active) continue;
        const struct rmxy_sound *sound = &sounds[voice->sound];
        if (!sound->frames) { voice->active = 0; continue; }
        unsigned int i = voice->delay < frames ? voice->delay : frames;
        voice->delay -= i;
        if (pitch == 1.0f) {
            for (; i < frames && voice->pos < sound->length; i++, voice->pos++) {
                float g = RMXY_SCALE * (lvl0 + lvl_slope * (float)i);
                if (voice->pos < RMXY_FADE_IN) g *= (float)(voice->pos + 1u) * (1.0f / (float)RMXY_FADE_IN);
                unsigned int left = sound->length - voice->pos;
                if (left < RMXY_FADE_OUT) g *= (float)left * (1.0f / (float)RMXY_FADE_OUT);
                if (voice->choked && i >= voice->choke) { g *= (float)voice->release * (1.0f / (float)RMXY_CHOKE); if (--voice->release == 0u) { voice->active = 0u; break; } }
                io[i * 2u] += g * (float)sound->frames[voice->pos * 2u];
                io[i * 2u + 1u] += g * (float)sound->frames[voice->pos * 2u + 1u];
            }
        } else {
            /* Pitched: the sound is read `pitch` frames per output frame, linearly interpolated (up = higher and shorter). */
            for (; i < frames && voice->pos < sound->length; i++) {
                float g = RMXY_SCALE * (lvl0 + lvl_slope * (float)i);
                if (voice->pos < RMXY_FADE_IN) g *= (float)(voice->pos + 1u) * (1.0f / (float)RMXY_FADE_IN);
                unsigned int left = sound->length - voice->pos;
                if (left < RMXY_FADE_OUT) g *= (float)left * (1.0f / (float)RMXY_FADE_OUT);
                if (voice->choked && i >= voice->choke) { g *= (float)voice->release * (1.0f / (float)RMXY_CHOKE); if (--voice->release == 0u) { voice->active = 0u; break; } }
                unsigned int next = voice->pos + 1u < sound->length ? voice->pos + 1u : voice->pos;
                float a0 = (float)sound->frames[voice->pos * 2u], a1 = (float)sound->frames[next * 2u];
                float b0 = (float)sound->frames[voice->pos * 2u + 1u], b1 = (float)sound->frames[next * 2u + 1u];
                io[i * 2u] += g * (a0 + (a1 - a0) * voice->frac);
                io[i * 2u + 1u] += g * (b0 + (b1 - b0) * voice->frac);
                voice->frac += pitch;
                while (voice->frac >= 1.0f) { voice->frac -= 1.0f; voice->pos++; }
            }
        }
        if (voice->pos >= sound->length) voice->active = 0;
        if (voice->choked) voice->choke = voice->choke > frames ? voice->choke - frames : 0u;
    }
    y->level = lvl1;
}

/* ---- the sound files: 44.1 kHz PCM16 stereo WAV ---- */
static inline unsigned int rmxy_le16(const uint8_t *p) { return (unsigned int)p[0] | (unsigned int)p[1] << 8; }
static inline unsigned int rmxy_le32(const uint8_t *p) { return rmxy_le16(p) | rmxy_le16(p + 2) << 16; }

/* The audio bytes of a WAV image, or 0; `*bytes` their count (a whole number of frames). */
static inline const uint8_t *rmxy_wave_data(const uint8_t *wave, unsigned long size, unsigned int *bytes)
{
    *bytes = 0u;
    if (!wave || size < 44u || wave[0] != 'R' || wave[1] != 'I' || wave[2] != 'F' || wave[3] != 'F' ||
        wave[8] != 'W' || wave[9] != 'A' || wave[10] != 'V' || wave[11] != 'E') return 0;
    unsigned long cursor = 12u;
    const uint8_t *format = 0, *data = 0;
    unsigned int length = 0;
    while (size - cursor >= 8u) {
        const uint8_t *chunk = wave + cursor;
        unsigned long count = rmxy_le32(chunk + 4);
        cursor += 8u;
        if (count > size - cursor) {   /* a cut data chunk is read as far as it goes */
            if (chunk[0] != 'd' || chunk[1] != 'a' || chunk[2] != 't' || chunk[3] != 'a') return 0;
            count = size - cursor;
        }
        if (chunk[0] == 'f' && chunk[1] == 'm' && chunk[2] == 't' && chunk[3] == ' ') { if (count < 16u) return 0; format = wave + cursor; }
        else if (chunk[0] == 'd' && chunk[1] == 'a' && chunk[2] == 't' && chunk[3] == 'a') { data = wave + cursor; length = (unsigned int)count; }
        cursor += count + (count & 1u);
        if (cursor > size) break;
    }
    if (!format || !data || rmxy_le16(format) != 1u || rmxy_le16(format + 2) != 2u || rmxy_le32(format + 4) != 44100u ||
        rmxy_le16(format + 14) != 16u || length < 4u) return 0;
    *bytes = length - length % 4u;
    return data;
}

#endif

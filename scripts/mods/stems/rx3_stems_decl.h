/* SPDX-License-Identifier: MPL-2.0
 * Private Stems state and data formats, compiled by rx3_stems_module.c.
 */

#ifndef RX3_STEMS_DECL_H
#define RX3_STEMS_DECL_H

#include "rx3_stems_limits.h"
#ifdef RX3_OVERCUE_PROTOTYPE
#include "overcue/overcue.h"
#endif

typedef struct { int16_t left, right; } Short2;

/* Crossfade length when a role is switched, in valid frames. */
#define TRANSITION_FRAMES 256u


enum stem_mode {
    MODE_NONE = 0,
    MODE_INSTRUMENTAL = 1,
    MODE_VOCAL = 2,
    MODE_BOTH = MODE_INSTRUMENTAL | MODE_VOCAL
};

enum stem_format { FORMAT_F32 = 1, FORMAT_S16 = 2, FORMAT_S16_GAIN = 3 };

struct __attribute__((packed)) stem_header {
    char magic[8];
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t format;
    uint32_t header_size;
    uint64_t frames;
    uint8_t reserved[32];
};

static float stems_pcm_gain(const struct stem_header *h)
{
    if(h->format==FORMAT_S16) {
        for(unsigned int i=0;i<32;i++) if(h->reserved[i]) return 0;
        return 1.0f;
    }
    if(h->format!=FORMAT_S16_GAIN) return 0;
    float gain;
    memcpy(&gain,h->reserved,4);
    if(!(gain>=1.0f && gain<=64.0f)) return 0;
    for(unsigned int i=4;i<32;i++) if(h->reserved[i]) return 0;
    return gain;
}

struct stem_payload {
    const void *data;
    uint32_t format;
    float pcm_gain;
    uint64_t frames;
    void *block;
    size_t block_size;
    const uint8_t *wave;
    unsigned int wave_size;
};

struct stems_deck_context {
#ifdef RX3_OVERCUE_PROTOTYPE
    unsigned int overcue;
#endif
    void *volatile reader;
    volatile unsigned int status;
    volatile unsigned int selection;
    /* Four 7-bit percentages; one atomic snapshot for buttons and audio. */
    volatile uint32_t levels;
    volatile uint32_t generation;
    volatile int armed;
    volatile unsigned int readers_active;
    float gain[4];
    float from[4];
    float target[4];
    unsigned int transition_cursor;
    unsigned int payload_count;
    struct stem_payload payloads[3];
    /* While `streaming`, only the first `loaded` frames of every part are
       resident: a package streams in from the start, and past that point the
       deck plays stock. Not streaming: all of it. */
    volatile unsigned int streaming, loaded;
    int pending_has_stem;
    int pending_fds[3];
    char pending_path[1024];
    char track_path[1024];       /* the loaded track, for MAKE STEMS from the deck */
};

struct stems_load_request {
    struct stems_deck_context *context;
    void *reader;
    uint32_t generation;
    int fds[3];
};

/* Echo Out (SLIP LOOP pads 5 to 8): one delay line per deck and per role
   (bit i = 1 << i: HARMONY, VOCAL, DRUMS, BASS), counted in the deck's
   output frames after TimeStretch, so a changed pitch keeps the echo on the
   beat it was given. Lines hold int16 stereo and are allocated once at
   install; without them the mix behaves exactly as before.
   - At rest a line records what its role is playing (gain applied), so the
     last beat is ready when the pad is pressed.
   - A press mutes the role (the usual 256-frame ramp) and starts the tail:
     the line repeats every `delay` frames (half a beat) with STEMS_ECHO_FEEDBACK through
     a one-pole low-pass, and stops by itself once a whole repeat stays
     under STEMS_ECHO_SILENT. The tail keeps running while the deck pauses.
   - Bringing the role back fades the tail out over 256 frames.
   `delay` is one beat from the deck's BPM, written by the input thread only
   while no tail plays; `tails` holds the role bits whose tail should play:
   the input thread sets and clears them, the audio thread clears a bit when
   its tail has died away. */
#define STEMS_ECHO_FRAMES 88200u          /* 2 s: one beat down to 30 BPM */
#define STEMS_ECHO_DEFAULT_DELAY 11025u   /* 250 ms: half of the native Beat FX default */
#define STEMS_ECHO_FEEDBACK 0.45f
#define STEMS_ECHO_WET 0.7f
#define STEMS_ECHO_DAMPING 0.35f          /* low-pass coefficient in the loop */
#define STEMS_ECHO_SILENT (1.0f / 1024.0f) /* -60 dBFS */
#define STEMS_ECHO_REPEATS 20u            /* a tail never outlives this many */
struct stems_echo_line {
    float low_left, low_right, peak, fade;
    unsigned int state;                   /* 0 rest, 1 tail, 2 fading out */
    unsigned int repeats;
};
struct stems_echo_deck {
    Short2 *buffer[4];
    unsigned int cursor, window, running;
    struct stems_echo_line line[4];
    volatile unsigned int delay;
    volatile unsigned int tails;
};
static struct stems_echo_deck stems_echo[2];
/* With the pitch fader at 0 the RX3 asks a deck for two interleaved streams at different
   positions (log 2026-11-03: every block a jump, half of them backwards, 172 blocks a second
   instead of 86); an echo fed by both crackles. Each stream keeps its own lines (buffers,
   cursor, per-role state) in stems_echo (stream 0) and stems_echo_alt (stream 1); `delay` and
   `tails` live in stems_echo[deck] only, for both. */
static struct stems_echo_deck stems_echo_alt[2];
/* The stream the block being mixed belongs to, 0 or 1: set by stems_stream on the audio thread,
   the only one that reads it. */
static unsigned int stems_stream_index;
static void *stems_echo_block;
static size_t stems_echo_bytes;

/* Called, not hooked (analysis 01-05-06 §1.5, the accessor now-playing
   uses): int UiGetPlayBpm(int deck), read on the input thread when an echo
   pad is pressed. now-playing reads it as BPM in hundredths and 0xffff
   without one; the unit is not confirmed on hardware (DA_FIXARE/20), so a
   value outside 40-300 BPM falls back to STEMS_ECHO_DEFAULT_DELAY. */
#define STEMS_PLAY_BPM ((unsigned long)0x000fd1fc)
static const uint8_t stems_play_bpm_guard[8] = {
    0x08, 0x40, 0x2d, 0xe9, 0x00, 0x10, 0xa0, 0xe1
};
typedef int (*stems_bpm_fn)(unsigned int deck);
/* Set at install once the guard matches; null on the host and on refusal. */
static stems_bpm_fn stems_bpm_reader;

static struct stems_deck_context stems_decks[2] = {
    {.pending_fds = {-1,-1,-1}}, {.pending_fds = {-1,-1,-1}}
};
static volatile unsigned int stems_callbacks_enabled;
static volatile unsigned int captured_pad_mask[2];
/* SHIFT+CUE toggles the STEMS panel. These track whether it is on screen and
   which CUE presses were taken, so their releases never reach the player. */
static volatile unsigned int stems_panel_shown;
static volatile unsigned int stems_cue_captured;
static const char *stems_dir;

#endif /* RX3_STEMS_DECL_H */

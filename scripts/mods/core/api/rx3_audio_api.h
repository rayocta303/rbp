/* SPDX-License-Identifier: MPL-2.0 */
#ifndef RX3_AUDIO_API_H
#define RX3_AUDIO_API_H
#include "rx3_platform.h"
/* Typed audio stages. Each stage has one owner at a time; a second claim
 * fails without replacing the first. Processing runs on the player's audio
 * threads and must not allocate, perform I/O or wait. The core counts callers
 * in flight: release() detaches the native hook, drains admitted callbacks
 * and only then returns, so it must not be called from a callback. Buffers
 * belong to the player and are valid only during the call.
 */
struct rx3_stereo { float left, right; };

/* A deck's playback stream after speed and master tempo (TimeStretch), the
 * signal the deck actually plays. `position` is the reader frame of the first
 * output frame at 44.1 kHz; `reader` identifies the deck's current PcmReader
 * and changes with each track load. */
typedef void (*rx3_deck_stream_fn)(unsigned int deck, const void *reader, int position,
                                   struct rx3_stereo *output, unsigned int frames);
/* The master bus before the microphone talkover attenuator. */
typedef void (*rx3_master_fn)(struct rx3_stereo *output, unsigned int frames);

/* Where a deck's playback stream is: the reader frame of the first output
 * frame of its last block that moved (a block asked for again at the same
 * position does not count), and a counter that grows with each of them. The
 * deck stage feeds the master bus in the same audio cycle, so on the master
 * bus this is the deck's position at the start of the block being mixed. */
struct rx3_playhead { int position; unsigned int frames; unsigned int serial; };

struct rx3_audio_service {
    int (*claim_deck_stream)(const void *owner, rx3_deck_stream_fn);
    void (*release_deck_stream)(const void *owner);
    int (*claim_master)(const void *owner, rx3_master_fn);
    void (*release_master)(const void *owner);
    /* Keep the deck stage hooked for playhead(), whoever owns its client.
       Counted: each use_playhead() that returned 1 takes one drop_playhead(). */
    int (*use_playhead)(void);
    void (*drop_playhead)(void);
    /* Audio threads included: no lock, no waiting. 0 before any block. */
    int (*playhead)(unsigned int deck, struct rx3_playhead *out);
    /* Any thread, no lock: 0 = no track has been loaded on either deck since the mod started (the player
       is idle: modules may use the CPU freely); 1 = a track is loading now; 2 = a track is loaded. Modules
       with background work give the CPU back to the player on 1 and 2. There is no unload hook: a deck
       stays "loaded" once a track was in it. */
    unsigned int (*load_state)(void);
    /* A second, independent client of the master bus. It runs right after the
       claim_master() client in the same callback, so a module that only needs
       the summed output (a roll that repeats it) does not compete with the
       module that owns the first stage. One owner; a second claim fails
       without replacing the first. Same rules as claim_master() for the
       callback; release_master_after() detaches, drains and returns, so it
       must not be called from a callback. The native hook stays installed
       while either client holds it. */
    int (*claim_master_after)(const void *owner, rx3_master_fn);
    void (*release_master_after)(const void *owner);
};
extern const struct rx3_audio_service rx3_audio;
#endif

/* SPDX-License-Identifier: MPL-2.0
 * RELEASE FX of the RMX-1000 on the master: BRAKE, ECHO, SPIN, three held buttons on the FX tab
 * (panel 6) that exist only when the RMX-1000 Roll module is on (RX3_RMX_ROLL=1).
 *
 * A finger on a button holds the effect (the press begins it, the lift ends it); one effect at a
 * time. The touch (input thread) only leaves messages in atomics; the audio thread is the only one
 * that touches the effect's state, at the start of each block of the master:
 *   fxrel_held   type + 1 under the finger, 0 when none (written by the touch, cleared by every way
 *                the finger can go: the lift, a slide off the button, the tab closing)
 *   fxrel_tap    type + 1 of a press the audio thread has not started yet; kept until the unit is
 *                idle, so a press that lands while the previous effect is still fading out (20 ms)
 *                is not lost, and a tap shorter than a block still plays its crossfades
 *   fxrel_delay  the echo delay in frames, read by the press: half a beat of the MASTER deck when
 *                its BPM can be read, else 0.375 s
 *   fxrel_live   type + 1 of the effect that is on, written by the audio thread for the painter
 * The 1 MiB of state is static (zero) and is cleared once, in start(), never on the audio thread.
 * Without the RMX module nothing here runs and the master is not touched.
 */
#ifndef RX3_SAMPLES_FXRELEASE_H
#define RX3_SAMPLES_FXRELEASE_H

#include "rx3_samples_release.h"

#define FXREL_FIRST_WIDGET 4u          /* BRAKE, ECHO, SPIN are widgets 4, 5, 6 of the FX row */
#define FXREL_COUNT 3u

static struct smplr fxrel;
static volatile unsigned int fxrel_ready;
static volatile unsigned int fxrel_held, fxrel_tap, fxrel_delay, fxrel_live;
static roll_bpm_fn fxrel_bpm;

/* RELEASE FX (BRAKE, ECHO, SPIN) are shelved for now: they show only when RX3_RMX_RELEASE=1 (the shipped
   configuration leaves it off, so the FX tab is the stock one). */
static int fxrel_wanted(void)
{
    const char *on = getenv("RX3_RMX_RELEASE");
    return roll_superseded() && on && on[0] == '1';
}

/* Half a beat of the master deck in frames, or the core's default delay. Input thread. */
static unsigned int fxrel_delay_frames(void)
{
    if (fxrel_bpm && framework->browse && framework->browse->reference) {
        struct rx3_harmonic_reference master = framework->browse->reference();
        if (master.deck == 0 || master.deck == 1) {
            int bpm = fxrel_bpm((unsigned int)master.deck);
            if (bpm >= 4000 && bpm <= 30000)
                return (44100u * 60u * 100u + (unsigned int)bpm) / ((unsigned int)bpm * 2u);
        }
    }
    return SMPLR_DELAY_DEFAULT;
}

/* start() only (a loading thread): clears the ring, finds the BPM reader. */
static void fxrel_prepare(void)
{
    __atomic_store_n(&fxrel_ready, 0u, __ATOMIC_SEQ_CST);
    if (!fxrel_wanted()) return;
    smplr_reset(&fxrel);
    fxrel_bpm = 0;
#if defined(__arm__)
    if (!memcmp((const void *)ROLL_PLAY_BPM, roll_play_bpm_guard, 8))
        fxrel_bpm = (roll_bpm_fn)ROLL_PLAY_BPM;
    else
        log_line("release fx: UiGetPlayBpm prologue differs, echo at 0.375 s");
#endif
    __atomic_store_n(&fxrel_held, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&fxrel_tap, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&fxrel_live, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&fxrel_delay, SMPLR_DELAY_DEFAULT, __ATOMIC_SEQ_CST);
    __atomic_store_n(&fxrel_ready, 1u, __ATOMIC_SEQ_CST);
    log_line("release fx: brake, echo and spin on the FX tab");
}

static void fxrel_stop(void)
{
    __atomic_store_n(&fxrel_ready, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&fxrel_held, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&fxrel_tap, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&fxrel_live, 0u, __ATOMIC_SEQ_CST);
}

/* Input thread: the finger on button `type` (0 BRAKE, 1 ECHO, 2 SPIN), still down (committed 0) or lifted. */
static void fxrel_touch(unsigned int type, unsigned int committed)
{
    if (type >= FXREL_COUNT || !__atomic_load_n(&fxrel_ready, __ATOMIC_SEQ_CST)) return;
    if (committed) {
        __atomic_store_n(&fxrel_held, 0u, __ATOMIC_SEQ_CST);
        return;
    }
    if (__atomic_load_n(&fxrel_held, __ATOMIC_SEQ_CST) == type + 1u) return;   /* the same finger moving */
    __atomic_store_n(&fxrel_delay, fxrel_delay_frames(), __ATOMIC_SEQ_CST);
    __atomic_store_n(&fxrel_held, type + 1u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&fxrel_tap, type + 1u, __ATOMIC_SEQ_CST);
}

/* Input or render thread: no finger on any button (the tab closed, the panel changed). */
static void fxrel_let_go(void)
{
    __atomic_store_n(&fxrel_held, 0u, __ATOMIC_SEQ_CST);
}

/* Lit while the finger is on it or the effect is on. */
static int fxrel_lit(unsigned int type)
{
    return __atomic_load_n(&fxrel_held, __ATOMIC_SEQ_CST) == type + 1u ||
           __atomic_load_n(&fxrel_live, __ATOMIC_SEQ_CST) == type + 1u;
}

/* Audio thread: the master bus after the samples. Idle, the block is only copied into the ring. */
static void fxrel_master(struct rx3_stereo *output, unsigned int frames)
{
    if (!__atomic_load_n(&fxrel_ready, __ATOMIC_SEQ_CST) || !output || !frames) return;
    unsigned int began = 0u;
    if (!smplr_active(&fxrel)) {
        unsigned int tap = __atomic_exchange_n(&fxrel_tap, 0u, __ATOMIC_SEQ_CST);
        if (tap) {
            smplr_set_delay(&fxrel, __atomic_load_n(&fxrel_delay, __ATOMIC_SEQ_CST));
            began = (unsigned int)smplr_begin(&fxrel, tap - 1u);
        }
    }
    /* A lift seen now ends it, except in the block it began in: a tap shorter than a block still plays
       a short entry and exit instead of vanishing. */
    if (!began && fxrel.state == SMPLR_ON &&
        __atomic_load_n(&fxrel_held, __ATOMIC_SEQ_CST) != fxrel.type + 1u)
        smplr_end(&fxrel);
    smplr_process(&fxrel, &output[0].left, frames);
    unsigned int live = smplr_active(&fxrel) ? fxrel.type + 1u : 0u;
    if (__atomic_load_n(&fxrel_live, __ATOMIC_RELAXED) != live)
        __atomic_store_n(&fxrel_live, live, __ATOMIC_SEQ_CST);
}

#endif /* RX3_SAMPLES_FXRELEASE_H */

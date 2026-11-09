/* SPDX-License-Identifier: MPL-2.0 */
#ifndef RX3_STEMS_ISO_H
#define RX3_STEMS_ISO_H
/* ISOLATOR <-> STEMS, per mixer channel (analysis 03-11 §1). SHIFT+CUE of a
 * deck switches its channel; the two channels are independent. In STEMS the
 * channel's ISO HI/MID/LOW knobs (0x501a/b/c, operation 5, raw 0..1023)
 * become stem levels and the native Isolator stays flat: the key handler
 * injects raw 512 into the three bands on the way in and the last physical
 * positions on the way out, through the core's send_key, and takes the knob
 * events while the channel is flat. Mixer channel 1 is deck 1, channel 2
 * deck 2 (the default routing, DA_FIXARE/23).
 *
 * Knob -> parts (role bits: 1 HARMONY, 2 VOCAL, 4 DRUMS, 8 BASS):
 *   HI = VOCAL, MID = HARMONY, LOW = BASS + DRUMS (default: LOW stays the
 *   "kill the low end" it is on the Isolator, kick and bass together, and the
 *   COLOR knob keeps its Sound Color FX);
 *   with RX3_STEMS_ISO_COLOR=1: LOW = BASS and COLOR (0x509d) = DRUMS, the
 *   COLOR knob then taken while the channel is in STEMS.
 * A knob whose parts the package lacks drives the residual (HARMONY/INST),
 * so on a VOCAL + INST track MID and LOW both move INST. A part driven by
 * two knobs plays at the product of their gains.
 *
 * Every knob's raw is remembered, ISO or STEMS. Until a knob has moved since
 * the mod started its position is unknown and 512 (centre) is assumed
 * (DA_FIXARE/22): no address that would read it back is documented.
 */
#define STEMS_KNOB_HI    0x501au
#define STEMS_KNOB_MID   0x501bu
#define STEMS_KNOB_LOW   0x501cu
#define STEMS_KNOB_COLOR 0x509du
#define STEMS_KNOB_FLAT  512u
#define STEMS_KNOB_KNOWN 0x80000000u
#define STEMS_KNOB_COUNT 4u

/* Last raw per deck and knob (HI, MID, LOW, COLOR), STEMS_KNOB_KNOWN set
   once the panel reported it. Written by the key handler only. */
static volatile unsigned int stems_knob_raw[2][STEMS_KNOB_COUNT];
/* Bit deck: the operator chose STEMS for that channel (SHIFT+CUE). */
static volatile unsigned int stems_channel_stems;
/* Bit deck: the native Isolator of that channel was made flat and its knobs
   are taken. Follows stems_channel_stems while the deck has stems. */
static volatile unsigned int stems_channel_flat;
/* RX3_STEMS_ISO_COLOR=1: COLOR = DRUMS, LOW = BASS. */
static unsigned int stems_iso_color;

static int stems_knob_of(unsigned int key)
{
    if (key >= STEMS_KNOB_HI && key <= STEMS_KNOB_LOW) return (int)(key - STEMS_KNOB_HI);
    if (key == STEMS_KNOB_COLOR) return 3;
    return -1;
}

static unsigned int stems_knob_position(unsigned int deck, unsigned int knob)
{
    unsigned int raw = __atomic_load_n(&stems_knob_raw[deck][knob], __ATOMIC_SEQ_CST);
    return raw & STEMS_KNOB_KNOWN ? raw & 1023u : STEMS_KNOB_FLAT;
}

/* The parts one knob drives on a package with `available` parts. */
static unsigned int stems_knob_parts(unsigned int knob, unsigned int available)
{
    static const unsigned int plain[STEMS_KNOB_COUNT] = {2u, 1u, 12u, 0u};
    static const unsigned int color[STEMS_KNOB_COUNT] = {2u, 1u, 8u, 4u};
    if (knob >= STEMS_KNOB_COUNT) return 0u;
    unsigned int parts = (stems_iso_color ? color : plain)[knob];
    if (!parts) return 0u;
    return parts & available ? parts & available : 1u & available;
}

/* The Isolator's own feel (analysis 03-11 §1.3): index (int)(v*255+0.5)
   into IsolatorGainCurve, which reads 0 at 0, 0.508 at 64 and 1.0 from 124,
   capped at 1.0 (the plateau, 100 %). Between those published points the
   curve is approximated by straight lines; the table itself is not read. */
static float stems_knob_gain(unsigned int raw)
{
    if (raw > 1023u) raw = 1023u;
    unsigned int index = (raw * 510u + 1023u) / 2046u;
    if (index >= 124u) return 1.0f;
    if (index <= 64u) return 0.508f * (float)index * (1.0f / 64.0f);
    return 0.508f + 0.492f * (float)(index - 64u) * (1.0f / 60.0f);
}

/* Stem levels from the knob positions, for the parts the knobs in `knobs`
   (bit per knob) drive. Any thread: atomics only. */
static void stems_apply_knobs(unsigned int deck, unsigned int knobs)
{
    struct stems_deck_context *context = &stems_decks[deck];
    unsigned int available = stems_available(context), touched = 0u;
    for (unsigned int k = 0; k < STEMS_KNOB_COUNT; k++)
        if (knobs & (1u << k)) touched |= stems_knob_parts(k, available);
    for (unsigned int bit = 1u; bit <= 8u; bit <<= 1u) {
        if (!(touched & bit)) continue;
        float gain = 1.0f;
        for (unsigned int k = 0; k < STEMS_KNOB_COUNT; k++)
            if (stems_knob_parts(k, available) & bit)
                gain *= stems_knob_gain(stems_knob_position(deck, k));
        stems_set_level(context, bit, (unsigned int)(gain * 100.0f + 0.5f));
    }
}

/* After a load publishes a set, a channel in STEMS follows its knobs. */
static void stems_iso_track_ready(unsigned int deck)
{
    if (__atomic_load_n(&stems_channel_flat, __ATOMIC_SEQ_CST) & (1u << deck))
        stems_apply_knobs(deck, (1u << STEMS_KNOB_COUNT) - 1u);
}
#endif /* RX3_STEMS_ISO_H */

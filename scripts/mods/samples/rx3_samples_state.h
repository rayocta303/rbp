/* SPDX-License-Identifier: MPL-2.0 */
static volatile unsigned int samples_callbacks_enabled;
static unsigned int samples_volume = SAMPLES_VOLUME_DEFAULT;
static unsigned int samples_volume_touched;
/* One bank per deck. With one bank on both decks, deck 2's slots point at
   deck 1's audio (block 0, nothing of their own to unmap) and its settings
   are a copy of deck 1's. Voices are per deck either way. */
static struct samples_config samples_config[SAMPLES_DECKS];
static struct sample_slot samples_slots[SAMPLES_DECKS][SAMPLES_PAD_COUNT];
static struct sample_bank_slot samples_bank[SAMPLES_DECKS][SAMPLES_PAD_COUNT];

/* Pads whose press the sampler took, one bit per pad and per deck. Their
   release is taken too, even when the page changed while the pad was down,
   so the player never sees half of a gesture (as captured_pad_mask in
   stems). Input events are serialized by the player. */
static unsigned int samples_captured[SAMPLES_DECKS];

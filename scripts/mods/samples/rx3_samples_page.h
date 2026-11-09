/* SPDX-License-Identifier: MPL-2.0 */
#ifndef RX3_SAMPLES_PAGE_H
#define RX3_SAMPLES_PAGE_H

/* The titles of RELEASE FX. The player draws that page as eight pictures (BEAT
   BRAKE, VINYL BRAKE...) and a title picture, the same set in each deck's pad
   window; the core paints over them, in the pad's colour, with the text this
   page answers for. Each deck answers for its own bank, so two banks show
   sixteen different titles. Ids 5477 to 5493 are the title and the eight OFF/ON
   pairs of the page (docs/analisi/01-05-06, 2.1). */
#define SAMPLES_PAGE_TITLE_IMAGE 5477u
#define SAMPLES_PAGE_LAST_IMAGE  5493u

static int samples_page_active(unsigned int deck)
{
    return deck < SAMPLES_DECKS && __atomic_load_n(&samples_callbacks_enabled, __ATOMIC_SEQ_CST);
}

/* 0 no sound (the cell stays dark), 1 idle, 2 sounding. */
static unsigned int samples_page_state(unsigned int deck, unsigned int cell)
{
    if (deck >= SAMPLES_DECKS || cell >= SAMPLES_PAD_COUNT ||
        !__atomic_load_n(&samples_bank[deck][cell].frames, __ATOMIC_SEQ_CST))
        return 0u;
    return samples_voice_position(&samples_slots[deck][cell]) == SAMPLE_SLOT_IDLE ? 1u : 2u;
}

static uint16_t samples_page_colour(unsigned int deck, unsigned int cell)
{
    uint32_t rgb = deck < SAMPLES_DECKS && cell < SAMPLES_PAD_COUNT
        ? __atomic_load_n(&samples_config[deck].colour[cell], __ATOMIC_SEQ_CST) : 0u;
    return (uint16_t)(((rgb >> 8u) & 0xf800u) | ((rgb >> 5u) & 0x07e0u) | ((rgb >> 3u) & 0x1fu));
}

/* The label the pad was given; a pad with a sound and no label shows its
   number, so a loaded pad is never an empty box. */
static const char *samples_page_caption(unsigned int deck, unsigned int cell)
{
    static const char *const numbers[SAMPLES_PAD_COUNT] = {
        "PAD 1", "PAD 2", "PAD 3", "PAD 4", "PAD 5", "PAD 6", "PAD 7", "PAD 8"
    };
    if (deck >= SAMPLES_DECKS || cell >= SAMPLES_PAD_COUNT) return 0;
    const char *label = samples_config[deck].label[cell];
    return label[0] ? label : numbers[cell];
}

static const char *samples_page_title(unsigned int deck)
{
    (void)deck;
    return "SAMPLER";
}

/* Watcher thread: has a sound started or ended since last asked. A one-shot
   that runs out by itself is only seen this way. Loading a bank is not a
   change worth a repaint: nothing is on the page yet, and a repaint asked
   while another page is up is what the core must never do. */
static int samples_page_needs_refresh(void)
{
    static unsigned int seen;
    unsigned int now = 0u;
    if (!__atomic_load_n(&samples_callbacks_enabled, __ATOMIC_SEQ_CST)) return 0;
    for (unsigned int deck = 0; deck < SAMPLES_DECKS; deck++)
        for (unsigned int pad = 0; pad < SAMPLES_PAD_COUNT; pad++)
            if (samples_page_state(deck, pad) == 2u)
                now |= 1u << (deck * SAMPLES_PAD_COUNT + pad);
    int changed = seen != now;
    seen = now;
    return changed;
}

static const struct rx3_pad_page samples_page = {
    .first_image = SAMPLES_PAGE_TITLE_IMAGE, .last_image = SAMPLES_PAGE_LAST_IMAGE,
    .title_image = SAMPLES_PAGE_TITLE_IMAGE,
    .active = samples_page_active, .cell_state = samples_page_state,
    .colour = samples_page_colour, .caption = samples_page_caption,
    .title = samples_page_title, .needs_refresh = samples_page_needs_refresh
};

/* A pad started or stopped on the page: repaint now rather than at the next
   look of the watcher. */
static void samples_page_changed(void)
{
    if (framework->panels->invalidate_page) framework->panels->invalidate_page();
}

#endif /* RX3_SAMPLES_PAGE_H */

/* SPDX-License-Identifier: MPL-2.0 */
#ifndef RX3_STEMWAVE_DECL_H
#define RX3_STEMWAVE_DECL_H

#include "../core/api/rx3_palette.h"

#define WAVEFORM_STATE_BASE   ((unsigned long)0x03275930)
#define WAVEFORM_STATE_STRIDE ((unsigned long)0x0001e8a0)
#define WAVEFORM_MODE_OFFSET  0x0cu
#define WAVEFORM_COUNT_FLAT   0x14u
#define WAVEFORM_COUNT_BANDS  0x1cu
#define WAVEFORM_COUNT_BLUE   0x24u
#define WAVEFORM_COLUMNS_BASE  ((unsigned long)0x0449ff08)
#define WAVEFORM_COLUMN_STRIDE ((unsigned long)0x0083d600)
#define WAVEFORM_COLUMN_LIMIT  540000u
#define WAVEFORM_3BAND_BASE ((unsigned long)0x059901b0)
#define WAVEFORM_3BAND_BYTES 0xab0u
#define WAVEFORM_PALETTE_BASE ((unsigned long)0x004422f8)
#define WAVEFORM_PALETTE_BYTES 0x20u
#define WAVEFORM_REQUEST_RENEW ((unsigned long)0x001856fc)

enum waveform_shape {
    WAVEFORM_BLUE = 0,
    WAVEFORM_RGB = 1,
    WAVEFORM_3BAND = 2
};

#define STEMWAVE_COLOUR_NONE   0x0000u
/* One colour per stem bit, from core/api/rx3_palette.h. */
#define STEMWAVE_COLOUR_ONE    RX3_PAL_WAVE_HARMONY_565
#define STEMWAVE_COLOUR_TWO    RX3_PAL_WAVE_VOCAL_565
#define STEMWAVE_COLOUR_FOUR   RX3_PAL_WAVE_DRUMS_565
#define STEMWAVE_COLOUR_EIGHT  RX3_PAL_WAVE_BASS_565
/* The initial cache is the ordinary two-stem full mix. */
#define STEMWAVE_MASK_INITIAL 3u

struct waveform_column {
    uint8_t amplitude[3];
    uint8_t reserved;
    uint32_t colour[3];
};
_Static_assert(sizeof(struct waveform_column) == 16u, "waveform column stride");

struct stemwave_selection {
    unsigned int selected;
    unsigned int available;
    unsigned int steps;   /* each role's knob in tenths (0..10), 4 bits per role */
    unsigned int ready;   /* the stems are loaded and playing */
};

struct stemwave_style {
    unsigned int colour;
    unsigned int roles;
    int suppress_high;
};

struct stemwave_deck {
    volatile unsigned int drawn_mask;
    volatile unsigned int pending;
    volatile unsigned int columns;
    uint8_t package_amplitudes[WAVEFORM_COLUMN_LIMIT * 3u];
    uint8_t blue_original[WAVEFORM_3BAND_BYTES];
    uint8_t blue_filtered[WAVEFORM_3BAND_BYTES];
    volatile unsigned int blue_saved;
    /* Native height over the package mix's, x256, measured once per track and
       shape while the columns still hold the player's own (0: not yet). */
    volatile unsigned int ratio_x256, ratio_shape;
};

#endif /* RX3_STEMWAVE_DECL_H */

/* SPDX-License-Identifier: MPL-2.0
 *
 * The mod's colours, in one place (rx3-ventotto, roadmap 9 / P6).
 *
 * Every colour the mod paints with is written down here once and read from
 * here: the core's pad row, the light theme's fills, inks and pixel tables,
 * the stems and KEY panels, Key Match, and build_labels.py, which parses this
 * file to draw the glyph atlas and the tab selection. A colour that also
 * exists in artwork (the atlas grounds, the light tab ground) is therefore the
 * same number in the artwork and in the code that paints next to it.
 *
 * Two forms, by what the consumer takes:
 *   RGB888  0xRRGGBB, the default; the pad fills and the theme take this;
 *   RGB565  suffixed _565, for what the player stores as a 16-bit pixel
 *           (semantic accents, the Camelot wheel, recoloured images).
 * Conversions are macros so a derived value is never typed a second time.
 *
 * Only constants: no types, no functions, nothing that needs libc, so the
 * Python generator and the host tests can read it as text.
 */

#ifndef RX3_PALETTE_H
#define RX3_PALETTE_H

/* --- Conversions ------------------------------------------------------- */
#define RX3_PAL_R(c) (((c) >> 16) & 0xffu)
#define RX3_PAL_G(c) (((c) >> 8) & 0xffu)
#define RX3_PAL_B(c) ((c) & 0xffu)
/* Truncating, the same rounding build_labels.py and the theme pack with. */
#define RX3_PAL_565(c) ((((c) >> 8) & 0xf800u) | (((c) >> 5) & 0x07e0u) | \
                        (((c) >> 3) & 0x001fu))

/* --- Pioneer's colour language (both themes) ---------------------------- */
/* Measured from the BEAT FX captions in imagedata.dat, ids 0x1439..: dim or
   white lettering on black or blue. Blue means "selected" in both rooms. */
#define RX3_PAL_SELECT      0x007de6u   /* #007DE6, Pioneer selection blue */
#define RX3_PAL_SELECT_INK  0xffffffu   /* white lettering on the blue */

/* --- Dark room ----------------------------------------------------------- */
#define RX3_PAL_DARK_GROUND 0x000000u   /* caption and pad ground */
#define RX3_PAL_DARK_DIM    0x626562u   /* inactive lettering (98,101,98) */
#define RX3_PAL_DARK_INK    0xffffffu   /* pressed / status lettering */
#define RX3_PAL_DARK_LINE   0x41474du   /* pad frame */
#define RX3_PAL_DARK_TRACK  0x30363cu   /* slider track */
#define RX3_PAL_DARK_HANDLE 0xffffffu   /* slider fill and handle */

/* --- Light room ---------------------------------------------------------- */
/* L4: GROUND under the panes, PANEL for the panes (the light tab artwork and
   the light atlas are drawn on it: 0xdedb in RGB565), INK and DIM for text. */
#define RX3_PAL_LIGHT_GROUND 0xc9c9ccu  /* #C9C9CC */
#define RX3_PAL_LIGHT_PANEL  0xdedbdeu  /* #DEDBDE */
#define RX3_PAL_LIGHT_INK    0x1c1c1eu  /* #1C1C1E, native text ink (L1) */
#define RX3_PAL_LIGHT_DIM    0x6e6e6eu  /* #6E6E6E, native grey text (L1) */
/* The atlas lettering, measured off the light artwork of a build that shipped
   with the display mode: a touch lighter than INK. */
#define RX3_PAL_LIGHT_LABEL_INK 0x292829u  /* pressed / status (41,40,41) */
#define RX3_PAL_LIGHT_LABEL_DIM 0x525152u  /* inactive (82,81,82) */
#define RX3_PAL_LIGHT_LINE   0x8c949cu  /* pad frame, LINE of L4 */
#define RX3_PAL_LIGHT_TRACK  0xa5adb5u  /* slider track */
#define RX3_PAL_LIGHT_HANDLE RX3_PAL_LIGHT_LABEL_INK

/* --- Semantic accents (RGB565, theme independent) ------------------------ */
/* A widget that names no colour of its own: the selection blue. */
#define RX3_PAL_ACCENT_DEFAULT_565 RX3_PAL_565(RX3_PAL_SELECT)
/* Stem roles, as the user's rekordbox roles: drums blue, vocal green,
   harmony red; grey for a slot with no stem. */
#define RX3_PAL_STEM_DRUMS_565   0x001fu
#define RX3_PAL_STEM_VOCAL_565   0x07e0u
#define RX3_PAL_STEM_HARMONY_565 0xf800u
#define RX3_PAL_STEM_NONE_565    0x8410u
#define RX3_PAL_STEM_BASS_565    0xa01fu
/* The same roles on the stem waveform, one per stem bit (1, 2, 4, 8): drums
   are a lighter blue there, and bass yellow. */
#define RX3_PAL_WAVE_HARMONY_565 0xf800u
#define RX3_PAL_WAVE_VOCAL_565   0x07e0u
#define RX3_PAL_WAVE_DRUMS_565   0x7e5fu
#define RX3_PAL_WAVE_BASS_565    0xa01fu
/* Key Match: the three badge recolours, in the order of its categories. */
#define RX3_PAL_MATCH_YELLOW_565 0xffe0u
#define RX3_PAL_MATCH_ORANGE_565 0xfc00u
#define RX3_PAL_MATCH_RED_565    0xf800u
/* Mixed In Key's published Camelot wheel, 1A 1B 2A 2B ... 12A 12B,
   https://mixedinkey.com/camelot-wheel/, quantised to RGB565. */
#define RX3_PAL_CAMELOT_565 \
    0xb7fcu,0x8ffau, 0xc7f8u,0xa7f3u, 0xd7d4u,0xb7aeu, \
    0xe734u,0xd68eu, 0xf635u,0xf50fu, 0xfd77u,0xfbf1u, \
    0xf579u,0xf3f6u, 0xe57du,0xd3fbu, 0xd57fu,0xb3ffu, \
    0xc61fu,0x9d3fu, 0xb73fu,0x8edfu, 0xafffu,0x7fffu

#endif /* RX3_PALETTE_H */

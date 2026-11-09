/* SPDX-License-Identifier: MPL-2.0 */
#ifndef RX3_THEME_INK_H
#define RX3_THEME_INK_H
/* Native text ink in the light table (L1).
 *
 * The player's text is white or light grey, drawn for a dark room; on the
 * light ground it all but disappears. The core hands every native text draw's
 * colour field to this policy while the light table is shown, and white and
 * light greys come back as INK #1C1C1E and DIM #6E6E6E. Colours, and greys
 * already dark, are left exactly as they are.
 *
 * The field is not plain RGB. NS_PALRender_DrawText decodes it by the pixel
 * format of the window it draws into (REFERENCES §4): the low byte alone, the
 * whole word, or a packed 24-bit value unpacked into 5/6/5. The window's
 * format is not visible here, so the encoding is read off the value:
 *
 *   above 0xffffff     a whole 32-bit word: not documented, left alone;
 *   0x10000..0xffffff  packed 24-bit RGB888, the 5/6/5 branch;
 *   0x100..0xffff      a 16-bit word, RGB565, the pixel these windows pack;
 *   0..0xff            the low-byte branch: what it means is not measured, so
 *                      it is left alone (0 is also the browse's native style).
 *
 * A 24-bit colour with no red at all (pure cyan, green, blue) has the same
 * value as an RGB565 pixel, and would be read as one. DA_FIXARE/28 has the
 * measurement that settles it.
 */
#define THEME_INK_UNKNOWN 0u
#define THEME_INK_RGB565 1u
#define THEME_INK_RGB888 2u
/* A whole word (windows other than indexed): the low three bytes are the
   colour, the top byte is kept as it is (RX3 photo 2026-10-03: white text
   in these windows stayed white). */
#define THEME_INK_RGB888_ALPHA 3u

/* INK #1C1C1E and DIM #6E6E6E from core/api/rx3_palette.h, in both
   encodings. */
#define THEME_TEXT_INK_888 RX3_PAL_LIGHT_INK
#define THEME_TEXT_DIM_888 RX3_PAL_LIGHT_DIM
#define THEME_TEXT_INK_565 RX3_PAL_565(RX3_PAL_LIGHT_INK)
#define THEME_TEXT_DIM_565 RX3_PAL_565(RX3_PAL_LIGHT_DIM)

/* A text grey is neutral within this many steps (8 bits per channel), and
   counts as white from the first luma and as light grey from the second.
   Darker greys already read on the light ground. */
#define THEME_TEXT_NEUTRAL_SPREAD 0x18u
#define THEME_TEXT_WHITE_LUMA 0xd0u
#define THEME_TEXT_GREY_LUMA 0x80u

static unsigned int theme_ink_encoding(uint32_t ink)
{
    if (ink > 0xffffffu) return THEME_INK_RGB888_ALPHA;
    if (ink > 0xffffu) return THEME_INK_RGB888;
    if (ink > 0xffu) return THEME_INK_RGB565;
    return THEME_INK_UNKNOWN;
}

/* Render thread, through the core's image service. Pure: the same ink always
   maps to the same ink, and nothing is read but the value. The layer is there
   for a per-window table once the formats are measured (DA_FIXARE/28). */
static uint32_t theme_text_ink(uint32_t ink, unsigned int layer)
{
    (void)layer;
    unsigned int encoding = theme_ink_encoding(ink);
    unsigned int red, green, blue;
    if (encoding == THEME_INK_RGB888 || encoding == THEME_INK_RGB888_ALPHA) {
        red = (ink >> 16) & 0xffu;
        green = (ink >> 8) & 0xffu;
        blue = ink & 0xffu;
    } else if (encoding == THEME_INK_RGB565) {
        red = (ink >> 11) & 0x1fu;
        green = (ink >> 5) & 0x3fu;
        blue = ink & 0x1fu;
        red = red << 3 | red >> 2;
        green = green << 2 | green >> 4;
        blue = blue << 3 | blue >> 2;
    } else {
        return ink;
    }
    unsigned int high = red > green ? red : green, low = red < green ? red : green;
    if (blue > high) high = blue;
    if (blue < low) low = blue;
    if (high - low > THEME_TEXT_NEUTRAL_SPREAD) return ink;
    unsigned int luma = (red * THEME_LUMA_RED + green * THEME_LUMA_GREEN +
                         blue * THEME_LUMA_BLUE) >> 8;
    int white = luma >= THEME_TEXT_WHITE_LUMA;
    if (!white && luma < THEME_TEXT_GREY_LUMA) return ink;
    if (encoding == THEME_INK_RGB888_ALPHA)
        return (ink & 0xff000000u) | (white ? THEME_TEXT_INK_888 : THEME_TEXT_DIM_888);
    if (encoding == THEME_INK_RGB888)
        return white ? THEME_TEXT_INK_888 : THEME_TEXT_DIM_888;
    return white ? THEME_TEXT_INK_565 : THEME_TEXT_DIM_565;
}
#endif

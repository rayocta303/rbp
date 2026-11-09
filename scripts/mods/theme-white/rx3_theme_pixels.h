/* SPDX-License-Identifier: MPL-2.0 */
#ifndef RX3_THEME_PIXELS_H
#define RX3_THEME_PIXELS_H

/* The two per-pixel conversions and what they are decided from.
 *
 * Nothing here reads a player address or writes a log, so a host compiles
 * and runs it. tests/test_theme_pixels.py does exactly that, which is the
 * only way either conversion gets checked without a deck in front of you.
 */

/* Split one RGB565 word into eight-bit channels, as the panel expands it. */
static void theme_unpack(uint16_t pixel, unsigned int *red, unsigned int *green,
                         unsigned int *blue)
{
    *red = (unsigned int)(pixel >> 8) & 0xf8u;
    *green = (unsigned int)(pixel >> 3) & 0xfcu;
    *blue = ((unsigned int)pixel & 0x1fu) * 8u;
}

static uint16_t theme_pack(unsigned int red, unsigned int green, unsigned int blue)
{
    uint16_t packed = (uint16_t)(((red & 0xf8u) << 8) | ((green & 0xfcu) << 3) |
                                 (blue >> 3));
    /* One step of green off the colour key, the same nudge the logo container
       makes. A pixel that lands on the key would be punched out of the image
       it belongs to. */
    return packed == COLOUR_KEY ? (uint16_t)(packed | 1u << 5) : packed;
}

static unsigned int theme_spread(unsigned int red, unsigned int green,
                                 unsigned int blue)
{
    unsigned int high = red > green ? red : green;
    unsigned int low = red < green ? red : green;
    if (blue > high)
        high = blue;
    if (blue < low)
        low = blue;
    return high - low;
}

/* Whether this bitmap carries a picture rather than interface furniture.
 *
 * Counted rather than declared: a quarter of the opaque pixels being vivid is
 * what separates a sleeve or a waveform from a button. Chrome is what the
 * theme exists to repaint, and repainting a picture would be vandalism.
 */
static inline int theme_is_artwork(const uint16_t *pixels, unsigned int count)
{
    unsigned int opaque = 0;
    unsigned int vivid = 0;
    for (unsigned int index = 0; index < count; index++) {
        uint16_t pixel = pixels[index];
        if (pixel == COLOUR_KEY)
            continue;
        unsigned int red;
        unsigned int green;
        unsigned int blue;
        theme_unpack(pixel, &red, &green, &blue);
        opaque++;
        if (theme_spread(red, green, blue) > THEME_VIVID_SPREAD)
            vivid++;
    }
    return opaque != 0 && opaque * THEME_ARTWORK_PERCENT <= vivid * 100u;
}

/* Light conversion v2 (rx3-ventotto).
 *
 * The v1 conversion above inverted each channel on its own, which turned the
 * Pioneer blue-grey beige, capped every colour at a luma of 0x66, which turned
 * orange brown, and kept the black antialias around coloured glyphs. v2 is a
 * function of the pixel and of the image class only, so it is computed once
 * into four 64 K tables and the conversion loop is a lookup.
 *
 * Neutrals invert their luminance only, from PANEL (black) to INK (white), and
 * keep a little of their original tint. A colour keeps its hue at full
 * strength: what is darker than the class threshold is read as that colour at
 * partial coverage over black and recomposed over PANEL, which removes the dark
 * halo. Only colours too pale to read on PANEL (yellow, cyan) are darkened.
 *
 * Doubles throughout: the tables are built once, off the render thread, and
 * double keeps them identical to docs/analisi/tema/script/conv.py.
 */
#define THEME_V2_CHROME 0u     /* interface furniture */
#define THEME_V2_ARTWORK 1u    /* opaque artwork: tiles, key buttons */
#define THEME_V2_GLYPH 2u      /* a neutral glyph drawn on black */
#define THEME_V2_KEYED_ART 3u  /* coloured glyphs cut out with the colour key */
#define THEME_V2_CLASSES 4u
#define THEME_V2_LUT_BYTES (THEME_V2_CLASSES * 65536u * 2u)

/* PANEL and INK, from core/api/rx3_palette.h. */
static const double theme_v2_panel[3] = {
    (double)RX3_PAL_R(RX3_PAL_LIGHT_PANEL), (double)RX3_PAL_G(RX3_PAL_LIGHT_PANEL),
    (double)RX3_PAL_B(RX3_PAL_LIGHT_PANEL)
};
static const double theme_v2_ink[3] = {
    (double)RX3_PAL_R(RX3_PAL_LIGHT_INK), (double)RX3_PAL_G(RX3_PAL_LIGHT_INK),
    (double)RX3_PAL_B(RX3_PAL_LIGHT_INK)
};

static double theme_v2_clip(double value, double low, double high)
{
    return value < low ? low : value > high ? high : value;
}

static uint16_t theme_v2_pack(double red, double green, double blue)
{
    unsigned int r = (unsigned int)(int)theme_v2_clip(red, 0.0, 255.0);
    unsigned int g = (unsigned int)(int)theme_v2_clip(green, 0.0, 255.0);
    unsigned int b = (unsigned int)(int)theme_v2_clip(blue, 0.0, 255.0);
    return theme_pack(r, g, b);
}

static double theme_v2_luma(double red, double green, double blue)
{
    return 0.2126 * red + 0.7152 * green + 0.0722 * blue;
}

/* Chrome and glyph classes: the neutral inversion blended with the colour
   recomposition by how far the pixel is from neutral. */
static uint16_t theme_v2_chrome(uint16_t pixel, int glyph)
{
    unsigned int ri, gi, bi;
    theme_unpack(pixel, &ri, &gi, &bi);
    double c[3] = {(double)ri, (double)gi, (double)bi};
    double hi = c[0] > c[1] ? c[0] : c[1];
    double lo = c[0] < c[1] ? c[0] : c[1];
    if (c[2] > hi) hi = c[2];
    if (c[2] < lo) lo = c[2];
    double spread = hi - lo;
    double t = theme_v2_luma(c[0], c[1], c[2]) / 255.0;
    double k = theme_v2_clip(spread / 0x38, 0.0, 1.0);
    double mean = (c[0] + c[1] + c[2]) / 3;
    double threshold = glyph ? 0xb0 : 0x50;
    double alpha = theme_v2_clip(hi / threshold, 0.0, 1.0);
    double up = hi < threshold ? threshold / (hi > 1.0 ? hi : 1.0) : 1.0;
    double p[3] = {c[0] * up, c[1] * up, c[2] * up};
    double lp = theme_v2_luma(p[0], p[1], p[2]);
    double scale = lp > 150 ? 150 / (lp > 1.0 ? lp : 1.0) : 1.0;
    double blend = glyph ? theme_v2_clip((spread - 0x08) / 0x10, 0.0, 1.0)
                         : theme_v2_clip((spread - 0x20) / 0x20, 0.0, 1.0);
    double out[3];
    for (unsigned int i = 0; i < 3; i++) {
        double neutral = theme_v2_panel[i] + (theme_v2_ink[i] - theme_v2_panel[i]) * t;
        neutral += (c[i] - mean) * 0.6 * k;
        double colour = alpha * (p[i] * scale) + (1 - alpha) * theme_v2_panel[i];
        out[i] = neutral * (1 - blend) + colour * blend;
    }
    return theme_v2_pack(out[0], out[1], out[2]);
}

/* Artwork classes: unchanged except the very dark pixels, which are the
   antialias towards a black ground and are recomposed over PANEL. */
static uint16_t theme_v2_artwork(uint16_t pixel, double threshold, int cap)
{
    unsigned int ri, gi, bi;
    theme_unpack(pixel, &ri, &gi, &bi);
    double c[3] = {(double)ri, (double)gi, (double)bi};
    double hi = c[0] > c[1] ? c[0] : c[1];
    if (c[2] > hi) hi = c[2];
    double v = hi > 1.0 ? hi : 1.0;
    double al = theme_v2_clip(hi / threshold, 0.0, 1.0);
    double out[3];
    for (unsigned int i = 0; i < 3; i++)
        out[i] = hi < threshold ? al * c[i] * threshold / v * (hi > 0 ? 1.0 : 0.0) +
                                      (1 - al) * theme_v2_panel[i]
                                : c[i];
    uint16_t packed = theme_v2_pack(out[0], out[1], out[2]);
    if (!cap)
        return packed;
    /* Keyed glyphs sit straight on the panel: a pale colour is darkened so it
       still reads against it. */
    theme_unpack(packed, &ri, &gi, &bi);
    double l = theme_v2_luma((double)ri, (double)gi, (double)bi);
    unsigned int high = ri > gi ? ri : gi, low = ri < gi ? ri : gi;
    if (bi > high) high = bi;
    if (bi < low) low = bi;
    double s = l > 160 && high - low > 0x38 ? 150 / (l > 1.0 ? l : 1.0) : 1.0;
    return theme_v2_pack(ri * s, gi * s, bi * s);
}

static uint16_t theme_v2_pixel(uint16_t pixel, unsigned int image_class)
{
    if (pixel == COLOUR_KEY)
        return pixel;
    switch (image_class) {
    case THEME_V2_ARTWORK: return theme_v2_artwork(pixel, 0x60, 0);
    case THEME_V2_GLYPH: return theme_v2_chrome(pixel, 1);
    case THEME_V2_KEYED_ART: return theme_v2_artwork(pixel, 0xb0, 1);
    default: return theme_v2_chrome(pixel, 0);
    }
}

/* All four tables, class after class. 512 KiB, built once by the watcher. */
static void theme_v2_build_luts(uint16_t *luts)
{
    for (unsigned int image_class = 0; image_class < THEME_V2_CLASSES; image_class++)
        for (unsigned int pixel = 0; pixel < 65536u; pixel++)
            luts[image_class * 65536u + pixel] =
                theme_v2_pixel((uint16_t)pixel, image_class);
}

/* The class of one image, from counts gathered over its opaque pixels. */
static unsigned int theme_v2_classify(unsigned int opaque, unsigned int vivid,
                                      unsigned int near_black, int keyed)
{
    if (opaque && opaque * THEME_ARTWORK_PERCENT <= vivid * 100u)
        return keyed ? THEME_V2_KEYED_ART : THEME_V2_ARTWORK;
    if (opaque && near_black * 2u >= opaque)
        return THEME_V2_GLYPH;
    return THEME_V2_CHROME;
}

/* Eight dimmed BANK bitmaps carry one pixel value each that has to land on an
 * exact colour, whatever the class conversion would make of it.
 */
static uint16_t theme_light_special(unsigned int image, uint16_t pixel, uint16_t converted)
{
    if ((image == 0xa3fu || image == 0xa60u || image == 0xa81u || image == 0xaa1u) &&
        pixel == 0x3907u) return 0xce59u;
    if ((image == 0xa40u || image == 0xa61u || image == 0xa82u || image == 0xaa2u) &&
        pixel == 0x28e6u) return RX3_PAL_565(RX3_PAL_LIGHT_PANEL);
    return converted;
}

/* One pixel, for a lit room, without the tables: the reference the tests and
   the desktop preview use. The renderer reads the same values from the LUTs. */
static inline uint16_t theme_pixel_light_for_image(unsigned int image, uint16_t pixel,
                                                   unsigned int image_class)
{
    return theme_light_special(image, pixel, theme_v2_pixel(pixel, image_class));
}

/* One pixel, for a dark room.
 *
 * The mirror of the light conversion, and a narrower one. A pixel with any
 * real colour in it is left alone: only what is already grey is taken down to
 * roughly a third of its brightness, which is what turns the stock chrome
 * black without touching a status colour or a sleeve.
 *
 * The three channels share one multiplier. Read from the disassembly at
 * 0003164c, where the shifts differ per channel but the constant does not.
 */
static uint16_t theme_pixel_dark(uint16_t pixel)
{
    unsigned int green = (unsigned int)(pixel >> 3) & 0xfcu;
    unsigned int red = (unsigned int)(pixel >> 8) & 0xf8u;
    unsigned int blue = ((unsigned int)pixel << 3) & 0xffu;
    unsigned int high = red > green ? red : green;
    unsigned int low = red > green ? green : red;
    if (low > blue)
        low = blue;
    if (high < blue)
        high = blue;
    if (pixel == COLOUR_KEY)
        return pixel;
    if (high - low > THEME_DARK_SPREAD)
        return pixel;
    return (uint16_t)((((red * THEME_DARK_SCALE) >> 11) & 0x7800u) |
                      ((blue * THEME_DARK_SCALE) >> 22) |
                      (((green * THEME_DARK_SCALE) >> 16) & ~0x1fu));
}

#endif

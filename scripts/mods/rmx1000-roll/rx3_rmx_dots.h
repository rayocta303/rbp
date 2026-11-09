/* SPDX-License-Identifier: MPL-2.0 */
/* A window of round LEDs, as a reusable component: a matrix of ROWS x COLS dots
 * with a brightness of 0..255 per dot, drawn from a few pre-rendered sprites.
 * It is meant as the indicator of an X-PAD (position and level as a bell that
 * moves with the finger) and, when wanted, as a 5x7 dot font for short
 * words. Original work: the shapes, the sizes and the glyphs are drawn here
 * from scratch; nothing is taken from any product's artwork or font.
 *
 * Layout. 9 rows by 35 columns, the columns grouped in 5 blocks of 7 (the
 * five blocks are the five zones of a pad; for a roll, five lengths). A
 * brightness becomes one of RMXD_LEVELS sprites: the dimmer a dot, the
 * smaller and the greyer it is; the brightest are larger and white with a
 * small halo; the dimmest are barely visible.
 *
 * Cost. The sprites are built once (integer arithmetic only, no sqrt, no
 * libm), then a frame is a copy of one small RGB565 tile per cell that
 * changed since the last frame (rmxd_draw returns how many), never a
 * circle computed per frame. No allocation, no I/O, no hidden state: the
 * caller owns every buffer.
 */
#ifndef RX3_RMX_DOTS_H
#define RX3_RMX_DOTS_H

#define RMXD_ROWS 9u
#define RMXD_COLS 35u
#define RMXD_BLOCK 7u
#define RMXD_BLOCKS 5u
#define RMXD_LEVELS 16u                 /* sprites, brightness steps */
#define RMXD_SPRITE_MAX 32u

struct rmxd_field { uint8_t v[RMXD_ROWS][RMXD_COLS]; };       /* brightness 0..255 */
struct rmxd_shown { uint8_t level[RMXD_ROWS][RMXD_COLS]; };   /* sprite last drawn; 255 = nothing yet */

enum rmxd_pattern {
    RMXD_OFF = 0,
    RMXD_BELLS,     /* the same bell in every block, its peak following the finger */
    RMXD_ZONE,      /* the pad's zone under the finger rings, the others sleep (roll lengths) */
    RMXD_LEVEL      /* a bar from the left, as long as the level (a meter) */
};

static inline uint8_t rmxd_clamp8(unsigned int x) { return (uint8_t)(x > 255u ? 255u : x); }

/* A bell: 255 at distance 0 falling to 0 at `half` (both in sixteenths of a dot),
   smooth at the top and with soft flanks (the square of an inverted parabola). */
static inline unsigned int rmxd_bell(unsigned int distance16, unsigned int half16)
{
    if (!half16 || distance16 >= half16) return 0u;
    unsigned int d2 = distance16 * distance16, h2 = half16 * half16;
    unsigned int u = 255u * (h2 - d2) / h2;                 /* 255 .. 0 */
    return u * u / 255u;
}

static inline unsigned int rmxd_absdiff(unsigned int a, unsigned int b) { return a > b ? a - b : b - a; }

/* Fills the field for an X-PAD at (x, y), each 0..255 (y 255 = top), `level`
   0..255 scaling everything. Block width and row height are in dots. */
static inline void rmxd_field_xpad(struct rmxd_field *f, unsigned int pattern, unsigned int x,
                                   unsigned int y, unsigned int level)
{
    for (unsigned int r = 0; r < RMXD_ROWS; r++)
        for (unsigned int c = 0; c < RMXD_COLS; c++) f->v[r][c] = 0u;
    if (pattern == RMXD_OFF) return;
    if (x > 255u) x = 255u;
    if (y > 255u) y = 255u;
    if (pattern == RMXD_LEVEL) {
        /* Columns lit = level / 255 of 35, the last one partly; every row the same. */
        unsigned int lit16 = level * RMXD_COLS * 16u / 255u;
        for (unsigned int c = 0; c < RMXD_COLS; c++) {
            unsigned int edge = (c + 1u) * 16u;
            unsigned int v = lit16 >= edge ? 255u : lit16 + 16u > edge ? (lit16 + 16u - edge) * 255u / 16u : 0u;
            for (unsigned int r = 0; r < RMXD_ROWS; r++) f->v[r][c] = (uint8_t)v;
        }
        return;
    }
    /* Vertical peak: y = 255 on the top row, 0 on the bottom one (sixteenths of a row). */
    unsigned int peak_row16 = (255u - y) * (RMXD_ROWS - 1u) * 16u / 255u;
    /* The finger's zone, and its place in the zone in sixteenths of a column. */
    unsigned int zone = x * RMXD_BLOCKS / 256u;
    unsigned int in_zone16 = (x * RMXD_BLOCKS * 16u * (RMXD_BLOCK - 1u) / 256u) - zone * 16u * (RMXD_BLOCK - 1u);
    for (unsigned int b = 0; b < RMXD_BLOCKS; b++) {
        unsigned int amplitude = 256u;
        unsigned int peak_col16 = in_zone16;
        if (pattern == RMXD_ZONE && b != zone) { amplitude = 40u; peak_col16 = 16u * (RMXD_BLOCK / 2u); }
        for (unsigned int c = 0; c < RMXD_BLOCK; c++) {
            unsigned int col16 = c * 16u;
            unsigned int bx = rmxd_bell(rmxd_absdiff(col16, peak_col16), 16u * 4u);
            for (unsigned int r = 0; r < RMXD_ROWS; r++) {
                unsigned int by = rmxd_bell(rmxd_absdiff(r * 16u, peak_row16), 16u * 5u);
                unsigned int v = bx * by / 255u * amplitude / 256u * level / 255u;
                f->v[r][b * RMXD_BLOCK + c] = rmxd_clamp8(v);
            }
        }
    }
}

/* 5x7 dot font: rows of 5 bits, top first, the leftmost dot the highest bit.
   Space . / - + % 0-9 A-Z; anything else is a space. */
static const uint8_t rmxd_font[42][7] = {
    {0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 12, 12},
    {1, 1, 2, 4, 8, 16, 16},
    {0, 0, 0, 31, 0, 0, 0},
    {0, 4, 4, 31, 4, 4, 0},
    {25, 26, 2, 4, 8, 11, 19},
    {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14}, {14, 17, 1, 2, 4, 8, 31},
    {30, 1, 1, 14, 1, 1, 30}, {2, 6, 10, 18, 31, 2, 2}, {31, 16, 30, 1, 1, 17, 14},
    {6, 8, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8}, {14, 17, 17, 14, 17, 17, 14},
    {14, 17, 17, 15, 1, 2, 12},
    {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30}, {14, 17, 16, 16, 16, 17, 14},
    {30, 17, 17, 17, 17, 17, 30}, {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16},
    {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17}, {14, 4, 4, 4, 4, 4, 14},
    {7, 2, 2, 2, 2, 18, 12}, {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},
    {17, 27, 21, 21, 17, 17, 17}, {17, 25, 21, 19, 17, 17, 17}, {14, 17, 17, 17, 17, 17, 14},
    {30, 17, 17, 30, 16, 16, 16}, {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17},
    {15, 16, 16, 14, 1, 1, 30}, {31, 4, 4, 4, 4, 4, 4}, {17, 17, 17, 17, 17, 17, 14},
    {17, 17, 17, 17, 17, 10, 4}, {17, 17, 17, 21, 21, 27, 17}, {17, 17, 10, 4, 10, 17, 17},
    {17, 17, 10, 4, 4, 4, 4}, {31, 1, 2, 4, 8, 16, 31}
};

static inline unsigned int rmxd_glyph(char ch)
{
    if (ch >= '0' && ch <= '9') return 6u + (unsigned int)(ch - '0');
    if (ch >= 'A' && ch <= 'Z') return 16u + (unsigned int)(ch - 'A');
    if (ch >= 'a' && ch <= 'z') return 16u + (unsigned int)(ch - 'a');
    switch (ch) {
    case '.': return 1u;
    case '/': return 2u;
    case '-': return 3u;
    case '+': return 4u;
    case '%': return 5u;
    default: return 0u;
    }
}

/* Writes `text` into the field at (row, col), one glyph per six columns
   (five dots and a gap), at brightness `v`; dots outside the window are
   dropped. Returns the column after the last glyph. */
static inline unsigned int rmxd_text(struct rmxd_field *f, unsigned int row, unsigned int col,
                                     const char *text, unsigned int v)
{
    for (; text && *text; text++, col += 6u) {
        const uint8_t *g = rmxd_font[rmxd_glyph(*text)];
        for (unsigned int r = 0; r < 7u; r++)
            for (unsigned int c = 0; c < 5u; c++)
                if ((g[r] >> (4u - c)) & 1u && row + r < RMXD_ROWS && col + c < RMXD_COLS)
                    f->v[row + r][col + c] = rmxd_clamp8(v);
    }
    return col;
}

static inline uint16_t rmxd_rgb565(unsigned int r, unsigned int g, unsigned int b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* a + (b - a) * k / 255, unsigned only (signed division is not available in the hook). */
static inline unsigned int rmxd_mix(unsigned int a, unsigned int b, unsigned int k)
{
    return b >= a ? a + (b - a) * k / 255u : a - (a - b) * k / 255u;
}

/* The sprites: RMXD_LEVELS tiles of size x size RGB565, level 0 the dimmest.
   `background` is the window's colour, baked into every tile so a draw is a
   plain copy. `tint` is the colour of the brightest dots' halo (0xRRGGBB);
   the dots themselves go from dark grey to white. Needs size <= RMXD_SPRITE_MAX.
   Geometry in sixteenths of a pixel; coverage from squared distance, no sqrt. */
static inline int rmxd_sprites(uint16_t *out, unsigned int size, uint32_t background, uint32_t tint)
{
    if (!out || size < 4u || size > RMXD_SPRITE_MAX) return 0;
    const unsigned int br = (background >> 16) & 255u, bg = (background >> 8) & 255u, bb = background & 255u;
    const unsigned int tr = (tint >> 16) & 255u, tg = (tint >> 8) & 255u, tb = tint & 255u;
    for (unsigned int l = 0; l < RMXD_LEVELS; l++) {
        const unsigned int t = l * 255u / (RMXD_LEVELS - 1u);              /* 0..255 */
        /* Radius: 14 % of the tile when dim, 38 % when bright; the halo reaches the tile's edge. */
        const unsigned int rad = size * 16u * (36u + 61u * t / 255u) / 256u;
        const unsigned int halo = size * 8u;
        const unsigned int soft = 12u;                                      /* 0.75 px each side */
        const unsigned int r_in = rad > soft ? rad - soft : 0u, r_out = rad + soft;
        const unsigned int ri2 = r_in * r_in, ro2 = r_out * r_out, h2 = halo * halo;
        /* Ink: dark grey for the dim dots, white for the bright ones. */
        const unsigned int ink = 52u + 203u * t / 255u;
        for (unsigned int y = 0; y < size; y++)
            for (unsigned int x = 0; x < size; x++) {
                const int dx = ((int)(2u * x + 1u) - (int)size) * 8, dy = ((int)(2u * y + 1u) - (int)size) * 8;
                const unsigned int d2 = (unsigned int)(dx * dx + dy * dy);
                unsigned int cover = 0u, glow = 0u;                         /* 0..255 */
                if (d2 <= ri2) cover = 255u;
                else if (d2 < ro2) cover = 255u * (ro2 - d2) / (ro2 - ri2);
                if (t > 120u && d2 > ri2 && h2 > ro2 && d2 < h2) {
                    unsigned int k = 255u * (h2 - d2) / (h2 - ri2);
                    glow = k * k / 255u * (t - 120u) / 135u * 70u / 255u;   /* faint, only when bright */
                }
                unsigned int rr = rmxd_mix(br, tr, glow), gg = rmxd_mix(bg, tg, glow), bl = rmxd_mix(bb, tb, glow);
                rr = rmxd_mix(rr, ink, cover); gg = rmxd_mix(gg, ink, cover); bl = rmxd_mix(bl, ink, cover);
                out[(l * size + y) * size + x] = rmxd_rgb565(rr, gg, bl);
            }
    }
    return 1;
}

/* The sprite a brightness picks. */
static inline unsigned int rmxd_level(unsigned int v)
{
    return (v * (RMXD_LEVELS - 1u) + 127u) / 255u;
}

static inline void rmxd_shown_reset(struct rmxd_shown *s)
{
    for (unsigned int r = 0; r < RMXD_ROWS; r++)
        for (unsigned int c = 0; c < RMXD_COLS; c++) s->level[r][c] = 255u;
}

/* Copies the sprite of each cell that changed since `shown` into a frame of
   RGB565 pixels (`stride` pixels per line), the window's top-left at `fb`,
   cells `pitch` pixels apart with the sprite centred in its cell, and
   updates `shown`. Returns how many cells were drawn. */
static inline unsigned int rmxd_draw(uint16_t *fb, unsigned int stride, unsigned int pitch,
                                     const uint16_t *sprites, unsigned int size,
                                     const struct rmxd_field *now, struct rmxd_shown *shown)
{
    unsigned int drawn = 0u;
    const unsigned int inset = pitch > size ? (pitch - size) / 2u : 0u;
    for (unsigned int r = 0; r < RMXD_ROWS; r++)
        for (unsigned int c = 0; c < RMXD_COLS; c++) {
            unsigned int level = rmxd_level(now->v[r][c]);
            if (shown->level[r][c] == level) continue;
            shown->level[r][c] = (uint8_t)level;
            const uint16_t *tile = sprites + level * size * size;
            uint16_t *dst = fb + (r * pitch + inset) * stride + c * pitch + inset;
            for (unsigned int y = 0; y < size; y++)
                for (unsigned int x = 0; x < size; x++) dst[y * stride + x] = tile[y * size + x];
            drawn++;
        }
    return drawn;
}

#endif

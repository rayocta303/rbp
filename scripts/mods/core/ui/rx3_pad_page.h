/* SPDX-License-Identifier: MPL-2.0
 *
 * The sampler page: the player's own RELEASE FX pictures, painted over.
 *
 * RELEASE FX draws eight pictures, one per pad, and a title, into each deck's
 * pad window (docs/analisi/01-05-06 2.1). The sampler plays on that page, so
 * what the pictures say (BEAT BRAKE, VINYL BRAKE...) is wrong. Nothing here
 * decides when the page is up: the player draws its pictures only while it is,
 * so a draw of one of them is the page being shown, and outside it there is
 * nothing to intercept and the player's own screen is untouched.
 *
 * Each picture is replaced where the player placed it, by a cell the module
 * titles. The player's own box says where; the arithmetic in rx3_pad_layout.h
 * says which pad it is, from that box. An unexpected box is refused and the
 * picture stays the player's, with a line in the log saying what it was.
 */

#ifndef RX3_PAD_PAGE_H
#define RX3_PAD_PAGE_H

#define PAD_PAGE_DECKS 2u
#define PAD_PAGE_LOG_LIMIT 24u
#define PAD_PAGE_REPAINT_EVERY_US 40000u

/* The smallest y of a cell seen so far, per deck: the top row. */
static unsigned int pad_page_row0_y[PAD_PAGE_DECKS] = {0xffffu, 0xffffu};
static unsigned int pad_page_logged;
static unsigned int pad_page_refused;
static unsigned int pad_page_painted[PAD_PAGE_DECKS];
/* Whether the player's last picture of this kind in a deck's pad window was
   the page's own, that is whether the page is what the deck is showing. A
   repaint is asked of the player only while it is: on the console (2026-10-24)
   a repaint asked at start-up, with HOT CUE showing, left both pad windows
   cleared to the transparent chequerboard until something else redrew them. */
static unsigned int pad_page_visible[PAD_PAGE_DECKS];
/* The player's picture of each cell as it last drew it, kept to paint the cell
   again by itself: the player draws these pictures once, when the page comes
   up, and asking it to redraw (refresh_performance_ui) cleared both pad windows
   to the transparent chequerboard on the console, at every pad press. */
struct pad_page_slot {
    uint8_t model[0x54];
    unsigned int valid;
    /* Where the player placed this picture's origin while drawing it: a fill
       made later, from inside another draw, is placed from that draw's own. */
    int origin_x, origin_y;
};
static struct pad_page_slot pad_page_slots[PAD_PAGE_DECKS][RX3_PAD_PAGE_CELLS];
static uint64_t pad_page_repaint_next_us;
static unsigned short pad_page_advance[128];
static int pad_page_advance_ready;

/* How far the typeface moves the pen for each ASCII code, folded as the
   atlas folds it. Built once the atlas is up. */
static void pad_page_build_advance(void)
{
    if (pad_page_advance_ready || !pad_atlas_ready)
        return;
    for (unsigned int c = 0; c < 128u; c++) {
        int slot = pad_atlas_slot(c);
        pad_page_advance[c] = c == ' ' ? (unsigned short)pad_atlas.space_advance
            : slot < 0 ? (unsigned short)RX3_PAD_PAGE_NO_GLYPH
                       : (unsigned short)pad_atlas_glyphs[slot].advance;
    }
    pad_page_advance_ready = 1;
}

/* A caption fitted to `room` px, as the 16-bit text the atlas draws. */
static const uint16_t *pad_page_text(const char *caption, int room, int *width)
{
    static uint16_t text[RX3_PAD_PAGE_LABEL_MAX + 1u];
    unsigned short fitted[RX3_PAD_PAGE_LABEL_MAX + 1u];
    unsigned int i;

    *width = 0;
    text[0] = 0;
    if (!caption || !pad_page_advance_ready)
        return text;
    *width = rx3_pad_page_fit((const unsigned char *)caption, pad_page_advance,
                              (int)pad_atlas.space_advance, room, fitted);
    for (i = 0; fitted[i]; i++)
        text[i] = fitted[i];
    text[i] = 0;
    return text;
}

/* An idle cell: the pad's colour, a third of the way up from the face the
   other controls sit on, so it reads as that pad without shouting. */
static uint32_t pad_page_dim(uint32_t accent, uint32_t ground)
{
    uint32_t out = 0;
    for (unsigned int shift = 0; shift < 24u; shift += 8u) {
        uint32_t a = (accent >> shift) & 255u, g = (ground >> shift) & 255u;
        out |= ((a * 3u + g * 5u) >> 3) << shift;
    }
    return out;
}

static void pad_page_log_picture(unsigned int image_id, uint16_t layer, const uint16_t *box)
{
    if (pad_page_logged >= PAD_PAGE_LOG_LIMIT)
        return;
    pad_page_logged++;
    log_number("sampler page: picture id = ", image_id);
    log_number("  layer = ", layer);
    log_number("  x1 * 1000 + y1 = ", (unsigned long)box[0] * 1000u + box[1]);
    log_number("  x2 * 1000 + y2 = ", (unsigned long)box[2] * 1000u + box[3]);
}

/* One cell, painted in the box the player gave its picture. */
static void pad_page_cell(void *render, const void *image, uint8_t window,
                          const struct rx3_pad_page *page, unsigned int deck,
                          unsigned int cell, int x1, int y1, int x2, int y2)
{
    unsigned int state = page->cell_state(deck, cell);
    uint32_t ground = pad_atlas_ground_colour(RX3_PAD_INK_PRESSED);
    uint32_t accent = pad_colour_rgb888(page->colour(deck, cell));
    uint32_t face = ground, edge = pad_frame_colour();
    unsigned int ink = RX3_PAD_INK_PRESSED;
    int border = 1, width = x2 - x1 + 1, text_width;
    const uint16_t *text;
    struct rx3_pad_cell span = {x1, x2};

    if (state) {
        border = 2;
        edge = accent;
        face = state > 1u ? accent : pad_page_dim(accent, ground);
        ink = pad_face_ink(face);
    }
    pad_fill(render, image, window, 0u, x1, y1, x2, y2, edge);
    pad_fill(render, image, window, 0u, x1 + border, y1 + border,
             x2 - border, y2 - border, face);
    if (!state)
        return;
    text = pad_page_text(page->caption(deck, cell),
                         width - 2 * RX3_PAD_PAGE_INSET, &text_width);
    pad_draw_caption_at(render, image, window, 0u, span, text, ink,
                        y1 + ((y2 - y1 + 1 - (int)pad_atlas.cell_height) >> 1));
}

/* The title picture: covered in the row's ground, then titled. */
static void pad_page_title(void *render, const void *image, uint8_t window,
                           const struct rx3_pad_page *page, unsigned int deck,
                           int x1, int y1, int x2, int y2)
{
    int text_width;
    struct rx3_pad_cell span = {x1, x2};
    const uint16_t *text;

    pad_fill(render, image, window, 0u, x1, y1, x2, y2,
             pad_atlas_ground_colour(RX3_PAD_INK_INACTIVE));
    if (y2 - y1 + 1 < (int)pad_atlas.cell_height)
        return;
    text = pad_page_text(page->title ? page->title(deck) : 0, x2 - x1 + 1 - 2 * RX3_PAD_PAGE_INSET, &text_width);
    pad_draw_caption_at(render, image, window, 0u, span, text, RX3_PAD_INK_INACTIVE,
                        y1 + ((y2 - y1 + 1 - (int)pad_atlas.cell_height) >> 1));
}

/* Called for a picture of the page, drawn in a deck's pad window. Returns
   nonzero when it has been painted over, zero to let the player draw it. */
static int pad_page_draw(void *render, const void *image,
                         const struct rx3_pad_page *page, unsigned int deck,
                         unsigned int image_id)
{
    uint16_t layer, box[4];
    int x1, y1, x2, y2;
    uint8_t window;

    memcpy(&layer, (const uint8_t *)image + 0x10u, sizeof(layer));
    memcpy(box, (const uint8_t *)image + 0x18u, sizeof(box));
    pad_page_log_picture(image_id, layer, box);
    if (deck >= PAD_PAGE_DECKS)
        return 0;
    /* Letters are artwork and the faces are fills: without either there is
       nothing to put in the picture's place. */
    pad_page_build_advance();
    if (!pad_atlas_ready || !pad_button_face())
        return 0;
    window = (uint8_t)(layer & 0xffu);
    x1 = box[0]; y1 = box[1]; x2 = box[2]; y2 = box[3];

    if (!pad_page_visible[deck]) {
        pad_page_visible[deck] = 1u;
        log_number("sampler page shown, deck = ", deck + 1u);
    }
    rx3_image_suspend_fill(1);
    /* The player's box ends one past its last pixel; fills and captions are
       given the last pixel itself. */
    if (page->title_image && image_id == page->title_image) {
        pad_page_title(render, image, window, page, deck, x1, y1, x2 - 1, y2 - 1);
    } else if (rx3_pad_page_is_cell(x1, y1, x2, y2)) {
        if ((unsigned int)y1 < pad_page_row0_y[deck]) {
            /* The top row is the smallest y seen. A picture of the bottom row
               that comes first is titled as the top one until the real top
               row shows up, and then the page is painted again. */
            if (pad_page_row0_y[deck] != 0xffffu)
                rx3_panels.invalidate_page();
            pad_page_row0_y[deck] = (unsigned int)y1;
            memset(pad_page_slots[deck], 0, sizeof(pad_page_slots[deck]));
        }
        unsigned int cell = (unsigned int)rx3_pad_page_cell(x1, y1, (int)pad_page_row0_y[deck]);
        memcpy(pad_page_slots[deck][cell].model, image, sizeof(pad_page_slots[deck][cell].model));
        pad_page_slots[deck][cell].valid = 1u;
        pad_page_cell(render, image, window, page, deck, cell, x1, y1, x2 - 1, y2 - 1);
        pad_page_slots[deck][cell].origin_x = pad_fill_seen_x;
        pad_page_slots[deck][cell].origin_y = pad_fill_seen_y;
        pad_page_painted[deck]++;
    } else {
        rx3_image_suspend_fill(0);
        /* Not a cell: the player's picture stays. Said once per kind. */
        if (pad_page_refused < 4u) {
            pad_page_refused++;
            log_line("sampler page: a picture is not a pad cell, left native");
        }
        return 0;
    }
    rx3_image_suspend_fill(0);
    return 1;
}

/* Any other picture the size of a pad cell drawn into a deck's pad window is
   another page (HOT CUE, BEAT JUMP...) taking the cells back. */
static void pad_page_other_picture(const void *image, unsigned int deck)
{
    uint16_t box[4];

    if (deck >= PAD_PAGE_DECKS || !pad_page_visible[deck])
        return;
    memcpy(box, (const uint8_t *)image + 0x18u, sizeof(box));
    if (rx3_pad_page_is_cell(box[0], box[1], box[2], box[3])) {
        pad_page_visible[deck] = 0u;
        log_number("sampler page left, deck = ", deck + 1u);
    }
}

/* Paint a deck's cells again, each in the box of the picture it replaces. */
static void pad_page_repaint(void *render, const struct rx3_pad_page *page, unsigned int deck)
{
    int now_x, now_y;

    pad_native_origin(&now_x, &now_y);
    rx3_image_suspend_fill(1);
    for (unsigned int cell = 0; cell < RX3_PAD_PAGE_CELLS; cell++) {
        const uint8_t *model = pad_page_slots[deck][cell].model;
        uint16_t layer, box[4];
        if (!pad_page_slots[deck][cell].valid)
            continue;
        memcpy(&layer, model + 0x10u, sizeof(layer));
        memcpy(box, model + 0x18u, sizeof(box));
        pad_fill_origin_x = pad_page_slots[deck][cell].origin_x;
        pad_fill_origin_y = pad_page_slots[deck][cell].origin_y;
        pad_fill_origin_set = 1;
        pad_image_shift_x = pad_page_slots[deck][cell].origin_x - now_x;
        pad_image_shift_y = pad_page_slots[deck][cell].origin_y - now_y;
        pad_page_cell(render, model, (uint8_t)(layer & 0xffu), page, deck, cell,
                      box[0], box[1], box[2] - 1, box[3] - 1);
        pad_fill_origin_set = 0;
        pad_image_shift_x = pad_image_shift_y = 0;
    }
    rx3_image_suspend_fill(0);
}

/* The sampler page asks for its own repaint (a pad started, a sound ended).
   The cells are painted again here, from inside a draw of the player's, with
   that draw's renderer, and only for a deck that is showing the page: the
   player is not asked to redraw anything. With a custom panel open the row
   repaints itself. Not more often than the screen can use. */
static void pad_page_run_pending(void *render)
{
    const struct rx3_pad_page *page = rx3_panel_page();

    if (!rx3_panel_take_page_repaint() || overlay_panel || !page)
        return;
    if (!pad_page_visible[0] && !pad_page_visible[1])
        return;
    uint64_t now = monotonic_enough_us();
    if (now < pad_page_repaint_next_us) {
        rx3_panels.invalidate_page();
        return;
    }
    pad_page_repaint_next_us = now + PAD_PAGE_REPAINT_EVERY_US;
    for (unsigned int deck = 0; deck < PAD_PAGE_DECKS; deck++)
        if (pad_page_visible[deck] && page->active(deck) && pad_button_face() && pad_atlas_ready)
            pad_page_repaint(render, page, deck);
}

/* The first picture of a deck painted is worth a line: it is what says the
   page was found at all. */
static void pad_page_report(unsigned int deck)
{
    static unsigned int told;
    if (deck < PAD_PAGE_DECKS && !(told & (1u << deck)) && pad_page_painted[deck]) {
        told |= 1u << deck;
        log_number("sampler page painted over the player's pictures, deck = ", deck + 1u);
    }
}

#endif /* RX3_PAD_PAGE_H */

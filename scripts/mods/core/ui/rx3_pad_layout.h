/* SPDX-License-Identifier: MPL-2.0
 *
 * Where the controls of the performance pad row sit, and what a touch at a
 * given x lands on. One solver answers both questions, because the row used to
 * answer them twice: every panel laid its controls out in its draw and worked
 * out what was hit by repeating the same arithmetic in its touch handler, with
 * nothing keeping the two copies in step.
 *
 * The block between the markers is integer arithmetic over int and nothing
 * else -- no player types, no globals, no libc -- so the test compiles it for
 * the computer and runs it against the preview's copy rather than reading it
 * as text. Keep it that way: a memcpy or a log line in here costs the only
 * verification this row has before it reaches a deck.
 */

#ifndef RX3_PAD_LAYOUT_H
#define RX3_PAD_LAYOUT_H

/* The row, in the pad window's own coordinates. The ground is drawn wider than
   the controls at each end; the controls are what a finger can reach. */
#define RX3_PAD_GROUND_TOP     17
#define RX3_PAD_GROUND_BOTTOM  63
#define RX3_PAD_CTRL_TOP       21
#define RX3_PAD_CTRL_BOTTOM    59
#define RX3_PAD_GROUND_BLEED    4

/* P3: the row sits on the native HOT CUE cells under it, four of 148 px with
   a 9 px gap from x 11 in each deck's window (docs/analisi/02-09, 2.1), so
   its outer edges are theirs and a four-control row is exactly their cells.
   Measured from a capture, not from the binary: DA_FIXARE/49. */
#define RX3_PAD_HOTCUE_X       11
#define RX3_PAD_HOTCUE_CELL   148
#define RX3_PAD_HOTCUE_GAP      9
#define RX3_PAD_HOTCUE_COUNT    4

/* One deck's half, and the whole screen for a control that spans both: from
   the first hot cue cell of deck 1 to the last of deck 2. */
#define RX3_PAD_DECK_STRIDE   640
#define RX3_PAD_DECK_ORIGIN   RX3_PAD_HOTCUE_X
#define RX3_PAD_DECK_SPAN     (RX3_PAD_HOTCUE_COUNT * RX3_PAD_HOTCUE_CELL + \
                               (RX3_PAD_HOTCUE_COUNT - 1) * RX3_PAD_HOTCUE_GAP)
#define RX3_PAD_SCREEN_ORIGIN RX3_PAD_DECK_ORIGIN
#define RX3_PAD_SCREEN_SPAN   (RX3_PAD_DECK_STRIDE + RX3_PAD_DECK_SPAN)

/* A drawn control at local y 21 is touched at screen y 521, and both are 39
   tall. The touch band is derived from the drawn control through this one
   number rather than written down a second time, because the two literals
   disagreeing by a few pixels is a dead strip along one edge of every control
   that nothing would ever report. */
#define RX3_PAD_LOCAL_TO_SCREEN_Y 500
#define RX3_PAD_TOUCH_TOP    (RX3_PAD_CTRL_TOP    + RX3_PAD_LOCAL_TO_SCREEN_Y)
#define RX3_PAD_TOUCH_BOTTOM (RX3_PAD_CTRL_BOTTOM + RX3_PAD_LOCAL_TO_SCREEN_Y)

/* RX3 PAD LAYOUT BEGIN */
#define RX3_PAD_CELL_MAX 8

/* A stepper's arrow ends: a quarter of the cell, held between these so a narrow
   stepper keeps a readable value and a wide one does not grow two huge arrows. */
#define RX3_PAD_STEP_END_MIN   40
#define RX3_PAD_STEP_END_MAX   96
#define RX3_PAD_STEP_INNER_GAP  6
#define RX3_PAD_STEP_VALUE_MIN 16

struct rx3_pad_cell { int x1, x2; };

/* The gap between controls, by how many there are.
 *
 * Four is the hot cue gap (P3), so four equal controls land on the four
 * native cells exactly: 148 wide, 9 apart. Two, three and five keep the stems
 * strip's own gaps, which it measured on the row; over the hot cue span two
 * divide exactly (300) and the others spread their remainder to the last
 * cells. Past five the shape simply continues.
 */
static const int rx3_pad_gap[RX3_PAD_CELL_MAX + 1] = {
    0, 0, 19, 17, 9, 11, 11, 9, 9
};

/* Lay count cells across span, in proportion to their weights.
 *
 * Every division here is unsigned on purpose. Signed division on this target is
 * a call to __aeabi_idiv, which rbp does not export and tests/test_hook_symbols
 * would refuse; __aeabi_uidiv is the one that is available.
 */
static int rx3_pad_solve(const unsigned char *weights, int count,
                         int origin, int span, struct rx3_pad_cell *out)
{
    int i, x, shares = 0, free_width, gap;
    unsigned int unit, spare;

    if (count <= 0 || count > RX3_PAD_CELL_MAX || !weights || !out)
        return 0;
    gap = rx3_pad_gap[count];
    free_width = span - gap * (count - 1);
    if (free_width < count)
        return 0;
    for (i = 0; i < count; i++)
        shares += weights[i] ? (int)weights[i] : 1;
    unit = (unsigned int)free_width / (unsigned int)shares;
    spare = (unsigned int)free_width - unit * (unsigned int)shares;

    int remaining = shares;
    x = origin;
    for (i = 0; i < count; i++) {
        int weight = weights[i] ? (int)weights[i] : 1;
        int width = (int)unit * weight;
        /* The pixels that did not divide go to the last cells, so the row ends
           on origin + span - 1 whatever the count and nothing shifts under a
           finger already resting on the control at the end. */
        remaining -= weight;
        int extra = (int)spare - remaining;
        if (extra > weight) extra = weight;
        if (extra > 0) width += extra;
        out[i].x1 = x;
        out[i].x2 = x + width - 1;
        x += width + gap;
    }
    return count;
}

/* A stepper is one declared widget and three touchable parts: decrement, the
   value, increment. */
static void rx3_pad_stepper_parts(struct rx3_pad_cell cell,
                                  struct rx3_pad_cell *parts)
{
    int width = cell.x2 - cell.x1 + 1;
    int end = width >> 2;

    if (end < RX3_PAD_STEP_END_MIN) end = RX3_PAD_STEP_END_MIN;
    if (end > RX3_PAD_STEP_END_MAX) end = RX3_PAD_STEP_END_MAX;
    if (end * 2 + RX3_PAD_STEP_INNER_GAP * 2 + RX3_PAD_STEP_VALUE_MIN > width) {
        end = (width - RX3_PAD_STEP_INNER_GAP * 2 - RX3_PAD_STEP_VALUE_MIN) >> 1;
        if (end < 8) end = 8;
    }
    parts[0].x1 = cell.x1;
    parts[0].x2 = cell.x1 + end - 1;
    parts[2].x2 = cell.x2;
    parts[2].x1 = cell.x2 - end + 1;
    parts[1].x1 = parts[0].x2 + 1 + RX3_PAD_STEP_INNER_GAP;
    parts[1].x2 = parts[2].x1 - 1 - RX3_PAD_STEP_INNER_GAP;
}

/* Which cell an x lands on, or -1 for the gap between two. The gaps are not
   dead by accident: a finger there is handed back to the player, which is what
   parking the native touch areas exists to make safe. */
static int rx3_pad_hit(const struct rx3_pad_cell *cells, int count, int x)
{
    int i;
    for (i = 0; i < count; i++)
        if (x >= cells[i].x1 && x <= cells[i].x2)
            return i;
    return -1;
}

/* Where a slider sits when the finger is at x, rounded to nearest.
 *
 * The released control's own mapping was ((x * 100) - 1536) / 1128, fitted to
 * one track at one width. This is the same shape stated in terms of the cell,
 * so a slider of any width lands on 0 at its left edge and max at its right.
 */
static int rx3_pad_slider_value(struct rx3_pad_cell cell, int x, int max)
{
    int width = cell.x2 - cell.x1 + 1;
    unsigned int travel, offset;

    if (max <= 0 || width <= 1)
        return 0;
    if (x <= cell.x1) return 0;
    if (x >= cell.x2) return max;
    travel = (unsigned int)(width - 1);
    offset = (unsigned int)(x - cell.x1);
    return (int)((offset * (unsigned int)max + travel / 2u) / travel);
}

/* The inverse, for drawing the filled part of a track. */
static int rx3_pad_slider_x(struct rx3_pad_cell cell, int value, int max)
{
    int width = cell.x2 - cell.x1 + 1;
    unsigned int travel;

    if (max <= 0 || width <= 1)
        return cell.x1;
    if (value <= 0) return cell.x1;
    if (value >= max) return cell.x2;
    travel = (unsigned int)(width - 1);
    return cell.x1 + (int)(((unsigned int)value * travel +
                            (unsigned int)max / 2u) / (unsigned int)max);
}
/* The sampler page. RELEASE FX shows eight cells of the same 148 x 20 as the
   hot cue cells, four to a row, and the sampler paints a title in each one
   (DA_FIXARE/17). Each cell is painted where the player's own image for it
   would have gone, so the position comes from the player and only the cell
   *number* is worked out here, from where that image sits: the column from x
   (the cells are one pitch apart), the row from y against the top row's y,
   which is learned from the first row-0 image drawn (the rows are at least
   20 px apart). A box that is not a cell at all is refused, so a page whose
   pictures turn out to be something else stays native. */
#define RX3_PAD_PAGE_CELLS      8
#define RX3_PAD_PAGE_COLUMNS    4
#define RX3_PAD_PAGE_ORIGIN    11
#define RX3_PAD_PAGE_PITCH    157
#define RX3_PAD_PAGE_WIDTH_MIN 120
#define RX3_PAD_PAGE_WIDTH_MAX 160
#define RX3_PAD_PAGE_HEIGHT_MIN 16
#define RX3_PAD_PAGE_HEIGHT_MAX 24
#define RX3_PAD_PAGE_ROW_SPLIT  10
/* 148 - 2 * 6 = 136 px of text, the room the desktop app measures a label against. */
#define RX3_PAD_PAGE_INSET       6
#define RX3_PAD_PAGE_LABEL_MAX  16
#define RX3_PAD_PAGE_NO_GLYPH   0xffffu

/* Is this box one of the page's cells? The player's boxes end one past their
   last pixel (a cell is 11,23 to 159,43 for 148 x 20), which is how they are
   taken here. */
static int rx3_pad_page_is_cell(int x1, int y1, int x2, int y2)
{
    int width = x2 - x1, height = y2 - y1;
    return x1 >= 0 && y1 >= 0 &&
           width >= RX3_PAD_PAGE_WIDTH_MIN && width <= RX3_PAD_PAGE_WIDTH_MAX &&
           height >= RX3_PAD_PAGE_HEIGHT_MIN && height <= RX3_PAD_PAGE_HEIGHT_MAX;
}

/* Which cell, 0 to 7, left to right along the top row and then the bottom
   one. The caller keeps `row0_y` as the smallest y it has seen. */
static int rx3_pad_page_cell(int x1, int y1, int row0_y)
{
    int column = 0, row = y1 - row0_y >= RX3_PAD_PAGE_ROW_SPLIT;
    int shifted = x1 - RX3_PAD_PAGE_ORIGIN + (RX3_PAD_PAGE_PITCH >> 1);

    if (shifted > 0)
        column = (int)((unsigned int)shifted / (unsigned int)RX3_PAD_PAGE_PITCH);
    if (column >= RX3_PAD_PAGE_COLUMNS)
        column = RX3_PAD_PAGE_COLUMNS - 1;
    return row * RX3_PAD_PAGE_COLUMNS + column;
}

/* A pad's text, made fit for its cell. `advance` holds, for each ASCII code,
   how far the typeface moves the pen, or RX3_PAD_PAGE_NO_GLYPH where it has
   no such character; those, and every control character, become a space. Lower
   case is folded as the typeface does. The text is cut at the last character
   that fits in `room` px, never drawn wider, and loses its leading and trailing
   spaces so it centres on its ink. `out` takes up to RX3_PAD_PAGE_LABEL_MAX
   characters and a zero. Returns the width in px. */
static int rx3_pad_page_fit(const unsigned char *text, const unsigned short *advance,
                            int space, int room, unsigned short *out)
{
    int count = 0, width = 0, ink_width = 0, ink_count = 0, i;

    if (!text || !advance || !out)
        return 0;
    for (i = 0; text[i] && count < RX3_PAD_PAGE_LABEL_MAX; i++) {
        unsigned int c = text[i];
        int step;
        if (c >= 'a' && c <= 'z')
            c -= 32u;
        if (c <= ' ' || c >= 127u || advance[c] == RX3_PAD_PAGE_NO_GLYPH)
            c = ' ';
        step = c == ' ' ? space : (int)advance[c];
        if (width + step > room)
            break;
        if (c == ' ' && !ink_count) {
            continue;                   /* no leading space */
        }
        out[count++] = (unsigned short)c;
        width += step;
        if (c != ' ') {
            ink_width = width;
            ink_count = count;
        }
    }
    out[ink_count] = 0;
    return ink_width;
}
/* RX3 PAD LAYOUT END */

#endif /* RX3_PAD_LAYOUT_H */

/*
 * settings_paint.c -- the SETTINGS panel's image: a title, a rule and five rows, each
 * with a label at its left end, a value at its right end, and a mark that says what a tap
 * does.
 *
 * READ settings_zone.h FIRST. Nothing in this file decides any of the behaviour: which
 * rows there are, what they read, what a tap asks for and whether a row can be operated
 * at all are all that module's, and this file draws whatever state it is handed. What is
 * here is the picture, in the palette side_paint.c and menu_paint.c use.
 *
 * NO FRAME ROUND THE BOX, the same restyle the band and the chooser had on 2026-10-06:
 * a black bed, and the white goes round each ROW rather than round the whole panel
 * its header). SB_BORDER stays as the box's margin -- it is what keeps the
 * title and the rows off the bed's edge, and it is what keeps the hit test and the ink
 * the same pixels.
 *
 * THERE ARE NO MARKS IN THIS PICTURE -- not one glyph that is not a letter, a digit or one
 * of the eight punctuation marks the atlas carries. The first version drew a cycle arrow on
 * the cycling rows and a chevron on the one that opens the USB STOP chooser, drawn as
 * geometry because the atlas has neither; the operator asked whether the panel needed them
 * at all, and the answer was no (settings_paint.h records the reasoning and what would come
 * back if the drill-down ever wants marking). What is left is two columns of text per row
 * and the row's own frame.
 *
 * See settings_paint.h for the contract. The ~10 duplicated lines below (the pixel
 * accessor, the fill, the frame, the glyph walk and the blend) are duplicated in
 * side_paint.c and menu_paint.c for the reason menu_paint.h records:
 * exporting them would mean an accessor that has to know four module-internal notions to
 * save ten lines. What is NOT duplicated is the palette -- menu_pixel() is public, so the
 * panel, the band, the chooser and the drawers are the same colours by construction.
 *
 * EVERY PIXEL IS WRITTEN ON EVERY CALL. The panel's plane is single-buffered, so the
 * whole picture is filled first and every mark goes on top; nothing here reads the
 * buffer, so a second paint of the same state leaves the same image.
 */
#include "settings_paint.h"
#include "menu_font.h"

#include <stddef.h>          /* size_t, NULL */
#include <stdio.h>
#include "net_info.h"

/* --- pixels, in the view's own (framebuffer) coordinates -------------------- */

static void sp_set(const struct menu_view *v, int fx, int fy, unsigned int pv)
{
    unsigned char *row;

    if (fx < 0 || fy < 0 || fx >= v->dw || fy >= v->dh)
        return;
    row = (unsigned char *)v->pix + (size_t)fy * v->pitch * (v->bpp / 8);
    if (v->bpp == 32)
        *(unsigned int *)(row + (size_t)fx * 4) = pv;
    else
        *(unsigned short *)(row + (size_t)fx * 2) = (unsigned short)pv;
}

static void sp_fill(const struct menu_view *v, int x0, int y0, int x1, int y1,
                    unsigned int pv)
{
    int x, y;

    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++)
            sp_set(v, x, y, pv);
}

/* A one-pixel outline, drawn inside the rect so a row's mark and its hit target are the
 * same pixels -- settings_zone.c's hit tests use the inclusive rect, not the ink. */
static void sp_frame(const struct menu_view *v, int x0, int y0, int x1, int y1,
                     unsigned int pv)
{
    sp_fill(v, x0, y0, x1, y0 + 1, pv);
    sp_fill(v, x0, y1 - 1, x1, y1, pv);
    sp_fill(v, x0, y0, x0 + 1, y1, pv);
    sp_fill(v, x1 - 1, y0, x1, y1, pv);
}

/* --- logical (box-local) to framebuffer ------------------------------------
 *
 * The view's picture rect IS the box (settings_paint.h), so the two scales are the box's
 * own: SB_W logical px across `dw` framebuffer px, SB_H down `dh`. PR's own arithmetic
 * with this box's constants, and the origin subtracted first is SB_X0/SB_Y0, so a caller
 * that moves the panel on the page moves `bx`/`by` and nothing else. */
static int sp_fx(const struct menu_view *v, int lx)
{
    return v->bx + (lx - SB_X0) * v->dw / SB_W;
}

static int sp_fy(const struct menu_view *v, int ly)
{
    return v->by + (ly - SB_Y0) * v->dh / SB_H;
}

/* A half-open rect in box-local coordinates: (lx0,ly0) included, (lx1,ly1) NOT. The
 * inclusive rect settings_zone.c's hit tests use is one pixel wider by construction, and
 * every caller below adds the 1 back at the call site. */
static void sp_rect(const struct menu_view *v, int lx0, int ly0, int lx1, int ly1,
                    int *fx0, int *fy0, int *fx1, int *fy1)
{
    *fx0 = sp_fx(v, lx0);
    *fy0 = sp_fy(v, ly0);
    *fx1 = sp_fx(v, lx1);
    *fy1 = sp_fy(v, ly1);
    if (*fx1 <= *fx0) *fx1 = *fx0 + 1;
    if (*fy1 <= *fy0) *fy1 = *fy0 + 1;
}

/* --- ink --------------------------------------------------------------------- */

/* `ink` over `base` at coverage `cov`/255. Coverage 0 and 255 return one argument
 * unchanged, so the ends of the range are palette values and only the glyph edges blend --
 * the rule every painter in this shim keeps, and the reason a second paint of the same
 * image writes the same bytes. */
static unsigned int sp_blend(int bpp, unsigned int base, unsigned int ink, int cov)
{
    int br, bg, bb, ir, ig, ib;

    if (cov <= 0)
        return base;
    if (cov >= 255)
        return ink;
    if (bpp == 32) {
        br = (int)((base >> 16) & 0xffu); bg = (int)((base >> 8) & 0xffu);
        bb = (int)(base & 0xffu);
        ir = (int)((ink >> 16) & 0xffu);  ig = (int)((ink >> 8) & 0xffu);
        ib = (int)(ink & 0xffu);
        return (unsigned int)(((br * (255 - cov) + ir * cov + 127) / 255) << 16 |
                              ((bg * (255 - cov) + ig * cov + 127) / 255) << 8 |
                              ((bb * (255 - cov) + ib * cov + 127) / 255));
    }
    br = (int)((base >> 11) & 0x1fu) * 255 / 31;
    bg = (int)((base >> 5) & 0x3fu) * 255 / 63;
    bb = (int)(base & 0x1fu) * 255 / 31;
    ir = (int)((ink >> 11) & 0x1fu) * 255 / 31;
    ig = (int)((ink >> 5) & 0x3fu) * 255 / 63;
    ib = (int)(ink & 0x1fu) * 255 / 31;
    return (unsigned int)(((((br * (255 - cov) + ir * cov + 127) / 255) & 0xf8) << 8) |
                          ((((bg * (255 - cov) + ig * cov + 127) / 255) & 0xfc) << 3) |
                          ((((bb * (255 - cov) + ib * cov + 127) / 255) & 0xf8) >> 3));
}

/* One glyph, pen at `pen_fx`, the top of the LINE BOX at `line_fy`, clipped to
 * [cx0, cx1) so text can never run out of the row it is drawn into. */
static void sp_glyph(const struct menu_view *v, int pen_fx, int line_fy,
                     unsigned char c, unsigned int base, unsigned int ink,
                     int cx0, int cx1)
{
    const struct menu_glyph *g = menu_font_glyph(c);
    int col, row;

    for (row = 0; row < (int)g->h; row++)
        for (col = 0; col < (int)g->w; col++) {
            int fx = pen_fx + g->left + col;
            int cov = menu_font_cov(g, col, row);

            if (cov == 0 || fx < cx0 || fx >= cx1)
                continue;
            sp_set(v, fx, line_fy + g->top + row,
                   cov >= 255 ? ink : sp_blend(v->bpp, base, ink, cov));
        }
}

static void sp_text(const struct menu_view *v, int pen_fx, int line_fy,
                    const char *s, unsigned int base, unsigned int ink,
                    int cx0, int cx1)
{
    if (!s)
        return;
    for (; *s; s++) {
        sp_glyph(v, pen_fx, line_fy, (unsigned char)*s, base, ink, cx0, cx1);
        pen_fx += menu_font_adv((unsigned char)*s, (unsigned char)s[1]);
    }
}

/* --- the parts of a row ------------------------------------------------------ */

/* One row: face, frame, label, value and mark. `live` is settings_row_live(), and a row
 * that cannot be operated wears the palette's two OFF classes -- menu_paint.h's own idiom
 * for a dead button, and the whole of what makes the mixer row's refusal VISIBLE rather
 * than a tap that silently does nothing (settings_zone.h's "no recognised mixer" rule).
 * `pressed` beats `live`, deliberately: a finger on a dim value still has to see that it
 * landed somewhere. */
static void sp_row(const struct menu_view *v, const struct settings_state *S, int row,
                   int live, int pressed)
{
    unsigned int face, ink;
    int fx0, fy0, fx1, fy1, lx0, lx1, vx0, vx1, line_fy;
    const char *label = settings_row_label(row);

    if (pressed) {
        face = menu_pixel(v->bpp, MENU_BTN_PRESSED);
        ink  = menu_pixel(v->bpp, MENU_LABEL_PRESSED);
    } else if (!live) {
        face = menu_pixel(v->bpp, MENU_BTN_OFF);
        ink  = menu_pixel(v->bpp, MENU_LABEL_OFF);
    } else {
        face = menu_pixel(v->bpp, MENU_BTN);
        ink  = menu_pixel(v->bpp, MENU_LABEL);
    }

    /* The row's own inclusive rect, drawn as the half-open rect the fill loops want. */
    {
        int ry0 = SB_ROW_TOP(row - 1);

        sp_rect(v, SB_ROW_X0, ry0, SB_ROW_X1 + 1, ry0 + SB_ROW_H, &fx0, &fy0, &fx1, &fy1);
    }
    sp_fill(v, fx0, fy0, fx1, fy1, face);
    sp_frame(v, fx0, fy0, fx1, fy1, ink);

    /* THE LABEL, LEFT-ALIGNED against the row's own left edge, and the value RIGHT-ALIGNED
     * against the mark. A centred pair would drift apart as the two strings changed
     * length, and a settings list is read down the left edge. */
    line_fy = fy0 + ((fy1 - fy0) - MENU_FONT_LINE) / 2;
    lx0 = sp_fx(v, SB_ROW_X0) + SP_LABEL_X;
    lx1 = sp_fx(v, SB_VALUE_X0);
    sp_text(v, lx0, line_fy, label, face, ink, lx0, lx1);

    /* The value, right-aligned SP_VALUE_RIGHT in from the row's own right edge -- the
     * padding the operator asked for, and the same number the label has on its side. There
     * is nothing else at this end of the row: the marks are gone (settings_paint.h). */
    vx0 = sp_fx(v, SB_VALUE_X0);
    vx1 = sp_fx(v, SB_VALUE_X1 + 1) - (SP_VALUE_RIGHT * v->dw) / SB_W;

    {
        /* CLIPPED, because a USB row's value is the STICK'S OWN NAME (settings_zone.h) and a
         * name is the operator's to write: the size refusal measures "EMPTY" -- what every
         * USB row is guaranteed to read -- and cuts any longer name to what is left, rather
         * than refusing a view because somebody labelled their stick at length. The table rows
         * fit by construction and the clip is a no-op for them. */
        char clipped[ST_NAME_MAX + 1];

        settings_text_clip(settings_row_text(row, S), vx1 - vx0, clipped,
                           (int)sizeof clipped);
        {
            int pen = vx1 - menu_text_width(clipped);

            if (pen < vx0)
                pen = vx0;             /* the refusal should make this unreachable */
            sp_text(v, pen, line_fy, clipped, face, ink, vx0, vx1);
        }
    }
}

/* --- the picture ------------------------------------------------------------- */

int settings_paint_w(int page_dw)
{
    int w = (SB_W * page_dw) / MZ_LOGICAL_W;

    return w < 1 ? 1 : w;
}

int settings_paint_h(int page_dh)
{
    int h = (SB_H * page_dh) / MZ_LOGICAL_H;

    return h < 1 ? 1 : h;
}

int settings_paint_ok(const struct menu_view *v)
{
    int i;

    if (!v || !v->pix)
        return 0;
    if (v->bpp != 16 && v->bpp != 32)
        return 0;
    if (v->dw < 1 || v->dh < 1 || v->pitch < v->dw)
        return 0;
    /* Every row has to host the font's line box after scaling, or the labels collide with
     * the frames around them. A PROPERTY OF THE SIZE AND NOT OF THE STATE, like
     * every box here keeps -- which is why it is asked once by the caller rather than per
     * pixel here. */
    if ((SB_ROW_H * v->dh) / SB_H < MENU_FONT_LINE + 2)
        return 0;

    /* And every row has to hold its own label whole AND its WIDEST VALUE whole -- the
     * widest and not the one in force, so that a row whose value grew could never take the
     * whole panel off the glass. */
    for (i = 1; i <= SB_ROWS; i++) {
        const char *lab = settings_row_label(i);
        const char *val = settings_row_widest_value(i);
        int lx0 = sp_fx(v, SB_ROW_X0), lx1 = sp_fx(v, SB_VALUE_X0);
        int vx0 = sp_fx(v, SB_VALUE_X0), vx1 = sp_fx(v, SB_VALUE_X1 + 1);
        int vpad = (SP_VALUE_RIGHT * v->dw) / SB_W;

        if (!lab || !val)
            return 0;
        if (lx1 - lx0 < 2 * SP_LABEL_X + menu_text_width(lab))
            return 0;
        if (vx1 - vx0 < vpad + menu_text_width(val))
            return 0;
    }
    return 1;
}

void settings_paint(const struct menu_view *v, const struct settings_state *S,
                    int pressed_row, int flash_row)
{
    unsigned int fill, rule, ink;
    int i, rx0, ry0, rx1, ry1;

    if (!settings_paint_ok(v))
        return;
    if (pressed_row < 0 || pressed_row > SB_ROWS)
        pressed_row = 0;
    if (flash_row < 0 || flash_row > SB_ROWS)
        flash_row = 0;

    /* A HELD ROW IS NEVER ALSO THE FINGER'S, which is settings_zone.c's own doing:
     * settings_pressed() answers 0 for the row whose hold is running, so the blink has
     * somewhere to blink to -- the two are the same pair of pixels, and a row that wore both
     * would show the operator nothing at all. */
    if (flash_row && flash_row == pressed_row)
        pressed_row = 0;

    fill = menu_pixel(v->bpp, MENU_FILL);
    rule = menu_pixel(v->bpp, MENU_DIV);
    ink  = menu_pixel(v->bpp, MENU_LABEL);

    /* The bed first, so every pixel of the box is written by this call. There is no frame
     * round the box: the white goes round each row (this file's header). */
    sp_fill(v, 0, 0, v->dw, v->dh, fill);

    /* The title, centred on the BOX -- not on the rows' narrower rect, because it is the
     * panel's caption and not a sixth row. */
    {
        int tx0, ty0, tx1, ty1, w, line_fy;
        char title_buf[128], ip[64], host[64];
        get_system_net_info(ip, sizeof(ip), host, sizeof(host));
        snprintf(title_buf, sizeof(title_buf), "%s - %s (%s)", SB_TITLE, host, ip);

        sp_rect(v, SB_X0 + SB_BORDER, SB_TITLE_Y0, SB_X1 + 1 - SB_BORDER, SB_TITLE_Y1 + 1,
                &tx0, &ty0, &tx1, &ty1);
        w = menu_text_width(title_buf);
        line_fy = ty0 + ((ty1 - ty0) - MENU_FONT_LINE) / 2;
        sp_text(v, (tx0 + tx1 - w) / 2, line_fy, title_buf, fill, ink, tx0, tx1);
    }

    /* The rule under it, one logical px, across the inner width. */
    sp_rect(v, SB_X0 + SB_BORDER, SB_RULE_Y, SB_X1 + 1 - SB_BORDER, SB_RULE_Y + 1,
            &rx0, &ry0, &rx1, &ry1);
    sp_fill(v, rx0, ry0, rx1, ry1, rule);

    /* The rows. `live` is asked here, per row, so a row that has just gone inoperable is
     * dim on the next paint and not on some later one. */
    for (i = 1; i <= SB_ROWS; i++)
        sp_row(v, S, i, settings_row_live(i, S),
               i == pressed_row || i == flash_row);
}

/* SPDX-License-Identifier: MPL-2.0 */
#ifndef RX3_PANEL_API_H
#define RX3_PANEL_API_H
#include "rx3_platform.h"
/* What a control does when a finger arrives.
 *
 * A stepper is one widget with three touchable parts rather than three
 * controls that happen to sit together, because that is what it is: the ends
 * move a value the middle shows, and the row should not have to be told twice
 * that they belong to each other.
 */
#define RX3_PAD_OUTLINE_BUTTON 6u /* neutral face, semantic outline, fires on release */
#define RX3_PAD_STATUS  5u   /* informational; consumes touch without firing */
#define RX3_PAD_BUTTON  0u   /* fires on release */
#define RX3_PAD_TOGGLE  1u   /* fires on release; is_on lights it */
#define RX3_PAD_STEPPER 2u   /* one widget, three parts: [-] [value] [+] */
#define RX3_PAD_TOGGLE_SLIDER 4u /* tap toggles; hold 350 ms then drag adjusts */
#define RX3_PAD_SLIDER  3u   /* drag, and tap anywhere in the track */

/* A row is laid out inside one deck's half, or once across the screen and
   clipped into each half as it is drawn. */
#define RX3_PAD_SCOPE_DECK   0u
#define RX3_PAD_SCOPE_SCREEN 1u

struct rx3_pad_widget {
    unsigned char kind;
    unsigned char weight;   /* shares of the free width; 0 reads as 1 */
};

/* What a row's `paint` callback draws with. Both calls take SCREEN x of the
   screen-scope row (the cells' own coordinates) and a y inside the control band
   RX3_PAD_BRUSH_TOP .. RX3_PAD_BRUSH_BOTTOM. The core clips each box to the deck
   window being painted (so a box may cross the seam between the two halves).
   box: a filled rectangle, colour RGB888. text: printable ASCII, up to 16
   characters, centred in x1..x2 and drawn whole or not at all; ink 0 = the
   ordinary caption, 1 = lettering for a bright accent face (dark on bright).
   frame, face (neutral control) and light (nonzero: light theme) are the
   theme's own colours, RGB888, valid for the call. Render thread only. */
#define RX3_PAD_BRUSH_TOP 21
#define RX3_PAD_BRUSH_BOTTOM 59
struct rx3_pad_brush {
    void (*box)(int x1, int y1, int x2, int y2, uint32_t rgb888);
    void (*text)(int x1, int x2, const char *ascii, unsigned int ink);
    uint32_t frame, face;
    unsigned int light;
};

/* One row of controls, declared rather than drawn.
 *
 * Nothing a feature supplies here is a coordinate. The core solves the row,
 * paints it and hit-tests it from the one set of rectangles, which is the
 * point: every panel used to lay its controls out in its draw and then work
 * out what had been touched by repeating the same arithmetic somewhere else,
 * with nothing keeping the two in step.
 */
struct rx3_pad_row {
    unsigned int panel_id;
    unsigned int tab_image;
    unsigned char scope;
    unsigned char count;                    /* declared widgets */
    const struct rx3_pad_widget *widgets;

    /* How many of them this deck shows now. Null means always `count`. A
       stems row is one to four wide by what actually loaded. */
    unsigned int (*live_count)(unsigned int deck);

    /* `part` is 0 for a plain widget, and 0, 1 or 2 for a stepper's
       decrement, value and increment. */
    const uint16_t *(*caption)(unsigned int deck, unsigned int widget,
                               unsigned int part);
    int (*is_on)(unsigned int deck, unsigned int widget, unsigned int part);
    void (*fire)(unsigned int deck, unsigned int widget, unsigned int part);

    /* Sliders, in the feature's own units, so the core owns the only division
       on the path and a feature never converts a coordinate. `committed` is
       set on the release that ends a drag and clear while it is moving. */
    unsigned int (*slider_max)(unsigned int deck, unsigned int widget);
    unsigned int (*slider_get)(unsigned int deck, unsigned int widget);
    void (*slider_set)(unsigned int deck, unsigned int widget,
                       unsigned int value, unsigned int committed);

    int (*needs_refresh)(void);
    /* Optional per-deck presentation; state and actions stay with the client. */
    unsigned int (*kind)(unsigned int deck, unsigned int widget);
    /* Optional RGB565 accent; the renderer converts to native RGB888. */
    uint16_t (*colour)(unsigned int deck, unsigned int widget);
    /* Hybrid controls show a track during a drag and while this returns true. */
    int (*slider_visible)(unsigned int deck, unsigned int widget);
    /* Optional per-part semantic RGB565 accent, independent of selection.
       Zero means no semantic colour (unknown value or unavailable choice). */
    uint16_t (*part_colour)(unsigned int deck, unsigned int widget, unsigned int part);
    /* Optional, on the input or render thread: 1 when this row becomes the
       visible panel, 0 whenever the core leaves or bypasses it. Calls with
       0 can repeat. Panel 3 replaces the native STATUS tab. */
    void (*activate)(unsigned int active);
    /* Nonzero draws each TOGGLE_SLIDER as a level button: the face fills from
       the bottom in proportion to the value, full at the maximum and empty
       at zero, with the caption centred. Touch behaviour is unchanged. */
    unsigned char level_style;
    /* Nonzero draws the first widget, a SLIDER, as this many side-by-side
       squares; caption(deck, 0, n) labels square n (1-based) and the square
       holding the value is lit. Touch stays one continuous slider. */
    unsigned char segments;
    /* Optional: while a finger held on a widget moves, how far it is above
       (negative) or below (positive) where it landed, in pixels; 0 when it
       lifts. The value under the finger still follows the horizontal. */
    void (*vertical)(unsigned int deck, unsigned int widget, int offset);
    /* Optional: paints the widget itself, in place of the stock face. Touch is
       unchanged and follows the widget's kind (a SLIDER reports press, moves
       and release through slider_set). x1..x2 is the widget's cell. */
    void (*paint)(unsigned int deck, unsigned int widget, int x1, int x2,
                  const struct rx3_pad_brush *brush);
};

/* A page of the player's own pad pictures that a module paints for itself.
 *
 * RELEASE FX is the sampler, and its eight effect names (BEAT BRAKE...) are
 * pictures, one per pad, drawn into each deck's pad window. While `active`
 * answers yes for a deck, every picture of the page drawn into that deck's
 * window -- ids first_image..last_image -- is replaced by a cell the core
 * paints in the picture's own box: the caption in the pad's colour, lit while
 * the pad is, dark when it has nothing. The title picture, `title_image`, is
 * painted over with the page's title. Outside the page, or with no page
 * registered, nothing is drawn here and the player's own pictures show.
 *
 * Nothing here is a coordinate. The core places the cells from where the
 * player placed its pictures, and says so in the log when those do not look
 * like cells. Callbacks run on the render thread: no allocation, I/O or
 * waiting. Descriptors have process lifetime; one page at a time.
 */
struct rx3_pad_page {
    unsigned int first_image, last_image;
    unsigned int title_image;               /* 0 for none */
    int (*active)(unsigned int deck);
    /* Cell 0 to 7 of a deck: 0 empty (dark), 1 idle, 2 lit. */
    unsigned int (*cell_state)(unsigned int deck, unsigned int cell);
    /* The cell's colour, RGB565, as for a row. */
    uint16_t (*colour)(unsigned int deck, unsigned int cell);
    /* Printable ASCII, up to 16 characters, or null for none. Characters the
       typeface lacks are drawn as spaces; the core cuts the text to the cell. */
    const char *(*caption)(unsigned int deck, unsigned int cell);
    const char *(*title)(unsigned int deck);
    /* The watcher asks now and then; nonzero when a cell changed and the page
       wants repainting (a sound that ended by itself). */
    int (*needs_refresh)(void);
};


/* Descriptors and callbacks have process lifetime. Register at start and
 * unregister at stop. The renderer owns geometry, colours and touch capture.
 * open() queues a request; the render thread applies it.
 */
struct rx3_panel_service {
    int (*register_row)(const struct rx3_pad_row *);
    void (*unregister_row)(const struct rx3_pad_row *);
    int (*open)(unsigned int panel_id);
    /* Render thread only: repaint the performance row and tabs now. */
    void (*refresh)(void);
    /* Queues a return to the native display, as tapping the open tab does.
       The render thread applies it; a no-op when no custom panel is open. */
    int (*close)(void);
    /* The sampler page above. A second registration fails without replacing
       the first. */
    int (*register_page)(const struct rx3_pad_page *);
    void (*unregister_page)(const struct rx3_pad_page *);
    /* Any thread: the page changed (a pad started or stopped), repaint it. The
       render thread applies it, rate limited; nothing happens while the page
       is not on screen but a repaint of the native layers. */
    void (*invalidate_page)(void);
};
extern const struct rx3_panel_service rx3_panels;
#endif

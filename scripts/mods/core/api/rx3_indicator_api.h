/* SPDX-License-Identifier: MPL-2.0 */
#ifndef RX3_INDICATOR_API_H
#define RX3_INDICATOR_API_H
#include "rx3_platform.h"
/* One small status mark in the native header, beside the notice strip.
 *
 * The module owns the pixels, the colours and when it blinks; the core owns
 * the native fill calls. `draw` runs on the render thread each time the header
 * is repainted: no allocation, I/O or waiting. It paints by calling `rect`
 * (header-window pixels, inclusive corners, colour 0xRRGGBB) and draws nothing
 * to hide. `light` is nonzero while the light theme is shown.
 * One owner at a time; descriptors have process lifetime.
 */
typedef void (*rx3_indicator_rect)(void *context, int x1, int y1, int x2, int y2,
                                   uint32_t rgb888);
typedef void (*rx3_indicator_draw_fn)(rx3_indicator_rect rect, void *context, int light);
/* Diagnostic (netstatus-prova): called, at most every 50 ms per native window layer, from the
 * render thread after a text draw. `layer_index` numbers the layers in order of first sight (0-9),
 * (glyph_x, glyph_y) is the glyph's own box origin, `point` is 0 for a text draw, 1 for a text of layer
 * 0x0101, 2 for the player's top-bar image point (where the mark itself is drawn). */
typedef void (*rx3_indicator_probe_fn)(rx3_indicator_rect rect, void *context, unsigned int layer_index,
                                       int glyph_x, int glyph_y, unsigned int point);
struct rx3_indicator_service {
    int (*acquire)(const void *owner, rx3_indicator_draw_fn draw);
    void (*release)(const void *owner);
    /* The owner switches the diagnostic on (a function) or off (null). */
    void (*probe)(const void *owner, rx3_indicator_probe_fn fn);
    /* Any thread: the mark changed (state, minute, blink); the render thread repaints it, at most
       every 250 ms. Costs nothing while no top-bar image has been seen. */
    void (*invalidate)(const void *owner);
    /* Thin progress line along the bottom of the header (theme loading): 0 hides it, 1..1024 is the
       filled share in 1/1024. Any thread; the render thread repaints at most every 250 ms. */
    void (*header_progress)(unsigned int p1024);
};
#endif

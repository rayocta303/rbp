/* SPDX-License-Identifier: MPL-2.0 */
/* Long press on a track in the browser: the detection only. No native call, no
 * drawing, no allocation, and no division, so it runs on the touch thread and
 * under host tests. Down, time, movement, up: a press that stays within
 * LP_RADIUS_PX of where it began for LP_HOLD_MS fires once. */
#ifndef RX3_LONGPRESS_STATE_H
#define RX3_LONGPRESS_STATE_H

#define LP_HOLD_MS 600u
#define LP_RADIUS_PX 12
/* The rows of the track list, in screen pixels (1280x800). Measured on the sort
 * header (y 50..100, x from 434); the row band below it is NOT measured. */
#define LP_ZONE_X1 434
#define LP_ZONE_Y1 100
#define LP_ZONE_X2 1280
#define LP_ZONE_Y2 780

enum lp_state { LP_IDLE, LP_ARMED, LP_FIRED, LP_IGNORED };
enum lp_event { LP_NONE, LP_FIRE, LP_CANCEL };
enum lp_why { LP_WHY_NONE, LP_WHY_MOVED, LP_WHY_LEFT, LP_WHY_RELEASED_EARLY, LP_WHY_NOT_LIST };

struct lp_machine {
    unsigned state;
    int x0, y0;
    uint32_t t0;
    /* Of the last event returned, for the log. */
    uint32_t held_ms;
    int dx, dy;
    unsigned why;
    int on_release;
};

static int lp_in_zone(int x, int y)
{
    return x >= LP_ZONE_X1 && x < LP_ZONE_X2 && y >= LP_ZONE_Y1 && y < LP_ZONE_Y2;
}

static int lp_moved(const struct lp_machine *m, int x, int y)
{
    int dx = x - m->x0, dy = y - m->y0;
    return dx * dx + dy * dy > LP_RADIUS_PX * LP_RADIUS_PX;
}

/* One touch event. eligible: the track list is on screen with no menu, now.
 * Events arrive while the finger is down; if the screen stops sending them while
 * the finger is still, the release fires it instead (on_release = 1). */
static unsigned lp_step(struct lp_machine *m, int pressed, int x, int y, uint32_t now_ms, int eligible)
{
    m->why = LP_WHY_NONE;
    m->on_release = 0;
    if (!pressed) {
        unsigned was = m->state;
        m->held_ms = now_ms - m->t0;
        m->dx = x - m->x0;
        m->dy = y - m->y0;
        m->state = LP_IDLE;
        if (was == LP_ARMED && eligible && !lp_moved(m, x, y) && m->held_ms >= LP_HOLD_MS) {
            m->on_release = 1;
            return LP_FIRE;
        }
        if (was == LP_ARMED) {
            m->why = LP_WHY_RELEASED_EARLY;
            return LP_CANCEL;
        }
        return LP_NONE;
    }
    if (m->state == LP_IDLE) {
        m->x0 = x; m->y0 = y; m->t0 = now_ms; m->held_ms = 0; m->dx = m->dy = 0;
        if (!eligible || !lp_in_zone(x, y)) {
            m->state = LP_IGNORED;
            m->why = LP_WHY_NOT_LIST;
            return LP_NONE;
        }
        m->state = LP_ARMED;
        return LP_NONE;
    }
    if (m->state != LP_ARMED)
        return LP_NONE;
    m->held_ms = now_ms - m->t0;
    m->dx = x - m->x0;
    m->dy = y - m->y0;
    if (!eligible || !lp_in_zone(x, y)) {
        m->state = LP_IGNORED;
        m->why = LP_WHY_LEFT;
        return LP_CANCEL;
    }
    if (lp_moved(m, x, y)) {
        m->state = LP_IGNORED;
        m->why = LP_WHY_MOVED;
        return LP_CANCEL;
    }
    if (m->held_ms >= LP_HOLD_MS) {
        m->state = LP_FIRED;
        return LP_FIRE;
    }
    return LP_NONE;
}

/* Notice text: prefix then title, at most 95 UTF-16 units, a title cut so no half
 * surrogate is left, control characters dropped. Empty title: the bare prefix
 * without its trailing ": " (prefix_bare). Returns the length. */
static unsigned lp_compose(uint16_t *out, const uint16_t *prefix, const uint16_t *prefix_bare,
                           const uint16_t *title)
{
    unsigned n = 0, t = 0, have = 0;
    if (title) while (title[t] && t < 95) { if (title[t] >= 0x20) { have = 1; break; } t++; }
    const uint16_t *head = have ? prefix : prefix_bare;
    while (head[n] && n < 95) { out[n] = head[n]; n++; }
    if (have) {
        for (; title[t] && n < 95; t++)
            if (title[t] >= 0x20) out[n++] = title[t];
        if (n && out[n - 1] >= 0xd800 && out[n - 1] <= 0xdbff) n--;
    }
    out[n] = 0;
    return n;
}

/* "long press x=.. y=.. held=..ms id=.. row=.. result" without printf. */
static unsigned lp_append_text(char *out, unsigned at, unsigned size, const char *text)
{
    while (*text && at + 1 < size) out[at++] = *text++;
    out[at] = 0;
    return at;
}
static unsigned lp_append_number(char *out, unsigned at, unsigned size, long value)
{
    char reversed[12];
    unsigned n = 0, magnitude = value < 0 ? (unsigned)(-value) : (unsigned)value;
    if (value < 0 && at + 1 < size) out[at++] = '-';
    do { reversed[n++] = (char)('0' + magnitude % 10u); magnitude /= 10u; } while (magnitude && n < 11);
    while (n && at + 1 < size) out[at++] = reversed[--n];
    out[at] = 0;
    return at;
}
static void lp_format_log(char *out, unsigned size, int x, int y, uint32_t held_ms,
                          long id, long row, const char *result)
{
    unsigned at = 0;
    out[0] = 0;
    at = lp_append_text(out, at, size, "browse-longpress: x=");
    at = lp_append_number(out, at, size, x);
    at = lp_append_text(out, at, size, " y=");
    at = lp_append_number(out, at, size, y);
    at = lp_append_text(out, at, size, " held=");
    at = lp_append_number(out, at, size, (long)held_ms);
    at = lp_append_text(out, at, size, "ms id=");
    at = lp_append_number(out, at, size, id);
    at = lp_append_text(out, at, size, " row=");
    at = lp_append_number(out, at, size, row);
    at = lp_append_text(out, at, size, " ");
    (void)lp_append_text(out, at, size, result);
}
#endif

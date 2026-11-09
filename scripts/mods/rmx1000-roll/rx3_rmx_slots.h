/* SPDX-License-Identifier: MPL-2.0 */
/* A BEAT FX position (a "slot" of the selector, 0..13) can keep the player's own
 * effect or play an RMX effect chosen from a list. This header is the logic of
 * that, as pure code the computer can test: the registry of the effects, the
 * reading of the configuration, and the state machine of the choosing.
 * No allocation, no I/O, no libc, no threads. Original work.
 *
 * Registry. Every effect is one row: an id (what a configuration file names),
 * a short name for the screen (at most 10 characters), a one-line description,
 * and whether it is written yet. A new effect is a new row (and its processing
 * function in the module); the list on the screen is generated from the rows.
 *
 * Configuration (data, one `key=value` per line; unknown keys, unknown or
 * unwritten effects, and bad numbers are skipped, never an error):
 *   slot.<position>=<id>     the effect that position plays instead of the native one
 *   list=<id>,<id>,...       the effects the chooser offers, in this order
 *   policy=reset|keep        what leaving a position does to a chosen effect
 *   timeout=<seconds>        how long the chooser waits before it cancels
 *
 * Choosing. Enter on a position, move through the list (the native effect is
 * always the first entry), confirm or cancel. If the effect is ON when you
 * confirm, the new one waits and takes over at the next OFF, so the sound never
 * changes under your hands. Leaving the position cancels the choosing and, with
 * policy=reset, puts the position back to what the configuration says (policy=keep
 * leaves the chosen effect where it is).
 */
#ifndef RX3_RMX_SLOTS_H
#define RX3_RMX_SLOTS_H

#define RMXL_POSITIONS 14u
#define RMXL_LIST_MAX 16u
#define RMXL_NATIVE 0u                 /* registry row 0: the player's own effect */
#define RMXL_NONE 0xffu
#define RMXL_DEFAULT_TIMEOUT_MS 6000u

struct rmxl_effect { const char *id; const char *name; const char *about; unsigned int written; };

/* Row 0 is the native effect. `written` is 1 for effects that exist in the mod. */
static const struct rmxl_effect rmxl_registry[] = {
    {"native",     "NATIVE",     "the player's own effect",                              1},
    {"hpf-echo",   "HPF ECHO",   "sliding high-pass filter, then a tempo echo",           1},
    {"lpf-echo",   "LPF ECHO",   "sliding low-pass filter, then a tempo echo",            1},
    {"bpf-echo",   "BPF ECHO",   "sliding band-pass filter, then a tempo echo",           1},
    {"rmx-roll",   "RMX",        "repeats the MASTER from the press, at a note value",    1},
    {"dub-echo",   "DUB ECHO",   "echo whose repeats get darker each pass",               0},
    {"noise",      "NOISE",      "white noise through a band-pass filter and an echo",    0},
    {"crush-echo", "CRUSH ECHO", "bit and rate reduction, then an echo",                  0},
    {"spiral-up",  "SPIRAL UP",  "a delay that climbs in pitch",                          0},
    {"spiral-down","SPIRAL DN",  "a delay that falls in pitch",                           0},
    {"reverb-up",  "REVERB UP",  "reverb with a high-pass on the tail",                   0},
    {"reverb-down","REVERB DN",  "reverb with a low-pass on the tail",                    0}
};
#define RMXL_EFFECTS ((unsigned int)(sizeof(rmxl_registry) / sizeof(rmxl_registry[0])))

static inline int rmxl_streq(const char *a, unsigned int alen, const char *b)
{
    unsigned int i = 0;
    for (; i < alen; i++) if (!b[i] || a[i] != b[i]) return 0;
    return b[i] == '\0';
}

/* The registry row of an id of `length` characters, or RMXL_NONE. */
static inline unsigned int rmxl_find(const char *id, unsigned int length)
{
    for (unsigned int i = 0; i < RMXL_EFFECTS; i++)
        if (rmxl_streq(id, length, rmxl_registry[i].id)) return i;
    return RMXL_NONE;
}

struct rmxl_config {
    unsigned char assign[RMXL_POSITIONS];   /* registry row per position, native by default */
    unsigned char list[RMXL_LIST_MAX];      /* the chooser's entries; list[0] is always the native one */
    unsigned char list_count;
    unsigned char reset;                    /* 1 reset on leaving a position, 0 keep */
    unsigned int timeout_ms;
};

/* The default list: the native entry, then every written effect, in registry order. */
static inline void rmxl_defaults(struct rmxl_config *c)
{
    for (unsigned int i = 0; i < RMXL_POSITIONS; i++) c->assign[i] = RMXL_NATIVE;
    c->list_count = 0;
    for (unsigned int i = 0; i < RMXL_EFFECTS && c->list_count < RMXL_LIST_MAX; i++)
        if (rmxl_registry[i].written) c->list[c->list_count++] = (unsigned char)i;
    c->reset = 1;
    c->timeout_ms = RMXL_DEFAULT_TIMEOUT_MS;
}

static inline int rmxl_number(const char *s, unsigned int length, unsigned int limit, unsigned int *out)
{
    if (!length || length > 6u) return 0;
    unsigned int v = 0;
    for (unsigned int i = 0; i < length; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10u + (unsigned int)(s[i] - '0');
    }
    if (v > limit) return 0;
    *out = v;
    return 1;
}

static inline int rmxl_begins(const char *line, unsigned int length, const char *key, unsigned int *at)
{
    unsigned int k = 0;
    while (key[k]) { if (k >= length || line[k] != key[k]) return 0; k++; }
    *at = k;
    return 1;
}

/* Reads `text` (`length` bytes) over the defaults already in `c`. Returns how
   many lines were skipped because they were wrong (a comment or a blank line is
   not wrong). Never fails and never writes out of bounds. */
static inline unsigned int rmxl_parse(struct rmxl_config *c, const char *text, unsigned int length)
{
    unsigned int skipped = 0, at = 0;
    unsigned char list[RMXL_LIST_MAX];
    unsigned int list_count = 0, list_given = 0;
    while (at < length) {
        unsigned int end = at;
        while (end < length && text[end] != '\n' && text[end] != '\r') end++;
        const char *line = text + at;
        unsigned int n = end - at, k = 0;
        at = end < length ? end + 1u : end;
        while (n && (line[n - 1] == ' ' || line[n - 1] == '\t')) n--;
        if (!n || line[0] == '#') continue;
        if (rmxl_begins(line, n, "slot.", &k)) {
            unsigned int eq = k;
            while (eq < n && line[eq] != '=') eq++;
            unsigned int position = 0, row;
            if (eq >= n || !rmxl_number(line + k, eq - k, RMXL_POSITIONS - 1u, &position)) { skipped++; continue; }
            row = rmxl_find(line + eq + 1u, n - eq - 1u);
            if (row == RMXL_NONE || !rmxl_registry[row].written) { skipped++; continue; }
            c->assign[position] = (unsigned char)row;
        } else if (rmxl_begins(line, n, "list=", &k)) {
            list_given = 1; list_count = 0;
            list[list_count++] = RMXL_NATIVE;
            unsigned int s = k;
            while (s <= n) {
                unsigned int e = s;
                while (e < n && line[e] != ',') e++;
                if (e > s) {
                    unsigned int row = rmxl_find(line + s, e - s);
                    unsigned int dup = 0;
                    for (unsigned int i = 0; i < list_count; i++) dup |= list[i] == row;
                    if (row == RMXL_NONE || !rmxl_registry[row].written) skipped++;
                    else if (!dup && list_count < RMXL_LIST_MAX) list[list_count++] = (unsigned char)row;
                }
                s = e + 1u;
            }
        } else if (rmxl_begins(line, n, "policy=", &k)) {
            if (rmxl_streq(line + k, n - k, "reset")) c->reset = 1;
            else if (rmxl_streq(line + k, n - k, "keep")) c->reset = 0;
            else skipped++;
        } else if (rmxl_begins(line, n, "timeout=", &k)) {
            unsigned int seconds;
            if (rmxl_number(line + k, n - k, 60u, &seconds) && seconds >= 2u) c->timeout_ms = seconds * 1000u;
            else skipped++;
        } else skipped++;
    }
    if (list_given && list_count > 1u) {          /* a list of only the native entry is no list: keep the default */
        for (unsigned int i = 0; i < list_count; i++) c->list[i] = list[i];
        c->list_count = (unsigned char)list_count;
    }
    return skipped;
}

enum rmxl_state { RMXL_IDLE = 0, RMXL_CHOOSING = 1 };

struct rmxl {
    struct rmxl_config cfg;
    unsigned char current[RMXL_POSITIONS];   /* what each position plays now */
    unsigned char pending[RMXL_POSITIONS];   /* chosen while ON, waiting for OFF; RMXL_NONE = nothing */
    unsigned int position;                   /* the selector as last seen, RMXL_NONE until seen */
    unsigned int state, cursor, idle_ms;
    unsigned int on;
    unsigned int generation;                 /* grows with every change that the screen should show */
};

static inline void rmxl_init(struct rmxl *m, const struct rmxl_config *cfg)
{
    m->cfg = *cfg;
    for (unsigned int i = 0; i < RMXL_POSITIONS; i++) { m->current[i] = cfg->assign[i]; m->pending[i] = RMXL_NONE; }
    m->position = RMXL_NONE; m->state = RMXL_IDLE; m->cursor = 0; m->idle_ms = 0; m->on = 0; m->generation = 0;
}

/* The effect the audio should run for the position now (a registry row; RMXL_NATIVE = leave it to the player). */
static inline unsigned int rmxl_active(const struct rmxl *m)
{
    return m->position < RMXL_POSITIONS ? m->current[m->position] : RMXL_NATIVE;
}

/* The selector moved (or was seen for the first time). */
static inline void rmxl_selector(struct rmxl *m, unsigned int position)
{
    if (position >= RMXL_POSITIONS) return;
    if (position == m->position) return;
    unsigned int old = m->position;
    if (old < RMXL_POSITIONS) {
        if (m->cfg.reset) { m->current[old] = m->cfg.assign[old]; }        /* back to what the configuration says */
        m->pending[old] = RMXL_NONE;
    }
    m->position = position;
    m->state = RMXL_IDLE; m->idle_ms = 0;
    m->generation++;
}

/* Starts the choosing on the current position. Refused (0) while the position is not known. */
static inline int rmxl_enter(struct rmxl *m)
{
    if (m->position >= RMXL_POSITIONS || m->cfg.list_count == 0) return 0;
    if (m->state == RMXL_CHOOSING) return 1;
    m->state = RMXL_CHOOSING; m->idle_ms = 0;
    /* The cursor starts on what the position plays now (or what is waiting), if it is on the list. */
    unsigned int shown = m->pending[m->position] != RMXL_NONE ? m->pending[m->position] : m->current[m->position];
    m->cursor = 0;
    for (unsigned int i = 0; i < m->cfg.list_count; i++) if (m->cfg.list[i] == shown) m->cursor = i;
    m->generation++;
    return 1;
}

/* Moves the cursor by +1 or -1 around the list. */
static inline void rmxl_move(struct rmxl *m, int step)
{
    if (m->state != RMXL_CHOOSING) return;
    unsigned int n = m->cfg.list_count;
    m->cursor = step > 0 ? (m->cursor + 1u) % n : (m->cursor + n - 1u) % n;
    m->idle_ms = 0; m->generation++;
}

/* The entry the cursor is on, as a registry row. */
static inline unsigned int rmxl_cursor_row(const struct rmxl *m)
{
    return m->cursor < m->cfg.list_count ? m->cfg.list[m->cursor] : RMXL_NATIVE;
}

/* Confirms: the effect is taken now when the effect is OFF, at the next OFF when it is ON. */
static inline void rmxl_confirm(struct rmxl *m)
{
    if (m->state != RMXL_CHOOSING || m->position >= RMXL_POSITIONS) return;
    unsigned int row = rmxl_cursor_row(m);
    if (m->on && row != m->current[m->position]) m->pending[m->position] = (unsigned char)row;
    else { m->current[m->position] = (unsigned char)row; m->pending[m->position] = RMXL_NONE; }
    m->state = RMXL_IDLE; m->generation++;
}

static inline void rmxl_cancel(struct rmxl *m)
{
    if (m->state != RMXL_CHOOSING) return;
    m->state = RMXL_IDLE; m->generation++;
}

/* The effect went ON or OFF; an effect that waited takes over at OFF. */
static inline void rmxl_set_on(struct rmxl *m, unsigned int on)
{
    on = on != 0;
    if (on == m->on) return;
    m->on = on;
    if (!on && m->position < RMXL_POSITIONS && m->pending[m->position] != RMXL_NONE) {
        m->current[m->position] = m->pending[m->position];
        m->pending[m->position] = RMXL_NONE;
        m->generation++;
    }
}

/* Time passes while choosing; an idle chooser cancels itself. */
static inline void rmxl_tick(struct rmxl *m, unsigned int ms)
{
    if (m->state != RMXL_CHOOSING) return;
    m->idle_ms += ms;
    if (m->idle_ms >= m->cfg.timeout_ms) rmxl_cancel(m);
}

#endif

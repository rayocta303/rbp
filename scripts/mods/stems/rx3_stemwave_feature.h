/* SPDX-License-Identifier: MPL-2.0 */
#ifndef RX3_STEMWAVE_FEATURE_H
#define RX3_STEMWAVE_FEATURE_H

#include "rx3_stemwave_render.h"

static int stemwave_enabled;
static struct stemwave_deck stemwave_decks[2];
static unsigned int stemwave_reported_shapes;
static unsigned int stemwave_package_reported;
static unsigned int stemwave_axis_reported;
/* Diagnostic only (RX3 2026-11-03: "le bande non cambiano quando spegni uno
   stem, diventano RGB"): which way stemwave_paint_bands went, the first time
   each. If it ever refuses, the 3 Band theme falls through to the embedded
   or native path below, whose colours are not the part colours. */
static unsigned int stemwave_bands_painted, stemwave_bands_refused;

/* Availability and selected roles are published together by the stems loader. */
static struct stemwave_selection stemwave_selection_for(unsigned int deck)
{
    struct rx3_mix_state state = framework->mix_state(deck);
    struct stemwave_selection selection = {
        .selected = state.ready ? state.selected : state.available,
        .available = state.available,
        .ready = state.ready
    };
    /* The waveform follows the knobs in tenths: each step costs one stock
       rebuild (fifths read as jumps on the RX3, 2026-10-15). */
    if (state.ready)
        for (unsigned int role = 0; role < 4u; role++) {
            unsigned int step = (((state.levels >> (role * 7u)) & 127u) + 5u) / 10u;
            if (step > 10u) step = 10u;
            if (!(state.available & (1u << role))) step = 0u;
            selection.steps |= step << (role * 4u);
        }
    return selection;
}

/* How loud one part is over the whole track, from its own embedded waveform.
   Zero when the package has no waveform for that part alone. */
static unsigned int stemwave_role_weight(unsigned int deck, unsigned int role, unsigned int shape,
                                         uint8_t *scratch, unsigned int count)
{
    unsigned int format = shape == WAVEFORM_3BAND ? 3u : shape;
    if (framework->waveform(deck, 1u << role, format, scratch, count) != count) return 0u;
    unsigned long long sum = 0u;
    for (unsigned int i = 0; i < count; i++) {
        if (shape == WAVEFORM_BLUE) sum += scratch[i] & 31u;
        else if (shape == WAVEFORM_RGB) sum += (scratch[i * 2u + 1u] >> 2u) & 31u;
        else sum += (unsigned int)scratch[i * 3u] + scratch[i * 3u + 1u] + scratch[i * 3u + 2u];
    }
    return (unsigned int)(sum / count) + 1u;
}

/* 0..100: how much of the selected parts' waveform the knobs leave. Each part
   counts as loud as it is in the track, so lowering a quiet part shrinks the
   waveform a little and lowering the vocal of a vocal track a lot. */
static unsigned int stemwave_knob_scale(unsigned int deck, struct stemwave_selection selection,
                                        unsigned int shape, uint8_t *scratch, unsigned int count)
{
    unsigned int weight[4] = {0}, missing = 0u, full = 1u;
    for (unsigned int role = 0; role < 4u; role++)
        if ((selection.selected & (1u << role)) && ((selection.steps >> (role * 4u)) & 15u) < 10u)
            full = 0u;
    if (full) return 100u;
    for (unsigned int role = 0; role < 4u; role++) {
        if (!(selection.selected & (1u << role))) continue;
        weight[role] = stemwave_role_weight(deck, role, shape, scratch, count);
        if (!weight[role]) missing = 1u;
    }
    unsigned long long num = 0u, den = 0u;
    for (unsigned int role = 0; role < 4u; role++) {
        if (!(selection.selected & (1u << role))) continue;
        unsigned int w = missing ? 1u : weight[role];
        num += (unsigned long long)w * ((selection.steps >> (role * 4u)) & 15u);
        den += (unsigned long long)w * 10u;
    }
    return den ? (unsigned int)(num * 100u / den) : 100u;
}

static void stemwave_scale_columns(struct waveform_column *columns, unsigned int count,
                                   unsigned int scale)
{
    if (scale >= 100u) return;
    for (unsigned int i = 0; i < count; i++)
        for (unsigned int band = 0; band < 3u; band++)
            columns[i].amplitude[band] = (uint8_t)(columns[i].amplitude[band] * scale / 100u);
}

/* The waveform in the parts' colours (RX3, 2026-10-15): every column takes
   the colour of the loudest part there, VOCE green, HARMONY red, BASS purple,
   DRUMS blue, and its height follows the knobs. The package carries each
   part's own waveform, so this is ours alone; any miss leaves the stock path. */
static uint8_t *stemwave_roles;          /* four single-part waveforms, mapped once */
static const uint16_t stemwave_role_colour[4] = {
    STEMWAVE_COLOUR_ONE, STEMWAVE_COLOUR_TWO, STEMWAVE_COLOUR_FOUR, STEMWAVE_COLOUR_EIGHT
};

static unsigned int stemwave_height(const uint8_t *data, unsigned int i, unsigned int shape)
{
    if (shape == WAVEFORM_BLUE) return data[i] & 31u;
    if (shape == WAVEFORM_RGB) return ((((unsigned int)data[i * 2u] << 8u) | data[i * 2u + 1u]) >> 2u) & 31u;
    unsigned int a = data[i * 3u], b = data[i * 3u + 1u], c = data[i * 3u + 2u];
    return a > b ? (a > c ? a : c) : (b > c ? b : c);
}

/* The package draws its mix lower than the player draws the track (half,
   RX3 2026-10-15): the player's own height over it, x256, read once per track and
   shape from the columns while they are still the player's. */
static unsigned int stemwave_height_ratio(struct stemwave_deck *state, unsigned int shape,
                                          const struct waveform_column *columns,
                                          const uint8_t *mix, unsigned int count)
{
    unsigned int ratio = __atomic_load_n(&state->ratio_x256, __ATOMIC_SEQ_CST);
    if (ratio && state->ratio_shape == shape) return ratio;
    unsigned long long native = 0u, package = 0u;
    for (unsigned int i = 0; i < count; i++) {
        native += (unsigned int)columns[i].amplitude[0] + columns[i].amplitude[1] +
                  columns[i].amplitude[2];
        package += stemwave_height(mix, i, shape);
    }
    ratio = native && package ? (unsigned int)(native * 256u / package) : 256u;
    if (ratio < 128u) ratio = 128u;
    if (ratio > 1024u) ratio = 1024u;
    state->ratio_shape = shape;
    __atomic_store_n(&state->ratio_x256, ratio, __ATOMIC_SEQ_CST);
    framework->log_number("stem waveform: height scale x100 = ", ratio * 100u / 256u);
    return ratio;
}

#define STEMWAVE_ROOM ((unsigned long)WAVEFORM_COLUMN_LIMIT * 3u)

/* Every available part's own waveform into stemwave_roles, and each selected part's knob
   (0..100) into `level`. 0 when anything is missing. */
static int stemwave_load_roles(unsigned int deck, struct stemwave_selection selection,
                               unsigned int format, unsigned int count, unsigned int level[4])
{
    if (!stemwave_roles) {
        void *block = mmap(0, STEMWAVE_ROOM * 4u, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (block == MAP_FAILED) return 0;
        stemwave_roles = block;
    }
    for (unsigned int role = 0; role < 4u; role++) {
        level[role] = 0u;
        if (!(selection.available & (1u << role))) continue;
        if (framework->waveform(deck, 1u << role, format, stemwave_roles + role * STEMWAVE_ROOM, count) != count)
            return 0;
        if (selection.selected & (1u << role))
            level[role] = ((selection.steps >> (role * 4u)) & 15u) * 10u;
    }
    return 1;
}

/* The player's own column heights per deck, saved with the height ratio. */
static uint8_t *stemwave_native[2];
static unsigned int stemwave_native_count[2];

static void stemwave_save_native(unsigned int deck, const struct waveform_column *columns,
                                 unsigned int count)
{
    stemwave_native_count[deck] = 0u;
    if (!stemwave_native[deck]) {
        void *block = mmap(0, STEMWAVE_ROOM, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (block == MAP_FAILED) return;
        stemwave_native[deck] = block;
    }
    for (unsigned int i = 0; i < count; i++)
        memcpy(stemwave_native[deck] + i * 3u, columns[i].amplitude, 3u);
    stemwave_native_count[deck] = count;
}

static int stemwave_paint_coloured(unsigned int deck, struct stemwave_selection selection,
                                   unsigned int shape, struct waveform_column *columns,
                                   unsigned int count)
{
    if (!selection.ready || !selection.available) return 0;
    const unsigned long room = STEMWAVE_ROOM;
    unsigned int format = shape == WAVEFORM_3BAND ? 3u : shape, level[4];
    if (!stemwave_load_roles(deck, selection, format, count, level)) return 0;
    /* The height is the whole mix's, so a part alone (always lower than
       the mix) does not halve the waveform (RX3, 2026-10-15). The knobs
       take away each part's share of it. */
    /* The deck's scratch: the fallback below fills it only after this returned 0. */
    uint8_t *stemwave_mix = stemwave_decks[deck].package_amplitudes;
    if (framework->waveform(deck, selection.available, format, stemwave_mix, count) != count)
        return 0;
    struct stemwave_deck *state = &stemwave_decks[deck];
    unsigned int cap = shape == WAVEFORM_3BAND ? 255u : 31u;
    if (!__atomic_load_n(&state->ratio_x256, __ATOMIC_SEQ_CST) || state->ratio_shape != shape)
        stemwave_save_native(deck, columns, count);
    unsigned int ratio = stemwave_height_ratio(state, shape, columns, stemwave_mix, count);
    const uint8_t *native = stemwave_native_count[deck] == count ? stemwave_native[deck] : 0;
    for (unsigned int i = 0; i < count; i++) {
        unsigned int best = 0u, loudest = 0u, total = 0u, kept = 0u, share[4] = {0};
        for (unsigned int role = 0; role < 4u; role++) {
            if (!(selection.available & (1u << role))) continue;
            unsigned int height = stemwave_height(stemwave_roles + role * room, i, shape);
            total += height;
            kept += height * level[role];
            share[role] = height * level[role];
            if (level[role] && height * level[role] > loudest) {
                loudest = height * level[role];
                best = role;
            }
        }
        struct waveform_column *column = &columns[i];
        if (shape == WAVEFORM_RGB && native) {
            stemwave_rgb_column(column, native + i * 3u, kept, total, share);
            continue;
        }
        unsigned int mix = stemwave_height(stemwave_mix, i, shape) * ratio / 256u;
        unsigned int height = total ? mix * kept / (total * 100u) : 0u;
        memset(column, 0, sizeof(*column));
        column->amplitude[0] = (uint8_t)(height > cap ? cap : height);
        column->colour[0] = shape == WAVEFORM_RGB ? stemwave_blend_565(share) : stemwave_role_colour[best];
    }
    return 1;
}

/* The 3 Band theme with stems: one band per part (VOCAL, HARMONY, DRUMS + BASS, RX3
   2026-11-03), stacked to the whole mix's height at the player's scale and lowered by the
   knobs like RGB. Until a knob or pad changes anything the player's own columns stay;
   after that they are repainted in place, never rebuilt. */
static int stemwave_paint_bands(unsigned int deck, struct stemwave_selection selection,
                                struct waveform_column *columns, unsigned int count, int full)
{
    struct stemwave_deck *state = &stemwave_decks[deck];
    if (!selection.ready || !selection.available) {
        if (!stemwave_bands_refused) {
            stemwave_bands_refused = 1u;
            framework->log_line("stem waveform: 3 band refused, not ready");
        }
        return 0;
    }
    if (framework->waveform(deck, selection.available, 3u, state->package_amplitudes, count) != count) {
        if (!stemwave_bands_refused) {
            stemwave_bands_refused = 1u;
            framework->log_line("stem waveform: 3 band refused, package axis mismatch");
        }
        return 0;
    }
    unsigned int first = !__atomic_load_n(&state->ratio_x256, __ATOMIC_SEQ_CST) ||
        state->ratio_shape != WAVEFORM_3BAND;
    unsigned int ratio = stemwave_height_ratio(state, WAVEFORM_3BAND, columns, state->package_amplitudes, count);
    if (first && full) return 1;
    unsigned int level[4];
    if (!stemwave_load_roles(deck, selection, 3u, count, level)) {
        if (!stemwave_bands_refused) {
            stemwave_bands_refused = 1u;
            framework->log_line("stem waveform: 3 band refused, a part's waveform was not ready");
        }
        return 0;
    }
    if (!stemwave_bands_painted) {
        stemwave_bands_painted = 1u;
        framework->log_line("stem waveform: 3 band painted in the parts' colours");
    }
    for (unsigned int i = 0; i < count; i++) {
        unsigned int total = 0u, weight[4] = {0};
        unsigned long long kept = 0u;
        for (unsigned int role = 0; role < 4u; role++) {
            if (!(selection.available & (1u << role))) continue;
            unsigned int height = stemwave_height(stemwave_roles + role * STEMWAVE_ROOM, i, WAVEFORM_3BAND);
            total += height;
            weight[role] = height * level[role];
            kept += weight[role];
        }
        unsigned int mix = stemwave_height(state->package_amplitudes, i, WAVEFORM_3BAND) * ratio / 256u;
        stemwave_stack_parts(&columns[i], total ? (unsigned int)(mix * kept / (total * 100u)) : 0u, weight);
    }
    return 1;
}

static int hooked_ex_wave_renew_check(unsigned int deck)
{
    int result = original_ex_wave_renew_check(deck);
    if (deck >= 2u)
        return result;
    struct stemwave_deck *state = &stemwave_decks[deck];
    const uint8_t *player = (const uint8_t *)(WAVEFORM_STATE_BASE + deck * WAVEFORM_STATE_STRIDE);
    unsigned int shape = *(const uint32_t *)(player + WAVEFORM_MODE_OFFSET);
    struct stemwave_selection selection = stemwave_selection_for(deck);
    /* Probe before overview or column writes; absence is not a render hint. */
    if (framework->waveform(deck, 0u, shape==WAVEFORM_3BAND?3u:shape, state->package_amplitudes, 0u) == 0xfffffffeu)
        return result;
    if (shape == WAVEFORM_3BAND)
        stemwave_blue_overview(state, (uint8_t *)(WAVEFORM_3BAND_BASE + deck * WAVEFORM_3BAND_BYTES), selection);

    unsigned int key = selection.selected | (shape << 8u) | (selection.steps << 12u);
    unsigned int drawn_before = __atomic_load_n(&state->drawn_mask, __ATOMIC_SEQ_CST);
    enum stemwave_action action = stemwave_renew_action(state, result, key);
    /* A knob step, or a part switched off by its knob (under 5 %, RX3
       2026-10-15): the coloured columns come from the package, so they
       are repainted in place; only a new shape asks the player. Asking the player to rebuild first showed its
       stock shape between two coloured frames (RX3, 2026-10-15). */
    if (action == STEMWAVE_REQUEST && selection.ready &&
        stemwave_repaint_in_place(drawn_before, key, __atomic_load_n(&state->ratio_x256, __ATOMIC_SEQ_CST) &&
                                  state->ratio_shape == shape))
        action = STEMWAVE_PAINT;
    int full_mix = selection.selected == selection.available;
    if (action == STEMWAVE_REQUEST) {
        ((void (*)(unsigned int, unsigned int))WAVEFORM_REQUEST_RENEW)(deck, 1u);
        return result;
    }
    unsigned int all_full = 1u;
    for (unsigned int role = 0; role < 4u; role++)
        if ((selection.available & (1u << role)) && ((selection.steps >> (role * 4u)) & 15u) < 10u &&
            (selection.selected & (1u << role)))
            all_full = 0u;
    if (action != STEMWAVE_PAINT || (full_mix && all_full && !selection.ready))
        return result;
    struct stemwave_style style = {.colour = 0xffffu, .roles = 7u, .suppress_high = 0};
    int supported = stemwave_style_for(selection, &style);
    unsigned int offset = shape == WAVEFORM_RGB ? WAVEFORM_COUNT_BANDS :
        (shape == WAVEFORM_3BAND ? WAVEFORM_COUNT_BLUE : WAVEFORM_COUNT_FLAT);
    const uint32_t *count_pointer = *(const uint32_t *const *)(player + offset);
    if (!count_pointer)
        return result;
    unsigned int count = *count_pointer;
    if (count > WAVEFORM_COLUMN_LIMIT)
        count = WAVEFORM_COLUMN_LIMIT;
    if (!count || shape > WAVEFORM_3BAND)
        return result;
    struct waveform_column *columns = (struct waveform_column *)(WAVEFORM_COLUMNS_BASE + deck * WAVEFORM_COLUMN_STRIDE);
    if (shape == WAVEFORM_3BAND && stemwave_paint_bands(deck, selection, columns, count, full_mix && all_full)) {
        __atomic_store_n(&state->columns, count, __ATOMIC_SEQ_CST);
        return result;
    }
    if (shape != WAVEFORM_3BAND && stemwave_paint_coloured(deck, selection, shape, columns, count)) {
        __atomic_store_n(&state->columns, count, __ATOMIC_SEQ_CST);
        if (!(stemwave_reported_shapes & (16u << shape))) {
            stemwave_reported_shapes |= 16u << shape;
            framework->log_line("stem waveform: painted in the parts' colours");
        }
        return result;
    }
    if (full_mix && all_full) return result;
    unsigned int scale = stemwave_knob_scale(deck, selection, shape, state->package_amplitudes, count);
    if (full_mix) {
        /* Every part plays but some quieter: the stock waveform, only lower. */
        stemwave_scale_columns(columns, count, scale);
        __atomic_store_n(&state->columns, count, __ATOMIC_SEQ_CST);
        return result;
    }
    unsigned int embedded = framework->waveform(deck, selection.selected, shape==WAVEFORM_3BAND?3u:shape, state->package_amplitudes, count);
    if (embedded == 0xfffffffeu) return result;
    if (embedded == 0xffffffffu) {
        if (!stemwave_axis_reported) {
            stemwave_axis_reported=1u;
            framework->log_line("stem waveform: package axis mismatch, keeping native waveform");
        }
        return result;
    }
    if (embedded == count) {
        if(shape==WAVEFORM_3BAND)
            stemwave_decode_pwv7(columns, state->package_amplitudes, count);
        else
            stemwave_decode_columns(columns, state->package_amplitudes, count, shape);
        if (!stemwave_package_reported) {
            stemwave_package_reported=1u;
            framework->log_line("stem waveform: rendered embedded package waveform");
        }
    } else if (supported) {
        stemwave_render_columns(columns, count, shape, &style);
    } else {
        return result;
    }
    stemwave_scale_columns(columns, count, scale);
    __atomic_store_n(&state->columns, count, __ATOMIC_SEQ_CST);
    if (!(stemwave_reported_shapes & (1u << shape))) {
        stemwave_reported_shapes |= 1u << shape;
        framework->log_line("stem waveform: rendered a new shape");
    }
    return result;
}

static int stemwave_feature_configured(void)
{
    const char *setting = getenv("RX3_STEMS_DIR");
    stemwave_enabled = setting && setting[0];
    return stemwave_enabled;
}

static int stemwave_feature_install(void)
{
    /* This direct call needs its own guard before any waveform writes.
       Hardware validation is pending. */
    const char *stems = getenv("RX3_STEMS_DIR");
    if (!stems || !stems[0]) {
        framework->log_line("stemwave refused: stems provider not configured");
        return 0;
    }
    static const uint8_t renew_guard[8] = {0x56, 0x0e, 0x80, 0xe2, 0x01, 0x20, 0x41, 0xe2};
    if (!stemwave_enabled || memcmp((const void *)WAVEFORM_REQUEST_RENEW, renew_guard, 8u))
        return 0;
    original_ex_wave_renew_check = (ex_wave_renew_check_fn)framework->install_hook(
        &ex_wave_renew_hook, EX_WAVE_RENEW_CHECK, ex_wave_renew_guard,
        hooked_ex_wave_renew_check);
    if (!original_ex_wave_renew_check)
        return 0;
    framework->log_line("stem waveform: refresh hook installed");
    return 1;
}

static void stemwave_feature_remove(void)
{
    if (framework->uninstall_hook(&ex_wave_renew_hook))
        original_ex_wave_renew_check = 0;
}

static void stemwave_feature_track_did_load(unsigned int deck, void *reader,
                                           const void *track_info)
{
    (void)reader;
    (void)track_info;
    if (deck >= 2u)
        return;
    struct stemwave_deck *state = &stemwave_decks[deck];
    __atomic_store_n(&state->drawn_mask, STEMWAVE_MASK_INITIAL, __ATOMIC_SEQ_CST);
    __atomic_store_n(&state->pending, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&state->columns, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&state->blue_saved, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&state->ratio_x256, 0u, __ATOMIC_SEQ_CST);
    __sync_synchronize();
}

#endif /* RX3_STEMWAVE_FEATURE_H */

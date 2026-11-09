// SPDX-License-Identifier: MPL-2.0
/*
 * Modular two-deck performance core for the XDJ-RX3.
 *
 * The core is the sole broker for guarded inline hooks, deck identity, native
 * rendering, image tables and touch routing. Features are separate modules:
 * they reach the player only through the public services in api/, own their
 * state, assets and decisions, and can fail independently.
 *
 * The performance overlay clones an existing NS_GlyphText object, so text,
 * rectangles, fonts, and clipping stay inside rbp's own UI renderer.
 */

#define PCM_LOAD      ((unsigned long)0x00038ff0)
#define ON_KEY_HOT_CUE ((unsigned long)0x003030ec)
#define ON_KEY_BEAT_LOOP ((unsigned long)0x003031cc)
#define ON_KEY_SLIP_LOOP ((unsigned long)0x00303238)
#define ON_KEY_BEAT_JUMP ((unsigned long)0x00303294)
#define PAL_DRAW_TEXT  ((unsigned long)0x001d23e4)
#define PAL_DRAW_IMAGE ((unsigned long)0x001d3284)
#define SOLVE_TOUCH    ((unsigned long)0x002dc104)
#define BEATFX_XPAD_CTOR ((unsigned long)0x0035b0f8)
#define AUDIO_START    ((unsigned long)0x000447b8)
#define GET_HMI_MANAGER ((unsigned long)0x001d09b4)
#define REFRESH_GLYPH   ((unsigned long)0x001d07b0)
#define SET_BEATFX_STORAGE ((unsigned long)0x001331fc)

#define LOG_FILE  "/tmp/rx3-stems.log"
#define READY_FILE "/tmp/rx3-performance.ready"
#define RENDER_PROBE_FILE "/tmp/rx3-render-probe.log"
/* Events logged by RX3_KEY_TRACE: enough to flip a switch back and forth a
   few times even when nearby knobs jitter. */
#define KEY_TRACE_LIMIT 64u

#define RX3_DIAGNOSTIC_ONLY 0
#define RX3_PITCH_DIAGNOSTIC 1
#define BEATFX_LEFT_LAYER 0x1701u
#define XPAD_RIGHT_LAYER  0x1801u
#define HEADER_LAYER      0x0101u
#define PERFORMANCE_TAB_LAYER 0x0e01u

/* Private IDs above NS_GetImageCount() == 0x15cd. The image-info hook resolves
   only these IDs to private RGB565 records, so no Pioneer resource or cached
   DirectFB surface is overwritten. IDs 0x0d7c..0x0d84 were previously used
   here by mistake; static extraction proved that they are the live SOURCE
   Aqua/Blue/Default colour selector. */
#define TAB_IMAGE_KEY   0x1600u
#define TAB_IMAGE_STEMS 0x1601u
#define TAB_IMAGE_STATUS_NONE 0x1602u
#define TAB_IMAGE_KEY_NONE 0x1603u
#define TAB_IMAGE_BYTES 18000u
#define TAB_IMAGE_SAMPLES 0x1604u
#define TAB_IMAGE_SAMPLES_NONE 0x1605u
#define TAB_IMAGE_SAMPLES_BEATFX 0x1606u
#define TAB_IMAGE_SINGLE_KEY_NONE 0x1607u
#define TAB_IMAGE_SINGLE_KEY_SELECTED 0x1608u
#define TAB_IMAGE_SINGLE_STEMS_NONE 0x1609u
#define TAB_IMAGE_SINGLE_STEMS_SELECTED 0x160au
#define TAB_IMAGE_COUNT 11u
/* Share the STATUS / BEAT FX top border; stop touches before that lower row. */
#define CUSTOM_TAB_OFFSET_Y 21u
#define CUSTOM_TAB_TOUCH_BOTTOM 431
#define STOCK_IMAGE_COUNT RX3_STOCK_IMAGE_COUNT
/* The private ids: eleven for the tab strip, then the glyph atlas's reserve.
   This is one more than the largest id, and the guarded launch patch in
   mod/modules/core/module.sh writes that largest id into the player's
   own bound. The two move together, with tests/test_module_consistency.py
   pinning them to each other and to the image-info guard below. */
#define EXTENDED_IMAGE_COUNT 0x16a6u
#define IMAGE_TABLE_POINTER RX3_IMAGE_TABLE_POINTER

/* Types and libc declarations. */

#include "api/rx3_platform.h"
#include "services/rx3_hooks.h"
#include "firmware/rx3_patch.h"
#include "services/rx3_log.h"
#include "runtime/rx3_modules.h"
#include "api/rx3_module_api.h"
#include "services/rx3_panels.h"
#include "services/rx3_images.h"
#include "services/rx3_browse.h"
#include "services/rx3_input.h"
#include "services/rx3_audio.h"
#include "services/rx3_mix_state.h"

#include "diagnostics/rx3_probe_format.h"
#include "ui/rx3_pad_layout.h"

#define log_number rx3_log_number

/* The status slot: a registered panel with this ID replaces the native
   STATUS tab and its artwork. */
#define STATUS_PANEL 3u
/* Takes the BEAT FX tab when registered (the sampler's volume). */
#define BEATFX_PANEL 6u

typedef int (*load_fn)(void *, const void *);
typedef int (*on_key_pad_fn)(void *, const void *);
typedef void (*draw_text_fn)(void *, void *);
typedef void (*draw_image_fn)(void *, void *);
typedef void *(*image_info_fn)(unsigned int);
typedef void (*solve_touch_fn)(void *, const void *, const void *);
typedef void *(*beatfx_xpad_ctor_fn)(void *, void *);
typedef void (*audio_start_fn)(void *, void *);
typedef int (*audio_buffer_size_fn)(void *);
typedef double (*audio_sample_rate_fn)(void *);
typedef void (*set_beatfx_selected_fn)(int);
typedef int (*get_beatfx_selected_fn)(void);

/* Expected prologues, checked before writing executable code. */
static const uint8_t load_guard[8] = {
    0xf0, 0x4f, 0x2d, 0xe9, 0x5c, 0xd0, 0x4d, 0xe2
};
static const uint8_t hot_cue_guard[8] = {
    0x10, 0x40, 0x2d, 0xe9, 0x00, 0x40, 0xa0, 0xe1
};
static const uint8_t pad_mode_guard[8] = {
    0x0b, 0x30, 0xd1, 0xe5, 0x00, 0x20, 0xa0, 0xe1
};
static const uint8_t draw_text_guard[8] = {
    0xf0, 0x4f, 0x2d, 0xe9, 0x4d, 0xdf, 0x4d, 0xe2
};
static const uint8_t draw_image_guard[8] = {
    0xf0, 0x4f, 0x2d, 0xe9, 0xcc, 0xd0, 0x4d, 0xe2
};
static const uint8_t touch_guard[8] = {
    0xf0, 0x45, 0x2d, 0xe9, 0x02, 0x60, 0xa0, 0xe1
};
static const uint8_t beatfx_xpad_ctor_guard[8] = {
    0xf0, 0x4f, 0x2d, 0xe9, 0x7c, 0xd0, 0x4d, 0xe2
};
static const uint8_t audio_start_guard[8] = {
    0x00, 0x30, 0x91, 0xe5, 0xf0, 0x47, 0x2d, 0xe9
};
static const uint8_t set_beatfx_guard[8] = {
    0x98, 0x33, 0x0b, 0xe3, 0x16, 0x32, 0x40, 0xe3
};
static load_fn       original_load;
static on_key_pad_fn original_on_key_hot_cue;
static on_key_pad_fn original_on_key_beat_loop;
static on_key_pad_fn original_on_key_slip_loop;
static on_key_pad_fn original_on_key_beat_jump;

static volatile int state_thread_running;
static pthread_t state_thread;
static int state_thread_started;
static void *const *performance_view_pointer = (void *)0x0114c2d0u;
static volatile uint64_t overlay_drawn_us;
static volatile unsigned int captured_touch;
static unsigned int captured_touch_deck;
static volatile unsigned int overlay_panel;
static volatile unsigned int native_beatfx_selected;
static volatile unsigned long draw_calls;
static volatile unsigned long main_window_draws;
static volatile unsigned long image_draw_calls;
static volatile unsigned long custom_tab_draws;
static volatile unsigned long custom_pad_draws;
static volatile unsigned long touch_calls;
/* The render probe. The counters above say how much was drawn; the probe says
   what. One line per distinct draw, held in memory and written out by the
   watcher, because a draw hook that opens a file runs on the render thread. */
static int render_probe_enabled;
static void sicon_load(void);
static void sicon_report(void);
/* RX3_DRAW_TRACE=1 (diagnostica, DA_FIXARE/58): gli ultimi 4096 disegni d'immagine, in un anello; `touch /tmp/rx3-trace-now`
   li scrive in /tmp/rx3-draw-trace.log (dal watcher) quando cambia il contenuto di /tmp/rx3-trace-now (es. `date > /tmp/rx3-trace-now`). */
static int draw_trace_enabled;
#define DRAW_TRACE_N 4096u
static uint8_t draw_trace_ring[DRAW_TRACE_N][0x54];
static unsigned int draw_trace_head;
/* /tmp/rx3-fill-rects (diagnostica): righe "<alto layer> <x1> <y1> <x2> <y2> <rrggbb>" in coordinate di SCHERMO, riletto dal watcher;
   ogni riga e' un riquadro dipinto a ogni frame nella finestra (1<<8)|alto. Serve a trovare la finestra dell'onda senza riavviare. */
#define FILL_RECTS_MAX 32u
static struct { unsigned int high; int x1, y1, x2, y2; unsigned int rgb; } fill_rects[FILL_RECTS_MAX];
static volatile unsigned int fill_rect_count;   /* `touch /tmp/rx3-fill-test`: una fila di riquadri colorati in ogni finestra, per mappare le finestre sullo schermo */
#define PROBE_SEEN_MAX   1024u
#define PROBE_RING_BYTES 16384u
/* Three words identify a draw: a tagged layer, and two fields off the object. */
static unsigned int probe_seen[PROBE_SEEN_MAX][3];
static unsigned int probe_seen_count;
static char probe_ring[PROBE_RING_BYTES];
static unsigned int probe_ring_used;
static volatile unsigned int stock_tab_backing_ready;
static uint8_t stock_tab_backing[0x54];
static volatile unsigned int tab_assets_ready;
static volatile unsigned int tab_assets_installing;
/* The watcher holds the private image table back until this microsecond, so an
   operator can watch the stock tabs draw first and see which route installs.
   Zero means no delay, which is the ordinary case. */
static uint64_t tab_install_not_before_us;
static volatile unsigned int initial_performance_refresh_done;
static volatile unsigned int startup_message_queued;
static volatile unsigned long audio_start_calls;
static volatile uint8_t performance_window;
static volatile unsigned int performance_window_ready;
static volatile unsigned int text_template_ready;
static uint8_t text_template[0x54];
/* 0 none, 1 a plausible label, 2 a label that also carries a fill. */
static volatile unsigned int pad_text_template_ready;
static uint8_t pad_text_template[0x54];

struct touch_geometry {
    int x;
    int y;
    unsigned int width;
    unsigned int height;
};

static void *beatfx_touch_areas[6];
static struct touch_geometry stock_touch_geometry[6];
/* Which control is held, so the row can paint it pressed. -1 is nothing. */
static void *performance_left_glyph;
static void *performance_right_glyph;
static void *left_tab_glyph;
static void *right_tab_glyph;
static void *stock_status_glyph;
static volatile unsigned int beatfx_reselect_generation;
static volatile unsigned int beatfx_reselect_pending;
static volatile unsigned int beatfx_reselect_phase;
static volatile unsigned int beatfx_reselect_started_ms;

static const struct rx3_pad_row *row_for_id(unsigned int panel_id);

static struct installed_hook load_hook;
static struct installed_hook hot_cue_hook;
static struct installed_hook beat_loop_hook;
static struct installed_hook slip_loop_hook;
static struct installed_hook beat_jump_hook;
static struct installed_hook draw_text_hook;
static struct installed_hook draw_image_hook;
static struct installed_hook image_info_hook;
static struct installed_hook touch_hook;
static struct installed_hook beatfx_xpad_ctor_hook;
static struct installed_hook audio_start_hook;
static struct installed_hook set_beatfx_hook;
static draw_text_fn original_draw_text;
static draw_image_fn original_draw_image;
static image_info_fn original_image_info;
static solve_touch_fn original_solve_touch;
static beatfx_xpad_ctor_fn original_beatfx_xpad_ctor;
static audio_start_fn original_audio_start;
static set_beatfx_selected_fn original_set_beatfx_selected;
/* Logging. */

/* A drawable's layer, and the two object fields that separate one draw from
   another within a layer. Read here so the two record builders below agree. */
#define PROBE_LAYER_OFFSET  0x10u
#define PROBE_FIRST_OFFSET  0x28u
#define PROBE_SECOND_OFFSET 0x44u
#define PROBE_STYLE_OFFSET  0x38u
/* Text and images share one table, so the text side tags its key. */
#define PROBE_TEXT_TAG      0x10000u

static unsigned int probe_word(const void *object, unsigned int offset)
{
    unsigned int value;
    memcpy(&value, (const uint8_t *)object + offset, sizeof(value));
    return value;
}

static unsigned int probe_layer(const void *object)
{
    uint16_t value;
    memcpy(&value, (const uint8_t *)object + PROBE_LAYER_OFFSET, sizeof(value));
    return value;
}

/* Answers whether this draw has been recorded before. Once the table is full it
   stops growing and every unmatched draw records again, which is the reference's
   behaviour and is itself the signal that the run has more shapes than fit. */
static int probe_first_time(unsigned int key, unsigned int first, unsigned int second)
{
    for (unsigned int i = 0; i < probe_seen_count; i++)
        if (probe_seen[i][0] == key && probe_seen[i][1] == first &&
            probe_seen[i][2] == second)
            return 0;
    if (probe_seen_count < PROBE_SEEN_MAX) {
        probe_seen[probe_seen_count][0] = key;
        probe_seen[probe_seen_count][1] = first;
        probe_seen[probe_seen_count][2] = second;
        probe_seen_count++;
    }
    return 1;
}

/* A line that does not fit is dropped rather than truncated: half a rectangle
   reads as a rectangle. The ring stops growing until the watcher drains it. */
static void probe_append(const char *start, const char *end)
{
    unsigned int length = (unsigned int)(end - start);
    if (probe_ring_used + length + 1u > PROBE_RING_BYTES)
        return;
    if (length)
        memcpy(probe_ring + probe_ring_used, start, length);
    probe_ring[probe_ring_used + length] = '\n';
    probe_ring_used += length + 1u;
}

/* Worst case is the text record: two tag bytes, three hex words, a rectangle of
   four decimals, and one more decimal, with a separator between each. */
#define PROBE_RECORD_BYTES 128u

static void probe_record_image(const void *image)
{
    unsigned int layer = probe_layer(image);
    unsigned int second = probe_word(image, PROBE_SECOND_OFFSET);
    if (!probe_first_time(layer, second, 0u))
        return;
    char record[PROBE_RECORD_BYTES];
    char *out = record;
    *out++ = 'I';
    *out++ = ' ';
    out = probe_put_hex(out, layer);
    *out++ = ' ';
    out = probe_put_hex(out, second);
    *out++ = ' ';
    out = probe_put_rect(out, image);
    probe_append(record, out);
}

static void probe_record_text(const void *text)
{
    unsigned int layer = probe_layer(text);
    unsigned int first = probe_word(text, PROBE_FIRST_OFFSET);
    unsigned int second = probe_word(text, PROBE_SECOND_OFFSET);
    unsigned int style = ((const uint8_t *)text)[PROBE_STYLE_OFFSET];
    if (!probe_first_time(layer | PROBE_TEXT_TAG, first, second | (style << 24)))
        return;
    char record[PROBE_RECORD_BYTES];
    char *out = record;
    *out++ = 'T';
    *out++ = ' ';
    out = probe_put_hex(out, layer);
    *out++ = ' ';
    out = probe_put_hex(out, first);
    *out++ = ' ';
    out = probe_put_hex(out, second);
    *out++ = ' ';
    out = probe_put_rect(out, text);
    *out++ = ' ';
    out = probe_put_dec(out, style);
    probe_append(record, out);
}

/* Called from the watcher, never from a draw hook: this opens a file. */
static void probe_flush(void)
{
    if (!probe_ring_used)
        return;
    int fd = open(RENDER_PROBE_FILE, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0)
        return;
    (void)write(fd, probe_ring, probe_ring_used);
    close(fd);
    probe_ring_used = 0;
}








struct rx3_timeval { long seconds, microseconds; };

static uint64_t monotonic_enough_us(void)
{
    struct rx3_timeval value;
    if (gettimeofday(&value, 0))
        return 0;
    return (uint64_t)(unsigned long)value.seconds * 1000000u +
           (uint64_t)(unsigned long)value.microseconds;
}

/* juce::Time::currentTimeMillis, which the panel compares the LED blink
   against, is gettimeofday reduced to milliseconds. Reproducing it here keeps
   the on-screen toggles in the LED's own time domain. */
static unsigned int now_ms(void)
{
    return (unsigned int)(monotonic_enough_us() / 1000u);
}

static void refresh_performance_ui(void);
static int read_exactly(int fd, void *destination, size_t length);

static uint8_t tab_image_pixels[TAB_IMAGE_COUNT][TAB_IMAGE_BYTES];
static uint8_t light_tab_image_pixels[TAB_IMAGE_COUNT][TAB_IMAGE_BYTES];
static unsigned int light_tab_assets_ready;
static unsigned int tab_asset_attempts;
/* A refresh asks the player to repaint five glyphs, once. One repaint loses a
   race the player starts on its own: it redraws the stock row from a path of
   its own, our controls go with it, and nothing asks a second time, so a
   change made at the wrong moment flickers and reverts. Holding a deadline
   repeats the request for as long as the window lasts.

   The repeat is throttled rather than run on every intercepted draw. Painting
   the row once per draw was measured at 331 paints where 6 were wanted, so the
   window costs about eleven repaints instead of one, and not three hundred. */
static volatile unsigned int performance_refresh_pending;
/* The sampler page's own counters, declared again in ui/rx3_pad_page.h. */
static unsigned int pad_page_painted[2];
#define PERFORMANCE_REFRESH_WINDOW_US 1000000u
#define PERFORMANCE_REFRESH_EVERY_US   100000u
static uint64_t performance_refresh_until_us;
static uint64_t performance_refresh_next_us;
static unsigned int performance_refresh_reported;

#include "ui/rx3_pad_atlas.h"
#include "firmware/rx3_message.h"

static int load_tab_pixels(void)
{
    light_tab_assets_ready = 1u;
    for (unsigned int i = 0; i < TAB_IMAGE_COUNT; i++) {
        char dark_key[]="RX3_TAB_DARK_00",light_key[]="RX3_TAB_LIGHT_00";
        dark_key[sizeof(dark_key)-3u]=(char)('0'+i/10u);dark_key[sizeof(dark_key)-2u]=(char)('0'+i%10u);
        light_key[sizeof(light_key)-3u]=(char)('0'+i/10u);light_key[sizeof(light_key)-2u]=(char)('0'+i%10u);
        const char *path=getenv(dark_key);
        /* An absent module contributes no files, including from earlier runs. */
        if (!path || !path[0]) continue;
        int fd=open(path,O_RDONLY);
        if (fd<0 || read_exactly(fd,tab_image_pixels[i],TAB_IMAGE_BYTES)) {
            if (fd>=0) close(fd);
            log_line("warning: custom tab bitmap installation failed");
            return 0;
        }
        close(fd);
        path=getenv(light_key);
        fd=path && path[0] ? open(path,O_RDONLY) : -1;
        if (fd<0 || read_exactly(fd,light_tab_image_pixels[i],TAB_IMAGE_BYTES))
            light_tab_assets_ready=0u;
        if (fd>=0) close(fd);
    }

    return 1;
}

/* The light variant: a rebased copy of the finished table whose tab and pad
   glyph records point at the light artwork. Optional light tabs fall back to
   the dark ones: generic conversion cannot keep their selected-state contrast.
   The allocation stays resident for the life of the process. */
static uint8_t *build_light_table(uint8_t *dark_table)
{
    size_t bytes = EXTENDED_IMAGE_COUNT * 44u;
    uint8_t *light = mmap(0, bytes, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (light == MAP_FAILED) {
        log_line("warning: light image table allocation failed");
        return 0;
    }
    memcpy(light, dark_table, bytes);
    uint32_t rebase = (uint32_t)(unsigned long)dark_table -
                      (uint32_t)(unsigned long)light;
    for (unsigned int image = 0; image < EXTENDED_IMAGE_COUNT; image++) {
        uint8_t *record = light + image * 44u;
        uint32_t offset;
        memcpy(&offset, record + 0x20u, 4u);
        offset += rebase;
        memcpy(record + 0x20u, &offset, 4u);
        if (record[0x19u]) {
            memcpy(&offset, record + 0x24u, 4u);
            offset += rebase;
            memcpy(record + 0x24u, &offset, 4u);
        }
    }
    rx3_image_install_replacements(light, 1);
    for (unsigned int i = 0; i < TAB_IMAGE_COUNT; i++) {
        uint8_t *record = light + (TAB_IMAGE_KEY + i) * 44u;
        uint16_t width = 180u, height = 50u;
        const void *source = light_tab_assets_ready
            ? light_tab_image_pixels[i] : tab_image_pixels[i];
        uint32_t pixels = (uint32_t)(unsigned long)source -
                          (uint32_t)(unsigned long)light;
        uint32_t palette = 0;
        memcpy(record + 4u, &width, 2u);
        memcpy(record + 6u, &height, 2u);
        record[0x18u] = 2u;
        record[0x19u] = 0u;
        memcpy(record + 0x20u, &pixels, 4u);
        memcpy(record + 0x24u, &palette, 4u);
    }
    /* The pad row's glyphs, the same way: the light artwork keeps the dark
       set's image IDs and only this table points at it, so the row never has
       to choose an atlas. */
    if (pad_atlas_ready && pad_atlas_light_blob)
        pad_atlas_install_records(light, pad_atlas_light_blob);
    return light;
}

static void install_tab_assets(const char *route)
{
    if (tab_assets_ready)
        return;
    if (!__sync_bool_compare_and_swap(&tab_assets_installing, 0u, 1u))
        return;
    if (!load_tab_pixels()) {
        tab_assets_installing=0u;
        return;
    }

    pad_atlas_load();

    uint8_t *stock_table = *(uint8_t **)IMAGE_TABLE_POINTER;
    if (!stock_table) {
        tab_assets_installing = 0u;
        return;
    }
    size_t table_bytes = EXTENDED_IMAGE_COUNT * 44u;
    uint8_t *table = mmap(0, table_bytes, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (table == MAP_FAILED) {
        log_line("warning: private image table allocation failed");
        tab_assets_installing = 0u;
        return;
    }
    memcpy(table, stock_table, STOCK_IMAGE_COUNT * 44u);

    /* Stock records keep their pixel payloads in rbp's original allocation.
       Convert each relative offset so resolving it against the secondary
       table reaches the same original address. */
    uint32_t relocation = (uint32_t)(unsigned long)stock_table -
                          (uint32_t)(unsigned long)table;
    for (unsigned int image = 0; image < STOCK_IMAGE_COUNT; image++) {
        uint8_t *record = table + image * 44u;
        uint32_t data_offset;
        memcpy(&data_offset, record + 0x20u, sizeof(data_offset));
        data_offset += relocation;
        memcpy(record + 0x20u, &data_offset, sizeof(data_offset));
        if (record[0x19u]) {
            uint32_t palette_offset;
            memcpy(&palette_offset, record + 0x24u,
                   sizeof(palette_offset));
            palette_offset += relocation;
            memcpy(record + 0x24u, &palette_offset,
                   sizeof(palette_offset));
        }
    }

    for (unsigned int i = 0; i < TAB_IMAGE_COUNT; i++) {
        uint8_t *record = table + (TAB_IMAGE_KEY + i) * 44u;
        memcpy(record, table + 0x1598u * 44u, 44u);
        uint16_t width = 180u;
        uint16_t height = 50u;
        uint32_t pixels = (uint32_t)(unsigned long)tab_image_pixels[i] -
                          (uint32_t)(unsigned long)table;
        uint32_t no_palette = 0u;
        memcpy(record + 4u, &width, sizeof(width));
        memcpy(record + 6u, &height, sizeof(height));
        record[0x18u] = 1u; /* RGB565 */
        record[0x19u] = 0u;
        memcpy(record + 0x20u, &pixels, sizeof(pixels));
        memcpy(record + 0x24u, &no_palette, sizeof(no_palette));
    }

    if (pad_atlas_ready)
        pad_atlas_install_records(table, pad_atlas_blob);

    rx3_image_install_replacements(table, 0);
    /* The light interface is built from the finished table, so it inherits the
       replacements and the tab assets rather than needing its own copies. */
    uint8_t *light = rx3_image_variants_wanted() ? build_light_table(table) : 0;
    rx3_image_publish_tables(table, light, stock_table);

    __sync_synchronize();
    *(uint8_t **)IMAGE_TABLE_POINTER = table;
    __sync_synchronize();
    tab_assets_ready = 1u;
    /* Released on the way out as it is on every failure path. The ready latch
       above already turns later callers away, so holding the claim changed
       nothing; a flag that is only ever true after a success says the wrong
       thing to whoever adds the next route. */
    tab_assets_installing = 0u;
    log_line("extended private image table installed");
    if (light) log_line("light image table installed");
    log_line(route);
}

/* How fast the screen is redrawn. The header's text is drawn once per pass
   of the main window, so its count per second is the repaint rate (fps of
   the GUI, which draws only when something changed); image draws show how
   much each pass paints. Every second a sample; every ten the fastest one
   goes to the log, while the screen is busy, at most 60 lines. */
static void screen_rate_sample(unsigned int ticks)
{
    static unsigned long last_passes, last_images;
    static unsigned int best_passes, best_images, lines;
    if (ticks % 20u) return;
    unsigned long passes = main_window_draws, images = image_draw_calls;
    unsigned int pass_rate = (unsigned int)(passes - last_passes);
    unsigned int image_rate = (unsigned int)(images - last_images);
    last_passes = passes;
    last_images = images;
    if (pass_rate > best_passes) best_passes = pass_rate;
    if (image_rate > best_images) best_images = image_rate;
    if (ticks % 200u) return;
    if (best_passes > 2u && lines < 60u) {
        lines++;
        log_number("screen: repaints per second, best of 10 s = ", best_passes);
        log_number("screen: images drawn per second, best of 10 s = ", best_images);
    }
    best_passes = best_images = 0u;
}

static void *watch_patch_state(void *unused)
{
    (void)unused;
    unsigned int ticks = 0;
    unsigned int last_browse_pages=0;
    while (__atomic_load_n(&state_thread_running, __ATOMIC_SEQ_CST)) {
        usleep(50000u);
        if (ticks % 10u == 0u) (void)rx3_message_allowed();
        if (!tab_assets_ready &&
            (!tab_install_not_before_us ||
             monotonic_enough_us() >= tab_install_not_before_us))
            install_tab_assets("image table route: watcher thread");
        if (render_probe_enabled)
            probe_flush();
        if (draw_trace_enabled) {
            static char buf[2048];
            int fd = open("/tmp/rx3-fill-rects", O_RDONLY);
            unsigned int n = 0;
            if (fd >= 0) {
                ssize_t got = read(fd, buf, sizeof buf - 1);
                close(fd);
                if (got > 0) {
                    const char *c = buf, *end = buf + got;
                    while (c < end && n < FILL_RECTS_MAX) {
                        long v[6]; unsigned int k = 0;
                        while (c < end && *c != '\n' && k < 6u) {
                            long x = 0; int neg = 0, any = 0, hex = (k == 5u);
                            while (c < end && *c == ' ') c++;
                            if (c < end && *c == '-') { neg = 1; c++; }
                            while (c < end) {
                                int d = *c >= '0' && *c <= '9' ? *c - '0' : hex && *c >= 'a' && *c <= 'f' ? *c - 'a' + 10 : -1;
                                if (d < 0) break;
                                x = x * (hex ? 16 : 10) + d; any = 1; c++;
                            }
                            if (!any) break;
                            v[k++] = neg ? -x : x;
                        }
                        while (c < end && *c != '\n') c++;
                        if (c < end) c++;
                        if (k == 6u) {
                            fill_rects[n].high = (unsigned int)v[0]; fill_rects[n].x1 = (int)v[1]; fill_rects[n].y1 = (int)v[2];
                            fill_rects[n].x2 = (int)v[3]; fill_rects[n].y2 = (int)v[4]; fill_rects[n].rgb = (unsigned int)v[5]; n++;
                        }
                    }
                }
            }
            __atomic_store_n(&fill_rect_count, n, __ATOMIC_SEQ_CST);
        }
        static char trace_token[16];
        char token[16] = {0};
        int tfd = draw_trace_enabled ? open("/tmp/rx3-trace-now", O_RDONLY) : -1;
        if (tfd >= 0) { (void)read(tfd, token, sizeof token - 1); close(tfd); }
        if (tfd >= 0 && token[0] && memcmp(token, trace_token, sizeof token)) {
            memcpy(trace_token, token, sizeof token);
            static char text[DRAW_TRACE_N * 64u + 0x1000u * 260u];
            char *out = text;
            unsigned int head = draw_trace_head, n = head < DRAW_TRACE_N ? head : DRAW_TRACE_N, i;
            for (i = 0; i < n; i++) {
                const uint8_t *m = draw_trace_ring[(head - n + i) % DRAW_TRACE_N];
                out = probe_put_hex(out, probe_layer(m));
                *out++ = ' ';
                out = probe_put_hex(out, probe_word(m, PROBE_SECOND_OFFSET));
                *out++ = ' ';
                out = probe_put_rect(out, m);
                *out++ = '\n';
            }
            /* la tabella delle finestre del player (la stessa di draw_native_box_local): indice, puntatore e le prime parole dell'oggetto */
            {
                unsigned int w, k;
                for (w = 0; w < 0x1000u; w++) {
                    const uint32_t *obj = ((const uint32_t *const *)0x05a15f68u)[w];
                    if (((unsigned long)obj & 3u) || (unsigned long)obj < 0x02000000ul || (unsigned long)obj > 0x03000000ul) continue;
                    out = probe_put_hex(out, w);
                    *out++ = ' ';
                    out = probe_put_hex(out, (unsigned int)(unsigned long)obj);
                    *out++ = ':';
                    for (k = 0; k < 4u; k++) { *out++ = ' '; out = probe_put_hex(out, obj[k]); }
                    *out++ = '\n';
                }
            }
            int fd = open("/tmp/rx3-draw-trace.log", O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd >= 0) { (void)write(fd, text, (size_t)(out - text)); close(fd); }
        }
        /* Nothing else invalidates the pad windows while a row animates, so
           the row says when it needs the redraw; it decides the cadence. */
        const struct rx3_pad_row *row = row_for_id(overlay_panel);
        if (row && row->needs_refresh && row->needs_refresh() &&
            performance_window_ready)
            __atomic_store_n(&performance_refresh_pending, 1u, __ATOMIC_SEQ_CST);
        rx3_panel_poll_page();
        if (ticks % 20u == 0u) sicon_load();   /* ogni ~1 s: un file in RAM, nessun accesso alla chiavetta */
        if (ticks % 200u == 100u) sicon_report();
        ticks++;
        screen_rate_sample(ticks);
        if (RX3_PITCH_DIAGNOSTIC && ticks % 100u == 0u)
            rx3_modules_report();
        if(ticks%100u==0u) {
            struct rx3_browse_metrics m;
            rx3_browse_get_metrics(&m);
            if(m.pages!=last_browse_pages) {
                last_browse_pages=m.pages;
                log_number("browse pages = ",m.pages);
                log_number("browse native ms = ",m.native_ms);
                log_number("browse mod ms = ",m.added_ms);
                log_number("browse cache hits = ",m.hits);
                log_number("browse cache misses = ",m.misses);
                log_number("browse cache expiries = ",m.expiries);
                log_number("browse cache scopes = ",m.scopes);
                log_number("browse key queries = ",m.key_queries);
                log_number("browse field queries = ",m.field_queries);
                log_number("browse local metadata hits = ",m.local_hits);
                log_number("browse local metadata ms = ",m.local_ms);
                log_number("browse local metadata records = ",m.local_records);
            }
        }
        if (ticks == 100u) {
            log_number("probe text draw calls = ", draw_calls);
            log_number("probe main-window draws = ", main_window_draws);
            log_number("probe image draw calls = ", image_draw_calls);
            log_number("probe custom tab draws = ", custom_tab_draws);
            log_number("probe custom PAD draws = ", custom_pad_draws);
            log_number("probe touch calls = ", touch_calls);
            log_number("probe audio-start calls = ", audio_start_calls);
        }
        /* How often the player draws the RELEASE FX pictures at all, which is
           what says whether it keeps them out of the way while another page is
           up: five readings, ten seconds apart, of the cells painted over. */
        if (ticks % 200u == 0u && ticks <= 1000u && rx3_panel_page()) {
            log_number("probe sampler page cells painted, deck 1 = ", pad_page_painted[0]);
            log_number("probe sampler page cells painted, deck 2 = ", pad_page_painted[1]);
        }
    }
    return 0;
}

static unsigned int self_pid(void)
{
    char stat[16];
    int fd = open("/proc/self/stat", O_RDONLY);
    if (fd < 0) return 0;
    ssize_t count = read(fd, stat, sizeof(stat));
    close(fd);
    if (count < 2) return 0;
    unsigned int pid = 0;
    for (ssize_t i = 0; i < count; i++) {
        if (stat[i] == ' ') return pid;
        if (stat[i] < '0' || stat[i] > '9') return 0;
        pid = pid * 10u + (unsigned int)(stat[i] - '0');
    }
    return 0;
}

static void publish_ready(void)
{
    unsigned int value = self_pid();
    if (!value) return;
    int fd = open(READY_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return;
    /* Bind readiness to this process. A marker left by an earlier rbp must
       never be accepted as proof that the current process installed hooks. */
    char digits[16];
    unsigned int length = 0;
    do {
        digits[length++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value && length < sizeof(digits) - 1u);
    char marker[16];
    for (unsigned int i = 0; i < length; i++)
        marker[i] = digits[length - i - 1u];
    marker[length++] = '\n';
    (void)write(fd, marker, length);
    close(fd);
}

/* Artwork loading. */

static int read_exactly(int fd, void *destination, size_t length)
{
    uint8_t *cursor = destination;
    while (length) {
        ssize_t got = read(fd, cursor, length > 0x100000u ? 0x100000u : length);
        if (got <= 0)
            return -1;
        cursor += got;
        length -= (size_t)got;
    }
    return 0;
}

/* Hook installation. */

/* Native overlay. NS_PALRender_DrawText receives a fully attached 0x54-byte
   NS_GlyphText. Clone a stock label from the pane the row stands in for, retain
   its window/parent, and alter only the public text-box fields established by
   NS_GlyphText_CreateFromProperty. Cloning is what carries the font: nothing
   below sets one, so the controls wear whichever face the model was drawn in. */

static const uint16_t text_empty[]    = {0};

static const struct rx3_pad_row *row_for_id(unsigned int panel_id)
{
    return rx3_panel_find(panel_id);
}

static const struct rx3_pad_row *row_for_slot(unsigned int slot)
{
    return rx3_panel_find(slot + 1u);
}

static void set_u16(void *object, unsigned int offset, uint16_t value)
{
    memcpy((uint8_t *)object + offset, &value, sizeof(value));
}

static void set_u32(void *object, unsigned int offset, uint32_t value)
{
    memcpy((uint8_t *)object + offset, &value, sizeof(value));
}

/* Native surface fills use one byte per component, at 5/6/5 precision. */
static uint32_t pad_native_fill_colour(uint32_t rgb888)
{
    return ((rgb888 >> 19) & 31u) | (((rgb888 >> 10) & 63u) << 8) |
           (((rgb888 >> 3) & 31u) << 16);
}

/* The origin the last fill was placed from, and one to place the next fills
   from instead (rx3_pad_page.h). The pictures drawn at the same time are moved
   by the same difference: the player draws an object relative to the one it is
   drawing, so a repaint made from inside another object's draw lands off by the
   difference between the two (4 px up in the sampler page's cells). */
static int pad_fill_seen_x, pad_fill_seen_y;
static int pad_fill_origin_set, pad_fill_origin_x, pad_fill_origin_y;
static int pad_image_shift_x, pad_image_shift_y;

/* Where the draw in progress places its object. */
static void pad_native_origin(int *x, int *y)
{
    *x = *y = 0;
    ((void (*)(int *, int *))0x001d46f0u)(x, y);
}

static void draw_native_box_local(void *render, const void *model,
                                  const void *window_model,
                                  uint8_t target_window,
                                  int x1, int y1, int x2, int y2,
                                  const uint16_t *label,
                                  uint32_t foreground, uint32_t background)
{
    /* NS_PALRender_DrawText packs a 16-bit pixel for these windows, but
       DS_HW_Core_Surface_FillRectangle consumes three separate 5/6/5 bytes.
       Use the same native window/position and rectangle API directly. This
       avoids losing blue and folding its bits into green (rbp 1.19). */
    (void)render; (void)model; (void)label; (void)foreground;
    static int checked, supported;
    if (!checked) {
        static const uint32_t fill_guard[2] = {0xe92d4030u, 0xe24dd014u};
        static const uint32_t colour_guard[2] = {0xe59f3140u, 0xe92d4030u};
        static const uint32_t position_guard[2] = {0xe30831d0u, 0xe3403249u};
        supported = !memcmp((const void *)0x001a245cu,fill_guard,8) &&
                    !memcmp((const void *)0x001a0680u,colour_guard,8) &&
                    !memcmp((const void *)0x001d46f0u,position_guard,8);
        checked = 1;
        if (!supported) log_line("pad fill disabled: unexpected native drawing ABI");
    }
    if (!supported) return;
    uint16_t window_layer;
    memcpy(&window_layer,(const uint8_t *)window_model+0x10u,2);
    unsigned int index = ((unsigned int)target_window << 8) | (window_layer >> 8);
    void *window = ((void **)0x05a15f68u)[index];
    if (!window) return;
    int origin_x=0, origin_y=0;
    pad_native_origin(&origin_x, &origin_y);
    /* That origin is where the draw the player is in the middle of places its
       object, so a fill made from inside another object's draw lands off by
       the difference (4 px up, in the sampler page's cells). The page keeps the
       origin its own pictures were drawn with and has it used again. */
    pad_fill_seen_x = origin_x;
    pad_fill_seen_y = origin_y;
    if (pad_fill_origin_set) {
        origin_x = pad_fill_origin_x;
        origin_y = pad_fill_origin_y;
    }
    uint32_t colour = pad_native_fill_colour(background);
    ((int (*)(void *,unsigned int,uint32_t))0x001a0680u)(window,1u,colour);
    uint32_t size = (uint32_t)(x2-x1+1) | ((uint32_t)(y2-y1+1) << 16);
    ((int (*)(void *,int,int,uint32_t))0x001a245cu)(window,origin_x+x1,origin_y+y1,size);
}

/* The same clone, for artwork rather than a string.
 *
 * An image draw carries its id in the field a text draw uses for a background,
 * and the box places it: draw_custom_tabs has been putting a 180x50 tab on
 * screen this way since the tab strip was first replaced. The model to clone is
 * the pane backdrop the row is painted from, which is already attached to the
 * right renderer, window and subtree. */
static void draw_native_image_local(void *render, const void *model,
                                    uint8_t target_window,
                                    int x1, int y1, int x2, int y2,
                                    uint32_t image_id)
{
    uint8_t box[0x54];
    uint16_t window_layer;
    memcpy(box, model, sizeof(box));
    memcpy(&window_layer, (const uint8_t *)model + 0x10u, sizeof(window_layer));
    window_layer = (uint16_t)((window_layer & 0xff00u) | target_window);
    set_u16(box, 0x10, window_layer);
    set_u16(box, 0x18, (uint16_t)(x1 + pad_image_shift_x));
    set_u16(box, 0x1a, (uint16_t)(y1 + pad_image_shift_y));
    set_u16(box, 0x1c, (uint16_t)(x2 + pad_image_shift_x));
    set_u16(box, 0x1e, (uint16_t)(y2 + pad_image_shift_y));
    set_u32(box, 0x44, image_id);
    original_draw_image(render, box);
}

/* Compatibility marker for the pad-fill call; captions use the glyph atlas. */
#define PAD_COLOUR_INHERIT  0x00000000u

#include "ui/rx3_title_visibility.h"

static void refresh_initial_performance_tabs_if_ready(void)
{
    /* REFRESH_GLYPH must run from rbp's UI rendering path. Calling it from the
       state watcher can stall the renderer during startup. Every caller sets
       or captures a native glyph immediately before reaching this guard. */
    if (!initial_performance_refresh_done && tab_assets_ready &&
        stock_tab_backing_ready && (row_for_slot(0u) || row_for_slot(1u)) &&
        (left_tab_glyph || right_tab_glyph) && stock_status_glyph) {
        initial_performance_refresh_done = 1u;
        refresh_performance_ui();
        log_line("initial native performance tabs refreshed");
    }
}

/* Every native text the core passes on goes through the image service, which
   lets the display-mode policy choose its ink while the light table is shown. */
static void draw_text_themed(void *render, void *text)
{
    rx3_image_draw_text(original_draw_text, render, text);
}

/* A browse row's fill (P8, the Camelot pill): the glyph's own window, whose
   number is the low byte of the window-layer word, and its coordinates. */
static void draw_browse_fill(void *render, const void *model, unsigned int x1,
                             unsigned int y1, unsigned int x2, unsigned int y2,
                             uint32_t rgb888)
{
    uint16_t window_layer;
    memcpy(&window_layer, (const uint8_t *)model + 0x10u, sizeof(window_layer));
    draw_native_box_local(render, model, model, (uint8_t)(window_layer & 0xffu),
                          (int)x1, (int)y1, (int)x2, (int)y2, 0, 0, rgb888);
}

/* The module's status mark (netstatus). Its rectangles go to the window of the object being drawn
   (the layer's low byte is the window, as for every native fill here). It is drawn from one place,
   right after a stock image in the top-right of the bar on the player/browser screens
   (indicator_image_point). The text hooks only serve the diagnostic. */
#include "ui/rx3_header_bar.h"
static void indicator_rect(void *context, int x1, int y1, int x2, int y2, uint32_t rgb888)
{
    const uint8_t *model = ((void **)context)[1];
    uint16_t layer;
    memcpy(&layer, model + 0x10u, sizeof layer);
    draw_native_box_local(((void **)context)[0], model, model, (uint8_t)(layer & 0xffu), x1, y1, x2, y2, 0, 0, rgb888);
}
static void draw_status_indicator(void *render, const void *model)
{
    void *context[2] = { render, (void *)model };
    rx3_indicator_draw(indicator_rect, context, rx3_images.light_active());
    rx3_header_bar_draw(indicator_rect, context, rx3_images.light_active(), rx3_header_progress());
}
/* netstatus-prova: big coloured squares from the module, per native window layer, and a log of
   which layers draw text and how often. Off unless the module turns it on. */
#define PROBE_LAYERS 10u
static struct { uint16_t layer; unsigned long calls; uint64_t drawn_us; } probe_layers[PROBE_LAYERS];
static unsigned int probe_layer_count;
static uint64_t probe_log_us;
static void indicator_probe_text(void *render, const uint8_t *glyph, uint16_t layer)
{
    if (!rx3_indicator_probe_enabled()) return;
    unsigned int i = 0;
    while (i < probe_layer_count && probe_layers[i].layer != layer) i++;
    if (i == probe_layer_count) {
        if (i >= PROBE_LAYERS) return;
        probe_layers[i].layer = layer;
        probe_layer_count++;
    }
    uint64_t now = monotonic_enough_us();
    probe_layers[i].calls++;
    if (!probe_log_us) probe_log_us = now;
    if (now - probe_log_us >= 5000000u) {
        unsigned long ms = (unsigned long)((now - probe_log_us) / 1000u);
        for (unsigned int k = 0; k < probe_layer_count; k++) {
            log_number("netstatus probe: layer index", k);
            log_number("  layer word =", probe_layers[k].layer);
            log_number("  text draws per second =", probe_layers[k].calls * 1000u / ms);
            probe_layers[k].calls = 0;
        }
        probe_log_us = now;
    }
    if (now - probe_layers[i].drawn_us < 50000u) return;
    probe_layers[i].drawn_us = now;
    uint16_t box[2];
    memcpy(box, glyph + 0x18u, sizeof box);
    void *context[2] = { render, (void *)glyph };
    rx3_indicator_probe(indicator_rect, context, i, box[0], box[1], layer == HEADER_LAYER ? 1u : 0u);
}

/* Temporarily use the overview strip inside the native header window.
 * Use its own glyph model so UTF-16, font, clipping and theme remain native. */
static void draw_message_header(void *render,const void *model)
{
    if(!message_display_active && !message_display_dirty)return;
    uint32_t clone[21];memcpy(clone,model,sizeof(clone));
    uint8_t *text=(void *)clone;uint16_t box[4]={194,8,630,41};
    memcpy(text+0x18,box,sizeof(box));
    draw_native_box_local(render,model,model,1,194,8,630,41,0,0,0);
    if(message_display_active) {
        uint32_t pointer=(uint32_t)(unsigned long)message_display_text;
        memcpy(text+0x34,&pointer,4);
        unsigned n=0;while(n<95 && message_display_text[n])n++;
        text[0x38]=(uint8_t)n;
        draw_text_themed(render,text);
    }
    message_display_dirty=0;
}

int rx3_loader_busy(void);

/* The notice stays while the mod is still loading (the loader's queue: sample
   banks, the snare, stems), re-posted before it runs out, so nobody starts a
   set on a half-ready deck (RX3, 2026-10-15). */
static void keep_startup_message(void)
{
    static uint64_t shown_ms, posted_ms;
    static unsigned int done;
    if (!startup_message_queued || done) return;
    uint64_t now = monotonic_enough_us() / 1000u;
    if (!shown_ms) shown_ms = posted_ms = now;
    if (!rx3_loader_busy() && now - shown_ms >= 6000u) {
        done = 1u;
        log_line("messages: mod ready, loading notice ends");
        return;
    }
    if (now - posted_ms >= RX3_MESSAGE_HOLD_US / 1000u - 1000u) {
        posted_ms = now;
        (void)rx3_message_show(0u, &message_drive_loading);
    }
}

static void show_startup_message_when_ready(void)
{
    keep_startup_message();
    if (startup_message_queued || !rx3_message_allowed() ||
        !rx3_message_call_site_ready())
        return;
    if (rx3_message_show(0u, &message_drive_loading) == RX3_NOTICE_OK) {
        startup_message_queued = 1u;
        log_line("messages: startup notice queued");
        rx3_message_run_pending();
    }
}

static uint32_t performance_tab_image(const struct rx3_pad_row *left,
                                      const struct rx3_pad_row *right,
                                      unsigned int panel)
{
    if (!left && !right) return 0u;
    if (left && right) {
        const struct rx3_pad_row *active = panel == left->panel_id ? left :
                                           panel == right->panel_id ? right : 0;
        return active ? active->tab_image : TAB_IMAGE_KEY_NONE;
    }
    if (left) return panel == left->panel_id ?
                     TAB_IMAGE_SINGLE_KEY_SELECTED : TAB_IMAGE_SINGLE_KEY_NONE;
    return panel == right->panel_id ?
           TAB_IMAGE_SINGLE_STEMS_SELECTED : TAB_IMAGE_SINGLE_STEMS_NONE;
}

static uint32_t performance_status_image(unsigned int status_panel,
                                         unsigned int panel,
                                         uint32_t native_image)
{
    if (!status_panel) return panel ? TAB_IMAGE_STATUS_NONE : native_image;
    if (panel == STATUS_PANEL) return TAB_IMAGE_SAMPLES;
    if (panel == BEATFX_PANEL) return TAB_IMAGE_SAMPLES_BEATFX;
    if (!panel && native_image == 0x1598u) return TAB_IMAGE_SAMPLES_BEATFX;
    return TAB_IMAGE_SAMPLES_NONE;
}


/* The tab strip. The stock STATUS / BEAT FX row is captured as it goes past --
   see the image id below -- and redrawn at the KEY / STEMS position, so the
   frame, the corner radius and the palette are rbp's own rather than a
   reconstruction. Drawing the boxes by hand was tried and rejected: the theme
   colours are not exposed, and every literal guess looked foreign. */
static void draw_custom_tabs(void *render, const void *image)
{
    const struct rx3_pad_row *left = row_for_slot(0u);
    const struct rx3_pad_row *right = row_for_slot(1u);
    uint32_t tab_image = performance_tab_image(left,right,overlay_panel);
    if (!stock_tab_backing_ready || !tab_image) {
        original_draw_image(render, (void *)image);
        return;
    }
    uint16_t window_layer;
    memcpy(&window_layer, (const uint8_t *)image + 0x10u, sizeof(window_layer));

    uint8_t backing[0x54];
    memcpy(backing, stock_tab_backing, sizeof(backing));
    if (tab_assets_ready) set_u32(backing, 0x44, tab_image);
    set_u16(backing, 0x10, window_layer);
    set_u16(backing, 0x18, 10u);
    set_u16(backing, 0x1a, CUSTOM_TAB_OFFSET_Y);
    set_u16(backing, 0x1c, 190u);
    set_u16(backing, 0x1e, 50u + CUSTOM_TAB_OFFSET_Y);
    original_draw_image(render, backing);
    custom_tab_draws++;
}

/* The model a filled box is cut from.
 *
 * It no longer carries the row's typeface: the lettering is artwork now, and
 * nothing about a cloned text object ever selected a face anyway. What is left
 * is a model with the right renderer and window attachment to place a
 * rectangle with, so either template will do, and neither being captured yet
 * means there is nothing to cut from and the caller says so. */
static const void *pad_button_face(void)
{
    if (pad_text_template_ready)
        return pad_text_template;
    return text_template_ready ? text_template : 0;
}

#include "ui/rx3_pad_widgets.h"
#include "ui/rx3_pad_page.h"

static void draw_custom_pad_half(void *render, const void *model,
                                 uint8_t window, unsigned int deck)
{
    const struct rx3_pad_row *row = row_for_id(overlay_panel);
    if (!row)
        return;
    custom_pad_draws++;
    /* A display-mode fill policy would repaint the ground under everything
       drawn here, so it stands down for the length of the row. */
    rx3_image_suspend_fill(1);
    pad_row_paint(render, model, window, deck, row);
    rx3_image_suspend_fill(0);
}



/* The row's own subtree. The low byte of a window-layer is the deck's window,
   which decks 1 and 2 share with their info strips; the high byte names the
   widget subtree, which is what "the pads" actually means. Keying on the low
   byte swallowed AUTO CUE, QUANTIZE and the tempo badges along with the pads. */
static int is_performance_pad_subtree(uint16_t window_layer)
{
    unsigned int subtree = (unsigned int)(window_layer >> 8);
    return subtree == 0x17u || subtree == 0x18u;
}

static unsigned int deck_for_pad_subtree(uint16_t window_layer)
{
    return (unsigned int)(window_layer >> 8) == 0x17u ? 0u : 1u;
}

/* Clone a stock label from that subtree so the controls inherit the row's font,
   padding and clipping. Take the first plausible label, then upgrade once to
   one that also carries a fill, which is a real button rather than a caption.
   Capturing before the interception below is what makes this work at all: the
   draws worth cloning are exactly the ones a live panel replaces. */
/* Which subtree to clone the control face from.
   The pad subtree draws no text at all -- measured: twelve subtrees issue text
   draws and 0x17/0x18 are not among them, because that row is images. So there
   is no stock pad label to clone and the donor has to be chosen from elsewhere.
   0 keeps the original behaviour of waiting for a pad label that never comes.
   The knob that let an emulator try other subtrees, and the one that widened
   the height search below, left with the emulator; a deck never set either. */
static unsigned int font_donor_subtree(void)
{
    return 0u;
}

/* Selecting a donor by layer picked four faces that all render at 19 px,
   because a layer draws text at more than one size. Selecting by measured box
   height searches the actual range instead. */
static int font_donor_max_height(void)
{
    return 64;
}

static void capture_pad_text_template(const void *text, uint16_t window_layer)
{
    unsigned int donor = font_donor_subtree();
    if (pad_text_template_ready >= 2u)
        return;
    if (donor ? ((unsigned int)(window_layer >> 8) != donor)
              : !is_performance_pad_subtree(window_layer))
        return;
    if (((const uint8_t *)text)[0x38] == 0u)
        return;
    uint16_t y1, y2;
    memcpy(&y1, (const uint8_t *)text + 0x1au, sizeof(y1));
    memcpy(&y2, (const uint8_t *)text + 0x1eu, sizeof(y2));
    int height = (int)y2 - (int)y1;
    if (height < 8 || height > font_donor_max_height())
        return;
    uint32_t background;
    memcpy(&background, (const uint8_t *)text + 0x44u, sizeof(background));
    unsigned int level = background ? 2u : 1u;
    if (level <= pad_text_template_ready)
        return;
    memcpy(pad_text_template, text, sizeof(pad_text_template));
    pad_text_template_ready = level;
    log_number("pad label template captured, level = ", level);
    log_number("  donor layer = ", (unsigned long)window_layer);
    log_number("  donor box height = ", (unsigned long)height);
}

static void *hooked_image_info(unsigned int image_id)
{
    if (!tab_assets_ready && tab_asset_attempts < 8u) {
        if (*(uint8_t **)IMAGE_TABLE_POINTER)
            tab_asset_attempts++;
        install_tab_assets("image table route: image lookup hook");
    }
    void *variant=rx3_image_resolve(image_id,original_image_info,*(void **)IMAGE_TABLE_POINTER);
    return variant ? variant : original_image_info(image_id);
}

static void select_custom_panel(unsigned int panel);
static void restore_status(void);
static void pump_beatfx_reselect(void);

static void indicator_refresh_if_asked(void);
static void run_pending_ui(void)
{
    indicator_refresh_if_asked();
    title_refresh_pending();
    pump_beatfx_reselect();
    unsigned int requested_panel = rx3_panel_take_open();
    /* A close request is honoured only while a custom panel is shown; an
       open request made in the same pass wins, so a toggle never flickers. */
    if (rx3_panel_take_close() && overlay_panel && !requested_panel) restore_status();
    if (requested_panel) select_custom_panel(requested_panel);
    unsigned int refresh_requested = __atomic_exchange_n(&performance_refresh_pending, 0u, __ATOMIC_SEQ_CST);
    /* Module refresh windows must never repeatedly clear native BEAT FX or
       STATUS layers after their custom panel has closed. */
    if (!overlay_panel) {
        performance_refresh_until_us = 0;
        performance_refresh_next_us = 0;
    } else if (refresh_requested) {
        uint64_t now = monotonic_enough_us();
        performance_refresh_until_us = now + PERFORMANCE_REFRESH_WINDOW_US;
        performance_refresh_next_us = now + PERFORMANCE_REFRESH_EVERY_US;
        refresh_performance_ui();
    } else if (performance_refresh_until_us) {
        uint64_t now = monotonic_enough_us();
        if (now > performance_refresh_until_us) {
            performance_refresh_until_us = 0;
            if (render_probe_enabled && !performance_refresh_reported) {
                performance_refresh_reported = 1u;
                log_line("probe: the performance row held its refresh window");
            }
        } else if (now >= performance_refresh_next_us) {
            performance_refresh_next_us = now + PERFORMANCE_REFRESH_EVERY_US;
            refresh_performance_ui();
        }
    }
    rx3_message_run_pending();
}

/* DS_GR_UpdateScreen (rbp 1.20 @0x1a2a34): il player compone lo schermo dopo aver disegnato tutte le finestre. Qui, prima della
   composizione, si dipinge nelle finestre del player (DS_GR_SetWindowColor + DS_GR_FillRect, coordinate della finestra): l'unico
   momento in cui l'onda dettagliata (finestra 0x112, disegnata fuori da draw_image) e' gia' disegnata e non ancora sullo schermo. */
#define GR_UPDATE_SCREEN ((unsigned long)0x001a2a34)
static const uint8_t gr_update_guard[8] = {0x08, 0x40, 0x2d, 0xe9, 0x1c, 0x30, 0x9f, 0xe5};
typedef int (*gr_update_fn)(void *);
static struct installed_hook gr_update_hook;
static gr_update_fn original_gr_update;
static void gr_fill(unsigned int index, int x, int y, int w, int h, uint32_t rgb)
{
    void *window;
    if (w <= 0 || h <= 0 || index >= 0x1000u) return;
    window = ((void **)0x05a15f68u)[index];
    if (!window) return;
    ((int (*)(void *, unsigned int, uint32_t))0x001a0680u)(window, 1u, pad_native_fill_colour(rgb));
    ((int (*)(void *, int, int, uint32_t))0x001a245cu)(window, x, y, (uint32_t)w | ((uint32_t)h << 16));
}
/* Onda del deck (buffer RGB565 892 x 444, deck 1 righe 0..221, deck 2 222..443): ui_PLAYMODE_Set (@0x24fb80) la disegna, la mod
   puo' ritoccarla subito dopo. In modalita' giorno la linea di riproduzione (x 220) usciva bianca sul fondo bianco, invisibile:
   qui diventa nera piena, 3 px, su tutta l'altezza di ogni deck. Notte: invariata. RX3_BJWAVE=0 toglie l'aggancio. */
static int bjwave_enabled(void) { const char *v = getenv("RX3_BJWAVE"); return !(v && v[0] == '0'); }
#define WAVE_G ((unsigned long)0x02498c88)
#define WAVE_W 892
#define WAVE_H 444
#define WAVE_DECK_H 222
#define WAVE_PITCH 892
#define WAVE_HEAD_X 220
#define PLAYMODE_SET ((unsigned long)0x0024fb80)
static const uint8_t playmode_set_guard[8] = {0xf0, 0x4f, 0x2d, 0xe9, 0x04, 0x8b, 0x2d, 0xed};
typedef int (*playmode_set_fn)(int, int, int, int);
static struct installed_hook playmode_set_hook;
static playmode_set_fn original_playmode_set;
#define C565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#include "ui/rx3_wave_head.h"
static void wave_head_line_light(void)
{
    if (!rx3_images.light_active() || !*(void *const *)(WAVE_G + 188u)) return;
    uint16_t *buf = *(uint16_t *const *)(WAVE_G + 64u);
    if (!buf) return;
    rx3_wave_head_paint(buf, WAVE_PITCH, 2, WAVE_DECK_H, WAVE_HEAD_X, 3);
}
static int hooked_playmode_set(int a, int b, int c, int d);
#ifdef RX3_DEVELOPER
#include "services/rx3_bjwave_model.h"
/* Voce SULL'ONDA DETTAGLIATA (DA_FIXARE/58, solo instabile; il righello beat jump e' stato tolto su richiesta dell'utente).
   L'onda e' una finestra DS a parte (non nella tabella delle finestre NS): ui_PLAYMODE_Set (@0x24fb80) la blocca ([G+188],
   G = 0x02498c88), azzera il buffer ([G+64], RGB565, 892 x 444: deck 1 righe 0..221, deck 2 222..443, 1784 byte per riga),
   ci disegna colonne, tacche dei battiti e cue, e la sblocca. Subito dopo, la mod dipinge nello stesso buffer la striscia "vocal"
   (fascia delle tacche basse, senza coprire le tacche rosse) dove le colonne VOCAL del .rx3stem del deck dicono che c'e' voce, in scala
   con i battiti misurati dalle tacche che il player ha appena disegnato. Deck 1 e 2, senza stems niente. RX3_BJWAVE=0 la spegne. */
static const char bjwave_chars[] = "-0123456789xvocal";
static const uint8_t bjwave_font[][5] = {
    {0,0,7,0,0},{7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},{7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},{7,5,7,5,7},{7,5,7,1,7},
    {0,5,2,5,0},{0,5,5,5,2},{0,7,5,5,7},{0,7,4,4,7},{0,6,1,7,7},{6,2,2,2,7}};
static void wave_rect(uint16_t *buf, int x, int y, int w, int h, uint16_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (x + w > WAVE_W) w = WAVE_W - x;
    if (y < 0) { h += y; y = 0; }
    if (y + h > WAVE_H) h = WAVE_H - y;
    for (int r = 0; r < h; r++) { uint16_t *p = buf + (y + r) * WAVE_PITCH + x; for (int i = 0; i < w; i++) p[i] = c; }
}
static void wave_text(uint16_t *buf, int x, int y, const char *s, uint16_t c, int sc)
{
    for (; *s; s++, x += 4 * sc) {
        int g = -1;
        for (int i = 0; bjwave_chars[i]; i++) if (bjwave_chars[i] == *s) { g = i; break; }
        if (g < 0) continue;
        for (int r = 0; r < 5; r++)
            for (int k = 0; k < 3; k++)
                if (bjwave_font[g][r] & (4 >> k)) wave_rect(buf, x + k * sc, y + r * sc, sc, sc, c);
    }
}
/* Pixel per battito (x16), dalle tacche rosse delle battute (ogni 4 battiti) che il player ha appena disegnato nella fascia
   alta del deck: la distanza piu' frequente tra due tacche rosse vicine, diviso 4. Le bianche (battiti) non sempre ci sono:
   dipende dallo zoom. 0 se non si trova. */
#define WAVE_RED(p) (((p) >> 11) >= 20u && (((p) >> 5) & 63u) < 24u && ((p) & 31u) < 12u)
#define WAVE_OURS(p) ((p) == C565(0xff, 0x7a, 0x00) || (p) == C565(0xd9, 0x66, 0x00) || (p) == C565(0xff, 0xc3, 0x80))
static unsigned int wave_measure_ppb16(const uint16_t *buf, int y0)
{
    for (int y = y0 + 14; y < y0 + 26; y++) {
        const uint16_t *row = buf + y * WAVE_PITCH;
        int starts[64], n = 0;
        for (int x = 1; x < WAVE_W && n < 64; x++) {
            int on = WAVE_RED(row[x]) && !WAVE_OURS(row[x]), before = WAVE_RED(row[x - 1]) && !WAVE_OURS(row[x - 1]);
            if (on && !before) starts[n++] = x;
        }
        if (n < 2) continue;
        int best = 0, best_count = 0;
        for (int i = 1; i < n; i++) {
            int d = starts[i] - starts[i - 1], count = 0;
            if (d < 8) continue;
            for (int j = 1; j < n; j++) { int e = starts[j] - starts[j - 1]; if (e >= d - 1 && e <= d + 1) count++; }
            if (count > best_count || (count == best_count && d < best)) { best_count = count; best = d; }
        }
        if (!best) continue;
        int first = -1, last = -1, steps = 0;
        for (int i = 1; i < n; i++) { int d = starts[i] - starts[i - 1]; if (d >= best - 1 && d <= best + 1) { if (first < 0) first = starts[i - 1]; last = starts[i]; steps++; } }
        return steps ? ((unsigned int)((last - first) * 16) / (unsigned int)(steps * 4))   /* last > first, steps > 0: unsigned, no __aeabi_idiv */ : (unsigned int)best * 4u;
    }
    return 0;
}
/* --- voce: colonne del ruolo VOCAL del .rx3stem del deck (servizio del core rx3_wave_read, 150 colonne al secondo, 1 byte),
   lette una volta per brano; posizione e griglia del deck come il Roll (samples/rx3_samples_roll.h). Senza stems: niente striscia.
   RX3_BJWAVE_VOICE_TEST=1 (solo per provare il disegno in emulatore, dove non ci sono stems): voce finta a blocchi di 16 battiti. */
#define BJW_COLS_MAX 540000u
#define BJW_GET_ENGINE ((unsigned long)0x000b5e94)
#define BJW_BEAT_INFO ((unsigned long)0x0005f5bc)
#define BJW_POSITION ((unsigned long)0x0005d950)
static const uint8_t bjw_engine_guard[8] = {0xf8,0x40,0x2d,0xe9,0x9c,0x40,0x9f,0xe5};
static const uint8_t bjw_info_guard[8] = {0x0c,0x30,0x90,0xe5,0x10,0x20,0x90,0xe5};
static const uint8_t bjw_position_guard[8] = {0x02,0x00,0x51,0xe3,0x10,0x40,0x2d,0xe9};
static uint8_t *bjw_cols[2];
static uint32_t bjw_count[2];
static unsigned int bjw_threshold[2];
static int bjw_grid(unsigned int deck, bjw_i64 *pos_ms, bjw_i64 *anchor_ms, uint32_t *beat_us)
{
    static int ok = -1;
    if (ok < 0) ok = !memcmp((const void *)BJW_GET_ENGINE, bjw_engine_guard, 8) &&
                     !memcmp((const void *)BJW_BEAT_INFO, bjw_info_guard, 8) &&
                     !memcmp((const void *)BJW_POSITION, bjw_position_guard, 8);
    if (!ok) return 0;
    void *engine = ((void *(*)(void *))BJW_GET_ENGINE)(0);
    if (!engine) return 0;
    const uint8_t *info = ((const uint8_t *(*)(void *, unsigned int))BJW_BEAT_INFO)(engine, deck);
    if (!info || !*(const uint32_t *)(info + 0x24)) return 0;
    long offset = *(const long *)(info + 0x28);
    const uint32_t *first = *(const uint32_t *const *)(info + 0x34), *last = *(const uint32_t *const *)(info + 0x38);
    if (!first || last <= first + 2) return 0;
    unsigned long count = (unsigned long)(last - first) / 2u, low = 0, high;
    long played = ((long (*)(void *, unsigned int))BJW_POSITION)(engine, deck);
    bjw_i64 audio_ms = bjw_div((bjw_i64)played * 10, 441);                 /* campioni a 44,1 kHz -> ms */
    bjw_i64 ms = audio_ms - offset;
    if (ms < (bjw_i64)first[1]) return 0;
    high = count - 1u;
    while (low < high) { unsigned long mid = (low + high + 1u) / 2u; if ((bjw_i64)first[mid * 2u + 1u] <= ms) low = mid; else high = mid - 1u; }
    unsigned int bpm = first[low * 2u] >> 16;                          /* BPM x 100 */
    if (bpm < 2000u || bpm > 60000u) return 0;
    *pos_ms = audio_ms;
    *anchor_ms = (bjw_i64)first[low * 2u + 1u] + offset;
    *beat_us = (uint32_t)(6000000000ull / bpm);
    return 1;
}
static int bjw_voice_columns(unsigned int deck)
{
    const char *test = getenv("RX3_BJWAVE_VOICE_TEST");
    const uint8_t *player = (const uint8_t *)(0x03275930ul + deck * 0x0001e8a0ul);
    const uint32_t *count_pointer = *(const uint32_t *const *)(player + 0x14u);
    uint32_t count = count_pointer ? *count_pointer : 0u;
    if (!count || count > BJW_COLS_MAX) return 0;
    if (count == bjw_count[deck] && bjw_cols[deck]) return 1;
    if (!bjw_cols[deck]) {
        void *m = mmap(0, BJW_COLS_MAX, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == (void *)-1) return 0;
        bjw_cols[deck] = m;
    }
    unsigned int got = rx3_wave_read(deck, 2u /* VOCAL */, 0u, bjw_cols[deck], count), peak = 0;
    if (got != count) {
        if (!(test && test[0] == '1')) { bjw_count[deck] = 0; return 0; }
        for (uint32_t i = 0; i < count; i++) bjw_cols[deck][i] = ((i / (150u * 8u)) % 2u) ? 24u : 2u;  /* finta: 8 s si', 8 s no */
    }
    for (uint32_t i = 0; i < count; i++) if (bjw_cols[deck][i] > peak) peak = bjw_cols[deck][i];
    if (!peak) { bjw_count[deck] = 0; return 0; }
    bjw_threshold[deck] = peak / 3u ? peak / 3u : 1u;
    bjw_count[deck] = count;
    log_number(got == count ? "beat jump bar: VOCAL columns = " : "beat jump bar: TEST voice columns = ", count);
    return 1;
}
static void bjwave_wave_paint(void)
{
    static int checked, on;
    if (!checked) { on = bjwave_enabled(); checked = 1;
                    log_line(on ? "vocal on the waveform: on" : "vocal on the waveform: off"); }
    if (!on || !*(void *const *)(WAVE_G + 188u)) return;
    uint16_t *buf = *(uint16_t *const *)(WAVE_G + 64u);
    if (!buf) return;
    for (unsigned int deck = 0; deck < 2u; deck++) {
        if (!bjw_voice_columns(deck)) continue;          /* senza stems con colonne VOCAL: niente */
        int y0 = (int)deck * WAVE_DECK_H;
        static unsigned int ppb_cache[2];
        unsigned int ppb16 = wave_measure_ppb16(buf, y0);
        if (ppb16) ppb_cache[deck] = ppb16; else ppb16 = ppb_cache[deck];
        if (!ppb16) continue;
        bjw_i64 pos = 0, anchor = 0; uint32_t beat_us = 0;
        struct bjwave_run runs[BJWAVE_MAX_RUNS];
        if (!bjw_grid(deck, &pos, &anchor, &beat_us)) continue;
        unsigned int nr = bjwave_voice_runs(runs, bjw_cols[deck], bjw_count[deck], 150u, anchor, beat_us,
                                            pos, WAVE_HEAD_X, ppb16, WAVE_W, bjw_threshold[deck]);
        /* striscia nella fascia delle tacche basse: le tacche rosse delle battute restano visibili sopra la striscia */
        const int sy = y0 + WAVE_DECK_H - 14, sh = 12;
        for (unsigned int r = 0; r < nr; r++) {
            int x0 = runs[r].x0, x1 = runs[r].x1;
            for (int y = sy; y < sy + sh; y++) {
                uint16_t *row = buf + y * WAVE_PITCH;
                for (int x = x0 < 0 ? 0 : x0; x < x1 && x < WAVE_W; x++)
                    if (!WAVE_RED(row[x])) row[x] = C565(0xff, 0xc3, 0x80);
            }
            if (x1 - x0 >= 44) wave_text(buf, x0 + 4, sy + 1, "vocal", 0x0000u, 2);
        }
    }
}
static void bjwave_paint(void) { }
#else
static void bjwave_paint(void) { }
#endif
static int hooked_playmode_set(int a, int b, int c, int d)
{
    int result = original_playmode_set(a, b, c, d);
    wave_head_line_light();
#ifdef RX3_DEVELOPER
    bjwave_wave_paint();
#endif
    return result;
}
static int hooked_gr_update(void *screen)
{
    unsigned int i, n = __atomic_load_n(&fill_rect_count, __ATOMIC_SEQ_CST);
    for (i = 0; i < n && i < FILL_RECTS_MAX; i++)
        gr_fill((1u << 8) | fill_rects[i].high, fill_rects[i].x1, fill_rects[i].y1,
                fill_rects[i].x2 - fill_rects[i].x1 + 1, fill_rects[i].y2 - fill_rects[i].y1 + 1, fill_rects[i].rgb);
    bjwave_paint();
    return original_gr_update(screen);
}

static int indicator_screen_ok(void);
/* The player's own top-bar images (the REC dot, the INFO button) are drawn on the screens that show
   the bar and in the right window; the mark is drawn from the first of them each pass, in that
   window, so it is where the player draws and not in a window that only Utility shows. The distinct
   top-bar images seen are logged once (id, layer word, box) to confirm the point. */
static unsigned long indicator_image_hits;
static void *indicator_glyph;                 /* the last top-bar image model, to repaint it on a change */
static uint64_t indicator_glyph_us, indicator_refresh_us;
static void indicator_refresh_if_asked(void)
{
    if (!rx3_indicator_take_refresh() || !indicator_glyph) return;
    uint64_t now = monotonic_enough_us();
    /* Only a model seen a moment ago (the player still draws it), and not more than 4 times a second. */
    if (now - indicator_glyph_us > 3000000u || now - indicator_refresh_us < 250000u) return;
    indicator_refresh_us = now;
    ((void (*)(void *, int, void *))REFRESH_GLYPH)(((void *(*)(void))GET_HMI_MANAGER)(), -1, indicator_glyph);
}
static void indicator_image_point(void *render, const uint8_t *image)
{
    if (!rx3_indicator_active() && !rx3_header_progress()) return;    /* nothing to draw: netstatus off, no theme line */
    uint16_t box[4], layer;
    uint32_t id;
    static uint32_t seen_id[16];
    static uint16_t seen_layer[16];
    static unsigned int seen_count;
    memcpy(box, image + 0x18u, sizeof box);
    if (box[1] > 40u || box[3] > 70u || box[0] < 1000u) return;      /* top right of the 1280x800 bar */
    memcpy(&layer, image + 0x10u, sizeof layer);
    memcpy(&id, image + 0x44u, sizeof id);
    unsigned int i = 0;
    while (i < seen_count && (seen_id[i] != id || seen_layer[i] != layer)) i++;
    if (i == seen_count && seen_count < 16u) {
        seen_id[seen_count] = id;
        seen_layer[seen_count++] = layer;
        log_line("netstatus: top-bar image seen");
        log_number("  image id   =", id);
        log_number("  layer word =", layer);
        log_number("  box x1     =", box[0]);
        log_number("  box y1     =", box[1]);
        log_number("  box x2     =", box[2]);
        log_number("  box y2     =", box[3]);
    }
    if (!indicator_screen_ok()) return;
    indicator_image_hits++;
    indicator_glyph = (void *)image;
    indicator_glyph_us = monotonic_enough_us();
    draw_status_indicator(render, image);
    if (rx3_indicator_probe_enabled()) {
        void *context[2] = { render, (void *)image };
        rx3_indicator_probe(indicator_rect, context, 0u, box[0], box[1], 2u);
    }
}

static void hooked_draw_image(void *render, void *image)
{
    image_draw_calls++;
    if (render_probe_enabled)
        probe_record_image(image);
    if (draw_trace_enabled) {
        memcpy(draw_trace_ring[draw_trace_head % DRAW_TRACE_N], image, 0x54u);
        draw_trace_head++;
    }
    if (RX3_DIAGNOSTIC_ONLY) {
        original_draw_image(render, image);
        return;
    }
    run_pending_ui();
    pad_page_run_pending(render);
    /* Populate the private image records before their first DirectFB lookup.
       Pioneer image-table records and their cached surfaces stay untouched. */
    if (!tab_assets_ready)
        install_tab_assets("image table route: image draw hook");
    /* The image models can all be captured before the watcher finishes
       installing the replacement payloads. Re-check on every ordinary image
       draw so the first draw after installation performs the one-shot refresh
       on rbp's UI thread. Limiting this guard to the capture branches made the
       bootstrap dependent on draw ordering and could leave ZOOM/GRID visible
       until another native invalidation. */
    refresh_initial_performance_tabs_if_ready();
    uint16_t window_layer;
    memcpy(&window_layer, (const uint8_t *)image + 0x10u,
           sizeof(window_layer));
    uint8_t window = (uint8_t)(window_layer & 0xffu);
    uint32_t image_id;
    memcpy(&image_id, (const uint8_t *)image + 0x44u, sizeof(image_id));
    if (title_draw_eye(render,image)) return;
    rx3_image_drawn(image_id);

    /* Capture the native 180x50 model, including its renderer/window
       attachment. A status panel replaces STATUS only while it is registered.
       The selected tab reflects the visible panel, not the internal BEAT FX
       state that keeps a custom pad subtree live. */
    if (window_layer == PERFORMANCE_TAB_LAYER &&
        (image_id == 0x1598u || image_id == 0x1599u)) {
        stock_status_glyph = image;
        memcpy(stock_tab_backing, image, sizeof(stock_tab_backing));
        stock_tab_backing_ready = 1u;
        refresh_initial_performance_tabs_if_ready();
        /* The native glyph can still contain the previous frame's selection.
           The setter owns selection; drawing must not overwrite it. */
        unsigned int selected_image=native_beatfx_selected?0x1598u:0x1599u;
        unsigned int status_panel = row_for_id(STATUS_PANEL) != 0;
        if (tab_assets_ready && (overlay_panel || status_panel ||
                                 image_id!=selected_image)) {
            uint8_t neutral[0x54];
            memcpy(neutral, image, sizeof(neutral));
            uint32_t tab = performance_status_image(status_panel,
                                                    overlay_panel,selected_image);
            set_u32(neutral, 0x44, tab);
            original_draw_image(render, neutral);
            return;
        }
    }

    if (window_layer == BEATFX_LEFT_LAYER && image_id == 0x14e9u)
        performance_left_glyph = image;
    if (window_layer == XPAD_RIGHT_LAYER && image_id == 0x14eau)
        performance_right_glyph = image;
    if (image_id == 0x15c9u)
        left_tab_glyph = image;
    if (image_id == 0x15cau)
        right_tab_glyph = image;
    refresh_initial_performance_tabs_if_ready();

    /* Hardware trace: BeatFxSelectItem/Trash use window-layer 0x1701 and the
       right X-PAD subtree uses 0x1801. Deck summaries below use 0x0301, so
       the full layer is the safe discriminator that image IDs alone lacked. */
    if (overlay_panel && image_id == 0x159au) {
        performance_window = window;
        if (!performance_window_ready) {
            performance_window_ready = 1u;
            log_number("native performance window = ", window);
        }
    }
    /* The pane backdrop opens each pass over the row, so painting the controls
       from it -- and only from it -- gives exactly one row per pass instead of
       one per intercepted call. Everything else inside the subtree is the stock
       pad furniture a live panel stands in for, and is dropped. */
    if (overlay_panel && is_performance_pad_subtree(window_layer)) {
        if (image_id == 0x14e9u || image_id == 0x14eau) {
            original_draw_image(render, image);
            /* The row used to wait here for a stock pad label to clone a face
               from. That label is never drawn -- the pad subtree is images --
               so the condition is what it actually needs: artwork for the
               lettering, or a text model to cut the fills from. */
            if (pad_atlas_ready || pad_text_template_ready || text_template_ready)
                draw_custom_pad_half(render, image, window,
                                     deck_for_pad_subtree(window_layer));
        }
        return;
    }

    /* The sampler page: the player's own RELEASE FX pictures, in a deck's pad
       window, are painted over by the module's titles. They are drawn only
       while that page is up, so nothing else needs to know that it is. */
    if (!overlay_panel) {
        const struct rx3_pad_page *page = rx3_panel_page();
        if (page && image_id >= page->first_image && image_id <= page->last_image) {
            if (is_performance_pad_subtree(window_layer)) {
                unsigned int deck = deck_for_pad_subtree(window_layer);
                if (page->active(deck) &&
                    pad_page_draw(render, image, page, deck, image_id)) {
                    pad_page_report(deck);
                    return;
                }
            } else if (pad_page_logged < PAD_PAGE_LOG_LIMIT) {
                pad_page_logged++;
                log_number("sampler page: its picture is in another layer = ", window_layer);
            }
        } else if (page && is_performance_pad_subtree(window_layer)) {
            pad_page_other_picture(image, deck_for_pad_subtree(window_layer));
        }
    }

    if (image_id != 0x15c9u && image_id != 0x15cau) {
        if (rx3_browse_hide_image(image)) return;
        original_draw_image(render, image);
        rx3_browse_caption(render,image,draw_text_themed);
        indicator_image_point(render, image);
        return;
    }
    draw_custom_tabs(render, image);
}


static void draw_text_observed(void *render, void *text);
/* Icona "S" nel browser (DA_FIXARE/59): un quadratino colorato con la S a sinistra del titolo dei brani che
   hanno un .rx3stem. L'elenco dei nomi lo scrive stems/module.sh (/tmp/rx3-stems-index.txt, "Artista - Titolo" per riga) e il
   watcher lo legge una volta (e di nuovo se cambia dimensione); nel disegno si confronta solo in RAM il testo della riga con il
   nome intero o con la parte dopo " - " (il titolo). RX3_STEMS_ICON=0 la spegne. */
#include "ui/rx3_stems_icon.h"
static struct rx3_sicon_set sicon_sets[2];
static volatile unsigned int sicon_live;
static uint32_t sicon_hash_seen;
static int sicon_have;
static void sicon_load(void)
{
    static char buf[RX3_SICON_MAX * RX3_SICON_LEN];
    const char *v = getenv("RX3_STEMS_ICON");
    if (v && v[0] == '0') return;
    int fd = open("/tmp/rx3-stems-index.txt", O_RDONLY);
    if (fd < 0) return;
    long got = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (got < 0) return;
    uint32_t h = rx3_sicon_hash(buf, (unsigned long)got);
    if (sicon_have && h == sicon_hash_seen) return;
    /* Si riempie l'insieme che il disegno non sta usando e poi si scambia: mai un elenco vuoto o a metà. */
    unsigned int live = __atomic_load_n(&sicon_live, __ATOMIC_SEQ_CST);
    unsigned int next = live ^ 1u;
    rx3_sicon_parse(&sicon_sets[next], buf, (unsigned long)got);
    sicon_hash_seen = h;
    /* Un indice senza nomi, quando se ne avevano, non è una chiavetta svuotata: è un elenco della cartella fallito mentre la
       chiavetta era occupata (il caricamento di un brano con stems). Si tiene l'elenco che c'è: le S non spariscono. */
    if (rx3_sicon_keep_old(sicon_have, sicon_sets[next].count, sicon_sets[live].count)) {
        log_line("stems icon: empty index ignored, keeping the names already known");
        return;
    }
    __atomic_store_n(&sicon_live, next, __ATOMIC_SEQ_CST);
    sicon_have = 1;
    log_number("stems icon: names = ", sicon_sets[next].count);
}
/* Contatori per capire, dal log della chiavetta, perché una S non compare: righe viste con il browser aperto, S disegnate,
   testi passati con il browser chiuso. Una riga di log ogni 10 s solo se qualcosa è cambiato, al massimo 40 righe. */
static volatile unsigned int sicon_rows, sicon_hits, sicon_closed;
static void sicon_report(void)
{
    static unsigned int last_rows, last_hits, last_closed, lines;
    unsigned int rows = __atomic_load_n(&sicon_rows, __ATOMIC_RELAXED);
    unsigned int hits = __atomic_load_n(&sicon_hits, __ATOMIC_RELAXED);
    unsigned int closed = __atomic_load_n(&sicon_closed, __ATOMIC_RELAXED);
    if (rows == last_rows && hits == last_hits) { last_closed = closed; return; }
    if (lines++ < 40u) {
        log_number("stems icon: rows with browser open = ", rows - last_rows);
        log_number("stems icon: S drawn = ", hits - last_hits);
        log_number("stems icon: texts with browser closed = ", closed - last_closed);
    }
    last_rows = rows; last_hits = hits; last_closed = closed;
}
static int sicon_match(const uint16_t *s, unsigned int n)
{
    return rx3_sicon_match(&sicon_sets[__atomic_load_n(&sicon_live, __ATOMIC_SEQ_CST)], s, n);
}
static const uint8_t sicon_s[5] = {7, 4, 7, 1, 7};
/* Il quadratino (14x14, viola dei ruoli degli stems) e la S bianca, nella finestra del testo, prima del titolo. */
static void sicon_draw(void *render, void *glyph, int x, int y_mid)
{
    uint16_t layer;
    memcpy(&layer, (const uint8_t *)glyph + 0x10u, 2);
    uint8_t win = (uint8_t)(layer & 0xffu);
    int y = y_mid - 7;
    draw_native_box_local(render, glyph, glyph, win, x, y, x + 13, y + 13, 0, 0, 0x9b4dffu);
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 3; c++)
            if (sicon_s[r] & (4 >> c))
                draw_native_box_local(render, glyph, glyph, win, x + 4 + c * 2, y + 2 + r * 2, x + 5 + c * 2, y + 3 + r * 2, 0, 0, 0xffffffu);
}

/* A module may draw another label in place of a native one (REVERB shown as
   SHIMMER): the glyph's text is swapped for this draw and put back after. */
static void hooked_draw_text(void *render, void *text)
{
    const uint16_t *swap = 0, *kept;
    unsigned int length = 0;
    uint8_t *glyph = text;
    if (!RX3_DIAGNOSTIC_ONLY) {
        uint16_t layer;
        struct rx3_text_observation observation;
        memcpy(&layer, glyph + 0x10u, sizeof(layer));
        observation.layer = layer;
        memcpy(observation.box, glyph + 0x18u, sizeof(observation.box));
        memcpy(&observation.text, glyph + 0x34u, sizeof(observation.text));
        observation.length = glyph[0x38u];
        swap = rx3_modules_text_substitute(&observation, &length);
    }
    if (!swap || !length || length > 0xffu) {
        uint16_t lay;
        memcpy(&lay, glyph + 0x10u, 2);
        if (!RX3_DIAGNOSTIC_ONLY && lay != 0x201u && lay != 0x301u && !rx3_browse_count_visible())
            __atomic_add_fetch(&sicon_closed, 1u, __ATOMIC_RELAXED);
        if (!RX3_DIAGNOSTIC_ONLY && rx3_browse_count_visible() && lay != 0x201u && lay != 0x301u) {
            const uint16_t *s; unsigned int n = glyph[0x38u];
            memcpy(&s, glyph + 0x34u, sizeof s);
            if (n == 255u && s) { n = 0; while (n < 254u && s[n]) n++; }
            __atomic_add_fetch(&sicon_rows, 1u, __ATOMIC_RELAXED);
            if (sicon_match(s, n)) {
                __atomic_add_fetch(&sicon_hits, 1u, __ATOMIC_RELAXED);
                uint16_t box[4], x1;
                memcpy(box, glyph + 0x18u, sizeof box);
                x1 = box[0];
                box[0] = (uint16_t)(box[0] + 18u);
                memcpy(glyph + 0x18u, box, 2);
                draw_text_observed(render, text);
                memcpy(glyph + 0x18u, &x1, 2);
                sicon_draw(render, glyph, x1, (box[1] + box[3]) / 2);
                return;
            }
        }
        draw_text_observed(render, text);
        return;
    }
    uint8_t kept_length = glyph[0x38u];
    memcpy(&kept, glyph + 0x34u, sizeof(kept));
    memcpy(glyph + 0x34u, &swap, sizeof(swap));
    glyph[0x38u] = (uint8_t)length;
    draw_text_observed(render, text);
    memcpy(glyph + 0x34u, &kept, sizeof(kept));
    glyph[0x38u] = kept_length;
}

static void draw_text_observed(void *render, void *text)
{
    draw_calls++;
    if (render_probe_enabled)
        probe_record_text(text);
    if (!RX3_DIAGNOSTIC_ONLY) {
        const uint8_t *glyph = text;
        uint16_t layer;
        struct rx3_text_observation observation;
        memcpy(&layer, glyph + 0x10u, sizeof(layer));
        observation.layer = layer;
        memcpy(observation.box, glyph + 0x18u, sizeof(observation.box));
        memcpy(&observation.text, glyph + 0x34u, sizeof(observation.text));
        observation.length = glyph[0x38u];
        rx3_modules_text_observed(&observation);
        run_pending_ui();
    }
    uint16_t window_layer;
    memcpy(&window_layer, (const uint8_t *)text + 0x10u, sizeof(window_layer));
    if (!RX3_DIAGNOSTIC_ONLY)
        capture_pad_text_template(text, window_layer);
    /* The row is painted from the pane backdrop, so a stock label inside the
       subtree is simply dropped once it has been cloned. */
    if (!RX3_DIAGNOSTIC_ONLY && overlay_panel &&
        is_performance_pad_subtree(window_layer))
        return;
    if (!RX3_DIAGNOSTIC_ONLY &&
        rx3_browse_draw(render,text,draw_text_themed,original_draw_image,draw_browse_fill))return;
    if (!RX3_DIAGNOSTIC_ONLY && title_text_hidden(text)) return;
    if (RX3_DIAGNOSTIC_ONLY) original_draw_text(render, text);
    else draw_text_themed(render, text);
    if (RX3_DIAGNOSTIC_ONLY)
        return;
    indicator_probe_text(render, text, window_layer);
    if (window_layer != HEADER_LAYER)
        return;
    if (!text_template_ready) {
        memcpy(text_template, text, sizeof(text_template));
        text_template_ready = 1u;
    }
    refresh_initial_performance_tabs_if_ready();
    main_window_draws++;
    overlay_drawn_us = monotonic_enough_us();
    show_startup_message_when_ready();
    draw_message_header(render,text);
}

/* Player (1, 2) and browse (3) show the bar the mark belongs to; Utility and the rest do not. */
static int indicator_screen_ok(void)
{
    const uint8_t *view = *performance_view_pointer;
    unsigned int primary = 0;
    if (view) memcpy(&primary, view + 4u, sizeof(primary));
    return primary >= 1u && primary <= 3u;
}

static int performance_overlay_is_visible(void)
{
    /* UiNotifyDispInfoUpdate (1.19, 0x000ff58c): player views are 1/2,
       BROWSE is 3. A header repaint does not change the active screen. */
    const uint8_t *view = *performance_view_pointer;
    unsigned int primary = 0;
    if (view) memcpy(&primary, view + 4u, sizeof(primary));
    return primary == 1u || primary == 2u;
}

static int point_in_rect(int x, int y, int x1, int y1, int x2, int y2)
{
    return x >= x1 && x <= x2 && y >= y1 && y <= y2;
}

static void set_touch_geometry(void *area, int x, int y,
                               unsigned int width, unsigned int height)
{
    if (!area)
        return;
    *(int *)((uint8_t *)area + 8u) = x;
    *(int *)((uint8_t *)area + 0xcu) = y;
    *(unsigned int *)((uint8_t *)area + 0x10u) = width;
    *(unsigned int *)((uint8_t *)area + 0x14u) = height;
}

/* The six Beat FX touch objects used to be repurposed as two decks' worth of
   controls, which is what capped a row at three controls per deck: there are
   only six of them. The row hit-tests its own rectangles now, so while a custom
   panel is up these are parked off screen instead, and handed back untouched on
   the way out. Parking is what keeps a native action from firing in the gaps
   between controls, which is the one place the row declines a touch. */
static void park_native_performance_touches(int parked)
{
    if (!beatfx_touch_areas[0])
        return;
    if (parked) {
        for (unsigned int i = 0; i < 6u; i++)
            set_touch_geometry(beatfx_touch_areas[i], -4096, -4096, 1u, 1u);
    } else {
        for (unsigned int i = 0; i < 6u; i++)
            set_touch_geometry(beatfx_touch_areas[i],
                               stock_touch_geometry[i].x,
                               stock_touch_geometry[i].y,
                               stock_touch_geometry[i].width,
                               stock_touch_geometry[i].height);
    }
}

/* A backdrop refresh repaints only the backdrop. To restore native controls
   after an overlay, invalidate their owning layer, including all children.
   NS_GlyphRender_HandleType: 0x11 is Layer; 0x14 is Window (do not cross it). */
static void *performance_native_layer(void *glyph)
{
    void *cursor=glyph;
    uint16_t expected=0;
    if(glyph)memcpy(&expected,(uint8_t *)glyph+0x10u,2);
    for(unsigned int depth=0;cursor && depth<16u;depth++) {
        uint8_t type=((uint8_t *)cursor)[4];
        uint16_t layer;memcpy(&layer,(uint8_t *)cursor+0x10u,2);
        if(type==0x11u && layer==expected)return cursor;
        if(type==0x14u)break;
        void *parent;memcpy(&parent,(uint8_t *)cursor+8u,sizeof(parent));
        if(parent==cursor)break;
        cursor=parent;
    }
    return glyph;
}

static void refresh_performance_ui(void)
{
    void *manager = ((void *(*)(void))GET_HMI_MANAGER)();
    void *glyphs[5] = {
        performance_left_glyph, performance_right_glyph,
        left_tab_glyph, right_tab_glyph, stock_status_glyph
    };
    if(!overlay_panel) {
        glyphs[0]=performance_native_layer(glyphs[0]);
        glyphs[1]=performance_native_layer(glyphs[1]);
    }
    for (unsigned int i = 0; i < 5u; i++)
        if (glyphs[i])
            ((void (*)(void *, int, void *))REFRESH_GLYPH)(
                manager, -1, glyphs[i]);
}

/* Active touchscreen tabs and physical pad-mode presses return to the
   native display without resetting samples or stem gains. */
static void restore_status(void)
{
    log_line("native STATUS requested");
    beatfx_reselect_pending = 0u;
    beatfx_reselect_phase = 0u;
    (void)__sync_add_and_fetch(&beatfx_reselect_generation, 1u);
    rx3_panel_activate(0u);
    overlay_panel = 0u;
    native_beatfx_selected = 0u;
    pad_row_clear_press();
    park_native_performance_touches(0);
    if (original_set_beatfx_selected)
        original_set_beatfx_selected(0);
    refresh_performance_ui();
}

/* Close before native dispatch, so the chosen pad function paints normally.
   The 1.19 dispatcher can call a dedicated hook too; do not refresh twice. */
static void leave_performance_panel(void)
{
    if (overlay_panel || native_beatfx_selected || beatfx_reselect_pending)
        restore_status();
    else
        rx3_panel_activate(0u);
}

static int pad_mode_key_pressed(const void *key_input)
{
    return (*(const uint8_t *)((const uint8_t *)key_input + 0x0bu) & 0x0fu) == 0u;
}

static int hooked_on_key_hot_cue(void *player, const void *input)
{
    if (pad_mode_key_pressed(input))
        leave_performance_panel();
    return original_on_key_hot_cue(player, input);
}

static int hooked_on_key_beat_loop(void *player, const void *input)
{
    if (pad_mode_key_pressed(input))
        leave_performance_panel();
    return original_on_key_beat_loop(player, input);
}

static int hooked_on_key_slip_loop(void *player, const void *input)
{
    if (pad_mode_key_pressed(input))
        leave_performance_panel();
    return original_on_key_slip_loop(player, input);
}

static int hooked_on_key_beat_jump(void *player, const void *input)
{
    if (pad_mode_key_pressed(input))
        leave_performance_panel();
    return original_on_key_beat_jump(player, input);
}

static void *hooked_beatfx_xpad_ctor(void *object, void *notification)
{
    void *result = original_beatfx_xpad_ctor(object, notification);
    if (!tab_assets_ready)
        install_tab_assets("image table route: Beat FX constructor");
    for (unsigned int i = 0; i < 6u; i++) {
        void *area = *(void **)((uint8_t *)object + 4u + i * 4u);
        beatfx_touch_areas[i] = area;
        if (!area)
            continue;
        stock_touch_geometry[i].x = *(int *)((uint8_t *)area + 8u);
        stock_touch_geometry[i].y = *(int *)((uint8_t *)area + 0xcu);
        stock_touch_geometry[i].width =
            *(unsigned int *)((uint8_t *)area + 0x10u);
        stock_touch_geometry[i].height =
            *(unsigned int *)((uint8_t *)area + 0x14u);
    }
    park_native_performance_touches(overlay_panel != 0u);
    log_line("native BeatFxAndXPad touch areas captured");
    return result;
}

static void select_custom_panel(unsigned int panel)
{
    if (render_probe_enabled)
        log_number("probe: custom panel selected = ", (unsigned long)panel);
    beatfx_reselect_pending = 0u;
    beatfx_reselect_phase = 0u;
    (void)__sync_add_and_fetch(&beatfx_reselect_generation, 1u);
    rx3_panel_activate(panel);
    overlay_panel = panel;
    /* BeatFxAndXPad is dispatched only while the firmware's binary state is
       BEAT FX. Keep that state active for the lifetime of the custom panel;
       Touchscreen tabs leave it through the explicit STATUS/BEAT FX route. */
    if (original_set_beatfx_selected)
        original_set_beatfx_selected(1);
    park_native_performance_touches(1);
    refresh_performance_ui();
}

static void pump_beatfx_reselect(void)
{
    if (!beatfx_reselect_pending || overlay_panel) return;
    /* Queue the complete native layers once. No provisional STATUS state:
       that round trip blinked the panels and left the tab image stale. */
    beatfx_reselect_pending=0u;
    beatfx_reselect_phase=0u;
    refresh_performance_ui();
    log_line("native Beat FX subtree refresh queued");
}


static void hooked_set_beatfx_selected(int selected)
{
    unsigned int leaving_custom_panel = overlay_panel != 0u;
    if (leaving_custom_panel || (unsigned int)(selected != 0) != native_beatfx_selected)
        log_number("native Beat FX setter = ", (unsigned long)(selected != 0));
    rx3_panel_activate(0u);
    if (render_probe_enabled) {
        log_number("probe: beatfx setter selected = ", (unsigned long)selected);
        log_number("probe: beatfx setter leaving_custom = ",
                   (unsigned long)leaving_custom_panel);
    }
    overlay_panel = 0;
    pad_row_clear_press();
    park_native_performance_touches(0);
    unsigned int changed=(unsigned int)(selected!=0)!=native_beatfx_selected;
    native_beatfx_selected = selected != 0;
    (void)__sync_add_and_fetch(&beatfx_reselect_generation, 1u);
    beatfx_reselect_pending=0u;
    beatfx_reselect_phase=0u;
    original_set_beatfx_selected(selected);
    if (leaving_custom_panel && selected) {
        beatfx_reselect_pending=1u;
        beatfx_reselect_started_ms=now_ms();
    } else if(leaving_custom_panel || changed) {
        refresh_performance_ui();
    }
}


/* Handle only a new touchscreen press; release/drag ownership stays in
   hooked_solve_touch so a held finger cannot toggle a tab twice. */
static int performance_tab_touch(int x, int y)
{
    const struct rx3_pad_row *left = row_for_slot(0u);
    const struct rx3_pad_row *right = row_for_slot(1u);
    const struct rx3_pad_row *status = row_for_id(STATUS_PANEL);
    if ((overlay_panel == 1u && !left) ||
        (overlay_panel == 2u && !right) ||
        (overlay_panel == STATUS_PANEL && !status))
        restore_status();
    if ((!!left != !!right) && point_in_rect(x, y, 1090, 363 + CUSTOM_TAB_OFFSET_Y,
                                            1270, CUSTOM_TAB_TOUCH_BOTTOM)) {
        const struct rx3_pad_row *single = left ? left : right;
        if (overlay_panel == single->panel_id) restore_status();
        else select_custom_panel(single->panel_id);
        log_line("touch action = single feature panel");
        return 1;
    }
    if (point_in_rect(x, y, 1090, 363 + CUSTOM_TAB_OFFSET_Y,
                      1179, CUSTOM_TAB_TOUCH_BOTTOM)) {
        if (!left) return 0;
        if (overlay_panel == left->panel_id) restore_status();
        else select_custom_panel(left->panel_id);
        log_line("touch action = left feature panel");
        return 1;
    } else if (point_in_rect(x, y, 1181, 363 + CUSTOM_TAB_OFFSET_Y,
                             1270, CUSTOM_TAB_TOUCH_BOTTOM)) {
        if (!right) return 0;
        if (overlay_panel == right->panel_id) restore_status();
        else select_custom_panel(right->panel_id);
        log_line("touch action = right feature panel");
        return 1;
    } else if (status && tab_assets_ready &&
               point_in_rect(x, y, 1090, 432, 1179, 482)) {
        if (overlay_panel == STATUS_PANEL) restore_status();
        else select_custom_panel(STATUS_PANEL);
        return 1;
    } else if (!status &&
               point_in_rect(x, y, 1090, 432, 1179, 482)) {
        restore_status();
        return 1;
    } else if (row_for_id(BEATFX_PANEL) && tab_assets_ready &&
               point_in_rect(x, y, 1181, 432, 1270, 482)) {
        if (overlay_panel == BEATFX_PANEL) restore_status();
        else select_custom_panel(BEATFX_PANEL);
        return 1;
    } else if (point_in_rect(x, y, 1181, 432, 1270, 482)) {
        /* BEAT FX is a destination, like STATUS: repeated taps keep it open. */
        if (overlay_panel || !native_beatfx_selected)
            hooked_set_beatfx_selected(1);
        return 1;
    }
    return 0;
}

static void hooked_solve_touch(void *handler, const void *status,
                               const void *mode)
{
    touch_calls++;
    if (RX3_DIAGNOSTIC_ONLY) {
        original_solve_touch(handler, status, mode);
        return;
    }
    const uint8_t *event = status;
    int pressed = event[0] != 0;
    int x = *(const int *)(event + 4u);
    int y = *(const int *)(event + 8u);
    /* Native TouchStatus already contains screen pixels (1280x800).
       Do not reinterpret lower-screen coordinates as raw ADC samples. */

#ifdef RX3_DEVELOPER
    rx3_modules_touch_observed(pressed ? x : *(const int *)((const uint8_t *)handler+8u),
                               pressed ? y : *(const int *)((const uint8_t *)handler+0xcu), pressed);
#endif
    if (rx3_browse_touch(pressed ? x : *(const int *)((const uint8_t *)handler+8u),
                         pressed ? y : *(const int *)((const uint8_t *)handler+0xcu),pressed)) {
        *(uint8_t *)((uint8_t *)handler+4u)=(uint8_t)pressed;
        *(int *)((uint8_t *)handler+8u)=x;
        *(int *)((uint8_t *)handler+0xcu)=y;
        return;
    }

    if (captured_touch) {
        /* The capture is what makes sliding off a control mean something: every
           event of the gesture arrives here, including the release, wherever on
           the screen the finger finally leaves. */
        pad_touch_y = y;
        if (captured_touch == 4u)
            pad_row_touch(row_for_id(overlay_panel), captured_touch_deck,
                          x - (int)captured_touch_deck * RX3_PAD_DECK_STRIDE,
                          pressed ? 2u : 0u);
        *(uint8_t *)((uint8_t *)handler + 4u) = (uint8_t)pressed;
        *(int *)((uint8_t *)handler + 8u) = x;
        *(int *)((uint8_t *)handler + 0xcu) = y;
        if (!pressed)
            captured_touch = 0;
        return;
    }

    int title_x=pressed?x:*(const int *)((const uint8_t *)handler+8u);
    int title_y=pressed?y:*(const int *)((const uint8_t *)handler+0xcu);
    if (rx3_titles_touch(title_touch_hit(title_x,title_y,performance_overlay_is_visible()),
                         pressed, !*(const uint8_t *)((const uint8_t *)handler+4u))) {
        *(uint8_t *)((uint8_t *)handler+4u)=(uint8_t)pressed;
        *(int *)((uint8_t *)handler+8u)=x;
        *(int *)((uint8_t *)handler+0xcu)=y;
        if (!pressed) {
            log_number("title hidden deck 1 = ", rx3_title_hidden(0));
            log_number("title hidden deck 2 = ", rx3_title_hidden(1));
        }
        return;
    }

    /* A module's own target on a native screen (REVERB -> SHIMMER): the
       module checks that its label is on screen now. */
    if (pressed && !*(const uint8_t *)((const uint8_t *)handler + 4u) && !overlay_panel &&
        rx3_modules_beatfx_touch(x, y)) {
        captured_touch = 3u;
        *(uint8_t *)((uint8_t *)handler + 4u) = 1;
        *(int *)((uint8_t *)handler + 8u) = x;
        *(int *)((uint8_t *)handler + 0xcu) = y;
        return;
    }
    if (pressed && !*(const uint8_t *)((const uint8_t *)handler + 4u) &&
        performance_overlay_is_visible()) {
        if (performance_tab_touch(x, y)) captured_touch = 3u;
        if (captured_touch) {
            *(uint8_t *)((uint8_t *)handler + 4u) = 1;
            *(int *)((uint8_t *)handler + 8u) = x;
            *(int *)((uint8_t *)handler + 0xcu) = y;
            return;
        }
    }
    if (pressed && !*(const uint8_t *)((const uint8_t *)handler + 4u) &&
        performance_overlay_is_visible() &&
        y >= RX3_PAD_TOUCH_TOP && y <= RX3_PAD_TOUCH_BOTTOM) {
        unsigned int deck = x >= RX3_PAD_DECK_STRIDE ? 1u : 0u;
        pad_touch_y = y;
        /* A press in a gap between controls is declined, and falls through to
           the player with the native areas parked out of its way. */
        if (pad_row_touch(row_for_id(overlay_panel), deck,
                          x - (int)deck * RX3_PAD_DECK_STRIDE, 1u)) {
            captured_touch_deck = deck;
            captured_touch = 4u;
            *(uint8_t *)((uint8_t *)handler + 4u) = 1u;
            *(int *)((uint8_t *)handler + 8u) = x;
            *(int *)((uint8_t *)handler + 0xcu) = y;
            return;
        }
    }
    original_solve_touch(handler, status, mode);
}

/* Audio device. */

/* rbp publishes the audio device format here. Features that size buffers from
   it cannot be built before this point. */
static void hooked_audio_start(void *engine, void *device)
{
    unsigned int rate = 44100u;
    audio_start_calls++;
    if (device) {
        void **vtable = *(void ***)device;
        double sample_rate = ((audio_sample_rate_fn)vtable[0x44u / 4u])(device);
        if (sample_rate > 0.0 && sample_rate <= 192000.0)
            rate = (unsigned int)sample_rate;
    }
    original_audio_start(engine, device);
    if (RX3_DIAGNOSTIC_ONLY) {
        log_line("diagnostic: audioDeviceAboutToStart observed");
        return;
    }
    rx3_modules_audio_started(rate);
}


/* Deck identity: PcmReader::load is the one place a reader meets its deck. */
static int hooked_load(void *reader, const void *track_info)
{
    unsigned int channel = *(const uint32_t *)((const uint8_t *)reader + 0x20u);
    if (channel >= 2u) {
        int result = original_load(reader, track_info);
        log_number("feature dispatch ignored: unknown PcmReader channel = ",
                   channel);
        return result;
    }

    rx3_audio_track_loading(channel);
    rx3_modules_track_will_load(channel, reader, track_info);
    int result = original_load(reader, track_info);
    rx3_audio_track_loaded(channel, reader);
    rx3_modules_track_did_load(channel, reader, track_info);
    return result;
}

/* Lifecycle. */

/* Both teardown paths take the same hooks out in the same order: the error exit
   of the installer, and the destructor. They were two copies, so adding a hook
   and updating only one of them left that hook installed on the path nobody
   exercises until something has already gone wrong. */
static void uninstall_performance_hooks(void)
{
    park_native_performance_touches(0);
    if (uninstall_hook(&beatfx_xpad_ctor_hook)) original_beatfx_xpad_ctor = 0;
    else log_line("cleanup retained hook: beatfx_xpad_ctor");
    if (uninstall_hook(&beat_jump_hook)) original_on_key_beat_jump = 0;
    else log_line("cleanup retained hook: beat_jump");
    if (uninstall_hook(&slip_loop_hook)) original_on_key_slip_loop = 0;
    else log_line("cleanup retained hook: slip_loop");
    if (uninstall_hook(&beat_loop_hook)) original_on_key_beat_loop = 0;
    else log_line("cleanup retained hook: beat_loop");
    if (uninstall_hook(&hot_cue_hook)) original_on_key_hot_cue = 0;
    else log_line("cleanup retained hook: hot_cue");
    if (uninstall_hook(&set_beatfx_hook)) original_set_beatfx_selected = 0;
    else log_line("cleanup retained hook: set_beatfx");
    if (uninstall_hook(&touch_hook)) original_solve_touch = 0;
    else log_line("cleanup retained hook: touch");
    if (original_gr_update && uninstall_hook(&gr_update_hook)) original_gr_update = 0;
    if (original_playmode_set && uninstall_hook(&playmode_set_hook)) original_playmode_set = 0;
    if (uninstall_hook(&draw_image_hook)) original_draw_image = 0;
    else log_line("cleanup retained hook: draw_image");
    if (uninstall_hook(&image_info_hook)) original_image_info = 0;
    else log_line("cleanup retained hook: image_info");
    if (uninstall_hook(&draw_text_hook)) original_draw_text = 0;
    else log_line("cleanup retained hook: draw_text");
    if (uninstall_hook(&load_hook)) original_load = 0;
    else log_line("cleanup retained hook: load");
    if (uninstall_hook(&audio_start_hook)) original_audio_start = 0;
    else log_line("cleanup retained hook: audio_start");
}

/* LD_PRELOAD is inherited by exec'ed utilities. They must never touch player
   addresses, truncate its readiness marker, or run its teardown. */
static int player_process_initialized;
static int is_player_process(void)
{
    char name[16];
    int fd = open("/proc/self/comm", O_RDONLY);
    if (fd < 0) return 0;
    int length = read(fd, name, sizeof(name));
    close(fd);
    return length == 4 && !memcmp(name, "rbp\n", 4u);
}

__attribute__((constructor)) static void initialize(void)
{
    if (!is_player_process()) return;
    player_process_initialized = 1;
    rx3_log_configure();
    /* An earlier marker must not survive a rejected startup. */
    int ready_fd = open(READY_FILE, O_WRONLY | O_TRUNC, 0600);
    if (ready_fd >= 0) close(ready_fd);
    /* On unless the operator says otherwise. The sentinel file is the other
       half of the switch, for a deck that is already misbehaving. */
    const char *messages = getenv("RX3_MESSAGES");
    messages_enabled = !(messages && messages[0] == '0');
    (void)rx3_message_allowed();

    const char *delay = getenv("RX3_TAB_DELAY_MS");
    if (delay) {
        unsigned long ms = 0;
        for (const char *c = delay; *c >= '0' && *c <= '9'; c++)
            ms = ms * 10u + (unsigned long)(*c - '0');
        if (ms) {
            tab_install_not_before_us = monotonic_enough_us() + (uint64_t)ms * 1000u;
            log_number("diagnostic: image table install held back, ms = ", ms);
        }
    }
    const char *probe = getenv("RX3_RENDER_PROBE");
    render_probe_enabled = probe && probe[0] == '1';
    { const char *tr = getenv("RX3_DRAW_TRACE"); draw_trace_enabled = tr && tr[0] == '1'; }
    if (render_probe_enabled) {
        log_line("render probe active: " RENDER_PROBE_FILE);
        /* Enough to see a modifier go down, a key follow it and both come
           up: this runs on the input path and each line reaches the drive. */
        rx3_input_trace_keys(12u);
    }
    /* RX3_KEY_TRACE=<decimal key code> (0 = every key) logs the first
       KEY_TRACE_LIMIT matching SEND_KEY events with their raw value, e.g.
       17417 for the AUX LINE/PORTABLE switch (TEST_SU_RX3 §8).
       Each line is I/O on the input thread: diagnostics only. */
    const char *trace = getenv("RX3_KEY_TRACE");
    if (trace && trace[0] >= '0' && trace[0] <= '9') {
        unsigned long code = 0;
        for (const char *c = trace; *c >= '0' && *c <= '9' && code <= 0xffffu; c++)
            code = code * 10u + (unsigned long)(*c - '0');
        if (code <= 0xffffu) {
            rx3_input_trace_only((unsigned int)code);
            rx3_input_trace_keys(KEY_TRACE_LIMIT);
            log_number("diagnostic: tracing SEND_KEY, code = ", code);
        }
    }
    /* RX3_BUTTON_TRACE=1 (exported by the logging module): the first
       KEY_TRACE_LIMIT button events, every code, knobs left out. What the
       panel really sends for a combination (SHIFT+CUE, SEARCH) is read from
       mod.txt after one test, instead of assumed. */
    const char *buttons = getenv("RX3_BUTTON_TRACE");
    if (buttons && buttons[0] == '1' && !(trace && trace[0] >= '0' && trace[0] <= '9')) {
        rx3_input_trace_only(0u);
        rx3_input_trace_keys(KEY_TRACE_LIMIT * 2u);
        log_line("diagnostic: tracing the first buttons pressed");
    }
    rx3_input_bind_mode_keys(leave_performance_panel);
    rx3_panels_bind_refresh(refresh_performance_ui);
    rx3_modules_bind_writer(rx3_write_guarded);

    /* Each feature is a module of its own and announces itself through the
       environment its module.sh exports. Modules start first: they register
       rows, images and handlers, and the shared services install their own
       native hooks as clients arrive. */
    unsigned int started = rx3_modules_start();
    if (rx3_modules_failures()) { rx3_modules_stop(); return; }
    if (!rx3_panel_count() && !rx3_image_contributions() && !rx3_browse_count() &&
        !rx3_titles_enabled()) {
        if (started) {
            publish_ready();
            log_line("RX3 performance hook active");
        }
        return;
    }

    /* PcmReader::load is the core deck-identity service: readers meet their
       decks there, and track notifications go out from it. */
    original_load = (load_fn)install_hook(
        &load_hook, PCM_LOAD, load_guard, (void *)hooked_load);
    if (!original_load) {
        log_line("rejected: unexpected PcmReader::load prologue");
        goto reject_performance_hooks;
    }

    original_set_beatfx_selected = (set_beatfx_selected_fn)install_hook(
        &set_beatfx_hook, SET_BEATFX_STORAGE, set_beatfx_guard,
        (void *)hooked_set_beatfx_selected);
    if (!original_set_beatfx_selected) {
        log_line("rejected: unexpected Beat FX state setter prologue");
        goto reject_performance_hooks;
    }

    original_on_key_hot_cue = (on_key_pad_fn)install_hook(
        &hot_cue_hook, ON_KEY_HOT_CUE, hot_cue_guard,
        (void *)hooked_on_key_hot_cue);
    original_on_key_beat_loop = (on_key_pad_fn)install_hook(
        &beat_loop_hook, ON_KEY_BEAT_LOOP, pad_mode_guard,
        (void *)hooked_on_key_beat_loop);
    original_on_key_slip_loop = (on_key_pad_fn)install_hook(
        &slip_loop_hook, ON_KEY_SLIP_LOOP, pad_mode_guard,
        (void *)hooked_on_key_slip_loop);
    original_on_key_beat_jump = (on_key_pad_fn)install_hook(
        &beat_jump_hook, ON_KEY_BEAT_JUMP, pad_mode_guard,
        (void *)hooked_on_key_beat_jump);
    if (!original_on_key_hot_cue || !original_on_key_beat_loop ||
        !original_on_key_slip_loop || !original_on_key_beat_jump) {
        log_line("rejected: unexpected hardware pad-mode key prologue");
        goto reject_performance_hooks;
    }

    original_beatfx_xpad_ctor = (beatfx_xpad_ctor_fn)install_hook(
        &beatfx_xpad_ctor_hook, BEATFX_XPAD_CTOR, beatfx_xpad_ctor_guard,
        (void *)hooked_beatfx_xpad_ctor);
    if (!original_beatfx_xpad_ctor) {
        log_line("rejected: unexpected BeatFxAndXPad constructor prologue");
        goto reject_performance_hooks;
    }


    /* The launch patch extends the lookup bound to include the private IDs.
       Both instructions are independent of PC and can be relocated intact. */
    /* movw r3, #EXTENDED_IMAGE_COUNT - 1, which is what module.sh writes.
       This word, the count above and the module's replacement string are one
       fact in three files; a test compares them rather than trusting them. */
    static const uint8_t image_info_guard[8] = {
        0xa5, 0x36, 0x01, 0xe3, 0x03, 0x00, 0x50, 0xe1
    };
    original_image_info = (image_info_fn)install_hook(
        &image_info_hook, 0x001d192c, image_info_guard, (void *)hooked_image_info);
    if (!original_image_info) {
        static const uint8_t stock_image_info_guard[8] = {
            0xcc, 0x35, 0x01, 0xe3, 0x03, 0x00, 0x50, 0xe1
        };
        original_image_info = (image_info_fn)install_hook(
            &image_info_hook, 0x001d192c, stock_image_info_guard, (void *)hooked_image_info);
    }
    if (!original_image_info)
        log_line("warning: early image lookup hook unavailable");

    original_draw_text = (draw_text_fn)install_hook(
        &draw_text_hook, PAL_DRAW_TEXT, draw_text_guard, (void *)hooked_draw_text);
    if (!original_draw_text) {
        log_line("rejected: unexpected NS_PALRender_DrawText prologue");
        goto reject_performance_hooks;
    }

    original_draw_image = (draw_image_fn)install_hook(
        &draw_image_hook, PAL_DRAW_IMAGE, draw_image_guard, (void *)hooked_draw_image);
    if (!original_draw_image) {
        log_line("rejected: unexpected NS_PALRender_DrawImage prologue");
        goto reject_performance_hooks;
    }
    if (draw_trace_enabled) {
        original_gr_update = (gr_update_fn)install_hook(&gr_update_hook, GR_UPDATE_SCREEN, gr_update_guard,
                                                        (void *)hooked_gr_update);
        log_line(original_gr_update ? "screen overlay: DS_GR_UpdateScreen hook installed"
                                    : "screen overlay: unexpected DS_GR_UpdateScreen prologue, overlay off");
    }
    if (bjwave_enabled()) {
        original_playmode_set = (playmode_set_fn)install_hook(&playmode_set_hook, PLAYMODE_SET, playmode_set_guard,
                                                              (void *)hooked_playmode_set);
        log_line(original_playmode_set ? "beat jump bar: ui_PLAYMODE_Set hook installed"
                                       : "beat jump bar: unexpected ui_PLAYMODE_Set prologue, bar off");
    }

    original_solve_touch = (solve_touch_fn)install_hook(
        &touch_hook, SOLVE_TOUCH, touch_guard, (void *)hooked_solve_touch);
    if (!original_solve_touch) {
        log_line("rejected: unexpected solveCoordToKey prologue");
        goto reject_performance_hooks;
    }

    if (rx3_modules_uses_audio()) {
        original_audio_start = (audio_start_fn)install_hook(
            &audio_start_hook, AUDIO_START, audio_start_guard, (void *)hooked_audio_start);
        if (!original_audio_start) goto reject_performance_hooks;
    }


    state_thread_running = 1;
    if (!pthread_create(&state_thread, 0, watch_patch_state, 0))
        state_thread_started = 1;
    else
        log_line("warning: patch-state watcher could not start");
    publish_ready();
    log_line("RX3 performance hook active");
    return;

reject_performance_hooks:
    log_line("runtime failed: selected modules not ready");
    rx3_modules_stop();
    uninstall_performance_hooks();
}

__attribute__((destructor)) static void finalize(void)
{
    if (!player_process_initialized) return;
    __atomic_store_n(&state_thread_running, 0, __ATOMIC_SEQ_CST);
    if (state_thread_started) {
        pthread_join(state_thread, 0);
        state_thread_started = 0;
    }
    rx3_modules_stop();
    uninstall_performance_hooks();
}

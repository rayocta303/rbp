/* SPDX-License-Identifier: MPL-2.0 */
#ifndef RX3_THEME_CONVERSION_H
#define RX3_THEME_CONVERSION_H
/* One render-owned job. Both classification and conversion spend this budget;
   records never point into a partially filled allocation. */
#define THEME_PIXEL_BUDGET 8192u
#define THEME_JOB_BUDGET 4u
#define THEME_PIXEL_BUDGET_FAST 131072u   /* deck vuoti: il player non fa nulla, la mod finisce subito */
#define THEME_JOB_BUDGET_FAST 16u
#define THEME_PIXEL_BUDGET_LIGHT 2048u     /* una traccia carica o in caricamento: la CPU torna al player */
#define THEME_JOB_BUDGET_LIGHT 1u
static unsigned int theme_pixel_budget = THEME_PIXEL_BUDGET, theme_job_budget = THEME_JOB_BUDGET;
static int theme_cpu_light;
/* state: 0 deck vuoti, 1 caricamento, 2 traccia caricata (rx3_audio.load_state). */
static void theme_set_cpu_mode(unsigned int state)
{
    theme_cpu_light = state != 0u;
    theme_pixel_budget = state ? THEME_PIXEL_BUDGET_LIGHT : THEME_PIXEL_BUDGET_FAST;
    theme_job_budget = state ? THEME_JOB_BUDGET_LIGHT : THEME_JOB_BUDGET_FAST;
}
static uint8_t theme_requested[(STOCK_IMAGE_COUNT + 7u) / 8u];
/* Images turned away because the arena had no room. Not done: they are not
   asked for again while the mode lasts, and are retried after the next mode
   change (L5). Counted so the first refusal of a session is logged once. */
static uint8_t theme_id_full[(STOCK_IMAGE_COUNT + 7u) / 8u];
static unsigned int theme_full_count;
static unsigned int theme_request_cursor;
static unsigned long theme_memory_kb;
static uint64_t theme_memory_next_us;
static struct {
    const uint16_t *source;
    unsigned int image, count, cursor, opaque, vivid, near_black, classifying;
    unsigned int image_class;
    int light, keyed;
} theme_job;
/* The four v2 tables, built by the watcher before the arena is published. */
static const uint16_t *theme_v2_luts;

/* Watcher only: no renderer opens /proc or allocates the image arena. The
   allocation remains resident, including during theme changes and shutdown. */
static int theme_conversion_pending(void);
/* Avvio: il player ha gia' disegnato con la tabella di serie quando il thread del core installa la tabella privata
   (puntatore cambiato fuori dal confine di una passata di disegno): pannelli, hot cue, barra dei deck restavano a meta'
   (grigi a blocchi) finche' un ridisegno completo (SOURCE) non li rifaceva. Una volta sola, mezzo secondo dopo che le tabelle
   sono pronte, se il modo e' scuro (in chiaro ci pensa gia' il cambio di tema), si chiede lo stesso ridisegno completo. */
static uint64_t theme_boot_repaint_at_us;
static int theme_boot_repaint_done;
static int theme_boot_repaint_due(int tables_ready, uint64_t now_us, int light)
{
    if (theme_boot_repaint_done || !tables_ready) return 0;
    if (!theme_boot_repaint_at_us) { theme_boot_repaint_at_us = now_us + 500000u; return 0; }
    if (now_us < theme_boot_repaint_at_us) return 0;
    theme_boot_repaint_done = 1;
    return !light;
}
static void theme_prepare_conversion(void)
{
    if (!theme_enabled || !framework->images->variants_ready()) return;
    uint64_t now = monotonic_enough_us();
    if (now < theme_memory_next_us) return;
    /* Con l'arena già pronta e niente in coda la memoria libera serve solo come riserva: una lettura di
       /proc/meminfo ogni 2 s basta (prima 4 al secondo, per sempre). Con lavoro in coda, come prima. */
    int idle = __atomic_load_n(&theme_arena, __ATOMIC_SEQ_CST) && !theme_conversion_pending();
    theme_memory_next_us = now + (idle ? 2000000u : 250000u);
    const struct rx3_memory_service *memory = framework->memory;
    unsigned long available = memory->available_kb();
    __atomic_store_n(&theme_memory_kb, available, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&theme_arena, __ATOMIC_SEQ_CST) || available < THEME_ARENA_FLOOR_KB) return;
    /* The same floor through the shared ledger: the arena itself is inside
       it, and allocations other modules have in flight are counted. */
    const size_t total = THEME_ARENA_BYTES + THEME_V2_LUT_BYTES;
    if (!memory->reserve(&theme_enabled, total,
                         THEME_ARENA_FLOOR_KB - total / 1024u)) return;
    void *arena = mmap(0, total, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (arena == MAP_FAILED) {
        memory->abandon(&theme_enabled, total);
        return;
    }
    memory->settle(&theme_enabled, total);
    /* The tables sit after the conversions. Built here, off the render
       thread, and published together with the arena. */
    uint16_t *luts = (uint16_t *)((uint8_t *)arena + THEME_ARENA_BYTES);
    theme_v2_build_luts(luts);
    __atomic_store_n(&theme_v2_luts, luts, __ATOMIC_SEQ_CST);
    __atomic_store_n(&theme_arena, arena, __ATOMIC_SEQ_CST);
}

/* Render thread, from the core just before a stock image is drawn. Images the
   core has replaced never reach the conversion: native_image() refuses them. */
static void theme_remap_image(unsigned int image)
{
    if ((!theme_light_active() && !theme_global_dark) ||
        !framework->images->variants_ready() || image >= STOCK_IMAGE_COUNT) return;
    unsigned int bit = 1u << (image & 7u);
    if (!((theme_id_done[image >> 3u] | theme_id_full[image >> 3u]) & bit))
        theme_requested[image >> 3u] |= bit;
}

/* Render thread, after a mode change: what the arena turned away is asked
   for again the next time it is drawn. The arena is never freed, because
   published records keep pointing into it, so a retry succeeds only if room
   came back (a partial job dropped on the change); DA_FIXARE/29. */
static void theme_retry_deferred(void)
{
    if (!theme_full_count) return;
    memset(theme_id_full, 0, sizeof(theme_id_full));
    theme_full_count = 0;
}

static void theme_publish_conversion(unsigned int image, const uint16_t *pixels)
{
    (void)framework->images->publish_variant(image, pixels);
    unsigned int bit = 1u << (image & 7u);
    theme_id_done[image >> 3u] |= bit;
    theme_requested[image >> 3u] &= ~bit;
}

/* At most one full scan per admission; the number of admissions is bounded. */
static int theme_next_request(void)
{
    for (unsigned int n = 0; n < STOCK_IMAGE_COUNT; n++) {
        unsigned int image = theme_request_cursor++;
        if (theme_request_cursor == STOCK_IMAGE_COUNT) theme_request_cursor = 0;
        if (theme_requested[image >> 3u] & (1u << (image & 7u))) return (int)image;
    }
    return -1;
}

/* Lavoro ancora da fare: un'immagine a metà o richieste in coda. Finché c'è, il passaggio di
   disegno chiede un altro ridisegno, così il tema appare per intero senza aspettare il ciclo successivo. */
static int theme_conversion_pending(void)
{
    if (theme_job.source) return 1;
    for (unsigned int i = 0; i < sizeof(theme_requested); i++)
        if (theme_requested[i]) return 1;
    return 0;
}

static int theme_begin_conversion(unsigned int image)
{
    struct rx3_native_image native;
    int known = framework->images->native_image(image, &native);
    uint64_t bytes = known ? (uint64_t)native.width * native.height * 2u : 0u;
    const uint16_t *source = known ? native.pixels : 0;
    if (!known || (native.format != THEME_FORMAT_RGB565 &&
                   native.format != THEME_FORMAT_RGB565_KEYED) || native.paletted ||
        !native.width || !native.height || bytes > THEME_IMAGE_MAX_BYTES) {
        theme_id_done[image >> 3u] |= 1u << (image & 7u);
        theme_requested[image >> 3u] &= ~(1u << (image & 7u));
        return 0;
    }
    for (unsigned int i = 0; i < theme_seen_count; i++) {
        if (theme_seen_source[i] == source) {
            theme_publish_conversion(image, theme_seen_pixels[i]);
            return 1;
        }
    }
    if (bytes > THEME_ARENA_BYTES - theme_arena_used) {
        if (!theme_full_count++) {
            log_number("theme: arena full, image deferred = ", image);
            log_number("  arena used bytes = ", (unsigned long)theme_arena_used);
        }
        theme_id_full[image >> 3u] |= 1u << (image & 7u);
        theme_requested[image >> 3u] &= ~(1u << (image & 7u));
        return 0;
    }
    memset(&theme_job, 0, sizeof(theme_job));
    theme_job.source = source;theme_job.image = image;
    theme_job.count = native.width * native.height;
    theme_job.light = theme_light_active() != 0;
    theme_job.classifying = theme_job.light;
    return 0;
}

static int theme_run_conversions(void)
{
    uint8_t *arena = __atomic_load_n(&theme_arena, __ATOMIC_SEQ_CST);
    if ((!theme_light_active() && !theme_global_dark) ||
        !arena || !framework->images->variants_ready() ||
        __atomic_load_n(&theme_memory_kb, __ATOMIC_SEQ_CST) < THEME_CONVERT_RESERVE_KB) return 0;
    /* A partial result belongs to the mode it was started in. Its source stays
       queued, and its uncommitted arena space can be reused on a mode change. */
    if (theme_job.source) {
        struct rx3_native_image native;
        if (theme_job.light != (theme_light_active() != 0) ||
            !framework->images->native_image(theme_job.image, &native) ||
            theme_job.source != native.pixels ||
            theme_job.count != native.width * native.height)
            memset(&theme_job, 0, sizeof(theme_job));
    }
    unsigned int remaining = theme_pixel_budget;
    int changed = 0;
    for (unsigned int jobs = 0; jobs < theme_job_budget && remaining; jobs++) {
        if (!theme_job.source) {
            int image = theme_next_request();
            if (image < 0) break;
            changed |= theme_begin_conversion((unsigned int)image);
            if (!theme_job.source) continue;
        }
        if (theme_job.classifying) {
            while (remaining && theme_job.cursor < theme_job.count) {
                uint16_t pixel = theme_job.source[theme_job.cursor++];remaining--;
                if (pixel == COLOUR_KEY) { theme_job.keyed = 1; continue; }
                unsigned int red, green, blue;
                theme_unpack(pixel, &red, &green, &blue);
                theme_job.opaque++;
                if (theme_spread(red, green, blue) > THEME_VIVID_SPREAD) theme_job.vivid++;
                unsigned int high = red > green ? red : green;
                if (blue > high) high = blue;
                if (high < THEME_V2_NEAR_BLACK) theme_job.near_black++;
            }
            if (theme_job.cursor != theme_job.count) break;
            theme_job.image_class = theme_v2_classify(theme_job.opaque, theme_job.vivid,
                                                      theme_job.near_black, theme_job.keyed);
            theme_job.classifying = 0;theme_job.cursor = 0;
        }
        uint16_t *converted = (void *)(arena + theme_arena_used);
        const uint16_t *lut = theme_v2_luts + theme_job.image_class * 65536u;
        while (remaining && theme_job.cursor < theme_job.count) {
            unsigned int i = theme_job.cursor++;remaining--;
            uint16_t pixel = theme_job.source[i];
            converted[i] = theme_job.light ?
                theme_light_special(theme_job.image, pixel, lut[pixel]) :
                theme_pixel_dark(pixel);
        }
        if (theme_job.cursor != theme_job.count) break;
        theme_arena_used += theme_job.count * 2u;
        if (theme_seen_count < 4096u) {
            theme_seen_source[theme_seen_count] = theme_job.source;
            theme_seen_pixels[theme_seen_count++] = converted;
        }
        theme_publish_conversion(theme_job.image, converted);
        theme_job.source = 0;changed = 1;
    }
    return changed;
}

/* Linea di caricamento del tema chiaro (solo developer): quante immagini aspettano, quante sono state convertite
   da quando la linea e' apparsa. Contare e dividere si fa nel thread del modulo, mai nel disegno. */
static unsigned int theme_bits(const uint8_t *map, unsigned int bytes)
{
    unsigned int n = 0;
    for (unsigned int i = 0; i < bytes; i++)
        for (uint8_t b = map[i]; b; b &= (uint8_t)(b - 1u)) n++;
    return n;
}
static unsigned int theme_pending_images(void)
{
    return theme_bits(theme_requested, sizeof theme_requested) + (theme_job.source &&
        !(theme_requested[theme_job.image >> 3u] & (1u << (theme_job.image & 7u))) ? 1u : 0u);
}
/* num/den in 1/1024, senza divisione a 32 bit (il .so non lega libgcc). */
static unsigned int theme_bar_ratio(unsigned int num, unsigned int den)
{
    unsigned int q = 0, r = 0, value = num << 10;
    if (!den || num > 0x3fffffu) return 0;
    for (int bit = 31; bit >= 0; bit--) {
        r = (r << 1) | ((value >> bit) & 1u);
        if (r >= den) { r -= den; q |= 1u << bit; }
    }
    return q;
}
static unsigned int theme_bar_state_start, theme_bar_state_shown;
static uint64_t theme_bar_state_next_us;
static int theme_bar_enabled = 1;
/* 0 = nascosta; altrimenti 1..1023 mentre c'e' lavoro. `done` = immagini convertite in tutto. */
static unsigned int theme_bar_progress(unsigned int pending, unsigned int done)
{
    if (!pending) { theme_bar_state_shown = 0; return 0; }
    if (!theme_bar_state_shown) { theme_bar_state_shown = 1; theme_bar_state_start = done; }
    unsigned int gained = done > theme_bar_state_start ? done - theme_bar_state_start : 0u;
    unsigned int p = theme_bar_ratio(gained, gained + pending);
    return p ? (p > 1023u ? 1023u : p) : 1u;
}
#endif

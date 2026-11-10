/* SPDX-License-Identifier: MPL-2.0 */
/* Display mode. The module owns its conversion policy, the chrome decisions,
 * the SHIFT + SHORTCUT switch, the Utility row, the waveform palette and its
 * pixel arena. The core owns the tables, the fill adapter and SHIFT itself. */
#include "../core/api/rx3_module_api.h"
static const struct rx3_services *framework;
#define install_hook framework->install_hook
#define uninstall_hook framework->uninstall_hook
#define log_line framework->log_line
#define log_number framework->log_number

struct rx3_timeval { long seconds, microseconds; };
static uint64_t monotonic_enough_us(void)
{
    struct rx3_timeval value;
    if (gettimeofday(&value, 0))
        return 0;
    return (uint64_t)(unsigned long)value.seconds * 1000000u +
           (uint64_t)(unsigned long)value.microseconds;
}

#include "rx3_theme_decl.h"
#include "rx3_theme_feature.h"
#include "../core/api/rx3_thread.h"

/* The module's own worker: the Utility row can commit a choice without a
   redraw, the arena is allocated off the render path, and the dark remap's
   kill switch is a file. Nothing here touches a table or the display. */
static pthread_t theme_worker;
static int theme_worker_started;
static volatile int theme_worker_running;

/* Thread del modulo, al massimo 4 volte al secondo: pubblica l'avanzamento alla linea della barra del titolo.
   A lavoro finito la sparisce con un ridisegno completo (lo stesso del cambio di tema). */
static void theme_bar_tick(void)
{
    uint64_t now = monotonic_enough_us();
    if (theme_cpu_light) {
        /* Una traccia si carica o e' caricata: la linea si ferma e sparisce (senza ridisegni), riparte da zero se serve. */
        if (theme_bar_state_shown) { theme_bar_state_shown = 0; if (framework->indicator && framework->indicator->header_progress) framework->indicator->header_progress(0); }
        return;
    }
    if (now < theme_bar_state_next_us || !framework->indicator || !framework->indicator->header_progress) return;
    theme_bar_state_next_us = now + 250000u;
    int active = theme_bar_enabled && (theme_light_active() || theme_global_dark);
    unsigned int pending = active ? theme_pending_images() : 0u;
    unsigned int was = theme_bar_state_shown;
    unsigned int p = theme_bar_progress(pending, active ? theme_bits(theme_id_done, sizeof theme_id_done) : 0u);
    framework->indicator->header_progress(p);
    if (was && !p) theme_refresh_pending = 1u;
}

static void *theme_watch(void *unused)
{
    (void)unused;
    rx3_background_thread();
    while (__atomic_load_n(&theme_worker_running, __ATOMIC_SEQ_CST)) {
        usleep(theme_cpu_light ? 200000u : 50000u);   /* una traccia in gioco: meno risvegli */
        utility_poll_theme_row();
        theme_prepare_conversion();
        theme_bar_tick();
        if (theme_global_dark) {
            int sentinel = open(THEME_DARK_SENTINEL, O_RDONLY);
            if (sentinel >= 0) {
                close(sentinel);
                theme_global_dark = 0;
                log_line("theme: rx3-theme.off seen, global remap stopped");
            }
        }
        {
            int tfd = open("/tmp/rx3-theme.toggle", O_RDONLY);
            if (tfd >= 0) {
                close(tfd);
                unlink("/tmp/rx3-theme.toggle");
                __sync_bool_compare_and_swap(&theme_toggle_pending, 0, 1);
                log_line("theme: rx3-theme.toggle triggered");
            }
        }
        {
            static int s_last_light = -1;
            int cur_light = (framework && framework->images) ? framework->images->light_active() : 0;
            if (cur_light != s_last_light) {
                s_last_light = cur_light;
                int sf = open("/tmp/rx3-theme.state", O_WRONLY | O_CREAT | O_TRUNC, 0666);
                if (sf >= 0) {
                    char ch = cur_light ? '1' : '0';
                    write(sf, &ch, 1);
                    close(sf);
                }
            }
        }
    }
    return 0;
}

int rx3_theme_is_light(void)
{
    return framework && framework->images ? framework->images->light_active() : 0;
}

void rx3_theme_toggle(void)
{
    __sync_bool_compare_and_swap(&theme_toggle_pending, 0, 1);
}

static int theme_configured(void)
{
    const char *theme = getenv("RX3_THEME");
    return theme && theme[0] != '\0';
}

static int theme_start(const struct rx3_services *services)
{
    framework = services;
    if (!services->images || !services->images->claim_variants || !services->input ||
        !services->panels || !services->memory)
        return 0;
    const char *theme = getenv("RX3_THEME");
    theme_enabled = theme && theme[0] != '\0';
    if (!theme_enabled) return 0;
    theme_read_mode(theme);
    { const char *bar = getenv("RX3_THEME_BAR"); theme_bar_enabled = !(bar && bar[0] == '0'); }
    if (!theme_feature_install()) return 0;
    __atomic_store_n(&theme_worker_running, 1, __ATOMIC_SEQ_CST);
    if (pthread_create(&theme_worker, 0, theme_watch, 0)) {
        theme_worker_running = 0;
        log_line("light theme: watcher could not start");
        return 0;
    }
    theme_worker_started = 1;
    return 1;
}

static void theme_stop(void)
{
    if (!framework) return;
    __atomic_store_n(&theme_worker_running, 0, __ATOMIC_SEQ_CST);
    if (theme_worker_started) {
        pthread_join(theme_worker, 0);
        theme_worker_started = 0;
    }
    if (framework->images && framework->input) theme_feature_remove();
    theme_enabled = 0;
}

const struct rx3_module rx3_theme_module = {
    .version = RX3_MODULE_API_VERSION, .size = sizeof(struct rx3_module),
    .name = "theme-white", .configured = theme_configured,
    .start = theme_start, .stop = theme_stop
};

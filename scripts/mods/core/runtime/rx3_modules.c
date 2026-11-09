/* SPDX-License-Identifier: MPL-2.0 */
#include "../api/rx3_module_api.h"
#include "rx3_modules.h"
#include "../services/rx3_hooks.h"
#include "../services/rx3_log.h"
#include "../services/rx3_mix_state.h"
#include "../services/rx3_notice.h"
extern const struct rx3_module *const rx3_bundle[];
extern const unsigned int rx3_bundle_count;
#define RX3_MODULE_LIMIT 32u
/* The header mark: one owner, one draw callback, read on the render thread. */
static const void *indicator_owner;
static rx3_indicator_draw_fn indicator_draw_fn;
static rx3_indicator_probe_fn indicator_probe_fn;
static int indicator_acquire(const void *owner, rx3_indicator_draw_fn draw)
{
    const void *none = 0;
    if (!owner || !draw) return 0;
    if (!__atomic_compare_exchange_n(&indicator_owner, &none, owner, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        return 0;
    __atomic_store_n(&indicator_draw_fn, draw, __ATOMIC_SEQ_CST);
    return 1;
}
static void indicator_release(const void *owner)
{
    const void *expected = owner;
    if (!owner || __atomic_load_n(&indicator_owner, __ATOMIC_SEQ_CST) != owner) return;
    __atomic_store_n(&indicator_draw_fn, (rx3_indicator_draw_fn)0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&indicator_probe_fn, (rx3_indicator_probe_fn)0, __ATOMIC_SEQ_CST);
    (void)__atomic_compare_exchange_n(&indicator_owner, &expected, 0, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
static void indicator_probe_set(const void *owner, rx3_indicator_probe_fn fn)
{
    if (!owner || __atomic_load_n(&indicator_owner, __ATOMIC_SEQ_CST) != owner) return;
    __atomic_store_n(&indicator_probe_fn, fn, __ATOMIC_SEQ_CST);
}
static unsigned int indicator_dirty;
static void indicator_invalidate(const void *owner)
{
    if (owner && __atomic_load_n(&indicator_owner, __ATOMIC_SEQ_CST) == owner)
        __atomic_store_n(&indicator_dirty, 1u, __ATOMIC_SEQ_CST);
}
int rx3_indicator_take_refresh(void)
{
    return __atomic_load_n(&indicator_dirty, __ATOMIC_RELAXED) && __atomic_exchange_n(&indicator_dirty, 0u, __ATOMIC_SEQ_CST);
}
static unsigned int header_progress_value;
static void header_progress_set(unsigned int p1024)
{
    if (p1024 > 1024u) p1024 = 1024u;
    /* Nascondere (0) non chiede un ridisegno: la linea sparisce al prossimo disegno della barra, che arriva comunque
       (l'orologio); durante il caricamento di una traccia non si forza nulla. */
    if (__atomic_exchange_n(&header_progress_value, p1024, __ATOMIC_SEQ_CST) != p1024 && p1024)
        __atomic_store_n(&indicator_dirty, 1u, __ATOMIC_SEQ_CST);
}
unsigned int rx3_header_progress(void) { return __atomic_load_n(&header_progress_value, __ATOMIC_SEQ_CST); }
static const struct rx3_indicator_service indicator_service = { indicator_acquire, indicator_release, indicator_probe_set, indicator_invalidate, header_progress_set };
int rx3_indicator_active(void) { return __atomic_load_n(&indicator_draw_fn, __ATOMIC_SEQ_CST) != 0; }
int rx3_indicator_probe_enabled(void) { return __atomic_load_n(&indicator_probe_fn, __ATOMIC_SEQ_CST) != 0; }
void rx3_indicator_probe(rx3_indicator_rect rect, void *context, unsigned int layer_index, int x, int y, unsigned int point)
{
    rx3_indicator_probe_fn fn = __atomic_load_n(&indicator_probe_fn, __ATOMIC_SEQ_CST);
    if (fn) fn(rect, context, layer_index, x, y, point);
}
void rx3_indicator_draw(rx3_indicator_rect rect, void *context, int light)
{
    rx3_indicator_draw_fn draw = __atomic_load_n(&indicator_draw_fn, __ATOMIC_SEQ_CST);
    if (draw) draw(rect, context, light);
}
/* Not const: the firmware writer is bound by the core at startup, so host
   builds of this unit never link the ARM instruction adapter. */
static struct rx3_services services = {
    install_hook, uninstall_hook, log_line, rx3_mix_read, &rx3_notices, &rx3_dsp, &rx3_panels, rx3_wave_read, detach_hook, release_hook, &rx3_images, &rx3_browse, &rx3_titles,
    &rx3_input, &rx3_audio, rx3_log_number, &rx3_memory, 0,
    rx3_mix_claim, rx3_mix_release, rx3_wave_claim, rx3_wave_release, &rx3_loader,
    &indicator_service
};
void rx3_modules_bind_writer(int (*writer)(unsigned long, const void *, const void *, unsigned int))
{
    services.write_guarded = writer;
}
static unsigned char active[RX3_MODULE_LIMIT];
static unsigned int failures;
static unsigned int notifications_enabled, notifications_active;
/* Stop closes admission before waiting. A late entrant sees the closed gate
   and never touches a module. No waiting occurs on the callback threads. */
static int enter_notification(void)
{
    __atomic_add_fetch(&notifications_active,1u,__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&notifications_enabled,__ATOMIC_SEQ_CST)) return 1;
    __atomic_sub_fetch(&notifications_active,1u,__ATOMIC_SEQ_CST);
    return 0;
}
static void leave_notification(void)
{
    __atomic_sub_fetch(&notifications_active,1u,__ATOMIC_SEQ_CST);
}
unsigned int rx3_modules_failures(void) { return failures; }
unsigned int rx3_modules_start(void)
{
    failures = 0;
    if (rx3_bundle_count > RX3_MODULE_LIMIT) { failures = 1; return 0; }
    unsigned int count = 0;
    for (unsigned int i = 0; i < rx3_bundle_count; i++) {
        const struct rx3_module *module = rx3_bundle[i];
        if (active[i]) { count++; continue; }
        if (module->version != RX3_MODULE_API_VERSION ||
            module->size != sizeof(*module) || !module->start || !module->stop) {
            failures++;
            log_line("module refused: incompatible descriptor");
            continue;
        }
        if (module->configured && !module->configured()) continue;
        log_line("module configured:");
        log_line(module->name);
        if (!module->start(&services)) {
            failures++;
            module->stop();
            log_line("module refused: partial installation removed");
            log_line(module->name);
            continue;
        }
        active[i] = 1;
        log_line("module active:");
        log_line(module->name);
        count++;
    }
    __atomic_store_n(&notifications_enabled,1u,__ATOMIC_SEQ_CST);
    return count;
}
void rx3_modules_stop(void)
{
    __atomic_store_n(&notifications_enabled,0u,__ATOMIC_SEQ_CST);
    while (__atomic_load_n(&notifications_active,__ATOMIC_SEQ_CST)) usleep(1000u);
    if (rx3_bundle_count > RX3_MODULE_LIMIT) return;
    for (unsigned int i = rx3_bundle_count; i > 0; i--) {
        if (!active[i - 1]) continue;
        rx3_bundle[i - 1]->stop();
        active[i - 1] = 0;
    }
}

void rx3_modules_track_will_load(unsigned int deck, void *reader, const void *info)
{
    if (!enter_notification()) return;
    for (unsigned int i = 0; i < rx3_bundle_count; i++)
        if (active[i] && rx3_bundle[i]->track_will_load)
            rx3_bundle[i]->track_will_load(deck, reader, info);
    leave_notification();
}

void rx3_modules_track_did_load(unsigned int deck, void *reader, const void *info)
{
    if (!enter_notification()) return;
    for (unsigned int i = 0; i < rx3_bundle_count; i++)
        if (active[i] && rx3_bundle[i]->track_did_load)
            rx3_bundle[i]->track_did_load(deck, reader, info);
    leave_notification();
}

void rx3_modules_audio_started(unsigned int rate)
{
    if (!enter_notification()) return;
    for (unsigned int i=0; i<rx3_bundle_count; i++)
        if (active[i] && rx3_bundle[i]->audio_started)
            rx3_bundle[i]->audio_started(rate);
    leave_notification();
}
void rx3_modules_text_observed(const struct rx3_text_observation *text)
{
    if (!enter_notification()) return;
    for (unsigned int i=0; i<rx3_bundle_count; i++)
        if (active[i] && rx3_bundle[i]->text_observed)
            rx3_bundle[i]->text_observed(text);
    leave_notification();
}
const uint16_t *rx3_modules_text_substitute(const struct rx3_text_observation *text,
                                            unsigned int *length)
{
    const uint16_t *swap = 0;
    if (!enter_notification()) return 0;
    for (unsigned int i=0; i<rx3_bundle_count && !swap; i++)
        if (active[i] && rx3_bundle[i]->text_substitute)
            swap = rx3_bundle[i]->text_substitute(text, length);
    leave_notification();
    return swap;
}
int rx3_modules_beatfx_touch(int x, int y)
{
    int taken = 0;
    if (!enter_notification()) return 0;
    for (unsigned int i=0; i<rx3_bundle_count && !taken; i++)
        if (active[i] && rx3_bundle[i]->beatfx_touch) taken = rx3_bundle[i]->beatfx_touch(x, y);
    leave_notification();
    return taken;
}
void rx3_modules_touch_observed(int x, int y, int pressed)
{
    if (!enter_notification()) return;
    for (unsigned int i=0; i<rx3_bundle_count; i++)
        if (active[i] && rx3_bundle[i]->touch_observed) rx3_bundle[i]->touch_observed(x, y, pressed);
    leave_notification();
}
void rx3_modules_report(void)
{
    if (!enter_notification()) return;
    for (unsigned int i=0; i<rx3_bundle_count; i++)
        if (active[i] && rx3_bundle[i]->report) rx3_bundle[i]->report();
    leave_notification();
}

int rx3_modules_uses_audio(void)
{
    if (rx3_bundle_count > RX3_MODULE_LIMIT) return 0;
    for (unsigned int i=0; i<rx3_bundle_count; i++)
        if (active[i] && rx3_bundle[i]->audio_started) return 1;
    return 0;
}

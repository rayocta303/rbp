/* SPDX-License-Identifier: MPL-2.0 */
/* Long press on a track in the browser -> a header notice "MAKE STEMS in coda: <title>".
 * Step 1 of DA_FIXARE study: it only announces and logs; NOTHING is written to
 * the queue. The touch event is never consumed: the native list handles it as always.
 * Detection is in rx3_longpress_state.h; this file holds the native-facing part. */
#include "../core/api/rx3_module_api.h"
#include "rx3_longpress_state.h"

static const char lp_owner[] = "browse-longpress";
static const struct rx3_services *lp_framework;
static struct lp_machine lp_machine;

extern int gettimeofday(void *, void *);

static uint32_t lp_now_ms(void)
{
    long tv[2] = { 0, 0 };
    gettimeofday(tv, 0);
    return (uint32_t)((unsigned long)tv[0] * 1000ul + (unsigned long)tv[1] / 1000ul);
}

static const uint16_t lp_en[] = u"MAKE STEMS in queue: ";
static const uint16_t lp_en_bare[] = u"MAKE STEMS in queue";
static const uint16_t lp_it[] = u"MAKE STEMS in coda: ";
static const uint16_t lp_it_bare[] = u"MAKE STEMS in coda";

static void lp_announce(const struct rx3_browse_row *row, int have_row)
{
    const struct rx3_notice_service *notices = lp_framework->notices;
    uint16_t text[RX3_NOTICE_TEXT_UNITS];
    if (!notices) return;
    struct rx3_notice note = { lp_owner, 0, 0, RX3_NOTICE_INFO, 3500u, text };
    /* English, then Italian, like the other notices. */
    lp_compose(text, lp_en, lp_en_bare, have_row ? row->title : 0);
    notices->post(&note);
    note.id = 1;
    lp_compose(text, lp_it, lp_it_bare, have_row ? row->title : 0);
    notices->post(&note);
}

static void lp_log(int x, int y, long id, long row, const char *result)
{
    char line[160];
    lp_format_log(line, sizeof(line), x, y, lp_machine.held_ms, id, row, result);
    lp_framework->log_line(line);
}

static void lp_touch(int x, int y, int pressed)
{
    const struct rx3_browse_service *browse = lp_framework ? lp_framework->browse : 0;
    if (!browse || !browse->list_visible) return;
    unsigned event = lp_step(&lp_machine, pressed, x, y, lp_now_ms(), browse->list_visible());
    if (event == LP_FIRE) {
        struct rx3_browse_row row;
        int have = browse->highlighted && browse->highlighted(&row);
        lp_log(x, y, have ? (long)row.id : -1, have ? (long)row.row : -1,
               have ? (lp_machine.on_release ? "notice (at release)" : "notice")
                    : "notice without title (no cursor row)");
        lp_announce(&row, have);
    } else if (event == LP_CANCEL && lp_machine.why == LP_WHY_MOVED) {
        lp_log(x, y, -1, -1, "cancelled: moved");
    }
}

static int lp_configured(void)
{
    const char *value = getenv("RX3_BROWSE_LONGPRESS");
    return value && value[0] == '1';
}

static int lp_start(const struct rx3_services *services)
{
    if (!services->browse || !services->browse->highlighted || !services->notices) {
        services->log_line("browse-longpress: no browse or notice service");
        return 0;
    }
    memset(&lp_machine, 0, sizeof(lp_machine));
    lp_framework = services;
    services->log_line("browse-longpress: active (notice only, nothing is queued)");
    return 1;
}

static void lp_stop(void)
{
    const struct rx3_services *framework = lp_framework;
    lp_framework = 0;
    if (framework && framework->notices) {
        framework->notices->cancel(lp_owner, 0);
        framework->notices->cancel(lp_owner, 1);
    }
}

const struct rx3_module rx3_browse_longpress_module = {
    .version = RX3_MODULE_API_VERSION, .size = sizeof(struct rx3_module),
    .name = "browse-longpress", .configured = lp_configured, .start = lp_start, .stop = lp_stop,
    .touch_observed = lp_touch
};

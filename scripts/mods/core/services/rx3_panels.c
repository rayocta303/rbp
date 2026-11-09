/* SPDX-License-Identifier: MPL-2.0 */
#include "rx3_panels.h"
#define PANEL_LIMIT 16u
static const struct rx3_pad_row *rows[PANEL_LIMIT];
static unsigned int pending;
static unsigned int close_pending;
const struct rx3_pad_row *rx3_panel_find(unsigned int id)
{
    if (!id || id > PANEL_LIMIT) return 0;
    return __atomic_load_n(&rows[id-1u], __ATOMIC_SEQ_CST);
}
static int register_row(const struct rx3_pad_row *row)
{
    if (!row || !row->panel_id || row->panel_id > PANEL_LIMIT ||
        !row->count || row->count > 8u || !row->widgets ||
        row->scope > RX3_PAD_SCOPE_SCREEN) return 0;
    for (unsigned int i=0; i<row->count; i++) {
        if (row->widgets[i].kind > RX3_PAD_OUTLINE_BUTTON) return 0;
        if ((row->widgets[i].kind == RX3_PAD_SLIDER || row->widgets[i].kind == RX3_PAD_TOGGLE_SLIDER) &&
            (!row->slider_get || !row->slider_set)) return 0;
    }
    const struct rx3_pad_row *expected = 0;
    return __atomic_compare_exchange_n(&rows[row->panel_id-1u], &expected,
            row, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) || expected == row;
}
static void unregister_row(const struct rx3_pad_row *row)
{
    if (!row || !row->panel_id || row->panel_id > PANEL_LIMIT) return;
    const struct rx3_pad_row *expected = row;
    (void)__atomic_compare_exchange_n(&rows[row->panel_id-1u], &expected,
                                      0, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
static int open_panel(unsigned int id)
{
    if (!rx3_panel_find(id)) return 0;
    __atomic_store_n(&pending, id, __ATOMIC_SEQ_CST);
    return 1;
}
unsigned int rx3_panel_take_open(void)
{
    unsigned int id=__atomic_exchange_n(&pending, 0u, __ATOMIC_SEQ_CST);
    return rx3_panel_find(id) ? id : 0;
}
static int close_panel(void)
{
    __atomic_store_n(&close_pending, 1u, __ATOMIC_SEQ_CST);
    return 1;
}
unsigned int rx3_panel_take_close(void)
{
    return __atomic_exchange_n(&close_pending, 0u, __ATOMIC_SEQ_CST);
}
unsigned int rx3_panel_count(void)
{
    unsigned int count=0;
    for(unsigned int i=1; i<=PANEL_LIMIT; i++) count += rx3_panel_find(i)!=0;
    return count;
}
/* Every row but `id` hears it is left, then `id` hears it is shown. Zero
   leaves them all. Rows are resident, so a concurrent unregister is safe. */
void rx3_panel_activate(unsigned int id)
{
    for (unsigned int i = 1; i <= PANEL_LIMIT; i++) {
        const struct rx3_pad_row *row = rx3_panel_find(i);
        if (row && row->activate && i != id) row->activate(0u);
    }
    const struct rx3_pad_row *shown = rx3_panel_find(id);
    if (shown && shown->activate) shown->activate(1u);
}
static void (*refresh_action)(void);
void rx3_panels_bind_refresh(void (*action)(void)) { refresh_action = action; }
static void refresh(void) { if (refresh_action) refresh_action(); }

/* The sampler page: one at a time, resident like the rows. */
static const struct rx3_pad_page *page_registered;
static unsigned int page_repaint;
const struct rx3_pad_page *rx3_panel_page(void)
{
    return __atomic_load_n(&page_registered, __ATOMIC_SEQ_CST);
}
static int register_page(const struct rx3_pad_page *page)
{
    if (!page || !page->active || !page->cell_state || !page->colour || !page->caption ||
        page->first_image > page->last_image || page->last_image - page->first_image > 63u ||
        (page->title_image && !page->title)) return 0;
    const struct rx3_pad_page *expected = 0;
    return __atomic_compare_exchange_n(&page_registered, &expected, page, 0,
            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) || expected == page;
}
static void unregister_page(const struct rx3_pad_page *page)
{
    if (!page) return;
    const struct rx3_pad_page *expected = page;
    (void)__atomic_compare_exchange_n(&page_registered, &expected, 0, 0,
                                      __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
static void invalidate_page(void) { __atomic_store_n(&page_repaint, 1u, __ATOMIC_SEQ_CST); }
unsigned int rx3_panel_take_page_repaint(void)
{
    return __atomic_exchange_n(&page_repaint, 0u, __ATOMIC_SEQ_CST);
}
void rx3_panel_poll_page(void)
{
    const struct rx3_pad_page *page = rx3_panel_page();
    if (page && page->needs_refresh && page->needs_refresh()) invalidate_page();
}
const struct rx3_panel_service rx3_panels = {register_row, unregister_row, open_panel, refresh, close_panel,
                                             register_page, unregister_page, invalidate_page};

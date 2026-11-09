/* SPDX-License-Identifier: MPL-2.0 */
#ifndef RX3_PANELS_H
#define RX3_PANELS_H
#include "../api/rx3_panel_api.h"
const struct rx3_pad_row *rx3_panel_find(unsigned int);
unsigned int rx3_panel_count(void);
unsigned int rx3_panel_take_open(void);
unsigned int rx3_panel_take_close(void);
void rx3_panel_activate(unsigned int id);
void rx3_panels_bind_refresh(void (*)(void));
const struct rx3_pad_page *rx3_panel_page(void);
/* Nonzero once since the last call if the page asked for a repaint
   (invalidate_page, or its needs_refresh as polled by the watcher). */
unsigned int rx3_panel_take_page_repaint(void);
void rx3_panel_poll_page(void);
#endif

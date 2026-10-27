#ifndef RBPI4B_UI_OVERLAY_H
#define RBPI4B_UI_OVERLAY_H

#include "menu_paint.h"

void ui_overlay_draw(const struct menu_view *v, int browse_mode);
int  ui_overlay_touch(int down, int x, int y, int browse_mode);

#endif /* RBPI4B_UI_OVERLAY_H */

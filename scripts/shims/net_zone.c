/*
 * net_zone.c -- Pure hit testing and state machine for Network Settings overlay.
 */

#include "net_zone.h"
#include <stddef.h>

enum {
    TARGET_NONE = 0,
    TARGET_STRIP,
    TARGET_CLOSE,
    TARGET_TAB0,
    TARGET_TAB1,
    TARGET_TAB2,
    TARGET_TAB3,
    TARGET_TOGGLE,
    TARGET_ACTION,
    TARGET_ROW_CONNECT
};

static int s_open = 0;
static int s_tab = NET_TAB_WIFI;
static int s_pressed_target = TARGET_NONE;
static int s_pressed_val = -1;

int net_is_open(void)
{
    return s_open;
}

void net_open(void)
{
    s_open = 1;
}

void net_close(void)
{
    s_open = 0;
    s_pressed_target = TARGET_NONE;
}

void net_toggle(void)
{
    s_open = !s_open;
    s_pressed_target = TARGET_NONE;
}

int net_get_tab(void)
{
    return s_tab;
}

void net_set_tab(int tab)
{
    if (tab >= 0 && tab < NET_TABS_COUNT)
        s_tab = tab;
}

int net_get_pressed(void)
{
    return s_pressed_target;
}

static int in_rect(int x, int y, int x0, int y0, int x1, int y1)
{
    return (x >= x0 && x <= x1 && y >= y0 && y <= y1);
}

int net_feed(int down, int x, int y, int *act, int *val)
{
    if (act) *act = NET_ACT_NONE;
    if (val) *val = 0;

    /* When closed: check if finger hits top bar strip pill */
    if (!s_open) {
        if (down) {
            if (in_rect(x, y, NET_HIT_STRIP_X0, NET_HIT_STRIP_Y0, NET_HIT_STRIP_X1, NET_HIT_STRIP_Y1)) {
                s_pressed_target = TARGET_STRIP;
                return MZ_FEED_TAKEN;
            }
            return MZ_FEED_NONE;
        } else {
            if (s_pressed_target == TARGET_STRIP) {
                s_pressed_target = TARGET_NONE;
                if (in_rect(x, y, NET_HIT_STRIP_X0, NET_HIT_STRIP_Y0, NET_HIT_STRIP_X1, NET_HIT_STRIP_Y1)) {
                    s_open = 1;
                    if (act) *act = NET_ACT_OPEN;
                    return MZ_FEED_TAKEN;
                }
            }
            return MZ_FEED_NONE;
        }
    }

    /* When open: overlay is active */

    /* If finger taps top bar strip while open, toggle closed */
    if (in_rect(x, y, NET_HIT_STRIP_X0, NET_HIT_STRIP_Y0, NET_HIT_STRIP_X1, NET_HIT_STRIP_Y1)) {
        if (down) {
            s_pressed_target = TARGET_STRIP;
            return MZ_FEED_TAKEN;
        } else {
            if (s_pressed_target == TARGET_STRIP) {
                s_pressed_target = TARGET_NONE;
                s_open = 0;
                if (act) *act = NET_ACT_CLOSE;
            }
            return MZ_FEED_TAKEN;
        }
    }

    /* Below the overlay (y > NET_WIN_Y1 / y >= 580): pass through to DJ deck controls! */
    if (y > NET_WIN_Y1) {
        s_pressed_target = TARGET_NONE;
        return MZ_FEED_NONE;
    }

    /* Above the overlay (y < 50): consume so rbp doesn't receive stray touches */
    if (y < NET_WIN_Y0) {
        s_pressed_target = TARGET_NONE;
        return MZ_FEED_TAKEN;
    }

    /* Inside the overlay window: (x 0..1279, y 50..709) */
    if (down) {
        s_pressed_target = TARGET_NONE;
        s_pressed_val = -1;

        /* Close Button [X] */
        if (in_rect(x, y, NET_CLOSE_X0, NET_CLOSE_Y0, NET_CLOSE_X1, NET_CLOSE_Y1)) {
            s_pressed_target = TARGET_CLOSE;
            return MZ_FEED_TAKEN;
        }

        /* Tabs */
        if (in_rect(x, y, NET_TAB0_X0, NET_TAB_Y0, NET_TAB0_X1, NET_TAB_Y1)) {
            s_pressed_target = TARGET_TAB0;
            return MZ_FEED_TAKEN;
        }
        if (in_rect(x, y, NET_TAB1_X0, NET_TAB_Y0, NET_TAB1_X1, NET_TAB_Y1)) {
            s_pressed_target = TARGET_TAB1;
            return MZ_FEED_TAKEN;
        }
        if (in_rect(x, y, NET_TAB2_X0, NET_TAB_Y0, NET_TAB2_X1, NET_TAB_Y1)) {
            s_pressed_target = TARGET_TAB2;
            return MZ_FEED_TAKEN;
        }
        if (in_rect(x, y, NET_TAB3_X0, NET_TAB_Y0, NET_TAB3_X1, NET_TAB_Y1)) {
            s_pressed_target = TARGET_TAB3;
            return MZ_FEED_TAKEN;
        }

        /* Main Toggle / Action Button in Control Bar */
        int tog_x1 = NET_TOGGLE_X1;
        if (s_tab == NET_TAB_HOTSPOT) tog_x1 = NET_TOGGLE_X0 + 130;
        else if (s_tab == NET_TAB_STATUS) tog_x1 = NET_TOGGLE_X0 + 190;

        if (in_rect(x, y, NET_TOGGLE_X0, NET_TOGGLE_Y0, tog_x1, NET_TOGGLE_Y1)) {
            s_pressed_target = TARGET_TOGGLE;
            return MZ_FEED_TAKEN;
        }

        /* Action/Scan Button */
        if (in_rect(x, y, NET_ACTION_X0, NET_ACTION_Y0, NET_ACTION_X1, NET_ACTION_Y1)) {
            s_pressed_target = TARGET_ACTION;
            return MZ_FEED_TAKEN;
        }

        /* Wi-Fi Tab Rows (Tab 0) */
        if (s_tab == NET_TAB_WIFI) {
            for (int i = 0; i < NET_WIFI_ROWS; i++) {
                int ry0 = NET_ROW_Y0(i);
                int ry1 = NET_ROW_Y1(i);
                if (in_rect(x, y, NET_ROW_CONNECT_X0, ry0, NET_ROW_CONNECT_X1, ry1)) {
                    s_pressed_target = TARGET_ROW_CONNECT;
                    s_pressed_val = i;
                    return MZ_FEED_TAKEN;
                }
            }
        }

        return MZ_FEED_TAKEN;
    } else {
        /* Finger UP / Release */
        int prev = s_pressed_target;
        int pval = s_pressed_val;
        s_pressed_target = TARGET_NONE;
        s_pressed_val = -1;

        if (prev == TARGET_CLOSE && in_rect(x, y, NET_CLOSE_X0, NET_CLOSE_Y0, NET_CLOSE_X1, NET_CLOSE_Y1)) {
            s_open = 0;
            if (act) *act = NET_ACT_CLOSE;
            return MZ_FEED_TAKEN;
        }

        if (prev == TARGET_TAB0 && in_rect(x, y, NET_TAB0_X0, NET_TAB_Y0, NET_TAB0_X1, NET_TAB_Y1)) {
            s_tab = NET_TAB_WIFI;
            if (act) *act = NET_ACT_SET_TAB;
            if (val) *val = NET_TAB_WIFI;
            return MZ_FEED_TAKEN;
        }
        if (prev == TARGET_TAB1 && in_rect(x, y, NET_TAB1_X0, NET_TAB_Y0, NET_TAB1_X1, NET_TAB_Y1)) {
            s_tab = NET_TAB_BT;
            if (act) *act = NET_ACT_SET_TAB;
            if (val) *val = NET_TAB_BT;
            return MZ_FEED_TAKEN;
        }
        if (prev == TARGET_TAB2 && in_rect(x, y, NET_TAB2_X0, NET_TAB_Y0, NET_TAB2_X1, NET_TAB_Y1)) {
            s_tab = NET_TAB_HOTSPOT;
            if (act) *act = NET_ACT_SET_TAB;
            if (val) *val = NET_TAB_HOTSPOT;
            return MZ_FEED_TAKEN;
        }
        if (prev == TARGET_TAB3 && in_rect(x, y, NET_TAB3_X0, NET_TAB_Y0, NET_TAB3_X1, NET_TAB_Y1)) {
            s_tab = NET_TAB_STATUS;
            if (act) *act = NET_ACT_SET_TAB;
            if (val) *val = NET_TAB_STATUS;
            return MZ_FEED_TAKEN;
        }

        int tog_x1 = NET_TOGGLE_X1;
        if (s_tab == NET_TAB_HOTSPOT) tog_x1 = NET_TOGGLE_X0 + 130;
        else if (s_tab == NET_TAB_STATUS) tog_x1 = NET_TOGGLE_X0 + 190;

        if (prev == TARGET_TOGGLE && in_rect(x, y, NET_TOGGLE_X0, NET_TOGGLE_Y0, tog_x1, NET_TOGGLE_Y1)) {
            if (s_tab == NET_TAB_WIFI) {
                if (act) *act = NET_ACT_WIFI_TOGGLE;
            } else if (s_tab == NET_TAB_BT) {
                if (act) *act = NET_ACT_BT_TOGGLE;
            } else if (s_tab == NET_TAB_HOTSPOT) {
                if (act) *act = NET_ACT_HOTSPOT_TOGGLE;
            } else if (s_tab == NET_TAB_STATUS) {
                if (act) *act = NET_ACT_NET_RESTART;
            }
            return MZ_FEED_TAKEN;
        }

        if (prev == TARGET_ACTION && in_rect(x, y, NET_ACTION_X0, NET_ACTION_Y0, NET_ACTION_X1, NET_ACTION_Y1)) {
            if (s_tab == NET_TAB_WIFI) {
                if (act) *act = NET_ACT_WIFI_SCAN;
            } else if (s_tab == NET_TAB_BT) {
                if (act) *act = NET_ACT_BT_SCAN;
            } else if (s_tab == NET_TAB_STATUS) {
                if (act) *act = NET_ACT_NET_RESTART;
            }
            return MZ_FEED_TAKEN;
        }

        if (prev == TARGET_ROW_CONNECT && pval >= 0 && pval < NET_WIFI_ROWS) {
            int ry0 = NET_ROW_Y0(pval);
            int ry1 = NET_ROW_Y1(pval);
            if (in_rect(x, y, NET_ROW_CONNECT_X0, ry0, NET_ROW_CONNECT_X1, ry1)) {
                if (act) *act = NET_ACT_WIFI_CONNECT;
                if (val) *val = pval;
                return MZ_FEED_TAKEN;
            }
        }

        return MZ_FEED_TAKEN;
    }
}

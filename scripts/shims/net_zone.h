/*
 * net_zone.h -- OS Network & Wireless Settings overlay geometry and hit testing.
 *
 * Designed for Pioneer XDJ-RX3 running rbp. Follows the modular, pure geometry
 * pattern of fx_zone.h and util_zone.h.
 *
 * Surfaces:
 * 1. Top Bar Pill Box (x: 742..828, y: 7..43) containing Wi-Fi & Bluetooth icons.
 *    Touch hit target: x: 735..835, y: 0..50. Tapping toggles the overlay.
 * 2. Network Settings Overlay (x: 0..1279, y: 50..709).
 *    Bottom decks (y: 710..800) remain 100% visible and responsive to DJ touch.
 *    - Header (y: 50..96) with Title & Close button [X] (x: 1205..1265, y: 56..92).
 *    - Tab Bar (y: 100..142) with 4 tabs: WI-FI, BLUETOOTH, HOTSPOT, STATUS/IP.
 *    - Content Body (x: 40..1240, y: 148..700).
 */

#ifndef RBPI4B_NET_ZONE_H
#define RBPI4B_NET_ZONE_H

#include "menu_zone.h"   /* MZ_FEED_NONE, MZ_FEED_TAKEN */

/* --- Top Bar Button Strip Geometry (KONSEP MOD RBP VIEW UTAMA, x: 939..1066) --- */
#define NET_STRIP_X0       939
#define NET_STRIP_X1      1066
#define NET_STRIP_Y0         8
#define NET_STRIP_Y1        44

/* Top Bar Pill Hit Target (generous for fingers) */
#define NET_HIT_STRIP_X0   930
#define NET_HIT_STRIP_X1  1075
#define NET_HIT_STRIP_Y0     0
#define NET_HIT_STRIP_Y1    48

/* Wi-Fi & BT Icon Locations inside Strip */
#define NET_ICON_WIFI_X    966
#define NET_ICON_WIFI_Y     13
#define NET_ICON_BT_X     1022
#define NET_ICON_BT_Y       12

/* --- Main Overlay Window Geometry --- */
#define NET_WIN_X0           0
#define NET_WIN_X1        1279
#define NET_WIN_Y0          50
#define NET_WIN_Y1         579   /* Strictly above the blue deck line at y=581! */

/* Header Bar & Close Button */
#define NET_HDR_Y0          50
#define NET_HDR_Y1          92
#define NET_CLOSE_X0      1140
#define NET_CLOSE_X1      1240
#define NET_CLOSE_Y0        54
#define NET_CLOSE_Y1        88

/* Tab Bar (5 evenly distributed tabs across x: 40..1240) */
#define NET_TABS_COUNT       5
#define NET_TAB_Y0          96
#define NET_TAB_Y1         134

#define NET_TAB0_X0         40    /* WI-FI */
#define NET_TAB0_X1        270
#define NET_TAB1_X0        282    /* BLUETOOTH */
#define NET_TAB1_X1        512
#define NET_TAB2_X0        524    /* HOTSPOT */
#define NET_TAB2_X1        754
#define NET_TAB3_X0        766    /* MOD MENU */
#define NET_TAB3_X1        996
#define NET_TAB4_X0       1008    /* STATUS / IP */
#define NET_TAB4_X1       1240

/* Content Area */
#define NET_BODY_X0         40
#define NET_BODY_X1       1240
#define NET_BODY_Y0        140
#define NET_BODY_Y1        576

/* Control Bar & Buttons inside Content Area */
#define NET_CTRL_Y0        140
#define NET_CTRL_Y1        186

#define NET_TOGGLE_X0      230
#define NET_TOGGLE_X1      320
#define NET_TOGGLE_Y0      145
#define NET_TOGGLE_Y1      181

#define NET_ACTION_X0      335
#define NET_ACTION_X1      435
#define NET_ACTION_Y0      145
#define NET_ACTION_Y1      181

/* Wi-Fi List Rows (Tab 0) */
#define NET_WIFI_ROWS        7
#define NET_WIFI_HDR_Y0    192
#define NET_WIFI_HDR_Y1    214

#define NET_ROW_Y0(i)      (216 + (i) * 50)
#define NET_ROW_Y1(i)      (NET_ROW_Y0(i) + 46)
#define NET_ROW_CONNECT_X0 1060
#define NET_ROW_CONNECT_X1 1220

/* Mod Menu Geometry (Tab 3) */
#define NET_MOD_PAD_Y0     174
#define NET_MOD_PAD_Y1     218
#define NET_MOD_PAD0_X0     60
#define NET_MOD_PAD0_X1    330
#define NET_MOD_PAD1_X0    350
#define NET_MOD_PAD1_X1    620
#define NET_MOD_PAD2_X0    640
#define NET_MOD_PAD2_X1    910
#define NET_MOD_PAD3_X0    930
#define NET_MOD_PAD3_X1   1220

#define NET_MOD_ROW_Y0(i)  (270 + (i) * 44)
#define NET_MOD_ROW_Y1(i)  (NET_MOD_ROW_Y0(i) + 36)
#define NET_MOD_TOGGLE_L_X0 500
#define NET_MOD_TOGGLE_L_X1 610
#define NET_MOD_TOGGLE_R_X0 1100
#define NET_MOD_TOGGLE_R_X1 1210

#define NET_MOD_APPLY_X0   880
#define NET_MOD_APPLY_X1  1220
#define NET_MOD_APPLY_Y0   514
#define NET_MOD_APPLY_Y1   564

/* Tab IDs */
enum {
    NET_TAB_WIFI = 0,
    NET_TAB_BT,
    NET_TAB_HOTSPOT,
    NET_TAB_MODS,
    NET_TAB_STATUS
};

/* Actions emitted by touch release */
enum {
    NET_ACT_NONE = 0,
    NET_ACT_OPEN,
    NET_ACT_CLOSE,
    NET_ACT_SET_TAB,        /* val = tab index */
    NET_ACT_WIFI_TOGGLE,
    NET_ACT_WIFI_SCAN,
    NET_ACT_WIFI_CONNECT,   /* val = network row index */
    NET_ACT_BT_TOGGLE,
    NET_ACT_BT_SCAN,
    NET_ACT_HOTSPOT_TOGGLE,
    NET_ACT_NET_RESTART,
    NET_ACT_MOD_PAD,        /* val = 0..3 */
    NET_ACT_MOD_TOGGLE,     /* val = 0..9 */
    NET_ACT_MOD_RESTART
};

/* State Queries & Manipulation */
int  net_is_open(void);
void net_open(void);
void net_close(void);
void net_toggle(void);
int  net_get_tab(void);
void net_set_tab(int tab);
int  net_get_pressed(void);

/*
 * Pure touch feed: evaluates finger down/up and updates state.
 * Returns MZ_FEED_CONSUMED (swallowed) or MZ_FEED_NONE (passed to rbp).
 * When an action is triggered on finger up, sets *act and *val.
 */
int net_feed(int down, int x, int y, int *act, int *val);

#endif /* RBPI4B_NET_ZONE_H */

/*
 * net_paint.h -- Renderer for OS Network & Wireless Settings overlay on XDJ-RX3.
 *
 * Implements pure pixel-accurate rendering into struct menu_view.
 * Provides the Top Bar Pill Box (Wi-Fi & Bluetooth icons) and the full
 * Network Settings overlay page (y: 50..709).
 */

#ifndef RBPI4B_NET_PAINT_H
#define RBPI4B_NET_PAINT_H

#include "menu_paint.h"      /* struct menu_view, menu_pixel() */
#include "net_zone.h"

#define NET_MAX_WIFI_ITEMS 8

struct net_wifi_item {
    int  in_use;            /* 1 if connected */
    char ssid[36];
    int  signal;            /* 0..100 */
    char security[16];      /* WPA2, OPEN, etc. */
};

struct net_render_state {
    int  wifi_enabled;      /* 1 = ON, 0 = OFF */
    int  wifi_connected;    /* 1 = connected */
    char wifi_ssid[36];     /* current connected SSID */
    char wifi_ip[24];       /* current IP address */

    int  bt_enabled;        /* 1 = ON, 0 = OFF */
    char bt_name[32];       /* adapter / device name */

    int  ap_enabled;        /* 1 = Hotspot ON, 0 = OFF */
    char ap_ssid[32];
    char ap_pass[32];
    char ap_ip[24];
    int  ap_clients;

    char eth_ip[24];
    char hostname[32];

    int  wifi_item_count;
    struct net_wifi_item wifi_items[NET_MAX_WIFI_ITEMS];
};

/* Paint the top bar strip pill box (x: 742..828, y: 7..43) */
void net_strip_paint(const struct menu_view *v,
                     int wifi_active,
                     int bt_active,
                     int pressed);

/* Paint the full network overlay page (x: 0..1279, y: 50..709) */
void net_overlay_paint(const struct menu_view *v,
                       const struct net_render_state *st,
                       int active_tab,
                       int pressed_target,
                       int pressed_val);

#endif /* RBPI4B_NET_PAINT_H */

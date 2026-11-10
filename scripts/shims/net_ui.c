/*
 * net_ui.c -- Coordinator and IPC for OS Network & Wireless Settings overlay.
 *
 * Implements flicker-free double buffering: renders off-screen into s_overlay_buf
 * on state changes, and blits cleanly to real_fb during vsync presentation.
 */

#define _GNU_SOURCE
#include "net_ui.h"
#include "net_paint.h"
#include "net_zone.h"
#include "menu_zone.h"
#include "pointsrc.h"
#include "rbp_key.h"
#include "rbp_abi.h"
#include "tscfake.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>

#define STATE_FILE "/tmp/xdj_net_state.txt"
#define CMD_FIFO   "/tmp/xdj_net_cmd.fifo"

static struct net_render_state s_st;
static int s_inited = 0;
static time_t s_last_state_read = 0;
static int s_pressed_val = -1;

/* Double buffering & dirty tracking */
static unsigned char *s_overlay_buf = NULL;
static size_t s_overlay_buf_len = 0;
static int s_dirty = 1;
static int s_rendered_tab = -1;
static int s_rendered_pressed = -1;
static int s_rendered_val = -1;
static int s_last_open_state = 0;

/* Send asynchronous non-blocking command to background net_daemon */
static void net_send_cmd(const char *cmd)
{
    if (!cmd || !cmd[0]) return;
    int fd = open(CMD_FIFO, O_WRONLY | O_NONBLOCK);
    if (fd >= 0) {
        size_t len = strlen(cmd);
        ssize_t written = write(fd, cmd, len);
        (void)written;
        close(fd);
        pointsrc_log("net_ui: sent command: %s", cmd);
    } else {
        pointsrc_log("net_ui: daemon fifo not open (cmd=%s)", cmd);
    }
}

/* Parse key=value from state file */
static void parse_state_line(char *line)
{
    char *eq = strchr(line, '=');
    if (!eq) return;
    *eq = '\0';
    char *key = line;
    char *val = eq + 1;

    /* Trim newline */
    char *nl = strpbrk(val, "\r\n");
    if (nl) *nl = '\0';

    if (strcmp(key, "wifi_enabled") == 0) {
        int v = atoi(val);
        if (s_st.wifi_enabled != v) { s_st.wifi_enabled = v; s_dirty = 1; }
    } else if (strcmp(key, "wifi_connected") == 0) {
        int v = atoi(val);
        if (s_st.wifi_connected != v) { s_st.wifi_connected = v; s_dirty = 1; }
    } else if (strcmp(key, "wifi_ssid") == 0) {
        if (strncmp(s_st.wifi_ssid, val, sizeof(s_st.wifi_ssid)) != 0) {
            strncpy(s_st.wifi_ssid, val, sizeof(s_st.wifi_ssid) - 1);
            s_dirty = 1;
        }
    } else if (strcmp(key, "wifi_ip") == 0) {
        if (strncmp(s_st.wifi_ip, val, sizeof(s_st.wifi_ip)) != 0) {
            strncpy(s_st.wifi_ip, val, sizeof(s_st.wifi_ip) - 1);
            s_dirty = 1;
        }
    } else if (strcmp(key, "bt_enabled") == 0) {
        int v = atoi(val);
        if (s_st.bt_enabled != v) { s_st.bt_enabled = v; s_dirty = 1; }
    } else if (strcmp(key, "bt_name") == 0) {
        strncpy(s_st.bt_name, val, sizeof(s_st.bt_name) - 1);
    } else if (strcmp(key, "ap_enabled") == 0) {
        int v = atoi(val);
        if (s_st.ap_enabled != v) { s_st.ap_enabled = v; s_dirty = 1; }
    } else if (strcmp(key, "ap_ssid") == 0) {
        strncpy(s_st.ap_ssid, val, sizeof(s_st.ap_ssid) - 1);
    } else if (strcmp(key, "ap_pass") == 0) {
        strncpy(s_st.ap_pass, val, sizeof(s_st.ap_pass) - 1);
    } else if (strcmp(key, "ap_ip") == 0) {
        strncpy(s_st.ap_ip, val, sizeof(s_st.ap_ip) - 1);
    } else if (strcmp(key, "ap_clients") == 0) {
        int v = atoi(val);
        if (s_st.ap_clients != v) { s_st.ap_clients = v; s_dirty = 1; }
    } else if (strcmp(key, "eth_ip") == 0) {
        strncpy(s_st.eth_ip, val, sizeof(s_st.eth_ip) - 1);
    } else if (strcmp(key, "hostname") == 0) {
        strncpy(s_st.hostname, val, sizeof(s_st.hostname) - 1);
    } else if (strcmp(key, "net_count") == 0) {
        int v = atoi(val);
        if (v > NET_MAX_WIFI_ITEMS) v = NET_MAX_WIFI_ITEMS;
        if (s_st.wifi_item_count != v) { s_st.wifi_item_count = v; s_dirty = 1; }
    } else if (strncmp(key, "net_", 4) == 0) {
        int idx = atoi(key + 4);
        if (idx >= 0 && idx < NET_MAX_WIFI_ITEMS) {
            /* Format: in_use:ssid:signal:security */
            char *p1 = strchr(val, ':');
            if (p1) {
                *p1 = '\0';
                s_st.wifi_items[idx].in_use = atoi(val);
                char *p2 = strchr(p1 + 1, ':');
                if (p2) {
                    *p2 = '\0';
                    strncpy(s_st.wifi_items[idx].ssid, p1 + 1, sizeof(s_st.wifi_items[idx].ssid) - 1);
                    char *p3 = strchr(p2 + 1, ':');
                    if (p3) {
                        *p3 = '\0';
                        s_st.wifi_items[idx].signal = atoi(p2 + 1);
                        strncpy(s_st.wifi_items[idx].security, p3 + 1, sizeof(s_st.wifi_items[idx].security) - 1);
                    } else {
                        s_st.wifi_items[idx].signal = atoi(p2 + 1);
                        strncpy(s_st.wifi_items[idx].security, "WPA2", sizeof(s_st.wifi_items[idx].security) - 1);
                    }
                }
            }
        }
    }
}

/* Refresh state from disk */
static void net_state_refresh(void)
{
    time_t now = time(NULL);
    if (now - s_last_state_read < 1 && s_last_state_read != 0)
        return;
    s_last_state_read = now;

    FILE *f = fopen(STATE_FILE, "r");
    if (!f) return;

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        parse_state_line(line);
    }
    fclose(f);
}

void net_ui_init(void)
{
    if (s_inited) return;
    s_inited = 1;

    memset(&s_st, 0, sizeof(s_st));
    s_st.wifi_enabled = 1;
    s_st.wifi_connected = 1;
    strncpy(s_st.wifi_ssid, "NOVA_7F08", sizeof(s_st.wifi_ssid) - 1);
    strncpy(s_st.wifi_ip, "192.168.137.22", sizeof(s_st.wifi_ip) - 1);
    s_st.bt_enabled = 1;
    strncpy(s_st.bt_name, "XDJ-RX3-UNX", sizeof(s_st.bt_name) - 1);
    strncpy(s_st.hostname, "unx.dj", sizeof(s_st.hostname) - 1);

    s_st.wifi_item_count = 2;
    s_st.wifi_items[0].in_use = 1;
    strncpy(s_st.wifi_items[0].ssid, "NOVA_7F08", sizeof(s_st.wifi_items[0].ssid) - 1);
    s_st.wifi_items[0].signal = 100;
    strncpy(s_st.wifi_items[0].security, "WPA2", sizeof(s_st.wifi_items[0].security) - 1);

    s_st.wifi_items[1].in_use = 0;
    strncpy(s_st.wifi_items[1].ssid, "ALBADR", sizeof(s_st.wifi_items[1].ssid) - 1);
    s_st.wifi_items[1].signal = 69;
    strncpy(s_st.wifi_items[1].security, "WPA1 WPA2", sizeof(s_st.wifi_items[1].security) - 1);

    net_state_refresh();
}

/* Debug key trigger: allows switching rbp views directly via /tmp/xdj_rbp_key */
static void check_debug_key_cmd(void)
{
    static time_t s_last_cmd_check = 0;
    time_t now = time(NULL);
    if (now == s_last_cmd_check) return;
    s_last_cmd_check = now;

    FILE *f = fopen("/tmp/xdj_rbp_key", "r");
    if (!f) return;

    char cmd[64] = {0};
    if (fgets(cmd, sizeof(cmd), f)) {
        cmd[strcspn(cmd, "\r\n")] = '\0';
        pointsrc_log("net_ui: debug key command: '%s'", cmd);
        if (strcmp(cmd, "source") == 0) {
            send_rx_key(K_SOURCE, OP_PRESS, CH_GLOBAL, 0);
            send_rx_key(K_SOURCE, OP_RELEASE, CH_GLOBAL, 0);
        } else if (strcmp(cmd, "browse") == 0) {
            send_rx_key(K_BROWSE, OP_PRESS, CH_GLOBAL, 0);
            send_rx_key(K_BROWSE, OP_RELEASE, CH_GLOBAL, 0);
        } else if (strcmp(cmd, "taglist") == 0) {
            send_rx_key(K_TAGLIST, OP_PRESS, CH_GLOBAL, 0);
            send_rx_key(K_TAGLIST, OP_RELEASE, CH_GLOBAL, 0);
        } else if (strcmp(cmd, "playlist") == 0) {
            send_rx_key(K_PLAYLIST, OP_PRESS, CH_GLOBAL, 0);
            send_rx_key(K_PLAYLIST, OP_RELEASE, CH_GLOBAL, 0);
        } else if (strcmp(cmd, "search") == 0) {
            send_rx_key(K_SEARCH, OP_PRESS, CH_GLOBAL, 0);
            send_rx_key(K_SEARCH, OP_RELEASE, CH_GLOBAL, 0);
        } else if (strcmp(cmd, "menu") == 0) {
            send_rx_key(K_MENU, OP_PRESS, CH_GLOBAL, 0);
            send_rx_key(K_MENU, OP_RELEASE, CH_GLOBAL, 0);
        } else if (strcmp(cmd, "utility") == 0) {
            send_rx_key(K_MENU, OP_PRESS, CH_GLOBAL, 0);
            usleep(500000);
            send_rx_key(K_MENU, OP_RELEASE, CH_GLOBAL, 0);
        } else if (strcmp(cmd, "shortcut") == 0) {
            send_rx_key(K_SHORTCUT, OP_PRESS, CH_GLOBAL, 0);
            send_rx_key(K_SHORTCUT, OP_RELEASE, CH_GLOBAL, 0);
        } else if (strcmp(cmd, "info") == 0) {
            send_rx_key(K_INFO, OP_PRESS, CH_GLOBAL, 0);
            send_rx_key(K_INFO, OP_RELEASE, CH_GLOBAL, 0);
        } else if (strcmp(cmd, "back") == 0) {
            send_rx_key(K_BACK, OP_PRESS, CH_GLOBAL, 0);
            send_rx_key(K_BACK, OP_RELEASE, CH_GLOBAL, 0);
        } else if (strcmp(cmd, "open") == 0) {
            net_open();
            s_dirty = 1;
        } else if (strcmp(cmd, "close") == 0) {
            net_close();
            s_dirty = 1;
        } else if (strcmp(cmd, "tab0") == 0) {
            net_set_tab(NET_TAB_WIFI);
            net_open();
            s_dirty = 1;
        } else if (strcmp(cmd, "tab1") == 0) {
            net_set_tab(NET_TAB_BT);
            net_open();
            s_dirty = 1;
        } else if (strcmp(cmd, "tab2") == 0) {
            net_set_tab(NET_TAB_HOTSPOT);
            net_open();
            s_dirty = 1;
        } else if (strcmp(cmd, "tab3") == 0) {
            net_set_tab(NET_TAB_MODS);
            net_open();
            s_dirty = 1;
        } else if (strcmp(cmd, "tab4") == 0) {
            net_set_tab(NET_TAB_STATUS);
            net_open();
            s_dirty = 1;
        } else if (strncmp(cmd, "raw ", 4) == 0) {
            int code = (int)strtol(cmd + 4, NULL, 0);
            if (code > 0) {
                send_rx_key(code, OP_PRESS, CH_GLOBAL, 0);
                send_rx_key(code, OP_RELEASE, CH_GLOBAL, 0);
            }
        }
    }
    fclose(f);
    unlink("/tmp/xdj_rbp_key");
}

int net_ui_is_open(void)
{
    if (!rbp_perf_screen() || menu_is_open()) {
        if (net_is_open()) {
            net_close();
            s_dirty = 1;
        }
        return 0;
    }
    return net_is_open();
}

void net_ui_paint_strip(void *fb, int w, int h, int pitch_px, int bpp)
{
    if (!fb || w <= 0 || h <= 0) return;
    if (!s_inited) net_ui_init();

    if (!rbp_perf_screen() || menu_is_open()) {
        return;
    }

    net_state_refresh();

    struct menu_view v;
    v.dw = w;
    v.dh = h;
    v.bx = 0;
    v.by = 0;
    v.pitch = pitch_px;
    v.bpp = bpp;
    v.pix = fb;

    int pressed_tgt = net_get_pressed();
    int wifi_active = (s_st.wifi_enabled && s_st.wifi_connected);
    int bt_active = s_st.bt_enabled;

    net_strip_paint(&v, wifi_active, bt_active, pressed_tgt);
}

void net_ui_present(void *real_fb, int w, int h, int pitch_px, int bpp)
{
    if (!real_fb || w <= 0 || h <= 0) return;
    if (!s_inited) net_ui_init();

    check_debug_key_cmd();

    /* Strictly limit network buttons and overlay to the Performance screen (waveform view).
     * On Browse, Source, Utility, etc., or when the top menu band is open, do NOT paint anything! */
    if (!rbp_perf_screen() || menu_is_open()) {
        if (net_is_open()) {
            net_close();
            s_dirty = 1;
        }
        return;
    }

    net_state_refresh();

    int open = net_is_open();
    if (open != s_last_open_state) {
        s_dirty = 1;
        s_last_open_state = open;
    }

    /* If overlay is open, render into double buffer and blit cleanly */
    if (open) {
        size_t bpp_bytes = (bpp > 0) ? (bpp / 8) : 4;
        size_t needed = (size_t)w * h * bpp_bytes;

        if (!s_overlay_buf || s_overlay_buf_len < needed) {
            if (s_overlay_buf) free(s_overlay_buf);
            s_overlay_buf = malloc(needed);
            s_overlay_buf_len = needed;
            s_dirty = 1;
        }

        int cur_tab = net_get_tab();
        int cur_prs = net_get_pressed();

        if (s_dirty || cur_tab != s_rendered_tab || cur_prs != s_rendered_pressed || s_pressed_val != s_rendered_val) {
            if (s_overlay_buf) {
                struct menu_view ov;
                ov.dw = w;
                ov.dh = h;
                ov.bx = 0;
                ov.by = 0;
                ov.pitch = pitch_px;
                ov.bpp = bpp;
                ov.pix = s_overlay_buf;

                net_overlay_paint(&ov, &s_st, cur_tab, cur_prs, s_pressed_val);

                s_rendered_tab = cur_tab;
                s_rendered_pressed = cur_prs;
                s_rendered_val = s_pressed_val;
                s_dirty = 0;
            }
        }

        /* Fast single contiguous memory block copy of y 50..709 directly to real_fb */
        if (s_overlay_buf) {
            size_t pitch_bytes = (size_t)pitch_px * bpp_bytes;
            int y0 = NET_WIN_Y0; /* 50 */
            int y1 = NET_WIN_Y1; /* 709 */
            size_t copy_rows = (size_t)(y1 - y0 + 1);
            memcpy((unsigned char *)real_fb + (size_t)y0 * pitch_bytes,
                   (unsigned char *)s_overlay_buf + (size_t)y0 * pitch_bytes,
                   copy_rows * pitch_bytes);
        }
    }
}

int net_ui_touch(int down, int x, int y)
{
    if (!rbp_perf_screen()) {
        if (net_is_open()) {
            net_close();
            s_dirty = 1;
        }
        return 0;
    }

    if (!s_inited) net_ui_init();

    int act = NET_ACT_NONE;
    int val = 0;
    int feed = net_feed(down, x, y, &act, &val);

    if (act == NET_ACT_OPEN) {
        pointsrc_log("net_ui: network settings overlay opened");
        s_dirty = 1;
        net_send_cmd("wifi_scan\n");
    } else if (act == NET_ACT_CLOSE) {
        pointsrc_log("net_ui: network settings overlay closed");
        s_dirty = 1;
    } else if (act == NET_ACT_SET_TAB) {
        pointsrc_log("net_ui: tab switched to %d", val);
        s_dirty = 1;
    } else if (act == NET_ACT_TOP_KEY) {
        pointsrc_log("net_ui: top button KEY tapped -> select KEY pad row");
        tscfake_emit(1, 1135, 390);
        tscfake_emit(0, 1135, 390);
    } else if (act == NET_ACT_TOP_STEMS) {
        pointsrc_log("net_ui: top button STEMS tapped -> select STEMS pad row");
        tscfake_emit(1, 1225, 390);
        tscfake_emit(0, 1225, 390);
    } else if (act == NET_ACT_TOP_RMX) {
        pointsrc_log("net_ui: top button RMX tapped");
        send_rx_key(0x0400, OP_PRESS, 0, 0);
        send_rx_key(0x0400, OP_RELEASE, 0, 0);
    } else if (act == NET_ACT_TOP_FXMOD) {
        pointsrc_log("net_ui: top button FX MOD tapped -> open MOD MENU");
        s_st.wifi_enabled = s_st.wifi_enabled; /* refresh */
        net_open();
        net_ui_touch(1, NET_TAB3_X0 + 10, NET_TAB_Y0 + 10);
        net_ui_touch(0, NET_TAB3_X0 + 10, NET_TAB_Y0 + 10);
        s_dirty = 1;
    } else if (act == NET_ACT_WIFI_TOGGLE) {
        s_st.wifi_enabled = !s_st.wifi_enabled;
        s_dirty = 1;
        pointsrc_log("net_ui: toggle wifi -> %d", s_st.wifi_enabled);
        net_send_cmd("wifi_toggle\n");
    } else if (act == NET_ACT_WIFI_SCAN) {
        s_dirty = 1;
        pointsrc_log("net_ui: wifi scan requested");
        net_send_cmd("wifi_scan\n");
    } else if (act == NET_ACT_WIFI_CONNECT) {
        if (val >= 0 && val < s_st.wifi_item_count) {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "wifi_connect:%s\n", s_st.wifi_items[val].ssid);
            pointsrc_log("net_ui: connect to wifi %s", s_st.wifi_items[val].ssid);
            net_send_cmd(cmd);
        }
    } else if (act == NET_ACT_BT_TOGGLE) {
        s_st.bt_enabled = !s_st.bt_enabled;
        s_dirty = 1;
        pointsrc_log("net_ui: toggle bluetooth -> %d", s_st.bt_enabled);
        net_send_cmd("bt_toggle\n");
    } else if (act == NET_ACT_BT_SCAN) {
        s_dirty = 1;
        pointsrc_log("net_ui: bluetooth scan requested");
        net_send_cmd("bt_scan\n");
    } else if (act == NET_ACT_HOTSPOT_TOGGLE) {
        s_st.ap_enabled = !s_st.ap_enabled;
        s_dirty = 1;
        pointsrc_log("net_ui: toggle hotspot -> %d", s_st.ap_enabled);
        net_send_cmd("ap_toggle\n");
    } else if (act == NET_ACT_NET_RESTART) {
        s_dirty = 1;
        pointsrc_log("net_ui: restart network requested");
        net_send_cmd("restart_net\n");
    } else if (act == NET_ACT_MOD_RESTART) {
        s_dirty = 1;
        pointsrc_log("net_ui: restart rblive4 requested from mod menu");
        system("systemctl restart rblive4 2>/dev/null || systemctl restart rbp 2>/dev/null || pkill -9 rbp &");
    }

    if (s_pressed_val != val) {
        s_pressed_val = val;
        s_dirty = 1;
    }

    return (feed == MZ_FEED_TAKEN) ? 1 : 0;
}

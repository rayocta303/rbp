/*
 * net_ui.h -- Coordinator and public interface for OS Network Settings overlay.
 */

#ifndef RBPI4B_NET_UI_H
#define RBPI4B_NET_UI_H

/* Initialize network UI state and IPC */
void net_ui_init(void);

/*
 * Called every frame during vsync in fb_shim.c.
 * Always renders the top bar Wi-Fi & Bluetooth strip pill.
 * If overlay is open (net_is_open()), renders the network settings overlay.
 */
void net_ui_present(void *real_fb, int w, int h, int pitch_px, int bpp);

/*
 * Called from touch dispatcher in pointsrc.c.
 * Returns 1 if touch was handled/consumed by Network UI.
 * Returns 0 if touch belongs to rbp / standard touch processing.
 */
int net_ui_touch(int down, int x, int y);

/* Query whether the network overlay is currently open */
int net_ui_is_open(void);

#endif /* RBPI4B_NET_UI_H */

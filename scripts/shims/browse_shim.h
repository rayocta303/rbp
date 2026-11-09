/* SPDX-License-Identifier: GPL-2.0-only */
/**
 * @file browse_shim.h
 * @brief Declarations for XDJ-RX3 Browse Mods: Touch Preview Remember,
 *        Custom Columns (Hide Track Number, Expand Title), and Camelot Key.
 */
#ifndef BROWSE_SHIM_H
#define BROWSE_SHIM_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initializes and installs Browse memory hooks into rbp.
 * Called from fb_shim.c during constructor initialization.
 */
void browse_shim_init(void);

/**
 * Periodic tick if needed (called from vsync or timer tick).
 */
void browse_shim_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* BROWSE_SHIM_H */

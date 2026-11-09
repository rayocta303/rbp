/* SPDX-License-Identifier: GPL-2.0-only */
/**
 * @file browse_shim.c
 * @brief Implementation of XDJ-RX3 Browse Mods:
 *        1. Touch Preview / Song Info Remember across reboots
 *        2. Custom Columns / Row Formatting (Hide Track Number, Expand Title)
 *        3. Camelot Key (Mixed-In-Key) notation override
 *
 * Runs inside rbp via fbshim.so (RB_LD_PRELOAD).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>

#include "browse_shim.h"
#include "envutil.h"
#include "pointsrc.h"

/* --- Target VMAs in /root/pdj/rbp (ARM32 EABI5) --- */
#define ADDR_SET_SONGINFO      0x00113c78u  /* setSongInfo(int on) */
#define ADDR_IS_SONGINFO_ON    0x00112e30u  /* IsSongInfoOn() */
#define ADDR_UI_BROWSE         0x0326f8b8u  /* uiBrowse struct base */
#define OFF_SONGINFO_FLAG      0x28u        /* *(uiBrowse + 0x28) = SongInfoOn */

#define ADDR_SET_LINKSTATE     0x0018dca0u  /* ui_com_draw_SetLinkState(win, obj_id, state) */
#define ADDR_SET_POS_LEFT      0x0018e2ccu  /* ui_com_draw_SetObjectPosLeft(win, obj_id, left) */
#define ADDR_IS_KEY_MIXED      0x0010cf88u  /* UiBrowseComm_IsKeyDispStyleMixedInKey() */

/* Expected prologues (prevent patching unexpected binary versions) */
#define PROLOGUE_SET_SONGINFO  0xE30F38B8u  /* movw r3, #0xf8b8 */
#define PROLOGUE_SET_LINKSTATE 0xE3710001u  /* cmn r1, #1 */
#define PROLOGUE_SET_POS_LEFT  0xE92D4038u  /* push {r3, r4, r5, lr} */
#define PROLOGUE_IS_KEY_MIXED  0xE92D4070u  /* push {r4, r5, r6, lr} */

#define STATE_DIR              "/opt/rblive4/settings"
#define STATE_FILE_SONGINFO    STATE_DIR "/browse_songinfo.state"

/* Track Number Object IDs in Left Browse List (12 rows) */
static const int s_track_num_ids[12] = {
    262, 254, 246, 238, 230, 222, 214, 206, 198, 190, 182, 174
};

/* Main Text Object IDs (ID_LINE_TEXT) in Left Browse List (12 rows) */
static const int s_line_text_ids[12] = {
    103, 100, 97, 94, 91, 88, 85, 82, 79, 76, 73, 70
};

/* Original function pointers (trampolines) */
static void (*g_orig_setSongInfo)(int on) = NULL;
static int  (*g_orig_SetLinkState)(void *win, int obj_id, int state) = NULL;
static int  (*g_orig_SetObjectPosLeft)(void *win, int obj_id, int left) = NULL;
static int  (*g_orig_IsKeyMixedInKey)(void) = NULL;

/* Configuration knobs */
static int g_songinfo_remember = 1;
static int g_hide_trackno = 1;
static int g_shift_left_px = 44;
static int g_force_camelot = 1;

static int is_track_num_id(int id)
{
    for (int i = 0; i < 12; i++) {
        if (s_track_num_ids[i] == id)
            return 1;
    }
    return 0;
}

static int is_line_text_id(int id)
{
    for (int i = 0; i < 12; i++) {
        if (s_line_text_ids[i] == id)
            return 1;
    }
    return 0;
}

/* Save Song Info state to persistent storage */
static void save_songinfo_state(int on)
{
    int fd = open(STATE_FILE_SONGINFO, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        char buf[8];
        int len = snprintf(buf, sizeof(buf), "%d\n", on ? 1 : 0);
        write(fd, buf, len);
        close(fd);
        pointsrc_log("browse_shim: saved songinfo state = %d", on ? 1 : 0);
    }
}

/* Load Song Info state from persistent storage (default: 1 if absent) */
static int load_songinfo_state(void)
{
    int fd = open(STATE_FILE_SONGINFO, O_RDONLY);
    if (fd < 0)
        return 1; /* Default to enabled on first install */
    char buf[8] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n > 0) {
        int v = atoi(buf);
        return v ? 1 : 0;
    }
    return 1;
}

/* Hook for setSongInfo(int on) */
static void hook_setSongInfo(int on)
{
    if (g_songinfo_remember)
        save_songinfo_state(on);

    if (g_orig_setSongInfo)
        g_orig_setSongInfo(on);
    else {
        volatile uint32_t *p = (volatile uint32_t *)(ADDR_UI_BROWSE + OFF_SONGINFO_FLAG);
        *p = on ? 1 : 0;
    }
}

/* Hook for ui_com_draw_SetLinkState(void *win, int obj_id, int state) */
static int hook_SetLinkState(void *win, int obj_id, int state)
{
    if (g_hide_trackno && is_track_num_id(obj_id)) {
        /* Force track number column to stay hidden */
        state = 0;
    }
    if (g_orig_SetLinkState)
        return g_orig_SetLinkState(win, obj_id, state);
    return 0;
}

/* Hook for ui_com_draw_SetObjectPosLeft(void *win, int obj_id, int left) */
static int hook_SetObjectPosLeft(void *win, int obj_id, int left)
{
    if (g_hide_trackno && is_line_text_id(obj_id)) {
        /* Shift track title/artist text leftwards into the freed track# space */
        if (left >= g_shift_left_px)
            left -= g_shift_left_px;
    }
    if (g_orig_SetObjectPosLeft)
        return g_orig_SetObjectPosLeft(win, obj_id, left);
    return 0;
}

/* Hook for UiBrowseComm_IsKeyDispStyleMixedInKey() */
static int hook_IsKeyMixedInKey(void)
{
    if (g_force_camelot)
        return 1; /* Force Camelot Alphanumeric key display */
    if (g_orig_IsKeyMixedInKey)
        return g_orig_IsKeyMixedInKey();
    return 0;
}

/* Arm an instruction trampoline hook */
static int install_arm_hook(uintptr_t target_addr, uint32_t expected_prologue,
                            void *hook_fn, void **orig_fn_out)
{
    const size_t pgsz = 4096;
    uint8_t *p = (uint8_t *)target_addr;

    if (*(volatile uint32_t *)p != expected_prologue) {
        pointsrc_log("browse_shim: prologue mismatch at 0x%08x (seen 0x%08x, expected 0x%08x)",
                     target_addr, *(volatile uint32_t *)p, expected_prologue);
        return -1;
    }

    uint8_t *tramp = (uint8_t *)mmap(NULL, pgsz, PROT_READ | PROT_WRITE | PROT_EXEC,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED) {
        pointsrc_log("browse_shim: mmap failed for trampoline at 0x%08x", target_addr);
        return -1;
    }

    /* Copy displaced 8 bytes into trampoline */
    memcpy(tramp, p, 8);
    /* Jump back to target_addr + 8 */
    *(uint32_t *)(tramp + 8) = 0xE51FF004u; /* ldr pc, [pc, #-4] */
    *(uint32_t *)(tramp + 12) = (uint32_t)(target_addr + 8);
    __builtin___clear_cache((char *)tramp, (char *)tramp + 16);

    if (orig_fn_out)
        *orig_fn_out = tramp;

    /* Make target code page writable */
    uintptr_t pg = target_addr & ~(pgsz - 1);
    if (mprotect((void *)pg, pgsz, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        pointsrc_log("browse_shim: mprotect RW failed at 0x%08x: %s", target_addr, strerror(errno));
        return -1;
    }

    /* Patch target entry with ldr pc, [pc, #-4] */
    *(uint32_t *)(p + 0) = 0xE51FF004u;
    *(uint32_t *)(p + 4) = (uint32_t)hook_fn;

    mprotect((void *)pg, pgsz, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + 8);

    pointsrc_log("browse_shim: hook installed at 0x%08x -> %p (tramp=%p)",
                 target_addr, hook_fn, tramp);
    return 0;
}

void browse_shim_init(void)
{
    /* Load configuration from environment */
    g_songinfo_remember = env_flag("BROWSE_SONGINFO_REMEMBER", 1);
    g_hide_trackno      = env_flag("BROWSE_HIDE_TRACKNO", 1);
    g_shift_left_px     = env_flag("BROWSE_SHIFT_LEFT_PX", 44);
    g_force_camelot     = env_flag("BROWSE_CAMELOT_KEY", 1);

    pointsrc_log("browse_shim: init: songinfo_remember=%d hide_trackno=%d shift=%dpx camelot=%d",
                 g_songinfo_remember, g_hide_trackno, g_shift_left_px, g_force_camelot);

    /* 1. Touch Preview / Song Info Remember hook */
    if (g_songinfo_remember) {
        install_arm_hook(ADDR_SET_SONGINFO, PROLOGUE_SET_SONGINFO,
                         (void *)hook_setSongInfo, (void **)&g_orig_setSongInfo);

        /* Pre-seed state from saved file */
        int saved_state = load_songinfo_state();
        volatile uint32_t *p_flag = (volatile uint32_t *)(ADDR_UI_BROWSE + OFF_SONGINFO_FLAG);
        *p_flag = saved_state ? 1 : 0;
        pointsrc_log("browse_shim: pre-seeded Song Info state to %d", saved_state);
    }

    /* 2. Custom Columns: Hide Track Number & Expand Title */
    if (g_hide_trackno) {
        install_arm_hook(ADDR_SET_LINKSTATE, PROLOGUE_SET_LINKSTATE,
                         (void *)hook_SetLinkState, (void **)&g_orig_SetLinkState);
        install_arm_hook(ADDR_SET_POS_LEFT, PROLOGUE_SET_POS_LEFT,
                         (void *)hook_SetObjectPosLeft, (void **)&g_orig_SetObjectPosLeft);
    }

    /* 3. Camelot Key display override */
    if (g_force_camelot) {
        install_arm_hook(ADDR_IS_KEY_MIXED, PROLOGUE_IS_KEY_MIXED,
                         (void *)hook_IsKeyMixedInKey, (void **)&g_orig_IsKeyMixedInKey);
    }
}

__attribute__((constructor))
static void browse_shim_ctor(void)
{
    browse_shim_init();
}

void browse_shim_tick(void)
{
    /* Periodic checks or telemetry if needed */
}

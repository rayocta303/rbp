/*
 * map_flx6.c -- Pioneer DJ / AlphaTheta DDJ-FLX6 control surface mapping.
 *
 * PROVENANCE:
 * Every note, CC, and channel assignment is taken directly from Pioneer DJ's
 * official publication: "DDJ-FLX6 List of MIDI messages" (Ver 1.0).
 *
 * Architecture:
 * - 2 Decks (MIDI channels 1 & 2 -> 0-based rch 0 & 1):
 *   Decks 1 & 2 route to Left & Right Players (sch = 1 & 2).
 *   Decks 3 & 4 are disabled for clean 2-deck standalone operation on XDJ-RX3.
 * - 2-Channel Mixer Strip (Ch 1 & 2):
 *   Trim (CC 4), EQ Hi (CC 7), EQ Mid (CC 11), EQ Low (CC 15), Fader (CC 19),
 *   Headphone Cue (Note 84).
 *   Sound Color FX / Filter (CC 23, 24 on Ch 7).
 * - SHIFT + HEADPHONE CUE 1-4:
 *   Instantly selects Sound Color FX (Filter, Dub Echo, Noise, Space, Crush, Sweep).
 * - Dedicated Browse / Navigation on Ch 7:
 *   Browse turn (CC 64) / push (Note 65), Shift-push Source (Note 66),
 *   BACK button (Note 101 -> K_BACK), VIEW button (Note 122 -> K_BROWSE),
 *   SHIFT + VIEW button (Note 104 -> K_SOURCE),
 *   Dedicated Load 1 & 2 buttons (Notes 70 & 71).
 * - 8 Performance Pads per deck across 4 modes (Hot Cue, Beat Loop, Slip Loop, Beat Jump).
 * - Relative Jog Wheels (0x40-centred) for Decks 1 & 2.
 * - 14-bit Tempo Sliders for Decks 1 & 2.
 * - Beat FX & Merge FX controls (Slots 1-3 on FX1 and FX2).
 * - Channel Level Meters (CC 2, banded 0x26..0x7F).
 * - Dynamic hot reload of Color FX and Beat FX slot configurations from /run/rblive4/flx6_fx.conf.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sound/asequencer.h>
#include <pthread.h>
#include <unistd.h>

#include "shimutil.h"
#include "shmstate.h"
#include "rbp_abi.h"
#include "rbp_bridge.h"
#include "rbp_vu.h"
#include "pitch_state.h"
#include "ctrl_map.h"
#include "midi_io.h"
#include "rbp_transport.h"

/* ---- MIDI receive channels, 0-based (List channel minus 1) ---- */
#define CH_DECK1  0   /* List ch 1  -- deck 1 */
#define CH_DECK2  1   /* List ch 2  -- deck 2 */
#define CH_DECK3  2   /* List ch 3  -- deck 3 */
#define CH_DECK4  3   /* List ch 4  -- deck 4 */
#define CH_FX1    4   /* List ch 5  -- Beat FX 1 */
#define CH_FX2    5   /* List ch 6  -- Beat FX 2 */
#define CH_MIX    6   /* List ch 7  -- mixer knobs, browse, master */

/* Pads (Normal / +SHIFT) */
#define CH_PADS1  7   /* List ch 8  -- deck 1 pads */
#define CH_PADS1S 8   /* List ch 9  -- deck 1 pads +SHIFT */
#define CH_PADS2  9   /* List ch 10 -- deck 2 pads */
#define CH_PADS2S 10  /* List ch 11 -- deck 2 pads +SHIFT */
#define CH_PADS3  11  /* List ch 12 -- deck 3 pads */
#define CH_PADS3S 12  /* List ch 13 -- deck 3 pads +SHIFT */
#define CH_PADS4  13  /* List ch 14 -- deck 4 pads */
#define CH_PADS4S 14  /* List ch 15 -- deck 4 pads +SHIFT */

/* ---- Deck button notes (channels 0..1) ---- */
#define N_PLAY        11   /* PLAY/PAUSE (+SHIFT 71) */
#define N_CUE         12   /* CUE        (+SHIFT 72) */
#define N_SHIFT       63   /* SHIFT */
#define N_JOG_TOUCH   54   /* Platter touch (+SHIFT 103) */
#define N_IN          16   /* Loop IN    (+SHIFT 76) */
#define N_OUT         17   /* Loop OUT   (+SHIFT 119) */
#define N_IN_4BEAT    20   /* Long-press IN -> Auto 4-Beat Loop */
#define N_IN_SHIFT    76   /* +SHIFT IN -> Auto 4-Beat Loop */
#define N_4BEAT       77   /* Reloop/Exit */
#define N_RELOOP_SHIFT 80  /* +SHIFT Reloop/Exit */
#define N_CALL_PREV   81   /* CUE/LOOP CALL < */
#define N_CALL_NEXT   83   /* CUE/LOOP CALL > */
#define N_CALL_PREV_SHIFT 97 /* +SHIFT CUE/LOOP CALL < -> Track Search Prev */
#define N_CALL_NEXT_SHIFT 98 /* +SHIFT CUE/LOOP CALL > -> Track Search Next */
#define N_MEM_CUE     61   /* CUE/LOOP MEMORY */
#define N_MEM_DEL     62   /* +SHIFT CUE/LOOP MEMORY -> Delete */
#define N_SYNC        88   /* BEAT SYNC  (+SHIFT 93) */
#define N_SYNC_SHIFT  93   /* 1-15L/R +SHIFT BEAT SYNC -> Master Tempo / Key Lock */
#define N_MASTER      92   /* MASTER */
#define N_TEMPO_RANGE 96   /* +SHIFT MASTER -> Tempo Range (+-6/10/16/WIDE) */
#define N_CH_CUE      84   /* Channel CUE (PFL) */
#define N_CH_CUE_SHIFT 104 /* +SHIFT Channel CUE 1-4 -> Color FX */

/* Pad mode buttons (Positional mapping for RX3) */
#define N_MODE_HOT    27   /* HOT CUE */
#define N_MODE_PFX1   30   /* PAD FX 1 -> AUTO BEAT LOOP */
#define N_MODE_JUMP   32   /* BEAT JUMP -> SLIP BEAT LOOP */
#define N_MODE_SMPL   34   /* SAMPLER -> BEAT JUMP */

/* Deck select toggle notes */
#define N_DECK_TOGGLE 114  /* Deck 3/4 button press (ignored on RX3) */
#define N_DECK_STATE  60   /* Deck state feedback */
#define N_DECK_L_SHIFT 124 /* 1-5L SHIFT + DECK L -> SLIP Deck 1 (on CH_MIX) */
#define N_DECK_R_SHIFT 125 /* 1-5R SHIFT + DECK R -> SLIP Deck 2 (on CH_MIX) */

/* ---- Mixer & Browse buttons on CH_MIX (ch 6) ---- */
#define N_BROWSE_PUSH       65  /* Browse knob push */
#define N_BROWSE_PUSH_SHIFT 66  /* SHIFT + browse knob push -> SOURCE screen */
#define N_BACK              101 /* Dedicated BACK button */
#define N_VIEW              122 /* Dedicated VIEW button -> BROWSE screen */
#define N_VIEW_SHIFT        104 /* Dedicated SHIFT + VIEW button -> SOURCE screen */
#define N_LOAD1             70  /* LOAD deck 1 */
#define N_LOAD2             71  /* LOAD deck 2 */
#define N_LOAD3             72  /* LOAD deck 3 */
#define N_LOAD4             73  /* LOAD deck 4 */
#define N_LOAD1_SHIFT       88  /* SHIFT + LOAD 1 -> TRACK FILTER (K_TRACKFILTER) */
#define N_LOAD2_SHIFT       89  /* SHIFT + LOAD 2 -> SHORTCUT */
#define N_LOAD3_SHIFT       90  /* SHIFT + LOAD 3 -> Buka / Tutup Menu UTILITY / Settings (K_MENU) */
#define N_MASTER_CUE        99  /* MASTER CUE */

/* ---- Beat FX notes (channels 4 & 5) ---- */
#define N_FX_ONOFF          71  /* Beat FX ON/OFF */
#define N_FX_BEATPREV       6   /* BEAT < */
#define N_FX_BEATNEXT       7   /* BEAT > */
#define N_FX_SELECT1        112 /* FX Select 1 */
#define N_FX_SELECT2        113 /* FX Select 2 */
#define N_FX_SELECT3        114 /* FX Select 3 */
#define N_MERGE_FX_PRESS    46  /* Merge FX push */

/* ---- Continuous Controls (CCs) ---- */
#define CC_TEMPO_MSB        0
#define CC_TEMPO_LSB        32
#define CC_JOG_WHEEL        33  /* Outer ring relative 0x40 */
#define CC_JOG_VINYL        34  /* Platter vinyl on */
#define CC_JOG_NOVINYL      35  /* Platter vinyl off */
#define CC_JOG_SHIFT        41
#define CC_TRIM             4
#define CC_EQH              7
#define CC_EQM              11
#define CC_EQL              15
#define CC_FADER            19

/* Mixer & Global CCs (CH_MIX = 6) */
#define CC_XFADER           31
#define CC_MASTER_MSB       8
#define CC_MASTER_LSB       40
#define CC_BOOTH            9
#define CC_HP_MIX           12
#define CC_HP_LEVEL         13
#define CC_CFX1             23  /* Filter / Color FX CH1 */
#define CC_CFX2             24  /* Filter / Color FX CH2 */
#define CC_CFX3             25  /* Filter / Color FX CH3 */
#define CC_CFX4             26  /* Filter / Color FX CH4 */
#define CC_BROWSE           64  /* Browse relative 0x01/0x7F */
#define CC_BROWSE_SHIFT     100
#define CC_FX_DEPTH         2   /* Beat FX Level/Depth (ch 4 & 5) */
#define CC_MERGE_FX         8   /* Merge FX rotary (ch 4 & 5) */

#define MIRROR_GAIN_MID_DEFAULT 0.6

/* ---- Runtime calibration variables ---- */
static int knob_scale = 1;
static int jog_scale = 1;
static int jog_ppr = 720;
static int jog_rev = 0;
static int jog_idle_ms = 120;
static int jog_verbose = 0;
static int tempo_verbose = 0;

static void *flx6_trigger_settings(void *arg)
{
     (void)arg;
     int mode = 0;
     if (is_rbp_process()) {
          mode = *(volatile int *)(uintptr_t)(UIBROWSE_GLOBAL + UIBROWSE_OFF_MODE);
     }
     if (mode == BROWSE_MODE_UTILITY) {
          /* If already in UTILITY, send K_BACK to exit */
          send_rx_key(K_BACK, OP_PRESS, CH_GLOBAL, 0);
          send_rx_key(K_BACK, OP_RELEASE, CH_GLOBAL, 0);
          klog("knobshim2: FLX6 Settings -> Exiting UTILITY screen\n");
     } else {
          /* Hold K_MENU (0x0206) for 500ms to open rbp's native UTILITY / Settings */
          send_rx_key(K_MENU, OP_PRESS, CH_GLOBAL, 0);
          usleep(500000);
          send_rx_key(K_MENU, OP_RELEASE, CH_GLOBAL, 0);
          klog("knobshim2: FLX6 Settings -> Opening UTILITY screen (K_MENU held 500ms)\n");
     }
     return NULL;
}
static int tempo_rev = 0;

#define BFX_TYPE_POSITIONS 14

struct jog_ctrl {
     int rch;
     int sch;
     unsigned int vpos;
     float speed;
     unsigned long long last_ms;
     int moving;
     int started;
};

static struct jog_ctrl jog_state[2];

struct flx6_pitch_ctrl {
     int have_hi;
     int pos;
};

static struct flx6_pitch_ctrl flx6_pitch_state[2] = {
     { 0, 0x2000 },
     { 0, 0x2000 },
};

/* Sound Color FX slots (assigned to SHIFT + HEADPHONE CUE 1-4) */
static int flx6_cfx_slots[4] = {
     K_FILTER,   /* Slot 1: Filter */
     K_DUBECHO,  /* Slot 2: Dub Echo */
     K_NOISE,    /* Slot 3: Noise */
     K_SPACE     /* Slot 4: Space */
};
static int flx6_active_cfx_slot = 0;

struct flx6_bfx_slot {
     int fx_type;    /* BFX switch position (0..13) */
     int channel;    /* 0=Deck 1, 1=Deck 2, BFX_CH_MASTER=Master */
     int depth;      /* 0..127 */
     int on;         /* 0=off, 1=on */
};

/* 6 slots on DDJ-FLX6:
 * Slots 0..2: FX Rack 1 (ch 4, 0x94), Notes 71..73, CCs 2, 4, 6
 * Slots 3..5: FX Rack 2 (ch 5, 0x95), Notes 71..73, CCs 2, 4, 6 */
static struct flx6_bfx_slot flx6_slots[6] = {
     { 0, 0, 64, 0 },  /* FX1-1: Delay,   Deck 1 (Note 71, CC 2) */
     { 1, 0, 64, 0 },  /* FX1-2: Echo,    Deck 1 (Note 72, CC 4) */
     { 3, 0, 64, 0 },  /* FX1-3: Spiral,  Deck 1 (Note 73, CC 6) */
     { 4, 1, 64, 0 },  /* FX2-1: Reverb,  Deck 2 (Note 71, CC 2) */
     { 6, 1, 64, 0 },  /* FX2-2: Flanger, Deck 2 (Note 72, CC 4) */
     { 9, 1, 64, 0 },  /* FX2-3: Roll,    Deck 2 (Note 73, CC 6) */
};
static int flx6_active_slot = 0;
static int bfx_led_blink = 0;
static unsigned long long bfx_led_last_ms = 0;
static int bfx_led_last_sent[6] = { -1, -1, -1, -1, -1, -1 };
static int last_seen_rbp_fx_pos = -1;

static const char *flx6_cfx_name(int key)
{
     switch (key) {
     case K_FILTER:  return "Filter";
     case K_DUBECHO: return "Dub Echo";
     case K_NOISE:   return "Noise";
     case K_SPACE:   return "Space";
     case K_CRUSH:   return "Crush";
     case K_SWEEP:   return "Sweep";
     default:        return "Unknown";
     }
}

static int flx6_cfx_from_name(const char *name)
{
     if (!name) return K_FILTER;
     if (!strcasecmp(name, "filter"))  return K_FILTER;
     if (!strcasecmp(name, "dubecho") || !strcasecmp(name, "dub echo")) return K_DUBECHO;
     if (!strcasecmp(name, "noise"))   return K_NOISE;
     if (!strcasecmp(name, "space"))   return K_SPACE;
     if (!strcasecmp(name, "crush"))   return K_CRUSH;
     if (!strcasecmp(name, "sweep"))   return K_SWEEP;
     return K_FILTER;
}

static void flx6_select_cfx_slot(int slot)
{
     if (slot < 0 || slot >= 4) return;
     int key = flx6_cfx_slots[slot];
     if (key <= 0) return;

     send_rx_key(key, OP_PRESS, 1, 0);
     send_rx_key(key, OP_RELEASE, 1, 0);
     send_rx_key(key, OP_PRESS, 2, 0);
     send_rx_key(key, OP_RELEASE, 2, 0);
     flx6_active_cfx_slot = slot;

     klog("knobshim2: FLX6 SHIFT+CUE%d -> Color FX Slot %d: %s (0x%04x)\n",
          slot + 1, slot + 1, flx6_cfx_name(key), key);
}

static unsigned long long flx6_cfg_last_check = 0;

static void flx6_load_fx_config(void)
{
     const char *path = "/run/rblive4/flx6_fx.conf";
     FILE *f = fopen(path, "r");
     if (!f) {
          path = "/opt/rblive4/flx6_fx.conf";
          f = fopen(path, "r");
     }
     if (!f) return;

     int changed = 0;
     char line[128];
     while (fgets(line, sizeof line, f)) {
          char *p = line;
          while (*p == ' ' || *p == '\t') p++;
          if (*p == '#' || *p == '\0' || *p == '\r' || *p == '\n') continue;

          char *eq = strchr(p, '=');
          if (!eq) continue;
          *eq = '\0';
          char *val = eq + 1;
          char *k_end = eq - 1;
          while (k_end > p && (*k_end == ' ' || *k_end == '\t')) *k_end-- = '\0';
          while (*val == ' ' || *val == '\t') val++;
          char *v_end = val + strlen(val) - 1;
          while (v_end >= val && (*v_end == '\r' || *v_end == '\n' || *v_end == ' ' || *v_end == '\t'))
               *v_end-- = '\0';

          if (!strncmp(p, "cfx_slot", 8)) {
               int s = p[8] - '1';
               if (s >= 0 && s < 4) {
                    int k = flx6_cfx_from_name(val);
                    if (flx6_cfx_slots[s] != k) {
                         flx6_cfx_slots[s] = k;
                         changed = 1;
                    }
               }
          } else if (!strncmp(p, "bfx_slot", 8)) {
               int s = p[8] - '1';
               if (s >= 0 && s < 6) {
                    if (strstr(p, "_type")) {
                         int t = atoi(val);
                         if (t >= 0 && t < 14 && flx6_slots[s].fx_type != t) {
                              flx6_slots[s].fx_type = t;
                              changed = 1;
                         }
                    } else if (strstr(p, "_ch")) {
                         int c = atoi(val);
                         if ((c == 0 || c == 1 || c == BFX_CH_MASTER) && flx6_slots[s].channel != c) {
                              flx6_slots[s].channel = c;
                              changed = 1;
                         }
                    }
               }
          }
     }
     fclose(f);
     if (changed)
          klog("knobshim2: FLX6 FX configuration updated from %s\n", path);
}

static void flx6_check_fx_config(void)
{
     unsigned long long now = shim_now_ms();
     if (now - flx6_cfg_last_check < 500) return;
     flx6_cfg_last_check = now;

     flx6_load_fx_config();
}

static void flx6_log_only(int ch, int note)
{
     if (verbose)
          klog("knobshim2: ch%d note%d (unmapped)\n", ch, note);
}

/* Master volume tracking for HDMI mirror gain */
static int mastervol_have_hi = 0;
static int mastervol_pos = 0;
static double mastervol_mid = MIRROR_GAIN_MID_DEFAULT;

static float flx6_mastervol_gain(int pos)
{
     double mid = mastervol_mid;
     double g;
     if (!(mid > 0.0) || mid > 1.0)
          mid = MIRROR_GAIN_MID_DEFAULT;
     g = ((double)pos / 16383.0) / mid;
     if (g > 1.0) g = 1.0;
     return (float)g;
}

static void flx6_mastervol(int cc, int val)
{
     if (cc == CC_MASTER_MSB) {
          mastervol_have_hi = 1;
          mastervol_pos = (mastervol_pos & 0x7f) | (val << 7);
          return;
     }
     mastervol_pos = (mastervol_pos & 0x3f80) | (val & 0x7f);
     if (!mastervol_have_hi)
          return;
     int pos = mastervol_pos;
     if (pos < 0) pos = 0;
     if (pos > 0x3FFF) pos = 0x3FFF;
     g_mirror_gain = flx6_mastervol_gain(pos);
}

/* 14-bit pitch fader across 2 decks */
static void flx6_pitch(int ch, int cc, int val)
{
     if (ch < 0 || ch > 1) return;
     int idx = ch;
     int sch = idx + 1;
     struct flx6_pitch_ctrl *s = &flx6_pitch_state[ch];

     if (cc == CC_TEMPO_MSB) {
          s->have_hi = 1;
          s->pos = (s->pos & 0x7f) | (val << 7);
          return;
     }
     if (cc != CC_TEMPO_LSB)
          return;
     s->pos = (s->pos & 0x3f80) | (val & 0x7f);
     if (!s->have_hi)
          return;

     int pos = s->pos;
     if (pos < 0) pos = 0;
     if (pos > 0x3FFF) pos = 0x3FFF;
     float norm = ((float)pos - 0x2000) / 8192.0f;
     if (norm > 1.0f) norm = 1.0f;
     if (norm < -1.0f) norm = -1.0f;
     if (tempo_rev) norm = -norm;
     int v10 = (int)((norm + 1.0f) * 511.5f);
     if (v10 < 0) v10 = 0;
     if (v10 > 1023) v10 = 1023;

     pitch_state_set(idx, norm);
     send_rx_key_fl(K_TEMPO_SLIDER, OP_VALUE, sch, (long)v10, norm, (long)pos);
     if (tempo_verbose)
          klog("knobshim2: pitch ch%d (deck %d) pos=0x%04x -> norm=%.3f v10=%d\n",
               ch, sch, pos, (double)norm, v10);
}

static int g_xfader_enabled = 1;

static void flx6_sync_crossfader(void)
{
     static unsigned long long last_check = 0;
     static int cached_curve = -1;
     static int cached_enabled = -1;
     unsigned long long now = shim_now_ms();
     if (now - last_check < 500) return;
     last_check = now;
     void *me = mixer_engine();
     if (!me) return;

     int en = 1;
     FILE *fe = fopen("/run/rblive4/xfader_enable", "r");
     if (!fe) fe = fopen("/opt/rblive4/xfader_enable", "r");
     if (fe) {
          int v = 1;
          if (fscanf(fe, "%d", &v) == 1 && (v == 0 || v == 1))
               en = v;
          fclose(fe);
     }

     int cv = 1;
     FILE *fc = fopen("/run/rblive4/xfader_curve", "r");
     if (!fc) fc = fopen("/opt/rblive4/xfader_curve", "r");
     if (fc) {
          int v = 1;
          if (fscanf(fc, "%d", &v) == 1 && v >= 0 && v <= 2)
               cv = v;
          fclose(fc);
     }

     if (en != cached_enabled) {
          cached_enabled = en;
          g_xfader_enabled = en;
          if (en) {
               me_set_crossfader_assign(0, 1); /* Ch1 -> A */
               me_set_crossfader_assign(1, 2); /* Ch2 -> B */
               me_set_crossfader_curve(cv);
               cached_curve = cv;
               klog("knobshim2: crossfader enabled (Ch1->A, Ch2->B, curve=%d)\n", cv);
          } else {
               me_set_crossfader_assign(0, 0); /* Ch1 -> Thru */
               me_set_crossfader_assign(1, 0); /* Ch2 -> Thru */
               klog("knobshim2: crossfader disabled (both channels THRU)\n");
          }
     } else if (en && cv != cached_curve) {
          cached_curve = cv;
          me_set_crossfader_curve(cv);
          klog("knobshim2: crossfader curve changed to %d (current=%d)\n", cv, me_get_crossfader_curve());
     }
}

static float flx6_get_jog_multiplier(void)
{
     static unsigned long long last_check = 0;
     static float cached_mult = 1.0f;
     unsigned long long now = shim_now_ms();
     if (now - last_check >= 500) {
          last_check = now;
          FILE *f = fopen("/run/rblive4/jog_speed", "r");
          if (!f) f = fopen("/opt/rblive4/jog_speed", "r");
          if (f) {
               float val = 1.0f;
               if (fscanf(f, "%f", &val) == 1 && val >= 0.01f && val <= 1.0f) {
                    cached_mult = val;
               }
               fclose(f);
          }
     }
     return cached_mult;
}

static float jog_accum[2] = {0.0f, 0.0f};

/* Relative jog handler */
static void flx6_jog(int ch, int val)
{
     if (ch < 0 || ch > 1) return;
     int d_idx = ch;
     struct jog_ctrl *s = &jog_state[d_idx];
     int d = val - 0x40;
     if (d == 0) return;

     unsigned long long t = shim_now_ms();
     if (!s->started) {
          s->started = 1;
          s->last_ms = t;
          return;
     }
     if (jog_rev) d = -d;
     float mult = flx6_get_jog_multiplier();
     jog_accum[d_idx] += (float)d * (float)jog_scale * mult;
     int dp = (int)jog_accum[d_idx];
     jog_accum[d_idx] -= (float)dp;
     if (dp == 0) return;

     if (dp > 4096) dp = 4096;
     if (dp < -4096) dp = -4096;

     float dt = (float)(long long)(t - s->last_ms) / 1000.0f;
     s->last_ms = t;
     if (dt < 0.0005f) dt = 0.0005f;

     s->vpos = (unsigned int)(s->vpos + (unsigned int)dp) & 0xFFFFu;
     float speed = ((float)d / (float)jog_ppr / dt) * mult;
     if (speed > 8.0f) speed = 8.0f;
     if (speed < -8.0f) speed = -8.0f;

     s->moving = 1;
     s->speed = speed;
     send_rx_key_fl(K_JOG_ROT, OP_ROTATE, s->sch, 0, speed, (long)s->vpos);
     if (jog_verbose)
          klog("knobshim2: jog ch%d delta=%d mult=%.2f speed=%.2f pos=%u (sch%d)\n",
               ch, d, (double)mult, (double)speed, s->vpos, s->sch);
}

/* Jog idle handler */
static void flx6_jog_idle(void)
{
     unsigned long long t = shim_now_ms();
     for (int i = 0; i < 2; i++) {
          struct jog_ctrl *s = &jog_state[i];
          if (!s->started || !s->moving)
               continue;
          if ((unsigned long long)(t - s->last_ms) < (unsigned long long)jog_idle_ms)
               continue;
          s->moving = 0;
          s->speed = 0.0f;
          send_rx_key_fl(K_JOG_ROT, OP_ROTATE, s->sch, 0, 0.0f, (long)s->vpos);
          if (jog_verbose)
               klog("knobshim2: jog ch%d idle -> speed 0\n", s->rch);
     }
}

/* Browse knob relative delta */
static void flx6_browse(int val)
{
     if (val < 0 || val > 127) return;
     int delta = (val >= 64) ? (val - 128) : val;
     if (delta == 0) return;
     long n = (long)delta * (long)knob_scale;
     send_rx_key(K_SELECTOR, OP_ROTATE, CH_GLOBAL, n < 0 ? -1 : 1);
     if (verbose)
          klog("knobshim2: browse knob v=%d delta=%ld -> K_SELECTOR rotate\n", val, n);
}

/* CC dispatch */
static void flx6_cc_abs(int ch, int cc, int val)
{
     for (int i = 0; i < abs_map_n; i++) {
          if (abs_map[i].rch != ch || abs_map[i].cc != cc)
               continue;
          int v = cc_to_10bit(val);
          if (v == abs_map[i].last)
               return;
          abs_map[i].last = v;
          int key = abs_map[i].key;
          int sch = abs_map[i].sch;
          float fval = (float)v / 1023.0f;
          if (key == K_FADER && sch >= 1 && sch <= 2) {
               g_fader[sch] = v;
               g_fader_seen[sch] = 1;
               me_set_channel_fader(sch - 1, fval);
          } else if (key == K_XFADER) {
               flx6_sync_crossfader();
               if (g_xfader_enabled) {
                    me_set_crossfader(fval);
                    send_rx_key_f(key, OP_VALUE, 0, v, fval);
                    if (verbose)
                         klog("knobshim2: crossfader val=%d f=%.3f (curve=%d)\n",
                              v, (double)fval, me_get_crossfader_curve());
               } else {
                    if (verbose)
                         klog("knobshim2: crossfader ignored (disabled)\n");
               }
               return;
          } else if (key == K_HPMIX) {
               g_cue_mix = fval;
          } else if (key == K_HPLEVEL) {
               g_cue_gain = fval;
          }
          send_rx_key_f(key, OP_VALUE, sch, v, fval);
          if (verbose)
               klog("knobshim2: ch%d cc%d -> 0x%04x val=%d f=%.3f (sch%d)\n",
                    ch, cc, key, v, (double)fval, sch);
          return;
     }
     if (verbose)
          klog("knobshim2: unmapped ch%d cc%d val=%d\n", ch, cc, val);
}

/* ---- Build mapping tables ---- */
static void flx6_build(void)
{
     knob_scale = env_num("KNOB_SCALE", 1);
     if (knob_scale < 1) knob_scale = 1;
     jog_scale = env_num("JOG_SCALE", 1);
     if (jog_scale < 1) jog_scale = 1;
     jog_ppr = env_num("JOG_PPR", 720);
     if (jog_ppr < 1) jog_ppr = 1;
     jog_rev = env_on("JOG_REV", 0);
     jog_idle_ms = env_num("JOG_IDLE_MS", 120);
     if (jog_idle_ms < 10) jog_idle_ms = 10;
     mastervol_mid = env_dnum("MIRROR_GAIN_MID", MIRROR_GAIN_MID_DEFAULT);
     jog_verbose = env_on("JOG_VERBOSE", 0);
     tempo_verbose = env_on("TEMPO_VERBOSE", 0);
     tempo_rev = env_on("TEMPO_REV", 0);

     /* Initialize jog state (only Decks 1 & 2) */
     for (int d = 0; d < 2; d++) {
          jog_state[d].rch = d;
          jog_state[d].sch = d + 1;
          jog_state[d].vpos = 0;
          jog_state[d].speed = 0.0f;
          jog_state[d].last_ms = 0;
          jog_state[d].moving = 0;
          jog_state[d].started = 0;
     }

     /* ---- 4 Decks (ch 0..3: Deck 1, 2, 3, 4) ---- */
     for (int d = 0; d < 4; d++) {
          int rch = d;
          int side = (d == 0 || d == 2) ? 0 : 1;
          int sch = side + 1;

          /* Transport & Loop */
          add_note(rch, N_PLAY, K_PLAY, sch);
          add_note(rch, N_CUE, K_CUE, sch);
          add_note(rch, N_JOG_TOUCH, K_JOG_TOUCH, sch);
          add_note(rch, N_IN, K_LOOPIN, sch);
          add_note(rch, N_OUT, K_LOOPOUT, sch);
          add_note(rch, N_4BEAT, K_RELOOP, sch);
          add_note(rch, N_SYNC, K_SYNC, sch);
          add_note(rch, N_SYNC_SHIFT, K_MT, sch);
          add_note(rch, N_MASTER, K_MASTER, sch);
          add_note(rch, N_TEMPO_RANGE, K_TEMPO_RANGE, sch);
          add_note(rch, N_SHIFT, 0, sch);

          /* Memory Cue & Call / Track Search */
          add_note(rch, N_CALL_PREV, K_CALL_PREV, sch);
          add_note(rch, N_CALL_NEXT, K_CALL_NEXT, sch);
          add_note(rch, N_CALL_PREV_SHIFT, K_TRREV, sch);
          add_note(rch, N_CALL_NEXT_SHIFT, K_TRFWD, sch);
          add_note(rch, N_MEM_CUE, K_MEM_CUE, sch);
          add_note(rch, N_MEM_DEL, K_MEM_DEL, sch);

          /* Pad mode buttons (positional mapping for RX3) */
          add_note(rch, N_MODE_HOT, K_HOTCUE, sch);
          add_note(rch, N_MODE_PFX1, K_ALOOP, sch);
          add_note(rch, N_MODE_JUMP, K_SLIPLOOP, sch);
          add_note(rch, N_MODE_SMPL, K_BEATJUMP, sch);

          /* Mixer strip on deck channels */
          add_abs(rch, CC_TRIM, K_TRIM, sch);
          add_abs(rch, CC_EQH, K_EQH, sch);
          add_abs(rch, CC_EQM, K_EQM, sch);
          add_abs(rch, CC_EQL, K_EQL, sch);
          add_abs(rch, CC_FADER, K_FADER, sch);
     }

     /* ---- 8 Performance Pads per deck (Decks 1, 2, 3, 4) ---- */
     static const int pad_channels[4] = { CH_PADS1, CH_PADS2, CH_PADS3, CH_PADS4 };
     for (int d = 0; d < 4; d++) {
          int rch = pad_channels[d];
          int side = (d == 0 || d == 2) ? 0 : 1;
          int sch = side + 1;
          for (int p = 0; p < 8; p++) {
               add_note(rch,  0 + p, K_PAD1 + p, sch);  /* HOT CUE */
               add_note(rch, 16 + p, K_PAD1 + p, sch);  /* PAD FX 1 -> AUTO BEAT LOOP */
               add_note(rch, 32 + p, K_PAD1 + p, sch);  /* BEAT JUMP -> SLIP BEAT LOOP */
               add_note(rch, 48 + p, K_PAD1 + p, sch);  /* SAMPLER -> BEAT JUMP */
          }
     }

     /* ---- Mixer / Browse / Global controls on CH_MIX (ch 6) ---- */
     /* 4-Channel Sound Color FX (Filter) */
     add_abs(CH_MIX, CC_CFX1, K_COLOR, 1);
     add_abs(CH_MIX, CC_CFX2, K_COLOR, 2);
     add_abs(CH_MIX, CC_CFX3, K_COLOR, 1);
     add_abs(CH_MIX, CC_CFX4, K_COLOR, 2);

     /* Crossfader & Headphone Cue */
     add_abs(CH_MIX, CC_XFADER, K_XFADER, CH_GLOBAL);
     add_abs(CH_MIX, CC_HP_MIX, K_HPMIX, CH_GLOBAL);
     add_abs(CH_MIX, CC_HP_LEVEL, K_HPLEVEL, CH_GLOBAL);

     /* Browse knob */
     add_note(CH_MIX, N_BROWSE_PUSH, K_SELECTOR, CH_GLOBAL);
     add_note(CH_MIX, N_BROWSE_PUSH_SHIFT, K_SOURCE, CH_GLOBAL);

     /* Dedicated navigation buttons on DDJ-FLX6 */
     add_note(CH_MIX, N_BACK, K_BACK, CH_GLOBAL);
     add_note(CH_MIX, N_VIEW, K_BROWSE, CH_GLOBAL);
     add_note(CH_MIX, N_VIEW_SHIFT, K_SOURCE, CH_GLOBAL);

     /* Dedicated Load buttons 1..4 */
     add_note(CH_MIX, N_LOAD1, K_LOAD, 1);
     add_note(CH_MIX, N_LOAD2, K_LOAD, 2);
     add_note(CH_MIX, N_LOAD3, K_LOAD, 1);
     add_note(CH_MIX, N_LOAD4, K_LOAD, 2);

     /* Shift + Load shortcuts */
     add_note_repeat(CH_MIX, N_LOAD1_SHIFT, K_TRACKFILTER, CH_GLOBAL);
     add_note(CH_MIX, N_LOAD2_SHIFT, K_SHORTCUT, CH_GLOBAL);
     /* SHIFT + LOAD 3 (note 90 / 96) is handled in flx6_event() to toggle UTILITY / Settings */

     /* 1-5L/R Shift + Deck -> SLIP mode toggle (Deck 1 / Left & Deck 2 / Right) */
     add_note(CH_MIX, N_DECK_L_SHIFT, K_SLIP, 1);
     add_note(CH_MIX, N_DECK_R_SHIFT, K_SLIP, 2);

     /* Beat FX controls (Slots 1..3 on FX1 and FX2) are handled dynamically
      * per slot address in flx6_event() and flx6_update_bfx_led() */

     /* Merge FX L as Beat FX depth / enable (Merge FX R rotary is Beat FX Time) */
     add_note(CH_FX1, N_MERGE_FX_PRESS, K_BFX, CH_GLOBAL);
     add_note(CH_FX2, N_MERGE_FX_PRESS, K_BFX, CH_GLOBAL);
     add_abs(CH_FX1, CC_MERGE_FX, K_DEPTH, CH_GLOBAL);
}

static int flx6_beat_to_cc100(int numer, int denom)
{
     if (numer <= 0 || denom <= 0) return 0x05;
     if (denom >= 4) return 0x03;               /* 1/4 (and 1/8, 1/16) */
     if (denom == 2) return 0x04;               /* 1/2 */
     if (numer == 1) return 0x05;               /* 1 */
     if (numer == 2) return 0x06;               /* 2 */
     if (numer == 3) return 0x06;               /* 3/4 -> 2 */
     if (numer >= 4 && numer < 8) return 0x07;  /* 4 */
     if (numer >= 8 && numer < 16) return 0x08; /* 8 */
     if (numer >= 16 && numer < 32) return 0x09;/* 16 */
     if (numer >= 32) return 0x0A;              /* 32 */
     return 0x05;
}

static void flx6_update_bfx_led(void)
{
     unsigned long long now = shim_now_ms();
     if (now - bfx_led_last_ms >= 250) {
          bfx_led_last_ms = now;
          bfx_led_blink = !bfx_led_blink;
     }

     int rbp_on = rbp_beatfx_on();

     /* External sync: if rbp Beat FX turned off, clear all slot states */
     if (rbp_on == 0) {
          for (int s = 0; s < 6; s++) {
               flx6_slots[s].on = 0;
          }
     } else if (rbp_on == 1) {
          /* If rbp Beat FX turned on externally, ensure active slot is marked on */
          int any = 0;
          for (int s = 0; s < 6; s++) {
               if (flx6_slots[s].on) any = 1;
          }
          if (!any) {
               flx6_slots[flx6_active_slot].on = 1;
          }
     }

     /* Update LED output for each slot's unique address:
      * FX1-1: ch 4 note 71, FX1-2: ch 4 note 72, FX1-3: ch 4 note 73
      * FX2-1: ch 5 note 71, FX2-2: ch 5 note 72, FX2-3: ch 5 note 73 */
     for (int s = 0; s < 6; s++) {
          int rch = (s < 3) ? CH_FX1 : CH_FX2;
          int note = 71 + (s % 3);
          int want_vel = 0x00;

          if (flx6_slots[s].on) {
               want_vel = bfx_led_blink ? 0x7F : 0x00;
          } else {
               want_vel = 0x00;
          }

          if (want_vel != bfx_led_last_sent[s]) {
               bfx_led_last_sent[s] = want_vel;
               midi_note(rch, note, want_vel);
          }
     }
}

static unsigned char flx6_ring_pos[2] = {0, 0};

static void flx6_update_jog_ring(void)
{
     for (int d = 0; d < 2; d++) {
          if (!rbp_is_loaded(d)) {
               if (flx6_ring_pos[d] != 0) {
                    flx6_ring_pos[d] = 0;
                    midi_cc(11, d, 0);
               }
               continue;
          }

          int at_end = rbp_has_arrived_at_end(d);
          if (at_end) {
               /* Stop advancing at end of track */
               continue;
          }

          long pos = rbp_get_playing_pos(d);

          /* 33.33 RPM standard: 1 rev = 1.8 seconds = 79,380 samples @ 44.1 kHz.
           * 72 LEDs on DDJ-FLX6 ring. */
          long p = pos % 79380;
          if (p < 0) p += 79380;
          int led = (int)((double)p * 72.0 / 79380.0) + 1;
          if (led < 1) led = 1;
          if (led > 72) led = 72;

          if (led != (int)flx6_ring_pos[d]) {
               flx6_ring_pos[d] = (unsigned char)led;
               midi_cc(11, d, flx6_ring_pos[d]);
          }
     }
}

static int last_beat_cc_val = -1;

static void flx6_update_beat_fraction_led(void)
{
     int num = rbp_beatfx_numer();
     int den = rbp_beatfx_denom();
     if (num < 0 || den < 0) return;
     int cc_val = flx6_beat_to_cc100(num, den);
     if (cc_val != last_beat_cc_val) {
          last_beat_cc_val = cc_val;
          midi_cc(CH_FX1, 100, cc_val);
          midi_cc(CH_FX2, 100, cc_val);
          if (verbose)
               klog("knobshim2: FLX6 Beat FX fraction %d/%d -> CC100 val=0x%02x\n",
                    num, den, cc_val);
     }
}

/* ---- Deck Layer state ----
 * Left side:  CH_DECK1 (0) = Deck 1, CH_DECK3 (2) = Deck 3 -> Target Player 1 (sch 1)
 * Right side: CH_DECK2 (1) = Deck 2, CH_DECK4 (3) = Deck 4 -> Target Player 2 (sch 2) */
static int flx6_deck_layer[2] = { CH_DECK1, CH_DECK2 };
static int last_bfx_ch_led = -1;

int flx6_get_deck_channel(int deck)
{
     if (deck == 0) return flx6_deck_layer[0];
     if (deck == 1) return flx6_deck_layer[1];
     return -1;
}

int flx6_get_pad_channel(int deck)
{
     if (deck == 0) return (flx6_deck_layer[0] == CH_DECK1) ? CH_PADS1 : CH_PADS3;
     if (deck == 1) return (flx6_deck_layer[1] == CH_DECK2) ? CH_PADS2 : CH_PADS4;
     return -1;
}

int flx6_get_meter_channel(int m)
{
     if (m == 1) return flx6_deck_layer[0];
     if (m == 2) return flx6_deck_layer[1];
     return -1;
}

static void flx6_update_bfx_channel_led(void)
{
     int cur_ch = rbp_beatfx_channel();
     if (cur_ch < 0) return;

     int want_note = -1;
     if (cur_ch == 0) {
          want_note = (flx6_deck_layer[0] == CH_DECK1) ? 28 : 30; /* CH1 or CH3 */
     } else if (cur_ch == 1) {
          want_note = (flx6_deck_layer[1] == CH_DECK2) ? 29 : 31; /* CH2 or CH4 */
     } else if (cur_ch == BFX_CH_MASTER) {
          want_note = 20; /* MASTER */
     }

     if (want_note != last_bfx_ch_led) {
          last_bfx_ch_led = want_note;
          static const int ch_notes[] = { 20, 28, 29, 30, 31 };
          for (int i = 0; i < 5; i++) {
               int n = ch_notes[i];
               int vel = (n == want_note) ? 0x7F : 0x00;
               midi_note(CH_FX1, n, vel);
               midi_note(CH_FX2, n, vel);
          }
          if (verbose)
               klog("knobshim2: FLX6 Beat FX CH LED updated -> rbp_ch=%d note=%d\n",
                    cur_ch, want_note);
     }
}

static void flx6_clear_channel_leds(int ch, int pad_ch)
{
     static const int deck_notes[] = {
          N_PLAY, N_CUE, N_IN, N_OUT, N_4BEAT, N_SYNC, N_SYNC_SHIFT,
          N_MASTER, N_TEMPO_RANGE, N_MODE_HOT, N_MODE_PFX1, N_MODE_JUMP,
          N_MODE_SMPL, N_CH_CUE
     };
     for (size_t i = 0; i < sizeof(deck_notes)/sizeof(deck_notes[0]); i++) {
          midi_note(ch, deck_notes[i], 0x00);
     }
     midi_note(ch, N_DECK_STATE, 0x00);
     midi_cc(ch, 2, 0x00);

     if (pad_ch >= 0) {
          for (int p = 0; p < 8; p++) {
               midi_note(pad_ch, p, 0x00);
               midi_note(pad_ch, 16 + p, 0x00);
               midi_note(pad_ch, 32 + p, 0x00);
               midi_note(pad_ch, 48 + p, 0x00);
          }
     }
}

static void flx6_switch_deck(int side, int new_layer)
{
     if (side < 0 || side > 1) return;
     int old_layer = flx6_deck_layer[side];
     if (old_layer == new_layer) return;

     int old_pad_ch = (side == 0) ? ((old_layer == CH_DECK1) ? CH_PADS1 : CH_PADS3)
                                  : ((old_layer == CH_DECK2) ? CH_PADS2 : CH_PADS4);

     flx6_clear_channel_leds(old_layer, old_pad_ch);
     flx6_deck_layer[side] = new_layer;
     midi_note(new_layer, N_DECK_STATE, 0x7F);

     klog("knobshim2: FLX6 %s Deck switched: Deck %d -> Deck %d (Target Player %d, Mixer + Jog + Pads shifted)\n",
          (side == 0) ? "Left" : "Right", old_layer + 1, new_layer + 1, side + 1);

     last_bfx_ch_led = -1;
     flx6_update_bfx_channel_led();
}

static void flx6_startup(void)
{
     *(volatile uint32_t *)ADDR_MIXER_ROUTE_PLAYER0 = MIXER_ROUTE_INPUT0;
     *(volatile uint32_t *)ADDR_MIXER_ROUTE_PLAYER1 = MIXER_ROUTE_INPUT1;
     klog("knobshim2: routed Mixer Ch1 -> Deck1, Ch2 -> Deck2\n");

     send_rx_key(K_FILTER, OP_PRESS, 1, 0);
     send_rx_key(K_FILTER, OP_RELEASE, 1, 0);
     send_rx_key(K_FILTER, OP_PRESS, 2, 0);
     send_rx_key(K_FILTER, OP_RELEASE, 2, 0);
     send_rx_key_f(K_COLOR, OP_VALUE, 1, 512, 0.5f);
     send_rx_key_f(K_COLOR, OP_VALUE, 2, 512, 0.5f);
     klog("knobshim2: Sound Color FX initialized to Filter on Ch1 & Ch2\n");

     send_rx_key_f(K_MASTERLVL, OP_VALUE, CH_GLOBAL, 1023, 1.0f);
     klog("knobshim2: master level set to unity\n");

     flx6_load_fx_config();

     flx6_deck_layer[0] = CH_DECK1;
     flx6_deck_layer[1] = CH_DECK2;
     midi_note(CH_DECK1, N_DECK_STATE, 0x7F);
     midi_note(CH_DECK3, N_DECK_STATE, 0x00);
     midi_note(CH_DECK2, N_DECK_STATE, 0x7F);
     midi_note(CH_DECK4, N_DECK_STATE, 0x00);
     flx6_clear_channel_leds(CH_DECK3, CH_PADS3);
     flx6_clear_channel_leds(CH_DECK4, CH_PADS4);

     last_beat_cc_val = -1;
     flx6_update_beat_fraction_led();

     last_bfx_ch_led = -1;
     flx6_update_bfx_channel_led();

     for (int s = 0; s < 6; s++) {
          flx6_slots[s].on = 0;
          bfx_led_last_sent[s] = -1;
     }
     flx6_update_bfx_led();
}

/* Event dispatcher */
static void flx6_event(const struct snd_seq_event *ev)
{
     if (!ev) return;

     if (ev->type == SNDRV_SEQ_EVENT_NOTEON || ev->type == SNDRV_SEQ_EVENT_NOTEOFF) {
          int ch = ev->data.note.channel;
          int note = ev->data.note.note;
          int on = (ev->type == SNDRV_SEQ_EVENT_NOTEON) && (ev->data.note.velocity > 0);

          /* Deck 3 & 4 button toggle */
          if (note == N_DECK_TOGGLE) {
               if (on) {
                    if (ch == CH_DECK1 || ch == CH_DECK3) {
                         int next_layer = (flx6_deck_layer[0] == CH_DECK1) ? CH_DECK3 : CH_DECK1;
                         flx6_switch_deck(0, next_layer);
                    } else if (ch == CH_DECK2 || ch == CH_DECK4) {
                         int next_layer = (flx6_deck_layer[1] == CH_DECK2) ? CH_DECK4 : CH_DECK2;
                         flx6_switch_deck(1, next_layer);
                    }
               }
               return;
          }

          /* Discard events from inactive deck layers (parks inactive deck so mixer/pads/jog don't move) */
          if (ch == CH_DECK1 || ch == CH_DECK3) {
               if (ch != flx6_deck_layer[0]) return;
          } else if (ch == CH_DECK2 || ch == CH_DECK4) {
               if (ch != flx6_deck_layer[1]) return;
          } else if (ch == CH_PADS1 || ch == CH_PADS1S || ch == CH_PADS3 || ch == CH_PADS3S) {
               int act_pad = (flx6_deck_layer[0] == CH_DECK1) ? CH_PADS1 : CH_PADS3;
               if (ch != act_pad && ch != act_pad + 1) return;
          } else if (ch == CH_PADS2 || ch == CH_PADS2S || ch == CH_PADS4 || ch == CH_PADS4S) {
               int act_pad = (flx6_deck_layer[1] == CH_DECK2) ? CH_PADS2 : CH_PADS4;
               if (ch != act_pad && ch != act_pad + 1) return;
          }

          /* SHIFT + HEADPHONE CUE 1-4 (note 104 on ch 0..3): Switch Sound Color FX */
          if (ch >= 0 && ch <= 3 && note == N_CH_CUE_SHIFT) {
               if (on) {
                    flx6_select_cfx_slot(ch);
               }
               return;
          }

          /* Channel CUE (PFL) on active deck channel */
          if ((ch >= CH_DECK1 && ch <= CH_DECK4) && note == N_CH_CUE) {
               if (on) {
                    int side = (ch == CH_DECK1 || ch == CH_DECK3) ? 0 : 1;
                    int cur = me_get_cue(side);
                    int want = (cur == 0) ? 1 : 0;
                    me_set_cue(side, want);
                    if (verbose)
                         klog("knobshim2: FLX6 CH CUE side%d (Deck %d) -> cue=%d\n", side + 1, ch + 1, want);
               }
               return;
          }

          /* Master CUE on CH_MIX */
          if (ch == CH_MIX && note == N_MASTER_CUE) {
               if (on) {
                    int cur = me_get_master_cue();
                    int want = (cur == 0) ? 1 : 0;
                    me_set_master_cue(want);
                    if (verbose)
                         klog("knobshim2: FLX6 master cue -> %s\n", want ? "ON" : "OFF");
               }
               return;
          }

          /* Beat FX CH SELECT buttons */
          if ((ch == CH_FX1 || ch == CH_FX2) && on) {
               int target_ch = -1;
               if (note == 28) target_ch = 0;       /* CH 1 */
               else if (note == 29) target_ch = 1;  /* CH 2 */
               else if (note == 30) target_ch = 0;  /* CH 3 -> Deck 1 */
               else if (note == 31) target_ch = 1;  /* CH 4 -> Deck 2 */
               else if (note == 20) target_ch = BFX_CH_MASTER; /* MASTER */

               if (target_ch >= 0) {
                    int cur_ch = rbp_beatfx_channel();
                    int slot_ch = flx6_slots[flx6_active_slot].channel;
                    int changing = (cur_ch >= 0) ? (cur_ch != target_ch) : (slot_ch != target_ch);

                    /* Auto-disable Beat FX if currently ON before switching channel
                     * to avoid DSP popping and audio glitches */
                    if (changing && rbp_beatfx_on() > 0) {
                         send_rx_key(K_BFX, OP_PRESS, CH_GLOBAL, 0);
                         send_rx_key(K_BFX, OP_RELEASE, CH_GLOBAL, 0);
                         for (int s = 0; s < 6; s++) flx6_slots[s].on = 0;
                         klog("knobshim2: FLX6 FX CH SELECT -> Auto-disabled Beat FX prior to switching ch %d -> %d\n",
                              (cur_ch >= 0 ? cur_ch : slot_ch), target_ch);
                    }
                    flx6_slots[flx6_active_slot].channel = target_ch;
                    send_rx_key(K_BFXCH, OP_VALUE, CH_GLOBAL, target_ch);
                    klog("knobshim2: FLX6 FX CH SELECT note%d -> ch %d (slot %d)\n",
                         note, target_ch, flx6_active_slot + 1);
                    return;
               }
          }

          /* Beat FX BEAT < / > */
          if ((ch == CH_FX1 || ch == CH_FX2) && (note == N_FX_BEATPREV || note == N_FX_BEATNEXT)) {
               if (on) {
                    int key = (note == N_FX_BEATPREV) ? K_BEATPREV : K_BEATNEXT;
                    long d = (note == N_FX_BEATPREV) ? -1 : 1;
                    send_rx_key_fl(key, OP_PRESS, CH_GLOBAL, d, 0.0f, d);
                    flx6_update_beat_fraction_led();
               }
               return;
          }

          /* Beat FX SELECT buttons: Slot 1 (112), Slot 2 (113), Slot 3 (114) on FX1 & FX2 */
          if ((ch == CH_FX1 || ch == CH_FX2) && (note >= 112 && note <= 114)) {
               if (on) {
                    int sel = (ch == CH_FX1) ? (note - 112) : (3 + (note - 112));
                    if (sel >= 0 && sel < 6) {
                         /* Auto Disable FX when switching Beat FX so state matches */
                         if (rbp_beatfx_on() > 0) {
                              send_rx_key(K_BFX, OP_PRESS, CH_GLOBAL, 0);
                              send_rx_key(K_BFX, OP_RELEASE, CH_GLOBAL, 0);
                         }
                         for (int s = 0; s < 6; s++) flx6_slots[s].on = 0;

                         flx6_active_slot = sel;
                         send_rx_key(K_BFXTYPE, OP_VALUE, CH_GLOBAL, flx6_slots[sel].fx_type);
                         send_rx_key(K_BFXCH, OP_VALUE, CH_GLOBAL, flx6_slots[sel].channel);
                         send_rx_key_f(K_DEPTH, OP_VALUE, CH_GLOBAL,
                                       flx6_slots[sel].depth * 8,
                                       (float)flx6_slots[sel].depth / 127.0f);
                         klog("knobshim2: FLX6 BFX SELECT -> Slot %d (Auto-disabled FX, type %d, ch %d)\n",
                              sel + 1, flx6_slots[sel].fx_type, flx6_slots[sel].channel);
                    }
               }
               return;
          }

          /* Beat FX Slot Enable buttons: Slot 1 (71), Slot 2 (72), Slot 3 (73) on FX1 & FX2 */
          if ((ch == CH_FX1 || ch == CH_FX2) && (note >= 71 && note <= 73)) {
               if (on) {
                    int slot = (ch == CH_FX1) ? (note - 71) : (3 + (note - 71));
                    if (slot >= 0 && slot < 6) {
                         flx6_active_slot = slot;
                         int want_on = !flx6_slots[slot].on;
                         /* Ensure only the active slot is turned on (mutually exclusive) */
                         for (int s = 0; s < 6; s++) flx6_slots[s].on = 0;
                         flx6_slots[slot].on = want_on;

                         int any_on = want_on;
                         int cur_rbp_on = rbp_beatfx_on();
                         if (any_on && cur_rbp_on <= 0) {
                              /* Turn rbp Beat FX ON */
                              send_rx_key(K_BFXTYPE, OP_VALUE, CH_GLOBAL, flx6_slots[slot].fx_type);
                              send_rx_key(K_BFXCH, OP_VALUE, CH_GLOBAL, flx6_slots[slot].channel);
                              send_rx_key_f(K_DEPTH, OP_VALUE, CH_GLOBAL,
                                            flx6_slots[slot].depth * 8,
                                            (float)flx6_slots[slot].depth / 127.0f);
                              send_rx_key(K_BFX, OP_PRESS, CH_GLOBAL, 0);
                              send_rx_key(K_BFX, OP_RELEASE, CH_GLOBAL, 0);
                         } else if (!any_on && cur_rbp_on > 0) {
                              /* Turn rbp Beat FX OFF */
                              send_rx_key(K_BFX, OP_PRESS, CH_GLOBAL, 0);
                              send_rx_key(K_BFX, OP_RELEASE, CH_GLOBAL, 0);
                         }
                         klog("knobshim2: FLX6 BFX Button Slot %d (FX%d-%d) -> %s\n",
                              slot + 1, (slot < 3) ? 1 : 2, (slot % 3) + 1,
                              flx6_slots[slot].on ? "ON" : "OFF");
                    }
               }
               return;
          }

          /* Release FX (+SHIFT FX ON/OFF) */
          if ((ch == CH_FX1 || ch == CH_FX2) && note == 67) {
               if (on) {
                    for (int s = 0; s < 6; s++) flx6_slots[s].on = 0;
                    if (rbp_beatfx_on() > 0) {
                         send_rx_key(K_BFX, OP_PRESS, CH_GLOBAL, 0);
                         send_rx_key(K_BFX, OP_RELEASE, CH_GLOBAL, 0);
                    }
                    klog("knobshim2: FLX6 Release FX -> All Beat FX OFF\n");
               }
               return;
          }

          /* +SHIFT + HOT CUE pad (Delete cue) on channels 7..14 */
          if ((ch == CH_PADS1S || ch == CH_PADS2S || ch == CH_PADS3S || ch == CH_PADS4S) && note >= 0 && note < 8) {
               if (on) {
                    int side = (ch == CH_PADS1S || ch == CH_PADS3S) ? 0 : 1;
                    hotcue_delete(side, note + 1);
               }
               return;
          }

          /* 4-Beat Auto Loop / Active Loop: Long-press IN (20), +SHIFT IN (76), +SHIFT RELOOP (80) */
          if ((ch >= CH_DECK1 && ch <= CH_DECK4) && (note == N_IN_4BEAT || note == N_IN_SHIFT || note == N_RELOOP_SHIFT)) {
               if (on) {
                    int side = (ch == CH_DECK1 || ch == CH_DECK3) ? 0 : 1;
                    int sch = side + 1;
                    send_rx_key(K_LOOPIN, OP_REPEAT, sch, 0);
                    send_rx_key(K_LOOPIN, OP_RELEASE, sch, 0);
                    klog("knobshim2: FLX6 Auto 4-Beat Loop side%d note%d (deck %d)\n",
                         side + 1, note, sch);
               }
               return;
          }

          /* Browse knob push (Encoder click):
           * Jika di tampilan waveform (BROWSE_MODE_PLAY == 1), buka browser (seperti tombol BACK).
           * Jika di tampilan lain (Browse list, Utility, dll), biarkan K_SELECTOR bekerja normal. */
          if (ch == CH_MIX && note == N_BROWSE_PUSH) {
               static int enc_pressed_in_play = 0;
               if (on) {
                    int mode = 0;
                    if (is_rbp_process()) {
                         mode = *(volatile int *)(uintptr_t)(UIBROWSE_GLOBAL + UIBROWSE_OFF_MODE);
                    }
                    if (mode == BROWSE_MODE_PLAY) {
                         enc_pressed_in_play = 1;
                         send_rx_key(K_BACK, OP_PRESS, CH_GLOBAL, 0);
                         send_rx_key(K_BACK, OP_RELEASE, CH_GLOBAL, 0);
                         klog("knobshim2: FLX6 Browse Push on waveform (mode %d) -> K_BACK to open Browser\n", mode);
                         return;
                    }
                    enc_pressed_in_play = 0;
               } else {
                    if (enc_pressed_in_play) {
                         enc_pressed_in_play = 0;
                         return;
                    }
               }
               /* Jatuh ke table notes jika bukan di mode waveform (K_SELECTOR) */
          }

          /* SHIFT + LOAD 3 (note 96 from hardware, or note 90) -> Buka / Tutup Menu UTILITY / Settings */
          if (ch == CH_MIX && (note == 96 || note == N_LOAD3_SHIFT)) {
               if (on) {
                    pthread_t th;
                    pthread_create(&th, NULL, flx6_trigger_settings, NULL);
                    pthread_detach(th);
                    klog("knobshim2: FLX6 SHIFT+LOAD 3 (note %d) -> Toggle Menu UTILITY / Settings (K_MENU)\n", note);
               }
               return;
          }

          /* Table notes */
          for (int i = 0; i < note_map_n; i++) {
               if (note_map[i].rch != ch || note_map[i].note != note)
                    continue;
               int key = note_map[i].key;
               if (key == 0) {
                    flx6_log_only(ch, note);
                    return;
               }
               int *p = &note_map[i].pressed;
               if (on && !*p) {
                    *p = 1;
                    send_rx_key(key, OP_PRESS, note_map[i].sch, 0);
                    if (note_map[i].repeat)
                         send_rx_key(key, OP_REPEAT, note_map[i].sch, 0);
                    if (verbose || key == K_MEM_CUE || key == K_MEM_DEL || key == K_CALL_NEXT ||
                        key == K_CALL_PREV || key == K_RELOOP || key == K_BFX || key == K_TEMPO_RANGE ||
                        key == K_SOURCE)
                         klog("knobshim2: FLX6 ch%d note%d -> key 0x%04x PRESS (deck %d)\n",
                              ch + 1, note, key, note_map[i].sch);
               } else if (!on && *p) {
                    *p = 0;
                    send_rx_key(key, OP_RELEASE, note_map[i].sch, 0);
                    if (verbose || key == K_MEM_CUE || key == K_MEM_DEL || key == K_CALL_NEXT ||
                        key == K_CALL_PREV || key == K_RELOOP || key == K_BFX || key == K_TEMPO_RANGE ||
                        key == K_SOURCE)
                         klog("knobshim2: FLX6 ch%d note%d -> key 0x%04x RELEASE (deck %d)\n",
                              ch + 1, note, key, note_map[i].sch);
               }
               return;
          }
     } else if (ev->type == SNDRV_SEQ_EVENT_CONTROLLER) {
          int ch = ev->data.control.channel;
          int cc = ev->data.control.param;
          int val = ev->data.control.value;

          /* Filter deck layer: discard CC from parked/inactive deck */
          if (ch == CH_DECK1 || ch == CH_DECK3) {
               if (ch != flx6_deck_layer[0]) return;
          } else if (ch == CH_DECK2 || ch == CH_DECK4) {
               if (ch != flx6_deck_layer[1]) return;
          }

          /* Browse knob on CH_MIX */
          if (ch == CH_MIX && (cc == CC_BROWSE || cc == CC_BROWSE_SHIFT)) {
               flx6_browse(val);
               return;
          }

          /* Master level */
          if (ch == CH_MIX && (cc == CC_MASTER_MSB || cc == CC_MASTER_LSB)) {
               flx6_mastervol(cc, val);
               return;
          }

          /* Sound Color FX on CH_MIX: route exclusively to active deck */
          if (ch == CH_MIX) {
               if (cc == CC_CFX1 && flx6_deck_layer[0] != CH_DECK1) return;
               if (cc == CC_CFX3 && flx6_deck_layer[0] != CH_DECK3) return;
               if (cc == CC_CFX2 && flx6_deck_layer[1] != CH_DECK2) return;
               if (cc == CC_CFX4 && flx6_deck_layer[1] != CH_DECK4) return;
          }

          /* Jog wheel across active decks */
          if ((ch >= CH_DECK1 && ch <= CH_DECK4) && (cc == CC_JOG_WHEEL || cc == CC_JOG_VINYL ||
                                                     cc == CC_JOG_NOVINYL || cc == CC_JOG_SHIFT)) {
               int side = (ch == CH_DECK1 || ch == CH_DECK3) ? 0 : 1;
               flx6_jog(side, val);
               return;
          }

          /* Pitch fader across active decks */
          if ((ch >= CH_DECK1 && ch <= CH_DECK4) && (cc == CC_TEMPO_MSB || cc == CC_TEMPO_LSB)) {
               int side = (ch == CH_DECK1 || ch == CH_DECK3) ? 0 : 1;
               flx6_pitch(side, cc, val);
               return;
          }

          /* Beat FX Level / Depth knobs across slots 1..3 on FX1 and FX2 */
          if ((ch == CH_FX1 || ch == CH_FX2) && (cc == 2 || cc == 4 || cc == 6)) {
               int d_slot = (ch == CH_FX1) ? ((cc == 2) ? 0 : (cc == 4 ? 1 : 2))
                                           : ((cc == 2) ? 3 : (cc == 4 ? 4 : 5));
               flx6_slots[d_slot].depth = val;
               if (d_slot == flx6_active_slot) {
                    send_rx_key_f(K_DEPTH, OP_VALUE, CH_GLOBAL, val * 8, (float)val / 127.0f);
               }
               return;
          }

          /* 1-14R Merge FX Parameter R rotary turn -> Beat FX Time encoder (K_TIME 0x448e) */
          if (ch == CH_FX2 && (cc == CC_MERGE_FX || cc == (CC_MERGE_FX + 32))) {
               int delta = (val >= 64) ? (val - 128) : val;
               if (delta != 0) {
                    send_rx_key_fl(K_TIME, OP_ROTATE, CH_GLOBAL, delta, 0.0f, delta);
                    if (verbose)
                         klog("knobshim2: Merge FX R turn v=%d delta=%d -> K_TIME\n",
                              val, delta);
               }
               return;
          }

          /* Standard continuous controls */
          flx6_cc_abs(ch, cc, val);
     }
}

static void flx6_tick(void)
{
     flx6_jog_idle();
     flx6_check_fx_config();
     flx6_sync_crossfader();

     /* Sync active slot FX type if changed on rbp screen */
     int cur_t = rbp_beatfx_type();
     if (cur_t > 0 && cur_t < 15 && bfx_type_to_pos[cur_t] >= 0) {
          int cur_pos = bfx_type_to_pos[cur_t];
          if (cur_pos != last_seen_rbp_fx_pos) {
               last_seen_rbp_fx_pos = cur_pos;
               flx6_slots[flx6_active_slot].fx_type = cur_pos;
          }
     }

     /* Realtime LED sync from rbp player state */
     flx6_update_bfx_led();
     flx6_update_beat_fraction_led();
     flx6_update_bfx_channel_led();
     flx6_update_jog_ring();

     /* Keep DeckState LED illuminated on active deck layers */
     static unsigned long long last_deckstate_ms = 0;
     unsigned long long now = shim_now_ms();
     if (now - last_deckstate_ms >= 500) {
          last_deckstate_ms = now;
          midi_note(CH_DECK1, N_DECK_STATE, (flx6_deck_layer[0] == CH_DECK1) ? 0x7F : 0x00);
          midi_note(CH_DECK3, N_DECK_STATE, (flx6_deck_layer[0] == CH_DECK3) ? 0x7F : 0x00);
          midi_note(CH_DECK2, N_DECK_STATE, (flx6_deck_layer[1] == CH_DECK2) ? 0x7F : 0x00);
          midi_note(CH_DECK4, N_DECK_STATE, (flx6_deck_layer[1] == CH_DECK4) ? 0x7F : 0x00);
     }
}

/* LED & Meter definitions */
static const struct led_notes flx6_leds = {
     .deck_ch = 0,
     .n_sync = 88,
     .n_cue = 12,
     .n_play = 11,
     .n_keylock = 93,
     .n_vinyl = -1,
     .n_slip = -1,
     .n_loopin = 16,
     .n_loopout = 17,
     .n_autoloop = -1,
     .n_master = 92,

     .deck_ch_fn = flx6_get_deck_channel,
     .pad_ch_fn = flx6_get_pad_channel,
     .meter_ch_fn = flx6_get_meter_channel,

     .n_mode = { 27, 30, 32, 34 },

     .pad_ch = 7,              /* Deck 1 pads */
     .pad_ch2 = 9,             /* Deck 2 pads */
     .n_pad_base = { 0, 16, 32, 48 },
     .pad_enc = LED_ENC_NONE,

     .strip_ch_first = 0,      /* Ch 1..2 */
     .strip_count = 2,
     .n_strip_cue = 84,

     .master_ch_first = 6,
     .master_ch_count = 1,
     .n_master_cue = 99,

     .fx_ch = 4,               /* Ch 5 (0-based 4) for Beat FX */
     .n_fx = {
          [LED_FX_BFX_ONOFF]   = -1,   /* Handled by flx6_update_bfx_led across slots 1-3 on FX1 & FX2 */
          [LED_FX_CFX_FILTER]  = -1,
          [LED_FX_CFX_DUBECHO] = -1,
          [LED_FX_CFX_NOISE]   = -1,
          [LED_FX_CFX_SWEEP]   = -1,
     },

     .meter_ch_first = 0,      /* Decks 1..2 -> channels 0..1 */
     .n_meter_cc = 2,          /* CC 2 */
     .meter_enc = METER_ENC_FLX4_LEVEL,
     .meter_pre_fader = 1,
     .meter_master_ch = -1,
     .n_meter_master_l = -1,
     .n_meter_master_r = -1,
};

const struct ctrl_map map_flx6 = {
     "flx6",
     flx6_build,
     flx6_startup,
     flx6_event,
     flx6_tick,
     NULL,
     NULL,
     &flx6_leds,
};

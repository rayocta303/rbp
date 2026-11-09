/* SPDX-License-Identifier: MPL-2.0
 * Private light theme state and the player structures it reaches into.
 * Which image table is shown, and the fill adapter, belong to the core's
 * image service; this module supplies the policy and decides when to switch.
 */

#ifndef RX3_THEME_DECL_H
#define RX3_THEME_DECL_H

#include "../core/api/rx3_palette.h"

/* The global dark remap: the stock chrome taken down rather than inverted. It
   is a mode of its own, not the absence of the light one, and it runs the
   conversion in theme_pixel_dark over the same image table. */
static int theme_global_dark;
/* Its kill switch. The watcher polls for it, so a set that goes wrong can be
   put back to stock without a restart. */
#define THEME_DARK_SENTINEL "/tmp/rx3-theme.off"

/* The fill colour the core's adapter hands over. Three channels in one word:
 * blue at bits 0..4, green at 8..13, red at 16..20. Green carries six bits
 * where the others carry five, which is why a comparison across all three
 * shifts it down by one first.
 */
#define THEME_FILL_BLUE_SHIFT  0u
#define THEME_FILL_GREEN_SHIFT 8u
#define THEME_FILL_RED_SHIFT   16u

/* An RGB888 palette colour in the fill word above. */
#define THEME_FILL_FROM_RGB(c) \
    ((((c) >> 19) & 0x1fu) << THEME_FILL_RED_SHIFT | \
     (((c) >> 10) & 0x3fu) << THEME_FILL_GREEN_SHIFT | \
     (((c) >> 3) & 0x1fu) << THEME_FILL_BLUE_SHIFT)

/* What a dark neutral fill is replaced by, from the one rx3-ventotto palette
 * (core/api/rx3_palette.h): GROUND #C9C9CC under the panes, PANEL #DEDBDE for
 * the panes themselves (the same value the light tab artwork is drawn on,
 * 0xdedb in RGB565).
 */
#define THEME_FILL_GROUND THEME_FILL_FROM_RGB(RX3_PAL_LIGHT_GROUND)
#define THEME_FILL_PANEL  THEME_FILL_FROM_RGB(RX3_PAL_LIGHT_PANEL)
/* INK #1C1C1E: what white becomes when a light neutral fill is inverted. */
#define THEME_INK_RED   RX3_PAL_R(RX3_PAL_LIGHT_INK)
#define THEME_INK_GREEN RX3_PAL_G(RX3_PAL_LIGHT_INK)
#define THEME_INK_BLUE  RX3_PAL_B(RX3_PAL_LIGHT_INK)
#define THEME_PANEL_RED   RX3_PAL_R(RX3_PAL_LIGHT_PANEL)
#define THEME_PANEL_GREEN RX3_PAL_G(RX3_PAL_LIGHT_PANEL)
#define THEME_PANEL_BLUE  RX3_PAL_B(RX3_PAL_LIGHT_PANEL)

/* Only a fill that is already dark and close to neutral is replaced. Anything
 * brighter is a deliberate colour, and anything with a spread across channels
 * is carrying meaning: a status, a warning, an artwork edge. Recolouring those
 * is how a light theme turns into a broken one.
 */
#define THEME_FILL_MAX_CHANNEL 0x0bu
/* Four steps, not two: the Pioneer blue-grey (#2a3440) is three apart and is
   chrome. A dark red or blue is eight or more apart and keeps its colour. */
#define THEME_FILL_MAX_SPREAD  4u

/* Rectangles the player draws for whole panes. Anything smaller than the area
 * below is a control rather than a pane, and takes the panel colour.
 */
#define THEME_PANE_MIN_AREA 300000u
#define THEME_PANE_WIDE     0x444u
#define THEME_PANE_NARROW   0x43bu
#define THEME_PANE_MID      0x410u
#define THEME_PANE_SHORT    0x37cu
#define THEME_PANE_TALL     0x20du
#define THEME_PANE_MEDIUM   0x212u
#define THEME_PANE_LOW      0x202u
#define THEME_PANE_SHALLOW  0x1bcu

/* The key that completes SHIFT + SHORTCUT. The core's input service tracks
 * SHIFT per channel, because the player reports it as an ordinary key.
 */
#define THEME_KEY_SHORTCUT 0x0210u
/* Key operations, as the player numbers them. A release arrives as either of
 * the two values above one, which is why the test masks the low bit away.
 */
#define THEME_KEY_PRESS 0u
#define THEME_KEY_RELEASE 2u

/* The first two instructions are position-independent on firmware 1.19/1.20. */
#define THEME_RENDER_PASS ((unsigned long)0x001d48a0)
static const uint8_t theme_render_pass_guard[8] = {
    0x38, 0x40, 0x2d, 0xe9, 0x00, 0x40, 0xa0, 0xe1
};

/* Native window invalidation, called once at a theme transition. */
#define THEME_DIRTY_WINDOWS ((unsigned long)0x001d1144)
static const uint8_t theme_dirty_windows_guard[8] = {
    0xf8, 0x40, 0x2d, 0xe9, 0xcc, 0x40, 0x9f, 0xe5
};

/* Magenta. The player writes every other value to the framebuffer and skips
 * this one, so it is transparency and never a colour. Converting it would
 * punch a hole in whatever image it belongs to.
 *
 * The same value is in tools/rx3_logo/container.py, which writes containers
 * this reads; the logo module's guard compares the two.
 */
#define COLOUR_KEY 0xf81fu

/* The renderer allocates once after checking free memory, then stops growing
   the arena. Allocation failure disables further image conversion. */
#define THEME_ARENA_BYTES (16u * 1024u * 1024u)
#define THEME_ARENA_FLOOR_KB (128u * 1024u)
#define THEME_CONVERT_RESERVE_KB (48u * 1024u)
#define THEME_IMAGE_MAX_BYTES (4u * 1024u * 1024u)

/* Direct colour, no palette. Only these are converted: a paletted bitmap needs
 * its palette converted instead, and that is shared between images.
 */
#define THEME_FORMAT_RGB565 1u
#define THEME_FORMAT_RGB565_KEYED 2u

/* At least this share of the opaque pixels must be vivid for a bitmap to count
 * as artwork and keep its hue. Below it the bitmap is chrome, and chrome is
 * what the theme exists to repaint.
 */
#define THEME_ARTWORK_PERCENT 25u
/* How far apart the channels must be for one pixel to count as vivid. */
#define THEME_VIVID_SPREAD 0x37u
/* v2: a pixel this dark counts towards a glyph drawn on black. */
#define THEME_V2_NEAR_BLACK 0x20u

/* The dark conversion is decided on a tighter spread than anything above: a
   pixel is taken down only if it is already grey. */
#define THEME_DARK_SPREAD 0x17u
/* One multiplier for all three channels, each shifted to land in its own
   field. It works out at roughly a third of the original brightness. */
#define THEME_DARK_SCALE 0x2ccd1u

/* Weights summing to 256, so the luma is a shift rather than a division. */
#define THEME_LUMA_RED 0x36u
#define THEME_LUMA_GREEN 0xb7u
#define THEME_LUMA_BLUE 0x13u
/* Nothing in the light interface is brighter than this, so a colour above it
 * is scaled down rather than left to glare against a pale ground.
 */
#define THEME_LUMA_CEILING 0x66u
/* Chrome is folded into this range, inverted: what was near black becomes near
 * white, and the ends stay off the extremes so the panes keep their edges.
 */
#define THEME_GREY_FLOOR 0x30u
#define THEME_GREY_SPAN 0xb0u
/* Below the first spread a pixel is neutral and fully inverted; above the
 * second it is treated as artwork; between them the two are blended.
 */
#define THEME_NEUTRAL_SPREAD 0x19u
#define THEME_LIGHT_SPREAD 0x38u

/* The blend, exactly. One spread step is eight parts in two hundred and
 * fifty-six, counted from one below the neutral threshold, so a pixel that has
 * only just stopped being neutral keeps almost none of its colour and one that
 * is about to count as artwork keeps almost all of it.
 *
 * Two hundred and fifty-six and not two hundred and fifty-five: the products
 * are masked straight into the colour fields, so the total has to be a power
 * of two or every blended pixel is off by a fraction of a step.
 */
#define THEME_BLEND_BASE 0x18u
#define THEME_BLEND_STEP 8u
#define THEME_BLEND_TOTAL 0x100u

/* The waveform pane is not remapped like the rest of the interface. It stays a
 * dark well whichever mode the room is in, because a waveform read at a glance
 * needs contrast against its own background and not against the furniture.
 *
 * Two entries of the pane's own palette change, and one instruction that
 * chooses how far the pane is inset. Everything else in the table is checked
 * and left alone, so a firmware whose table is not this one is not written to.
 */
#define THEME_WAVE_TABLE ((unsigned long)0x004422f8)
#define THEME_WAVE_TABLE_WORDS 8u
#define THEME_WAVE_INSET_INDEX 3u
#define THEME_WAVE_TINT_INDEX 6u
#define THEME_WAVE_INSET_DARK 0x02bcu
#define THEME_WAVE_INSET_LIGHT 0x04feu
#define THEME_WAVE_TINT_DARK 0xb341u
#define THEME_WAVE_TINT_LIGHT 0xc461u

/* One instruction, rewritten in place: `mov r1, #0` for dark and `mov r1,
 * #0x4a` for light. Guarded the same way a hook prologue is, so an address
 * that is not this instruction is left alone.
 */
#define THEME_WAVE_INSTRUCTION ((unsigned long)0x0024fed0)
#define THEME_WAVE_OPCODE_DARK 0xe3a01000u
/* White (memset 0xff) in the light table: the grey 0x4a read as a dark pane
   on the RX3 (2026-10-15); the waveform sits on white like the overview. */
#define THEME_WAVE_OPCODE_LIGHT 0xe3a010ffu
#define THEME_WAVE_OPCODE_LIGHT_OLD 0xe3a0104au
/* Palette entry 0, the band behind the waveform: black, white in light. */
#define THEME_WAVE_BAND_INDEX 0u
#define THEME_WAVE_BAND_DARK 0x0000u
#define THEME_WAVE_BAND_LIGHT 0xffffu
/* Index 1, white: the play position line, lost on the white pane (RX3,
   2026-10-15). Black in light mode. */
#define THEME_WAVE_LINE_INDEX 1u
#define THEME_WAVE_LINE_DARK 0xffffu
#define THEME_WAVE_LINE_LIGHT 0x0000u

/* The band around the wave: SetUpDataRGB and SetUpDataBlue paint a dark
   gradient through ui_com_RGB24ToARGB16 (a pure function, r0 = RGB565), and
   all three modes fill a dark grey 0x3186. In light mode each call becomes
   `mvn r0, #0` (white, and it clobbers less than the call did) and each fill
   loads 0xffff. Every site is checked before any is written. */
struct theme_wave_site { unsigned long address; uint32_t dark, light; };
#define THEME_WAVE_WHITE_R0 0xe3e00000u
static const struct theme_wave_site theme_wave_sites[] = {
    {0x0024d5f0ul, 0xebfd13c2u, THEME_WAVE_WHITE_R0},
    {0x0024d61cul, 0xebfd13b7u, THEME_WAVE_WHITE_R0},
    {0x0024d650ul, 0xebfd13aau, THEME_WAVE_WHITE_R0},
    {0x0024d67cul, 0xebfd139fu, THEME_WAVE_WHITE_R0},
    {0x0024d6dcul, 0xebfd1387u, THEME_WAVE_WHITE_R0},
    {0x0024da58ul, 0xebfd12a8u, THEME_WAVE_WHITE_R0},
    {0x0024da88ul, 0xebfd129cu, THEME_WAVE_WHITE_R0},
    {0x0024daacul, 0xebfd1293u, THEME_WAVE_WHITE_R0},
    {0x0024dae8ul, 0xebfd1284u, THEME_WAVE_WHITE_R0},
    {0x0024db74ul, 0xebfd1261u, THEME_WAVE_WHITE_R0},
    {0x0024b644ul, 0xe3031186u, 0xe30f1fffu},
    {0x0024b6a8ul, 0xe3031186u, 0xe30f1fffu},
    {0x0024d514ul, 0xe3032186u, 0xe30f2fffu},
    {0x0024d574ul, 0xe3032186u, 0xe30f2fffu},
    {0x0024d950ul, 0xe3032186u, 0xe30f2fffu},
    {0x0024d9b0ul, 0xe3032186u, 0xe30f2fffu},
};
#define THEME_WAVE_SITES (sizeof(theme_wave_sites) / sizeof(theme_wave_sites[0]))

/* Asks the player to draw one deck again, so the pane picks the change up
 * without waiting for the next track.
 */
#define REFRESH_DECK ((unsigned long)0x001856fc)

#define THEME_HEADER_CONTROL ((unsigned long)0x02684400)
/* Queue one glyph for repaint, from the render thread. */
#define GET_HMI_MANAGER ((unsigned long)0x001d09b4)
#define REFRESH_GLYPH   ((unsigned long)0x001d07b0)
/* Stock records the conversion may be asked for. */
#define STOCK_IMAGE_COUNT RX3_NATIVE_IMAGE_COUNT

#endif /* RX3_THEME_DECL_H */

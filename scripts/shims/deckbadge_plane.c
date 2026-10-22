/*
 * deckbadge_plane.c -- see deckbadge_plane.h for why this is a plane and not a paint.
 *
 * TWO BANDS, and drmband.h says that is the intended use ("a second and third band (the two edge
 * drawers) go up alongside the first"), so nothing here is a new kind of object: it is the band's
 * machinery with a different rectangle, which is exactly what the operator asked for.
 */
#include "deckbadge_plane.h"

#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>        /* atoi */
#include <sys/mman.h>      /* PROT_READ, MAP_SHARED */
#include <unistd.h>

#include "deckbadge_paint.h"     /* deckbadge_ring(), the one ring drawer */
#include "deckbadge_zone.h"
#include "drmband.h"
#include "menu_paint.h"
#include "pointsrc.h"        /* pointsrc_log */
#include "syscalls.h"        /* real_mmap: the shim interposes mmap */

/* The ring, and the drawer's lit frame: two device px of the accent green (side_paint.c's
 * SP_FRAME_LIT and its PLAY accent). */
#define DB_RING_W 2

/* A DELAY IN BOTH DIRECTIONS, AND IT IS NOT POLISH. rbp's PLAY bit FLAPS -- the LED thread
 * reports lit and dark on alternate samples, 3583 transitions were counted in one session -- and a
 * plane shown and hidden on every flap is two DRM ioctls on the paint thread, which is also the
 * thread that owns the zones' latches. That is not a flickering ring: it is the pointer chain
 * stalling behind the paint thread, which is what "I can't input any more" was.
 *
 * SO THE RING NEEDS TWO THRESHOLDS AND NOT ONE. It comes up only after the deck has read as
 * playing for DB_SHOW_TICKS ticks IN A ROW, and goes down only after it has read as stopped for
 * DB_HIDE_TICKS in a row. A signal that flaps never reaches either, so it cannot make the plane
 * move; a real stop is a fifth of a second late, which no finger can see.
 *
 * AND IT DELAYS THE PLANE, NOT THE RING. The ring's presence in the buffer follows the bit on
 * EVERY tick -- the same bit, sampled the same way, as the drawer's PLAY frame, so the two blink
 * in step. What the thresholds govern is only whether the PLANE is up at all, and they are wide
 * so that a deck whose LED is blinking keeps its plane while the blink is in its dark half. A
 * plane that went up and down with the blink would be two DRM ioctls per blink, which is the
 * churn -- and the stalled pointer chain -- that these thresholds exist to prevent.
 *
 * NOTE THIS STILL DOES NOT MAKE THE BIT MEAN "PLAYING": it means "the LED is lit right now",
 * blink included. A ring that blinks with the drawer is the right picture, but a deck that is
 * merely PAUSED also blinks, so a paused deck wears a blinking ring rather than none. Telling
 * those apart means publishing the solid state from rbp_led.c, where rbp's LedStat says WHETHER
 * the LED blinks as well as whether it is lit. */
#define DB_SHOW_TICKS 4
#define DB_HIDE_TICKS 64     /* ...and now SHORT: the bit that drives this does not flap, because
                              * it is PLAY_SOLID and not the blinking PLAY, so the only thing this
                              * delays is how long a ring outlives the end of a track. What it must
                              * not do is wait for a blink's dark half, which is what made the plane
                              * go down and come back while a deck was playing. */

/* HOW OFTEN THE COPY IS REFRESHED, in ticks. The copy is a memcpy out of a mapping now, not a
 * read() per row (see fb_open_once), so the 30 ms stall and 93% of a core are gone -- but the
 * framebuffer is UNCACHED memory, and re-reading 95x50 twice a tick at 36 ticks a second still
 * measured 17.8% of a core. Nine ticks is ~4 Hz, which takes that to about a fiftieth of a core.
 *
 * WHAT IT IS FOR IS STILL ONLY THE FOSSIL: rbp's pixels in that box change when a screen changes,
 * a track loads or a title is rewritten, and this copy exists so the plane cannot show a stale
 * badge for a whole session. A quarter of a second of staleness is invisible, and the RING is drawn
 * from the live bit on the show/hide paths, so nothing the operator is watching is delayed. */
#define DB_COPY_TICKS 9

#define FB_PATH "/dev/fb0"

static struct drm_band band[2];
static int  tried;                  /* setup has been attempted (once per process) */
static int  usable[2];              /* this deck has a plane we can draw into */
static int  up[2];                  /* ...and it is currently on the glass */
static int  run[2];                 /* consecutive ticks this deck has read as playing */
static int  idle[2];
static int  copy_ticks[2];          /* ticks since this deck's copy was refreshed */
static unsigned int db_last_pv;    /* the pixel the ring is drawn with, for the log */

/* THE COPY IS COMPARED BEFORE IT HAPPENS. The ring sits on a copy of rbp's badge, so the copy has
 * to follow rbp -- but writing 9.5 KB into a buffer the display is scanning out of, every tick,
 * tears, and a torn box is what "the green squares flicker" was for an afternoon. rbp's pixels
 * there change when a screen changes or a track loads, which is rare: so the new read is compared
 * against the last one, and the plane is written only when something really moved -- or when the
 * ring itself goes on or off. `shown` is that ring state, because the ring is drawn into the same
 * buffer and would otherwise make every comparison differ. */
#define DB_BOX_W (DB_DECK1_X1 - DB_DECK1_X0 + 1)
#define DB_BOX_H (DB_Y1 - DB_Y0 + 1)
static unsigned short db_prev[2][DB_BOX_W * DB_BOX_H];
static unsigned short db_cur[2][DB_BOX_W * DB_BOX_H];
static unsigned char  db_have[2];
static signed char     db_shown[2];      /* -1 = nothing drawn yet, else the ring state shown */                /* ...and as stopped */

/* rbp's own buffer, opened read-only ONCE: the plane's image is a copy of what the badge looks
 * like, and this is the only place that can be read from. Not `menu_draw.c`'s view -- see
 * deckbadge_plane.h -- because that mapping is not the one the display presents. */
static int  fb_fd = -1;
static int  fb_open_done;          /* one attempt, whatever it answered */
static int  fb_pitch;               /* BYTES per row, which is what the ioctl reports */
/* rbp's pixels, MAPPED RATHER THAN read() (2026-10-12). See paint_plane() for why that is the
 * whole cost: a framebuffer read is a device you ask, at ~260 us a call; the same bytes out of a
 * mapping are a memcpy. */
static unsigned char *fb_map;
static size_t fb_map_len;
static int  fb_bpp;
static int  fb_w, fb_h;

/* THE GEOMETRY COMES FROM SYSFS, NOT FROM FBIOGET_VSCREENINFO, and that is a measurement and not
 * a preference. On this vc4 fbdev the ioctl answers 32 bpp with a 5120-byte stride while the pixels
 * really are RGB565 at 2560 -- /sys/class/graphics/fb0 says 16, and reading the buffer as 16 bpp
 * produces a correct picture. Copying 95 x 50 at the ioctl's numbers would take every other row
 * from the wrong place, which is a ring with someone else's pixels behind it: wrong in a way that
 * looks like a drawing bug rather than a units bug. */
static int read_attr_int(const char *attr, int *out)
{
    char path[128], buf[32];
    FILE *f;

    snprintf(path, sizeof path, "/sys/class/graphics/fb0/%s", attr);
    f = fopen(path, "r");
    if (!f)
        return -1;
    if (!fgets(buf, sizeof buf, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    *out = atoi(buf);
    return 0;
}

static void fb_open_once(void)
{
    int vw = 0, vh = 0;

    if (fb_fd >= 0 || fb_open_done)
        return;
    fb_open_done = 1;

    if (read_attr_int("bits_per_pixel", &fb_bpp) != 0 ||
        read_attr_int("stride", &fb_pitch) != 0 ||
        read_attr_int("virtual_size", &vw) != 0) {
        pointsrc_log("badge: /sys/class/graphics/fb0 will not say what the framebuffer is -- "
                     "no ring (the badge still works)");
        return;
    }
    /* "1280,800" -- the comma is the kernel's, and vh is the part after it. */
    {
        char buf[32];
        FILE *f = fopen("/sys/class/graphics/fb0/virtual_size", "r");

        vh = vw;
        if (f) {
            if (fgets(buf, sizeof buf, f)) {
                char *comma = strchr(buf, ',');
                if (comma) {
                    vw = atoi(buf);
                    vh = atoi(comma + 1);
                }
            }
            fclose(f);
        }
    }
    fb_w = vw;
    fb_h = vh;
    fb_fd = open(FB_PATH, O_RDONLY);
    if (fb_fd < 0) {
        pointsrc_log("badge: no %s (%m) -- nothing to copy the badge from, so no ring", FB_PATH);
        return;
    }

    /* MAP IT, DO NOT read() IT. The copy below used to be a lseek+read pair PER ROW: 50 of them per
     * deck, 100 per tick across the two, and each pair measured ~260 us -- 93% of a core, and a
     * THIRTY MILLISECOND STALL on the paint thread every tick, which is also the thread that owns
     * the zones' latches. A framebuffer is a device you ask, not memory you walk: the same pixels
     * out of a mapping are a memcpy, and the stall disappears rather than being spread out.
     *
     * This is what the sibling project (full-touch-ui-xdj-rx3) does everywhere and it is worth
     * naming: no shim of theirs contains a /dev/fb read(), a lseek or an FBIO* at all. They get a
     * CPU pointer -- DirectFB's Lock(DSLF_READ) -- and poke a few pixels. There is no reason for
     * this project to ask the kernel for bytes it can already see. */
    fb_map_len = (size_t)fb_pitch * (size_t)fb_h;
    fb_map = real_mmap(NULL, fb_map_len, PROT_READ, MAP_SHARED, fb_fd, 0);
    if (fb_map == (void *)-1 || fb_map == NULL) {
        fb_map = NULL;
        pointsrc_log("badge: could not map %s (%m) -- no ring (the badge still works)", FB_PATH);
        return;
    }
}

/* Take the 95 x 50 the badge already occupies out of rbp's buffer and put it in the plane, then
 * draw the ring over it. The plane is RGB565 and so is the page, so this is a row copy and not a
 * conversion -- the same reason drmband.h gives for menu_paint() being unchanged. */
static int paint_plane(int deck, int ring)
{
    const int w = DB_DECK1_X1 - DB_DECK1_X0 + 1;   /* ONE box; deck 2's is the same */
    const int h = DB_Y1 - DB_Y0 + 1;
    int ly;
    unsigned short *dst = band[deck].pix;
    int x0;

    if (!fb_map || fb_bpp != 16 || band[deck].bpp != 16 ||
        band[deck].w != w || band[deck].h != h)
        return -1;

    /* A MEMCPY OUT OF THE MAPPING, ONE ROW AT A TIME, AND NO SYSCALLS AT ALL. The old loop was a
     * lseek+read pair per row -- see fb_open_once() for the measurement and why the mapping is the
     * whole difference. The row arithmetic is unchanged and deliberately so: the same pitch, the
     * same column, the same bytes, so this cannot change what the ring looks like. */
    x0 = deck ? DB_DECK2_X0 : DB_DECK1_X0;
    for (ly = 0; ly < h; ly++)
        memcpy((char *)db_cur[deck] + (size_t)ly * (size_t)w * sizeof(unsigned short),
               fb_map + (size_t)(DB_Y0 + ly) * (size_t)fb_pitch
                      + (size_t)x0 * (size_t)(fb_bpp / 8),
               (size_t)w * sizeof(unsigned short));

    /* NOTHING MOVED AND THE RING DID NOT CHANGE: leave the buffer alone. This is the branch that
     * keeps the plane still, and it is the common one. */
    if (db_have[deck] && db_shown[deck] == (signed char)ring &&
        memcmp(db_cur[deck], db_prev[deck], sizeof db_cur[0]) == 0)
        return 0;

    memcpy(band[deck].pix, db_cur[deck], sizeof db_cur[0]);
    memcpy(db_prev[deck], db_cur[deck], sizeof db_cur[0]);
    db_have[deck]  = 1;
    db_shown[deck] = (signed char)ring;

    /* The ring, by the one implementation that draws one -- into a view over the plane's own
     * buffer, whose rect is local (0,0)-(w-1,h-1) because the buffer IS the badge box. */
    {
        struct menu_view v;

        memset(&v, 0, sizeof v);
        v.pix = (unsigned char *)dst;
        v.pitch = band[deck].pitch;
        v.bpp = band[deck].bpp;
        v.dw = band[deck].w;
        v.dh = band[deck].h;
        db_last_pv = menu_pixel(band[deck].bpp, MENU_PLAY);
        if (ring)
            deckbadge_ring(&v, 0, 0, w - 1, h - 1, db_last_pv);
    }
    return 0;
}

static int setup_once(int deck)
{
    static int failed[2];
    const int w = DB_DECK1_X1 - DB_DECK1_X0 + 1;
    const int h = DB_Y1 - DB_Y0 + 1;

    if (failed[deck])
        return -1;
    if (usable[deck])
        return 0;
    drm_band_want_rank(DRM_RANK_RING);
    if (drm_band_setup(&band[deck], w, h, 16) != 0) {
        /* Said once per deck and then remembered: a machine that cannot have a third plane is a
         * machine with no ring, and that must not cost the player anything. */
        failed[deck] = 1;
        pointsrc_log("badge: deck%d has no overlay plane -- no play ring (the badge still works)",
                     deck + 1);
        return -1;
    }
    usable[deck] = 1;
    return 0;
}

void deckbadge_plane_sync(const int *playing, int on_screen)
{
    int deck;

    if (!tried) {
        tried = 1;
        fb_open_once();
        pointsrc_log("badge: fb %dx%d %d bpp pitch %d bytes, mapped %zu bytes -- the ring's planes "
                     "copy from memory here, not from a read()",
                     fb_w, fb_h, fb_bpp, fb_pitch, fb_map_len);
        /* SET UP NOW, NOT ON THE FIRST PLAY. A third and fourth overlay plane either can be had on
         * this machine or cannot, and that is worth knowing at start-up rather than the moment the
         * operator hits play -- drm_band_setup() logs its own failures through pointsrc_log(). */
        setup_once(0);
        setup_once(1);
    }

    for (deck = 0; deck < 2; deck++) {
        int want = on_screen && playing[deck];

        if (want) {
            if (run[deck] < DB_SHOW_TICKS)
                run[deck]++;
            idle[deck] = 0;
        } else {
            if (idle[deck] < DB_HIDE_TICKS)
                idle[deck]++;
            run[deck] = 0;
        }

        if (!up[deck] && run[deck] < DB_SHOW_TICKS)
            continue;                       /* not for long enough to be playing */
        if (up[deck] && idle[deck] < DB_HIDE_TICKS) {
            /* RE-COPIED EVERY TICK, and this is the fix for the fossil. The plane is ours alone,
             * so writing its buffer IS what the display shows -- but the image in it is a COPY of
             * rbp's, and rbp's changes: a screen change, a track, a title. Copying it once froze
             * whatever was under those rows at that instant, which is how a browse screen's icon
             * came to sit in the deck badge for the rest of a session. 9.5 KB is a fraction of a
             * line time. No drm_band_show() here: the plane is already scanning this buffer. */
            /* THE RING IS DRAWN FOR AS LONG AS THE PLANE IS UP, not per tick from the bit. Making
             * it follow the bit every tick strobed: the bit flaps fast (3583 transitions in one
             * session) and the whole box read as flickering. The copy still runs every tick --
             * that is what keeps the box from fossilising -- but the ring on top of it is steady,
             * and the plane's own two thresholds decide when it exists. Synchronising it with the
             * drawer's PLAY needs the drawer's own repaint gate understood first, rather than
             * another guess. */
            if (++copy_ticks[deck] >= DB_COPY_TICKS) {
                copy_ticks[deck] = 0;
                paint_plane(deck, 1);
            }
            continue;                       /* rbp cannot reach the plane, only we can */
        }

        if (!want) {
            drm_band_hide(&band[deck]);
            up[deck] = 0;
            pointsrc_log("badge: deck%d ring down", deck + 1);
            continue;
        }
        if (setup_once(deck) != 0)
            continue;
        if (paint_plane(deck, want) != 0) {   /* `want` is the bit as it is THIS tick */
            pointsrc_log("badge: deck%d ring could not be drawn -- left off", deck + 1);
            continue;
        }
        if (drm_band_show(&band[deck], deck ? DB_DECK2_X0 : DB_DECK1_X0, DB_Y0) == 0) {
            pointsrc_log("badge: deck%d ring up at %d,%d, drawn with pv %#x", deck + 1,
                         deck ? DB_DECK2_X0 : DB_DECK1_X0, DB_Y0, db_last_pv);
            up[deck] = 1;
            copy_ticks[deck] = 0;          /* the paint just done is the copy's tick zero */
            pointsrc_log("badge: deck%d ring up at %d,%d, drawn in pv %#x", deck + 1,
                         deck ? DB_DECK2_X0 : DB_DECK1_X0, DB_Y0, db_last_pv);
        }
    }
}

void deckbadge_plane_teardown(void)
{
    int deck;

    for (deck = 0; deck < 2; deck++) {
        if (usable[deck]) {
            drm_band_teardown(&band[deck]);
            usable[deck] = 0;
            up[deck] = 0;
        }
    }
}

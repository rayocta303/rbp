/* SPDX-License-Identifier: MPL-2.0 */
#include "rx3_images.h"
#include "rx3_hooks.h"
#include "rx3_log.h"
/* Three suggestion colours need five native states and one badge each. */
#define IMAGE_LIMIT 24u
#define IMAGE_FIRST 0x1700u
#define IMAGE_PIXELS 1024u
struct variant {
    const void *owner;
    unsigned int source, ready, bitmap_width, bitmap_height;
    const void *source_base;
    uint32_t source_offset;
    uint16_t source_width, source_height;
    uint16_t from, to, pixels[IMAGE_PIXELS];
    uint32_t record[11];
};
static struct variant variants[IMAGE_LIMIT];
/* Preserve antialiasing shades along the source colour's RGB565 ray. */
static uint16_t recolour(uint16_t pixel,uint16_t from,uint16_t to)
{
    unsigned r=pixel>>11,g=(pixel>>5)&63,b=pixel&31;
    unsigned fr=from>>11,fg=(from>>5)&63,fb=from&31;
    if(r*fg!=g*fr || r*fb!=b*fr || g*fb!=b*fg)return pixel;
    unsigned scale=fr?fr:(fg?fg:fb),amount=fr?r:(fg?g:b);
    if(!scale || amount>scale)return pixel==from?to:pixel;
    return (uint16_t)((((to>>11)*amount+scale/2)/scale)<<11 |
        (((((to>>5)&63)*amount+scale/2)/scale)<<5) |
        (((to&31)*amount+scale/2)/scale));
}
static unsigned int register_recolour(const void *owner,unsigned int source,uint16_t from,uint16_t to)
{
    if (!owner || source>=IMAGE_FIRST || from==to) return 0;
    for(unsigned int i=0;i<IMAGE_LIMIT;i++) {
        struct variant *v=&variants[i];
        if(v->owner) continue;
        v->source=source;v->from=from;v->to=to;v->ready=0;
        v->bitmap_width=v->bitmap_height=0;
        __atomic_store_n(&v->owner,owner,__ATOMIC_SEQ_CST);
        return IMAGE_FIRST+i;
    }
    return 0;
}
unsigned int rx3_image_register_bitmap(const void *owner,unsigned int source,
    const uint16_t *pixels,unsigned int width,unsigned int height)
{
    if(!owner || !pixels || source>=IMAGE_FIRST || !width || !height ||
       width>IMAGE_PIXELS || height>IMAGE_PIXELS/width) return 0;
    for(unsigned int i=0;i<IMAGE_LIMIT;i++) {
        struct variant *v=&variants[i];
        if(v->owner) continue;
        v->source=source;v->bitmap_width=width;v->bitmap_height=height;
        v->ready=0;memcpy(v->pixels,pixels,width*height*2u);
        __atomic_store_n(&v->owner,owner,__ATOMIC_SEQ_CST);
        return IMAGE_FIRST+i;
    }
    return 0;
}
/* Composed pad labels (148x20): the frame and arrow of one stock image with
 * glyphs cut from others, all read from the player's own table on the render
 * thread, so no native artwork is copied anywhere else. Used by
 * beatjump-custom for jump values the player has no image for. */
#define LABEL_FIRST (IMAGE_FIRST + IMAGE_LIMIT + 8u)
#define LABEL_LIMIT 32u
#define LABEL_WIDTH 148u
#define LABEL_HEIGHT 20u
#define LABEL_GLYPHS 4u
/* Columns the stock digits use; the arrows sit outside them. */
#define LABEL_CLEAR_LEFT 34u
#define LABEL_CLEAR_RIGHT 114u
#define LABEL_ROW_FIRST 2u
#define LABEL_ROW_LAST 17u
#define LABEL_GAP 1u
struct label {
    const void *owner;
    unsigned int frame, glyphs, ready;
    unsigned int source[LABEL_GLYPHS];
    uint8_t index[LABEL_GLYPHS];
    const void *base;
    uint16_t pixels[LABEL_WIDTH * LABEL_HEIGHT];
    uint32_t record[11];
};
static struct label labels[LABEL_LIMIT];

static unsigned int register_label(const void *owner, unsigned int frame,
                                   const unsigned int *source, const uint8_t *index,
                                   unsigned int glyphs)
{
    if (!owner || !source || !index || !glyphs || glyphs > LABEL_GLYPHS ||
        frame >= RX3_STOCK_IMAGE_COUNT) return 0;
    for (unsigned int g = 0; g < glyphs; g++)
        if (source[g] >= RX3_STOCK_IMAGE_COUNT) return 0;
    for (unsigned int i = 0; i < LABEL_LIMIT; i++) {
        struct label *l = &labels[i];
        if (l->owner) continue;
        l->frame = frame; l->glyphs = glyphs; l->ready = 0;
        for (unsigned int g = 0; g < glyphs; g++) { l->source[g] = source[g]; l->index[g] = index[g]; }
        __atomic_store_n(&l->owner, owner, __ATOMIC_SEQ_CST);
        return LABEL_FIRST + i;
    }
    return 0;
}

/* A 148x20 direct-colour stock record, or null. */
static const uint16_t *label_source(void *(*lookup)(unsigned int), const void *base,
                                    unsigned int id, const uint8_t **record_out)
{
    const uint8_t *record = lookup(id);
    uint16_t width, height;
    uint32_t offset;
    if (!record) return 0;
    memcpy(&width, record + 4, 2); memcpy(&height, record + 6, 2);
    if (width != LABEL_WIDTH || height != LABEL_HEIGHT || record[0x19] ||
        (record[0x18] != 1 && record[0x18] != 2)) return 0;
    memcpy(&offset, record + 0x20, 4);
    if (record_out) *record_out = record;
    return (const void *)((unsigned long)base + offset);
}

static int label_ink(const uint16_t *pixels, unsigned int x, uint16_t ground)
{
    for (unsigned int y = LABEL_ROW_FIRST; y <= LABEL_ROW_LAST; y++) {
        uint16_t p = pixels[y * LABEL_WIDTH + x];
        if (p != ground && p != 0xf81fu) return 1;
    }
    return 0;
}

/* Columns [from, to) of glyph `which` (runs of inked columns, left to right). */
static int label_glyph(const uint16_t *pixels, unsigned int which,
                       unsigned int *from, unsigned int *to)
{
    uint16_t ground = pixels[10u * LABEL_WIDTH + 40u];
    unsigned int run = 0;
    for (unsigned int x = LABEL_CLEAR_LEFT - 14u; x < LABEL_CLEAR_RIGHT + 14u; x++) {
        if (!label_ink(pixels, x, ground)) continue;
        unsigned int end = x;
        while (end < LABEL_CLEAR_RIGHT + 14u && label_ink(pixels, end, ground)) end++;
        if (run++ == which) { *from = x; *to = end; return 1; }
        x = end;
    }
    return 0;
}

static int label_compose(struct label *l, void *(*lookup)(unsigned int), const void *base)
{
    const uint8_t *frame_record = 0;
    const uint16_t *frame = label_source(lookup, base, l->frame, &frame_record);
    if (!frame) return 0;
    unsigned int from[LABEL_GLYPHS], to[LABEL_GLYPHS], width = 0;
    const uint16_t *glyph[LABEL_GLYPHS];
    for (unsigned int g = 0; g < l->glyphs; g++) {
        glyph[g] = label_source(lookup, base, l->source[g], 0);
        if (!glyph[g] || !label_glyph(glyph[g], l->index[g], &from[g], &to[g])) return 0;
        width += to[g] - from[g] + (g ? LABEL_GAP : 0u);
    }
    if (width > LABEL_CLEAR_RIGHT - LABEL_CLEAR_LEFT) return 0;
    memcpy(l->pixels, frame, sizeof(l->pixels));
    uint16_t ground = frame[10u * LABEL_WIDTH + 40u];
    for (unsigned int y = LABEL_ROW_FIRST; y <= LABEL_ROW_LAST; y++)
        for (unsigned int x = LABEL_CLEAR_LEFT; x < LABEL_CLEAR_RIGHT; x++)
            l->pixels[y * LABEL_WIDTH + x] = ground;
    unsigned int x = (LABEL_WIDTH - width) / 2u;
    for (unsigned int g = 0; g < l->glyphs; g++) {
        uint16_t source_ground = glyph[g][10u * LABEL_WIDTH + 40u];
        for (unsigned int c = from[g]; c < to[g]; c++, x++)
            for (unsigned int y = LABEL_ROW_FIRST; y <= LABEL_ROW_LAST; y++) {
                uint16_t p = glyph[g][y * LABEL_WIDTH + c];
                if (p != source_ground && p != 0xf81fu) l->pixels[y * LABEL_WIDTH + x] = p;
            }
        x += LABEL_GAP;
    }
    memcpy(l->record, frame_record, 44);
    memset((uint8_t *)l->record + 0x24, 0, 4);
    return 1;
}

static void *label_resolve(unsigned int id, void *(*lookup)(unsigned int), const void *base)
{
    struct label *l = &labels[id - LABEL_FIRST];
    if (!__atomic_load_n(&l->owner, __ATOMIC_SEQ_CST)) return 0;
    if (!l->ready || l->base != base) {
        if (!label_compose(l, lookup, base)) return 0;
        l->base = base; l->ready = 1;
    }
    uint32_t offset = (uint32_t)((unsigned long)l->pixels - (unsigned long)base);
    memcpy((uint8_t *)l->record + 0x20, &offset, 4);
    uint32_t pixels = (uint32_t)(unsigned long)l->pixels;
    memcpy((uint8_t *)l->record + 0x0c, &pixels, 4);
    return l->record;
}

static void unregister_owner(const void *owner)
{
    for(unsigned int i=0;i<IMAGE_LIMIT;i++)
        if(variants[i].owner==owner) __atomic_store_n(&variants[i].owner,0,__ATOMIC_SEQ_CST);
    for(unsigned int i=0;i<LABEL_LIMIT;i++)
        if(labels[i].owner==owner) __atomic_store_n(&labels[i].owner,0,__ATOMIC_SEQ_CST);
}
unsigned int rx3_image_count(void)
{
    unsigned int n=0;
    for(unsigned int i=0;i<IMAGE_LIMIT;i++) n+=variants[i].owner!=0;
    return n;
}
/* Firmware adapter calls on the serialized native rendering thread. Offsets
   always use the current image-table base, including after a theme change. */
void *rx3_image_resolve(unsigned int id,void *(*lookup)(unsigned int),const void *base)
{
    if(id>=LABEL_FIRST && id<LABEL_FIRST+LABEL_LIMIT && lookup && base)
        return label_resolve(id,lookup,base);
    if(id<IMAGE_FIRST || id>=IMAGE_FIRST+IMAGE_LIMIT || !lookup || !base) return 0;
    struct variant *v=&variants[id-IMAGE_FIRST];
    if(!__atomic_load_n(&v->owner,__ATOMIC_SEQ_CST)) return 0;
    const uint8_t *source=lookup(v->source);
    if(!source) return 0;
    uint16_t width,height;
    uint32_t offset;
    memcpy(&width,source+4,2);memcpy(&height,source+6,2);
    unsigned int count=(unsigned int)width*height;
    if(!width || !height || count>IMAGE_PIXELS || source[0x19] ||
       (source[0x18]!=1 && source[0x18]!=2)) return 0;
    memcpy(v->record,source,44);
    memcpy(&offset,source+0x20,4);
    if(v->bitmap_width) {
        width=(uint16_t)v->bitmap_width;height=(uint16_t)v->bitmap_height;
        memcpy((uint8_t *)v->record+4,&width,2);
        memcpy((uint8_t *)v->record+6,&height,2);
        /* The replacement is raw RGB565, with no source palette. */
        memset((uint8_t *)v->record+0x24,0,4);
    } else if(!v->ready || v->source_base!=base || v->source_offset!=offset ||
       v->source_width!=width || v->source_height!=height) {
        const uint16_t *pixels=(const void *)((unsigned long)base+offset);
        /* Offsets wrap at 32 bits on ARM; native tests use a nearby fixture. */
        for(unsigned int i=0;i<count;i++) v->pixels[i]=recolour(pixels[i],v->from,v->to);
        v->source_base=base;v->source_offset=offset;
        v->source_width=width;v->source_height=height;v->ready=1;
    }
    offset=(uint32_t)((unsigned long)v->pixels-(unsigned long)base);
    memcpy((uint8_t *)v->record+0x20,&offset,4);
    /* lookup() already resolved the native record's absolute pixel pointer.
       Returning our record bypasses that native fixup, so resolve ours too. */
    uint32_t pixels=(uint32_t)(unsigned long)v->pixels;
    memcpy((uint8_t *)v->record+0x0c,&pixels,4);
    return v->record;
}

/* Native record replacements. The pixels belong to the contributor and stay
   referenced by every table built after registration. */
#define REPLACEMENT_LIMIT 4u
struct replacement {
    const void *owner;
    unsigned int image, width, height;
    const uint16_t *dark, *light;
};
static struct replacement replacements[REPLACEMENT_LIMIT];
static uint8_t *dark_table, *light_table;
static const uint8_t *stock_table;
static volatile int tables_published, light_selected, fill_suspended;
static const void *policy_owner;
static const struct rx3_image_policy *policy;

/* The display-mode fill adapter. The player keeps the colour of its next
   solid fill in one word: blue at bits 0..4, green at 8..13, red at 16..20.
   A rectangle carries its width and height at offsets 8 and 10. */
#define HW_FILL_RECT ((unsigned long)0x001a46dc)
#define FILL_REGISTER ((unsigned long)0x02459748)
static const uint8_t hw_fill_rect_guard[8] = {0xf0,0x41,0x2d,0xe9,0x08,0xd0,0x4d,0xe2};
typedef void (*hw_fill_rect_fn)(void *, const uint8_t *);
static struct installed_hook fill_hook;
static hw_fill_rect_fn original_fill;
static unsigned int fill_active;

static int replaced(unsigned int image)
{
    for (unsigned int i = 0; i < REPLACEMENT_LIMIT; i++)
        if (replacements[i].owner && replacements[i].image == image) return 1;
    return 0;
}

static int replace_native(const void *owner, unsigned int image, const uint16_t *dark,
                          const uint16_t *light, unsigned int width, unsigned int height)
{
    if (!owner || !dark || !width || !height || width > 0xffffu || height > 0xffffu ||
        image >= RX3_STOCK_IMAGE_COUNT || replaced(image) ||
        __atomic_load_n(&tables_published, __ATOMIC_SEQ_CST)) return 0;
    for (unsigned int i = 0; i < REPLACEMENT_LIMIT; i++) {
        struct replacement *r = &replacements[i];
        if (r->owner) continue;
        r->image = image; r->width = width; r->height = height;
        r->dark = dark; r->light = light; r->owner = owner;
        return 1;
    }
    return 0;
}

static int release_native(const void *owner)
{
    if (__atomic_load_n(&tables_published, __ATOMIC_SEQ_CST)) return 0;
    for (unsigned int i = 0; i < REPLACEMENT_LIMIT; i++)
        if (replacements[i].owner == owner) memset(&replacements[i], 0, sizeof(replacements[i]));
    return 1;
}

/* Called by the table builder. The format byte is 2, the variant that
   honours the colour key: replacements may leave parts of themselves
   unpainted. That is a reading of one released build, not a measurement. */
void rx3_image_install_replacements(uint8_t *table, int light)
{
    for (unsigned int i = 0; i < REPLACEMENT_LIMIT; i++) {
        const struct replacement *r = &replacements[i];
        if (!r->owner) continue;
        uint8_t *record = table + r->image * 44u;
        if (light) {
            if (!r->light) continue;
            uint32_t offset = (uint32_t)(unsigned long)r->light - (uint32_t)(unsigned long)table;
            memcpy(record + 0x20u, &offset, 4u);
            continue;
        }
        uint16_t width = (uint16_t)r->width, height = (uint16_t)r->height;
        uint32_t pixels = (uint32_t)(unsigned long)r->dark - (uint32_t)(unsigned long)table;
        uint32_t no_palette = 0u;
        memcpy(record + 4u, &width, 2u);
        memcpy(record + 6u, &height, 2u);
        record[0x18u] = 2u;
        record[0x19u] = 0u;
        memcpy(record + 0x20u, &pixels, 4u);
        memcpy(record + 0x24u, &no_palette, 4u);
        rx3_log_number("native image replaced = ", r->image);
    }
}

int rx3_image_variants_wanted(void) { return policy != 0; }

void rx3_image_publish_tables(uint8_t *dark, uint8_t *light, const uint8_t *stock)
{
    dark_table = dark;
    light_table = light;
    stock_table = stock;
    __atomic_store_n(&tables_published, 1, __ATOMIC_SEQ_CST);
}

void rx3_image_suspend_fill(int suspended) { fill_suspended = suspended; }
int rx3_image_is_light(void) { return light_selected; }

void rx3_image_drawn(unsigned int image)
{
    const struct rx3_image_policy *current = __atomic_load_n(&policy, __ATOMIC_SEQ_CST);
    if (current && current->drawn && image < RX3_STOCK_IMAGE_COUNT) current->drawn(image);
}

/* Indexed windows (rbp 1.20, NS_PALRender_DrawText @0x1d23e4 and
 * DS_GR_SetWindowColor @0x1a0680): the window of a text model is the handle
 * in the table @0x05a15f68 at the byte-swapped halfword +0x10 of the model.
 * A window with format 2 (+0x08) draws text with an index into its palette
 * (+0x10): the low byte of the ink field. The palette keeps 256 entries of
 * four bytes from +4, and both handles are live while bit 30 of their flag
 * word (+0x18 window, +0 palette) is set. */
#ifdef RX3_TEST_WINDOWS
extern const uint8_t *rx3_test_windows[];
#define RX3_WINDOW_TABLE ((unsigned long)rx3_test_windows)
#else
#define RX3_WINDOW_TABLE ((unsigned long)0x05a15f68)
#endif
#define RX3_WINDOW_FORMAT 0x08u
#define RX3_WINDOW_PALETTE 0x10u
#define RX3_WINDOW_FLAGS 0x18u
#define RX3_WINDOW_INDEXED 2u
#define RX3_HANDLE_LIVE 0x40000000u
#define RX3_PALETTE_ENTRIES 4u
#define RX3_PALETTE_SIZE 256u
/* A replacement entry must sit this close (sum of channel steps) to the ink
   the policy asked for, or the index is left alone. */
#define RX3_PALETTE_MATCH 0x90u

static const uint8_t *text_window(const void *text)
{
    uint16_t id;
    memcpy(&id, (const uint8_t *)text + RX3_TEXT_LAYER_OFFSET, 2u);
    unsigned int slot = ((unsigned int)(id & 0xffu) << 8) + (id >> 8);
    const uint8_t *const *table = (const uint8_t *const *)RX3_WINDOW_TABLE;
    const uint8_t *window = table[slot];
    uint32_t flags;
    if (!window) return 0;
    memcpy(&flags, window + RX3_WINDOW_FLAGS, 4u);
    return (flags & RX3_HANDLE_LIVE) ? window : 0;
}

static uint32_t palette_rgb(const uint8_t *palette, unsigned int index)
{
    const uint8_t *entry = palette + RX3_PALETTE_ENTRIES + index * 4u;
    return (uint32_t)entry[0] << 16 | (uint32_t)entry[1] << 8 | entry[2];
}

/* Render thread only: one small cache of index swaps per palette. */
/* Each swap remembers the colour it was decided from: a palette filled or
   changed after the decision (RX3: light text stayed white until SEARCH
   flushed the cache) is decided again instead of reusing a stale answer. */
static struct { const uint8_t *palette; uint32_t rgb; uint8_t from, to, valid; } palette_cache[16];
static unsigned int palette_cache_next;

static uint32_t indexed_ink(const struct rx3_image_policy *current, const uint8_t *window,
                            uint32_t ink, unsigned int layer)
{
    const uint8_t *palette;
    uint32_t flags;
    memcpy(&palette, window + RX3_WINDOW_PALETTE, sizeof(palette));
    if (!palette) return ink;
    memcpy(&flags, palette, 4u);
    if (!(flags & RX3_HANDLE_LIVE)) return ink;
    unsigned int index = ink & 0xffu;
    uint32_t rgb = palette_rgb(palette, index);
    for (unsigned int i = 0; i < 16u; i++)
        if (palette_cache[i].valid && palette_cache[i].palette == palette &&
            palette_cache[i].from == index && palette_cache[i].rgb == rgb)
            return (ink & ~0xffu) | palette_cache[i].to;
    uint32_t wanted = current->text_ink(rgb, layer);
    unsigned int best = index;
    if (wanted != rgb) {
        unsigned int best_distance = RX3_PALETTE_MATCH;
        for (unsigned int i = 0; i < RX3_PALETTE_SIZE; i++) {
            uint32_t candidate = palette_rgb(palette, i);
            unsigned int distance = 0;
            for (unsigned int shift = 0; shift < 24u; shift += 8u) {
                int a = (int)((candidate >> shift) & 0xffu), b = (int)((wanted >> shift) & 0xffu);
                distance += (unsigned int)(a > b ? a - b : b - a);
            }
            if (distance < best_distance) { best_distance = distance; best = i; }
        }
    }
    unsigned int slot = palette_cache_next++ & 15u;
    palette_cache[slot].palette = palette;
    palette_cache[slot].rgb = rgb;
    palette_cache[slot].from = (uint8_t)index;
    palette_cache[slot].to = (uint8_t)best;
    palette_cache[slot].valid = 1u;
    return (ink & ~0xffu) | best;
}

/* The box behind a text. NS_PALRender_DrawText fills the text's rectangle
 * with the colour at +0x44 before the glyphs (rbp 1.20 @0x1d2534..0x1d2614):
 * a palette index in its low byte for indexed windows, RGB888 for format 9.
 * Dark for a dark room, so in the light table every label sat in a black box.
 * The same policy as solid fills decides it, as a small rectangle. */
#define RX3_TEXT_BACK_OFFSET 0x44u
#define RX3_WINDOW_RGB888 9u
static unsigned int fill_word(uint32_t rgb)
{
    return ((rgb >> 19) & 0x1fu) << 16 | ((rgb >> 10) & 0x3fu) << 8 | ((rgb >> 3) & 0x1fu);
}
static uint32_t fill_rgb(unsigned int word)
{
    uint32_t red = (word >> 16) & 0x1fu, green = (word >> 8) & 0x3fu, blue = word & 0x1fu;
    return (red << 3 | red >> 2) << 16 | (green << 2 | green >> 4) << 8 | (blue << 3 | blue >> 2);
}
static unsigned int palette_nearest(const uint8_t *palette, uint32_t wanted, unsigned int fallback)
{
    unsigned int best = fallback, best_distance = RX3_PALETTE_MATCH;
    for (unsigned int i = 0; i < RX3_PALETTE_SIZE; i++) {
        uint32_t candidate = palette_rgb(palette, i);
        unsigned int distance = 0;
        for (unsigned int shift = 0; shift < 24u; shift += 8u) {
            int a = (int)((candidate >> shift) & 0xffu), b = (int)((wanted >> shift) & 0xffu);
            distance += (unsigned int)(a > b ? a - b : b - a);
        }
        if (distance < best_distance) { best_distance = distance; best = i; }
    }
    return best;
}
static uint32_t text_back(const struct rx3_image_policy *current, const uint8_t *window,
                          uint32_t format, uint32_t back)
{
    if (!current->fill || !window) return back;
    if (format == RX3_WINDOW_RGB888) {
        unsigned int word = fill_word(back), wanted = current->fill(word, 1u, 1u);
        return wanted == word ? back : (back & 0xff000000u) | fill_rgb(wanted);
    }
    if (format != RX3_WINDOW_INDEXED) return back;
    const uint8_t *palette;
    uint32_t flags;
    memcpy(&palette, window + RX3_WINDOW_PALETTE, sizeof(palette));
    if (!palette) return back;
    memcpy(&flags, palette, 4u);
    if (!(flags & RX3_HANDLE_LIVE)) return back;
    unsigned int index = back & 0xffu, word = fill_word(palette_rgb(palette, index));
    unsigned int wanted = current->fill(word, 1u, 1u);
    if (wanted == word) return back;
    return (back & ~0xffu) | palette_nearest(palette, fill_rgb(wanted), index);
}

/* The ink swap is in place and only for this draw: the field is put back
   unless something else wrote it meanwhile, so a colour change the player
   makes is never undone. Counted with the fills, so release waits for it. */
void rx3_image_draw_text(void (*draw)(void *, void *), void *render, void *text)
{
    __atomic_add_fetch(&fill_active, 1u, __ATOMIC_SEQ_CST);
    const struct rx3_image_policy *current = __atomic_load_n(&policy, __ATOMIC_SEQ_CST);
    if (!current || !current->text_ink || !light_selected || fill_suspended || !text) {
        __atomic_sub_fetch(&fill_active, 1u, __ATOMIC_SEQ_CST);
        draw(render, text);
        return;
    }
    uint8_t *field = (uint8_t *)text + RX3_TEXT_INK_OFFSET;
    uint32_t ink, wanted, now;
    uint16_t layer;
    memcpy(&ink, field, 4u);
    memcpy(&layer, (const uint8_t *)text + RX3_TEXT_LAYER_OFFSET, 2u);
    const uint8_t *window = text_window(text);
    uint32_t format = 0;
    if (window) memcpy(&format, window + RX3_WINDOW_FORMAT, 4u);
    /* Ink 0 in an RGB888 window is the font's own colour, white: the browse
       list's titles (RX3 log 2026-10-15: format 9, ink 0). Ask the policy
       about white, so the light table gets its dark ink there too. */
    wanted = format == RX3_WINDOW_INDEXED ? indexed_ink(current, window, ink, layer)
           : format == RX3_WINDOW_RGB888 && ink == 0u
             ? (current->text_ink(0xffffffu, layer) == 0xffffffu ? 0u : current->text_ink(0xffffffu, layer))
             : current->text_ink(ink, layer);
    uint8_t *back_field = (uint8_t *)text + RX3_TEXT_BACK_OFFSET;
    uint32_t back, back_wanted, back_now;
    memcpy(&back, back_field, 4u);
    back_wanted = text_back(current, window, format, back);
    /* Light theme, browse list (RX3, 2026-10-15: titles stayed white). Its
       ink is a low-byte style the policy leaves alone: log each distinct
       (format, style, back) once, at most 40, to learn what the styles mean. */
    if (ink <= 0xffu && wanted == ink) {
        static uint32_t seen[40];
        static unsigned int seen_count;
        uint32_t key = format << 24 | (ink & 0xffu) << 16 | (back & 0xffffu);
        unsigned int known = 0u;
        for (unsigned int i = 0; i < seen_count; i++) known |= seen[i] == key;
        if (!known && seen_count < 40u) {
            seen[seen_count++] = key;
            rx3_log_number("light text style: format = ", format);
            rx3_log_number("  style = ", ink);
            rx3_log_number("  back = ", back);
            rx3_log_number("  layer = ", layer);
        }
    }
    __atomic_sub_fetch(&fill_active, 1u, __ATOMIC_SEQ_CST);
    if (wanted == ink && back_wanted == back) {
        draw(render, text);
        return;
    }
    memcpy(field, &wanted, 4u);
    memcpy(back_field, &back_wanted, 4u);
    draw(render, text);
    memcpy(&now, field, 4u);
    if (now == wanted) memcpy(field, &ink, 4u);
    memcpy(&back_now, back_field, 4u);
    if (back_now == back_wanted) memcpy(back_field, &back, 4u);
}

static void fill_hooked(void *target, const uint8_t *rect)
{
    __atomic_add_fetch(&fill_active, 1u, __ATOMIC_SEQ_CST);
    const struct rx3_image_policy *current = __atomic_load_n(&policy, __ATOMIC_SEQ_CST);
    if (current && current->fill && light_selected && rect && !fill_suspended) {
        volatile unsigned int *fill = (volatile unsigned int *)FILL_REGISTER;
        uint16_t width, height;
        memcpy(&width, rect + 8u, 2u);
        memcpy(&height, rect + 10u, 2u);
        unsigned int colour = *fill, wanted = current->fill(colour, width, height);
        if (wanted != colour) *fill = wanted;
    }
    original_fill(target, rect);
    __atomic_sub_fetch(&fill_active, 1u, __ATOMIC_SEQ_CST);
}

static int claim_variants(const void *owner, const struct rx3_image_policy *wanted)
{
    if (!owner || !wanted || policy_owner) return 0;
    if (!original_fill) {
        original_fill = (hw_fill_rect_fn)install_hook(&fill_hook, HW_FILL_RECT,
                                                      hw_fill_rect_guard, (void *)fill_hooked);
        if (!original_fill) return 0;
    }
    policy_owner = owner;
    __atomic_store_n(&policy, wanted, __ATOMIC_SEQ_CST);
    return 1;
}

static void select_variant(int light)
{
    light_selected = light != 0;
    uint8_t *table = light_selected && light_table ? light_table : dark_table;
    if (table) {
        __sync_synchronize();
        *(uint8_t **)RX3_IMAGE_TABLE_POINTER = table;
    }
}

static void release_variants(const void *owner)
{
    if (!owner || owner != policy_owner) return;
    __atomic_store_n(&policy, 0, __ATOMIC_SEQ_CST);
    select_variant(0);
    int detached = original_fill && detach_hook(&fill_hook);
    for (;;) {
        while (__atomic_load_n(&fill_active, __ATOMIC_SEQ_CST)) usleep(1000u);
        usleep(10000u);
        if (!__atomic_load_n(&fill_active, __ATOMIC_SEQ_CST)) break;
    }
    policy_owner = 0;
    if (!original_fill) return;
    if (!detached) {
        log_line("images: fill adapter retained after restore failure");
        return;
    }
    (void)release_hook(&fill_hook);
    original_fill = 0;
}

static int variants_ready(void) { return dark_table && light_table; }

static int native_image(unsigned int image, struct rx3_native_image *out)
{
    if (!out || !dark_table || image >= RX3_STOCK_IMAGE_COUNT || replaced(image)) return 0;
    const uint8_t *record = dark_table + image * 44u;
    uint16_t width, height;
    uint32_t offset;
    memcpy(&width, record + 4u, 2u);
    memcpy(&height, record + 6u, 2u);
    memcpy(&offset, record + 0x20u, 4u);
    const uint16_t *pixels = (const void *)(dark_table + offset);
    /* Records whose pixels sit inside the stock record array carry no bitmap
       of their own. */
    uint32_t from_stock = (uint32_t)(unsigned long)pixels - (uint32_t)(unsigned long)stock_table;
    if (stock_table && from_stock <= RX3_STOCK_IMAGE_COUNT * 44u + 43u) return 0;
    out->width = width;
    out->height = height;
    out->format = record[0x18u];
    out->paletted = record[0x19u];
    out->pixels = pixels;
    return 1;
}

static int publish_variant(unsigned int image, const uint16_t *pixels)
{
    if (!light_table || !pixels || image >= RX3_STOCK_IMAGE_COUNT || replaced(image)) return 0;
    uint32_t offset = (uint32_t)(unsigned long)pixels - (uint32_t)(unsigned long)light_table;
    memcpy(light_table + image * 44u + 0x20u, &offset, 4u);
    return 1;
}

static int light_active(void) { return light_selected; }

unsigned int rx3_image_contributions(void)
{
    unsigned int n = policy != 0;
    for (unsigned int i = 0; i < REPLACEMENT_LIMIT; i++) n += replacements[i].owner != 0;
    return n + rx3_image_count();
}

const struct rx3_image_service rx3_images={
    register_recolour, unregister_owner, rx3_image_register_bitmap,
    replace_native, release_native, claim_variants, release_variants,
    variants_ready, native_image, publish_variant, select_variant, light_active, register_label
};

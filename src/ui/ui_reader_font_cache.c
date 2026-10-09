#include "ui_reader_font_cache.h"
#include "epd_memory.h"
#include "src/misc/cache/lv_cache.h"
#include "src/misc/cache/class/lv_cache_lru_rb.h"
#include "src/draw/lv_draw_buf_private.h"
#include <string.h>

#define DBG_TAG "reader_font"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define GLYPH_BUDGET (8u * 1024u * 1024u)
#define GLYPH_NODE_LIMIT 8192u
#define SHARED_RESERVE (1024u * 1024u)
#define DESCRIPTOR_BUDGET (1024u * 1024u)
#define UNICODE_PAGES (0x110000u / 256u)

typedef struct
{
    uint32_t valid[8];
    lv_font_glyph_dsc_t glyphs[256];
} descriptor_page_t;

typedef struct font_binding
{
    struct font_binding *next;
    lv_font_t *font;
    bool (*descriptor)(const lv_font_t *, lv_font_glyph_dsc_t *, uint32_t, uint32_t);
    const void *(*bitmap)(lv_font_glyph_dsc_t *, lv_draw_buf_t *);
    void (*release)(const lv_font_t *, lv_font_glyph_dsc_t *);
    descriptor_page_t **descriptors;
} font_binding_t;

typedef struct
{
    lv_cache_slot_size_t slot;
    const lv_font_t *font;
    uint32_t glyph;
    lv_draw_buf_t *buffer;
} glyph_item_t;

static lv_cache_t *glyph_cache;
static font_binding_t *bindings;
static struct rt_mutex descriptor_lock;
static bool lock_initialized, preparing, prepare_adjacent;
static unsigned glyph_count;
static size_t descriptor_bytes;
static uint32_t descriptor_hits, descriptor_misses, bitmap_hits, bitmap_misses, prepared_count;

static font_binding_t *binding_find(const lv_font_t *font)
{
    for (font_binding_t *p = bindings; p; p = p->next)
        if (p->font == font) return p;
    return NULL;
}

static bool cached_descriptor(const lv_font_t *font, lv_font_glyph_dsc_t *glyph,
                              uint32_t letter, uint32_t next)
{
    font_binding_t *binding = binding_find(font);
    rt_mutex_take(&descriptor_lock, RT_WAITING_FOREVER);
    /* Reader fonts disable kerning. A caller that enables it must still get
     * the original pair-dependent advance, never a single-letter cached one. */
    if (font->kerning != LV_FONT_KERNING_NONE || letter >= 0x110000u)
    {
        bool found = binding->descriptor(font, glyph, letter, next);
        rt_mutex_release(&descriptor_lock);
        return found;
    }
    descriptor_page_t *page = binding->descriptors[letter >> 8];
    unsigned slot = letter & 255u;
    uint32_t bit = 1u << (slot & 31u);
    if (page && (page->valid[slot >> 5] & bit))
    {
        *glyph = page->glyphs[slot];
        ++descriptor_hits;
        rt_mutex_release(&descriptor_lock);
        return true;
    }
    ++descriptor_misses;
    bool found = binding->descriptor(font, glyph, letter, next);
    /* Never turn a temporary Tiny TTF allocation failure into a cached miss. */
    if (found)
    {
        if (!page && descriptor_bytes + sizeof(*page) <= DESCRIPTOR_BUDGET &&
            epd_app_psram_available() >= sizeof(*page) + SHARED_RESERVE)
        {
            page = epd_app_alloc(sizeof(*page), EPD_APP_PSRAM);
            if (page)
            {
                memset(page, 0, sizeof(*page));
                binding->descriptors[letter >> 8] = page;
                descriptor_bytes += sizeof(*page);
            }
        }
        if (page)
        {
            page->glyphs[slot] = *glyph;
            page->glyphs[slot].entry = NULL;
            page->valid[slot >> 5] |= bit;
        }
    }
    rt_mutex_release(&descriptor_lock);
    return found;
}

static lv_cache_compare_res_t glyph_compare(const glyph_item_t *a, const glyph_item_t *b)
{
    if (a->font != b->font) return (uintptr_t)a->font > (uintptr_t)b->font ? 1 : -1;
    if (a->glyph != b->glyph) return a->glyph > b->glyph ? 1 : -1;
    return 0;
}

static bool glyph_create(glyph_item_t *item, void *source)
{
    item->buffer = lv_draw_buf_dup_ex(lv_draw_buf_get_font_handlers(), source);
    if (!item->buffer) return false;
    lv_draw_buf_flush_cache(item->buffer, NULL);
    ++glyph_count;
    return true;
}

static void glyph_free(glyph_item_t *item, void *unused)
{
    LV_UNUSED(unused);
    if (item->buffer)
    {
        lv_draw_buf_destroy(item->buffer);
        --glyph_count;
    }
}

static size_t glyph_charge(uint32_t bytes)
{
    /* Pixel bytes plus a conservative allowance for buffer/node/heap headers;
     * no 1 KiB minimum for small glyphs. Node count is bounded independently. */
    return bytes + 256u;
}

static bool glyph_room(uint32_t bytes, bool evict)
{
    size_t charge = glyph_charge(bytes);
    /* Keep descriptor growth possible after the bitmap budget fills. */
    size_t reserve = SHARED_RESERVE + DESCRIPTOR_BUDGET - descriptor_bytes;
    while (glyph_count >= GLYPH_NODE_LIMIT ||
           epd_app_psram_available() < 2u * bytes + reserve ||
           lv_cache_get_free_size(glyph_cache, NULL) < charge)
    {
        if (!evict || !lv_cache_evict_one(glyph_cache, NULL)) return false;
    }
    return true;
}

static const void *cached_bitmap(lv_font_glyph_dsc_t *glyph, lv_draw_buf_t *scratch)
{
    /* The EPIC draw thread and UI layout may access the same file-backed
     * Tiny TTF font. Serialize the original callbacks as well as our metadata. */
    rt_mutex_take(&descriptor_lock, RT_WAITING_FOREVER);
    font_binding_t *binding = binding_find(glyph->resolved_font);
    glyph_item_t key = {.font = glyph->resolved_font, .glyph = glyph->gid.index};
    lv_cache_entry_t *entry = lv_cache_acquire(glyph_cache, &key, NULL);
    if (entry)
    {
        if (!preparing) ++bitmap_hits;
        glyph->entry = entry;
        const void *buffer = ((glyph_item_t *)lv_cache_entry_get_data(entry))->buffer;
        rt_mutex_release(&descriptor_lock);
        return buffer;
    }
    if (!preparing) ++bitmap_misses;
    uint32_t stride = lv_draw_buf_width_to_stride(glyph->box_w, LV_COLOR_FORMAT_A8);
    bool keep = glyph_room(stride * glyph->box_h + LV_DRAW_BUF_ALIGN,
                           !preparing || prepare_adjacent);
    if (!keep && preparing)
    {
        rt_mutex_release(&descriptor_lock);
        return NULL;
    }
    /* The original callback applies weight and writes back the rasterized data. */
    const lv_draw_buf_t *source = binding->bitmap(glyph, scratch);
    if (!source || !keep)
    {
        rt_mutex_release(&descriptor_lock);
        return source;
    }
    key.slot.size = glyph_charge(source->data_size);
    entry = lv_cache_acquire_or_create(glyph_cache, &key, (void *)source);
    if (!entry)
    {
        rt_mutex_release(&descriptor_lock);
        return source;
    }
    binding->release(binding->font, glyph);
    glyph->entry = entry;
    if (preparing) ++prepared_count;
    const void *buffer = ((glyph_item_t *)lv_cache_entry_get_data(entry))->buffer;
    rt_mutex_release(&descriptor_lock);
    return buffer;
}

static void cached_release(const lv_font_t *font, lv_font_glyph_dsc_t *glyph)
{
    if (!glyph->entry) return;
    rt_mutex_take(&descriptor_lock, RT_WAITING_FOREVER);
    if (lv_cache_entry_get_cache(glyph->entry) == glyph_cache)
    {
        lv_cache_release(glyph_cache, glyph->entry, NULL);
        glyph->entry = NULL;
    }
    else binding_find(font)->release(font, glyph);
    rt_mutex_release(&descriptor_lock);
}

void ui_reader_font_cache_reserve(size_t bytes)
{
    if (!glyph_cache) return;
    /* UI-thread allocation boundary (new TTF or application). References held
     * by EPIC must finish before reclaiming speculative glyph bitmaps. */
    lv_draw_wait_for_finish();
    rt_mutex_take(&descriptor_lock, RT_WAITING_FOREVER);
    size_t target = bytes > SIZE_MAX - SHARED_RESERVE ? SIZE_MAX : bytes + SHARED_RESERVE;
    while (epd_app_psram_available() < target)
        if (!lv_cache_evict_one(glyph_cache, NULL)) break;
    rt_mutex_release(&descriptor_lock);
}

bool ui_reader_font_cache_attach(lv_font_t *font)
{
    font_binding_t *binding = lv_malloc(sizeof(*binding));
    if (!binding) return false;
    size_t bytes = UNICODE_PAGES * sizeof(*binding->descriptors);
    ui_reader_font_cache_reserve(bytes);
    binding->descriptors = epd_app_alloc(bytes, EPD_APP_PSRAM);
    if (!binding->descriptors) { lv_free(binding); return false; }
    memset(binding->descriptors, 0, bytes);
    if (!glyph_cache)
    {
        glyph_cache = lv_cache_create(&lv_cache_class_lru_rb_size, sizeof(glyph_item_t),
            GLYPH_BUDGET, (lv_cache_ops_t){
                .compare_cb = (lv_cache_compare_cb_t)glyph_compare,
                .create_cb = (lv_cache_create_cb_t)glyph_create,
                .free_cb = (lv_cache_free_cb_t)glyph_free});
        if (!glyph_cache)
        {
            epd_app_free(binding->descriptors);
            lv_free(binding);
            return false;
        }
        lv_cache_set_name(glyph_cache, "READER_GLYPHS");
        descriptor_hits = descriptor_misses = bitmap_hits = bitmap_misses = prepared_count = 0;
    }
    if (!lock_initialized)
    {
        rt_mutex_init(&descriptor_lock, "font_dsc", RT_IPC_FLAG_PRIO);
        lock_initialized = true;
    }
    binding->font = font;
    binding->descriptor = font->get_glyph_dsc;
    binding->bitmap = font->get_glyph_bitmap;
    binding->release = font->release_glyph;
    binding->next = bindings;
    bindings = binding;
    font->get_glyph_dsc = cached_descriptor;
    font->get_glyph_bitmap = cached_bitmap;
    font->release_glyph = cached_release;
    return true;
}

void ui_reader_font_cache_detach(lv_font_t *font)
{
    font_binding_t **link = &bindings;
    while (*link && (*link)->font != font) link = &(*link)->next;
    if (!*link) return;
    lv_draw_wait_for_finish();
    lv_cache_drop_all(glyph_cache, NULL);
    font_binding_t *binding = *link;
    font->get_glyph_dsc = binding->descriptor;
    font->get_glyph_bitmap = binding->bitmap;
    font->release_glyph = binding->release;
    *link = binding->next;
    for (unsigned i = 0; i < UNICODE_PAGES; ++i)
    {
        if (!binding->descriptors[i]) continue;
        epd_app_free(binding->descriptors[i]);
        descriptor_bytes -= sizeof(descriptor_page_t);
    }
    epd_app_free(binding->descriptors);
    lv_free(binding);
    if (!bindings)
    {
        LOG_I("dsc hit/miss=%u/%u, draw bitmap hit/miss=%u/%u, prepared=%u",
              descriptor_hits, descriptor_misses, bitmap_hits, bitmap_misses, prepared_count);
        lv_cache_destroy(glyph_cache, NULL);
        glyph_cache = NULL;
    }
}

bool ui_reader_font_prepare(const lv_font_t *font, uint32_t letter, bool adjacent)
{
    if (letter < 0x20 || letter == 0x7f) return true;
    lv_font_glyph_dsc_t glyph;
    if (!lv_font_get_glyph_dsc(font, &glyph, letter, 0)) return false;
    if (glyph.is_placeholder || !glyph.box_w || !glyph.box_h ||
        !binding_find(glyph.resolved_font)) return true;
    glyph_item_t key = {.font = glyph.resolved_font, .glyph = glyph.gid.index};
    lv_cache_entry_t *entry = lv_cache_acquire(glyph_cache, &key, NULL);
    if (entry) { lv_cache_release(glyph_cache, entry, NULL); return true; }
    uint32_t stride = lv_draw_buf_width_to_stride(glyph.box_w, LV_COLOR_FORMAT_A8);
    /* Book-wide preparation may fill unused space but never evict nearby text. */
    if (!glyph_room(stride * glyph.box_h + LV_DRAW_BUF_ALIGN, adjacent)) return false;
    prepare_adjacent = adjacent;
    preparing = true;
    const void *bitmap = lv_font_get_glyph_bitmap(&glyph, NULL);
    preparing = false;
    bool cached = bitmap && glyph.entry && lv_cache_entry_get_cache(glyph.entry) == glyph_cache;
    lv_font_glyph_release_draw_data(&glyph);
    return cached;
}

#include "ui_font.h"
#include "ui_reader_font_cache.h"
#include "epd_memory.h"
#include "storage.h"
#include "storage_file.h"
#include "mem_map.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define DBG_TAG "ui_font"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#ifndef UI_BUILTIN_FONT_PATH
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"

extern const unsigned char epub_ttf_zlib_data[];
extern const int epub_ttf_zlib_data_size;
extern const uint32_t ui_builtin_font_signature;
extern const uint32_t ui_builtin_font_size;

/* Initialized on the UI thread before applications start. Fonts borrow these
 * immutable bytes for the lifetime of the system. */
static unsigned char *builtin_font_data;

static bool builtin_font_prepare(void)
{
    if (builtin_font_data) return true;
    unsigned char *data = epd_app_alloc(ui_builtin_font_size, EPD_APP_PSRAM);
    if (!data)
    {
        LOG_E("builtin font allocation failed: need=%u, PSRAM free=%u",
              (unsigned)ui_builtin_font_size, (unsigned)epd_app_psram_available());
        return false;
    }
    rt_tick_t start = rt_tick_get();
    mz_ulong size = ui_builtin_font_size;
    /* mz_uncompress validates the zlib stream's Adler-32 checksum. Its
     * temporary decoder state is heap allocated and released before return. */
    int result = mz_uncompress(data, &size, epub_ttf_zlib_data, epub_ttf_zlib_data_size);
    if (result != MZ_OK || size != ui_builtin_font_size)
    {
        LOG_E("builtin font inflate failed: result=%d, size=%u/%u",
              result, (unsigned)size, (unsigned)ui_builtin_font_size);
        epd_app_free(data);
        return false;
    }
    builtin_font_data = data;
    LOG_I("builtin font: %u -> %u bytes in RAM1, %u ms, PSRAM free=%u",
          (unsigned)epub_ttf_zlib_data_size, (unsigned)size,
          (unsigned)((uint64_t)(rt_tick_get() - start) * 1000 / RT_TICK_PER_SECOND),
          (unsigned)epd_app_psram_available());
    return true;
}
#endif

static lv_font_t *fonts[5];

const void *ui_font_builtin_data(size_t *size)
{
#ifndef UI_BUILTIN_FONT_PATH
    if (size) *size = builtin_font_data ? ui_builtin_font_size : 0;
    return builtin_font_data;
#else
    if (size) *size = 0;
    return NULL;
#endif
}

typedef struct font_blob
{
    struct font_blob *next;
    void *data;
    uint32_t size, signature;
    unsigned references;
    char path[96];
} font_blob_t;

/* Reader font creation/destruction is serialized by the UI thread. */
static font_blob_t *font_blobs;

static void font_blob_release(font_blob_t *blob)
{
    if (!blob || --blob->references) return;
    font_blob_t **link = &font_blobs;
    while (*link != blob) link = &(*link)->next;
    *link = blob->next;
    epd_app_free(blob->data);
    lv_free(blob);
}

static font_blob_t *font_blob_acquire(const char *path)
{
    struct stat st;
    font_blob_t *blob = NULL;
    storage_lock();
    int fd = -1;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (uint32_t)st.st_size > PSRAM_SIZE || strlen(path) >= sizeof(blob->path)) goto done;

    for (font_blob_t *item = font_blobs; item; item = item->next)
    {
        if (item->size != (uint32_t)st.st_size || strcmp(item->path, path)) continue;
        uint32_t signature;
        if (!storage_file_signature(path, &signature)) goto done;
        if (item->signature != signature) continue;
        ++item->references;
        blob = item;
        goto done;
    }
    fd = open(path, O_RDONLY);
    if (fd < 0) goto done;
    blob = lv_malloc_zeroed(sizeof(*blob));
    if (!blob) goto done;
    blob->size = st.st_size;
    ui_reader_font_cache_reserve(blob->size);
    blob->data = epd_app_alloc(blob->size, EPD_APP_PSRAM);
    if (!blob->data) goto failed;
    uint32_t position = 0, hash = 2166136261u;
    while (position < blob->size)
    {
        unsigned want = blob->size - position;
        if (want > 8192) want = 8192;
        uint8_t *bytes = (uint8_t *)blob->data + position;
        int n = read(fd, bytes, want);
        if (n <= 0) goto failed;
        for (int i = 0; i < n; ++i) hash = (hash ^ bytes[i]) * 16777619u;
        position += n;
    }
    blob->signature = hash;
    blob->references = 1;
    strcpy(blob->path, path);
    blob->next = font_blobs;
    font_blobs = blob;
    goto done;
failed:
    epd_app_free(blob->data);
    lv_free(blob);
    blob = NULL;
done:
    if (fd >= 0) close(fd);
    storage_unlock();
    return blob;
}

typedef struct
{
    unsigned weight;
    font_blob_t *blob;
    bool (*descriptor)(const lv_font_t *, lv_font_glyph_dsc_t *, uint32_t, uint32_t);
    const void *(*bitmap)(lv_font_glyph_dsc_t *, lv_draw_buf_t *);
} reader_weight_t;

static bool weighted_descriptor(const lv_font_t *font, lv_font_glyph_dsc_t *glyph,
                                uint32_t letter, uint32_t next)
{
    const reader_weight_t *style = font->user_data;
    if (!style->descriptor(font, glyph, letter, next)) return false;
    /* Reserve the extra ink width in both line measurement and drawing. */
    if (style->weight == 1 && glyph->box_w > 1 && glyph->box_h > 1) ++glyph->adv_w;
    return true;
}

static const void *weighted_bitmap(lv_font_glyph_dsc_t *glyph, lv_draw_buf_t *scratch)
{
    const reader_weight_t *style = glyph->resolved_font->user_data;
    lv_draw_buf_t *buffer = (lv_draw_buf_t *)style->bitmap(glyph, scratch);
    if (!style->weight || !buffer || (buffer->header.flags & LV_IMAGE_FLAGS_USER1) ||
        buffer->header.cf != LV_COLOR_FORMAT_A8) return buffer;
    /* Each reader font owns its Tiny TTF cache. Process each new bitmap once;
     * never repeatedly embolden an already cached glyph. */
    for (uint32_t y = 0; y < buffer->header.h; ++y)
    {
        uint8_t *row = buffer->data + y * buffer->header.stride;
        uint8_t previous = 0;
        for (uint32_t x = 0; x < buffer->header.w; ++x)
        {
            uint8_t current = row[x];
            if (style->weight == 1)
                row[x] = current > previous ? current : previous;
            else
            {
                uint8_t next = x + 1 < buffer->header.w ? row[x + 1] : 0;
                uint8_t edge = previous < next ? previous : next;
                if (edge > current) edge = current;
                /* Half-pixel horizontal erosion preserves one-pixel strokes. */
                row[x] = ((unsigned)current + edge + 1) / 2;
            }
            previous = current;
        }
    }
    buffer->header.flags |= LV_IMAGE_FLAGS_USER1;
    /* Includes stride padding; the registered handler writes back PSRAM. */
    lv_draw_buf_flush_cache(buffer, NULL);
    return buffer;
}

static bool reader_weight_apply(lv_font_t *font, unsigned weight, font_blob_t *blob)
{
    if (!weight && !blob) return true;
    reader_weight_t *style = lv_malloc(sizeof(*style));
    if (!style) return false;
    style->weight = weight;
    style->blob = blob;
    style->descriptor = font->get_glyph_dsc;
    style->bitmap = font->get_glyph_bitmap;
    font->user_data = style;
    font->get_glyph_dsc = weighted_descriptor;
    font->get_glyph_bitmap = weighted_bitmap;
    return true;
}

void ui_font_reader_destroy(lv_font_t *font)
{
    if (!font) return;
    ui_reader_font_cache_detach(font);
    reader_weight_t *style = font->get_glyph_bitmap == weighted_bitmap ? font->user_data : NULL;
    /* Tiny TTF does not own the bytes passed to create_data_ex. */
    lv_tiny_ttf_destroy(font);
    if (style)
    {
        font_blob_release(style->blob);
        lv_free(style);
    }
}

bool ui_font_init(void)
{
    static const uint32_t sizes[] = {24, 28, 36, 20, 64};
    static const lv_font_t *fallbacks[] = {
        &lv_font_montserrat_24, &lv_font_montserrat_28, &lv_font_montserrat_36,
        &lv_font_montserrat_20, &lv_font_montserrat_36
    };
    if (fonts[0]) return true;
#ifndef UI_BUILTIN_FONT_PATH
    if (!builtin_font_prepare()) return false;
#endif
    for (unsigned i = 0; i < sizeof(fonts) / sizeof(fonts[0]); ++i)
    {
#ifdef UI_BUILTIN_FONT_PATH
        fonts[i] = lv_tiny_ttf_create_file_ex(UI_BUILTIN_FONT_PATH, sizes[i],
                    LV_FONT_KERNING_NONE, LV_TINY_TTF_CACHE_GLYPH_CNT);
#else
        fonts[i] = lv_tiny_ttf_create_data_ex(builtin_font_data, ui_builtin_font_size, sizes[i],
                    LV_FONT_KERNING_NONE, LV_TINY_TTF_CACHE_GLYPH_CNT);
#endif
        if (!fonts[i])
        {
            while (i) lv_tiny_ttf_destroy(fonts[--i]);
            for (i = 0; i < sizeof(fonts) / sizeof(fonts[0]); ++i) fonts[i] = NULL;
#ifndef UI_BUILTIN_FONT_PATH
            epd_app_free(builtin_font_data);
            builtin_font_data = NULL;
#endif
            return false;
        }
        fonts[i]->fallback = fallbacks[i];
    }
    return true;
}

const lv_font_t *ui_font_small(void) { return fonts[0]; }
const lv_font_t *ui_font_body(void) { return fonts[1]; }
const lv_font_t *ui_font_title(void) { return fonts[2]; }
const lv_font_t *ui_font_caption(void) { return fonts[3]; }
const lv_font_t *ui_font_temperature(void) { return fonts[4]; }

lv_font_t *ui_font_reader_create(unsigned family, unsigned size, unsigned weight, uint32_t *identity,
                                lv_font_t **fallback)
{
    static const char *const paths[] = {NULL, "/fonts/Song.ttf", "/fonts/Hei.ttf",
                                        "/fonts/Kai.ttf", "/fonts/Monospace.ttf"};
    lv_font_t *font = NULL;
    font_blob_t *blob = NULL;
    *fallback = NULL;
    if (weight > 2) weight = 0;
    *identity = size ^ (weight << 20);
    if (family > 0 && family < sizeof(paths) / sizeof(paths[0]))
    {
        struct stat st;
        char path[96];
        snprintf(path, sizeof(path), "%s%s", STORAGE_FLASH_ROOT, paths[family]);
        if (storage_available(STORAGE_FLASH) && stat(path, &st) != 0)
        {
            char source[96];
            snprintf(source, sizeof(source), "%s%s", STORAGE_SD_ROOT, paths[family]);
            storage_file_import(source, path);
        }
        if (storage_path_available(path) && stat(path, &st) == 0)
        {
            blob = font_blob_acquire(path);
            if (blob)
            {
                font = lv_tiny_ttf_create_data_ex(blob->data, blob->size, size,
                                                 LV_FONT_KERNING_NONE, LV_TINY_TTF_CACHE_GLYPH_CNT);
                if (!font) { font_blob_release(blob); blob = NULL; }
            }
            if (!font)
                font = lv_tiny_ttf_create_file_ex(path, size, LV_FONT_KERNING_NONE, LV_TINY_TTF_CACHE_GLYPH_CNT);
            uint32_t signature;
            if (blob) *identity ^= blob->signature ^ (family << 24);
            else if (font && storage_file_signature(path, &signature)) *identity ^= signature ^ (family << 24);
        }
    }
#ifdef UI_BUILTIN_FONT_PATH
    lv_font_t *builtin = lv_tiny_ttf_create_file_ex(UI_BUILTIN_FONT_PATH, size,
                                                  LV_FONT_KERNING_NONE, LV_TINY_TTF_CACHE_GLYPH_CNT);
    uint32_t builtin_signature;
    if (storage_file_signature(UI_BUILTIN_FONT_PATH, &builtin_signature)) *identity ^= builtin_signature;
#else
    lv_font_t *builtin = builtin_font_data ?
        lv_tiny_ttf_create_data_ex(builtin_font_data, ui_builtin_font_size, size,
                                 LV_FONT_KERNING_NONE, LV_TINY_TTF_CACHE_GLYPH_CNT) : NULL;
    *identity ^= ui_builtin_font_signature;
#endif
    if (!builtin)
    {
        if (font) lv_tiny_ttf_destroy(font);
        font_blob_release(blob);
        return NULL;
    }
    builtin->fallback = &lv_font_montserrat_16;
    if (font)
    {
        font->fallback = builtin;
        *fallback = builtin;
    }
    else font = builtin;
    if (!reader_weight_apply(font, weight, blob))
    {
        font_blob_release(blob);
        lv_tiny_ttf_destroy(font);
        if (*fallback) lv_tiny_ttf_destroy(*fallback);
        *fallback = NULL;
        return NULL;
    }
    if (*fallback && !reader_weight_apply(*fallback, weight, NULL))
    {
        ui_font_reader_destroy(font);
        ui_font_reader_destroy(*fallback);
        *fallback = NULL;
        return NULL;
    }
    if (!ui_reader_font_cache_attach(font) ||
        (*fallback && !ui_reader_font_cache_attach(*fallback)))
    {
        ui_font_reader_destroy(font);
        ui_font_reader_destroy(*fallback);
        *fallback = NULL;
        return NULL;
    }
    return font;
}

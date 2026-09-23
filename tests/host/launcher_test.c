#include "lvgl.h"
#include "launcher.h"
#include "ui_font.h"
#include "icons/ui_icons.h"
#include "src/stdlib/builtin/lv_tlsf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static uint16_t pixels[684 * 1216];
static uint16_t draw_buffer[684 * 40];
static unsigned checks;

static union
{
    uint64_t alignment;
    uint8_t bytes[UI_TEST_TINY_TTF_CACHE_SIZE];
} font_bitmap_pool;
static lv_tlsf_t font_bitmap_heap;
static size_t font_bitmap_live, font_bitmap_peak, font_bitmap_blocks;

static void *font_bitmap_alloc(size_t size, lv_color_format_t color_format)
{
    CHECK(color_format == LV_COLOR_FORMAT_A8);
    void *buffer = lv_tlsf_malloc(font_bitmap_heap, size);
    CHECK(buffer);
    font_bitmap_live += lv_tlsf_block_size(buffer) + lv_tlsf_alloc_overhead();
    if (font_bitmap_live > font_bitmap_peak) font_bitmap_peak = font_bitmap_live;
    ++font_bitmap_blocks;
    return buffer;
}

static void font_bitmap_free(void *buffer)
{
    CHECK(buffer && font_bitmap_blocks > 0);
    uintptr_t address = (uintptr_t)buffer;
    CHECK(address >= (uintptr_t)font_bitmap_pool.bytes &&
          address < (uintptr_t)font_bitmap_pool.bytes + sizeof(font_bitmap_pool.bytes));
    size_t size = lv_tlsf_block_size(buffer) + lv_tlsf_alloc_overhead();
    CHECK(font_bitmap_live >= size);
    font_bitmap_live -= size;
    --font_bitmap_blocks;
    lv_tlsf_free(font_bitmap_heap, buffer);
}

static void font_bitmap_init(void)
{
    CHECK((uintptr_t)font_bitmap_pool.bytes % lv_tlsf_align_size() == 0);
    font_bitmap_heap = lv_tlsf_create_with_pool(font_bitmap_pool.bytes, sizeof(font_bitmap_pool.bytes));
    CHECK(font_bitmap_heap);
    lv_draw_buf_handlers_init(lv_draw_buf_get_font_handlers(),
                             font_bitmap_alloc, font_bitmap_free,
                             lv_draw_buf_copy, lv_draw_buf_align,
                             NULL, NULL, lv_draw_buf_width_to_stride);
}

static void test_font_cache_eviction(void)
{
    const lv_font_t *fonts[] = {ui_font_small(), ui_font_body(), ui_font_title()};
    for (uint32_t codepoint = 0x4e00; codepoint < 0x5200; ++codepoint)
    {
        for (unsigned i = 0; i < 3; ++i)
        {
            lv_font_glyph_dsc_t glyph = {0};
            CHECK(lv_font_get_glyph_dsc(fonts[i], &glyph, codepoint, 0));
            CHECK(glyph.resolved_font == fonts[i] && glyph.box_w && glyph.box_h);
            CHECK(lv_font_get_glyph_bitmap(&glyph, NULL));
            lv_font_glyph_release_draw_data(&glyph);
        }
        CHECK(font_bitmap_blocks <= 3 * LV_TINY_TTF_CACHE_GLYPH_CNT);
    }
    CHECK(lv_tlsf_check(font_bitmap_heap) == 0);
    CHECK(lv_tlsf_check_pool(lv_tlsf_get_pool(font_bitmap_heap)) == 0);
    printf("PASS: 1024 Chinese glyphs at 24/28/36 px; isolated bitmap heap peak=%zu/%zu bytes, %zu cached blocks\n",
           font_bitmap_peak, sizeof(font_bitmap_pool.bytes), font_bitmap_blocks);
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *data)
{
    uint16_t *src = (uint16_t *)data;
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x)
            pixels[y * 684 + x] = *src++;
    lv_display_flush_ready(display);
}

static void render(void)
{
    lv_obj_update_layout(lv_screen_active());
    lv_refr_now(NULL);
}

static void capture(const char *filename)
{
    render();
    FILE *file = fopen(filename, "wb");
    CHECK(file);
    fprintf(file, "P6\n684 1216\n255\n");
    for (unsigned i = 0; i < 684 * 1216; ++i)
    {
        unsigned p = pixels[i];
        unsigned char rgb[3] = {
            (unsigned char)(((p >> 11) & 31) * 255 / 31),
            (unsigned char)(((p >> 5) & 63) * 255 / 63),
            (unsigned char)((p & 31) * 255 / 31)
        };
        fwrite(rgb, 1, 3, file);
    }
    fclose(file);
}

static lv_obj_t *find_label(lv_obj_t *obj, const char *text)
{
    if (lv_obj_check_type(obj, &lv_label_class) && !strcmp(lv_label_get_text(obj), text)) return obj;
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); ++i)
    {
        lv_obj_t *found = find_label(lv_obj_get_child(obj, i), text);
        if (found) return found;
    }
    return NULL;
}

static bool visible_icon(lv_obj_t *obj, const lv_image_dsc_t *icon)
{
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return false;
    if (lv_obj_check_type(obj, &lv_image_class) && lv_image_get_src(obj) == icon) return true;
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); ++i)
        if (visible_icon(lv_obj_get_child(obj, i), icon)) return true;
    return false;
}

static void assert_bounds(lv_obj_t *obj)
{
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return;
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    CHECK(area.x1 >= 0 && area.y1 >= 0 && area.x2 < 684 && area.y2 < 1216);
    ++checks;
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); ++i)
        assert_bounds(lv_obj_get_child(obj, i));
}

static void click_text(const char *text)
{
    lv_obj_t *label = find_label(lv_screen_active(), text);
    CHECK(label);
    lv_obj_send_event(lv_obj_get_parent(label), LV_EVENT_CLICKED, NULL);
    launcher_process();
    render();
}

static void key(launcher_key_t key)
{
    launcher_key(key);
    launcher_process();
    render();
}

static void test_model(void)
{
    CHECK(ui_radio_state(false, false) == UI_RADIO_OFF);
    CHECK(ui_radio_state(false, true) == UI_RADIO_OFF);
    CHECK(ui_radio_state(true, false) == UI_RADIO_DISCONNECTED);
    CHECK(ui_radio_state(true, true) == UI_RADIO_CONNECTED);
    CHECK(ui_battery_percent(-1) == 0 && ui_battery_percent(101) == 100);
    for (int percent = -1; percent <= 101; ++percent)
    {
        ui_battery_icon_t expected = percent <= 20 ? UI_BATTERY_EMPTY :
                                    percent < 80 ? UI_BATTERY_MID : UI_BATTERY_FULL;
        CHECK(ui_battery_icon(percent, false) == expected);
        CHECK(ui_battery_icon(percent, true) == UI_BATTERY_CHARGING);
        checks += 2;
    }
    ui_nav_t nav;
    ui_nav_init(&nav);
    CHECK(!ui_nav_back(&nav));
    CHECK(!ui_nav_push(&nav, UI_PAGE_COUNT));
    CHECK(!ui_nav_push(&nav, UI_PAGE_HOME));
    for (int i = 1; i < UI_NAV_CAPACITY; ++i) CHECK(ui_nav_push(&nav, (ui_page_id_t)i));
    CHECK(!ui_nav_push(&nav, UI_PAGE_ABOUT));
    CHECK(ui_nav_push(&nav, UI_PAGE_HOME) && nav.depth == 1);
}

static void test_tiny_ttf(void)
{
    FILE *file = fopen(UI_TEST_FONT_FILE, "rb");
    CHECK(file && fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    CHECK(size > 0 && fseek(file, 0, SEEK_SET) == 0);
    unsigned char *data = malloc((size_t)size);
    CHECK(data && fread(data, 1, (size_t)size, file) == (size_t)size);
    fclose(file);

    CHECK(lv_tiny_ttf_create_file("A:missing-font.ttf", 24) == NULL);
    lv_font_t *memory_font = lv_tiny_ttf_create_data_ex(data, (size_t)size, 24,
                                LV_FONT_KERNING_NONE, LV_TINY_TTF_CACHE_GLYPH_CNT);
    lv_font_t *file_font = lv_tiny_ttf_create_file_ex(UI_BUILTIN_FONT_PATH, 24,
                                LV_FONT_KERNING_NONE, LV_TINY_TTF_CACHE_GLYPH_CNT);
    CHECK(memory_font && file_font);
    static const int sizes[] = {24, 28, 36};
    static const uint32_t letters[] = {0x4e2d, 0x6587, 'A', 'g'};
    for (unsigned s = 0; s < 3; ++s)
    {
        lv_tiny_ttf_set_size(memory_font, sizes[s]);
        lv_tiny_ttf_set_size(file_font, sizes[s]);
        CHECK(memory_font->line_height == file_font->line_height);
        for (unsigned i = 0; i < 4; ++i)
        {
            lv_font_glyph_dsc_t md = {0}, fd = {0};
            CHECK(lv_font_get_glyph_dsc(memory_font, &md, letters[i], 0));
            CHECK(lv_font_get_glyph_dsc(file_font, &fd, letters[i], 0));
            CHECK(md.resolved_font == memory_font && fd.resolved_font == file_font);
            CHECK(md.format == LV_FONT_GLYPH_FORMAT_A8 && fd.format == md.format);
            CHECK(md.box_w > 0 && md.box_h > 0 && md.box_w == fd.box_w && md.box_h == fd.box_h);
            const lv_draw_buf_t *mb = lv_font_get_glyph_bitmap(&md, NULL);
            const lv_draw_buf_t *fb = lv_font_get_glyph_bitmap(&fd, NULL);
            CHECK(mb && fb);
            bool antialiased = false;
            for (unsigned y = 0; y < md.box_h; ++y)
            {
                const uint8_t *mr = mb->data + y * mb->header.stride;
                const uint8_t *fr = fb->data + y * fb->header.stride;
                CHECK(memcmp(mr, fr, md.box_w) == 0);
                for (unsigned x = 0; x < md.box_w; ++x)
                    if (mr[x] > 0 && mr[x] < 255) antialiased = true;
            }
            CHECK(antialiased);
            lv_font_glyph_release_draw_data(&md);
            lv_font_glyph_release_draw_data(&fd);
        }
    }
    lv_tiny_ttf_destroy(memory_font);
    lv_tiny_ttf_destroy(file_font);
    free(data);
    CHECK(font_bitmap_blocks == 0 && font_bitmap_live == 0);
    CHECK(lv_mem_test() == LV_RESULT_OK);
    puts("PASS: Tiny TTF memory/file glyphs match; Chinese/English antialiasing at 24/28/36 px; missing file handled");
}

int main(void)
{
    test_model();
    lv_init();
    font_bitmap_init();
    lv_display_t *display = lv_display_create(684, 1216);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    test_tiny_ttf();
    CHECK(launcher_init());
    test_font_cache_eviction();
    CHECK(lv_font_get_glyph_width(ui_font_title(), ' ', 'L') > 0);
    launcher_status_t status = {0};
    strcpy(status.time_text, "2026-09-22 17:10");
    status.battery_percent = -1;
    launcher_set_status(&status);
    render();
    CHECK(!visible_icon(lv_screen_active(), &ui_icon_wifi_on));
    CHECK(!visible_icon(lv_screen_active(), &ui_icon_wifi_disconnected));
    CHECK(!visible_icon(lv_screen_active(), &ui_icon_bt_on));
    CHECK(!visible_icon(lv_screen_active(), &ui_icon_bt_disconnected));
    CHECK(visible_icon(lv_screen_active(), &ui_icon_battery_empty));
    CHECK(find_label(lv_screen_active(), "0%"));
    assert_bounds(lv_screen_active());
    capture("home-off.ppm");
    status.bluetooth = UI_RADIO_DISCONNECTED;
    status.wifi = UI_RADIO_CONNECTED;
    status.battery_percent = 72;
    strcpy(status.recent_book, "The Immortal.txt");
    status.reading_percent = 34;
    strcpy(status.weather, "多云 26℃");
    launcher_set_status(&status);
    render();
    CHECK(visible_icon(lv_screen_active(), &ui_icon_bt_disconnected));
    CHECK(visible_icon(lv_screen_active(), &ui_icon_wifi_on));
    CHECK(visible_icon(lv_screen_active(), &ui_icon_battery_mid));
    assert_bounds(lv_screen_active());
    capture("home-connected.ppm");
    status.bluetooth = UI_RADIO_CONNECTED;
    status.wifi = UI_RADIO_DISCONNECTED;
    status.battery_percent = 100;
    launcher_set_status(&status);
    render();
    CHECK(visible_icon(lv_screen_active(), &ui_icon_bt_on));
    CHECK(visible_icon(lv_screen_active(), &ui_icon_wifi_disconnected));
    CHECK(visible_icon(lv_screen_active(), &ui_icon_battery_full));
    capture("home-full.ppm");
    status.charging = true;
    launcher_set_status(&status);
    render();
    CHECK(visible_icon(lv_screen_active(), &ui_icon_battery_charge));
    static const ui_page_id_t pages[] = {
        UI_PAGE_BOOKSHELF, UI_PAGE_WEATHER, UI_PAGE_TRANSFER,
        UI_PAGE_ALBUM, UI_PAGE_WIFI, UI_PAGE_SETTINGS, UI_PAGE_LOCK
    };
    for (unsigned i = 0; i < 7; ++i)
    {
        CHECK(launcher_focus_index() == i);
        key(LAUNCHER_KEY_ENTER);
        CHECK(launcher_current_page() == pages[i]);
        assert_bounds(lv_screen_active());
        if (pages[i] == UI_PAGE_LOCK)
        {
            capture("lock.ppm");
            key(LAUNCHER_KEY_PREVIOUS);
            CHECK(launcher_current_page() == UI_PAGE_LOCK);
            key(LAUNCHER_KEY_ENTER);
        }
        else key(LAUNCHER_KEY_BACK);
        CHECK(launcher_current_page() == UI_PAGE_HOME);
        CHECK(launcher_focus_index() == i);
        key(LAUNCHER_KEY_NEXT);
    }
    CHECK(launcher_focus_index() == 0);
    click_text("设置");
    click_text("文字设置");
    CHECK(launcher_current_page() == UI_PAGE_TEXT_SETTINGS);
    click_text("返回");
    CHECK(launcher_current_page() == UI_PAGE_SETTINGS);
    CHECK(launcher_focus_index() == 0);
    click_text("关于设备");
    CHECK(launcher_current_page() == UI_PAGE_ABOUT);
    click_text("返回");
    CHECK(launcher_focus_index() == 1);
    click_text("返回");
    click_text("书架");
    click_text("阅读页面");
    CHECK(launcher_current_page() == UI_PAGE_READER);
    click_text("返回");
    click_text("返回");
    click_text("天气");
    click_text("城市选择");
    CHECK(launcher_current_page() == UI_PAGE_CITY);
    click_text("返回");
    click_text("返回");
    click_text("Wi-Fi 配网");
    CHECK(find_label(lv_screen_active(), "配网功能暂未开放"));
    capture("wifi-placeholder.ppm");
    click_text("返回");
    lv_mem_monitor_t before, after;
    for (unsigned i = 0; i < 10; ++i)
    {
        click_text("Wi-Fi 配网");
        click_text("返回");
    }
    lv_mem_monitor(&before);
    size_t bitmap_blocks_before = font_bitmap_blocks;
    for (unsigned i = 0; i < 200; ++i)
    {
        click_text("Wi-Fi 配网");
        click_text("返回");
    }
    lv_mem_monitor(&after);
    printf("Heap before=%zu/%zu blocks after=%zu/%zu blocks\n",
           before.free_size, before.used_cnt, after.free_size, after.used_cnt);
    CHECK(after.used_cnt == before.used_cnt);
    /* TLSF block splitting can change alignment/slack without retaining objects. */
    CHECK(after.free_size + 512 >= before.free_size);
    CHECK(lv_mem_test() == LV_RESULT_OK);
    CHECK(font_bitmap_blocks == bitmap_blocks_before);
    CHECK(lv_tlsf_check(font_bitmap_heap) == 0);
    CHECK(lv_tlsf_check_pool(lv_tlsf_get_pool(font_bitmap_heap)) == 0);
    printf("Bitmap heap final=%zu bytes, peak=%zu/%zu bytes, cached blocks=%zu\n",
           font_bitmap_live, font_bitmap_peak, sizeof(font_bitmap_pool.bytes), font_bitmap_blocks);
    printf("PASS: %u state/layout checks, all page entries, touch/key navigation, focus restore, 200 round trips; heap stable (%zu bytes free)\n",
           checks, after.free_size);
    return 0;
}

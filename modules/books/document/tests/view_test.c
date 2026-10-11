/* Standalone host integration tests for the document UI scheduler and ownership. */
#define VIEW_INTEGRATION_TEST
#define main layout_test_main
#include "layout_test.c"
#undef main

static ui_bookshelf_book_t book;
static unsigned settings[UI_SETTING_COUNT], saved_positions, readers_live, readers_created;
static unsigned widgets_created, widgets_deleted, image_requests, image_cancels, image_takes;
static unsigned index_loads, index_saves, sync_image_loads;
static bool index_enabled, image_allocation_failure;
static void *saved_index;
static size_t saved_index_size;
static int request_failure;
static lv_obj_t body;

typedef struct mock_object {
    lv_obj_t base;
    struct mock_object *next;
    lv_obj_t *parent;
    unsigned flags, callbacks;
    int kind, decor;
    void (*clicked)(lv_event_t *);
    void (*deleted)(lv_event_t *);
    void *click_data, *delete_data;
    const char *text;
    const lv_image_dsc_t *image;
} mock_object_t;
typedef struct { lv_event_t base; lv_obj_t *target; void *data; } mock_event_t;
static mock_object_t *objects;

struct document_image_request {
    struct document_image_request *next;
    unsigned record, width, height;
    int result;
    bool transferred;
};
static document_image_request_t *requests;

const ui_bookshelf_book_t *ui_bookshelf_book(unsigned index) { return index ? NULL : &book; }
bool ui_bookshelf_save(unsigned index, const ui_reading_position_t *position)
{ assert(!index); book.position = *position; ++saved_positions; return true; }
bool ui_bookshelf_flush(void) { return true; }
unsigned ui_settings_index(ui_setting_id_t index) { assert(index < UI_SETTING_COUNT); return settings[index]; }
void epd_app_set_recent_reading(const char *title, int percent) { (void)title; (void)percent; }
void epd_app_object_init(lv_obj_t *object) { assert(object); }
bool epd_app_refresh_done(void) { return true; }
lv_font_t *epd_app_font_create(unsigned family, unsigned size, unsigned weight, uint32_t *identity, lv_font_t **fallback)
{
    (void)family; (void)weight; *identity = 123; *fallback = NULL;
    lv_font_t *font = epd_app_alloc(sizeof(*font), EPD_APP_PSRAM);
    if (font) { memset(font, 0, sizeof(*font)); font->line_height = size; }
    return font;
}
void epd_app_font_destroy(lv_font_t *font) { epd_app_free(font); }
const lv_font_t *epd_app_font_role(unsigned role) { (void)role; return &regular; }
lv_font_t *document_font_oblique(const lv_font_t *font)
{
    lv_font_t *out = epd_app_alloc(sizeof(*out), EPD_APP_PSRAM);
    if (out) *out = *font;
    return out;
}
void document_font_free(lv_font_t *font) { epd_app_free(font); }
document_t *document_open(const bookshelf_item_t *file, char *error, size_t size)
{ (void)file; (void)error; (void)size; return (document_t *)1; }
void document_close(document_t *document) { (void)document; assert(!readers_live && !requests); }
void document_service_process(void) {}
document_reader_t *document_reader_create(document_t *document)
{ assert(document); ++readers_live; return (document_reader_t *)(uintptr_t)++readers_created; }
void document_reader_destroy(document_reader_t *reader) { if (reader) { assert(readers_live); --readers_live; } }
bool document_index_load(document_t *document, uint32_t layout, void **data, size_t *size)
{
    (void)document; (void)layout; ++index_loads;
    if (!index_enabled || !saved_index) return false;
    *data = epd_app_alloc(saved_index_size, EPD_APP_PSRAM);
    if (!*data) return false;
    memcpy(*data, saved_index, saved_index_size); *size = saved_index_size; return true;
}
bool document_index_save(document_t *document, uint32_t layout, const void *data, size_t size)
{
    (void)document; (void)layout; ++index_saves;
    if (!index_enabled) return false;
    free(saved_index); saved_index = malloc(size); assert(saved_index);
    memcpy(saved_index, data, size); saved_index_size = size; return true;
}
bool document_image_load(document_t *document, uint32_t offset, unsigned width, unsigned height, document_bitmap_t *bitmap)
{ (void)document; (void)offset; (void)width; (void)height; (void)bitmap; ++sync_image_loads; return false; }
document_image_request_t *document_image_request(document_t *document, uint32_t offset, unsigned width, unsigned height)
{
    assert(document); ++image_requests;
    if (request_failure) return NULL;
    document_image_request_t *request = calloc(1, sizeof(*request)); assert(request);
    request->record = offset; request->width = width; request->height = height;
    document_image_request_t **at = &requests; while (*at) at = &(*at)->next; *at = request;
    return request;
}
int document_image_take(document_image_request_t *request, document_bitmap_t *bitmap)
{
    assert(!request->transferred); ++image_takes;
    if (request->result != 1) return request->result;
    if (image_allocation_failure) return -1;
    bitmap->memory = epd_app_alloc(16, EPD_APP_PSRAM);
    if (!bitmap->memory) return -1;
    memset(bitmap->memory, 0x5a, 16);
    bitmap->image.data = bitmap->memory; bitmap->image.data_size = 16;
    bitmap->image.header.w = request->width; bitmap->image.header.h = request->height;
    request->transferred = true; return 1;
}
void document_image_cancel(document_image_request_t *request)
{
    document_image_request_t **at = &requests;
    while (*at && *at != request) at = &(*at)->next;
    assert(*at); *at = request->next; ++image_cancels; free(request);
}
static void complete_images(int result)
{ for (document_image_request_t *request = requests; request; request = request->next) request->result = result; }
static lv_obj_t *object_create(lv_obj_t *parent, int kind)
{
    mock_object_t *object = calloc(1, sizeof(*object)); assert(object);
    object->next = objects; objects = object; object->parent = parent; object->kind = kind;
    ++widgets_created; return &object->base;
}
lv_obj_t *lv_obj_create(lv_obj_t *parent) { return object_create(parent, 0); }
lv_obj_t *lv_label_create(lv_obj_t *parent) { return object_create(parent, 1); }
lv_obj_t *lv_image_create(lv_obj_t *parent) { return object_create(parent, 2); }
lv_obj_t *lv_event_get_target_obj(lv_event_t *event) { return ((mock_event_t *)event)->target; }
void *lv_event_get_user_data(lv_event_t *event) { return ((mock_event_t *)event)->data; }
void lv_obj_delete(lv_obj_t *object)
{
    mock_object_t *mock = (mock_object_t *)object;
    if (mock->deleted) { mock_event_t event = {.target = object, .data = mock->delete_data}; mock->deleted(&event.base); }
    bool child;
    do {
        child = false;
        for (mock_object_t *p = objects; p; p = p->next)
            if (p->parent == object) { lv_obj_delete(&p->base); child = true; break; }
    } while (child);
    mock_object_t **at = &objects; while (*at && *at != mock) at = &(*at)->next;
    assert(*at); *at = mock->next; free(mock); ++widgets_deleted;
}
void lv_obj_add_event_cb(lv_obj_t *object, void (*callback)(lv_event_t *), int event, void *data)
{
    mock_object_t *mock = (mock_object_t *)object; ++mock->callbacks; assert(mock->callbacks == 1);
    if (event == LV_EVENT_CLICKED) { mock->clicked = callback; mock->click_data = data; }
    else { assert(event == LV_EVENT_DELETE); mock->deleted = callback; mock->delete_data = data; }
}
void lv_obj_add_flag(lv_obj_t *object, unsigned flags) { ((mock_object_t *)object)->flags |= flags; }
void lv_obj_remove_flag(lv_obj_t *object, unsigned flags) { ((mock_object_t *)object)->flags &= ~flags; }
void lv_label_set_text_static(lv_obj_t *object, const char *text) { ((mock_object_t *)object)->text = text; }
void lv_label_set_text(lv_obj_t *object, const char *text) { lv_label_set_text_static(object, text); }
void lv_image_set_src(lv_obj_t *object, const void *image) { ((mock_object_t *)object)->image = image; }
void lv_obj_set_style_text_decor(lv_obj_t *object, int decor, int selector)
{ (void)selector; ((mock_object_t *)object)->decor = decor; }
#define MOCK_STYLE(name) void name(lv_obj_t *o, int a, int b) { (void)o; (void)a; (void)b; }
MOCK_STYLE(lv_obj_set_size) MOCK_STYLE(lv_obj_set_pos)
MOCK_STYLE(lv_obj_set_style_pad_all) MOCK_STYLE(lv_obj_set_style_border_width)
MOCK_STYLE(lv_obj_set_style_bg_opa) MOCK_STYLE(lv_obj_set_style_bg_color)
MOCK_STYLE(lv_obj_set_style_text_color) MOCK_STYLE(lv_obj_set_style_text_line_space)
MOCK_STYLE(lv_obj_set_style_text_letter_space)
void lv_obj_set_style_text_font(lv_obj_t *object, const lv_font_t *font, int selector)
{ (void)object; (void)selector; assert(font); }
void lv_label_set_long_mode(lv_obj_t *object, int mode) { (void)object; (void)mode; }
int lv_color_white(void) { return 1; }
int lv_color_black(void) { return 0; }

static void reset_integration(void)
{
    document_view_close();
    assert(!requests && !objects && !readers_live);
    memset(&book, 0, sizeof(book)); strcpy(book.file.name, "Host document"); book.file.format = BOOK_FORMAT_MARKDOWN;
    memset(settings, 0, sizeof(settings)); settings[UI_SETTING_FONT_SIZE] = 4;
    spool_size = 0; stream_complete = false; readers_created = 0;
    saved_positions = widgets_created = widgets_deleted = image_requests = image_cancels = image_takes = 0;
    index_loads = index_saves = sync_image_loads = 0;
    image_info_calls = image_info_invalidations = 0;
    glyph_queries = progress_queries = read_queries = status_queries = 0;
    memset(reader_reads, 0, sizeof(reader_reads));
    request_failure = 0; image_allocation_failure = false; allocation_fail_after = -1;
    index_enabled = false; free(saved_index); saved_index = NULL; saved_index_size = 0;
}
static void emit_text_pages(void)
{
    char text[1000]; memset(text, 'z', sizeof(text));
    for (unsigned i = 0; i < 16; ++i) emit(DOCUMENT_TEXT, i & 1 ? BOOK_STYLE_BOLD : 0, text, sizeof(text));
    stream_complete = true;
}
static void drain(bool foreground, bool commit)
{
    for (unsigned i = 0; i < 30000; ++i)
    {
        bool runnable = document_view_step(8, foreground, commit);
        assert(!view.failure[0]);
        if (!runnable) return;
    }
    assert(!"scheduler failed to settle");
}
static void test_compute_commit_and_reuse(void)
{
    reset_integration(); emit_text_pages(); assert(document_view_open(0));
    uint32_t revision = view.view.revision;
    while (!view.display_pending) assert(document_view_step(1, true, false));
    assert(!reader_reads[2]); /* The target reuses the index's finished page. */
    drain(true, false);
    assert(view.complete && view.count > 3 && view.view.revision == revision && !view.view.ready && !saved_positions);
    assert(!document_view_bind(&body));
    unsigned snapshots = progress_queries;
    assert(!document_view_step(0, true, true) && progress_queries == snapshots + 1);
    assert(view.view.revision == revision + 1 && document_view_bind(&body) && view.view.page == 1);
    lv_obj_t *root = view.root;
    unsigned created = widgets_created, queries = glyph_queries;
    document_view_seek(2); assert(view.display_pending);
    document_view_step(0, true, true);
    assert(document_view_bind(&body) && view.root == root && view.view.page == 2);
    assert(glyph_queries == queries && widgets_created <= created + 2 && !widgets_deleted);
    queries = glyph_queries;
    document_view_seek(1); assert(view.display_pending); document_view_step(0, true, true);
    assert(document_view_bind(&body) && view.root == root && glyph_queries == queries);
    for (view_widget_t *w = view.widgets; w; w = w->next)
        assert(((mock_object_t *)w->object)->callbacks == 1);
    revision = view.view.revision; unsigned saves = saved_positions;
    drain(false, true);
    assert(view.view.revision == revision && saved_positions == saves);
    document_view_close(); assert(widgets_deleted == widgets_created);
}

static void test_target_priority(void)
{
    reset_integration(); assert(document_view_open(0));
    assert(document_view_step(8, true, true) == UI_READER_WORK_IDLE); /* Awaiting source data. */
    emit_text_pages();
    assert(document_view_step(0, true, false) == UI_READER_WORK_TARGET);
    while (!view.display_pending) document_view_step(1, true, false);
    unsigned metrics = glyph_queries, reads = read_queries;
    assert(document_view_step(8, true, true) == UI_READER_WORK_BACKGROUND);
    assert(glyph_queries == metrics && read_queries == reads && !view.complete);
    assert(document_view_bind(&body));
    assert(document_view_step(0, true, true) == UI_READER_WORK_BACKGROUND);
    document_view_seek(8);
    assert(document_view_step(0, true, false) == UI_READER_WORK_TARGET);
    document_view_cancel_pending();
    assert(document_view_step(0, true, true) == UI_READER_WORK_BACKGROUND);
    drain(true, true);
    assert(document_view_step(8, true, true) == UI_READER_WORK_IDLE);

    reset_integration(); emit_image(620, 1040); emit_text_pages();
    assert(document_view_open(0));
    while (!view.display_pending) document_view_step(1, true, false);
    assert(requests && !images_ready(view.building));
    /* The target waits for the image worker, even with runnable index work. */
    assert(document_view_step(0, true, true) == UI_READER_WORK_BACKGROUND);
    assert(!view.notified && !view.complete && !target_runnable());
    document_view_close();
}
static void test_stream_wait_and_background(void)
{
    reset_integration(); assert(document_view_open(0));
    uint32_t revision = view.view.revision;
    assert(!document_view_step(8, true, false));
    emit(DOCUMENT_TEXT, 0, "body", 4); drain(false, true);
    assert(!view.complete && !view.view.ready && !saved_positions && view.view.revision == revision);
    stream_complete = true; drain(false, true);
    assert(view.view.pages == 1 && !view.view.indexing && !view.view.ready && !saved_positions && view.view.revision == revision);
    document_view_step(0, true, true); assert(document_view_bind(&body));
    assert(view.view.ready && saved_positions == 1);
}
static void test_index_warm_open_and_corruption(void)
{
    reset_integration(); index_enabled = true;
    const char toc[] = "chapter\0Chapter one\0";
    emit(DOCUMENT_ANCHOR, 2, toc, sizeof(toc)); emit_text_pages();
    assert(document_view_open(0)); drain(true, false);
    assert(index_saves == 1 && saved_index && view.toc_count == 1);
    unsigned pages = view.count;
    document_cursor_t anchor = view.pages[pages - 2];
    document_view_close(); readers_created = 0;
    book.position.document_version = DOCUMENT_CONVERSION_VERSION;
    book.position.document_kind = book.file.format;
    book.position.offset = anchor.record; book.position.document_byte = anchor.byte;
    assert(document_view_open(0)); unsigned reads = read_queries, metrics = glyph_queries;
    document_view_step(0, true, false);
    assert(view.complete && view.from_cache && view.count == pages && view.toc_count == 1);
    assert(read_queries == reads && glyph_queries == metrics);
    drain(true, false); assert(cursor_equal(view.building->start, anchor) && index_saves == 1);
    document_view_close(); readers_created = 0;
    ((view_index_header_t *)saved_index)->checksum ^= 1;
    assert(document_view_open(0)); document_view_step(0, true, false);
    assert(!view.complete && !view.from_cache); drain(true, false); assert(index_saves == 2);
    document_view_close(); readers_created = 0;
    view_index_header_t *header = saved_index;
    document_cursor_t *positions = (document_cursor_t *)(header + 1);
    positions[1] = positions[0]; header->checksum = index_checksum(saved_index, saved_index_size);
    assert(document_view_open(0)); document_view_step(0, true, false);
    assert(!view.complete); drain(true, false);
    document_view_close(); readers_created = 0;
    header = saved_index; positions = (document_cursor_t *)(header + 1);
    /* Structurally valid blob, but the selected record is only 1000 bytes. */
    positions[1].byte = DOCUMENT_TEXT_LIMIT;
    header->checksum = index_checksum(saved_index, saved_index_size);
    memset(&book.position, 0, sizeof(book.position));
    assert(document_view_open(0)); document_view_step(0, true, false);
    assert(view.from_cache);
    document_view_seek(2); drain(true, false);
    assert(!view.from_cache && view.complete && !view.failure[0]);
}
static void test_async_images(void)
{
    reset_integration();
    emit(DOCUMENT_TEXT, 0, "Before illustration\n\n\n\n\n", 24);
    unsigned first = emit_image(620, 1040);
    emit_image(620, 1040);
    emit_image(620, 1040); stream_complete = true;
    assert(document_view_open(0)); drain(true, false);
    document_view_step(0, true, true); assert(document_view_bind(&body));
    lv_obj_t *root = view.root; unsigned saves = saved_positions;
    document_view_seek(2); drain(true, false);
    assert(requests && requests->record == first && view.view.page == 1 && saved_positions == saves);
    unsigned count = image_requests;
    document_view_seek(2); /* Repeating the same target must not restart its job. */
    assert(image_requests == count);
    for (unsigned i = 0; i < 5; ++i) assert(!document_view_step(8, true, false));
    assert(image_requests == count && !view.notified && !document_view_bind(&body));
    complete_images(1); drain(true, false);
    assert(view.view.page == 1 && saved_positions == saves);
    document_view_step(0, true, true); unsigned reads = read_queries;
    assert(document_view_bind(&body) && view.root == root && view.view.page == 2);
    assert(!sync_image_loads && read_queries == reads);
    for (view_widget_t *w = view.widgets; w; w = w->next)
        if (w->used && w->image) assert(((mock_object_t *)w->object)->image->data[0] == 0x5a);
    document_view_seek(4); unsigned cancels = image_cancels;
    drain(true, false); assert(requests && image_cancels >= cancels);
    assert(view.view.page == 2);
    complete_images(-1);
    document_view_step(8, true, false); /* One required retry, after optional eviction. */
    complete_images(-1);
    document_view_step(8, true, false);
    assert(view.failure[0] && !document_view_waiting() && view.view.page == 2);
    unsigned requests_at_failure = image_requests;
    for (unsigned i = 0; i < 4; ++i) assert(!document_view_step(8, true, false));
    assert(image_requests == requests_at_failure);
    document_view_step(0, true, true); assert(view.view.error[0]);
    document_view_close(); assert(!requests);
}
static void test_allocation_failure_and_reflow(void)
{
    reset_integration(); emit_text_pages(); assert(document_view_open(0));
    allocation_fail_after = 0;
    /* Neighbour allocation and persistent snapshots are optional. */
    drain(true, false); assert(view.complete && !view.failure[0]);
    allocation_fail_after = -1;
    document_view_step(0, true, true); assert(document_view_bind(&body));
    unsigned old_layout = view.layout;
    settings[UI_SETTING_FONT_SIZE] = 6;
    assert(document_view_reflow() && view.layout != old_layout);
    uint32_t revision = view.view.revision; unsigned saves = saved_positions;
    drain(true, false);
    assert(view.view.revision == revision && saved_positions == saves && view.display_layout == old_layout);
    document_view_step(0, true, true); assert(document_view_bind(&body) && view.display_layout == view.layout);
}
static void test_target_and_index_publication_wait(void)
{
    reset_integration(); assert(document_view_open(0));
    emit(DOCUMENT_TEXT, 0, "waiting", 7);
    document_progress(NULL, &view.progress);
    view.restoring = false; view.wanted = 0; view.count = 1;
    view.pages[1] = (document_cursor_t){spool_size, 0};
    /* Both consumers have exhausted the same immutable publication. */
    while (view.building->cursor.record < spool_size)
        assert(page_step(view.building, false, view.page_reader, &view.progress) == 1);
    page_reset(view.index, view.pages[1]);
    view.neighbours[0].number = view.neighbours[1].number = UINT_MAX;
    assert(target_needed() && !view.display_pending && !page_runnable(view.building));
    assert(!compute_runnable(true));
    assert(!document_view_step(8, true, false));
    emit(DOCUMENT_TEXT, 0, " published", 10);
    assert(document_view_step(1, true, false));
}
static void test_cancel_pending_preserves_reflow_cursor(bool prepared)
{
    reset_integration(); emit_text_pages(); assert(document_view_open(0));
    drain(true, false); document_view_step(0, true, true); assert(document_view_bind(&body));
    document_view_seek(4); drain(true, false);
    document_view_step(0, true, true); assert(document_view_bind(&body) && view.view.page == 4);
    document_cursor_t anchor = view.display->start;
    unsigned old_page = view.view.page;
    uint32_t old_layout = view.display_layout;
    assert(anchor.record && anchor.byte);

    document_view_cancel_pending();
    settings[UI_SETTING_FONT_SIZE] = 7;
    assert(document_view_reflow() && view.layout != old_layout);
    if (prepared)
    {
        drain(true, false);
        assert(!view.restoring && view.display_pending && view.wanted + 1 != old_page);
    }
    else
    {
        document_view_step(1, true, false);
        assert(view.restoring && !view.complete && !view.display_pending);
    }
    uint32_t revision = view.view.revision;
    unsigned saves = saved_positions;
    document_cursor_t index_cursor = view.index->cursor;
    document_view_cancel_pending();
    assert(cursor_equal(view.anchor, anchor) && view.restoring);
    assert(cursor_equal(view.index->cursor, index_cursor));
    assert(view.view.revision == revision && saved_positions == saves);
    assert(view.view.page == old_page && view.display_layout == old_layout);
    assert(!document_view_settings_changed());

    /* Resume without applying another settings change or starting another reflow. */
    drain(true, false);
    assert(view.display_pending && view.wanted + 1 != old_page);
    assert(!cursor_before(anchor, view.building->start) && cursor_before(anchor, view.building->cursor));
    assert(view.view.revision == revision && saved_positions == saves);
    document_view_step(0, true, true); assert(document_view_bind(&body));
    assert(view.display_layout == view.layout && view.view.page != old_page);
    assert(!cursor_before(anchor, view.display->start) && cursor_before(anchor, view.display->cursor));
    assert(book.position.offset == view.display->start.record && book.position.document_byte == view.display->start.byte);

    /* Cancellation within one layout still returns to the actual visible page. */
    unsigned visible_page = view.view.page;
    document_cursor_t visible_cursor = view.display->start;
    document_view_seek(visible_page + 1);
    document_view_cancel_pending();
    assert(!view.restoring && view.wanted + 1 == visible_page && !view.display_pending);
    assert(cursor_equal(view.display->start, visible_cursor) && !document_view_waiting());
}
static void test_image_request_failure_and_widget_failure(void)
{
    reset_integration();
    emit_image(620, 1040);
    stream_complete = true; request_failure = 1; assert(document_view_open(0));
    uint32_t revision = view.view.revision;
    for (unsigned i = 0; i < 100 && !view.failure[0]; ++i) document_view_step(8, true, false);
    assert(view.failure[0] && image_requests == 2 && !requests && !saved_positions);
    assert(view.view.revision == revision && !document_view_step(8, true, false));
    document_view_close();
    reset_integration(); emit_text_pages(); assert(document_view_open(0)); drain(true, false);
    document_view_step(0, true, true); allocation_fail_after = 0;
    assert(!document_view_bind(&body) && view.failure[0] && !view.view.ready && !saved_positions);
    allocation_fail_after = -1;
}
static void test_link_widget_rebinding(void)
{
    reset_integration(); emit_text_pages(); assert(document_view_open(0)); drain(true, false);
    view.building->runs[0].link = 35;
    document_view_step(0, true, true); assert(document_view_bind(&body));
    unsigned linked = 0;
    for (view_widget_t *w = view.widgets; w; w = w->next)
        if (w->used && w->link)
        {
            mock_object_t *mock = (mock_object_t *)w->object;
            assert(mock->click_data == w && mock->callbacks == 1 && mock->decor == LV_TEXT_DECOR_UNDERLINE);
            ++linked;
        }
    assert(linked == 1);
    document_view_seek(2); document_view_step(0, true, true); assert(document_view_bind(&body));
    for (view_widget_t *w = view.widgets; w; w = w->next)
    {
        mock_object_t *mock = (mock_object_t *)w->object;
        assert(!w->link && !(mock->flags & LV_OBJ_FLAG_CLICKABLE) && mock->callbacks == 1);
        if (w->used && !w->image) assert(mock->decor == LV_TEXT_DECOR_NONE);
    }
}
static void test_invalid_image_delegates_cache_invalidation(void)
{
    for (unsigned scenario = 0; scenario < INVALID_IMAGE_CASES; ++scenario)
    {
        reset_integration(); emit_invalid_image(scenario); stream_complete = true;
        assert(document_view_open(0));
        uint32_t revision = view.view.revision;
        assert(!document_view_step(8, true, false));
        assert(view.failure[0] && image_info_calls == 1 && image_info_invalidations == 1);
        assert(!image_requests && !sync_image_loads && !index_saves && !saved_positions);
        assert(!view.view.ready && !view.view.error[0] && view.view.revision == revision);
        assert(!document_view_step(8, true, true));
        assert(view.view.error[0] && !document_view_waiting() && !view.view.indexing);
        assert(image_info_calls == 1 && image_info_invalidations == 1 && !saved_positions);
    }
}
static reader_position_t shared_saved;
static unsigned shared_saves;
static bool shared_save(void *user, const reader_position_t *position)
{
    assert(user == &shared_saved);
    shared_saved = *position; ++shared_saves; return true;
}
static void test_shared_callbacks_and_cursor(void)
{
    reset_integration(); emit_text_pages();
    shared_saves = 0;
    reader_position_t restore = {.conversion_version = DOCUMENT_CONVERSION_VERSION};
    assert(epd_reader_attach((document_t *)1, "Cached chapter", &restore, shared_save, NULL, &shared_saved));
    drain(true, false); assert(view.count > 4 && !shared_saves && !saved_positions);
    document_cursor_t anchor = view.pages[3];
    epd_reader_seek_cursor(anchor); drain(true, false);
    epd_reader_step(0, true, true); assert(epd_reader_bind(&body));
    assert(shared_saves == 1 && shared_saved.page == 4 && !saved_positions);
    assert(cursor_equal(shared_saved.cursor, anchor));
    assert(shared_saved.conversion_version == DOCUMENT_CONVERSION_VERSION);
    unsigned before = shared_saves;
    assert(epd_reader_save() && shared_saves == before); /* Identical saves are coalesced. */
    restore = shared_saved;
    epd_reader_close(); readers_created = 0;
    assert(epd_reader_attach((document_t *)1, "Cached chapter", &restore, shared_save, NULL, &shared_saved));
    drain(true, false); epd_reader_step(0, true, true); assert(epd_reader_bind(&body));
    assert(cursor_equal(view.display->start, anchor) && epd_reader_get()->page == 4);
    settings[UI_SETTING_FONT_SIZE] = 5;
    assert(epd_reader_reflow()); drain(true, false); epd_reader_step(0, true, true); assert(epd_reader_bind(&body));
    assert(!cursor_before(anchor, view.display->start) &&
           (view.display->eof || cursor_before(anchor, view.display->cursor)));
    epd_reader_close();
    assert(!objects && !readers_live && !requests && !saved_positions);
    puts("PASS shared reader: save callback, seek_cursor, restore and reflow preserve content anchor without bookshelf history");
}
int main(void)
{
    test_compute_commit_and_reuse();
    test_target_priority();
    test_stream_wait_and_background();
    test_index_warm_open_and_corruption();
    test_async_images();
    test_allocation_failure_and_reflow();
    test_cancel_pending_preserves_reflow_cursor(false);
    test_cancel_pending_preserves_reflow_cursor(true);
    test_target_and_index_publication_wait();
    test_image_request_failure_and_widget_failure();
    test_link_widget_rebinding();
    test_invalid_image_delegates_cache_invalidation();
    test_shared_callbacks_and_cursor();
    document_view_close(); free(saved_index);
    puts("PASS: target/background/I-O priority, immediate target commit, shared budget, compute/commit, background totals, warm index validation, page/widget reuse, image ownership/cancel/failure, reflow, pause/resume cursor preservation, invalid-image delegation");
    return 0;
}

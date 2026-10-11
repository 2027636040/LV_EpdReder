/* Compile selected production functions unchanged; mock only their dependencies. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ui_reader.h"
#include "ui_settings.h"
#include "pages_types.h"

static void reader_resume(void), reader_resume_prepare(void), reader_create(lv_obj_t *screen);
static void reader_status_render(void), reader_footer_render(void), reader_render(void), reader_update(void);
static void reader_option_render(void), reader_save_position(void);
static void reader_panel_open(void), reader_panel_close(void), reader_toc_open(void);
static void bookshelf_resume(void);

static books_page_t page;
static books_page_t *current;
static int pending_page;
static ui_reader_view_t reading;
static lv_obj_t body, title, footer, progress, battery, clock_object, panel, option, jump;
static const lv_font_t old_font = {20}, new_font = {24}, default_font = {18};
static bool idle, document, settings_changed, more_work, compute_ready, target_pending, notified;
static bool reflow_failure, failure_pending;
static unsigned wanted, total_ready, actual_position, steps, computed, writes, reflows, turns;
static unsigned closes, saves, cancels, detaches, bookshelf_updates, popups, status_calls;
static unsigned step_budget, reflow_at_step;
static bool step_foreground, step_commit;

#define launcher_current_page() (current ? current->id : UI_PAGE_COUNT)
static bool lv_refreshing_done(void) { return idle; }
static bool document_view_active(void) { return document; }
static const lv_font_t *ui_font_body(void) { return &default_font; }
static void changed(lv_obj_t *object) { assert(idle); assert(object); ++writes; ++object->writes; }
static void text_update(lv_obj_t *object, const char *text) { assert(text); changed(object); }
static void lv_obj_set_style_text_font(lv_obj_t *object, const lv_font_t *font, int part)
{ assert(font); (void)part; changed(object); }
static void lv_obj_set_x(lv_obj_t *object, int value) { (void)value; changed(object); }
static void lv_obj_set_width(lv_obj_t *object, int value) { changed(object); object->width = value; }
static int lv_obj_get_width(lv_obj_t *object) { return object->width; }
static void lv_obj_set_style_text_line_space(lv_obj_t *object, int value, int part)
{ (void)value; (void)part; changed(object); }
static void lv_obj_remove_flag(lv_obj_t *object, unsigned flag) { (void)flag; changed(object); }
static void lv_obj_add_flag(lv_obj_t *object, unsigned flag) { (void)flag; changed(object); }
typedef struct { int battery_percent; const char *clock; } epd_app_status_t;
static void epd_app_status(epd_app_status_t *status)
{ assert(idle); ++status_calls; *status = (epd_app_status_t){80, "12:34"}; }
static void popup_open(const char *text, const char *detail)
{ assert(text && detail); changed(&panel); current->popup = &panel; ++popups; }

const ui_reader_view_t *ui_reader_view(void) { return &reading; }
bool ui_reader_settings_changed(void) { return settings_changed; }
bool ui_settings_cycle(ui_setting_id_t id) { (void)id; return true; }
const char *ui_settings_value(ui_setting_id_t id) { (void)id; return "5"; }
bool ui_reader_waiting(void) { return target_pending; }
void ui_reader_seek(unsigned target) { wanted = target; target_pending = target != reading.page; }
void ui_reader_turn(int direction) { ++turns; ui_reader_seek(reading.page + direction); }
void ui_reader_cancel_pending(void)
{ ++cancels; wanted = reading.page; target_pending = false; notified = false; }
bool ui_reader_save(void) { ++saves; actual_position = reading.page; return true; }
void ui_reader_close(void) { assert(idle); ++closes; ui_reader_save(); }
static void document_view_detach(void) { ++detaches; }
static unsigned document_view_toc_count(void) { return 0; }
static bool document_view_toc_seek(unsigned index) { (void)index; return false; }

bool ui_reader_reflow(void)
{
    assert(idle);
    ++reflows;
    reflow_at_step = steps;
    settings_changed = false;
    if (reflow_failure) { failure_pending = true; return false; }
    reading.font = &new_font;
    reading.margin = 32;
    reading.line_space = 4;
    reading.ready = document;
    reading.pages = 0;
    reading.indexing = true;
    ++reading.revision;
    wanted = 21;
    target_pending = true;
    notified = false;
    compute_ready = false;
    return true;
}

ui_reader_work_t ui_reader_step(uint32_t budget_ms, bool foreground, bool commit)
{
    ++steps;
    step_budget = budget_ms;
    step_foreground = foreground;
    step_commit = commit;
    assert(budget_ms == 8 && idle && commit);
    if (more_work) ++computed;
    if (commit)
    {
        if (failure_pending)
        {
            strcpy(reading.error, "layout failed");
            reading.indexing = false;
            ++reading.revision;
            failure_pending = false;
        }
        if (total_ready) { reading.pages = total_ready; reading.indexing = false; }
        if (foreground && target_pending && compute_ready && !notified)
        {
            ++reading.revision;
            if (document) notified = true;
            else
            {
                reading.page = wanted;
                reading.ready = true;
                target_pending = false;
                ui_reader_save();
            }
        }
    }
    if (!more_work || reading.error[0]) return UI_READER_WORK_IDLE;
    return foreground && target_pending && !compute_ready ? UI_READER_WORK_TARGET : UI_READER_WORK_BACKGROUND;
}

static bool document_view_bind(lv_obj_t *object)
{
    assert(idle);
    if (!notified) return false;
    changed(object);
    reading.page = wanted;
    reading.ready = true;
    target_pending = notified = false;
    ui_reader_save();
    return true;
}
static void reader_create(lv_obj_t *screen) { changed(screen); }
static void reader_panel_open(void) { changed(&panel); current->reader_panel = &panel; }
static void reader_panel_close(void)
{
    if (current->reader_panel) changed(&panel);
    current->reader_panel = NULL;
    current->reader_confirm_pending = false;
}
static void reader_toc_open(void) { reader_panel_open(); }
static void bookshelf_resume(void) { assert(idle); ++bookshelf_updates; }

#include "pages_under_test.h"

static void reset(bool is_document, bool menu)
{
    memset(&page, 0, sizeof(page));
    body = title = footer = progress = battery = clock_object = panel = option = jump = (lv_obj_t){0};
    current = &page;
    page.id = UI_PAGE_READER;
    page.foreground = true;
    page.reader_body = &body; page.reader_title = &title; page.reader_footer = &footer;
    page.reader_progress = &progress; page.reader_battery = &battery; page.reader_clock = &clock_object;
    page.reader_panel = menu ? &panel : NULL;
    page.reader_option_label = &option; page.reader_jump_label = &jump;
    page.reader_jump = 10;
    reading = (ui_reader_view_t){.text = "text", .title = "book", .font = &old_font,
        .page = 10, .percent = 25, .margin = 28, .ready = true, .indexing = true,
        .saved = true, .revision = 5};
    page.reader_revision = reading.revision;
    pending_page = NAV_IDLE;
    reader_session = reader_page_live = true;
    reader_resume_pending = false;
    idle = true; document = is_document;
    settings_changed = more_work = compute_ready = target_pending = notified = false;
    reflow_failure = failure_pending = false;
    wanted = actual_position = reading.page;
    total_ready = steps = computed = writes = reflows = turns = closes = saves = cancels = 0;
    detaches = bookshelf_updates = popups = status_calls = step_budget = reflow_at_step = 0;
}

static void test_busy_resume(bool is_document, bool edited_jump)
{
    reset(is_document, true);
    idle = false; settings_changed = true;
    reader_foreground(); reader_foreground();
    assert(reader_resume_pending && !writes && !steps && !reflows && !saves);
    more_work = true;
    assert(!books_process());
    assert(!steps && !computed && !writes && !reflows && !saves);
    more_work = false;
    assert(!books_process()); /* A pending idle-only resume does not request a short loop. */
    assert(!steps && !writes && !reflows);
    idle = true;
    assert(!books_process());
    assert(steps == 1 && reflows == 1 && reflow_at_step == 0 && step_foreground && step_commit);
    assert(!reader_resume_pending && page.reader_panel == &panel && page.reader_reflow_pending);
    assert(page.reader_reflow_page == 10 && page.reader_jump == 10 && actual_position == 10);
    assert(body.writes && battery.writes && clock_object.writes && option.writes && jump.writes);
    if (edited_jump) page.reader_jump = 35;
    unsigned before = writes;
    idle = false; compute_ready = true;
    books_process();
    assert(writes == before && actual_position == 10 && reflows == 1);
    idle = true;
    books_process();
    assert(reading.page == 21 && actual_position == 21 && !page.reader_reflow_pending);
    assert(page.reader_jump == (edited_jump ? 35u : 21u));
    assert(page.reader_panel == &panel && steps == 2 && reflows == 1);
}

static void test_unchanged_resume(void)
{
    reset(false, true);
    idle = false;
    reader_foreground();
    books_process();
    assert(!writes && !reflows && !saves);
    idle = true;
    books_process();
    assert(steps == 1 && !reflows && !body.writes && status_calls == 1);
    assert(option.writes == 1 && jump.writes == 1 && !reader_resume_pending);
    unsigned before = writes;
    assert(!books_process() && writes == before);
}

static void test_touch_budget(bool is_document)
{
    reset(is_document, false);
    books_process(); /* service_process runs computation before dispatching navigation. */
    int request = READER_NEXT;
    bool opened = false;
    assert(reader_action(&request, &opened));
    assert(turns == 1 && steps == 1 && !writes && reading.page == 10 && actual_position == 10);
    idle = false; compute_ready = true;
    books_process();
    assert(steps == 1 && !writes && reading.page == 10 && actual_position == 10);
    idle = true;
    books_process();
    assert(steps == 2 && reading.page == 11 && actual_position == 11 && body.writes);
}

static void test_initial_open_not_ready(bool is_document)
{
    reset(is_document, false);
    reading.ready = false;
    reading.page = 0;
    reading.saved = false;
    wanted = actual_position = 42;
    target_pending = true;
    reader_page_live = false;
    reader_start(&body);
    assert(reader_page_live && !reading.ready && !steps && !reflows);
    unsigned before = writes;
    idle = false;
    reader_foreground();
    assert(reader_resume_pending && writes == before && !steps && !reading.ready);
    more_work = compute_ready = true;
    assert(!books_process());
    assert(!steps && !computed && !reading.ready && !saves);
    assert(writes == before && actual_position == 42);
    idle = true; more_work = false;
    assert(!books_process());
    assert(steps == 1 && step_foreground && step_commit && !reader_resume_pending);
    assert(reading.ready && reading.page == 42 && actual_position == 42 && saves == 1);
    assert(page.reader_revision == reading.revision && body.writes > 1);
}

static void test_lifecycle_cancellation(void)
{
    reset(false, true);
    idle = false; settings_changed = true;
    reader_foreground();
    reader_pause();
    assert(!reader_resume_pending && cancels == 1 && actual_position == 10 && !writes);
    page.foreground = false; current = NULL;
    idle = true;
    books_process();
    assert(!step_foreground && !reflows && !writes && !closes);
    current = &page; page.foreground = true;
    reader_foreground();
    books_process();
    assert(reflows == 1 && page.reader_reflow_pending);

    reset(false, false);
    settings_changed = true;
    reader_foreground();
    pending_page = NAV_BACK;
    books_process();
    assert(!reflows && !step_foreground && reader_resume_pending);
    reader_pause(); reader_stop();
    assert(!reader_resume_pending && !reader_page_live);
    page.id = UI_PAGE_BOOKSHELF; pending_page = NAV_IDLE;
    total_ready = 60; target_pending = compute_ready = true; wanted = 30;
    books_process();
    assert(!step_foreground && closes == 1 && !reader_session && actual_position == 10);
    assert(bookshelf_updates == 1 && !reflows);

    reset(true, false);
    reader_foreground(); reader_stop();
    assert(!reader_resume_pending && detaches == 1);
    page.id = UI_PAGE_BOOKSHELF;
    books_process();
    assert(closes == 1 && !reader_session);
    reader_start(&body);
    assert(reader_page_live && !reader_resume_pending);
}

static void test_footer_only(void)
{
    reset(false, false);
    total_ready = 60; idle = false;
    books_process();
    assert(!writes && !reading.pages);
    idle = true;
    books_process();
    assert(footer.writes == 1 && !body.writes && !battery.writes && !clock_object.writes);
    assert(page.reader_rendered_pages == 60 && reading.revision == 5);
    books_process();
    assert(footer.writes == 1 && writes == 1);
}

static void test_reflow_failure(void)
{
    reset(false, true);
    settings_changed = reflow_failure = true;
    reader_foreground();
    assert(!writes);
    books_process();
    assert(reflows == 1 && steps == 1 && popups == 1 && reading.error[0]);
    assert(!reader_resume_pending && !page.reader_reflow_pending && actual_position == 10);
    books_process();
    assert(reflows == 1 && popups == 1);
}

static void test_work_priority(bool is_document)
{
    reset(is_document, false);
    more_work = true;
    assert(!books_process() && computed == 1); /* Prefetch uses the normal cadence. */
    ui_reader_seek(11);
    assert(books_process() && steps == 2); /* Only required CPU work expedites. */
    idle = false;
    unsigned before = writes;
    for (unsigned i = 0; i < 32; ++i) assert(!books_process());
    assert(steps == 2 && computed == 2 && writes == before && actual_position == 10);
    ui_reader_seek(12); /* The latest request survives display backpressure. */
    assert(!books_process() && target_pending && wanted == 12);
    idle = compute_ready = true;
    assert(!books_process());
    assert(reading.page == 12 && actual_position == 12 && steps == 3 && computed == 3);
    assert(!books_process() && computed == 4); /* Background preparation still advances. */
}

int main(void)
{
    for (unsigned doc = 0; doc < 2; ++doc)
    {
        test_busy_resume(doc, false);
        test_busy_resume(doc, true);
        test_touch_budget(doc);
        test_initial_open_not_ready(doc);
        test_work_priority(doc);
    }
    test_unchanged_resume();
    test_lifecycle_cancellation();
    test_footer_only();
    test_reflow_failure();
    puts("pages: display backpressure, target/background priority, cold open, busy resume, one-step budget, reflow/menu, lifecycle, footer and failure tests passed");
    return 0;
}

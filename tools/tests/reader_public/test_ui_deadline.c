#include <stdint.h>
static uint32_t test_tick;
static uint32_t rt_tick_get(void) { return test_tick; }

/* Run the existing service lifetime assertions before exercising deadlines. */
#define EPD_TEST_EXTERNAL_TICK
#define main storage_lifecycle_main
#include "../test_app_storage_service.c"
#undef main

static epd_service_t *first, *second;
static bool first_runnable, second_runnable;
static uint32_t first_delay, second_delay;
static unsigned step_calls;
static const uint32_t tick_ms = 1000u / RT_TICK_PER_SECOND;

static bool deadline_step(epd_service_t *service, bool stopping)
{
    ++step_calls;
    assert(!service->ui_requested);
    if (stopping)
    {
        epd_service_ui_request(service, 0);
        assert(!service->ui_requested);
        return true;
    }
    if (service == first && first_runnable)
    {
        epd_service_ui_request(service, first_delay);
        epd_service_ui_request(service, first_delay + tick_ms);
    }
    if (service == second && second_runnable)
    {
        epd_service_ui_request(service, second_delay + tick_ms);
        epd_service_ui_request(service, second_delay);
        epd_service_ui_request(first, 0);
    }
    return false;
}

static void completed_worker(epd_service_t *service) { (void)service; }

int main(void)
{
    assert(storage_lifecycle_main() == 0);
    assert(app_service_ui_delay(20) == 20);
    assert(app_service_ui_delay(0) == 0);
    epd_background_t definition = {.ui_process = deadline_step};
    first = app_service_start("first", &definition, NULL);
    second = app_service_start("second", &definition, NULL);
    assert(first && second);
    epd_service_ui_request(NULL, 0);
    epd_service_ui_request(first, 0);
    assert(app_service_ui_delay(20) == 20);
    for (unsigned i = 0; i < 10; ++i)
    {
        app_service_process();
        assert(app_service_ui_delay(20) == 20);
    }
    assert(step_calls == 20);
    first_runnable = second_runnable = true;
    first_delay = 5 * tick_ms;
    second_delay = 2 * tick_ms;
    app_service_process();
    assert(app_service_ui_delay(100) == 2 * tick_ms);
    assert(app_service_ui_delay(1) == 1);
    ++test_tick;
    assert(app_service_ui_delay(100) == tick_ms);
    ++test_tick;
    assert(app_service_ui_delay(100) == 0);
    assert(app_service_ui_delay(100) == 0);
    first_runnable = second_runnable = false;
    app_service_process();
    assert(app_service_ui_delay(20) == 20);
    first_runnable = true;
    first_delay = 1;
    app_service_process();
    assert(app_service_ui_delay(100) == tick_ms);
    ++test_tick;
    assert(app_service_ui_delay(100) == 0);
    first_delay = 5 * tick_ms;
    test_tick = UINT32_MAX - 2;
    app_service_process();
    assert(app_service_ui_delay(100) == 5 * tick_ms);
    test_tick += 4;
    assert(app_service_ui_delay(100) == tick_ms);
    ++test_tick;
    assert(app_service_ui_delay(100) == 0);
    first_delay = 0;
    app_service_process();
    assert(app_service_ui_delay(20) == 0);
    first_delay = UINT32_MAX - tick_ms;
    app_service_process();
    assert(app_service_ui_delay(20) == 20);
    first_delay = tick_ms;
    app_service_process();
    app_service_stop("first");
    assert(!first->ui_requested && app_service_ui_delay(20) == 20);
    app_service_stop("second");
    app_service_process();
    assert(!services);

    /* Worker completion and card cancellation clear deadlines before cleanup. */
    fail_thread_start = false;
    definition.run = completed_worker;
    definition.stack_size = 4096;
    definition.priority = 22;
    first = app_service_start("first", &definition, NULL);
    app_service_process();
    assert(first->ui_requested);
    mock_thread.entry(mock_thread.parameter);
    assert(!first->ui_requested && app_service_ui_delay(20) == 20);
    app_service_process();
    assert(!services);
    definition.run = NULL;
    card_available = true;
    first = app_service_start("first", &definition, &pins);
    app_service_process();
    assert(first->ui_requested);
    card_available = false;
    app_service_process();
    assert(!services && app_service_ui_delay(20) == 20 && !pins);
    printf("UI deadlines: lifecycle, idle, min, expiry, wrap, stop at %u Hz passed\n",
           RT_TICK_PER_SECOND);
    return 0;
}

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <sdcard.h>
#include "../../src/services/storage.c"
#include "../../src/boards/board_sdcard.c"

static sdcard_snapshot_t mock_card;
static sdcard_config_t started_config;
static unsigned starts, mounts, directories_prepared, stat_calls, pin_sets, pin_modes;
static int irq_depth, root_result, flash_result;
static rt_err_t start_result;
static uint32_t acknowledged;

rt_base_t rt_hw_interrupt_disable(void) { return irq_depth++; }
void rt_hw_interrupt_enable(rt_base_t level) { irq_depth = (int)level; }
rt_err_t rt_mutex_init(rt_mutex_t mutex, const char *name, int flag)
{
    assert(!strcmp(name, "storage") && flag == RT_IPC_FLAG_PRIO);
    mutex->nesting = 0;
    return RT_EOK;
}
rt_err_t rt_mutex_take(rt_mutex_t mutex, int timeout)
{
    assert(timeout == RT_WAITING_FOREVER);
    ++mutex->nesting;
    return RT_EOK;
}
rt_err_t rt_mutex_release(rt_mutex_t mutex) { --mutex->nesting; return RT_EOK; }
void rt_kprintf(const char *format, ...) { (void)format; }
int register_mtd_device(unsigned address, unsigned size, const char *name)
{
    assert(address == FS_REGION_START_ADDR && size == FS_REGION_SIZE);
    assert(!strcmp(name, "flash0"));
    return 0;
}
int dfs_mount(const char *device, const char *path, const char *type,
              unsigned long flags, const void *data)
{
    assert(flags == 0);
    ++mounts;
    if (!strcmp(path, "/"))
    {
        assert(!device && !strcmp(type, "rom") && data == &root);
        return root_result;
    }
    assert(!strcmp(device, "flash0") && !strcmp(path, "/flash") && !strcmp(type, "lfs"));
    return flash_result;
}
void storage_prepare_directories(void) { ++directories_prepared; }
int dfs_statfs(const char *path, struct statfs *fs)
{
    assert(io_lock.nesting == 1);
    assert(storage_path_available(path));
    ++stat_calls;
    fs->f_blocks = 8192;
    fs->f_bfree = 2048;
    fs->f_bsize = 4096;
    return 0;
}
void HAL_PIN_Set(int pad, int function, int pull, int hcpu)
{
    assert(pad == SD_INSERT_DETECT_PIN && function == GPIO_A0 + SD_INSERT_DETECT_PIN);
    assert(pull == PIN_PULLUP && hcpu == 1);
    ++pin_sets;
}
void rt_pin_mode(rt_base_t pin, rt_base_t mode)
{
    assert(pin == SD_INSERT_DETECT_PIN && mode == PIN_MODE_INPUT_PULLUP);
    ++pin_modes;
}
rt_err_t sdcard_start(const sdcard_config_t *config)
{
    assert(mounts == 2 && io_lock.nesting == 0);
    assert(!strcmp(config->device_name, "sd0") && !strcmp(config->mount_point, "/sdcard"));
    assert(config->detect_pin == 11 && config->present_level == PIN_LOW);
    assert(config->io_lock == &io_lock);
    started_config = *config;
    ++starts;
    return start_result;
}
void sdcard_get_snapshot(sdcard_snapshot_t *view) { *view = mock_card; }
rt_err_t sdcard_release_complete(uint32_t id)
{
    assert(io_lock.nesting == 0);
    acknowledged = id;
    if (!id || id != mock_card.release_id || mock_card.state != SDCARD_DRAINING)
        return -RT_EINVAL;
    return RT_EOK;
}

static void reset(void)
{
    memset(&mock_card, 0, sizeof(mock_card));
    memset(&started_config, 0, sizeof(started_config));
    starts = mounts = directories_prepared = stat_calls = pin_sets = pin_modes = 0;
    irq_depth = root_result = flash_result = 0;
    start_result = RT_EOK;
    flash_mounted = false;
    acknowledged = 0;
#ifdef PKG_USING_EPD_SDCARD
    apps_session = 0;
#endif
}

int main(void)
{
    storage_info_t info;
    reset();
#ifdef PKG_USING_EPD_SDCARD
    assert(test_board_early_init() == RT_EOK);
    board_sdcard_prepare();
    assert(pin_sets == 2 && pin_modes == 2);
#else
    board_sdcard_prepare();
    assert(!pin_sets && !pin_modes);
#endif
    storage_init();
    assert(mounts == 2 && directories_prepared == 1);
    assert(storage_available(STORAGE_FLASH) && !storage_available(STORAGE_SD));
    assert(!storage_available((storage_volume_t)-1) && !storage_available(STORAGE_COUNT));
    assert(storage_session_valid(STORAGE_FLASH, 0) && storage_apps_enabled(STORAGE_FLASH));
    assert(!storage_path_available(NULL) && !storage_path_available(""));
    assert(!strcmp(storage_path_root("/sdcard/book.txt"), "/sdcard"));
    assert(!strcmp(storage_relative_path("/flash/apps/books"), "/apps/books"));
    assert(storage_info(STORAGE_FLASH, &info) && info.total == 33554432 && info.used == 25165824);
    assert(!storage_info(STORAGE_SD, &info) && stat_calls == 1 && !info.mounted);
#ifdef PKG_USING_EPD_SDCARD
    assert(starts == 1 && started_config.io_lock == &io_lock);
    mock_card.state = SDCARD_PROBING;
    mock_card.revision = 2;
    assert(!storage_changing() && !storage_release_requested() && storage_revision() == 3);
    mock_card.state = SDCARD_READY;
    mock_card.mounted = mock_card.available = RT_TRUE;
    mock_card.session = 7;
    ++mock_card.revision;
    assert(storage_available(STORAGE_SD) && storage_card_session() == 7);
    assert(storage_revision() == 4 && storage_session_valid(STORAGE_SD, 7));
    assert(!storage_session_valid(STORAGE_SD, 6) && !storage_apps_enabled(STORAGE_SD));
    assert(!storage_card_enable_apps(6) && storage_card_enable_apps(7));
    assert(storage_apps_enabled(STORAGE_SD) && irq_depth == 0);
    assert(storage_info(STORAGE_SD, &info) && stat_calls == 2);
    /* Removal IRQ invalidates access before the worker reaches DRAINING. */
    mock_card.available = RT_FALSE;
    mock_card.session = 8;
    assert(!storage_available(STORAGE_SD) && !storage_session_valid(STORAGE_SD, 7));
    assert(!storage_apps_enabled(STORAGE_SD) && !storage_card_enable_apps(7));
    mock_card.state = SDCARD_DRAINING;
    mock_card.release_id = 41;
    assert(storage_changing() && storage_release_requested());
    assert(storage_release_request_id() == 41);
    assert(storage_release_complete(40) == -RT_EINVAL && acknowledged == 40);
    assert(storage_release_complete(41) == RT_EOK);
    mock_card.error_stage = SDCARD_ERROR_RELEASE_WAIT;
    mock_card.error = -RT_ETIMEOUT;
    assert(storage_release_timed_out() && !storage_changing());
    assert(storage_release_requested() && !storage_available(STORAGE_SD));
    mock_card.state = SDCARD_UNMOUNTING;
    assert(storage_changing() && !storage_release_requested() && !storage_release_timed_out());
    mock_card.state = SDCARD_FAULT;
    mock_card.error_stage = SDCARD_ERROR_UNMOUNT;
    assert(!storage_changing() && !storage_available(STORAGE_SD));
    assert(!storage_info(STORAGE_SD, &info) && stat_calls == 2);
    assert(mock_card.mounted); /* Adapter must not erase actual mount state. */
    mock_card.state = SDCARD_READY;
    mock_card.session = 9;
    mock_card.available = RT_TRUE;
    assert(!storage_apps_enabled(STORAGE_SD));
    assert(!storage_card_enable_apps(7) && storage_card_enable_apps(9));
    assert(storage_release_complete(41) == -RT_EINVAL);
    reset();
    start_result = -RT_ENOMEM;
    storage_init();
    assert(storage_available(STORAGE_FLASH) && !storage_available(STORAGE_SD));
    reset();
    flash_result = -1;
    storage_init();
    assert(!storage_available(STORAGE_FLASH) && starts == 1 && !directories_prepared);
#else
    assert(starts == 0 && storage_card_session() == 0 && storage_revision() == 1);
    assert(!storage_card_enable_apps(1) && !storage_release_requested());
    assert(!storage_changing() && !storage_release_timed_out());
    assert(storage_release_complete(1) == -RT_ENOSYS);
#endif
    reset();
    root_result = -1;
    storage_init();
    assert(mounts == 1 && starts == 0 && !storage_available(STORAGE_FLASH));
    puts("storage adapter: boot, board mux, snapshots, sessions and release IDs passed");
    return 0;
}

/* Compile the real worker; only RTOS, pin, DFS and host I/O are substituted. */
#include <stdio.h>
#include <setjmp.h>
#include "../src/sdcard.c"

static struct rt_mutex io;
static struct rt_thread thread;
static struct rt_device device = {RT_Device_Class_Block};
static struct dfs_filesystem filesystem = {"/sdcard"};
static struct dfs_fd file = {1, &filesystem};
static struct dfs_fd *files[1];
static struct dfs_fdtable table = {1, files};
static sdcard_port_status_t host_status;
static jmp_buf idle;
static int irq_level, pin, attached, enabled, errno_value, mutex_fail;
static int start_failure, sem_inits, sem_live, pin_modes, foreign_mount;
static int requests, mounts, unmounts, mount_error, unmount_error, request_error;
static int request_pending, pending_polls, pending_card, probe_card, missing_block;
static int hold_pending, remove_stuck, release_delay, release_waits, release_error;
static int mount_swap, probe_swap, release_swap, sample_bounce, sdk_poll_count, wait_count;
static uint32_t last_release;
static void (*irq_handler)(void *);

enum { FAIL_BIND=1, FAIL_PATH, FAIL_OWNER_LOCK, FAIL_SEM1, FAIL_SEM2,
       FAIL_THREAD, FAIL_ATTACH, FAIL_ENABLE, FAIL_STARTUP };

rt_base_t rt_hw_interrupt_disable(void) { return irq_level++; }
void rt_hw_interrupt_enable(rt_base_t level)
{
    assert(irq_level == level + 1);
    irq_level = (int)level;
}
rt_err_t rt_mutex_take(rt_mutex_t mutex, int timeout)
{
    assert(!irq_level && mutex == &io && timeout == RT_WAITING_FOREVER);
    if (start_failure == FAIL_OWNER_LOCK || mutex_fail) return -RT_ERROR;
    assert(!mutex->nesting);
    ++mutex->nesting;
    return RT_EOK;
}
rt_err_t rt_mutex_release(rt_mutex_t mutex)
{
    assert(!irq_level && mutex == &io && mutex->nesting == 1);
    --mutex->nesting;
    return RT_EOK;
}
rt_err_t rt_sem_init(struct rt_semaphore *sem, const char *name, int value, int flag)
{
    (void)name; (void)flag;
    ++sem_inits;
    if ((start_failure == FAIL_SEM1 && sem_inits == 1) ||
        (start_failure == FAIL_SEM2 && sem_inits == 2)) return -RT_ERROR;
    sem->value = value;
    sem->live = 1;
    ++sem_live;
    return RT_EOK;
}
rt_err_t rt_sem_detach(struct rt_semaphore *sem)
{
    assert(sem->live);
    sem->live = 0;
    --sem_live;
    return RT_EOK;
}
rt_err_t rt_sem_release(struct rt_semaphore *sem)
{
    assert(sem->live);
    /* The worker and release handshakes permit at most one queued token. */
    assert(sem->value == 0);
    ++sem->value;
    return RT_EOK;
}
rt_err_t rt_sem_take(struct rt_semaphore *sem, int timeout)
{
    assert(!irq_level && !io.nesting && sem->live);
    if (sem == &s_card.event && !sem->value) longjmp(idle, 1);
    if (sem == &s_card.released && !sem->value)
    {
        sdcard_snapshot_t view;
        ++release_waits;
        sdcard_get_snapshot(&view);
        assert(view.state == SDCARD_DRAINING && !view.available);
        assert(view.release_id && view.release_id != last_release);
        assert(sdcard_release_complete(0) == -RT_EINVAL);
        assert(sdcard_release_complete(view.release_id + 1) == -RT_EINVAL);
        if (last_release) assert(sdcard_release_complete(last_release) == -RT_EINVAL);
        if (release_swap)
        {
            release_swap = 0;
            /* The interrupt stays enabled while the consumer is releasing. */
            pin = PIN_LOW;
            irq_handler(NULL);
            assert(!s_card.view.available && !unmounts);
        }
        if (release_error) return -RT_ERROR;
        if (release_delay)
        {
            --release_delay;
            assert(timeout == SDCARD_RELEASE_MS);
            assert(unmounts == 0);
            return -RT_ETIMEOUT;
        }
        if (release_waits == 2)
        {
            assert(timeout == RT_WAITING_FOREVER);
            assert(view.error_stage == SDCARD_ERROR_RELEASE_WAIT && view.mounted);
        }
        assert(sdcard_release_complete(view.release_id) == RT_EOK);
        assert(sdcard_release_complete(view.release_id) == -RT_EBUSY);
        last_release = view.release_id;
    }
    assert(sem->value == 1);
    --sem->value;
    return RT_EOK;
}
rt_thread_t rt_thread_create(const char *name, void (*entry)(void *), void *arg,
                            unsigned stack, int priority, int tick)
{
    (void)name; (void)arg; (void)tick;
    assert(entry == _worker && stack == SDCARD_STACK_SIZE && priority == SDCARD_PRIORITY);
    if (start_failure == FAIL_THREAD) return RT_NULL;
    thread.live = 1;
    return &thread;
}
rt_err_t rt_thread_startup(rt_thread_t value)
{
    assert(value == &thread && thread.live);
    return start_failure == FAIL_STARTUP ? -RT_ERROR : RT_EOK;
}
rt_err_t rt_thread_delete(rt_thread_t value)
{
    assert(value == &thread && thread.live);
    thread.live = 0;
    return RT_EOK;
}
static void edge(int level)
{
    pin = level;
    assert(enabled && attached && irq_handler);
    irq_handler(RT_NULL);
}
static void finish_request(void)
{
    host_status.change = SDCARD_CHANGE_DONE;
    host_status.has_card = pending_card;
    host_status.sd_memory = pending_card;
    host_status.block_device = pending_card && !missing_block;
}
void rt_thread_mdelay(int milliseconds)
{
    assert(!irq_level && !io.nesting);
    assert(++wait_count < 5000);
    if (sample_bounce && milliseconds == SDCARD_CONFIRM_MS)
    {
        --sample_bounce;
        edge(PIN_HIGH); edge(PIN_LOW);
    }
    if (milliseconds == SDCARD_POLL_MS)
    {
        ++sdk_poll_count;
        if (probe_swap)
        {
            probe_swap = 0;
            edge(PIN_HIGH); edge(PIN_LOW);
        }
        if (!hold_pending && host_status.change == SDCARD_CHANGE_PENDING && --pending_polls <= 0)
            finish_request();
    }
}
rt_tick_t rt_tick_from_millisecond(int milliseconds) { return milliseconds; }
int rt_get_errno(void) { return errno_value; }
void rt_set_errno(int error) { errno_value = error; }
int rt_pin_read(rt_base_t value) { assert(value == SD_INSERT_DETECT_PIN); return pin; }
void rt_pin_mode(rt_base_t value, rt_base_t mode)
{
    assert(value == SD_INSERT_DETECT_PIN && mode == PIN_MODE_INPUT && attached);
    ++pin_modes;
}
rt_err_t rt_pin_attach_irq(rt_base_t value, int mode, void (*handler)(void *), void *arg)
{
    (void)arg;
    assert(value == SD_INSERT_DETECT_PIN && mode == PIN_IRQ_MODE_RISING_FALLING);
    if (start_failure == FAIL_ATTACH) return -RT_EBUSY;
    assert(!attached);
    attached = 1;
    irq_handler = handler;
    return RT_EOK;
}
rt_err_t rt_pin_detach_irq(rt_base_t value)
{
    assert(value == SD_INSERT_DETECT_PIN && attached);
    attached = 0;
    irq_handler = NULL;
    return RT_EOK;
}
rt_err_t rt_pin_irq_enable(rt_base_t value, int enable)
{
    assert(value == SD_INSERT_DETECT_PIN && attached);
    if (enable && start_failure == FAIL_ENABLE) return -RT_ERROR;
    enabled = enable;
    return RT_EOK;
}
rt_device_t rt_device_find(const char *name)
{
    assert(strcmp(name, "sd0") == 0);
    return host_status.block_device ? &device : RT_NULL;
}
char *dfs_normalize_path(const char *directory, const char *filename)
{
    char *result;
    assert(!directory);
    if (start_failure == FAIL_PATH) return NULL;
    result = malloc(strlen(filename) + 1);
    assert(result);
    strcpy(result, filename);
    return result;
}
const char *dfs_filesystem_get_mounted_path(rt_device_t value)
{
    assert(value == &device && io.nesting);
    return foreign_mount ? "/somewhere" : NULL;
}
struct dfs_filesystem *dfs_filesystem_lookup(const char *path)
{
    assert(strcmp(path, "/sdcard") == 0 && io.nesting);
    return foreign_mount ? &filesystem : NULL;
}
int dfs_mount(const char *name, const char *path, const char *type, unsigned flags, const void *data)
{
    assert(!irq_level && io.nesting);
    assert(strcmp(name, "sd0") == 0 && strcmp(path, "/sdcard") == 0);
    assert(strcmp(type, "elm") == 0 && !flags && !data);
    assert(host_status.change == SDCARD_CHANGE_DONE && host_status.has_card);
    ++mounts;
    if (mount_swap) { mount_swap = 0; edge(PIN_HIGH); edge(PIN_LOW); }
    if (mount_error) { errno_value = mount_error; return -1; }
    return 0;
}
int dfs_unmount(const char *path)
{
    assert(!irq_level && io.nesting && strcmp(path, "/sdcard") == 0);
    assert(s_card.acknowledged && !s_card.view.available && s_card.view.mounted);
    ++unmounts;
    if (unmount_error) { errno_value = unmount_error; return -1; }
    return 0;
}
void dfs_lock(void) { assert(!irq_level && io.nesting); }
void dfs_unlock(void) { assert(!irq_level && io.nesting); }
struct dfs_fdtable *dfs_fdtable_get(void) { return &table; }
rt_err_t sdcard_port_bind(const char *name, struct rt_mmcsd_host **host)
{
    assert(strcmp(name, "sd0") == 0);
    if (start_failure == FAIL_BIND) return -RT_ENOSYS;
    *host = (struct rt_mmcsd_host *)&device;
    return RT_EOK;
}
void sdcard_port_get_status(struct rt_mmcsd_host *host, sdcard_port_status_t *status)
{
    assert(host == (struct rt_mmcsd_host *)&device);
    *status = host_status;
}
rt_err_t sdcard_port_request(struct rt_mmcsd_host *host)
{
    assert(host == (struct rt_mmcsd_host *)&device && !io.nesting && !irq_level);
    assert(host_status.change != SDCARD_CHANGE_PENDING);
    ++requests;
    if (request_error) return request_error;
    if (host_status.has_card)
    {
        assert(!s_card.view.mounted && s_card.acknowledged);
        pending_card = remove_stuck ? 1 : 0;
    }
    else pending_card = probe_card;
    host_status.change = SDCARD_CHANGE_PENDING;
    if (request_pending || hold_pending) pending_polls = 2;
    else finish_request();
    return RT_EOK;
}

static const sdcard_config_t config = {"sd0", "/sdcard", SD_INSERT_DETECT_PIN, PIN_LOW, &io};
static void reset(void)
{
    memset(&s_card, 0, sizeof(s_card));
    memset(&host_status, 0, sizeof(host_status));
    io.nesting = thread.live = irq_level = attached = enabled = errno_value = mutex_fail = 0;
    start_failure = sem_inits = sem_live = pin_modes = foreign_mount = 0;
    requests = mounts = unmounts = mount_error = unmount_error = request_error = 0;
    request_pending = pending_polls = pending_card = missing_block = 0;
    hold_pending = remove_stuck = release_delay = release_waits = release_error = 0;
    mount_swap = probe_swap = release_swap = sample_bounce = sdk_poll_count = wait_count = 0;
    last_release = 0;
    pin = PIN_HIGH;
    probe_card = 1;
    files[0] = NULL;
    irq_handler = NULL;
}
static void pump(void)
{
    if (!setjmp(idle)) _worker(NULL);
    assert(!irq_level && !io.nesting);
}
static sdcard_snapshot_t snapshot(void)
{
    sdcard_snapshot_t result;
    sdcard_get_snapshot(&result);
    return result;
}
static void ready(void)
{
    reset();
    pin = PIN_LOW;
    assert(sdcard_start(&config) == RT_EOK);
    pump();
    assert(snapshot().available && snapshot().mounted && mounts == 1);
}
static void test_start_unwind(void)
{
    int failure;
    sdcard_config_t invalid = config;
    reset();
    invalid.detect_pin++;
    assert(sdcard_start(NULL) == -RT_EINVAL);
    assert(sdcard_start(&invalid) == -RT_EINVAL && !pin_modes && !requests);
    for (failure = FAIL_BIND; failure <= FAIL_STARTUP; ++failure)
    {
        reset();
        start_failure = failure;
        assert(sdcard_start(&config) != RT_EOK);
        assert(!sem_live && !thread.live && !attached && !enabled && !requests);
        assert(snapshot().error_stage == SDCARD_ERROR_START);
        if (failure <= FAIL_ATTACH) assert(!pin_modes);
        start_failure = sem_inits = 0;
        assert(sdcard_start(&config) == RT_EOK);
        assert(sdcard_start(&config) == -RT_EBUSY);
        pump();
        assert(snapshot().state == SDCARD_ABSENT && !requests);
    }
    reset(); foreign_mount = 1;
    assert(sdcard_start(&config) == -RT_EBUSY && !pin_modes && !sem_live);
}
static void test_boot_and_completion(void)
{
    int change, card;
    for (change = SDCARD_CHANGE_NONE; change <= SDCARD_CHANGE_DONE; ++change)
    for (card = 0; card <= 1; ++card)
    {
        reset(); pin = PIN_LOW;
        host_status.change = (sdcard_port_change_t)change;
        probe_card = pending_card = card;
        if (change == SDCARD_CHANGE_DONE)
            host_status.has_card = host_status.sd_memory = host_status.block_device = card;
        assert(sdcard_start(&config) == RT_EOK);
        pump();
        assert(requests == (change == SDCARD_CHANGE_NONE ? 1 : 0));
        assert(snapshot().available == card);
        if (!card)
        {
            assert(snapshot().error_stage == SDCARD_ERROR_PROBE_RESULT);
            _wake_worker(); pump();
            assert(requests == (change == SDCARD_CHANGE_NONE ? 1 : 0));
            edge(PIN_HIGH); pump();
            probe_card = 1;
            edge(PIN_LOW); pump();
            assert(snapshot().available);
        }
    }
    reset(); host_status.change = SDCARD_CHANGE_DONE;
    assert(sdcard_start(&config) == RT_EOK); pump();
    edge(PIN_LOW); pump();
    assert(snapshot().available && requests == 1);
    reset(); host_status.change = SDCARD_CHANGE_PENDING; pending_card = 1;
    assert(sdcard_start(&config) == RT_EOK); pump();
    assert(snapshot().state == SDCARD_ABSENT && !mounts && requests == 1);
}
static void test_sessions_and_release(void)
{
    uint32_t session;
    ready(); session = snapshot().session;
    release_delay = 1;
    edge(PIN_HIGH);
    assert(!snapshot().available && snapshot().mounted && snapshot().session != session);
    edge(PIN_LOW); edge(PIN_HIGH); edge(PIN_LOW);
    assert(s_card.event.value == 1);
    pump();
    assert(release_waits == 2 && unmounts == 1 && mounts == 2 && requests == 3);
    assert(snapshot().available && snapshot().session != session && !snapshot().release_id);
    assert(sdcard_release_complete(last_release) == -RT_EINVAL);
    release_waits = 0;
    edge(PIN_HIGH); pump();
    assert(snapshot().state == SDCARD_ABSENT && !snapshot().mounted && requests == 4);
    assert(last_release == 2);
    _wake_worker(); pump();
    assert(requests == 4);
}
static void test_edges_during_operations(void)
{
    reset(); pin = PIN_LOW; sample_bounce = 1;
    assert(sdcard_start(&config) == RT_EOK); pump();
    assert(snapshot().available && requests == 1 && mounts == 1);
    reset(); pin = PIN_LOW; request_pending = probe_swap = 1;
    assert(sdcard_start(&config) == RT_EOK); pump();
    assert(snapshot().available && requests == 3 && mounts == 1 && unmounts == 0);
    reset(); pin = PIN_LOW; mount_swap = 1;
    assert(sdcard_start(&config) == RT_EOK); pump();
    assert(snapshot().available && requests == 3 && mounts == 2 && unmounts == 1);
    ready(); release_swap = 1;
    edge(PIN_HIGH); pump();
    assert(snapshot().available && requests == 3 && mounts == 2 && unmounts == 1);
}
static void test_probe_and_mount_failure(void)
{
    int failure;
    for (failure = 0; failure != 4; ++failure)
    {
        reset(); pin = PIN_LOW;
        if (failure == 0) request_error = -RT_EFULL;
        if (failure == 1) missing_block = 1;
        if (failure == 2) mount_error = 19;
        if (failure == 3) mutex_fail = 1;
        /* A lock failure during startup is tested separately. */
        mutex_fail = 0;
        assert(sdcard_start(&config) == RT_EOK);
        if (failure == 3) mutex_fail = 1;
        pump();
        assert(snapshot().state == SDCARD_FAULT && !snapshot().available && !snapshot().mounted);
        assert(!s_card.fatal);
        assert(snapshot().error_stage == (failure == 0 ? SDCARD_ERROR_PROBE_SUBMIT :
                                         failure == 1 ? SDCARD_ERROR_PROBE_RESULT : SDCARD_ERROR_MOUNT));
        if (failure == 2) assert(snapshot().error == -19);
        _wake_worker(); pump(); assert(requests == 1);
        request_error = missing_block = mount_error = mutex_fail = 0;
        edge(PIN_HIGH); pump(); edge(PIN_LOW); pump();
        assert(snapshot().available);
    }
}
static void test_isolation_failures(void)
{
    int failure, before;
    for (failure = 0; failure < 7; ++failure)
    {
        ready();
        if (failure == 0) files[0] = &file;
        if (failure == 1) unmount_error = -5;
        if (failure == 2) request_error = -RT_EFULL;
        if (failure == 3) hold_pending = 1;
        if (failure == 4) remove_stuck = 1;
        if (failure == 5) release_error = 1;
        if (failure == 6) mutex_fail = 1;
        edge(PIN_HIGH); pump();
        assert(snapshot().state == SDCARD_FAULT && !snapshot().available && s_card.fatal);
        assert(snapshot().mounted == (failure <= 1 || failure >= 5));
        if (failure == 0) assert(snapshot().error_stage == SDCARD_ERROR_OPEN_FILES && !unmounts);
        if (failure == 3) assert(sdk_poll_count == SDCARD_REMOVE_POLLS);
        before = requests;
        edge(PIN_LOW); edge(PIN_HIGH); pump();
        assert(requests == before && !snapshot().available);
    }
    reset(); pin = PIN_LOW; hold_pending = 1;
    assert(sdcard_start(&config) == RT_EOK); pump();
    assert(snapshot().error_stage == SDCARD_ERROR_PROBE_WAIT && s_card.fatal);
    assert(sdk_poll_count == SDCARD_PROBE_POLLS && requests == 1);
    hold_pending = 0; finish_request();
    edge(PIN_HIGH); edge(PIN_LOW); pump();
    assert(!snapshot().available && !mounts && requests == 1);
}
int main(void)
{
    test_start_unwind();
    test_boot_and_completion();
    test_sessions_and_release();
    test_edges_during_operations();
    test_probe_and_mount_failure();
    test_isolation_failures();
    puts("sdcard worker: 6 groups passed (startup, boot adoption, sessions, edges, recovery, isolation)");
    return 0;
}

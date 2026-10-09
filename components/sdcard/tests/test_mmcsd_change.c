/* Compile the real SDK core and headers; only RTOS and card I/O are substituted. */
#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>
#include "mmcsd_core.c"

_Static_assert(sizeof(void *) == sizeof(rt_uint32_t), "Use a 32-bit host compiler");

struct card_fixture
{
    unsigned responds;
    int init_error;
    int probes;
    int removed;
    int busy_in_probe;
};

static jmp_buf detector_exit;
static int receive_budget;
static int receive_error_once;
static int send_error_once;
static int irq_level;
static int warnings;
static struct rt_mmcsd_host *unlock_watch;
static void (*send_hook)(void);
static void (*irq_hook)(void);
static struct rt_mmcsd_host *hook_host;

static void run_detector(int budget)
{
    receive_budget = budget;
    if (setjmp(detector_exit) == 0)
        mmcsd_detect(RT_NULL);
    assert(irq_level == 0);
}

rt_base_t rt_hw_interrupt_disable(void)
{
    int previous = irq_level;
    irq_level = 1;
    return previous;
}

void rt_hw_interrupt_enable(rt_base_t level)
{
    void (*hook)(void);
    assert(irq_level == 1);
    irq_level = (int)level;
    if (!irq_level && irq_hook)
    {
        hook = irq_hook;
        irq_hook = NULL;
        hook();
    }
}

rt_err_t rt_mb_init(struct rt_mailbox *mb, const char *name, void *pool, unsigned size, int flag)
{
    (void)name;
    (void)flag;
    memset(mb, 0, sizeof(*mb));
    mb->pool = pool;
    mb->size = size;
    return RT_EOK;
}

rt_err_t rt_mb_send(struct rt_mailbox *mb, rt_uint32_t value)
{
    void (*hook)(void);
    /* The checked request must not hold interrupts off across a scheduling IPC. */
    assert(irq_level == 0);
    if (mb == &mmcsd_detect_mb && send_error_once)
    {
        int error = send_error_once;
        send_error_once = 0;
        return error;
    }
    if (mb->entry == mb->size)
        return -RT_EFULL;
    mb->pool[mb->in] = value;
    mb->in = (mb->in + 1) % mb->size;
    mb->entry++;
    if (mb == &mmcsd_detect_mb && send_hook)
    {
        hook = send_hook;
        send_hook = NULL;
        hook();
    }
    return RT_EOK;
}

rt_err_t rt_mb_recv(struct rt_mailbox *mb, rt_ubase_t *value, int timeout)
{
    (void)timeout;
    assert(irq_level == 0);
    if (mb == &mmcsd_detect_mb)
    {
        if (receive_error_once)
        {
            receive_error_once = 0;
            return -RT_ERROR;
        }
        if (receive_budget == 0 || mb->entry == 0)
            longjmp(detector_exit, 1);
        --receive_budget;
    }
    if (!mb->entry)
        return -RT_ETIMEOUT;
    *value = mb->pool[mb->out];
    mb->out = (mb->out + 1) % mb->size;
    mb->entry--;
    return RT_EOK;
}

rt_err_t rt_mutex_init(struct rt_mutex *mutex, const char *name, int flag)
{
    (void)name;
    (void)flag;
    mutex->nesting = 0;
    return RT_EOK;
}

rt_err_t rt_mutex_take(struct rt_mutex *mutex, int timeout)
{
    (void)timeout;
    assert(irq_level == 0);
    mutex->nesting++;
    return RT_EOK;
}

rt_err_t rt_mutex_release(struct rt_mutex *mutex)
{
    assert(irq_level == 0 && mutex->nesting > 0);
    if (unlock_watch && mutex == &unlock_watch->bus_lock && mutex->nesting == 1)
        assert(mmcsd_change_state_get(unlock_watch) == MMCSD_CHANGE_PENDING);
    mutex->nesting--;
    return RT_EOK;
}

rt_err_t rt_mutex_detach(struct rt_mutex *mutex)
{
    assert(mutex->nesting == 0);
    return RT_EOK;
}

rt_err_t rt_sem_init(struct rt_semaphore *sem, const char *name, int value, int flag)
{
    (void)name;
    (void)flag;
    sem->value = value;
    return RT_EOK;
}

rt_err_t rt_sem_take(struct rt_semaphore *sem, int timeout)
{
    (void)timeout;
    assert(sem->value > 0);
    sem->value--;
    return RT_EOK;
}

rt_err_t rt_sem_release(struct rt_semaphore *sem) { sem->value++; return RT_EOK; }
rt_err_t rt_sem_detach(struct rt_semaphore *sem) { (void)sem; return RT_EOK; }

rt_err_t rt_thread_init(struct rt_thread *thread, const char *name,
                        void (*entry)(void *), void *param, void *stack,
                        unsigned size, int priority, int tick)
{
    (void)entry; (void)param; (void)stack; (void)size; (void)priority; (void)tick;
    strncpy(thread->name, name, sizeof(thread->name) - 1);
    return RT_EOK;
}

rt_err_t rt_thread_startup(struct rt_thread *thread) { (void)thread; return RT_EOK; }
void rt_thread_delay(unsigned ticks) { (void)ticks; assert(!irq_level); }
void rt_sdio_init(void) {}
void mmcsd_test_log(const char *format, ...) { (void)format; warnings++; }
int __rt_ffs(int value)
{
    int bit = 1;
    if (!value) return 0;
    while (!(value & 1)) { value >>= 1; bit++; }
    return bit;
}

static void fake_request(struct rt_mmcsd_host *host, struct rt_mmcsd_req *req)
{
    req->cmd->err = RT_EOK;
    mmcsd_req_complete(host);
}

static void fake_set_iocfg(struct rt_mmcsd_host *host, struct rt_mmcsd_io_cfg *cfg)
{
    (void)cfg;
    assert(host->bus_lock.nesting > 0);
}

static const struct rt_mmcsd_host_ops fake_ops = {fake_request, fake_set_iocfg};

static rt_err_t probe(struct rt_mmcsd_host *host, unsigned kind, rt_uint32_t *ocr)
{
    struct card_fixture *fixture = host->private_data;
    fixture->probes++;
    assert(mmcsd_change_state_get(host) == MMCSD_CHANGE_PENDING);
    assert(host->bus_lock.nesting > 0);
    assert(mmcsd_change_request(host) == -RT_EBUSY);
    fixture->busy_in_probe++;
    *ocr = host->valid_ocr;
    return (fixture->responds & (1u << kind)) ? RT_EOK : -RT_ERROR;
}

rt_err_t mmcsd_send_if_cond(struct rt_mmcsd_host *host, rt_uint32_t ocr)
{
    (void)host; (void)ocr; return RT_EOK;
}
rt_err_t mmcsd_send_app_op_cond(struct rt_mmcsd_host *host, rt_uint32_t ocr, rt_uint32_t *rocr)
{
    (void)ocr; return probe(host, MMCSD_HOST_DETECT_SDCARD, rocr);
}
rt_err_t mmc_send_op_cond(struct rt_mmcsd_host *host, rt_uint32_t ocr, rt_uint32_t *rocr)
{
    (void)ocr; return probe(host, MMCSD_HOST_DETECT_EMMC, rocr);
}
rt_int32_t sdio_io_send_op_cond(struct rt_mmcsd_host *host, rt_uint32_t ocr, rt_uint32_t *rocr)
{
    (void)ocr; return probe(host, MMCSD_HOST_DETECT_SDIO, rocr);
}

static rt_int32_t init_card(struct rt_mmcsd_host *host, unsigned sdio_functions)
{
    struct card_fixture *fixture = host->private_data;
    if (fixture->init_error)
        return fixture->init_error;
    host->card = calloc(1, sizeof(*host->card));
    assert(host->card);
    host->card->host = host;
    host->card->sdio_function_num = (rt_uint8_t)sdio_functions;
    return RT_EOK;
}
rt_int32_t init_sd(struct rt_mmcsd_host *host, rt_uint32_t ocr)
{
    (void)ocr; return init_card(host, 0);
}
rt_int32_t init_mmc(struct rt_mmcsd_host *host, rt_uint32_t ocr)
{
    (void)ocr; return init_card(host, 0);
}
rt_int32_t init_sdio(struct rt_mmcsd_host *host, rt_uint32_t ocr)
{
    (void)ocr; return init_card(host, 1);
}
void rt_mmcsd_blk_remove(struct rt_mmcsd_card *card)
{
    struct card_fixture *fixture = card->host->private_data;
    assert(mmcsd_change_state_get(card->host) == MMCSD_CHANGE_PENDING);
    fixture->removed++;
}

static struct rt_mmcsd_host *new_host(struct card_fixture *fixture)
{
    struct rt_mmcsd_host *host = mmcsd_alloc_host();
    assert(host);
    assert(mmcsd_change_state_get(host) == MMCSD_CHANGE_NONE);
    memset(fixture, 0, sizeof(*fixture));
    fixture->responds = 1u << MMCSD_HOST_DETECT_SDCARD;
    host->ops = &fake_ops;
    host->valid_ocr = 0x00ffff80;
    host->freq_min = 400000;
    host->private_data = fixture;
    return host;
}

static void delete_host(struct rt_mmcsd_host *host)
{
    assert(mmcsd_change_state_get(host) != MMCSD_CHANGE_PENDING);
    free(host->card);
    mmcsd_free_host(host);
}

static void expect_done(struct rt_mmcsd_host *host)
{
    assert(host->bus_lock.nesting == 0);
    assert(mmcsd_change_state_get(host) == MMCSD_CHANGE_DONE);
}

static void test_boot_duplicate_and_remove(void)
{
    struct card_fixture fixture;
    struct rt_mmcsd_host *host = new_host(&fixture);
    assert(mmcsd_change_request(NULL) == -RT_EINVAL);
    mmcsd_change(host); /* Same entry used by the SDK boot drivers. */
    assert(mmcsd_change_state_get(host) == MMCSD_CHANGE_PENDING);
    assert(mmcsd_change_request(host) == -RT_EBUSY);
    mmcsd_change(host);
    assert(mmcsd_detect_mb.entry == 1);
    mmcsd_set_stat(1);
    assert(mmcsd_change_state_get(host) == MMCSD_CHANGE_PENDING);
    unlock_watch = host;
    run_detector(1);
    expect_done(host);
    assert(host->card && fixture.busy_in_probe == 1 && mmcsd_get_stat() == 1);
    assert(mmcsd_wait_cd_changed(0) == MMCSD_HOST_PLUGED);
    mmcsd_change(host);
    run_detector(1);
    expect_done(host);
    assert(!host->card && fixture.removed == 1 && mmcsd_get_stat() == 0);
    assert(mmcsd_wait_cd_changed(0) == MMCSD_HOST_UNPLUGED);
    unlock_watch = NULL;
    delete_host(host);
}

static void test_mailbox_failure_and_host_isolation(void)
{
    struct card_fixture fixtures[5];
    struct rt_mmcsd_host *hosts[5];
    unsigned i;
    for (i = 0; i < 5; i++) hosts[i] = new_host(&fixtures[i]);
    for (i = 0; i < 4; i++) assert(mmcsd_change_request(hosts[i]) == RT_EOK);
    assert(mmcsd_change_request(hosts[4]) == -RT_EFULL);
    assert(mmcsd_change_state_get(hosts[4]) == MMCSD_CHANGE_NONE);
    receive_error_once = 1;
    run_detector(1);
    expect_done(hosts[0]);
    for (i = 1; i < 4; i++) assert(mmcsd_change_state_get(hosts[i]) == MMCSD_CHANGE_PENDING);
    assert(mmcsd_change_request(hosts[4]) == RT_EOK);
    assert(mmcsd_change_request(hosts[0]) == -RT_EFULL);
    expect_done(hosts[0]);
    send_error_once = -RT_ERROR;
    assert(mmcsd_change_request(hosts[0]) == -RT_ERROR);
    expect_done(hosts[0]);
    run_detector(4);
    for (i = 0; i < 5; i++) expect_done(hosts[i]);
    /* A full legacy notification mailbox must not prevent completion. */
    assert(mmcsd_hotpluge_mb.entry == 4);
    assert(mmcsd_change_request(hosts[0]) == RT_EOK);
    run_detector(1);
    expect_done(hosts[0]);
    assert(!hosts[0]->card);
    while (mmcsd_wait_cd_changed(0) >= 0) {}
    for (i = 0; i < 5; i++) delete_host(hosts[i]);
}

static void test_detection_end_paths(void)
{
    struct card_fixture fixture;
    struct rt_mmcsd_host *host;
    unsigned preferred, response;
    int fail;
    for (preferred = 0; preferred < 4; preferred++)
    {
        for (response = 0; response < 4; response++)
        {
            for (fail = 0; fail < 2; fail++)
            {
                host = new_host(&fixture);
                host->flags = preferred << MMCSD_HOST_TYPE_POS;
                fixture.responds = response ? 1u << response : 0;
                fixture.init_error = fail ? -RT_ERROR : 0;
                unlock_watch = host;
                assert(mmcsd_change_request(host) == RT_EOK);
                run_detector(1);
                expect_done(host);
                assert(mmcsd_get_stat() == 1);
                assert((host->card != NULL) == (response != 0 && !fail));
                assert(fixture.probes > 0 && fixture.probes <= 3);
                if (!response) assert(fixture.probes == 3);
                if (host->card)
                {
                    assert(mmcsd_change_request(host) == RT_EOK);
                    run_detector(1);
                    expect_done(host);
                    assert(mmcsd_get_stat() == 0);
                    /* The SDK does not remove SDIO functions on a change request. */
                    assert((host->card != NULL) == (response == MMCSD_HOST_DETECT_SDIO));
                }
                while (mmcsd_wait_cd_changed(0) >= 0) {}
                unlock_watch = NULL;
                delete_host(host);
            }
        }
    }
}

static void interrupt_duplicate(void)
{
    assert(mmcsd_change_state_get(hook_host) == MMCSD_CHANGE_PENDING);
    assert(mmcsd_change_request(hook_host) == -RT_EBUSY);
}

static void complete_before_send_returns(void)
{
    run_detector(1);
    expect_done(hook_host);
}

static void complete_and_queue_next(void)
{
    complete_before_send_returns();
    assert(mmcsd_change_request(hook_host) == RT_EOK);
}

static void test_interleaved_submission(void)
{
    struct card_fixture fixture;
    hook_host = new_host(&fixture);
    irq_hook = interrupt_duplicate;
    send_hook = complete_before_send_returns;
    assert(mmcsd_change_request(hook_host) == RT_EOK);
    expect_done(hook_host);
    assert(hook_host->card);
    send_hook = complete_and_queue_next;
    assert(mmcsd_change_request(hook_host) == RT_EOK);
    assert(mmcsd_change_state_get(hook_host) == MMCSD_CHANGE_PENDING);
    assert(!hook_host->card && mmcsd_detect_mb.entry == 1);
    run_detector(1);
    expect_done(hook_host);
    assert(hook_host->card);
    while (mmcsd_wait_cd_changed(0) >= 0) {}
    delete_host(hook_host);
    hook_host = NULL;
}

int main(void)
{
    assert(rt_mmcsd_core_init() == RT_EOK);
    test_boot_duplicate_and_remove();
    puts("PASS: boot adoption, duplicate rejection, legacy entry, ordered removal");
    test_mailbox_failure_and_host_isolation();
    puts("PASS: mailbox full/error rollback, retry, host isolation, notification failure");
    test_detection_end_paths();
    puts("PASS: 32 probe configurations, init failures and SDIO removal refusal");
    test_interleaved_submission();
    puts("PASS: interrupt interleaving and detector completion before send returns");
    assert(warnings > 0 && mmcsd_detect_mb.entry == 0 && irq_level == 0);
    puts("mmcsd_change tests: PASS");
    return 0;
}

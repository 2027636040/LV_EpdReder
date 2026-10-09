/* Exercise the production adapter; the board compile checks actual SDK types. */
#include <stdio.h>
#include "../port/sdcard_sifli_sdio.c"

static struct rt_mmcsd_host host = {MMCSD_HOST_TYPE_SDCARD, "sd0", NULL};
static struct rt_mmcsd_card card = {CARD_TYPE_SD, 0};
static struct rt_device device = {RT_Device_Class_Block};
static struct rt_mmcsd_host *candidate = &host;
static enum mmcsd_change_state change;
static int interrupt_level, finds, result, device_present = 1;

rt_base_t rt_hw_interrupt_disable(void) { return interrupt_level++; }
void rt_hw_interrupt_enable(rt_base_t level)
{
    assert(interrupt_level == level + 1);
    interrupt_level = (int)level;
}
struct rt_mmcsd_host *sifli_sdio_sdcard_get_host(void) { return candidate; }
enum mmcsd_change_state mmcsd_change_state_get(struct rt_mmcsd_host *value)
{
    assert(value == &host && interrupt_level);
    return change;
}
rt_err_t mmcsd_change_request(struct rt_mmcsd_host *value)
{
    assert(value == &host);
    return result;
}
rt_device_t rt_device_find(const char *name)
{
    assert(!interrupt_level && strcmp(name, "sd0") == 0);
    ++finds;
    return device_present ? &device : RT_NULL;
}
int main(void)
{
    struct rt_mmcsd_host *bound = NULL;
    sdcard_port_status_t status;
    assert(sdcard_port_bind("sd0", &bound) == RT_EOK && bound == &host);
    candidate = NULL;
    assert(sdcard_port_bind("sd0", &bound) == -RT_ENOSYS);
    candidate = &host;
    host.flags = 2;
    assert(sdcard_port_bind("sd0", &bound) == -RT_EINVAL);
    host.flags = MMCSD_HOST_TYPE_SDCARD;
    assert(sdcard_port_bind("sd1", &bound) == -RT_EINVAL);
    change = MMCSD_CHANGE_PENDING;
    host.card = (struct rt_mmcsd_card *)(uintptr_t)1; /* Must never be dereferenced. */
    sdcard_port_get_status(&host, &status);
    assert(status.change == SDCARD_CHANGE_PENDING && !status.has_card && !finds);
    change = MMCSD_CHANGE_DONE;
    host.card = NULL;
    sdcard_port_get_status(&host, &status);
    assert(status.change == SDCARD_CHANGE_DONE && !status.has_card);
    host.card = &card;
    sdcard_port_get_status(&host, &status);
    assert(status.has_card && status.sd_memory && status.block_device);
    card.sdio_function_num = 1;
    sdcard_port_get_status(&host, &status);
    assert(status.has_card && !status.sd_memory);
    card.sdio_function_num = 0; card.card_type = 0;
    sdcard_port_get_status(&host, &status);
    assert(!status.sd_memory);
    device.type = 0;
    sdcard_port_get_status(&host, &status);
    assert(!status.block_device);
    device_present = 0;
    sdcard_port_get_status(&host, &status);
    assert(!status.block_device);
    change = MMCSD_CHANGE_NONE;
    sdcard_port_get_status(&host, &status);
    assert(status.change == SDCARD_CHANGE_NONE);
    result = -RT_EFULL;
    assert(sdcard_port_request(&host) == -RT_EFULL);
    result = RT_EOK;
    assert(sdcard_port_request(&host) == RT_EOK);
    assert(!interrupt_level);
    puts("sdcard adapter: host selection, pending access exclusion, card/device result and request forwarding passed");
    return 0;
}

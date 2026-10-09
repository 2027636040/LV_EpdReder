#ifndef SDCARD_TEST_MMCSD_H
#define SDCARD_TEST_MMCSD_H
#include <rtthread.h>
#define MMCSD_HOST_TYPE_MASK 3
#define MMCSD_HOST_TYPE_SDCARD 1
#define CARD_TYPE_SD 1
enum mmcsd_change_state { MMCSD_CHANGE_NONE, MMCSD_CHANGE_PENDING, MMCSD_CHANGE_DONE };
struct rt_mmcsd_card { int card_type; unsigned sdio_function_num; };
struct rt_mmcsd_host
{
    unsigned flags;
    char name[16];
    struct rt_mmcsd_card *card;
};
enum mmcsd_change_state mmcsd_change_state_get(struct rt_mmcsd_host *host);
rt_err_t mmcsd_change_request(struct rt_mmcsd_host *host);
#endif

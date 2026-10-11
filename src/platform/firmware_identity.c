/* SPDX-License-Identifier: Apache-2.0 */
#include <rtthread.h>

extern const rt_uint32_t __firmware_build_id_start__[];
extern const rt_uint8_t __firmware_build_id_end__[];

static int firmware_id(void)
{
    static const char hex[] = "0123456789abcdef";
    const rt_uint32_t *note = __firmware_build_id_start__;
    const rt_uint8_t *bytes = (const rt_uint8_t *)note;
    char identity[41];

    /* The linker emits a GNU SHA-1 note: header, "GNU\0", 20-byte ID. */
    if (__firmware_build_id_end__ - bytes != 36 ||
        note[0] != 4 || note[1] != 20 || note[2] != 3 ||
        rt_memcmp(bytes + 12, "GNU", 4) != 0)
    {
        rt_kprintf("[firmware] build_id=unavailable\n");
        return -RT_ERROR;
    }
    for (unsigned i = 0; i < 20; ++i)
    {
        identity[2 * i] = hex[bytes[16 + i] >> 4];
        identity[2 * i + 1] = hex[bytes[16 + i] & 15];
    }
    identity[40] = '\0';
    rt_kprintf("[firmware] build_id=%s\n", identity);
    return RT_EOK;
}
INIT_APP_EXPORT(firmware_id);
MSH_CMD_EXPORT(firmware_id, Show firmware GNU build identity);

/* SPDX-License-Identifier: Apache-2.0 */
#include "rtconfig.h"

#ifdef BSP_USING_RTTHREAD
#include "board_ioex.h"
#include <rthw.h>
#include <rtdevice.h>
#include <drivers/pin.h>

#define IOEX_ADDR       0x59 /* AD1=0, AD0=1, seven-bit address */
#define IOEX_IRQ_PIN    21
#define IOEX_TF_BIT     (1U << 1)
#define REG_INPUT0     0x00
#define REG_INPUT1     0x01
#define REG_OUTPUT0    0x02
#define REG_CONFIG0    0x04
#define REG_CONFIG1    0x05
#define REG_INT0       0x06
#define REG_INT1       0x07
#define REG_ID         0x10
#define REG_CONTROL    0x11
#define REG_MODE0      0x12
#define REG_MODE1      0x13

static struct rt_i2c_bus_device *bus;
static struct rt_device_pin *pin_device;
static const struct rt_pin_ops *native_ops;
static struct rt_pin_ops board_ops;
static struct rt_mutex io_lock;
static struct rt_semaphore changed;
static rt_thread_t worker;
static rt_bool_t ready;
static rt_bool_t initialized;
static volatile int sd_level = PIN_HIGH;
static void (*sd_handler)(void *);
static void *sd_argument;
static rt_bool_t sd_irq_enabled;

static rt_err_t read_reg(rt_uint8_t reg, rt_uint8_t *value)
{
    struct rt_i2c_msg messages[2] = {
        {.addr = IOEX_ADDR, .flags = RT_I2C_WR, .len = 1, .buf = &reg},
        {.addr = IOEX_ADDR, .flags = RT_I2C_RD, .len = 1, .buf = value},
    };
    return rt_i2c_transfer(bus, messages, 2) == 2 ? RT_EOK : -RT_EIO;
}

static rt_err_t write_reg(rt_uint8_t reg, rt_uint8_t value)
{
    rt_uint8_t data[2] = {reg, value};
    struct rt_i2c_msg message = {
        .addr = IOEX_ADDR, .flags = RT_I2C_WR, .len = sizeof(data), .buf = data,
    };
    return rt_i2c_transfer(bus, &message, 1) == 1 ? RT_EOK : -RT_EIO;
}

/* Both ports must be read separately to acknowledge AW9523B INTN. */
static rt_err_t read_inputs(int *level)
{
    rt_uint8_t p0, p1;
    rt_err_t result;
    rt_mutex_take(&io_lock, RT_WAITING_FOREVER);
    result = read_reg(REG_INPUT0, &p0);
    if (result == RT_EOK)
        result = read_reg(REG_INPUT1, &p1);
    rt_mutex_release(&io_lock);
    if (result == RT_EOK)
        *level = (p0 & IOEX_TF_BIT) ? PIN_HIGH : PIN_LOW;
    return result;
}

static void publish_level(int level)
{
    rt_base_t lock = rt_hw_interrupt_disable();
    int previous = sd_level;
    sd_level = level;
    if (previous != level && sd_irq_enabled && sd_handler)
        sd_handler(sd_argument);
    rt_hw_interrupt_enable(lock);
}

static void ioex_irq(void *argument)
{
    (void)argument;
    native_ops->pin_irq_enable(&pin_device->parent, IOEX_IRQ_PIN, PIN_IRQ_DISABLE);
    /* Only TF input interrupts are enabled. Revoke access before deferred I2C.
     * The SD service debounces the actual level after the worker samples it. */
    publish_level(PIN_HIGH);
    rt_sem_release(&changed);
}

static void ioex_worker(void *argument)
{
    (void)argument;
    for (;;)
    {
        int level;
        rt_sem_take(&changed, RT_WAITING_FOREVER);
        while (read_inputs(&level) != RT_EOK)
        {
            publish_level(PIN_HIGH);
            rt_thread_mdelay(100);
        }
        publish_level(level);
        native_ops->pin_irq_enable(&pin_device->parent, IOEX_IRQ_PIN, PIN_IRQ_ENABLE);
        /* Cover a new edge between the register read and IRQ re-enable. */
        if (native_ops->pin_read(&pin_device->parent, IOEX_IRQ_PIN) == PIN_LOW)
            ioex_irq(RT_NULL);
    }
}

static void board_pin_mode(struct rt_device *device, rt_base_t pin, rt_base_t mode)
{
    if (pin != BOARD_IOEX_SD_PIN)
        native_ops->pin_mode(device, pin, mode);
    /* P0.1 is permanently an input with the board's external pull-up. */
}

static int board_pin_read(struct rt_device *device, rt_base_t pin)
{
    if (pin == BOARD_IOEX_SD_PIN)
        return sd_level; /* Also safe inside the SD service's IRQ critical section. */
    return native_ops->pin_read(device, pin);
}

static void board_pin_write(struct rt_device *device, rt_base_t pin, rt_base_t value)
{
    if (pin != BOARD_IOEX_SD_PIN)
        native_ops->pin_write(device, pin, value);
}

static rt_err_t board_pin_attach(struct rt_device *device, rt_int32_t pin,
                                rt_uint32_t mode, void (*handler)(void *), void *argument)
{
    rt_base_t lock;
    if (pin != BOARD_IOEX_SD_PIN)
        return native_ops->pin_attach_irq(device, pin, mode, handler, argument);
    if (mode != PIN_IRQ_MODE_RISING_FALLING || !handler)
        return -RT_EINVAL;
    lock = rt_hw_interrupt_disable();
    if (sd_handler)
    {
        rt_hw_interrupt_enable(lock);
        return -RT_EBUSY;
    }
    sd_handler = handler;
    sd_argument = argument;
    rt_hw_interrupt_enable(lock);
    return RT_EOK;
}

static rt_err_t board_pin_detach(struct rt_device *device, rt_int32_t pin)
{
    rt_base_t lock;
    if (pin != BOARD_IOEX_SD_PIN)
        return native_ops->pin_detach_irq(device, pin);
    lock = rt_hw_interrupt_disable();
    sd_irq_enabled = RT_FALSE;
    sd_handler = RT_NULL;
    sd_argument = RT_NULL;
    rt_hw_interrupt_enable(lock);
    return RT_EOK;
}

static rt_err_t board_pin_irq_enable(struct rt_device *device, rt_base_t pin,
                                    rt_uint32_t enabled)
{
    rt_base_t lock;
    if (pin != BOARD_IOEX_SD_PIN)
        return native_ops->pin_irq_enable(device, pin, enabled);
    if (!ready)
        return -RT_EIO;
    lock = rt_hw_interrupt_disable();
    sd_irq_enabled = enabled == PIN_IRQ_ENABLE;
    rt_hw_interrupt_enable(lock);
    return RT_EOK;
}

rt_err_t board_ioex_write(unsigned pin, int value)
{
    rt_uint8_t output;
    rt_uint8_t reg;
    rt_uint8_t bit;
    rt_err_t result;
    if (!ready)
        return -RT_EIO;
    if (pin >= 16 || pin < 4)
        return -RT_EINVAL;
    reg = (rt_uint8_t)(REG_OUTPUT0 + pin / 8);
    bit = (rt_uint8_t)(1U << (pin % 8));
    rt_mutex_take(&io_lock, RT_WAITING_FOREVER);
    result = read_reg(reg, &output);
    if (result == RT_EOK)
        result = write_reg(reg, value ? output | bit : output & ~bit);
    rt_mutex_release(&io_lock);
    return result;
}

static int board_ioex_init(void)
{
    rt_uint8_t id;
    int level;
    rt_err_t result;
    if (initialized)
        return ready ? RT_EOK : -RT_EIO;
    pin_device = (struct rt_device_pin *)rt_device_find("pin");
    if (!pin_device)
        return -RT_ENOSYS;
    initialized = RT_TRUE;
    native_ops = pin_device->ops;
    board_ops = *native_ops;
    board_ops.pin_mode = board_pin_mode;
    board_ops.pin_read = board_pin_read;
    board_ops.pin_write = board_pin_write;
    board_ops.pin_attach_irq = board_pin_attach;
    board_ops.pin_detach_irq = board_pin_detach;
    board_ops.pin_irq_enable = board_pin_irq_enable;
    pin_device->ops = &board_ops;

    bus = rt_i2c_bus_device_find("i2c1");
    if (!bus)
        return -RT_ENOSYS;
    result = rt_device_open((rt_device_t)bus,
                           RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_INT_TX | RT_DEVICE_FLAG_INT_RX);
    if (result != RT_EOK)
        return result;
    rt_mutex_init(&io_lock, "ioex", RT_IPC_FLAG_PRIO);
    rt_sem_init(&changed, "ioex", 0, RT_IPC_FLAG_FIFO);
    rt_thread_mdelay(5);
    result = read_reg(REG_ID, &id);
    if (result != RT_EOK || id != 0x23)
        goto fail;

    /* Apply AD1=0/AD0=1 reset output values: P0=0, P1=0x0f.
     * P0.0..3 stay inputs; all other pins stay GPIO outputs. */
    if (write_reg(REG_INT0, 0xff) != RT_EOK ||
        write_reg(REG_INT1, 0xff) != RT_EOK ||
        write_reg(REG_MODE0, 0xff) != RT_EOK ||
        write_reg(REG_MODE1, 0xff) != RT_EOK ||
        write_reg(REG_OUTPUT0, 0x00) != RT_EOK ||
        write_reg(REG_OUTPUT0 + 1, 0x0f) != RT_EOK ||
        write_reg(REG_CONFIG0, 0x0f) != RT_EOK ||
        write_reg(REG_CONFIG1, 0x00) != RT_EOK ||
        write_reg(REG_CONTROL, 0x13) != RT_EOK ||
        read_inputs(&level) != RT_EOK)
        goto fail;
    sd_level = level;
    native_ops->pin_mode(&pin_device->parent, IOEX_IRQ_PIN, PIN_MODE_INPUT_PULLUP);
    result = native_ops->pin_attach_irq(&pin_device->parent, IOEX_IRQ_PIN,
                                        PIN_IRQ_MODE_FALLING, ioex_irq, RT_NULL);
    if (result != RT_EOK)
        goto fail;
    worker = rt_thread_create("ioex", ioex_worker, RT_NULL, 1536, 18, 10);
    if (!worker)
        goto detach;
    if (write_reg(REG_INT0, 0xffU ^ IOEX_TF_BIT) != RT_EOK)
        goto delete_worker;
    ready = RT_TRUE;
    rt_thread_startup(worker);
    /* The initial sample also starts IRQ monitoring without an edge race. */
    rt_sem_release(&changed);
    rt_kprintf("[ioex] AW9523B ready, TF detect=129 (P0.1), present=%d\n", !sd_level);
    return RT_EOK;

delete_worker:
    rt_thread_delete(worker);
    worker = RT_NULL;
detach:
    native_ops->pin_detach_irq(&pin_device->parent, IOEX_IRQ_PIN);
fail:
    sd_level = PIN_HIGH;
    rt_sem_detach(&changed);
    rt_mutex_detach(&io_lock);
    rt_device_close((rt_device_t)bus);
    return -RT_EIO;
}
/* I2C is registered at BOARD; SDIO samples detect at DEVICE. Touch reset also
 * uses this expander, independently of whether the SD service is enabled. */
INIT_PREV_EXPORT(board_ioex_init);
#endif

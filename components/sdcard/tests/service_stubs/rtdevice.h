#ifndef SDCARD_TEST_RTDEVICE_H
#define SDCARD_TEST_RTDEVICE_H
#include <rtthread.h>
#define SD_INSERT_DETECT_PIN 11
#define PIN_LOW 0
#define PIN_HIGH 1
#define PIN_MODE_INPUT 1
#define PIN_IRQ_MODE_RISING_FALLING 2
#define PIN_IRQ_ENABLE 1
#define PIN_IRQ_DISABLE 0
#define RT_Device_Class_Block 2
struct rt_device { int type; };
typedef struct rt_device *rt_device_t;
rt_device_t rt_device_find(const char *name);
int rt_pin_read(rt_base_t pin);
void rt_pin_mode(rt_base_t pin, rt_base_t mode);
rt_err_t rt_pin_attach_irq(rt_base_t pin, int mode, void (*handler)(void *), void *arg);
rt_err_t rt_pin_detach_irq(rt_base_t pin);
rt_err_t rt_pin_irq_enable(rt_base_t pin, int enabled);
#endif

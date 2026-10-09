#include <rtthread.h>
#include <rtdevice.h>
#define PAD_PA00 0
#define GPIO_A0 100
#define PIN_PULLUP 1
void HAL_PIN_Set(int pad, int function, int pull, int hcpu);

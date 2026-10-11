#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BSP_USING_RTTHREAD 1
#define INIT_PREV_EXPORT(fn)
#define BOARD_IOEX_SD_PIN 129
#define RT_NULL NULL
#define RT_TRUE 1
#define RT_FALSE 0
#define RT_EOK 0
#define RT_EIO 5
#define RT_ENOSYS 38
#define RT_EINVAL 22
#define RT_EBUSY 16
#define RT_WAITING_FOREVER -1
#define RT_IPC_FLAG_PRIO 1
#define RT_IPC_FLAG_FIFO 0
#define RT_DEVICE_FLAG_RDWR 3
#define RT_DEVICE_FLAG_INT_TX 4
#define RT_DEVICE_FLAG_INT_RX 8
#define RT_I2C_WR 0
#define RT_I2C_RD 1
#define PIN_HIGH 1
#define PIN_LOW 0
#define PIN_IRQ_DISABLE 0
#define PIN_IRQ_ENABLE 1
#define PIN_IRQ_MODE_FALLING 1
#define PIN_IRQ_MODE_RISING_FALLING 2
#define PIN_MODE_INPUT_PULLUP 2
typedef int rt_err_t, rt_base_t, rt_bool_t, rt_int32_t;
typedef unsigned rt_uint32_t;
typedef uint8_t rt_uint8_t;
typedef void *rt_thread_t;
struct rt_device { int unused; };
typedef struct rt_device *rt_device_t;
struct rt_i2c_bus_device { struct rt_device parent; };
struct rt_mutex { int unused; };
struct rt_semaphore { int unused; };
struct rt_i2c_msg { uint16_t addr, mem_addr, mem_addr_size, flags, len; uint8_t *buf; };
struct rt_pin_ops {
    void (*pin_mode)(rt_device_t, int, int);
    void (*pin_write)(rt_device_t, int, int);
    int (*pin_read)(rt_device_t, int);
    int (*pin_attach_irq)(rt_device_t, int, unsigned, void (*)(void *), void *);
    int (*pin_detach_irq)(rt_device_t, int);
    int (*pin_irq_enable)(rt_device_t, int, unsigned);
};
struct rt_device_pin { struct rt_device parent; const struct rt_pin_ops *ops; };
static uint8_t registers[256];
static int critical, bad_read, bad_write, bad_alloc, missing_bus;
static int i2c_calls, reads0, reads1, native_pin, native_value, irq_level = 1;
static int worker_waits, output_events, output_values[8], closed, deleted;
static jmp_buf worker_done;
static struct rt_i2c_bus_device mock_bus;
static struct rt_device_pin mock_pin;
static void native_mode(rt_device_t d, int p, int v) { (void)d; native_pin=p; native_value=v; }
static void native_write(rt_device_t d, int p, int v) { native_mode(d,p,v); }
static int native_read(rt_device_t d, int p) { (void)d; native_pin=p; return irq_level; }
static int native_attach(rt_device_t d,int p,unsigned m,void (*h)(void *),void *a)
{ (void)d;(void)m;(void)h;(void)a;native_pin=p;return 0; }
static int native_detach(rt_device_t d,int p) { (void)d;native_pin=p;return 0; }
static int native_enable(rt_device_t d,int p,unsigned e) { (void)d;native_pin=p;native_value=(int)e;return 0; }
static const struct rt_pin_ops mock_ops={native_mode,native_write,native_read,native_attach,native_detach,native_enable};
static int rt_hw_interrupt_disable(void) { int old=critical;critical=1;return old; }
static void rt_hw_interrupt_enable(int old) { critical=old; }
static rt_device_t rt_device_find(const char *n) { assert(!strcmp(n,"pin"));return &mock_pin.parent; }
static struct rt_i2c_bus_device *rt_i2c_bus_device_find(const char *n)
{ assert(!strcmp(n,"i2c1"));return missing_bus ? NULL : &mock_bus; }
static int rt_device_open(rt_device_t d,int f) { (void)d;(void)f;return 0; }
static int rt_device_close(rt_device_t d) { (void)d;++closed;return 0; }
static void rt_mutex_init(struct rt_mutex *m,const char *n,int f) { (void)m;(void)n;(void)f; }
static void rt_mutex_take(struct rt_mutex *m,int t) { (void)m;(void)t;assert(!critical); }
static void rt_mutex_release(struct rt_mutex *m) { (void)m; }
static void rt_mutex_detach(struct rt_mutex *m) { (void)m; }
static void rt_sem_init(struct rt_semaphore *s,const char *n,int v,int f) { (void)s;(void)n;(void)v;(void)f; }
static void rt_sem_detach(struct rt_semaphore *s) { (void)s; }
static void rt_sem_release(struct rt_semaphore *s) { (void)s; }
static void rt_sem_take(struct rt_semaphore *s,int t)
{ (void)s;(void)t;if (++worker_waits > 1) longjmp(worker_done,1); }
static void rt_thread_mdelay(int t) { (void)t; }
static rt_thread_t rt_thread_create(const char *n,void (*e)(void *),void *a,int s,int p,int t)
{ (void)n;(void)e;(void)a;(void)s;(void)p;(void)t;return bad_alloc ? NULL : (void *)1; }
static void rt_thread_startup(rt_thread_t t) { assert(t); }
static void rt_thread_delete(rt_thread_t t) { assert(t);++deleted; }
static void rt_kprintf(const char *f,...) { (void)f; }
static unsigned rt_i2c_transfer(struct rt_i2c_bus_device *b,struct rt_i2c_msg *m,unsigned count)
{
    uint8_t reg=m[0].buf[0];
    assert(b==&mock_bus && !critical && m[0].addr==0x59);
    ++i2c_calls;
    if (count==2) {
        assert(m[0].len==1 && m[1].len==1 && m[1].flags==RT_I2C_RD);
        if(bad_read) return 0;
        *m[1].buf=registers[reg];
        if(reg==0) ++reads0;
        if(reg==1) ++reads1;
    } else {
        assert(count==1 && m[0].len==2);
        if(bad_write) return 0;
        registers[reg]=m[0].buf[1];
    }
    return count;
}
#include "ioex_under_test.h"

static void sd_callback(void *arg)
{
    assert(arg==(void *)2 && critical);
    assert(output_events < 8);
    output_values[output_events++]=board_pin_read(&mock_pin.parent,129);
}
static void run_worker_once(void)
{
    worker_waits=0;
    if(setjmp(worker_done)==0) ioex_worker(NULL);
}
int main(int argc,char **argv)
{
    int before;
    assert(argc==2);
    mock_pin.ops=&mock_ops;
    registers[REG_ID]=!strcmp(argv[1],"id") ? 0 : 0x23;
    registers[REG_INPUT0]=0; /* A card is present at boot. */
    bad_read=!strcmp(argv[1],"read");bad_write=!strcmp(argv[1],"write");
    bad_alloc=!strcmp(argv[1],"alloc");missing_bus=!strcmp(argv[1],"bus");
    if(strcmp(argv[1],"normal")) {
        assert(board_ioex_init()!=0);
        assert(board_pin_read(&mock_pin.parent,129)==PIN_HIGH);
        assert(board_pin_irq_enable(&mock_pin.parent,129,PIN_IRQ_ENABLE)!=0);
        assert(!ready && board_ioex_init()!=0);
        if(!missing_bus) assert(closed==1);
        printf("AW9523B failure cleanup %s: PASS\n",argv[1]);return 0;
    }
    assert(board_ioex_init()==0 && ready);
    assert(registers[REG_CONFIG0]==0x0f && registers[REG_CONFIG1]==0);
    assert(registers[REG_INT0]==0xfd && registers[REG_INT1]==0xff);
    assert(registers[REG_CONTROL]==0x13 && reads0==1 && reads1==1);
    before=i2c_calls;assert(board_ioex_init()==0 && i2c_calls==before);
    assert(board_ioex_write(14,1)==0 && registers[3]==0x4f);
    assert(board_ioex_write(14,0)==0 && registers[3]==0x0f);
    assert(board_ioex_write(1,1)==-RT_EINVAL);
    board_pin_mode(&mock_pin.parent,34,2);assert(native_pin==34);
    board_pin_write(&mock_pin.parent,24,1);assert(native_pin==24 && native_value==1);
    assert(board_pin_read(&mock_pin.parent,20)==1 && native_pin==20);
    assert(board_pin_attach(&mock_pin.parent,129,PIN_IRQ_MODE_RISING_FALLING,sd_callback,(void *)2)==0);
    assert(board_pin_irq_enable(&mock_pin.parent,129,PIN_IRQ_ENABLE)==0);
    before=i2c_calls;critical=1;ioex_irq(NULL);critical=0;
    assert(i2c_calls==before && sd_level==1 && output_events==1 && output_values[0]==1);
    registers[0]=2;run_worker_once();assert(sd_level==1);
    registers[0]=0;critical=1;ioex_irq(NULL);critical=0;run_worker_once();
    assert(sd_level==0 && output_events==2 && output_values[1]==0);
    assert(reads0==3 && reads1==3);
    assert(board_pin_detach(&mock_pin.parent,129)==0);
    critical=1;ioex_irq(NULL);critical=0;assert(output_events==2);
    printf("AW9523B GPIO delegation, touch reset, ISR isolation, remove/insert and detach: PASS\n");
    return 0;
}

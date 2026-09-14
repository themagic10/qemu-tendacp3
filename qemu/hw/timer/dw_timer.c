#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "qemu/log-for-trace.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "qemu/typedefs.h"
#include "qom/object.h"
#include "hw/core/sysbus.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include "hw/core/hw-error.h"
#include "qemu/error-report.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "hw/core/qdev.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/ptimer.h"

#define TYPE_DW_TIMER "dw-timer"
OBJECT_DECLARE_SIMPLE_TYPE(DWTimerState, DW_TIMER)

#define CHANNEL_AMT 2
#define CHANNEL_SIZE 0x14
#define FREQUENCY_HZ 10*1000000

//from the linux driver header
#define APBTMR_N_LOAD_COUNT		0x00
#define APBTMR_N_CURRENT_VALUE		0x04
#define APBTMR_N_CONTROL		0x08
#define APBTMR_N_EOI			0x0c
#define APBTMR_N_INT_STATUS		0x10

//global reg
#define G_TIMERS_INT_STATUS     0xa0
#define G_TIMERS_EOI            0xa4
#define G_TIMERS_RAW_INT_STATUS 0xa8

#define MASKED_INT (1u<<2)
#define CONTROL_USER_DEFINED_MODE (1u<<1)
#define CTRL_EN (1u<<0)

typedef struct TimerChan{
    ptimer_state *ptimer;
    qemu_irq irq;

    uint32_t load;
    uint32_t control;
    bool int_status;
} TimerChan;

struct DWTimerState{
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    TimerChan chan[CHANNEL_AMT];    
};



static void timer_update_irq(TimerChan *c){
    //check if we have non maskable interrupt
    bool active = c->int_status && !(c->control & MASKED_INT);
    qemu_set_irq(c->irq, active);
}
uint hitted = 0;
static void timer_hit(void *opaque){
    hitted++;
    qemu_log_mask(LOG_GUEST_ERROR, "HIT: %u\n", hitted);
    TimerChan *c = opaque;
    c->int_status = true;
    timer_update_irq(c);

    if (!(c->control & CONTROL_USER_DEFINED_MODE)){ //free running mode
        ptimer_set_limit(c->ptimer, 0xFFFFFFFFu, 1);
    }

}

/*
long story short: ptimer will check on start if the current limit is 0 and if so it will disable itself until it's manually enabled again.
this *shouldn't* be an issue but the kernel will first enable channel 2 with load 0 only to set it immediately afterwards. This should result in the
hit function to be called every cycle but ptimer will detect that and refuse to do so.

this function simply checks if the channel we're trying to start has a load time. if it does we start the timer otherwise we hold on and wait
for the load value to be set

*/
static void safe_run_chan(TimerChan *c){
    bool shouldrun = (c->control & CTRL_EN) && c->load != 0;
    if (shouldrun){
        ptimer_run(c->ptimer, 0);
    }
    else{
        ptimer_stop(c->ptimer);
    }
}

static uint64_t dw_timer_reg_read(void *opaque, hwaddr addr, unsigned size){
    DWTimerState *s = opaque;
  
    //global regs
    switch(addr){
        case G_TIMERS_INT_STATUS:{
            uint32_t status = 0;
            for (int i = 0; i < CHANNEL_AMT-1; i++){
                TimerChan *c = &s->chan[i];
                if (c->int_status && !(c->control & MASKED_INT)){
                    status |= (1u<<i);
                }
            }
            return status;
        }

        case G_TIMERS_EOI:{
            for (int i = 0; i < CHANNEL_AMT-1; i++){
                TimerChan *c = &s->chan[i];
                c->int_status = false;
                timer_update_irq(c);
            }

            return 0;
        }
        case G_TIMERS_RAW_INT_STATUS:{
            uint32_t status = 0;
            for (int i = 0; i < CHANNEL_AMT-1; i++){
                TimerChan *c = &s->chan[i];
                if (c->int_status){ // no mask check
                    status |= (1u<<i);
                }
            }
            return status;
        }
        default: break;
    }

    //channel regs
    unsigned channel_index = addr / CHANNEL_SIZE;
    if (channel_index > CHANNEL_AMT-1){
        qemu_log_mask(LOG_GUEST_ERROR, "dw timer: no timer implemented at address %lx\n", addr);
        return 0;
    }
    TimerChan *c = &s->chan[channel_index];
    unsigned reg_offset = addr % CHANNEL_SIZE;
    switch (reg_offset) {
        case APBTMR_N_LOAD_COUNT: return c->load;
        case APBTMR_N_CURRENT_VALUE: return ptimer_get_count(c->ptimer);
        case APBTMR_N_CONTROL: return c->control;
        case APBTMR_N_EOI:
            c->int_status = false;
            timer_update_irq(c);
            return 0;
        case APBTMR_N_INT_STATUS: return c->int_status ? 1 : 0;
        default:
            qemu_log_mask(LOG_GUEST_ERROR, "dw timer: reading from unimplemented register %lx\n", addr);
            return 0;
                
    }
}

static void dw_timer_reg_write(void *opaque, hwaddr addr, uint64_t value, unsigned size){
    DWTimerState *s = opaque;
  
    unsigned channel_index = addr / CHANNEL_SIZE;
    if (channel_index > CHANNEL_AMT-1){
        qemu_log_mask(LOG_GUEST_ERROR, "dw timer: no timer implemented at address %lx\n", addr);
        return;
    }
    TimerChan *c = &s->chan[channel_index];
    unsigned reg_offset = addr % CHANNEL_SIZE;
    switch (reg_offset) {
        case APBTMR_N_LOAD_COUNT:
            c->load = (uint32_t)value;

            ptimer_transaction_begin(c->ptimer);
            ptimer_set_limit(c->ptimer, c->load, 1);
            safe_run_chan(c);
            ptimer_transaction_commit(c->ptimer);
            break;
        case APBTMR_N_CONTROL:
            c->control = (uint32_t)value;

            ptimer_transaction_begin(c->ptimer);
            ptimer_set_limit(c->ptimer, c->load, 1);
            safe_run_chan(c);
            ptimer_transaction_commit(c->ptimer);
            timer_update_irq(c);
            break;

        default:
            qemu_log_mask(LOG_GUEST_ERROR, "dw timer: writing to unimplemented address %lx\n", addr);
            break;
    }
}

static const MemoryRegionOps dw_timer_ops = {
    .read = dw_timer_reg_read,
    .write = dw_timer_reg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void dw_timer_reset(DeviceState *dev){
    DWTimerState *s = DW_TIMER(dev); 
    for (int i = 0; i < CHANNEL_AMT-1; i++) {
        TimerChan *c = &s->chan[i];
 
        c->load = 0;
        c->control = 0;
        c->int_status = false;
 
        ptimer_transaction_begin(c->ptimer);
        ptimer_stop(c->ptimer);
        ptimer_set_count(c->ptimer, 0);
        ptimer_transaction_commit(c->ptimer);
 
        qemu_set_irq(c->irq, 0);
    }

}

static void dw_timer_init(Object *obj){
    DWTimerState *s = DW_TIMER(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &dw_timer_ops, s, "dw-timer", 0x4000);
    sysbus_init_mmio(sbd, &s->iomem);

    for (int i = 0; i< CHANNEL_AMT; i++){
        TimerChan *c = &s->chan[i];
        sysbus_init_irq(sbd, &c->irq);
        c->ptimer = ptimer_init(timer_hit, c, 0);
        ptimer_transaction_begin(c->ptimer);
        ptimer_set_freq(c->ptimer, FREQUENCY_HZ);
        ptimer_transaction_commit(c->ptimer);
    }
}

static void dw_timer_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->legacy_reset = dw_timer_reset;    
    
}

static const TypeInfo dw_timer_info = {
    .name          = TYPE_DW_TIMER,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DWTimerState),
    .instance_init = dw_timer_init,
    .class_init    = dw_timer_class_init,
};

static void dw_timer_register(void)
{

    type_register_static(&dw_timer_info);
}

type_init(dw_timer_register)

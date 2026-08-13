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

#define TYPE_DW_TIMER "dw-timer"
OBJECT_DECLARE_SIMPLE_TYPE(DWTimerState, DW_TIMER)

struct DWTimerState{
    SysBusDevice parent_obj;
    MemoryRegion iomem;

    uint32_t base;
    uint32_t control;

    uint64_t starttime;
    bool running;
    uint32_t rate;
};

static uint32_t eval_timer(DWTimerState *s){
    if (s->running){
        uint64_t curr = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        uint64_t elapsed = curr - s->starttime;
        return (s->base - elapsed);
    }
    return 0;
}

static uint64_t dw_timer_reg_read(void *opaque, hwaddr addr, unsigned size){
    DWTimerState *s = opaque;

    switch (addr) {
        case 0x00: return s->base;
        case 0x04: return eval_timer(s);
        case 0x08: return s->control;
        default:
            qemu_log_mask(LOG_GUEST_ERROR, "dw timer: reading from unimplemented register\n");
            return 0;
    }

}

static void dw_timer_reg_write(void *opaque, hwaddr addr, uint64_t value, unsigned size){
    //nope
}

static const MemoryRegionOps dw_timer_ops = {
    .read = dw_timer_reg_read,
    .write = dw_timer_reg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void dw_timer_realize(DeviceState* dev, Error **errp){
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    DWTimerState *s = DW_TIMER(dev);
    s->starttime = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->running = true;
    s->base = 1000000000;
    s->control = 5;

    memory_region_init_io(&s->iomem, OBJECT(s), &dw_timer_ops, 
        s, "dw-timer", 0x1000); // todo check true size
    sysbus_init_mmio(sbd, &s->iomem);

}

static void dw_timer_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = dw_timer_realize;    
    
}

static const TypeInfo dw_timer_info = {
    .name          = TYPE_DW_TIMER,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DWTimerState),
    .class_init    = dw_timer_class_init,
};

static void dw_timer_register(void)
{

    type_register_static(&dw_timer_info);
}

type_init(dw_timer_register)

/*
Name of the controller is based on the homonymous driver from the linux kernel source

https://www.kernel.org/doc/Documentation/devicetree/bindings/interrupt-controller/snps%2Cdw-apb-ictl.txt

*/

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/typedefs.h"
#include "qom/object.h"
#include <stdint.h>

#define TYPE_DW_INTC "dw-intc"
OBJECT_DECLARE_SIMPLE_TYPE(DWIntcState, DW_INTC)

#define IRQ_AMT 64

#define APB_INT_ENABLE_L	0x00
#define APB_INT_ENABLE_H	0x04
#define APB_INT_MASK_L		0x08
#define APB_INT_MASK_H		0x0c
#define APB_INT_FINALSTATUS_L	0x30
#define APB_INT_FINALSTATUS_H	0x34

struct DWIntcState{
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq output_irq;

    //hi and lo split
    uint32_t level[2];
    uint32_t enable[2];
    uint32_t mask[2];
};

static uint32_t dw_intc_finalstatus(DWIntcState *s, int half){
    return s->level[half] & s->enable[half] & ~s->mask[half];
}

static void dw_intc_update(DWIntcState *s){
    uint32_t active = dw_intc_finalstatus(s, 0) | dw_intc_finalstatus(s, 1);
    qemu_set_irq(s->output_irq, active != 0);

}

static void dw_intc_set_irq(void *opaque, int n, int level){
    DWIntcState *s = DW_INTC(opaque);
    int half = n/32;
    int bit = n % 32;

    if (level){
        s->level[half] |= (1u<<bit);
    }
    else {
        s->level[half] &= ~(1u<<bit);
    }

    dw_intc_update(s);
}

static uint64_t dw_intc_read(void *opaque, hwaddr addr, unsigned size){
    DWIntcState *s = DW_INTC(opaque);
    switch (addr) {
        case APB_INT_ENABLE_L: return s->enable[0];
        case APB_INT_ENABLE_H: return s->enable[1];
        case APB_INT_MASK_L: return s->mask[0];
        case APB_INT_MASK_H: return s->mask[1];
        case APB_INT_FINALSTATUS_L: return dw_intc_finalstatus(s, 0);
        case APB_INT_FINALSTATUS_H: return dw_intc_finalstatus(s, 1);
        default:
            qemu_log_mask(LOG_GUEST_ERROR, "dw_apb_ictl: reading from unimplemented offset %lx\n", addr);
            return 0;
    }
}

static void dw_intc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size){

    DWIntcState *s = DW_INTC(opaque);
    switch (addr) {
        case APB_INT_ENABLE_L:
            s->enable[0] = (uint32_t)val;
            break;
        case APB_INT_ENABLE_H:
            s->enable[1] = (uint32_t)val;
            break;
        case APB_INT_MASK_L:
            s->mask[0] = (uint32_t)val;
            break;
        case APB_INT_MASK_H:
            s->mask[1] = (uint32_t)val;
            break;
        case APB_INT_FINALSTATUS_L:
        case APB_INT_FINALSTATUS_H:
            break;
        default:
            break;
    }

    dw_intc_update(s);

}


static const MemoryRegionOps dw_intc_ops = {
    .read = dw_intc_read,
    .write = dw_intc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void dw_intc_reset(DeviceState *dev){
    DWIntcState *s = DW_INTC(dev);
    s->level[0] = 0;
    s->level[1] = 0;
    s->mask[0] = 0;
    s->mask[1] = 0;
    s->enable[0] = 0;
    s->enable[1] = 0;

    qemu_set_irq(s->output_irq, 0);
}

static void dw_intc_init(Object *obj)
{
    DWIntcState *s = DW_INTC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
 
    memory_region_init_io(&s->iomem, obj, &dw_intc_ops, s, TYPE_DW_INTC, 0x4000);
    sysbus_init_mmio(sbd, &s->iomem);
 
    qdev_init_gpio_in(DEVICE(obj), dw_intc_set_irq, IRQ_AMT);
    sysbus_init_irq(sbd, &s->output_irq);
}

static void dw_intc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->legacy_reset = dw_intc_reset;
}
 
static const TypeInfo dw_intc_info = {
    .name = TYPE_DW_INTC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DWIntcState),
    .instance_init = dw_intc_init,
    .class_init = dw_intc_class_init,
};
 
static void dw_intc_register_types(void)
{
    type_register_static(&dw_intc_info);
}
 
type_init(dw_intc_register_types)
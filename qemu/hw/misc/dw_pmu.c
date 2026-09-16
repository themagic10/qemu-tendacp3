#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "qemu/log-for-trace.h"
#include "hw/core/qdev-properties.h"
#include "qemu/module.h"
#include "qapi/error.h"
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

#define TYPE_DW_PMU "dw-pmu"
OBJECT_DECLARE_SIMPLE_TYPE(DwPMUState, DW_PMU)

struct DwPMUState{
    SysBusDevice parent_obj;
    MemoryRegion iomem;
};

static uint64_t dw_pmu_reg_read(void *opaque, hwaddr addr, unsigned size){
    DwPMUState *s = opaque;

    switch (addr) {
        case 0x54: return -1;
        default: break;    
    }
    
    return 0;
}

static void dw_pmu_reg_write(void *opaque, hwaddr addr, uint64_t value, unsigned size){
    DwPMUState *s = opaque;
  
}

static const MemoryRegionOps dw_pmu_ops = {
    .read = dw_pmu_reg_read,
    .write = dw_pmu_reg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void dw_pmu_reset(DeviceState *dev){
    DwPMUState *s = DW_PMU(dev); 
}

static void dw_pmu_init(Object *obj){
    DwPMUState *s = DW_PMU(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &dw_pmu_ops, s, "dw-pmu", 0x4000);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void dw_pmu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->legacy_reset = dw_pmu_reset;    
    
}

static const TypeInfo dw_pmu_info = {
    .name          = TYPE_DW_PMU,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DwPMUState),
    .instance_init = dw_pmu_init,
    .class_init    = dw_pmu_class_init,
};

static void dw_pmu_register(void)
{

    type_register_static(&dw_pmu_info);
}

type_init(dw_pmu_register)

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

#define TYPE_DW_MCI "dw-mci"
OBJECT_DECLARE_SIMPLE_TYPE(DwMCIState, DW_MCI)

//from the dw_mmc header inside the linux kernel
#define SDMMC_CDETECT		0x050

struct DwMCIState{
    SysBusDevice parent_obj;
    MemoryRegion iomem;
};

static uint64_t dw_mci_reg_read(void *opaque, hwaddr addr, unsigned size){
    DwMCIState *s = opaque;

    //0xc02c7234: return 0x1 to skip first cmp to jump to rescan, exits fun
    switch (addr) {
        case 0x44:  return 0x104;
        case SDMMC_CDETECT: return 0x1;
        default: break;    
    }
    
    return 0;
}

static void dw_mci_reg_write(void *opaque, hwaddr addr, uint64_t value, unsigned size){
    DwMCIState *s = opaque;
  
}

static const MemoryRegionOps dw_mci_ops = {
    .read = dw_mci_reg_read,
    .write = dw_mci_reg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void dw_mci_reset(DeviceState *dev){
    DwMCIState *s = DW_MCI(dev); 
}

static void dw_mci_init(Object *obj){
    DwMCIState *s = DW_MCI(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &dw_mci_ops, s, "dw-mci", 0x4000);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void dw_mci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->legacy_reset = dw_mci_reset;    
    
}

static const TypeInfo dw_mci_info = {
    .name          = TYPE_DW_MCI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DwMCIState),
    .instance_init = dw_mci_init,
    .class_init    = dw_mci_class_init,
};

static void dw_mci_register(void)
{

    type_register_static(&dw_mci_info);
}

type_init(dw_mci_register)

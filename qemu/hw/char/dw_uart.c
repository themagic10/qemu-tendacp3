#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/typedefs.h"
#include "qom/object.h"

#include "hw/core/qdev.h" 
#include "hw/char/serial.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/char/serial-mm.h"
#include "system/memory.h"

#define TYPE_DW_UART "dw-uart"
OBJECT_DECLARE_SIMPLE_TYPE(DWUartState, DW_UART)

#define DW_REGION_SIZE 0x1000

#define SHADOW_START 0x30
#define SHADOW_END 0x6c
#define DW_LPDLL
#define DW_LPDLH
#define DW_FAR
#define DW_TFR
#define DW_RFW
#define DW_USR 0x7c

#define UART_LSR_DR   0x01 //line status register, data ready bit

struct DWUartState {
    SysBusDevice parent_obj;

    MemoryRegion large_mmio;
    MemoryRegion dwmmio;

    SerialMM serial_mm;
};

static uint64_t dw_uart_reg_read(void *opaque, hwaddr addr, unsigned size){
    DWUartState *s = opaque;
    SerialState *ss = &s->serial_mm.serial;

    if (addr >= SHADOW_START && addr <=SHADOW_END){
        return serial_io_ops.read(ss, 0x0, 1);
    }

    switch (addr) {
        case DW_USR:{
            uint32_t usr = (1 << 1) | (1 << 2); // 0x6 base

            //required when using ttys0 in userspace
            if (ss->lsr & UART_LSR_DR) {
                usr |= (1 << 3); // RFNE
            }
            return usr;
        }
        default: 
            qemu_log_mask(LOG_GUEST_ERROR, "dw uart: reading unimplemented register %lx\n", addr);
            return 0;
    }
}

static void dw_uart_reg_write(void *opaque, hwaddr addr, uint64_t value, unsigned size){
    DWUartState *s = opaque;
    SerialState *ss = &s->serial_mm.serial;

    if (addr >= SHADOW_START && addr <=SHADOW_END){
        serial_io_ops.write(ss, 0x0, value & 0xff, 1);
        return;
    }
    
    switch (addr){
        default:
            qemu_log_mask(LOG_GUEST_ERROR, "dw uart: writing to unimplemented register %lx\n", addr);
            return;
    }

}

static const MemoryRegionOps dw_uart_ops = {
    .read = dw_uart_reg_read,
    .write = dw_uart_reg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1, 
    .valid.max_access_size = 4,
};

static void dw_uart_realize(DeviceState* dev, Error **errp){
    DWUartState *s = DW_UART(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    //set serial mm config
    qdev_prop_set_uint8(DEVICE(&s->serial_mm), "regshift", 2);
    qdev_prop_set_uint8(DEVICE(&s->serial_mm), "endianness", DEVICE_LITTLE_ENDIAN);


    sysbus_realize(SYS_BUS_DEVICE(&s->serial_mm), errp);

    memory_region_init(&s->large_mmio, OBJECT(dev), "dw uart large", DW_REGION_SIZE);

    //custom dw logic with lower priority
    memory_region_init_io(&s->dwmmio, OBJECT(dev), &dw_uart_ops, s, "dw uart", DW_REGION_SIZE);
    memory_region_add_subregion_overlap(&s->large_mmio, 0, &s->dwmmio, -1);

    MemoryRegion *serial_mm_region = sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->serial_mm), 0);
    memory_region_add_subregion_overlap(&s->large_mmio, 0, serial_mm_region, 0);

    sysbus_init_mmio(sbd, &s->large_mmio);
    sysbus_pass_irq(sbd, SYS_BUS_DEVICE(&s->serial_mm));
}

static void dw_uart_instance_init(Object *obj){
    DWUartState *s = DW_UART(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    object_initialize_child(obj, "serial-mm", &s->serial_mm, TYPE_SERIAL_MM);
    
    qdev_alias_all_properties(DEVICE(&s->serial_mm.serial), obj);
}

static void dw_uart_class_init(ObjectClass *klass, const void *data){
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = dw_uart_realize;

}

static const TypeInfo dw_uart_info = {
    .name          = TYPE_DW_UART,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DWUartState),
    .class_init    = dw_uart_class_init,
    .instance_init = dw_uart_instance_init,
};
 
static void dw_uart_register_types(void) {
    type_register_static(&dw_uart_info);
}
 
type_init(dw_uart_register_types)
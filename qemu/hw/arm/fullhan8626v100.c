#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/typedefs.h"
#include "qom/object.h"
#include "qemu/module.h"
 
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/char/pl011.h"
#include "hw/ssi/ssi.h"
#include "hw/misc/unimp.h"
#include "hw/core/loader.h"
#include "hw/char/serial-mm.h" 
#include "hw/core/qdev.h"
#include "hw/arm/machines-qom.h"
#include "target/arm/cpu-qom.h"


#include "system/address-spaces.h"
#include "system/blockdev.h"
#include "system/memory.h"
#include "system/reset.h"
#include "system/system.h"

 
#include "target/arm/cpu-qom.h"
#include <stdlib.h>
#include <string.h>

#define TYPE_FH8626V100_MACHINE MACHINE_TYPE_NAME("fullhan8626v100")
OBJECT_DECLARE_SIMPLE_TYPE(FHState, FH8626V100_MACHINE)

#define FH_ENTRY 0xa0800000

#define FH_DRAM_BASE 0xa0000000
#define FH_DRAM_SIZE 0x03000000
#define FH_DMAC_BASE 0xe0300000
#define FH_GMAC_BASE 0xe0600000

#define FH_UART_0_BASE 0xf0700000
#define FH_UART_1_BASE 0xf0800000
#define FH_UART_2_BASE 0xf1300000

#define FH_SPI_BASE 0xf0e00000



#define FH_TIMER_BASE 0xf0c00000

struct FHState{
    MachineState parent;
    ARMCPU* cpu;
    qemu_irq irq;
};

static void fh_reset(void* opaque){
    FHState *fhs = opaque;
    CPUState *cps = CPU(fhs->cpu);
    cpu_reset(cps);
    cpu_set_pc(cps, FH_ENTRY);
}

static void fh_init(MachineState *machine){
    FHState *fhs = FH8626V100_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    DeviceState *ssi_dev;
    SSIBus *ssi_bus;
    DriveInfo *dinfo;


    if (machine->ram_size != FH_DRAM_SIZE){
        error_report("fullhan8626v100: Ram size should be fixed at 48MB!\n");
        exit(1);
    }

    // todo: reminder for future me
    // despite the fact that the soc has 48mb of ram the kernel will only ask for 43
    // the rest might be dedicated to video encoding or other stuff, dma access to those region
    // *might* cause some issue if that's the case
    memory_region_add_subregion(sysmem, FH_DRAM_BASE, machine->ram);

    fhs->cpu = ARM_CPU(object_new(ARM_CPU_TYPE_NAME("arm1176")));
    object_property_set_bool(OBJECT(fhs->cpu), "has_el3", false, &error_fatal);
    object_property_set_bool(OBJECT(fhs->cpu), "realized", true, &error_fatal);

    fhs->irq = qdev_get_gpio_in(DEVICE(fhs->cpu), ARM_CPU_IRQ);

    //pl011_create(FH_UART_BASE, NULL, serial_hd(0));
    DeviceState *uart = qdev_new("dw-uart");
    qdev_prop_set_chr(uart, "chardev", serial_hd(0));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(uart), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(uart), 0, FH_UART_0_BASE);
    //sysbus_connect_irq(SYS_BUS_DEVICE(uart), 2, fhs->irq);

    ssi_dev = sysbus_create_simple("dw-spi", FH_SPI_BASE, NULL);
    ssi_bus = (SSIBus *)qdev_get_child_bus(ssi_dev, "ssi");

    dinfo = drive_get(IF_MTD, 0, 0);
    if (dinfo){
        DeviceState *flash = qdev_new("by25q64as");
        qdev_prop_set_drive_err(flash, "drive", 
            blk_by_legacy_dinfo(dinfo), &error_fatal);
        ssi_realize_and_unref(flash, ssi_bus, &error_fatal);
        qemu_irq flash_cs = qdev_get_gpio_in_named(flash, SSI_GPIO_CS, 0);
        qdev_connect_gpio_out_named(ssi_dev, SSI_GPIO_CS, 0, flash_cs);
    }

    //called that way in the kernel symbols... actually it's just pmu
    //create_unimplemented_device("fh-pmu-timer", 0xf0000000, 0x2000);
    MemoryRegion *sram = g_new(MemoryRegion, 1);
    memory_region_init_ram(sram, NULL, "pmu-test",
                        0x2000, &error_fatal);
    memory_region_add_subregion(get_system_memory(), 0xf0000000, sram);


    create_unimplemented_device("wdt stub", 0xf0d00000, 0x2000);
    create_unimplemented_device("gpio0 stub", 0xf0300000, 0x2000);
    create_unimplemented_device("gpio1 stub", 0xf4000000, 0x2000);

    //interrupt controller
    DeviceState *intc = qdev_new("dw-intc");
    sysbus_realize_and_unref(SYS_BUS_DEVICE(intc), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(intc), 0, 0xe0200000);
    sysbus_connect_irq(SYS_BUS_DEVICE(intc), 0, fhs->irq);

    //timer
    DeviceState *timer = qdev_new("dw-timer");
    sysbus_realize_and_unref(SYS_BUS_DEVICE(timer), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(timer), 0, FH_TIMER_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(timer), 1, qdev_get_gpio_in(intc, 3));

    //sysbus_create_simple("dw-timer", FH_TIMER_BASE, NULL);


    sysbus_create_simple("dw-dmac", FH_DMAC_BASE, NULL);

    if (machine->kernel_filename){
        //not the actual max size but still
        load_image_targphys(machine->kernel_filename, FH_ENTRY, 
            FH_DRAM_SIZE, &error_fatal);
    }

    DeviceState *uart1 = sysbus_create_simple("dw-uart", FH_UART_1_BASE, NULL);
    DeviceState *uart2 = sysbus_create_simple("dw-uart", FH_UART_2_BASE, NULL);

    qemu_register_reset(fh_reset, fhs);
}

static void fh_class_init(ObjectClass *oc, const void *data){
    MachineClass *mc = MACHINE_CLASS(oc);
    mc->desc = "Fullhan 8626 v100 SOC";
    mc->init = fh_init;

    static const char * const valid_cpu[] = {
        ARM_CPU_TYPE_NAME("arm1176"),
        NULL
    };
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm1176");
    mc->valid_cpu_types = valid_cpu;
    mc->min_cpus = 1;
    mc->max_cpus = 1;
    mc->default_cpus = 1;

    mc->default_ram_id = "fh.ram";
    mc->default_ram_size = FH_DRAM_SIZE;

    mc->block_default_type = IF_MTD;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;

}

static const TypeInfo fh8626_mach_type = {
    .name = TYPE_FH8626V100_MACHINE,
    .parent = TYPE_MACHINE,
    .class_init = fh_class_init,
    .interfaces = arm_machine_interfaces,
};

static void fullhan8626v100_register_types(void){
    type_register_static(&fh8626_mach_type);
}

type_init(fullhan8626v100_register_types);
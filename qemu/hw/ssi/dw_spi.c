#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "qemu/log-for-trace.h"
#include "hw/core/qdev-properties.h"
#include "hw/ssi/ssi.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qemu/typedefs.h"
#include "qom/object.h"
#include "hw/core/sysbus.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "hw/core/hw-error.h"
#include "qemu/error-report.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "hw/core/qdev.h"

#define TYPE_DW_SPI "dw-spi"
OBJECT_DECLARE_SIMPLE_TYPE(DWSPIState, DW_SPI)

// based on https://elixir.bootlin.com/linux/v7.1.5/source/drivers/spi/spi-dw.h
// technically should be half duplex ssi but it's always refered to spi according to the firmware so who cares...

// could move data to header to make the file cleaner
//(un)fortunately i dont care

#define DW_SPI_CTRLR0		    0x00
#define DW_SPI_CTRLR1		    0x04
#define DW_SPI_SSIENR		    0x08
#define DW_SPI_MWCR			    0x0c
#define DW_SPI_SER			    0x10
#define DW_SPI_BAUDR		    0x14
#define DW_SPI_TXFTLR		    0x18
#define DW_SPI_RXFTLR		    0x1c
#define DW_SPI_TXFLR		    0x20
#define DW_SPI_RXFLR		    0x24
#define DW_SPI_SR			    0x28
#define DW_SPI_IMR			    0x2c
#define DW_SPI_ISR			    0x30
#define DW_SPI_RISR			    0x34
#define DW_SPI_TXOICR		    0x38
#define DW_SPI_RXOICR		    0x3c
#define DW_SPI_RXUICR		    0x40
#define DW_SPI_MSTICR		    0x44
#define DW_SPI_ICR			    0x48
#define DW_SPI_DMACR		    0x4c
#define DW_SPI_DMATDLR		    0x50
#define DW_SPI_DMARDLR		    0x54
#define DW_SPI_IDR			    0x58
#define DW_SPI_VERSION		    0x5c
#define DW_SPI_DR_BASE			0x60
#define DW_SPI_DR_END			0xec
//these are effectively fake... the device will not respect them, those addresses
//are most likely vendor registers (possibily with the physical location on the bus for the flash)
#define DW_SPI_RX_SAMPLE_DLY	0xf0
#define DW_SPI_CS_OVERRIDE	    0xf4
#define DW_RESERVED             0xfc

//unknown regs, not in the documents...
#define VENDOR_BASE             0xf0
#define VENDOR_END              0xfff // overkill but who cares?
#define VENDOR_NREG (((VENDOR_END - VENDOR_BASE)/4) + 1)

// data frame size
// max 16 bit frame size, same as fifo regs
#define CTRL0_DFS(ctrl) ((ctrl) & 0xf)

/*
transmission mode:
00 tx rx
01 tx only
10 rx only
11 eeprom read
*/
#define CTRL0_TMOD(ctrl) ((ctrl>>8) & 0x3)
#define TMOD_TX_RX 0
#define TMOD_TX 1
#define TMOD_RX 2
#define TMOD_EEPROM 3

/* sr: status register */
#define SR_TFNF 1<<1 //transmit fifo not full
#define SR_TFE  1<<2 //transmit fifo empty
#define SR_RFNE 1<<3 //recieve fifo not empty
#define SR_RFF  1<<4 //recieve fifo full
#define SR_TXE  1<<5 //transmission error
#define SR_DCOL 1<<6 //data collision


#define VERSION 0x3332322a
#define MAX_DATA_REGS 36
#define MAX_SLAVE 16 //should be 31 according to the docs
#define MAX_FIFO 256 //arbitrary, fifo shouldn't be that big on a real device..
 
struct DWSPIState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    SSIBus *bus;
    qemu_irq irq; //required by realize()
    qemu_irq cs[MAX_SLAVE];

    // regs
    uint32_t ctrlr0;
    uint32_t ctrlr1; 
    uint32_t ssienr; //ssi enable register

    uint32_t mwcr;
    uint32_t ser;
    uint32_t baudr;
    uint32_t txftlr;
    uint32_t rxftlr;
    uint32_t imr;

    uint32_t dmacr;
    uint32_t dmatdlr;
    uint32_t dmardlr;
    uint32_t idr;

    uint32_t vendor[VENDOR_NREG];
    
    uint16_t dr[MAX_DATA_REGS]; //assuming SSI_MAX_XFER_SIZE is 16 (it is)

    bool hasselectedchip;
    uint32_t selected_cs;

    /* rx only/eeprom */
    uint32_t rx_left;

    /* fifo */
    uint16_t fifo[MAX_FIFO];
    uint32_t fifo_head;
    uint32_t fifo_len;
};

static void evaluatecs(DWSPIState* s){
    if (!s->ser){
        s->hasselectedchip = false;
        return;
    }

    uint index = ctz32(s->ser);
    s->selected_cs = index;
    s->hasselectedchip = true;
}

static uint32_t transfer(DWSPIState *s, uint32_t data){
    uint32_t rx = ssi_transfer(s->bus, data);
    return rx;
}

static void fifo_push_data(DWSPIState *s, uint32_t data){
    uint32_t val = transfer(s, data);
    //tail = head + len
    s->fifo[s->fifo_head + s->fifo_len] = val;
    s->fifo_len++;
}

static void dw_spi_flush(DWSPIState *s){
    s->fifo_head = 0;
    s->fifo_len = 0;
    s->rx_left = 0;
}

/*  DATA REGISTERS  */
static uint32_t dw_spi_read(DWSPIState *s){

    uint32_t ret;
    
    if (s->fifo_len){
        ret = s->fifo[s->fifo_head];
        s->fifo_head++;
        s->fifo_len--;
    }
    else if (s->rx_left){
        if (!s->hasselectedchip){
            error_report("dw spi: tried to read in rx/eprom without cs");
            return 0xffffffff;
        }
        ret = transfer(s, 0xffff);
        s->rx_left--;
    }
    else {
        ret = 0xffffffff;
    }

    return ret;
    

}
static void dw_spi_write(DWSPIState *s, uint32_t val){
    uint32_t ndf = s->ctrlr1 + 1;
    if (!s->ssienr){
        error_report("dw spi: trying to write to data register without setting ssienr, forbidden");
        return;
    }
    if (!s->hasselectedchip){
        error_report("dw spi: write to dr without selected cs, evaluating on the spot...");
        evaluatecs(s);
    }
    switch (CTRL0_TMOD(s->ctrlr0)) {
        case TMOD_TX_RX:
            fifo_push_data(s, val);
            break;
        case TMOD_RX:
            if (!s->rx_left){
                s->rx_left = ndf;
            }
            break;
        case TMOD_TX:
            transfer(s, val);
            break;
        case TMOD_EEPROM:
            hw_error("writing in eeprom mode, not implemented");
            break;
    }

}

/*  MMIO REGISTERS  */

static uint64_t dw_spi_reg_read(void *opaque, hwaddr addr, unsigned size){
    DWSPIState *s = opaque;
    uint32_t ret = 0;
    if (addr > 0x1000){
        return 0xffffffff;
    }

    if (addr >= DW_SPI_DR_BASE && addr <= DW_SPI_DR_END){
        return dw_spi_read(s);
    }

    if (addr >= VENDOR_BASE && addr <= VENDOR_END){
        return s->vendor[addr<<2];
    }

    switch (addr) {
        case DW_SPI_CTRLR0: ret = s->ctrlr0; break;
        case DW_SPI_CTRLR1: ret = s->ctrlr1; break;
        case DW_SPI_SSIENR: ret = s->ssienr; break;
        case DW_SPI_SER: ret = s->ser; break;
        case DW_SPI_BAUDR: ret = s->baudr; break;
        case DW_SPI_TXFTLR: ret = s->txftlr; break;
        case DW_SPI_RXFTLR: ret = s->rxftlr; break;
        case DW_SPI_TXFLR: ret = 0; break; // we consume tx instantly
        case DW_SPI_RXFLR: ret = s->fifo_len + s->rx_left; break;
        case DW_SPI_SR:
            ret = SR_TFNF | SR_TFE; // fifo not full
            break;
        case DW_SPI_IMR: ret = s->imr; break;
        default:
            error_report("dw spi: reading unimplemented register %X", addr);
            break;
    }
    return ret;

}
static void dw_spi_reg_write(void *opaque, hwaddr addr, uint64_t value, unsigned size){
    DWSPIState *s = opaque;
    if (addr > 0x1000){
        return;
    }

    if (addr >= DW_SPI_DR_BASE && addr <= DW_SPI_DR_END){
        dw_spi_write(s, value & 0xffffffff);
        return;
    }

    if (addr >= VENDOR_BASE && addr <= VENDOR_END){
        s->vendor[addr<<2] = value & 0xffffffff;
        return;
    }

    switch (addr) {
        case DW_SPI_CTRLR0:
            if (!s->ssienr){
                error_report("dw spi: writing to ctrl0 without setting ssienr");
            }
            s->ctrlr0 = value & 0xffffffff;
            break;
        case DW_SPI_CTRLR1:
            s->ctrlr1 = value & 0xffff;
            break;
        case DW_SPI_SSIENR:
            s->ssienr = value & 0x1;
            if (!s->ssienr){
                dw_spi_flush(s);
            }
            break;
        case DW_SPI_SER:
            //violates the reserved bits but i really dont care
            s->ser = value;
            evaluatecs(s);
            break;
        case DW_SPI_BAUDR:
            s->baudr = value;
            break;
        case DW_SPI_TXFTLR:
            s->txftlr = value;
            break;
        case DW_SPI_RXFTLR:
            s->rxftlr = value;
            break;
        case DW_SPI_IMR:
            s->imr = value;
            break;
        default:
            error_report("dw spi: writing to unimplemented register");
            fflush(stderr);
            fprintf(stderr, "dw spi write: currently writing to %X\n", addr);
            if (addr == 0x60){
                fprintf(stderr, "ctrl0 is %x", s->ctrlr0);
            }
            break;
    }

}

static const MemoryRegionOps dw_spi_ops = {
    .read = dw_spi_reg_read,
    .write = dw_spi_reg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

// registration

//mainly based on pl022.c
static void dw_spi_realize(DeviceState* dev, Error **errp){
    SysBusDevice *sdb = SYS_BUS_DEVICE(dev);
    DWSPIState *s = DW_SPI(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &dw_spi_ops, 
        s, "dw-spi", 0x1000);
    sysbus_init_mmio(sdb, &s->iomem);
    sysbus_init_irq(sdb, &s->irq);
    s->bus = ssi_create_bus(dev, "ssi");

    //aspeed_smc.c...
    qdev_init_gpio_out_named(dev, s->cs, SSI_GPIO_CS, MAX_SLAVE);
}

static void dw_spi_reset(DeviceState *dev){
    DWSPIState *s = DW_SPI(dev);
    s->ctrlr0 = 0x7;
    s->ctrlr1 = 0;
    s->ssienr = 0;
    s->ser = 0;

}

static void dw_spi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, dw_spi_reset);
    //TODO: ADD VMSTATE
    dc->realize = dw_spi_realize;

}

static const TypeInfo dw_spi_info = {
    .name          = TYPE_DW_SPI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DWSPIState),
    .class_init    = dw_spi_class_init,
};
 
static void dw_spi_register_types(void)
{
    type_register_static(&dw_spi_info);
}
 
type_init(dw_spi_register_types)
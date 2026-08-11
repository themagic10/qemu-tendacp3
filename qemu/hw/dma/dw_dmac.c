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
#include "math.h"
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include "hw/core/hw-error.h"
#include "qemu/error-report.h"
#include "hw/core/sysbus.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "system/memory.h"
#include "hw/core/qdev.h"
#include "exec/memattrs.h"
#include "qemu/bswap.h"
#include "qemu/log.h"

#define TYPE_DW_DMAC "dw-dmac"
OBJECT_DECLARE_SIMPLE_TYPE(DWDmacState, DW_DMAC);

#define MAX_CHANNELS 8

/*
note on cfg and ctl, as stated in the linux driver for dw dma regs
they are divided in low and high: while every other reg is effectively a 32 bit
value in a 64 bit register (hi reg is reserved), cfg and ctl have data in both 32 halves.
*/

/* channels regs */

#define CH_SAR      0x000
#define CH_DAR      0x008
#define CH_LLP      0x010
#define CH_CTL_LO   0x018
#define CH_CTL_HI   0x01c
#define CH_SSTAT    0x020
#define CH_DSTAT    0x028
#define CH_SSTATAR  0x030
#define CH_DSTATAR  0x038
#define CH_CFG_LO   0x040
#define CH_CFG_HI   0x044
#define CH_SGR      0x048
#define CH_DSR      0x050

/* global registers */

#define RAWTFR          0x2c0
#define RAWBLOCK        0x2c8
#define RAWSRCTRAN      0x2d0
#define RAWDSTTRAN      0x2d8
#define RAWERR          0x2e0

#define STATUSTFR       0x2e8
#define STATUSBLOCK     0x2f0
#define STATUSSRCTRAN   0x2f8
#define STATUSDSTTRAN   0x300
#define STATUSERR       0x308

#define MASKTFR         0x310
#define MASKBLOCK       0x318
#define MASKSRCTRAN     0x320
#define MASKDSTTRAN     0x328
#define MASKERR         0x330

#define CLEARTFR        0x338
#define CLEARBLOCK      0x340
#define CLEARSRCTRAN    0x348
#define CLEARDSTTRAN    0x350
#define CLEANERR        0x358
#define STATUSINT       0x360

#define REQSRCREG       0x368
#define REQDSTREG       0x370
#define SGLREQSRCREG    0x378
#define SGLREQDESTREG   0x380
#define LSTSRCREG       0x388
#define LSTDSTREG       0x390

#define DMACFGREG       0x398
#define CHENREG         0x3a0
#define DMAIDREG        0x3a8
#define DMATESTREG      0x3b0
//2 reserved..

#define DMA_COMP_PARAMS_6   0x3c8
#define DMA_COMP_PARAMS_5   0x3d0
#define DMA_COMP_PARAMS_4   0x3d8
#define DMA_COMP_PARAMS_3   0x3e0
#define DMA_COMP_PARAMS_2   0x3e8
#define DMA_COMP_PARAMS_1   0x3f0
#define DMA_COMP_ID         0x3f8


/* additional info */

#define CHAN_START  0x0
#define CHAN_END    0x2b8
#define CHAN_SIZE   0x58

/* chan ctl bits */
#define CTL_INT_EN(x)       (x & 0x1)
#define CTL_DEST_TRWIDTH(x) ((x>>1) & 0x7)
#define CTL_SRC_TRWIDTH(x)  ((x>>4) & 0x7)
#define CTL_DINC(x)         ((x>>7) & 0x3)
#define CTL_SINC(x)         ((x>>9) & 0x3)
#define CTL_TT_FC(x)        ((x>>20) & 0x7)
#define CTL_LLP_DEST_EN(x)  ((x>>27) & 0x1)
#define CTL_LLP_SRC_EN(x)   ((x>>28) & 0x1)

#define CTL_HI_BLOCK_TS(x)  (x & 0xfff)
//returns bytes
#define CTL_WIDTH_DECODE(x) (1u<<x) //not true with 11x but we're most likely not using those sizes

#define INC_INC 0
#define INC_DEC 1
//both 2 and 3 are no change



typedef struct DwDmacChanState{
    uint32_t sar;
    uint32_t dar;
    uint32_t llp;
    uint32_t ctl_lo, ctl_hi;
    uint32_t sstat;
    uint32_t dstat;
    uint32_t sstatar;
    uint32_t dstatar;
    uint32_t cfg_lo, cfg_hi;
    uint32_t sgr;
    uint32_t dsr;
    
} DwDmacChanState;


struct DWDmacState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t channel_amt;
    DwDmacChanState chan[MAX_CHANNELS]; 

    uint64_t raw_tfr, raw_block, raw_srctran, raw_dsttran, raw_err;
    uint64_t mask_tfr, mask_block, mask_srctran, mask_dsttran, mask_err;
    uint64_t dma_cfg;
    uint64_t chan_en;

    //downstream
    MemoryRegion* dma_mr;
    AddressSpace dma_as;

};

/* dma logic */

static void dmac_transfer(DWDmacState *s, uint channel_num, uint32_t ctl_lo, uint32_t ctl_hi, uint32_t *sar, uint32_t *dar){
    uint32_t src_w = CTL_WIDTH_DECODE(CTL_SRC_TRWIDTH(ctl_lo));
    uint32_t dst_w = CTL_WIDTH_DECODE(CTL_DEST_TRWIDTH(ctl_lo));
    uint32_t sinc = CTL_SINC(ctl_lo);
    uint32_t dinc = CTL_DINC(ctl_lo);
    uint32_t block_ts = CTL_HI_BLOCK_TS(ctl_hi);
    uint64_t total = (uint64_t)(block_ts*src_w); //64 bits just to be safe

    uint32_t saddr = *sar;
    uint32_t daddr = *dar;

    uint8_t fifo[4096];
    uint fifo_len = 0;

    uint64_t done = 0;
    qemu_log_mask(LOG_GUEST_ERROR,"ch=%u ctl_lo=%x srcw=%u dstw=%u totalblock=%u", channel_num, ctl_lo, src_w, dst_w, block_ts);

    while (done < total) {
        if (dma_memory_read(&s->dma_as, saddr, fifo+fifo_len, src_w, MEMTXATTRS_UNSPECIFIED) != MEMTX_OK){
            error_report("dw dmac: read failed at channel %u address %x", channel_num, saddr);
            s->raw_err |= 1u<<channel_num;
            goto out_loop;
        }

        if (sinc == INC_INC){
            saddr += src_w;
        }
        else if (sinc == INC_DEC) {
            saddr -= src_w;
        }
        //else stay fixed
        
        fifo_len +=src_w;
        done += src_w;
        while (fifo_len >= dst_w){
            if (dma_memory_write(&s->dma_as, daddr, fifo, dst_w, MEMTXATTRS_UNSPECIFIED) != MEMTX_OK){
                error_report("dw dmac: write failed at channel %u address %x", channel_num, daddr);
                s->raw_err |= 1u<<channel_num;
                goto out_loop; // double break
            }
        
            if (dinc == INC_INC){
                daddr += dst_w;
            }
            else if (dinc == INC_DEC){
                daddr -= dst_w;
            }
            fifo_len -= dst_w;
            //todo use actual fifo structure 
            memmove(fifo, fifo+dst_w, fifo_len);
        }
    }

out_loop:
    //update pointers
    *sar = saddr;
    *dar = daddr;                                     
}

static void dw_dmac_start_channel(DWDmacState *s, uint ch_num){
    DwDmacChanState ch = s->chan[ch_num];
    uint32_t sar = ch.sar;
    uint32_t dar = ch.dar;
    uint32_t ctl_lo = ch.ctl_lo;
    uint32_t ctl_hi = ch.ctl_hi;
    uint32_t next = ch.llp;

    // check if dmac is enabled
    if (!(s->dma_cfg & 0x1)){
        error_report("dw dmac: creating channel without enabling dmac!");
    }

    if (CTL_LLP_SRC_EN(ch.ctl_lo)| CTL_LLP_DEST_EN(ch.ctl_lo)){

        while (next != 0){
            uint32_t ll_buff[5];
            if (dma_memory_read(&s->dma_as, next, ll_buff, sizeof(ll_buff), MEMTXATTRS_UNSPECIFIED) != MEMTX_OK){
                s->raw_err |= 1u<<ch_num;
                break;
            }
            
            sar = le32_to_cpu(ll_buff[0]);
            dar = le32_to_cpu(ll_buff[1]);
            next = le32_to_cpu(ll_buff[2]); //llp reg
            ctl_lo = le32_to_cpu(ll_buff[3]);
            ctl_hi = le32_to_cpu(ll_buff[4]);

            dmac_transfer(s, ch_num, ctl_lo, ctl_hi, &sar, &dar);
            s->raw_block |= 1u<<ch_num;

            if (!(CTL_LLP_SRC_EN(ctl_lo) | CTL_LLP_DEST_EN(ctl_lo))){
                next = 0;
            }
        }
    }
    else {
        dmac_transfer(s, ch_num, ctl_lo, ctl_hi, &sar, &dar);
        s->raw_block |= 1u << ch_num;
    }

    ch.sar = sar;
    ch.dar = dar;
    ch.llp = next;
    ch.ctl_lo = ctl_lo;
    ch.ctl_hi = ctl_hi;

    s->raw_srctran |= 1u<<ch_num;
    s->raw_dsttran |= 1u<<ch_num;
    s->raw_tfr |= 1u<<ch_num;

    //disable channel
    s->chan_en &= ~(1u<<ch_num);
}

/* iomem r/w */

static uint64_t evaluate_status_int(DWDmacState *s){
    uint tfr = ((s->raw_tfr & s->mask_tfr) != 0);
    uint block = ((s->raw_block & s->mask_block) != 0);
    uint srctran = ((s->raw_srctran & s->mask_srctran) != 0);
    uint dsttran = ((s->raw_dsttran & s->mask_dsttran) != 0);
    uint err = ((s->raw_err & s->mask_err) != 0);
    return (tfr | (block<<1) | (srctran<<2) | (dsttran<<3) | (err<<4));
}



static uint64_t dw_dmac_reg_read(void *opaque, hwaddr addr, unsigned size){
    DWDmacState *s = DW_DMAC(opaque);
    if (addr >= CHAN_START && addr <= CHAN_END){
        uint chan_sel = addr/CHAN_SIZE;
        uint chan_off = addr % CHAN_SIZE;
        DwDmacChanState *ch = &s->chan[chan_sel];
        switch (chan_off) {
            case CH_SAR: return ch->sar;
            case CH_DAR: return ch->dar;
            case CH_LLP: return ch->llp;
            case CH_CTL_LO: return ch->ctl_lo;
            case CH_CTL_HI: return ch->ctl_hi;
            case CH_SSTAT: return ch->sstat;
            case CH_DSTAT: return ch->dstat;
            case CH_SSTATAR: return ch->sstatar;
            case CH_DSTATAR: return ch->dstatar;
            case CH_CFG_LO: return ch->cfg_lo;
            case CH_CFG_HI: return ch->cfg_hi;
            case CH_SGR: return ch->sgr;
            case CH_DSR: return ch->dsr;
            default: return 0;
        
        }
    }

    switch (addr) {
        case RAWTFR: return s->raw_tfr;
        case RAWBLOCK: return s->raw_block;
        case RAWSRCTRAN: return s->raw_srctran;
        case RAWDSTTRAN: return s->raw_dsttran;
        case RAWERR: return s->raw_err;

        //status is masked raw effectively..
        case STATUSTFR: return s->raw_tfr & s->mask_tfr;
        case STATUSBLOCK: return s->raw_block & s->mask_block;
        case STATUSSRCTRAN: return s->raw_srctran & s->mask_srctran;
        case STATUSDSTTRAN: return s->raw_dsttran & s->mask_dsttran;
        case STATUSERR: return s->raw_err & s->mask_err;

        case MASKTFR: return s->mask_tfr;
        case MASKBLOCK: return s->mask_block;
        case MASKSRCTRAN: return s->mask_srctran;
        case MASKDSTTRAN: return s->mask_dsttran;
        case MASKERR: return s->mask_err;

        case STATUSINT: return evaluate_status_int(s);

        case DMACFGREG: return s->dma_cfg;
        case CHENREG: return s->chan_en;
        default:
            error_report("dw dmac: reading unimplemented register %x\n", addr);
            return 0;
    }
}

static void dw_dmac_reg_write(void *opaque, hwaddr addr, uint64_t value, unsigned size){
    DWDmacState *s = DW_DMAC(opaque);
    uint32_t v = (uint32_t) value;
    if (addr >= CHAN_START && addr <= CHAN_END){
        uint chan_sel = addr/CHAN_SIZE;
        uint chan_off = addr % CHAN_SIZE;
        DwDmacChanState *ch = &s->chan[chan_sel];
        switch (chan_off) {
            case CH_SAR: ch->sar = v; break;
            case CH_DAR: ch->dar = v; break;
            case CH_LLP: ch->llp = v; break;
            case CH_CTL_LO:  ch->ctl_lo = v; break;
            case CH_CTL_HI:  ch->ctl_hi = v; break;
            case CH_SSTAT: ch->sstat = v; break;
            case CH_DSTAT: ch->dstat = v; break;
            case CH_SSTATAR: ch->sstatar = v; break;
            case CH_DSTATAR: ch->dstatar = v; break;
            case CH_CFG_LO: ch->cfg_lo = v; break;
            case CH_CFG_HI: ch->cfg_hi = v; break;
            case CH_SGR: ch->sgr = v; break;
            case CH_DSR: ch->dsr = v; break;
            default:
                error_report("dw dmac: writing to unknown channel register %x\n", addr);
                break;
        
        }
        return;        
    }

    // write enable logic
    uint32_t data = v & 0xff;
    uint32_t we = ((v>>8) & 0xff);

    switch (addr) {

        case MASKTFR: s->mask_tfr =  (s->mask_tfr & ~we) | (data & we); break;
        case MASKBLOCK: s->mask_block =  (s->mask_block & ~we) | (data & we); break;
        case MASKSRCTRAN: s->mask_srctran =  (s->mask_srctran & ~we) | (data & we); break;
        case MASKDSTTRAN: s->mask_dsttran =  (s->mask_dsttran & ~we) | (data & we); break;
        case MASKERR: s->mask_err =  (s->mask_err & ~we) | (data & we); break;

        // clears raw (and therefore status) regs
        case CLEARTFR: s->raw_tfr &= ~v; break;
        case CLEARBLOCK: s->raw_block &= ~v; break;
        case CLEARSRCTRAN: s->raw_srctran &= ~v; break;
        case CLEARDSTTRAN: s->raw_dsttran &= ~v; break;
        case CLEANERR: s->raw_err &= ~v; break;

        //enable reg
        case DMACFGREG: s->dma_cfg = v & 0x1; break;

        case CHENREG:
            // if we try to enable an already existing channel we will fail here
            uint newchan = (data & we) & ~s->chan_en; 

            for (uint c = 0; c < MAX_CHANNELS; c++){
                if (newchan & (1u<<c)){
                    s->chan_en |= 1u<<c;
                    dw_dmac_start_channel(s, c);
                }
            }
            break;

        default:
        error_report("dw dmac: writing to unimplemented register %x", addr);
    
    }

}

static const MemoryRegionOps dw_dmac_ops = {
    .read = dw_dmac_reg_read,
    .write = dw_dmac_reg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void dw_dmac_reset(DeviceState* dev){
    DWDmacState *s = DW_DMAC(dev);

    s->raw_tfr = 0;
    s->raw_block = 0;
    s->raw_srctran = 0;
    s->raw_dsttran = 0;
    s->raw_err = 0;

    s->mask_tfr = 0;
    s->mask_block = 0;
    s->mask_srctran = 0;
    s->mask_dsttran = 0;
    s->mask_err = 0;

    s->dma_cfg = 0;
    s->chan_en = 0;
}

static void dw_dmac_realize(DeviceState* dev, Error **errp){
    DWDmacState *s = DW_DMAC(dev);

    s->dma_mr = get_system_memory();
    address_space_init(&s->dma_as, s->dma_mr, "dw-dma-as");

}

static void dw_dmac_instance_init(Object *obj){
    DWDmacState *s = DW_DMAC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    //size of region before mirroring
    memory_region_init_io(&s->iomem, obj, &dw_dmac_ops, s, "dw-dmac", 0x400);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

}

static void dw_dmac_class_init(ObjectClass *klass, const void *data) {
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = dw_dmac_realize;
    device_class_set_legacy_reset(dc, dw_dmac_reset);
}

static const TypeInfo dw_dmac_info = {
    .name = TYPE_DW_DMAC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DWDmacState),
    .instance_init = dw_dmac_instance_init,
    .class_init = dw_dmac_class_init,
};

static void dw_dmac_register_type(void){
    type_register_static(&dw_dmac_info);
}

type_init(dw_dmac_register_type);


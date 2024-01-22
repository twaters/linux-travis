#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/mtd/mtd.h> 
#include <linux/mtd/rawnand.h>
#include <linux/mtd/partitions.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/of_platform.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <asm/io.h>
#include <linux/sysfs.h>

#include <linux/dmaengine.h>
#include <linux/dma-direction.h>
#include <linux/dma-mapping.h>
#include <linux/completion.h>

#include <linux/ibeos_edge2.h>

#define NUM_CHIPS 8

/* NOTE:
 *
 * The FPGA buffer for NAND is double the size of a page + spare area. (4096+256 bytes)
 *
 * The reason for this is each byte is accompanied by a byte of hamming code, which is 5 data bits
 * per byte. This allows for SECDED of each byte in the buffer. This driver must read/write to that
 * buffer and take those bits into account. So, DMA is used to move this 0x2200 byte buffer to/from
 * the device, while the hamming encode/decode algorithm stores/loads the actual data in memory. */

/* NOTE:
 * 
 * This driver does not use the chip option NAND_USES_DMA, because the DMA does not go directly
 * to/from the NAND subsystem buffers. The DMA is used to get the data to/from the FPGA, but
 * the hamming encode/decode moves the calculated data into and out of that buffer. So, there is
 * no need to burden the NAND subsystem with knowledge of DMA.
 *
 * Interestingly enough, the nand subsystem now handles bounce buffers when virtual addresses
 * are used... thanks, finally! */
#define FLASH_PAGE_SIZE 4096
#define FLASH_OOB_SIZE 256
#define FLASH_BUF_SIZE (2*(FLASH_PAGE_SIZE+FLASH_OOB_SIZE))
#define FLASH_BUF_PHYS_ADDR 0x30000000
#define ENAND_DMA_TX 0
#define ENAND_DMA_RX 1

#define NUM_FLASH_TRIES 2
#define NUM_DMA_TRIES 2
#define FLASH_CMD_DELAY_MS 500

#define NAND_STAT_OIP 0x100
#define NAND_STAT_ECCBITS 0x7000

typedef enum edge2_nand_cmd {
    ENAND_CMD_NULL=0,
    ENAND_CMD_WRITE_DISABLE=4,
    ENAND_CMD_WRITE_ENABLE=6,
    ENAND_CMD_PAGE_PROG=0x10,
    ENAND_CMD_PAGE_READ=0x13,
    ENAND_CMD_SET_FEATURE=0x1f,
    ENAND_CMD_BLOCK_ERASE=0xd8,
    ENAND_CMD_RESET=0xff,
} edge2_nand_cmd_t;

typedef enum edge2_nand_regs {
    ENAND_REG_FLASH_ID=0xc,
    ENAND_REG_IRQE=0x20,
    ENAND_REG_IRQA=0x24,
    ENAND_REG_CMD = 0x100,
    ENAND_REG_ADDR=0x104,
    ENAND_REG_COUNT=0x108,
    ENAND_REG_DATA=0x10c,
    ENAND_REG_FEAT=0x110,
    ENAND_REG_PWR=0x120,
} edge2_nand_regs_t;

typedef enum edge2_nand_feat_index {
    ENAND_FEAT_BLOCK_LOCK,
    ENAND_FEAT_CONFIGURATION,
    ENAND_FEAT_DIE_SELECT, /* 0x40 for die 1, 0x00 for die 0 */
    ENAND_FEAT_INVALID
} edge2_nand_feat_index_t;

struct edge2_nand_mtd {
    struct nand_controller controller;
    struct nand_chip chip;

    struct device enand_dev; /* used for sysfs attributes */
    void *bar;
    int irq;

    int column; /* remember the last column from the last page */
    int cmd;

    struct dma_chan *chan[2];
    struct completion completion;

    struct completion cmd_complete;
    struct completion dma_complete;

    dma_addr_t bounce_buffer_phys;
    uint8_t *bounce_buffer;

    int die_cache;
    uint8_t cfg_cache;
    uint8_t cs;
    uint8_t status;

    uint64_t num_read_ops; /* number of flash read operations */
    uint64_t num_write_ops; /* number of flash write operations */
    uint64_t num_erase_ops; /* number of flash erase operations */
    uint64_t num_hwecc_bitflips; /* number of hwecc (on-die) bitflips (corrected) */
    uint64_t num_hwecc_uncor; /* number of hwecc (on-die) uncorrectable errors */
    uint64_t num_buf_bitflips; /* number of fpga buffer (hamming) corrected bit-flips */
    uint64_t num_buf_uncor; /* number of fpga buffer (hamming) uncorrected bit-flips */
    uint64_t num_dma_timeouts; /* number of dma timeouts */
    uint64_t num_powercycles; /* times the driver had to attempt powercycle to recover */
};

/* NOTE: since this is an embedded board with one FPGA, it is ok to have
 * a global for the state structure. If the NAND driver were part of the
 * overall PCI driver, then we wouldn't do that since it would be
 * part of the whole PCI system. However, it made sense to split it
 * off for now because someday this may go into the kernel and be
 * a configurable option. */
static struct edge2_nand_mtd enm;

static int edge2_nand_run_cmd(struct edge2_nand_mtd *enm, edge2_nand_cmd_t cmd, uint32_t addr, uint32_t data, int write_enable, int wait);

static const uint8_t enc_lookup[256] = {
    0x00, 0x13, 0x15, 0x06, 0x16, 0x05, 0x03, 0x10, 0x07, 0x14, 0x12, 0x01, 0x11, 0x02, 0x04, 0x17,
    0x19, 0x0a, 0x0c, 0x1f, 0x0f, 0x1c, 0x1a, 0x09, 0x1e, 0x0d, 0x0b, 0x18, 0x08, 0x1b, 0x1d, 0x0e,
    0x1a, 0x09, 0x0f, 0x1c, 0x0c, 0x1f, 0x19, 0x0a, 0x1d, 0x0e, 0x08, 0x1b, 0x0b, 0x18, 0x1e, 0x0d,
    0x03, 0x10, 0x16, 0x05, 0x15, 0x06, 0x00, 0x13, 0x04, 0x17, 0x11, 0x02, 0x12, 0x01, 0x07, 0x14,
    0x0b, 0x18, 0x1e, 0x0d, 0x1d, 0x0e, 0x08, 0x1b, 0x0c, 0x1f, 0x19, 0x0a, 0x1a, 0x09, 0x0f, 0x1c,
    0x12, 0x01, 0x07, 0x14, 0x04, 0x17, 0x11, 0x02, 0x15, 0x06, 0x00, 0x13, 0x03, 0x10, 0x16, 0x05,
    0x11, 0x02, 0x04, 0x17, 0x07, 0x14, 0x12, 0x01, 0x16, 0x05, 0x03, 0x10, 0x00, 0x13, 0x15, 0x06,
    0x08, 0x1b, 0x1d, 0x0e, 0x1e, 0x0d, 0x0b, 0x18, 0x0f, 0x1c, 0x1a, 0x09, 0x19, 0x0a, 0x0c, 0x1f,
    0x1c, 0x0f, 0x09, 0x1a, 0x0a, 0x19, 0x1f, 0x0c, 0x1b, 0x08, 0x0e, 0x1d, 0x0d, 0x1e, 0x18, 0x0b,
    0x05, 0x16, 0x10, 0x03, 0x13, 0x00, 0x06, 0x15, 0x02, 0x11, 0x17, 0x04, 0x14, 0x07, 0x01, 0x12,
    0x06, 0x15, 0x13, 0x00, 0x10, 0x03, 0x05, 0x16, 0x01, 0x12, 0x14, 0x07, 0x17, 0x04, 0x02, 0x11,
    0x1f, 0x0c, 0x0a, 0x19, 0x09, 0x1a, 0x1c, 0x0f, 0x18, 0x0b, 0x0d, 0x1e, 0x0e, 0x1d, 0x1b, 0x08,
    0x17, 0x04, 0x02, 0x11, 0x01, 0x12, 0x14, 0x07, 0x10, 0x03, 0x05, 0x16, 0x06, 0x15, 0x13, 0x00,
    0x0e, 0x1d, 0x1b, 0x08, 0x18, 0x0b, 0x0d, 0x1e, 0x09, 0x1a, 0x1c, 0x0f, 0x1f, 0x0c, 0x0a, 0x19,
    0x0d, 0x1e, 0x18, 0x0b, 0x1b, 0x08, 0x0e, 0x1d, 0x0a, 0x19, 0x1f, 0x0c, 0x1c, 0x0f, 0x09, 0x1a,
    0x14, 0x07, 0x01, 0x12, 0x02, 0x11, 0x17, 0x04, 0x13, 0x00, 0x06, 0x15, 0x05, 0x16, 0x10, 0x03
};

static void edge2_nand_pwr_enable(struct edge2_nand_mtd *enm, int enable)
{
    /* NOTE: if we do anything with NOR, we need to protect this register between
     * the two drivers */
    uint32_t reg = ioread32(enm->bar+ENAND_REG_PWR);
    if(enable) reg |= 2;
    else reg &= ~2;
    iowrite32(reg, enm->bar+ENAND_REG_PWR);
}

static void edge2_nand_irq_enable(struct edge2_nand_mtd *enm, int enable)
{
    enable = !!enable; /* make 0 or 1 */
    iowrite32(4*enable, enm->bar+ENAND_REG_IRQE); /* disable interrupt */
}

/* NOTE: len is the in buffer size. out buffer must be twice that size */
static void edge2_nand_ham_encode(const uint8_t *in, uint8_t *out, int len)
{
    for(int i=0; i<len; ++i) {
        out[i*2] = in[i];
        out[(i*2)+1] = enc_lookup[in[i]];
    }
}

/* TODO: sysfs stats for buffer errors */
static int edge2_nand_ham_decode(const uint8_t *in, uint8_t *out, int len)
{
    int ret=0;
    static const uint8_t SYN_CHECK = 0xf0;
    static const uint8_t SYN_UNCORRECTABLE = 0xff;

    static const uint8_t syn_lookup[16] = {
        0, /* no error */
        SYN_CHECK,
        SYN_CHECK,
        1<<0, /* bit position 0 */
        SYN_CHECK,
        1<<1, /* bit position 1 */
        1<<2, /* bit position 2 */
        1<<3, /* bit position 3 */
        SYN_CHECK,
        1<<4, /* bit position 4 */
        1<<5, /* bit position 5 */
        1<<6, /* bit position 6 */
        1<<7, /* bit position 7 */
        SYN_UNCORRECTABLE,
        SYN_UNCORRECTABLE,
        SYN_UNCORRECTABLE,
    };

    for(int i=0; i<len; ++i) {
        uint16_t val = *(const uint16_t*)(in+(i*2));
        const uint8_t lookup = enc_lookup[val&0xff];
        const uint8_t checkpar = (val&0x1f00)>>8;

        if(checkpar==lookup) {
            //data matches
            goto out_val;
        }
        else {
            uint8_t ccheck=0; /* calculated check */

            /* at this point, there's at least one bit error, so calculate error position */
            ccheck |= ((__builtin_popcount(val&((1<<0)|(1<<1)|(1<<3)|(1<<4)|(1<<6)))&1)<<0);
            ccheck |= ((__builtin_popcount(val&((1<<0)|(1<<2)|(1<<3)|(1<<5)|(1<<6)))&1)<<1);
            ccheck |= ((__builtin_popcount(val&((1<<1)|(1<<2)|(1<<3)|(1<<7)))&1)       <<2);
            ccheck |= ((__builtin_popcount(val&((1<<4)|(1<<5)|(1<<6)|(1<<7)))&1)       <<3);

            ccheck ^= (checkpar&0xf);
            if(!ccheck) {
                /* if we get here, the parity bit must be bad, so data matches */
                goto out_val;
            }

            /* if we get here, then there is either a single or double bit error */
            {
                const uint8_t slookup = syn_lookup[ccheck];
                const int cnt = __builtin_popcount(val);

                if(slookup==SYN_UNCORRECTABLE) {
                    /* uncorrectable error (syndrome points to invalid bit), so must indicate failure */
                    return -EBADMSG;
                }

                if(!(cnt&1)) {
                    /* double-bit error, data is uncorreectable */
                    return -EBADMSG;
                }

                if(slookup==SYN_CHECK) {
                    /* single bit error in check bits, so data is ok */
                    goto out_val;
                }

                /* at this point, single bit correctable error, so fix it */
                val ^= slookup;
                ++ret;
            }
        }

out_val:
        out[i] = val&0xff;
    }

    return ret;
}

static void edge2_nand_dma_complete(void *param)
{
    struct edge2_nand_mtd *enm = param;
    complete(&enm->dma_complete);
}

static int edge2_nand_do_dma_op(struct edge2_nand_mtd *enm, int len, enum dma_transfer_direction dir)
{
    struct dma_async_tx_descriptor *op;
    dma_cookie_t cookie;
    struct dma_slave_config sc;
    int ret;
    int cindex;
    int i;

    reinit_completion(&enm->dma_complete);
    //pr_info("dma with %p %p\n", enm->chan[0], enm->chan[1]);
    memset(&sc, 0, sizeof(sc));

    if(dir==DMA_MEM_TO_DEV) {
        cindex = ENAND_DMA_TX;
        sc.dst_addr = FLASH_BUF_PHYS_ADDR;
    }
    else {
        cindex = ENAND_DMA_RX;
        sc.src_addr = FLASH_BUF_PHYS_ADDR;
    }

    ret = dmaengine_slave_config(enm->chan[cindex], &sc);
    if(ret) {
        pr_err("can't config slave\n");
        return -EINVAL;
    }

    for(i=0; i<NUM_DMA_TRIES; ++i) {
        op = dmaengine_prep_slave_single(enm->chan[cindex], enm->bounce_buffer_phys, len, dir, 0);
        //pr_info("dma with op %p\n", op);
        op->callback = edge2_nand_dma_complete;
        op->callback_param = enm;

        cookie = dmaengine_submit(op);
        ret = dma_submit_error(cookie);
        if(ret) {
            pr_err("dma_submit_error %d\n", cookie);
            return -EIO;
        }

        dma_async_issue_pending(enm->chan[cindex]);

        ret = wait_for_completion_timeout(&enm->dma_complete, msecs_to_jiffies(250));
        if(ret>0) break;

        /* ret holds number of jiffies left, so 0 is timeout */
        dmaengine_terminate_all(enm->chan[cindex]);
    }

    if(i==NUM_DMA_TRIES) {
        pr_err("dma timeout\n");
        enm->status = NAND_STATUS_FAIL;
        return -ETIMEDOUT;
    }

    return 0;
}

static void edge2_nand_abort(struct edge2_nand_mtd *enm)
{
    reinit_completion(&enm->cmd_complete);
    reinit_completion(&enm->dma_complete);
}

static int edge2_nand_set_features(struct edge2_nand_mtd *enm, edge2_nand_feat_index_t index, uint8_t val)
{
    static const uint8_t addrs[] = { 0xa0, 0xb0, 0xd0 };

    BUG_ON(index>=ENAND_FEAT_INVALID);

    if(index==ENAND_FEAT_CONFIGURATION) {
        if(val==enm->cfg_cache) return 0;
        enm->cfg_cache = val;
    }

    return edge2_nand_run_cmd(enm, ENAND_CMD_SET_FEATURE, addrs[index], val, 0, 0);
}

static void edge2_nand_set_addr(struct edge2_nand_mtd *enm, uint32_t page)
{
    const int die = page&(1<<17);
    page &= 0x1ffff;

    page |= (enm->cs<<17);

    if(die!=enm->die_cache) {
        static const uint8_t die_vals[2] = { 0, 0x40 };
        enm->die_cache = die;
        edge2_nand_set_features(enm, ENAND_FEAT_DIE_SELECT, die_vals[!!die]);
    }

    iowrite32(page, enm->bar+ENAND_REG_ADDR);
}

static int edge2_nand_do_reset(struct edge2_nand_mtd *enm)
{
    int ret = edge2_nand_run_cmd(enm, ENAND_CMD_RESET, 0, 0, 0, 1);
    if(ret) return ret;

    ret = edge2_nand_set_features(enm, ENAND_FEAT_BLOCK_LOCK, 0);
    if(ret) return ret;

    ret = edge2_nand_set_features(enm, ENAND_FEAT_DIE_SELECT, 0);
    if(ret) return ret;

    ret = edge2_nand_set_features(enm, ENAND_FEAT_CONFIGURATION, 0x10); /* enable ECC */
    if(ret) return ret;

    return 0;
}
        
static int edge2_nand_run_cmd(struct edge2_nand_mtd *enm, edge2_nand_cmd_t cmd, uint32_t addr, uint32_t data, int write_enable, int wait)
{
    int i;

    for(i=0; i<NUM_FLASH_TRIES; ++i) {
        iowrite32(FLASH_PAGE_SIZE+FLASH_OOB_SIZE, enm->bar+ENAND_REG_COUNT);
        if(cmd==ENAND_CMD_SET_FEATURE) {
            iowrite32(data, enm->bar+ENAND_REG_DATA);
            iowrite32(addr, enm->bar+ENAND_REG_ADDR);
        }
        else {
            edge2_nand_set_addr(enm, addr);

            if(write_enable) {
                iowrite32(ENAND_CMD_WRITE_ENABLE, enm->bar+ENAND_REG_CMD);
            }
        }

        if(wait) reinit_completion(&enm->cmd_complete);
        iowrite32(cmd, enm->bar+ENAND_REG_CMD);
        if(wait) {
            int ret;

            ret = wait_for_completion_timeout(&enm->cmd_complete, msecs_to_jiffies(500));
            if(ret>0) {
                /* didn't time out, so check nand status for operation failed */
                ret = ioread32(enm->bar+ENAND_REG_FEAT);
                if((ret&NAND_STAT_ECCBITS)!=NAND_STAT_ECCBITS) break;
            }

            //pr_info("timeout! %x\n", cmd);
            if(cmd==ENAND_CMD_RESET) return -ETIMEDOUT; /* can't recover from failed reset */

            ++enm->num_powercycles;
            edge2_nand_abort(enm);

            edge2_nand_pwr_enable(enm, 0);
            msleep(5);
            edge2_nand_irq_enable(enm, 0);
            edge2_nand_pwr_enable(enm, 1);

            /* at this point, NAND is powering back on, and should trigger OIP bit */
            msleep(5);
            //pr_info("status %x\n", ioread32(enm->bar+ENAND_REG_FEAT));
            if(ioread32(enm->bar+ENAND_REG_FEAT)&NAND_STAT_OIP) return -ETIMEDOUT; /* OIP never went low */

            iowrite32(7, enm->bar+ENAND_REG_IRQA); /* reset interrupts */
            edge2_nand_irq_enable(enm, 1);

            ret = edge2_nand_do_reset(enm);
            //pr_info("reset? %d\n", ret);
            if(ret) return -EIO; /* failed to re-initialize */
        }
        else break;
    }

    if(i==NUM_FLASH_TRIES) {
        pr_err("enand: COMMAND %d timeout\n", cmd);
        enm->status = NAND_STATUS_FAIL;
        return -ETIMEDOUT; /* TODO: ask Larry what to do if timeout */
    }

    return 0;
}

static int edge2_nand_page_read(struct edge2_nand_mtd *enm, void *buf, int buflen, void *oob_buf, int page, bool ecc)
{
    struct mtd_info *mtd = nand_to_mtd(&enm->chip);
    int ret;
    int max_bitflips=0;
    uint8_t ecc_stat;
    uint8_t cfg = enm->cfg_cache;
    enm->cs = enm->chip.cur_cs;

    if(ecc) cfg |= 0x10;
    else cfg &= ~0x10;
    edge2_nand_set_features(enm, ENAND_FEAT_CONFIGURATION, cfg);

    ++enm->num_read_ops;
    ret = edge2_nand_run_cmd(enm, ENAND_CMD_PAGE_READ, page, 0, 0, 1);
    if(ret) return ret;

    /* check on on-chip ecc errors */
    if(ecc) {
        ecc_stat = ((ioread32(enm->bar+ENAND_REG_FEAT)&(NAND_STAT_ECCBITS))>>12)&7;
        switch(ecc_stat) {
            case 0:
                max_bitflips=0;
                break;
            case 1:
                max_bitflips=3;
                break;
            case 2:
                max_bitflips=-EBADMSG;
                break;
            case 3:
                max_bitflips=6;
                break;
            case 5:
                max_bitflips=8;
                break;
            default:
                /* chip shows 4 on an empty page, so don't print anything */
                max_bitflips=0;
                break;
        }

        if(max_bitflips<0) {
            ++enm->num_hwecc_uncor;
            ++mtd->ecc_stats.failed;
            return 0;
        }
        else {
            enm->num_hwecc_bitflips += max_bitflips;
            mtd->ecc_stats.corrected += max_bitflips;
        }
    }

    ret = edge2_nand_do_dma_op(enm, FLASH_BUF_SIZE, DMA_DEV_TO_MEM);
    if(ret) return ret;

    if(buf) {
        ret = edge2_nand_ham_decode(enm->bounce_buffer, buf, buflen);
        /* TODO: sysfs show hamming stats */
        if(ret<0) {
            ++enm->num_buf_uncor;
            return ret;
        }
        enm->num_buf_bitflips += ret;
    }

    if(oob_buf) {
        ret = edge2_nand_ham_decode(enm->bounce_buffer+(FLASH_PAGE_SIZE*2), oob_buf, FLASH_OOB_SIZE);
        /* TODO: sysfs show hamming stats */
        if(ret<0) {
            ++enm->num_buf_uncor;
            return ret;
        }
        enm->num_buf_bitflips += ret;
    }

    return max_bitflips;
}

static int edge2_nand_page_write(struct edge2_nand_mtd *enm, const void *buf, int buflen, const void *oob_buf, int page, bool ecc)
{
    int ret;
    static const uint16_t empty = (enc_lookup[0xff]<<8)|0xff;
    uint8_t cfg = enm->cfg_cache;

    if(ecc) cfg |= 0x10;
    else cfg &= ~0x10;
    edge2_nand_set_features(enm, ENAND_FEAT_CONFIGURATION, cfg);

    ++enm->num_write_ops;

    edge2_nand_ham_encode(buf, enm->bounce_buffer, buflen); /* ok if buflen is 0 */
    if(buflen<FLASH_PAGE_SIZE) {
        for(int i=buflen; i<FLASH_PAGE_SIZE; ++i) memcpy(enm->bounce_buffer+(buflen*2)+(i*2), &empty, sizeof(empty));
    }

    if(oob_buf) edge2_nand_ham_encode(oob_buf, enm->bounce_buffer+(FLASH_PAGE_SIZE*2), FLASH_OOB_SIZE);
    else {
        for(int i=0; i<FLASH_OOB_SIZE; ++i) memcpy(enm->bounce_buffer+(FLASH_PAGE_SIZE*2)+(i*2), &empty, sizeof(empty));
    }

    ret = edge2_nand_do_dma_op(enm, FLASH_BUF_SIZE, DMA_MEM_TO_DEV);
    if(ret) return ret;

    return edge2_nand_run_cmd(enm, ENAND_CMD_PAGE_PROG, page, 0, 1, 1);
}

static inline struct edge2_nand_mtd *chip_to_enm(struct nand_chip *chip)
{
    return container_of(chip, struct edge2_nand_mtd, chip);
}

static inline struct edge2_nand_mtd *dev_to_enm(struct device *dev)
{
    return container_of(dev, struct edge2_nand_mtd, enand_dev);
}

static int edge2_nand_read_page(struct nand_chip *chip, u8 *buf, int oob_required, int page)
{
    return edge2_nand_page_read(chip_to_enm(chip), buf, FLASH_PAGE_SIZE, oob_required ? chip->oob_poi:NULL, page, true);
}

static int edge2_nand_read_page_raw(struct nand_chip *chip, u8 *buf, int oob_required, int page)
{
    return edge2_nand_page_read(chip_to_enm(chip), buf, FLASH_PAGE_SIZE, oob_required ? chip->oob_poi:NULL, page, false);
}

static int edge2_nand_write_page(struct nand_chip *chip, const u8 *buf, int oob_required, int page)
{
    return edge2_nand_page_write(chip_to_enm(chip), buf, FLASH_PAGE_SIZE, chip->oob_poi, page, true);
}

static int edge2_nand_write_page_raw(struct nand_chip *chip, const u8 *buf, int oob_required, int page)
{
    return edge2_nand_page_write(chip_to_enm(chip), buf, FLASH_PAGE_SIZE, chip->oob_poi, page, false);
}

static int edge2_nand_read_oob(struct nand_chip *chip, int page)
{
    return edge2_nand_page_read(chip_to_enm(chip), NULL, 0, chip->oob_poi, page, true);
}

static int edge2_nand_write_oob(struct nand_chip *chip, int page)
{
    return edge2_nand_page_write(chip_to_enm(chip), NULL, 0, chip->oob_poi, page, true);
}

static int edge2_nand_ooblayout_ecc(struct mtd_info *mtd, int section, struct mtd_oob_region *oobregion)
{
    if(section) return -ERANGE;

    oobregion->offset = 0x80;
    oobregion->length = 0x80;

    return 0;
}

static int edge2_nand_ooblayout_free(struct mtd_info *mtd, int section, struct mtd_oob_region *oobregion)
{
    if(section) return -ERANGE;

    oobregion->offset = 0;
    oobregion->length = 0x80;

    return 0;
}

static int edge2_nand_attach_chip(struct nand_chip *chip)
{
    struct mtd_info *mtd = nand_to_mtd(&enm.chip);
    static const struct mtd_ooblayout_ops edge2_nand_oob_ops = {
        .ecc = edge2_nand_ooblayout_ecc,
        .free = edge2_nand_ooblayout_free,
    };

    chip->ecc.engine_type = NAND_ECC_ENGINE_TYPE_ON_HOST;

    chip->bbt_options = NAND_BBT_USE_FLASH;
    chip->options = NAND_NO_SUBPAGE_WRITE|NAND_ROW_ADDR_3;

    chip->ecc.size = 512;
    chip->ecc.bytes = 13;
    chip->ecc.strength = 8;

    chip->ecc.read_page = edge2_nand_read_page;
	chip->ecc.write_page = edge2_nand_write_page;
	chip->ecc.read_page_raw = edge2_nand_read_page_raw;
	chip->ecc.write_page_raw = edge2_nand_write_page_raw;
	chip->ecc.read_oob = edge2_nand_read_oob;
	chip->ecc.write_oob = edge2_nand_write_oob;

    mtd_set_ooblayout(mtd, &edge2_nand_oob_ops);

    return 0;
}

static int edge2_nand_cmd(struct nand_chip *chip, const struct nand_subop *op)
{
    struct edge2_nand_mtd *enm = chip_to_enm(chip);
    int i, ret;
    uint8_t cmd=0;
    uint32_t addr=0;
    const struct nand_op_data_instr *data;

    for(i=0; i<op->ninstrs; ++i) {
        switch(op->instrs[i].type) {
            case NAND_OP_CMD_INSTR:
                cmd = op->instrs[i].ctx.cmd.opcode;
                switch(cmd) {
                    case NAND_CMD_RNDOUT:
                    case NAND_CMD_RNDOUTSTART:
                        enm->column = addr;
                    break;
                    default:
                        enm->column = 0;
                        enm->cmd = 0;
                    break;
                }
                break;
            case NAND_OP_ADDR_INSTR:
                for(int j=op->instrs[i].ctx.addr.naddrs; j>0; --j) {
                    addr <<= 8;
                    addr |= op->instrs[i].ctx.addr.addrs[j-1];
                }
                break;
            case NAND_OP_DATA_IN_INSTR: {
                /* assume page is already read in (parameter page */
                /* TODO: refactor handling parameter read */
                data = &op->instrs[i].ctx.data;
                if((enm->cmd==NAND_CMD_PARAM)&&(enm->column>0)) {
                    if(!data) return -EINVAL;
                    if((enm->column+data->len)>PAGE_SIZE) return -ERANGE;
                    ret = edge2_nand_ham_decode(enm->bounce_buffer+enm->column, data->buf.in, data->len);
                    enm->column += data->len;
                }
                break;
            }
            case NAND_OP_DATA_OUT_INSTR:
                data = &op->instrs[i].ctx.data;
                break;
            default:;
        }
    }

    /* special command handling */
    switch(cmd) {
        case NAND_CMD_RESET:
            return edge2_nand_do_reset(enm);
        break;
        case NAND_CMD_READID: {
            if((addr==0x20)&&(data->len>=4)) {
                memcpy(data->buf.in, "ONFI", 4);
            }
            else {
                uint32_t id = ioread32(enm->bar+ENAND_REG_FLASH_ID);
                ((uint8_t*)data->buf.in)[0] = (id>>24);
                ((uint8_t*)data->buf.in)[1] = (id>>16);
            }
        }
        break;    
        case NAND_CMD_PARAM: {
            ret = edge2_nand_set_features(enm, ENAND_FEAT_CONFIGURATION, 0x40);
            if(ret) return ret;
            if(!enm->column) ret = edge2_nand_page_read(enm, data->buf.in, data->len, NULL, 1, false); /* page 1 for parameter page, ecc doesn't matter */
            enm->column += data->len;
            enm->cmd = NAND_CMD_PARAM;
            //for(blah=0; blah<256; ++blah) pr_info("%x", ((uint8_t*)(data->buf.in))[blah]);
            //pr_info("\n");
            edge2_nand_set_features(enm, ENAND_FEAT_CONFIGURATION, 0x10); /* even if flash is in bad state, attempt to reset configuration */
            if(ret) return ret;
        }
        break;
        case NAND_CMD_STATUS:
            ((uint8_t*)data->buf.in)[0] = enm->status;
            enm->status = NAND_STATUS_READY|NAND_STATUS_WP;
        break;
        case NAND_CMD_ERASE2:
            //pr_info("erase: 0x%x %d %x %x %x\n", addr, enm->cs, chip->pagemask, chip->phys_erase_shift, chip->page_shift);
            ++enm->num_erase_ops;

            ret = edge2_nand_run_cmd(enm, ENAND_CMD_BLOCK_ERASE, addr, 0, 1, 1);
            if(ret) return ret;
        break;
        default:;
    }

    return 0;
}

static const struct nand_op_parser edge2_nand_op_parser = NAND_OP_PARSER(
    NAND_OP_PARSER_PATTERN(edge2_nand_cmd,
        NAND_OP_PARSER_PAT_CMD_ELEM(false),
        NAND_OP_PARSER_PAT_ADDR_ELEM(false, 2),
        NAND_OP_PARSER_PAT_CMD_ELEM(false),
        NAND_OP_PARSER_PAT_WAITRDY_ELEM(true),
        NAND_OP_PARSER_PAT_DATA_IN_ELEM(true,256)),
    NAND_OP_PARSER_PATTERN(edge2_nand_cmd,
        NAND_OP_PARSER_PAT_CMD_ELEM(true),
        NAND_OP_PARSER_PAT_ADDR_ELEM(true, 3),
        NAND_OP_PARSER_PAT_DATA_OUT_ELEM(true, 4096+256),
        NAND_OP_PARSER_PAT_CMD_ELEM(true),
        NAND_OP_PARSER_PAT_WAITRDY_ELEM(true)),
    NAND_OP_PARSER_PATTERN(edge2_nand_cmd,
        NAND_OP_PARSER_PAT_CMD_ELEM(true),
        NAND_OP_PARSER_PAT_ADDR_ELEM(true, 3),
        NAND_OP_PARSER_PAT_CMD_ELEM(true),
        NAND_OP_PARSER_PAT_WAITRDY_ELEM(true),
        NAND_OP_PARSER_PAT_DATA_IN_ELEM(true, 4096+256)));

static int edge2_nand_exec_op(struct nand_chip *chip, const struct nand_operation *op, bool check_only)
{
    struct edge2_nand_mtd *enm = chip_to_enm(chip);

    if(!check_only) enm->cs = op->cs;
    enm->cs = op->cs;

    return nand_op_parser_exec_op(chip, &edge2_nand_op_parser, op, check_only);
}

static irqreturn_t edge2_nand_isr(int irq, void *data)
{
    struct edge2_nand_mtd *enm = (struct edge2_nand_mtd*)data;
    if(ioread32(enm->bar+ENAND_REG_IRQA)&4) {
        iowrite32(4, enm->bar+ENAND_REG_IRQA); /* reset interrupt */
        complete(&enm->cmd_complete);
        return IRQ_HANDLED;
    }

    return IRQ_NONE;
}

static void enand_release(struct device *dev)
{
    /* nothing to do, but function is required */
}

#define SHOW_FN(x) \
static ssize_t x##_show(struct device *dev, struct device_attribute *attr, char *buf) \
{ \
    struct edge2_nand_mtd *enm = dev_to_enm(dev); \
    return sprintf(buf, "%lld\n", enm->x); \
} \
static DEVICE_ATTR_RO(x);

SHOW_FN(num_read_ops);
SHOW_FN(num_write_ops);
SHOW_FN(num_erase_ops);
SHOW_FN(num_hwecc_bitflips);
SHOW_FN(num_hwecc_uncor);
SHOW_FN(num_buf_bitflips);
SHOW_FN(num_buf_uncor);
SHOW_FN(num_dma_timeouts);
SHOW_FN(num_powercycles);

static struct attribute *enand_attrs[] = {
    &dev_attr_num_read_ops.attr,
    &dev_attr_num_write_ops.attr,
    &dev_attr_num_erase_ops.attr,
    &dev_attr_num_hwecc_bitflips.attr,
    &dev_attr_num_hwecc_uncor.attr,
    &dev_attr_num_buf_bitflips.attr,
    &dev_attr_num_buf_uncor.attr,
    &dev_attr_num_dma_timeouts.attr,
    &dev_attr_num_powercycles.attr,
    NULL,
};
ATTRIBUTE_GROUPS(enand);

static struct class enand_class = {
    .name = "enand",
    .dev_release = enand_release,
    .dev_groups = enand_groups
};

static int __init init_edge2_nand(void)
{
    static const struct nand_controller_ops edge2_nand_controller_ops = {
        .attach_chip = edge2_nand_attach_chip,
        .exec_op = edge2_nand_exec_op,
    };

    struct mtd_info *mtd;
    int ret;

    memset(&enm, 0, sizeof(enm));
    enm.bar = ibeos_edge2_get_bar(0, 2);

    enm.status = NAND_STATUS_READY | NAND_STATUS_WP;
    init_completion(&enm.cmd_complete);
    init_completion(&enm.dma_complete);

    nand_controller_init(&enm.controller);
    enm.controller.ops = &edge2_nand_controller_ops;

    enm.chip.controller = &enm.controller;

    mtd = nand_to_mtd(&enm.chip);
    mtd->owner = THIS_MODULE;
    mtd->writesize = 4096;
    mtd->oobsize=256; /* doesn't seem to hurt, and it fixes problem on 6.6 kernel */

    enm.bounce_buffer = dma_alloc_coherent(ibeos_edge2_get_dev(0), FLASH_BUF_SIZE, &enm.bounce_buffer_phys, GFP_KERNEL);
    if(!enm.bounce_buffer) return -ENOMEM;

    for(int d=0; d<2; ++d) {
        static const char *names[] = { "tx", "rx" };
        enm.chan[d] = dma_request_chan(ibeos_edge2_get_dev(0), names[d]);
        if(!enm.chan[d]) {
            ret = -ENODEV;
            pr_err("failed to get dma chan %d\n", d);
            goto out_bounce;
        }
    }

    edge2_nand_irq_enable(&enm, 0); /* disable interrupt */
    iowrite32(7, enm.bar+ENAND_REG_IRQA); /* reset interrupts */
    enm.irq = ibeos_edge2_get_virq(0, 24);
    ret = request_irq(enm.irq, edge2_nand_isr, 0, "enand isr", &enm);
    if(ret) {
        pr_err("failed to get irq\n");
        ret = -ENODEV;
        goto out_chan;
    }
    edge2_nand_irq_enable(&enm, 1); /* enable interrupt */

    ret = class_register(&enand_class);
    if(ret) goto out_irq;

    device_initialize(&enm.enand_dev);
    enm.enand_dev.parent = get_device(ibeos_edge2_get_dev(0));
    enm.enand_dev.class = &enand_class;
    dev_set_name(&enm.enand_dev, "enand");

    ret = device_add(&enm.enand_dev);
    if(ret) {
        put_device(&enm.enand_dev);
        goto out_class;
    }

    ret = nand_scan(&enm.chip, 8);
    if(ret) goto out_dev;

    ret = mtd_device_register(mtd, NULL, 0);
    if(!ret) return 0;

out_dev:
    device_del(&enm.enand_dev);

out_class:
    class_unregister(&enand_class);

out_irq:
    free_irq(enm.irq, &enm);

out_chan:
    for(int d=0; d<2; ++d) dma_release_channel(enm.chan[d]);

out_bounce:
    dma_free_coherent(ibeos_edge2_get_dev(0), FLASH_BUF_SIZE, enm.bounce_buffer, enm.bounce_buffer_phys);

    return ret;
}

static void __exit exit_edge2_nand(void)
{
    struct mtd_info *mtd = nand_to_mtd(&enm.chip);

    edge2_nand_irq_enable(&enm, 0); /* disable interrupt */

    device_del(&enm.enand_dev);
    class_unregister(&enand_class);

    for(int d=0; d<2; ++d) dma_release_channel(enm.chan[d]);
    dma_free_coherent(ibeos_edge2_get_dev(0), FLASH_BUF_SIZE, enm.bounce_buffer, enm.bounce_buffer_phys);

    free_irq(enm.irq, &enm);
    mtd_device_unregister(mtd);
    nand_cleanup(&enm.chip);
}

module_init(init_edge2_nand);
module_exit(exit_edge2_nand);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Steven Seeger");
MODULE_DESCRIPTION("MTD driver for Ibeos Edge-2 NAND");

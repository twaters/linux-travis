#ifndef IBEOS_EDGE2_DMA_INTERNAL__H
#define IBEOS_EDGE2_DMA_INTERNAL__H

#include <linux/ibeos_edge1100.h>
#include "../../dma/virt-dma.h"

/* taken from microsemi demo, but I am not a fan of bitfields in this way */
struct ibeos_edge_dma_desc {
    u32 status_num :4;
    u32 prc_status :4;
    u32 prc_page_size :24;
    u32 status_req :1;
    u32 type :3;
    u32 irq :4;
    u32 page_size :24;
    u32 se_cond :4;
    u32 next_rdy :1;
    u64 next_addr :59;
    u64 src_addr;
    u64 dest_addr;
};

struct ibeos_edge2_dma_hw_desc {
    struct ibeos_edge_dma_desc *dv;
    dma_addr_t dp;
    dma_addr_t phys;
    uint32_t len; /* per-desc len */
    uint32_t offset;
};

struct ibeos_edge2_dma_desc {
    struct virt_dma_desc vdesc;
    struct ibeos_edge2_dma_hw_desc *hw;
    struct ibeos_edge2_vdma *parent;
    uint32_t ndesc;
    size_t len; /* total dma len */
    enum dma_status status;
};

struct ibeos_edge2_dma_cdev_state {
    struct ibeos_edge2_dma_cdev *parent;
    struct dma_chan *chan;
    struct completion dma_comp;
};

struct ibeos_edge2_dma {
    struct dma_device dma_dev;
    struct ibeos_edge2_state *parent;
    void __iomem *regs[NUM_PDMA];
    /* since building dma into the driver, can't use devm_request_threaded_irq
     * so keep track of irq */

    int irq_flag;

    struct ibeos_edge2_vdma {
        struct virt_dma_chan vchan;
        struct ibeos_edge2_dma *parent;
        struct dma_slave_config config;
        void *pool;

        struct ibeos_edge2_dma_desc *desc;
    } chans[NUM_VDMA]; /* first half W, then R */

    struct ibeos_edge2_dma_cdev {
        struct cdev cdev;
        struct ibeos_edge2_dma *parent;
        const char *name;
    } cdevs[NUM_PDMA];

    struct ibeos_edge2_vdma *active[NUM_PDMA];
};

#endif

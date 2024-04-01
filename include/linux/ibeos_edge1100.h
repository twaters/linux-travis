/* Copyright 2021 Ibeos */

#ifndef IBEOS_EDGE1100__H
#define IBEOS_EDGE1100__H

#include <linux/device.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/irqdomain.h>
#include <linux/wait.h>

#include <linux/ibeos_edge1100_dma.h>

#define IBEOS_EDGE1100_DRVNAME "ibeos_edge1100"

#define NUM_BARS 6
#define NUM_IRQS 32

#define IRQ_IN 0
#define IRQ_OUT 1
#define IRQ_RUNNING 2
#define IRQ_NUM_FLOW 3

/* two irq per channel */
#define NUM_DMA_IRQ 2

/* number of virtual dma channels for each physical channel */
#define NUM_VDMA 8
#define NUM_PDMA 2

#if IS_ENABLED(CONFIG_PCIE_IBEOS_EDGE1100_DMA)
struct ibeos_edge2_dma;
#endif

typedef struct ibeos_edge2_state {
    struct pci_dev *pdev;

    char name[64];

    void __iomem *base[NUM_BARS]; /* NOTE: only 0 is used for now */
    size_t bar_size[NUM_BARS]; /* NOTE: if bar_size is non-zero, then associated chardevs are created */

    uint32_t nvec;

    struct ibeos_edge2_bar {
        struct ibeos_edge2_state *parent;
        struct cdev cdev;
    } bars[NUM_BARS];

    struct irq_domain *domain;
    struct fwnode_handle *fwnode;

    struct ibeos_edge2_irq {
        struct ibeos_edge2_state *parent;
        struct cdev cdev;
        int ev[IRQ_NUM_FLOW];
        wait_queue_head_t wq[IRQ_NUM_FLOW];
        char name[16];
    } irqs[NUM_IRQS];

#if IS_ENABLED(CONFIG_PCIE_IBEOS_EDGE1100_DMA)
    struct ibeos_edge2_dma *dma;
#endif
} ibeos_edge2_state;
#endif

#define PF_IMASK_LOCAL      0x4180
#define PF_ISTATUS_LOCAL    0x4184
#define PF_IMASK_HOST       0x4188
#define PF_ISTATUS_HOST     0x418c

#define PF_DMA0_OFFSET      0x4400
#define PF_DMA1_OFFSET      0x4440

/* the following addrs are offset from either PF_DMA0_OFFSET or PF_DMA1_OFFSET */
#define PF_DMA_SRC_PARAM    0x0
#define PF_DMA_DST_PARAM    0x4
#define PF_DMA_SRC_LSW      0x8
#define PF_DMA_SRC_MSW      0xc
#define PF_DMA_DST_LSW      0x10
#define PF_DMA_DST_MSW      0x14
#define PF_DMA_LEN          0x18
#define PF_DMA_CTRL         0x1c
#define PF_DMA_STAT         0x20
#define PF_DMA_PRC_LEN      0x24
#define PF_DMA_SHARE_ACC    0x28

extern dev_t ibeos_edge2_devt;
extern struct class *ibeos_edge2_class;

extern int ibeos_edge2_get_virq(int, int);
extern struct device *ibeos_edge2_get_dev(int devnum);
extern void *ibeos_edge2_get_bar(int devnum, int bar);
extern size_t ibeos_edge2_get_bar_size(int devnum, int bar);

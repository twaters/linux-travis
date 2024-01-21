/* Copyright 2021 Ibeos */

#ifndef IBEOS_EDGE2_DRIVER__H
#define IBEOS_EDGE2_DRIVER__H

#include <linux/types.h>
#include <asm/ioctl.h>
#ifndef __KERNEL__
#include <stdint.h>
#endif

#define MAX_DEVICES 1 /* change this if you need */
#if MAX_DEVICES>26
#error limit of max devices is 26 for naming purposes
#endif

struct ibeos_edge2_dma_args {
    void *buf;
    uint32_t len;
    uint32_t axi_addr;
};

#define IBEOS_EDGE2_IOCTL_GET_BAR_SIZE      _IOR('A', 0x10, uint64_t)
#define IBEOS_EDGE2_IOCTL_RDMA              _IOWR('A', 0x11, struct ibeos_edge2_dma_args*)
#define IBEOS_EDGE2_IOCTL_WDMA              _IOWR('A', 0x12, struct ibeos_edge2_dma_args*)

#endif


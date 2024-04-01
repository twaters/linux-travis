/* Copyright 2021 Ibeos */

#ifndef IBEOS_EDGE2_DMA__H
#define IBEOS_EDGE2_DMA__H

#include <linux/types.h>

struct ibeos_edge2_state;
int ibeos_edge_dma_init(struct ibeos_edge2_state *pps);
void ibeos_edge_dma_destroy(struct ibeos_edge2_state *pps);

#endif


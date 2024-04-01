/* Copyright 2021 Ibeos */

#ifndef IBEOS_EDGE2_MEM__H
#define IBEOS_EDGE2_MEM__H

#include <linux/types.h>

struct ibeos_edge2_state;
void ibeos_edge_mem_cdev_create(struct ibeos_edge2_state *pps, int bar_num);
void ibeos_edge_mem_cdev_destroy(struct ibeos_edge2_state *pps, int bar_num);

#endif


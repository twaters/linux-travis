/* Copyright 2021 Ibeos */

#ifndef IBEOS_EDGE2_IRQ__H
#define IBEOS_EDGE2_IRQ__H

#include <linux/types.h>

struct ibeos_edge2_state;
int ibeos_edge_irq_init(struct ibeos_edge2_state *pps);
void ibeos_edge_irq_destroy(struct ibeos_edge2_state *pps);

void ibeos_edge2_cascade_isr(struct irq_desc *desc);

void ibeos_edge_irq_cdev_create(struct ibeos_edge2_state *pps, int irq_num);
void ibeos_edge_irq_cdev_destroy(struct ibeos_edge2_state *pps, int irq_num);

#endif


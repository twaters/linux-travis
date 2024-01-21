#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/irq.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/poll.h>

#include <linux/ibeos_edge2.h>
#include <linux/ibeos_edge2_driver.h>

static DEFINE_RAW_SPINLOCK(ibeos_edge2_spinlock);

void ibeos_edge2_cascade_isr(struct irq_desc *desc)
{
    ibeos_edge2_state *pps = irq_desc_get_handler_data(desc);
    struct irq_chip *chip = irq_desc_get_chip(desc);
    int msi_number = (irq_desc_get_irq(desc) - pps->pdev->irq);
    uint32_t stat = ioread32(pps->base[0] + PF_ISTATUS_HOST);
    const uint32_t mask = ioread32(pps->base[0] + PF_IMASK_HOST);
    
    stat &= mask;
    chained_irq_enter(chip, desc);
    
    /* loop over the bits of PF_ISTATUS_HOST with a mod of nvec */
    while(msi_number<NUM_IRQS) {
        if(stat&(1<<msi_number)) {
            //dev_info(&pps->pdev->dev, "got virq %d\n", msi_number);
            generic_handle_irq(irq_linear_revmap(pps->domain, msi_number));
        }
        msi_number += pps->nvec;
    }
    chained_irq_exit(chip, desc);
}

static irqreturn_t ibeos_edge_cdev_isr(int irq, void *priv)
{
    struct ibeos_edge2_irq *ppi = priv;

    //dev_info(&ppi->parent->pdev->dev, "got irq %d\n", irq);

    ppi->ev[IRQ_RUNNING] = 1;

    ppi->ev[IRQ_IN] = 1;
    ppi->ev[IRQ_OUT] = 0;
    wake_up_interruptible(&ppi->wq[IRQ_IN]);

    //dev_info(&ppi->parent->pdev->dev, "wait irq %d\n", irq);
    //ppi->ev[IRQ_OUT] = 0;
    wait_event(ppi->wq[IRQ_OUT], ppi->ev[IRQ_OUT]);
    ppi->ev[IRQ_OUT] = 0;

    ppi->ev[IRQ_RUNNING] = 0;
    wake_up_interruptible(&ppi->wq[IRQ_RUNNING]);

    //dev_info(&ppi->parent->pdev->dev, "out irq %d\n", irq);
    return IRQ_HANDLED;
}

static int ibeos_edge_irq_cdev_open(struct inode *inode, struct file *file)
{
    struct ibeos_edge2_irq *ppi = container_of(inode->i_cdev, struct ibeos_edge2_irq, cdev);
    const int index = ppi-ppi->parent->irqs;
    int ret;
    const int irq = irq_linear_revmap(ppi->parent->domain, index);

    file->private_data = ppi;

    memset(ppi->ev, 0, sizeof(ppi->ev));

    ret = request_threaded_irq(irq, NULL, ibeos_edge_cdev_isr, IRQF_ONESHOT, ppi->name, ppi);
    return 0;
}

static int ibeos_edge_irq_cdev_release(struct inode *inode, struct file *file)
{
    struct ibeos_edge2_irq *ppi = file->private_data;
    const int index = ppi-ppi->parent->irqs;
    const int irq = irq_linear_revmap(ppi->parent->domain, index);

    //disable_irq_nosync(irq); /* TEMP TEMP TEMP */
    /* file is closing, so allow interrupt thread to wake if needed */
    if(ppi->ev[IRQ_RUNNING]) {
        /* interrupt thread is waiting for IRQ_OUT, but file is closing. allow it to exit gracefully */
        ppi->ev[IRQ_OUT] = 1;
        wake_up(&ppi->wq[IRQ_OUT]);
        wait_event_interruptible(ppi->wq[IRQ_RUNNING], !ppi->ev[IRQ_RUNNING]);
    }

    free_irq(irq, ppi);
    return 0;
}

static __poll_t ibeos_edge_irq_cdev_poll(struct file *file, poll_table *wait)
{
    struct ibeos_edge2_irq *ppi = file->private_data;

    poll_wait(file, &ppi->wq[IRQ_IN], wait);
    if(ppi->ev[IRQ_IN]) return EPOLLIN;
    return 0;
}

static ssize_t ibeos_edge_irq_cdev_read(struct file *file, char __user *buf, size_t count, loff_t *f_pos)
{
    struct ibeos_edge2_irq *ppi = file->private_data;
    int ret = 1;
    char val = '1';

    ret = wait_event_interruptible(ppi->wq[IRQ_IN], ppi->ev[IRQ_IN]);
    if(ret) {
        /* interrupted */
        return -EINTR;
    }

    ret = copy_to_user(buf, &val, sizeof(val));
    if(ret) ret = -EFAULT;

    ppi->ev[IRQ_IN] = 0;
    ppi->ev[IRQ_OUT] = 1;
    wake_up(&ppi->wq[IRQ_OUT]);

    return 1;
}

void ibeos_edge_irq_cdev_create(struct ibeos_edge2_state *pps, int irq_num)
{
    static const struct file_operations irq_fops = {
        .owner = THIS_MODULE,
        .open = ibeos_edge_irq_cdev_open,
        .release = ibeos_edge_irq_cdev_release,
        .poll = ibeos_edge_irq_cdev_poll,
        .read = ibeos_edge_irq_cdev_read,
    };

    int i;

    dev_t cur_dev;

    pps->irqs[irq_num].parent = pps;

    for(i=0; i<IRQ_NUM_FLOW; ++i) init_waitqueue_head(&pps->irqs[irq_num].wq[i]);

    snprintf(pps->irqs[irq_num].name, sizeof(pps->irqs[irq_num].name)-1, "eirq %d", irq_num); 
    cdev_init(&pps->irqs[irq_num].cdev, &irq_fops);
    cur_dev = MKDEV(MAJOR(ibeos_edge2_devt), MINOR(ibeos_edge2_devt+NUM_BARS+irq_num));
    device_create(ibeos_edge2_class, NULL, cur_dev, NULL, "eirq%d", /* TODO: support multiple devices? */ irq_num);
    cdev_add(&pps->irqs[irq_num].cdev, cur_dev, 1);
}

void ibeos_edge_irq_cdev_destroy(struct ibeos_edge2_state *pps, int irq_num)
{
    device_destroy(ibeos_edge2_class, MKDEV(MAJOR(ibeos_edge2_devt), MINOR(ibeos_edge2_devt+NUM_BARS+irq_num)));
    cdev_del(&pps->irqs[irq_num].cdev);
}

static void ibeos_edge2_irq_mask(struct irq_data *d)
{
    ibeos_edge2_state *pps = (ibeos_edge2_state*)d->domain->host_data;
    unsigned long flags;
    unsigned long mask;

    raw_spin_lock_irqsave(&ibeos_edge2_spinlock, flags);
    mask = ioread32(pps->base[0]+PF_IMASK_HOST);
    iowrite32(mask&~(1<<d->hwirq), pps->base[0]+PF_IMASK_HOST);
    raw_spin_unlock_irqrestore(&ibeos_edge2_spinlock, flags);
}

static void ibeos_edge2_irq_mask_ack(struct irq_data *d)
{
    ibeos_edge2_state *pps = (ibeos_edge2_state*)d->domain->host_data;
    unsigned long flags;
    unsigned long mask;

    raw_spin_lock_irqsave(&ibeos_edge2_spinlock, flags);
    mask = ioread32(pps->base[0]+PF_IMASK_HOST);
    iowrite32(mask&~(1<<d->hwirq), pps->base[0]+PF_IMASK_HOST);
    //dev_info(&pps->pdev->dev, "mask ack irq %ld\n", d->hwirq);
    iowrite32(1<<d->hwirq, pps->base[0]+PF_ISTATUS_HOST);
    raw_spin_unlock_irqrestore(&ibeos_edge2_spinlock, flags);
}

static void ibeos_edge2_irq_unmask(struct irq_data *d)
{
    ibeos_edge2_state *pps = (ibeos_edge2_state*)d->domain->host_data;
    unsigned long flags;
    unsigned long mask;

    raw_spin_lock_irqsave(&ibeos_edge2_spinlock, flags);
    mask = ioread32(pps->base[0]+PF_IMASK_HOST);
    iowrite32(mask|(1<<d->hwirq), pps->base[0]+PF_IMASK_HOST);
    raw_spin_unlock_irqrestore(&ibeos_edge2_spinlock, flags);
}

static int ibeos_edge2_domain_map(struct irq_domain *h, unsigned int virq, irq_hw_number_t hw)
{
    static struct irq_chip ibeos_edge2_pic = {
        .name = "pfpcie irq chip",
        .irq_mask = ibeos_edge2_irq_mask,
        .irq_mask_ack = ibeos_edge2_irq_mask_ack,
        .irq_unmask = ibeos_edge2_irq_unmask,
    };

    irq_set_chip_and_handler(virq, &ibeos_edge2_pic, handle_level_irq);
    irq_set_chip_data(virq, h->host_data);
    return 0;
}

int ibeos_edge_irq_init(struct ibeos_edge2_state *pps)
{
    static struct irq_domain_ops ibeos_edge2_domain_ops = {
        .map = ibeos_edge2_domain_map,
    };

    int i;

    pps->fwnode = irq_domain_alloc_named_fwnode("ibeos_edge2");
    if(!pps->fwnode) return -ENOMEM;

    pps->domain = irq_domain_create_linear(pps->fwnode, NUM_IRQS, &ibeos_edge2_domain_ops, pps);
    if(!pps->domain) return -ENOMEM;

    for(i=0; i<NUM_IRQS; ++i) {
        irq_create_mapping(pps->domain, i);
        ibeos_edge_irq_cdev_create(pps, i);
    }

    for(i=0; i<pps->nvec; ++i) {
        irq_set_chained_handler_and_data(pci_irq_vector(pps->pdev, i), ibeos_edge2_cascade_isr, pps);
    }

    return 0;
}

void ibeos_edge_irq_destroy(struct ibeos_edge2_state *pps)
{
    int i;

    for(i=0; i<pps->nvec; ++i) irq_set_chained_handler_and_data(pci_irq_vector(pps->pdev, i), NULL, NULL);

    for(i=0; i<NUM_IRQS; ++i) {
        ibeos_edge_irq_cdev_destroy(pps, i);
        irq_dispose_mapping(irq_linear_revmap(pps->domain, i));
    }

    irq_domain_remove(pps->domain);
    irq_domain_free_fwnode(pps->fwnode);
}


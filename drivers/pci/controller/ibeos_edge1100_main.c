/* Copyright 2021 Ibeos */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/irq.h>

#include "linux/ibeos_edge1100.h"
#if IS_ENABLED(CONFIG_PCIE_IBEOS_EDGE1100_DMA)
#include <linux/ibeos_edge1100_dma.h>
#endif
#include "ibeos_edge1100_mem.h"
#include "ibeos_edge1100_irq.h"
#include "linux/ibeos_edge1100_driver.h"

dev_t ibeos_edge2_devt;
struct class *ibeos_edge2_class;

static struct ibeos_edge2_state *states[MAX_DEVICES];
static int num_devices; /* TODO: atomic if more than one device */

void *ibeos_edge2_get_bar(int devnum, int bar)
{
    BUG_ON(devnum>=MAX_DEVICES);
    BUG_ON(bar>=NUM_BARS);
    //BUG_ON(!states[devnum]);
    return states[devnum]?states[devnum]->base[bar]:NULL;
}
EXPORT_SYMBOL(ibeos_edge2_get_bar);

size_t ibeos_edge2_get_bar_size(int devnum, int bar)
{
    BUG_ON(devnum>=MAX_DEVICES);
    BUG_ON(bar>=NUM_BARS);
    //BUG_ON(!states[devnum]);
    return states[devnum]?states[devnum]->bar_size[bar]:0;
}
EXPORT_SYMBOL(ibeos_edge2_get_bar_size);

int ibeos_edge2_get_virq(int devnum, int hwirq)
{
    BUG_ON(devnum>=MAX_DEVICES);
    BUG_ON(!states[devnum]);
    BUG_ON(hwirq>=NUM_IRQS);

    return irq_linear_revmap(states[devnum]->domain, hwirq);
}
EXPORT_SYMBOL(ibeos_edge2_get_virq);

struct device *ibeos_edge2_get_dev(int devnum)
{
    BUG_ON(devnum>=MAX_DEVICES);
    BUG_ON(!states[devnum]);
    return &states[devnum]->pdev->dev;
}
EXPORT_SYMBOL(ibeos_edge2_get_dev);

static int ibeos_edge2_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
    ibeos_edge2_state *pps;
    int ret, i;

    BUG_ON(num_devices>=MAX_DEVICES);

    dev_info(&pdev->dev, "ibeos_edge2_probe\n");

    pps = devm_kzalloc(&pdev->dev, sizeof(*pps), GFP_KERNEL);
    if(!pps) return -ENOMEM;

    snprintf(pps->name, sizeof(pps->name)-1, "ibeos_edge2 %s", dev_name(&pdev->dev));

    /* TODO: dma_set_mask_and_coherent */

    pci_set_drvdata(pdev, pps);
    pps->pdev = pdev;

    ret = pci_enable_device(pdev);
    if(ret) return -ENODEV;

    ret = pci_request_regions(pdev, IBEOS_EDGE1100_DRVNAME);
    if(ret) goto err_disable;

    for(i=0; i<NUM_BARS; ++i) {
        struct resource *res = &pdev->resource[i];
        pps->bar_size[i] = resource_size(res);
        dev_info(&pdev->dev, "bar size %d: 0x%lx\n", i, pps->bar_size[i]);
        if(pps->bar_size[i]>1) {
            /* only ioremap bar0 for bridge control regs */
            //if(!i) {
                pps->base[i] = devm_ioremap(&pdev->dev, res->start, pps->bar_size[i]);
                if(IS_ERR(pps->base[i])) {
                    ret = PTR_ERR(pps->base[i]);
                    goto err_release;
                }
            //}
        }
    }

    pci_set_master(pdev);

    pps->nvec = pci_alloc_irq_vectors(pdev, 1, 32, PCI_IRQ_MSI);
    if(pps->nvec<0) {
        dev_err(&pdev->dev, "pci_alloc_irq_vectors returned %d\n", pps->nvec);
        goto err_release;
    }

    dev_info(&pdev->dev, "nvec is %d, line is %d\n", pps->nvec, pdev->irq);

    ret = ibeos_edge_irq_init(pps);
    if(ret) goto err_vectors;

    dev_info(&pdev->dev, "bridge ver 0x%x\n", ioread32(pps->base[0]+0x4000));

#if IS_ENABLED(CONFIG_PCIE_IBEOS_EDGE1100_DMA)
    ret = ibeos_edge_dma_init(pps);
    if(ret) goto err_irq;
#endif

    for(i=0; i<NUM_BARS; ++i) {
        if(pps->bar_size[i]>1) ibeos_edge_mem_cdev_create(pps, i);
    }

    ++num_devices;
    states[0] = pps; /* TODO: support multiple devices */

    return 0;
    
#if IS_ENABLED(CONFIG_PCIE_IBEOS_EDGE1100_DMA)
err_irq:
#endif
    ibeos_edge_irq_destroy(pps);

err_vectors:
    pci_free_irq_vectors(pdev);

err_release:
    pci_release_regions(pdev);

err_disable:
    pci_disable_device(pdev);

    return ret;
}

static void ibeos_edge2_remove(struct pci_dev *pdev)
{
    ibeos_edge2_state *pps = pci_get_drvdata(pdev);
    int i;

    dev_info(&pdev->dev, "ibeos_edge2_remove\n");

    for(i=0; i<NUM_BARS; ++i) {
        if(pps->bar_size[i]>1) ibeos_edge_mem_cdev_destroy(pps, i);
    }

#if IS_ENABLED(CONFIG_PCIE_IBEOS_EDGE1100_DMA)
    ibeos_edge_dma_destroy(pps);
#endif

    ibeos_edge_irq_destroy(pps);

    pci_free_irq_vectors(pdev);
    pci_release_regions(pdev);
    pci_disable_device(pdev);

    --num_devices;
    states[0] = NULL; /* TODO: support multiple devices */
}

static const struct pci_device_id ibeos_edge2_ids[] = {
    { PCI_DEVICE(0x11aa, 0x1556) },
    {},
};

static struct pci_driver ibeos_edge2_driver = {
    .name = "ibeos_ibeos_edge2",
    .id_table = ibeos_edge2_ids,
    .probe = ibeos_edge2_probe,
    .remove = ibeos_edge2_remove,
};

static int __init ibeos_edge2_init(void)
{
    int ret;

    pr_info("Ibeos PF PCIe driver\n");

    ret = alloc_chrdev_region(&ibeos_edge2_devt, 0, MAX_DEVICES, IBEOS_EDGE1100_DRVNAME);
    if(ret<0) {
        pr_err("failed in alloc_chrdev_region\n");
        return PTR_ERR(ibeos_edge2_class);
    }

    ibeos_edge2_class = class_create("ibeos_edge2_class");
    if(IS_ERR(ibeos_edge2_class)) {
        pr_err("failed in class_create\n");
        return PTR_ERR(ibeos_edge2_class);
    }

    return pci_register_driver(&ibeos_edge2_driver);
}

static void __exit ibeos_edge2_exit(void)
{
    pr_info("Ibeos PF PCIe driver removed\n");

    pci_unregister_driver(&ibeos_edge2_driver);

    if(!IS_ERR(ibeos_edge2_class)) class_destroy(ibeos_edge2_class);
    if(ibeos_edge2_devt) unregister_chrdev_region(ibeos_edge2_devt, MAX_DEVICES);
}

module_init(ibeos_edge2_init);
module_exit(ibeos_edge2_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Steven Seeger <steven.seeger@flightsystems.net>");
MODULE_DESCRIPTION("PolarFire PCIe host bridge driver");


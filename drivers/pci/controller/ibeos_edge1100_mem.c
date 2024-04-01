#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/pci.h>

#include <linux/ibeos_edge1100.h>
#include <linux/ibeos_edge1100_driver.h>

static int ibeos_edge_mem_cdev_open(struct inode *inode, struct file *file)
{
    struct ibeos_edge2_bar *ppb = container_of(inode->i_cdev, struct ibeos_edge2_bar, cdev);

    file->private_data = ppb;
    return 0;
}

static int ibeos_edge_mem_cdev_mmap(struct file *file, struct vm_area_struct *vma)
{
    /* pci_mmap_resource_range is not exported in kernel, so reimpl here */
    static const struct vm_operations_struct pci_phys_vm_ops = {
#ifdef CONFIG_HAVE_IOREMAP_PROT
        .access = generic_access_phys,
#endif
    };

    struct ibeos_edge2_bar *ppb = file->private_data;
    const size_t size = vma->vm_end - vma->vm_start;
    const int index = ppb-ppb->parent->bars;
    struct resource *res = &ppb->parent->pdev->resource[index];
    enum pci_mmap_state mmap_state;
    int ret;

    BUG_ON(index>NUM_BARS);

    mmap_state = (res->flags & IORESOURCE_MEM)?pci_mmap_mem:pci_mmap_io;

    /* vma->vm_pgoff is 0 for wt, and 1 for wc (dependent on userspace lib) */
    if(!!vma->vm_pgoff) vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
    else vma->vm_page_prot = pgprot_device(vma->vm_page_prot);

    vma->vm_pgoff = 0;

    if(mmap_state==pci_mmap_io) {
        ret = pci_iobar_pfn(ppb->parent->pdev, index, vma);
        if(ret) return ret;
    }
    else vma->vm_pgoff += (pci_resource_start(ppb->parent->pdev, index) >> PAGE_SHIFT);

    vma->vm_ops = &pci_phys_vm_ops;

    return io_remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff, size, vma->vm_page_prot);
}

static int ibeos_edge_mem_cdev_release(struct inode *inode, struct file *file)
{
    return 0;
}

static long ibeos_edge_mem_cdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct ibeos_edge2_bar *ppb = file->private_data;
    const int index = ppb-ppb->parent->bars;

    BUG_ON(index>NUM_BARS);

    switch(cmd) {
        case IBEOS_EDGE2_IOCTL_GET_BAR_SIZE: {
            return copy_to_user((uint64_t __user*)arg, &ppb->parent->bar_size[index], sizeof(ppb->parent->bar_size[index]));
        }
        break;

        default:
            dev_err(&ppb->parent->pdev->dev, "unknown ioctl 0x%x\n", cmd);
            return -EINVAL;
    }

    return -1;
}

void ibeos_edge_mem_cdev_create(struct ibeos_edge2_state *pps, int bar_num)
{
    static const struct file_operations bar_fops = {
        .owner = THIS_MODULE,
        .open = ibeos_edge_mem_cdev_open,
        .unlocked_ioctl = ibeos_edge_mem_cdev_ioctl,
        .mmap = ibeos_edge_mem_cdev_mmap,
        .release = ibeos_edge_mem_cdev_release,
    };

    dev_t cur_dev;

    cdev_init(&pps->bars[bar_num].cdev, &bar_fops);
    cur_dev = MKDEV(MAJOR(ibeos_edge2_devt), MINOR(ibeos_edge2_devt+bar_num));
    device_create(ibeos_edge2_class, NULL, cur_dev, NULL, "ebar%d", /* TODO: support multiple devices? */ bar_num);
    cdev_add(&pps->bars[bar_num].cdev, cur_dev, 1);

    pps->bars[bar_num].parent = pps;
}

void ibeos_edge_mem_cdev_destroy(struct ibeos_edge2_state *pps, int bar_num)
{
    device_destroy(ibeos_edge2_class, MKDEV(MAJOR(ibeos_edge2_devt), MINOR(ibeos_edge2_devt+bar_num)));
    cdev_del(&pps->bars[bar_num].cdev);
}


/*
 * Copyright 2024 Ibeos
 * http://www.ibeos.com
 *
 */

/* NOTE: we could add mmap capability here, but it seems like a bad idea. FPGA spec says that
 * FRAM should be accessed via 32-bit accessors, to to ensure this, we should keep all access
 * behavior in the driver itself.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/printk.h>
#include <linux/sysfs.h>
#include <linux/device.h>
#include <asm/io.h>

#include <linux/ibeos_edge1100.h>

struct ibeos_edge1100_fram_state {
    struct miscdevice misc;
    struct device *dev;

    void __iomem *bar;
    size_t size;
};

static struct ibeos_edge1100_fram_state iefs; /* only have one FPGA on a board */

/* NOTE: we have a single global because there is a single PCIe device for the Edge1100 FPGA.
 * In order to split up the drivers, that's the model we use. Since the llseek/read/write functions
 * use this single device, we don't really need to use container_of, but let's do it the "right"
 * way in case the overall software infrastructure for this device changes.
 */

static loff_t fram_llseek(struct file *file, loff_t offset, int whence)
{
    struct ibeos_edge1100_fram_state *iefs = container_of(file->private_data, struct ibeos_edge1100_fram_state, misc);

    if(offset&3) return -EINVAL;
    return fixed_size_llseek(file, offset, whence, iefs->size);
}

static ssize_t fram_read(struct file *file, char __user *buf, size_t len, loff_t *pos)
{
    struct ibeos_edge1100_fram_state *iefs = container_of(file->private_data, struct ibeos_edge1100_fram_state, misc);
    uint32_t val;
    const ssize_t retlen = len;

    if(*pos<0) return -EINVAL;
    if(len&3) return -EINVAL;
    if(*pos&3) return -EINVAL;

    while(len) {
        if(*pos>=iefs->size) return -EINVAL;
        val = ioread32(iefs->bar+*pos);

        if(copy_to_user(buf, &val, sizeof(val))) return -EFAULT;
        *pos += sizeof(val);
        buf += sizeof(val);
        len -= sizeof(val);
    }

    return retlen;
}

static ssize_t fram_write(struct file *file, const char __user *buf, size_t len, loff_t *pos)
{
    struct ibeos_edge1100_fram_state *iefs = container_of(file->private_data, struct ibeos_edge1100_fram_state, misc);
    uint32_t val;
    const ssize_t retlen = len;

    if(*pos<0) return -EINVAL;
    if(*pos>=iefs->size) return -EINVAL;
    if(len&3) return -EINVAL;
    if(*pos&3) return -EINVAL;

    while(len) {
        if(*pos>=iefs->size) return -EINVAL;

        if(copy_from_user(&val, buf, sizeof(val))) return -EFAULT;
        iowrite32(val, iefs->bar+*pos);

        *pos += sizeof(val);
        buf += sizeof(val);
        len -= sizeof(val);
    }

    return retlen;
}

static const struct file_operations ibeos_edge1100_fram_fops = {
    .owner = THIS_MODULE,
    .read = fram_read,
    .write = fram_write,
    .llseek = fram_llseek,
};

static int __init init_edge1100_fram(void)
{
    int ret=0;

    if(iefs.bar) return -EBUSY;

    memset(&iefs, 0, sizeof(iefs));

    iefs.bar = ibeos_edge2_get_bar(0, 1);
    if(!iefs.bar) {
        ret = -ENODEV;
        goto err_bar;
    }

    iefs.size = ibeos_edge2_get_bar_size(0, 1);

    iefs.misc.name = "edge1100_fram";
    iefs.misc.minor = MISC_DYNAMIC_MINOR;
    iefs.misc.fops = &ibeos_edge1100_fram_fops;
    iefs.misc.nodename = "edge1100_fram";
    iefs.misc.groups = NULL;

    if(misc_register(&iefs.misc)) {
        pr_err("edge1100_fram: failed to register as misc device\n");
        ret = -ENODEV;
        goto err_bar;
    }

    pr_info("edge1100_fram: initialized\n");
    return 0;

err_bar:
    iefs.bar = NULL;
    return ret;
}

static void __exit exit_edge1100_fram(void)
{
    misc_deregister(&iefs.misc);
    iefs.bar = NULL;
}

module_init(init_edge1100_fram);
module_exit(exit_edge1100_fram);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Access to FRAM for Ibeos Edge-1100 board");
MODULE_AUTHOR("Steven Seeger <steven@efsi.com>");

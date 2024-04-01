/* Copyright 2021 Ibeos */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/delay.h>

#include "ibeos_edge1100_dma.h"
#include <linux/ibeos_edge1100.h>
#include <linux/ibeos_edge1100_dma.h>

#include <linux/ibeos_edge1100_driver.h>

/* 2 irqs for each physical channel */
static const int irq_map[NUM_PDMA][NUM_DMA_IRQ] = { {0, 8}, {1, 9} };

extern int ibeos_edge2_get_virq(int, int); /* TODO: the irqs are fixed, but device tree might make more sense */

typedef enum ibeos_edge_dma_type {
    PF_DMA_W,
    PF_DMA_R,
    PF_DMA_INVALID
} ibeos_edge_dma_type;

static ibeos_edge_dma_type ibeos_edge_dma_chan_to_type(const struct ibeos_edge2_vdma *ppv)
{
    int index = ppv-ppv->parent->chans;

    BUG_ON(index>=NUM_VDMA);
    if(index>=(NUM_VDMA/2)) return PF_DMA_R;

    return PF_DMA_W;
}

static void ibeos_edge_dma_start_transfer(struct ibeos_edge2_vdma *ppv);
static irqreturn_t ibeos_edge_dma_irq_common(void *priv, enum ibeos_edge_dma_type type)
{
    struct ibeos_edge2_dma *ppd = (struct ibeos_edge2_dma*)priv;
    struct ibeos_edge2_vdma *ppv = ppd->active[type];
    uint32_t stat;

    if(!ppd->active[type]) return IRQ_HANDLED; /* both err and completed length can occur, triggering two irqs */
    BUG_ON(!ppd->active[type]);
    
    stat = ioread32(ppd->regs[type]+PF_DMA_STAT);
    if(stat&8) {
        /* error occurred -- why? */
        ppv->desc->status = DMA_ERROR;
    }
    else ppv->desc->status = DMA_COMPLETE;
    vchan_cookie_complete(&ppv->desc->vdesc);
    ppd->active[type] = NULL;
    ibeos_edge_dma_start_transfer(ppv);

    return IRQ_HANDLED;
}

static irqreturn_t ibeos_edge2_dma_irq_end_W(int irq, void *priv)
{
    return ibeos_edge_dma_irq_common(priv, PF_DMA_W);
}

static irqreturn_t ibeos_edge2_dma_irq_err_W(int irq, void *priv)
{
    return ibeos_edge_dma_irq_common(priv, PF_DMA_W);
}

static irqreturn_t ibeos_edge2_dma_irq_end_R(int irq, void *priv)
{
    return ibeos_edge_dma_irq_common(priv, PF_DMA_R);
}

static irqreturn_t ibeos_edge2_dma_irq_err_R(int irq, void *priv)
{
    return ibeos_edge_dma_irq_common(priv, PF_DMA_R);
}

static void teardown_dma_dev(struct ibeos_edge2_state *pps)
{
    dma_async_device_unregister(&pps->dma->dma_dev);
}

static void cond_teardown_irq(struct ibeos_edge2_state *pps)
{
    int i, j;

    for(i=0; i<NUM_PDMA; ++i) {
        for(j=0; j<NUM_DMA_IRQ; ++j) if(pps->dma->irq_flag&(1<<j)) free_irq(irq_linear_revmap(pps->domain, irq_map[i][j]), pps->dma);
    }
}

static int ibeos_edge_dma_cdev_open(struct inode *inode, struct file *file)
{
    struct ibeos_edge2_dma_cdev *ppdc = container_of(inode->i_cdev, struct ibeos_edge2_dma_cdev, cdev);
    struct ibeos_edge2_dma_cdev_state *ppdcs = kzalloc(sizeof(*ppdcs), GFP_KERNEL);
    int ret = 0;

    if(!ppdcs) return -ENOMEM;

    file->private_data = ppdcs;
    ppdcs->parent = ppdc;

    init_completion(&ppdcs->dma_comp);
    //dev_info(&ppdc->parent->parent->pdev->dev, "requesting %s\n", ppdc->name);
    ppdcs->chan = dma_request_chan(&ppdc->parent->parent->pdev->dev, ppdc->name);
    if(!ppdcs->chan) {
        dev_err(&ppdc->parent->parent->pdev->dev, "dma_request_chan err\n");
        ret = -ENODEV;
        goto err_out;
    }
    return 0;

err_out:
    kfree(ppdcs);

    return ret;
}

static int ibeos_edge_dma_cdev_release(struct inode *inode, struct file *file)
{
    struct ibeos_edge2_dma_cdev_state *ppdcs = (struct ibeos_edge2_dma_cdev_state*)file->private_data;

    dma_release_channel(ppdcs->chan);
    kfree(ppdcs);
    return 0;
}

static void ibeos_edge_dma_cb(void *param)
{
    struct ibeos_edge2_dma_cdev_state *ppdcs = (struct ibeos_edge2_dma_cdev_state*)param;

    complete(&ppdcs->dma_comp);
}

static long ibeos_edge_dma_cdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct ibeos_edge2_dma_cdev_state *ppdcs = (struct ibeos_edge2_dma_cdev_state*)file->private_data;
    struct ibeos_edge2_dma_args args;
    int ret;
    //uint8_t *pv;
    //dma_addr_t pp;
    struct scatterlist *sgl, *sg;
    enum dma_data_direction dir;
    unsigned n;
    struct dma_async_tx_descriptor *dmad;
    dma_cookie_t dmac;
    struct dma_slave_config sc;

    struct page **pages;
    int nr_pages;
    int nr_pages_mapped;
    int bytes;
    int i;

    ret = copy_from_user(&args, (void __user*)arg, sizeof(args));
    if(ret) {
        dev_err(&ppdcs->parent->parent->parent->pdev->dev, "unable to copy args from user\n");
        return -ENOMEM;
    }

    nr_pages = (args.len+PAGE_SIZE-1) >> PAGE_SHIFT;

    pages = kmalloc(nr_pages * sizeof(*pages), GFP_KERNEL);
    if(!pages) {
        dev_err(&ppdcs->parent->parent->parent->pdev->dev, "unable to allocate pages\n");
        return -ENOMEM;
    }

    memset(&sc, 0, sizeof(sc));
    switch(cmd) {
        case IBEOS_EDGE2_IOCTL_RDMA:
            dir = DMA_FROM_DEVICE;
            sc.src_addr = args.axi_addr;
        break;

        case IBEOS_EDGE2_IOCTL_WDMA:
            dir = DMA_TO_DEVICE;
            sc.dst_addr = args.axi_addr;
        break;

        default:
            dev_err(&ppdcs->parent->parent->parent->pdev->dev, "invalid ioctl 0x%x\n", cmd);
            ret = -EINVAL;
            goto err_out;
    }

    nr_pages_mapped = get_user_pages_fast((uint64_t)args.buf, nr_pages, (dir==DMA_FROM_DEVICE), pages);
    if(nr_pages != nr_pages_mapped) {
        dev_err(&ppdcs->parent->parent->parent->pdev->dev, "unable to pin user buf pages\n");
        ret = -ENOMEM;
        goto err_out;
    }

    //dev_info(&ppdcs->parent->parent->parent->pdev->dev, "nr_pages_mapped %d\n", nr_pages_mapped);
    sgl = kmalloc(nr_pages_mapped * sizeof(*sgl), GFP_KERNEL);
    if(!sgl) {
        dev_err(&ppdcs->parent->parent->parent->pdev->dev, "unable to alloc sgl nr_pages_mapped %d\n", nr_pages_mapped);
        ret = -ENOMEM;
        goto err_pages;
    }

    sg_init_table(sgl, nr_pages_mapped);
    bytes = args.len;
    for_each_sg(sgl, sg, nr_pages_mapped, i) {
        n = min_t(unsigned, PAGE_SIZE, bytes);
        sg_set_page(sg, pages[i], n, 0);
    }

    //sg_init_one(&sg, pv, args.len);
    n = dma_map_sg(&ppdcs->parent->parent->parent->pdev->dev, sgl, nr_pages_mapped, dir);
    if(n==0) {
        dev_err(&ppdcs->parent->parent->parent->pdev->dev, "can't dma map %d byte buffer", args.len);
        ret = -EIO;
        goto err_sgl;
    }

    ret = dmaengine_slave_config(ppdcs->chan, &sc);
    if(ret) {
        dev_err(&ppdcs->parent->parent->parent->pdev->dev, "can't config slave\n");
        ret = -EINVAL;
        goto err_sgl;
    }

    dmad = dmaengine_prep_slave_sg(ppdcs->chan, sgl, nr_pages_mapped, (dir==DMA_FROM_DEVICE)?DMA_DEV_TO_MEM:DMA_MEM_TO_DEV, DMA_PREP_INTERRUPT);
    if(!dmad) {
        dev_err(&ppdcs->parent->parent->parent->pdev->dev, "failed to prep sg\n");
        goto err_map;
    }

    dmad->callback = ibeos_edge_dma_cb;
    dmad->callback_param = ppdcs;
    
    dmac = dmaengine_submit(dmad);
    dma_async_issue_pending(ppdcs->chan);

    wait_for_completion(&ppdcs->dma_comp);

err_map:
    dma_unmap_sg(&ppdcs->parent->parent->parent->pdev->dev, sgl, nr_pages_mapped, dir);
err_sgl:
    kfree(sgl);
err_pages:
    if(pages) {
        for(n=0;n<nr_pages_mapped;++n) put_page(pages[n]);
    }
err_out:
    //dma_free_coherent(&ppdcs->parent->parent->parent->pdev->dev, PAGE_SIZE, pv, pp);
    kfree(pages);
    return ret; /* TODO: update retval */
}

static void ibeos_edge_dma_cdev_create(struct ibeos_edge2_state *pps)
{
    static const struct file_operations dma_fops = {
        .owner = THIS_MODULE,
        .open = ibeos_edge_dma_cdev_open,
        .release = ibeos_edge_dma_cdev_release,
        .unlocked_ioctl = ibeos_edge_dma_cdev_ioctl,
    };

    int i;
    dev_t cur_dev;

    for(i=0; i<NUM_PDMA; ++i) {
        static const char *names[NUM_PDMA] = { "edmaW", "edmaR" };
        static const char *tnames[NUM_PDMA] = { "tx", "rx" };
        cdev_init(&pps->dma->cdevs[i].cdev, &dma_fops);
        cur_dev = MKDEV(MAJOR(ibeos_edge2_devt), MINOR(ibeos_edge2_devt+NUM_BARS+NUM_IRQS+i));

        device_create(ibeos_edge2_class, NULL, cur_dev, NULL, names[i]);

        cdev_add(&pps->dma->cdevs[i].cdev, cur_dev, 1);

        pps->dma->cdevs[i].parent = pps->dma;
        pps->dma->cdevs[i].name = tnames[i];
    }
}

static void ibeos_edge_dma_cdev_destroy(struct ibeos_edge2_state *pps)
{
    int i;

    for(i=0; i<NUM_PDMA; ++i) {
        device_destroy(ibeos_edge2_class, MKDEV(MAJOR(ibeos_edge2_devt), MINOR(ibeos_edge2_devt+NUM_BARS+NUM_IRQS+i)));
        cdev_del(&pps->dma->cdevs[i].cdev);
    }
}

static void teardown_cdevs(struct ibeos_edge2_state * pps)
{
    ibeos_edge_dma_cdev_destroy(pps);
}

static int ibeos_edge_dma_alloc_chan_resources(struct dma_chan *chan)
{
    struct ibeos_edge2_vdma *ppv = container_of(chan, struct ibeos_edge2_vdma, vchan.chan);
    ibeos_edge_dma_type type = ibeos_edge_dma_chan_to_type(ppv);

    char name[32]; /* arbitrary length, dma_pool_create copies */

    snprintf(name, sizeof(name)-1, "edma%c", 'A'+type);

    ppv->pool = dma_pool_create(name, &ppv->parent->parent->pdev->dev, sizeof(struct ibeos_edge_dma_desc), 32, 0);
    if(!ppv->pool) {
        dev_err(&ppv->parent->parent->pdev->dev, "no memory for descriptors\n");
        return -ENOMEM;
    }

    return 0;
}

static void ibeos_edge_dma_free_chan_resources(struct dma_chan *chan)
{
    struct ibeos_edge2_vdma *ppv = container_of(chan, struct ibeos_edge2_vdma, vchan.chan);

    vchan_free_chan_resources(&ppv->vchan);
    dma_pool_destroy(ppv->pool);
}

static void ibeos_edge_dma_free_desc(struct ibeos_edge2_vdma *ppv, struct ibeos_edge2_dma_desc *desc)
{
    struct ibeos_edge2_dma_hw_desc *hw;
    if(desc->ndesc) {
        int i = desc->ndesc;

        do {
            hw = &desc->hw[--i];
            dma_pool_free(ppv->pool, hw->dv, hw->dp);
        } while(i);
    }

    kfree(desc->hw);
    kfree(desc);
}

static void ibeos_edge_dma_free_vdesc(struct virt_dma_desc *vdesc)
{
    struct ibeos_edge2_dma_desc *desc = container_of(vdesc, struct ibeos_edge2_dma_desc, vdesc);

#if 1
    int i;
    //dev_info(&desc->parent->parent->parent->pdev->dev, "free vd\n");
    for(i=0; i<desc->ndesc;++i) {
        //dev_info(&desc->parent->parent->parent->pdev->dev, "stat %d %x %x %x\n", i, desc->hw[i].dv->status_num, desc->hw[i].dv->prc_status, desc->hw[i].dv->prc_page_size);
    }
#endif
    ibeos_edge_dma_free_desc(desc->parent, desc);
    desc->parent->desc = NULL;
}

static struct ibeos_edge2_dma_desc *ibeos_edge_dma_alloc_desc(struct ibeos_edge2_vdma *ppv, uint32_t ndesc)
{
    struct ibeos_edge2_dma_desc *ret = kzalloc(sizeof(*ret), GFP_NOWAIT);
    if(!ret) return NULL;

    ret->hw = kcalloc(ndesc, sizeof(*ret->hw), GFP_NOWAIT);
    if(!ret->hw) {
        kfree(ret);
        return NULL;
    }

    ret->ndesc = ndesc;
    ret->parent = ppv;
    return ret;
}

static struct dma_async_tx_descriptor *ibeos_edge_dma_prep_slave_sg(struct dma_chan *chan, struct scatterlist *sgl,
        unsigned int sg_len, enum dma_transfer_direction direction, unsigned long flags, void *context)
{
    struct ibeos_edge2_vdma *ppv = container_of(chan, struct ibeos_edge2_vdma, vchan.chan);
    struct ibeos_edge2_dma_desc *desc;
    struct scatterlist *sg;
    int i;
    uint32_t offset=0;

    struct ibeos_edge2_dma_hw_desc *hw;//, *dev_to_mem_hw;
    dma_addr_t next_desc = 0;

    /* TEMP: sanity check */
    switch(direction) {
        case DMA_MEM_TO_DEV:
            BUG_ON(ibeos_edge_dma_chan_to_type(ppv)!=PF_DMA_W);
            break;
        case DMA_DEV_TO_MEM:
            BUG_ON(ibeos_edge_dma_chan_to_type(ppv)!=PF_DMA_R);
            break;
        default:
            BUG();
    }

    //desc = ibeos_edge_dma_alloc_desc(ppv, sg_len+(direction==DMA_DEV_TO_MEM?1:0));
    desc = ibeos_edge_dma_alloc_desc(ppv, sg_len);
    if(!desc) return NULL;

    for_each_sg(sgl, sg, sg_len, i) {
        hw = &desc->hw[i];

        hw->dv = dma_pool_alloc(ppv->pool, GFP_NOWAIT, &hw->dp);
        if(!hw->dv) {
            desc->ndesc = i;
            ibeos_edge_dma_free_desc(ppv, desc);
            return NULL;
        }

        hw->phys = sg_dma_address(sg);
        hw->len = sg_dma_len(sg);
        hw->offset = offset;
        offset += hw->len;
    }

#if 0
    if(direction==DMA_DEV_TO_MEM) {
        /* this driver always does contiguous device access to memory, so need a single
         * descriptor for it.
         *
         * NOTE: at this point, i is indexing the extra hw desc allocated above, so use it
         */
        hw = &desc->hw[i];
        hw->dv = dma_pool_alloc(ppv->pool, GFP_NOWAIT, &hw->dp);
        if(!hw->dv) {
            ibeos_edge_dma_free_desc(ppv, desc); /* frees all the ones set up in for_each_sg */
            return NULL;
        }

        dev_to_mem_hw = hw; /* save this for later */

        ++i; /* must increment i, because we have one more desc in this direction */
    }
#endif

    //dev_info(&ppv->parent->parent->pdev->dev, "i is %d\n", i);
    desc->ndesc = i;
    //i = sg_len; /* just in case it had +1 in it for DEV_TO_MEM */
    desc->status = DMA_IN_PROGRESS;

    hw = &desc->hw[--i];
    //dev_info(&ppv->parent->parent->pdev->dev, "hw is %p\n", hw);
    hw->dv->se_cond = 3; /* end of chain in last desc */

    goto first;

    do {
        hw = &desc->hw[--i];
        hw->dv->next_rdy = 1;
first:
        hw->dv->status_req = 1; /* TEMP? */
        hw->dv->page_size = hw->len;
        hw->dv->next_addr = (next_desc>>5);
        hw->dv->se_cond = 2; /* stop on error */
        //dev_info(&ppv->parent->parent->pdev->dev, "WR set0 %x %llx %x %llx %x\n", hw->dv->next_rdy, (uint64_t)hw->dv->next_addr, hw->dv->se_cond, next_desc, hw->dv->page_size);

        if(direction==DMA_MEM_TO_DEV) {
            /* PF_DMA_W */
            //dev_info(&ppv->parent->parent->pdev->dev, "W set %llx %llx %x %x %llx\n", hw->phys, ppv->config.dst_addr, hw->offset, hw->len, next_desc);
            hw->dv->src_addr = hw->phys;
            hw->dv->dest_addr = ppv->config.dst_addr + hw->offset;
        }
        else {
            /* PF_DMA_R */
            //dev_info(&ppv->parent->parent->pdev->dev, "R!!!\n");
            //dev_info(&ppv->parent->parent->pdev->dev, "R set %llx %llx %llx %x %x %llx\n", hw->phys, ppv->config.src_addr, ppv->config.dst_addr, hw->offset, hw->len, next_desc);
            hw->dv->dest_addr = hw->phys;
            hw->dv->src_addr = ppv->config.src_addr + hw->offset;
        }

        //dev_info(&ppv->parent->parent->pdev->dev, "WR set1 %x %llx %llx\n", hw->dv->page_size, hw->dv->dest_addr, hw->dv->src_addr);
        desc->len += hw->len;
        next_desc = hw->dp;
        //for(j=0; j<32/4; ++j) {
        //    uint32_t *ptr = (uint32_t*)hw->dv;
        //    dev_info(&ppv->parent->parent->pdev->dev, "0x%08x ", ptr[j]);
        //}
    } while(i);

    return vchan_tx_prep(&ppv->vchan, &desc->vdesc, flags);
}

static void ibeos_edge_dma_start_transfer(struct ibeos_edge2_vdma *ppv)
{
    struct virt_dma_desc *vdesc;

    /* next virtual descriptor */
    vdesc = vchan_next_desc(&ppv->vchan);
    if(!vdesc) {
        ppv->desc = NULL;
        return;
    }

    list_del(&vdesc->node);
    ppv->desc = container_of(vdesc, struct ibeos_edge2_dma_desc, vdesc);

    if(ibeos_edge_dma_chan_to_type(ppv)==PF_DMA_W) {
        /* dma0 */
        BUG_ON(ppv->parent->active[PF_DMA_W]);
        ppv->parent->active[PF_DMA_W] = ppv;
        //dev_info(&ppv->parent->parent->pdev->dev, "W 0x%llx %x %lx\n", ppv->desc->hw->dp, ppv->desc->hw->len, ppv->desc->len);
        //dev_info(&ppv->parent->parent->pdev->dev, "W desc %x %x %llx %llx %x\n", ppv->desc->hw->dv->page_size, ppv->desc->hw->dv->se_cond, ppv->desc->hw->dv->src_addr, ppv->desc->hw->dv->dest_addr, ppv->desc->hw->dv->next_rdy);
        iowrite32(0, ppv->parent->regs[PF_DMA_W]+PF_DMA_SRC_PARAM);
        iowrite32(4, ppv->parent->regs[PF_DMA_W]+PF_DMA_DST_PARAM);
        iowrite32(ppv->desc->hw->dp&0xffffffff, ppv->parent->regs[PF_DMA_W]+PF_DMA_SRC_LSW);
        iowrite32(ppv->desc->hw->dp>>32, ppv->parent->regs[PF_DMA_W]+PF_DMA_SRC_MSW);
        iowrite32(0, ppv->parent->regs[PF_DMA_W]+PF_DMA_DST_LSW);
        iowrite32(0, ppv->parent->regs[PF_DMA_W]+PF_DMA_DST_MSW);
        iowrite32(ppv->desc->len, ppv->parent->regs[PF_DMA_W]+PF_DMA_LEN);
        iowrite32(0x030023a9, ppv->parent->regs[PF_DMA_W]+PF_DMA_CTRL);
    }
    else {
        /* dma1 */
        BUG_ON(ppv->parent->active[PF_DMA_R]);
        ppv->parent->active[PF_DMA_R] = ppv;
        iowrite32(4, ppv->parent->regs[PF_DMA_R]+PF_DMA_SRC_PARAM);
        iowrite32(0, ppv->parent->regs[PF_DMA_R]+PF_DMA_DST_PARAM);
        iowrite32(ppv->desc->hw->dp&0xffffffff, ppv->parent->regs[PF_DMA_R]+PF_DMA_SRC_LSW);
        iowrite32(ppv->desc->hw->dp>>32, ppv->parent->regs[PF_DMA_R]+PF_DMA_SRC_MSW);
        iowrite32(ppv->desc->hw->dp&0xffffffff, ppv->parent->regs[PF_DMA_R]+PF_DMA_DST_LSW);
        iowrite32(ppv->desc->hw->dp>>32, ppv->parent->regs[PF_DMA_R]+PF_DMA_DST_MSW);
        iowrite32(ppv->desc->len, ppv->parent->regs[PF_DMA_R]+PF_DMA_LEN);
        //dev_info(&ppv->parent->parent->pdev->dev, "R len 0x%lx dp %llx %x %x\n", ppv->desc->len, ppv->desc->hw->dp, ioread32(ppv->parent->regs[PF_DMA_R]+PF_DMA_CTRL), ioread32(ppv->parent->regs[PF_DMA_R]+PF_DMA_STAT));
        iowrite32(0x000023a9, ppv->parent->regs[PF_DMA_R]+PF_DMA_CTRL);
        //dev_info(&ppv->parent->parent->pdev->dev, "R len 0x%lx dp %llx %x %x\n", ppv->desc->len, ppv->desc->hw->dp, ioread32(ppv->parent->regs[PF_DMA_R]+PF_DMA_CTRL), ioread32(ppv->parent->regs[PF_DMA_R]+PF_DMA_STAT));
    }
}

static void ibeos_edge_dma_issue_pending(struct dma_chan *chan)
{
    struct ibeos_edge2_vdma *ppv = container_of(chan, struct ibeos_edge2_vdma, vchan.chan);
    unsigned long flags;

    spin_lock_irqsave(&ppv->vchan.lock, flags);

    if(vchan_issue_pending(&ppv->vchan) && !ppv->desc) {
        ibeos_edge_dma_start_transfer(ppv);
    }
    //else dev_info(&ppv->parent->parent->pdev->dev, "do not start transfer\n"); /* TEMP */

    spin_unlock_irqrestore(&ppv->vchan.lock, flags);
}

static enum dma_status ibeos_edge_dma_tx_status(struct dma_chan *chan, dma_cookie_t cookie, struct dma_tx_state *state)
{
    struct ibeos_edge2_vdma *ppv = container_of(chan, struct ibeos_edge2_vdma, vchan.chan);
    unsigned long flags;
    enum dma_status status;
    enum ibeos_edge_dma_type type = ibeos_edge_dma_chan_to_type(ppv);

    status = dma_cookie_status(chan, cookie, state);
    if(status==DMA_COMPLETE) return status;

    spin_lock_irqsave(&ppv->vchan.lock, flags);
    if(ppv->parent->active[type]) status = DMA_IN_PROGRESS;
    else status = DMA_COMPLETE;

    dma_set_residue(state, ioread32(ppv->parent->regs[type]+PF_DMA_PRC_LEN));
    spin_unlock_irqrestore(&ppv->vchan.lock, flags);
    return status;
}

static int ibeos_edge_dma_config(struct dma_chan *chan, struct dma_slave_config *config)
{
    struct ibeos_edge2_vdma *ppv = container_of(chan, struct ibeos_edge2_vdma, vchan.chan);
    memcpy(&ppv->config, config, sizeof(*config));

    return 0;
}

static int ibeos_edge_dma_terminate_all(struct dma_chan *chan)
{
    struct ibeos_edge2_vdma *ppv = container_of(chan, struct ibeos_edge2_vdma, vchan.chan);
    unsigned long flags;
    LIST_HEAD(head);
    uint32_t ctrl;
    enum ibeos_edge_dma_type type = ibeos_edge_dma_chan_to_type(ppv);

    spin_lock_irqsave(&ppv->vchan.lock, flags);
    ctrl = ioread32(ppv->parent->regs[type]+PF_DMA_CTRL);
    if(ctrl&1) {
        /* chan is active, so stop it */
        ctrl &= ~1;
        iowrite32(ctrl, ppv->parent->regs[type]+PF_DMA_CTRL);
    }
    if(ppv->desc) {
        ibeos_edge_dma_free_desc(ppv, ppv->desc);
        ppv->desc = NULL;
    }
    vchan_get_all_descriptors(&ppv->vchan, &head);
    spin_unlock_irqrestore(&ppv->vchan.lock, flags);

    vchan_dma_desc_free_list(&ppv->vchan, &head);
    return 0;
}

static void ibeos_edge_dma_synchronize(struct dma_chan *chan)
{
    struct ibeos_edge2_vdma *ppv = container_of(chan, struct ibeos_edge2_vdma, vchan.chan);
    vchan_synchronize(&ppv->vchan);
}

static bool ibeos_edge_dma_filter_fn(struct dma_chan *chan, void *param)
{
    struct ibeos_edge2_vdma *ppv = container_of(chan, struct ibeos_edge2_vdma, vchan.chan);
    ibeos_edge_dma_type type = ibeos_edge_dma_chan_to_type(ppv);

    if(type==(ibeos_edge_dma_type)param) return true;

    return false;
}

int ibeos_edge_dma_init(struct ibeos_edge2_state *pps)
{
    int i, j;
    int ret;

    static const uint32_t offsets[NUM_PDMA] = { PF_DMA0_OFFSET, PF_DMA1_OFFSET };

    static struct dma_slave_map ibeos_edge2_dma_map[] = {
        { NULL, "tx", (void*)PF_DMA_W },
        { NULL, "rx", (void*)PF_DMA_R },
    };

    pps->dma = devm_kzalloc(&pps->pdev->dev, sizeof(*pps->dma), GFP_KERNEL);
    if(!pps->dma) return -ENOMEM;

    pps->dma->parent = pps;

    for(i=0; i<NUM_PDMA; ++i) {
        ibeos_edge2_dma_map[i].devname = dev_name(&pps->pdev->dev);
        pps->dma->regs[i] = pps->base[0]+offsets[i];
    }

    dma_cap_set(DMA_SLAVE, pps->dma->dma_dev.cap_mask);
    dma_cap_set(DMA_PRIVATE, pps->dma->dma_dev.cap_mask);

    pps->dma->dma_dev.filter.fn = ibeos_edge_dma_filter_fn;
    pps->dma->dma_dev.filter.mapcnt = ARRAY_SIZE(ibeos_edge2_dma_map);
    pps->dma->dma_dev.filter.map = ibeos_edge2_dma_map;

    pps->dma->dma_dev.max_burst = 256;
    pps->dma->dma_dev.device_alloc_chan_resources = ibeos_edge_dma_alloc_chan_resources;
    pps->dma->dma_dev.device_free_chan_resources = ibeos_edge_dma_free_chan_resources;
    pps->dma->dma_dev.device_prep_slave_sg = ibeos_edge_dma_prep_slave_sg;
    pps->dma->dma_dev.device_issue_pending = ibeos_edge_dma_issue_pending;
    pps->dma->dma_dev.device_tx_status = ibeos_edge_dma_tx_status;
    pps->dma->dma_dev.device_config = ibeos_edge_dma_config;
    pps->dma->dma_dev.device_terminate_all = ibeos_edge_dma_terminate_all;
    pps->dma->dma_dev.device_synchronize = ibeos_edge_dma_synchronize;
    pps->dma->dma_dev.src_addr_widths = DMA_SLAVE_BUSWIDTH_32_BYTES;
    pps->dma->dma_dev.dst_addr_widths = DMA_SLAVE_BUSWIDTH_32_BYTES;
    pps->dma->dma_dev.directions = BIT(DMA_MEM_TO_DEV)|BIT(DMA_DEV_TO_MEM);
    pps->dma->dma_dev.residue_granularity = DMA_RESIDUE_GRANULARITY_BURST;

    pps->dma->dma_dev.dev = &pps->pdev->dev;

    INIT_LIST_HEAD(&pps->dma->dma_dev.channels);
    for(j=0; j<NUM_VDMA; ++j) {
        pps->dma->chans[j].parent = pps->dma;

        pps->dma->chans[j].vchan.desc_free = ibeos_edge_dma_free_vdesc;
        vchan_init(&pps->dma->chans[j].vchan, &pps->dma->dma_dev);
    }

    ibeos_edge_dma_cdev_create(pps);

    for(i=0; i<NUM_PDMA; ++i) {
        for(j=0; j<NUM_DMA_IRQ; ++j) {
            /* map of END/ERR irq per physical dma */
            static irqreturn_t (*const irq_fns[NUM_PDMA][NUM_DMA_IRQ])(int, void*) = {
                { ibeos_edge2_dma_irq_end_W, ibeos_edge2_dma_irq_err_W },
                { ibeos_edge2_dma_irq_end_R, ibeos_edge2_dma_irq_err_R } };
            //static const char *names[NUM_PDMA][NUM_DMA_IRQ] = {
            //    { "edma W", "edma W err" },
            //    { "edma  R", "edma R err" } };

            ret = request_irq(irq_linear_revmap(pps->domain, irq_map[i][j]), irq_fns[i][j], 0, dev_name(&pps->pdev->dev), pps->dma);
            if(ret) {
                dev_err(&pps->pdev->dev, "failed to allocate irq %d for chan %d", irq_map[i][j], i);
                goto err_out;
            }

            pps->dma->irq_flag |= (1<<j);
        }
    }

    ret = dma_async_device_register(&pps->dma->dma_dev);
    if(ret) goto err_out;

    return 0;

err_out:
    teardown_dma_dev(pps);
    cond_teardown_irq(pps);
    teardown_cdevs(pps);

    return ret;
}

void ibeos_edge_dma_destroy(struct ibeos_edge2_state *pps)
{
    teardown_dma_dev(pps);
    teardown_cdevs(pps);
    cond_teardown_irq(pps);
}

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Steven Seeger <steven.seeger@flightsystems.net>");
MODULE_DESCRIPTION("MTD driver for PolarFire PCIe Host Bridge dma");

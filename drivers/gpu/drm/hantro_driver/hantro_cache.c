// SPDX-License-Identifier: GPL-2.0
/*
 *    Hantro cache controller hardware driver.
 *
 *    Copyright (c) 2017 - 2020, VeriSilicon Inc.
 *    Copyright (c) 2020 - 2021, Intel Corporation
 */

#include "hantro_priv.h"
#include "hantro_cache.h"

/*------------------PORTING LAYER------------------------------------*/
/* Cache types */
#define CCLIENT_TYPE_VC8000E	"_VC8000E"
#define CCLIENT_TYPE_VC8000D0	"_VC8000D_0"
#define CCLIENT_TYPE_VC8000D1	"_VC8000D_1"
#define CCLIENT_TYPE_DECG10	"_DECODER_G1_0"
#define CCLIENT_TYPE_DECG11	"_DECODER_G1_1"
#define CCLIENT_TYPE_DECG20	"_DECODER_G2_0"
#define CCLIENT_TYPE_DECG21	"_DECODER_G2_1"
/* Cache directions */
#define CC_DIR_READ	"_DIRRD"
#define CC_DIR_WRITE	"_DIRWR"
#define CC_DIR_BIDIR	"_DIRBI"

/*------------------------------END-------------------------------------*/

/***************************TYPE AND FUNCTION DECLARATION****************/

/* here's all the must remember stuff */
static int reserve_io(struct cache_dev_t *);
static void release_io(struct cache_dev_t *);
static void reset_asic(struct cache_dev_t *dev);
static irqreturn_t cache_isr(int irq, void *dev_id);
/*********************local variable declaration*****************/

/******************************************************************************/
static int check_cache_irq(struct cache_dev_t *dev)
{
	struct device_info *pdevinfo = dev->pdevinfo;
	unsigned long flags;
	int rdy = 0;

	spin_lock_irqsave(&pdevinfo->cache_owner_lock, flags);
	if (dev->irq_received) {
		/* reset the wait condition(s) */
		dev->irq_received = 0;
		rdy = 1;
	}

	spin_unlock_irqrestore(&pdevinfo->cache_owner_lock, flags);

	return rdy;
}

static unsigned int wait_cache_ready(struct cache_dev_t *dev)
{
	struct device_info *pdevinfo = dev->pdevinfo;

	if (wait_event_interruptible(pdevinfo->cache_wait_queue,
				     check_cache_irq(dev))) {
		PDEBUG("Cache wait_event_interruptible interrupted\n");
		return -ERESTARTSYS;
	}

	return 0;
}

static int check_core_occupation(struct cache_dev_t *dev, struct file *filp)
{
	struct device_info *pdevinfo = dev->pdevinfo;
	int ret = 0;
	unsigned long flags;

	spin_lock_irqsave(&pdevinfo->cache_owner_lock, flags);
	if (!dev->is_reserved) {
		dev->is_reserved = 1;
		dev->cacheowner = filp;
		ret = 1;
	}

	spin_unlock_irqrestore(&pdevinfo->cache_owner_lock, flags);
	return ret;
}

static int get_workable_core(struct cache_dev_t *dev, struct file *filp)
{
	return check_core_occupation(dev, filp);
}

static long reserve_core(struct cache_dev_t *dev, struct file *filp)
{
	struct device_info *pdevinfo;
	int ret = 0;

	START_TIME;
	pdevinfo = dev->pdevinfo;
	/* lock a core that has specified core id */
	if (wait_event_interruptible(pdevinfo->cache_hw_queue,
				     get_workable_core(dev, filp) != 0))
		ret = -ERESTARTSYS;

	trace_cache_reserve(pdevinfo->deviceid, (sched_clock() - start) / 1000);
	return ret;
}

static void release_core(struct cache_dev_t *dev)
{
	unsigned long flags;
	struct device_info *pdevinfo = dev->pdevinfo;

	/* release specified core id */
	spin_lock_irqsave(&pdevinfo->cache_owner_lock, flags);
	if (dev->is_reserved) {
		dev->cacheowner = NULL;
		dev->is_reserved = 0;
	}

	dev->irq_received = 0;
	dev->irq_status = 0;
	spin_unlock_irqrestore(&pdevinfo->cache_owner_lock, flags);
	wake_up_interruptible_all(&pdevinfo->cache_hw_queue);
	trace_cache_release(pdevinfo->deviceid);
}

long hantrocache_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	u32 tmp, id, deviceid, node, type;
	u32 core_id;
	unsigned long long tmp64;
	struct cache_dev_t *pccore;

	if (hantro_drm.device_type != DEVICE_KEEMBAY && enable_dec400 == 0)
		return -EFAULT;

	switch (cmd) {
	case CACHE_IOCGHWOFFSET:
		__get_user(id, (int *)arg);
		deviceid = DEVICE_ID(id);
		node = KCORE(id);
		pccore = get_cachenodes(deviceid, node);
		if (!pccore)
			return -EFAULT;

		__put_user(pccore->com_base_addr, (unsigned long long *)arg);
		break;
	case CACHE_IOCGHWIOSIZE:
		id = (u32)arg;
		deviceid = DEVICE_ID(id);
		node = KCORE(id);
		pccore = get_cachenodes(deviceid, node);
		if (!pccore)
			return -EFAULT;
		else
			return pccore->core_cfg.iosize;

		break;
	case CACHE_IOCG_CORE_NUM:
		id = arg;
		id = get_devicecorenum(id, CORE_CACHE);
		return id;
	case CACHE_IOCH_HW_RESERVE: {
		driver_cache_dir dir;
		cache_client_type client;
		/* it's a little danger here since here's no protection of the chain */
		__get_user(tmp64, (unsigned long long *)arg);
		id = tmp64 >> 32;
		deviceid = DEVICE_ID(id);
		type = NODETYPE(id);
		node = KCORE(id);
		core_id = (u32)tmp64; /* get client and direction info */
		dir = core_id & 0x01;
		client = (core_id & 0x06) >> 1;
		pccore = get_cachenodes(deviceid, 0);

		while (pccore) {
			/* a valid core supports such client and dir */
			if (pccore->core_cfg.client == client &&
			    pccore->core_cfg.dir == dir &&
			    pccore->parentid == node && pccore->is_valid &&
			    ((type == NODE_TYPE_DEC &&
			      pccore->parenttype == CORE_DEC) ||
			     (type == NODE_TYPE_ENC &&
			      pccore->parenttype == CORE_ENC)))
				break;

			pccore = pccore->next;
		}

		if (!pccore)
			return -EFAULT;

		ret = reserve_core(pccore, filp);
		if (ret == 0)
			return pccore->core_id;

		return ret;
	}
	case CACHE_IOCH_HW_RELEASE:
		core_id = (u32)arg;
		deviceid = DEVICE_ID(core_id);
		node = KCORE(core_id);
		pccore = get_cachenodes(deviceid, node);
		if (!pccore)
			return -EFAULT;

		release_core(pccore);
		break;
	case CACHE_IOCG_ABORT_WAIT:
		core_id = (u32)arg;
		deviceid = DEVICE_ID(core_id);
		node = KCORE(core_id);
		pccore = get_cachenodes(deviceid, node);
		if (!pccore)
			return -EFAULT;

		tmp = wait_cache_ready(pccore);
		if (tmp == 0)
			return pccore->irq_status;

		break;
	}
	return 0;
}

int cache_open(struct inode *inode, struct file *filp)
{
	if (hantro_drm.device_type != DEVICE_KEEMBAY && enable_dec400 == 0)
		return 0;

	return 0;
}

int cache_release(struct file *filp)
{
	int i, devicecnt = get_devicecount();
	struct cache_dev_t *dev;

	if (hantro_drm.device_type != DEVICE_KEEMBAY && enable_dec400 == 0)
		return 0;

	for (i = 0; i < devicecnt; i++) {
		dev = get_cachenodes(i, 0);
		while (dev) {
			if (dev->cacheowner == filp && dev->is_reserved) {
				reset_asic(dev);
				release_core(dev);
			}

			dev = dev->next;
		}
	}

	return 0;
}

static void pcie_close(void)
{
}

int __init hantrocache_init(void)
{
	return 0;
}

int __exit hantrocache_cleanup(void)
{
	return 0;
}

static void cache_getcachetype(const char *name, int *client, int *dir)
{
	if (strstr(name, CCLIENT_TYPE_VC8000E))
		*client = VC8000E;
	else if (strstr(name, CCLIENT_TYPE_VC8000D0))
		*client = VC8000D_0;
	else if (strstr(name, CCLIENT_TYPE_VC8000D1))
		*client = VC8000D_1;
	else if (strstr(name, CCLIENT_TYPE_DECG10))
		*client = DECODER_G1_0;
	else if (strstr(name, CCLIENT_TYPE_DECG11))
		*client = DECODER_G1_1;
	else if (strstr(name, CCLIENT_TYPE_DECG20))
		*client = DECODER_G2_0;
	else if (strstr(name, CCLIENT_TYPE_DECG21))
		*client = DECODER_G2_1;
	else
		*client = -1;

	if (strstr(name, CC_DIR_READ))
		*dir = DIR_RD;
	else if (strstr(name, CC_DIR_WRITE))
		*dir = DIR_WR;
	else if (strstr(name, CC_DIR_BIDIR))
		*dir = DIR_RD;
	else
		*dir = -1;
}

int cache_probe(dtbnode *pnode)
{
	int result;
	int i;
	struct cache_dev_t *pccore;
	int type, dir;

	if (hantro_drm.device_type != DEVICE_KEEMBAY && enable_dec400 == 0)
		return 0;

	cache_getcachetype(pnode->ofnode->name, &type, &dir);
	if (type == -1 || dir == -1)
		return -EINVAL;

	pccore = vmalloc(sizeof(*pccore));
	if (!pccore)
		return -ENOMEM;

	memset(pccore, 0, sizeof(struct cache_dev_t));
	pccore->com_base_addr = pnode->ioaddr;
	pccore->core_cfg.base_addr = pnode->ioaddr;
	pccore->core_cfg.iosize = pnode->iosize;
	pccore->core_cfg.client = type;
	pccore->core_cfg.dir = dir;

	result = reserve_io(pccore);
	if (result < 0) {
		pr_err("cachecore: reserve reg 0x%llx-0x%llx fail\n",
		       pnode->ioaddr, pnode->iosize);
		vfree(pccore);
		return -ENODEV;
	}

	reset_asic(pccore); /* reset hardware */
	pccore->is_valid = 1;
	for (i = 0; i < 4; i++)
		pccore->irqlist[i] = -1;

	if (enable_irqmode == 1) {
		if (pnode->irq[0] > 0) {
			strcpy(pccore->irq_name[0], pnode->irq_name[0]);
			result = request_irq(pnode->irq[0], cache_isr,
					     IRQF_SHARED, pccore->irq_name[0],
					     (void *)pccore);
			if (result == 0) {
				pccore->irqlist[0] = pnode->irq[0];
			} else {
				pr_err("cachecore: request IRQ <%d> fail\n",
				       pnode->irq[0]);
				release_io(pccore);
				vfree(pccore);
				return -EINVAL;
			}
		}
	}

	pccore->core_cfg.parentaddr = pnode->parentaddr;
	add_cachenode(pnode->pdevinfo, pccore);
	return 0;
}

void hantrocache_remove(struct device_info *pdevinfo)
{
	struct cache_dev_t *pccore, *pnext;
	int k;

	pccore = get_cachenodes(pdevinfo->deviceid, 0);
	while (pccore) {
		pnext = pccore->next;
		writel(0, pccore->hwregs + 0x04); /* disable HW */
		writel(0xF, pccore->hwregs + 0x14); /* clear IRQ */
		/* free the encoder IRQ */
		for (k = 0; k < 4; k++)
			if (pccore->irqlist[k] > 0)
				free_irq(pccore->irqlist[k], (void *)pccore);

		release_io(pccore);
		vfree(pccore);
		pccore = pnext;
	}

	pcie_close();
}

static int cache_get_hwid(unsigned long base_addr, int *hwid)
{
	u8 *hwregs = NULL;

	if (!request_mem_region(base_addr, 4, "hantro_cache")) {
		PDEBUG(KERN_INFO
		       "hantr_cache: failed to reserve HW regs,base_addr:%p\n",
		       (void *)base_addr);
		return -1;
	}

	hwregs = (u8 *)ioremap(base_addr, 4);
	if (!hwregs) {
		PDEBUG(KERN_INFO "hantr_cache: failed to ioremap HW regs\n");
		release_mem_region(base_addr, 4);
		return -1;
	}

	*hwid = readl(hwregs + 0x00);
	PDEBUG(KERN_INFO "hantro_cache: hwid = %x, base_addr= %p\n", (int)*hwid,
	       (void *)base_addr);

	if (hwregs)
		iounmap((void *)hwregs);

	release_mem_region(base_addr, 4);
	return 0;
}

static int reserve_io(struct cache_dev_t *pccore)
{
	int hwid, hw_cfg;

	if (cache_get_hwid(pccore->core_cfg.base_addr, &hwid) < 0)
		return -1;

	hw_cfg = (hwid & 0xF0000) >> 16;

	if (hw_cfg > 2)
		return -1;

	if (hw_cfg == 1 && pccore->core_cfg.dir == DIR_WR) /* cache only */
		pccore->is_valid = 0;
	else if (hw_cfg == 2 &&
		 pccore->core_cfg.dir == DIR_RD) /* shaper only */
		pccore->is_valid = 0;
	else
		pccore->is_valid = 1;

	if (pccore->is_valid == 0)
		return -1;

	if (hwid == 0 && pccore->core_cfg.dir == DIR_RD) {
		pccore->core_cfg.base_addr += CACHE_WITH_SHAPER_OFFSET;
	} else if (hwid != 0) {
		if (pccore->core_cfg.dir == DIR_WR)
			pccore->core_cfg.base_addr += SHAPER_OFFSET;
		else if (pccore->core_cfg.dir == DIR_RD && hw_cfg == 0)
			pccore->core_cfg.base_addr += CACHE_WITH_SHAPER_OFFSET;
		else if (pccore->core_cfg.dir == DIR_RD && hw_cfg == 1)
			pccore->core_cfg.base_addr += CACHE_ONLY_OFFSET;
	}

	if (!request_mem_region(pccore->core_cfg.base_addr,
				pccore->core_cfg.iosize, pccore->reg_name)) {
		PDEBUG(KERN_INFO
		       "hantr_cache: failed to reserve HW regs,core:%x\n",
		       hwid);
		pccore->is_valid = 0;
		return -1;
	}

	pccore->hwregs = (u8 *)ioremap(pccore->core_cfg.base_addr,
				       pccore->core_cfg.iosize);

	if (!pccore->hwregs) {
		PDEBUG(KERN_INFO
		       "hantr_cache: failed to ioremap HW regs,core:%x\n",
		       hwid);
		release_mem_region(pccore->core_cfg.base_addr,
				   pccore->core_cfg.iosize);
		pccore->is_valid = 0;
		return -1;
	}

	if (pccore->core_cfg.dir == DIR_RD)
		PDEBUG("cache  reg[0x10]=%08x\n", readl(pccore->hwregs + 0x10));
	else
		PDEBUG("shaper reg[0x08]=%08x\n", readl(pccore->hwregs + 0x08));

	pr_info("hantrocache: HW at base <0x%llx> with ID 0x%x [mapped addr = 0x%llx]\n",
		pccore->core_cfg.base_addr, hwid,
		(unsigned long long)pccore->hwregs);

	return 0;
}

static void release_io(struct cache_dev_t *pccore)
{
	if (pccore->is_valid == 0)
		return;

	if (pccore->hwregs)
		iounmap((void *)pccore->hwregs);

	release_mem_region(pccore->core_cfg.base_addr, pccore->core_cfg.iosize);
}

static irqreturn_t cache_isr(int irq, void *dev_id)
{
	unsigned int handled = 0;
	struct cache_dev_t *dev = (struct cache_dev_t *)dev_id;
	u32 irq_status;
	unsigned long flags;
	u32 irq_triggered = 0;
	struct device_info *pdevinfo;

	pdevinfo = dev->pdevinfo;
	/* If core is not reserved by any user, but irq is received, just ignore it */
	spin_lock_irqsave(&pdevinfo->cache_owner_lock, flags);
	if (!dev->is_reserved) {
		spin_unlock_irqrestore(&pdevinfo->cache_owner_lock, flags);
		return IRQ_HANDLED;
	}

	spin_unlock_irqrestore(&pdevinfo->cache_owner_lock, flags);
	if (dev->core_cfg.dir == DIR_RD) {
		irq_status = readl(dev->hwregs + 0x04);
		if (irq_status & 0x28) {
			irq_triggered = 1;
			writel(irq_status, dev->hwregs + 0x04); /* clear irq */
		}
	} else {
		irq_status = readl(dev->hwregs + 0x0C);
		if (irq_status) {
			irq_triggered = 1;
			writel(irq_status, dev->hwregs + 0x0C); /* clear irq */
		}
	}

	if (irq_triggered == 1) {
		/* clear all IRQ bits. IRQ is cleared by writing 1 */
		spin_lock_irqsave(&pdevinfo->cache_owner_lock, flags);
		dev->irq_received = 1;
		dev->irq_status = irq_status;
		spin_unlock_irqrestore(&pdevinfo->cache_owner_lock, flags);
		wake_up_interruptible_all(&pdevinfo->cache_wait_queue);
		handled++;
	}

	if (!handled)
		PDEBUG("IRQ received, but not cache's!\n");

	return IRQ_HANDLED;
}

static void reset_asic(struct cache_dev_t *dev)
{
	int i;

	if (dev->is_valid == 0)
		return;

	for (i = 0; i < dev->core_cfg.iosize; i += 4)
		writel(0, dev->hwregs + i);
}

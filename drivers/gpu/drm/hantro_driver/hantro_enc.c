// SPDX-License-Identifier: GPL-2.0
/*
 *    Hantro encoder hardware driver.
 *
 *    Copyright (c) 2017 - 2020, VeriSilicon Inc.
 *    Copyright (c) 2020 - 2021, Intel Corporation
 */

#include "hantro_priv.h"
#include "hantro_enc.h"

static u32 resource_shared;
/*------------------------------------------------------------------------
 *****************************PORTING LAYER********************************
 *-------------------------------------------------------------------------
 */

#define KMB_VC8000E_PAGE_LUT           0x20885000

#define HANTRO_VC8KE_REG_BWREAD_KMB	215
#define HANTRO_VC8KE_REG_BWREAD		216
#define HANTRO_VC8KE_REG_BWWRITE_KMB	219
#define HANTRO_VC8KE_REG_BWWRITE	220
#define VC8KE_BURSTWIDTH 16

/*------------------------------END-------------------------------------*/

/***************************TYPE AND FUNCTION DECLARATION****************/

/* here's all the must remember stuff */

static int reserve_io(struct hantroenc_t *pcore);
static void release_io(struct hantroenc_t *pcore);
static void reset_asic(struct hantroenc_t *dev);
static int check_core_occupation(struct hantroenc_t *dev);
static void release_encoder(struct hantroenc_t *dev, u32 *core_info,
			    u32 nodenum);
/* IRQ handler */
static irqreturn_t hantroenc_isr(int irq, void *dev_id);

/*********************local variable declaration*****************/
unsigned long long sram_base;
unsigned int sram_size;
/* and this is our MAJOR; use 0 for dynamic allocation (recommended) */
static int hantroenc_major;
/******************************************************************************/

static int check_enc_irq(struct hantroenc_t *dev, u32 *core_info,
			 u32 *irq_status, u32 nodenum)
{
	unsigned long flags;
	int rdy = 0;
	u32 i = 0;
	u8 core_mapping = 0;
	struct device_info *pdevinfo = dev->pdevinfo;

	core_mapping = (u8)(*core_info & 0xFF);

	while (core_mapping) {
		if (core_mapping & 0x1) {
			if (i >= nodenum)
				break;

			spin_lock_irqsave(&pdevinfo->enc_owner_lock, flags);

			if (dev->irq_received) {
				/* reset the wait condition(s) */
				PDEBUG("check %d irq ready\n", i);
				dev->irq_received = 0;
				rdy = 1;
				*core_info = i;
				*irq_status = dev->irq_status;
			}

			spin_unlock_irqrestore(&pdevinfo->enc_owner_lock,
					       flags);
			break;
		}
		core_mapping = core_mapping >> 1;
		i++;
		dev = dev->next;
	}

	return rdy;
}

static unsigned int wait_enc_ready(struct hantroenc_t *dev, u32 *core_info,
				   u32 *irq_status, u32 nodenum)
{
	struct device_info *pdevinfo = dev->pdevinfo;

	PDEBUG("%s\n", __func__);
	if (wait_event_interruptible(pdevinfo->enc_wait_queue,
				     check_enc_irq(dev, core_info, irq_status,
						   nodenum))) {
		PDEBUG("ENC wait_event_interruptible interrupted\n");
		release_encoder(dev, core_info, nodenum);
		return -ERESTARTSYS;
	}

	return 0;
}

u32 hantroenc_readbandwidth(struct device_info *pdevinfo, int is_read_bw)
{
	int i, devcnt = get_devicecount();
	u32 bandwidth = 0;
	struct hantroenc_t *pcore;

	if (!pdevinfo) {
		for (i = 0; i < devcnt; i++) {
			pcore = get_encnode_bydeviceid(i, 0);
			while (pcore) {
				if (is_read_bw) {
					if (hantro_drm.device_type == DEVICE_KEEMBAY)
						bandwidth +=
							ioread32((void *)(pcore->hwregs +
									  HANTRO_VC8KE_REG_BWREAD_KMB * 4));
					else
						bandwidth +=
							ioread32((void *)(pcore->hwregs +
									  HANTRO_VC8KE_REG_BWREAD * 4));
				} else {
					if (hantro_drm.device_type == DEVICE_KEEMBAY)
						bandwidth +=
							ioread32((void *)(pcore->hwregs +
									  HANTRO_VC8KE_REG_BWWRITE_KMB * 4));
					else
						bandwidth +=
							ioread32((void *)(pcore->hwregs +
									  HANTRO_VC8KE_REG_BWWRITE * 4));
				}

				pcore = pcore->next;
			}
		}
	} else {
		pcore = get_encnode(pdevinfo, 0);
		while (pcore) {
			if (is_read_bw) {
				if (hantro_drm.device_type == DEVICE_KEEMBAY)
					bandwidth += ioread32((void *)(pcore->hwregs +
								       HANTRO_VC8KE_REG_BWREAD_KMB * 4));
				else
					bandwidth += ioread32((void *)(pcore->hwregs +
								       HANTRO_VC8KE_REG_BWREAD * 4));

			} else {
				if (hantro_drm.device_type == DEVICE_KEEMBAY)
					bandwidth += ioread32((void *)(pcore->hwregs +
								       HANTRO_VC8KE_REG_BWWRITE_KMB * 4));
				else
					bandwidth += ioread32((void *)(pcore->hwregs +
								       HANTRO_VC8KE_REG_BWWRITE * 4));
			}

			pcore = pcore->next;
		}
	}

	return bandwidth * VC8KE_BURSTWIDTH;
}

static int check_core_occupation(struct hantroenc_t *dev)
{
	int ret = 0;
	unsigned long flags;
	struct device_info *pdevinfo = dev->pdevinfo;

	spin_lock_irqsave(&pdevinfo->enc_owner_lock, flags);
	if (!dev->is_reserved) {
		dev->is_reserved = 1;
		dev->pid = current->pid;
		ret = 1;
		PDEBUG("%s pid=%d\n", __func__, dev->pid);
	}

	spin_unlock_irqrestore(&pdevinfo->enc_owner_lock, flags);
	return ret;
}

static int get_workable_core(struct hantroenc_t *dev, u32 *core_info,
			     u32 *core_info_tmp, u32 nodenum)
{
	int ret = 0;
	u32 i = 0;
	u32 cores;
	u32 core_id = 0;
	u8 core_mapping = 0;
	u32 required_num = 0;

	cores = *core_info;
	required_num = ((cores >> CORE_INFO_AMOUNT_OFFSET) & 0x7) + 1;
	core_mapping = (u8)(cores & 0xFF);

	if (*core_info_tmp == 0)
		*core_info_tmp = required_num << 8;
	else
		required_num = ((*core_info_tmp & 0xF00) >> 8);

	PDEBUG("%s:required_num=%d,core_info=%x\n", __func__, required_num,
	       *core_info);
	if (required_num) {
		/* a valid free Core that has specified core id */
		while (core_mapping) {
			if (core_mapping & 0x1) {
				if (i >= nodenum)
					break;

				core_id = i;
				if (check_core_occupation(dev)) {
					*core_info_tmp =
						((((*core_info_tmp & 0xF00) >>
						   8) -
						  1)
						 << 8) |
						(*core_info_tmp & 0x0FF);
					*core_info_tmp =
						*core_info_tmp | (1 << core_id);

					if (((*core_info_tmp & 0xF00) >> 8) ==
					    0) {
						ret = 1;
						*core_info =
							(*core_info &
							 0xFFFFFF00) |
							(*core_info_tmp & 0xFF);
						*core_info_tmp = 0;
						required_num = 0;
						break;
					}
				}
			}

			core_mapping = core_mapping >> 1;
			i++;
			dev = dev->next;
		}
	} else {
		ret = 1;
	}

	PDEBUG("*core_info = %x\n", *core_info);
	return ret;
}

static long reserve_encoder(struct hantroenc_t *dev, u32 *core_info,
			    u32 nodenum)
{
	struct device_info *pdevinfo = dev->pdevinfo;
	struct hantroenc_t *reserved_core = NULL;
	u32 core_info_tmp = 0;
	int ret = 0;

	START_TIME;
	PDEBUG("hx280enc: %s\n", __func__);
	/* If HW resources are shared inter cores, just make sure only one is using the HW */
	if (resource_shared) {
		if (down_interruptible(&pdevinfo->enc_core_sem)) {
			ret = -ERESTARTSYS;
			nodenum = 0xffffffff;
			goto out;
		}
	}

	/* lock a core that has specified core id */
	if (wait_event_interruptible(pdevinfo->enc_hw_queue,
				     get_workable_core(dev, core_info,
						       &core_info_tmp,
						       nodenum) != 0)) {
		nodenum = 0xffffffff;
		ret = -ERESTARTSYS;
		goto out;
	}

	reserved_core = get_encnode(pdevinfo, KCORE(*core_info) - 1);
	if (!reserved_core) {
		pr_debug("Core not found. Possibly Lookahead node");
		goto out;
	}

	mutex_lock(&reserved_core->core_mutex);
	hantroenc_core_status_change(reserved_core, true);
	mutex_unlock(&reserved_core->core_mutex);

	if (pdevinfo->thermal_data.clk_freq != reserved_core->clk_freq) {
		clk_set_rate(pdevinfo->dev_clk[reserved_core->clock_index],
			     pdevinfo->thermal_data.clk_freq);
		reserved_core->clk_freq = pdevinfo->thermal_data.clk_freq;
	}

	reserved_core->perf_data.last_resv = sched_clock();
out:
	trace_core_reserve(reserved_core->node_name,
			  (sched_clock() - start) / 1000);
	return ret;
}

static void release_encoder(struct hantroenc_t *dev, u32 *core_info,
			    u32 nodenum)
{
	unsigned long flags;
	u32 core_num = 0;
	u32 i = 0, core_id;
	u8 core_mapping = 0;
	struct device_info *pdevinfo = dev->pdevinfo;
	struct hantroenc_t *reserved_core = NULL;

	core_id = KCORE((*core_info));
	reserved_core = get_encnode(pdevinfo, core_id - 1);
	if (reserved_core) {
		reserved_core->perf_data.count++;
		reserved_core->perf_data.totaltime +=
			(sched_clock() -
			 (reserved_core->perf_data.last_resv == 0 ?
				  sched_clock() :
				  reserved_core->perf_data.last_resv));
	}

	core_num = ((*core_info >> CORE_INFO_AMOUNT_OFFSET) & 0x7) + 1;
	core_mapping = (u8)(*core_info & 0xFF);
	PDEBUG("%s:core_num=%d,core_mapping=%x\n", __func__, core_num,
	       core_mapping);
	/* release specified core id */
	while (core_mapping) {
		if (core_mapping & 0x1) {
			if (i >= nodenum)
				break;

			core_id = i;
			spin_lock_irqsave(&pdevinfo->enc_owner_lock, flags);
			PDEBUG("dev[core_id].pid=%d,current->pid=%d\n",
			       dev->pid, current->pid);
			if (dev->is_reserved && dev->pid == current->pid) {
				dev->pid = -1;
				dev->is_reserved = 0;
				dev->irq_received = 0;
				dev->irq_status = 0;
			}
			spin_unlock_irqrestore(&pdevinfo->enc_owner_lock,
					       flags);
		}

		core_mapping = core_mapping >> 1;
		i++;
		dev = dev->next;
	}

	wake_up_interruptible_all(&pdevinfo->enc_hw_queue);
	if (resource_shared)
		up(&pdevinfo->enc_core_sem);

	trace_core_release(reserved_core->node_name);
}

long hantroenc_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	unsigned int id, tmp, node, deviceid;
	struct hantroenc_t *pcore;
	u32 core_info;

	if (enable_encode == 0)
		return -EFAULT;

	switch (cmd) {
	case HX280ENC_IOCGHWOFFSET: {
		__get_user(id, (unsigned long long *)arg);
		node = KCORE(id);
		deviceid = DEVICE_ID(id);
		pcore = get_encnode_bydeviceid(deviceid, node);
		if (!pcore)
			return -EFAULT;

		__put_user(pcore->core_cfg.base_addr,
			   (unsigned long long *)arg);
		break;
	}

	case HX280ENC_IOCGHWIOSIZE: {
		u32 io_size;

		__get_user(id, (unsigned long *)arg);
		node = KCORE(id);
		deviceid = DEVICE_ID(id);
		pcore = get_encnode_bydeviceid(deviceid, node);
		if (!pcore)
			return -EFAULT;

		io_size = pcore->core_cfg.iosize;
		__put_user(io_size, (u32 *)arg);
		return 0;
	}
	case HX280ENC_IOCGSRAMOFFSET:
		__put_user(sram_base, (unsigned long long *)arg);
		break;
	case HX280ENC_IOCGSRAMEIOSIZE:
		__put_user(sram_size, (unsigned int *)arg);
		break;
	case HX280ENC_IOCG_CORE_NUM:
		tmp = arg;
		return get_devicecorenum(tmp, CORE_ENC);
	case HX280ENC_IOCH_ENC_RESERVE: {
		int ret;

		PDEBUG("Reserve ENC Cores\n");
		__get_user(core_info, (unsigned long *)arg);
		deviceid = (core_info >> 16) & 0xff;
		pcore = get_encnode_bydeviceid(deviceid,
					       0); /* from list header */
		if (!pcore) {
			pr_err("wrong device num");
			return -EFAULT;
		}

		tmp = get_devicecorenum(deviceid, CORE_ENC);
		ret = reserve_encoder(pcore, &core_info, tmp);
		if (ret == 0)
			__put_user(core_info, (u32 *)arg);

		return ret;
	}
	case HX280ENC_IOCH_ENC_RELEASE: {
		__get_user(core_info, (unsigned long *)arg);
		deviceid = (core_info >> 16) & 0xff;
		pcore = get_encnode_bydeviceid(deviceid,
					       0); /* from list header */
		if (!pcore)
			return -EFAULT;

		PDEBUG("Release ENC Core\n");
		tmp = get_devicecorenum(deviceid, CORE_ENC);
		release_encoder(pcore, &core_info, tmp);
		break;
	}

	case HX280ENC_IOCG_CORE_WAIT: {
		u32 irq_status;

		__get_user(core_info, (u32 *)arg);
		deviceid = DEVICE_ID(core_info);
		pcore = get_encnode_bydeviceid(deviceid, 0);
		if (!pcore)
			return -EFAULT;

		tmp = get_devicecorenum(deviceid, CORE_ENC);
		tmp = wait_enc_ready(pcore, &core_info, &irq_status, tmp);
		if (tmp == 0) {
			__put_user(irq_status, (unsigned int *)arg);
			return core_info; /* return core_id */
		}

		__put_user(0, (unsigned int *)arg);
		return -1;
		break;
	}
	}
	return 0;
}

int hantroenc_release(void)
{
	struct device_info *pdevinfo;
	int i, devicecnt = get_devicecount();
	struct hantroenc_t *dev;
	unsigned long flags;

	if (enable_encode == 0)
		return 0;

	for (i = 0; i < devicecnt; i++) {
		dev = get_encnode_bydeviceid(i, 0);
		if (!dev)
			continue;

		pdevinfo = dev->pdevinfo;
		while (dev) {
			spin_lock_irqsave(&pdevinfo->enc_owner_lock, flags);
			if (dev->is_reserved == 1 && dev->pid == current->pid) {
				dev->pid = -1;
				dev->is_reserved = 0;
				dev->irq_received = 0;
				dev->irq_status = 0;
				PDEBUG("release reserved core\n");
			}
			spin_unlock_irqrestore(&pdevinfo->enc_owner_lock,
					       flags);
			dev = dev->next;
		}

		wake_up_interruptible_all(&pdevinfo->enc_hw_queue);
		if (resource_shared)
			up(&pdevinfo->enc_core_sem);
	}

	return 0;
}

static void setup_enc_lut(void)
{
	u8 *enc_page_lut_regs;

	if (hantro_drm.enc_page_lut_regs) {
		pr_info("hantroenc: page_lut already reserved\n");
		return;
	}

	/* Register and set the page lookup table for read */
	if (!request_mem_region(KMB_VC8000E_PAGE_LUT, 0x100,
				"hantroenc_pagelut_read")) {
		pr_err("hantroenc: failed to reserve page lookup table registers\n");
		return;
	}

	enc_page_lut_regs = (u8 *)ioremap(KMB_VC8000E_PAGE_LUT, 0x100);
	if (!enc_page_lut_regs) {
		pr_err("hantroenc: failed to ioremap page lookup table registers\n");
		return;
	}

	/* Set write page LUT AXI ID 1-8 to 0x4 */
	iowrite32(0x04040400, (void *)enc_page_lut_regs + 0x10);
	pr_info("hx280enc: Page LUT WR AXI ID 3:0 = %x\n",
		ioread32((void *)enc_page_lut_regs + 0x10));
	iowrite32(0x04040404, (void *)enc_page_lut_regs + 0x14);
	pr_info("hx280enc: Page LUT WR AXI ID 7:4 = %x\n",
		ioread32((void *)enc_page_lut_regs + 0x14));
	iowrite32(0x00000004, (void *)enc_page_lut_regs + 0x18);
	pr_info("hx280enc: Page LUT WR AXI ID 8 = %x\n",
		ioread32((void *)enc_page_lut_regs + 0x18));
	iowrite32(0x04040004, (void *)enc_page_lut_regs);
	pr_info("hx280enc: RD AXI 3:0 = %x\n",
		ioread32((void *)enc_page_lut_regs));
	iowrite32(0x04040404, (void *)enc_page_lut_regs + 0x4);
	pr_info("hx280enc: RD AXI 7:4  = %x\n",
		ioread32((void *)enc_page_lut_regs + 0x4));
	iowrite32(0x00000004, (void *)enc_page_lut_regs + 0x8);
	pr_info("hx280enc: RD AXI 8 = %x\n",
		ioread32((void *)enc_page_lut_regs + 0x8));
	hantro_drm.enc_page_lut_regs = enc_page_lut_regs;
}

int __init hantroenc_init(void)
{
	sram_base = 0;
	sram_size = 0;
	hantroenc_major = 0;
	resource_shared = 0;
	if (hantro_drm.device_type == DEVICE_KEEMBAY) {
		if (enable_enc_lut)
			setup_enc_lut();
	}

	return 0;
}

int __exit hantroenc_cleanup(void)
{
	if (hantro_drm.enc_page_lut_regs) {
		iounmap((void *)hantro_drm.enc_page_lut_regs);
		hantro_drm.enc_page_lut_regs = NULL;
		release_mem_region(KMB_VC8000E_PAGE_LUT, 0x100);
	}

	return 0;
}

void hantroenc_core_status_change(struct hantroenc_t *pcore, bool turnon)
{
	START_TIME;

	if (!pcore || pcore->clock_index < 0)
		return;

	if (turnon && !pcore->enabled) {

		hantro_clock_control(pcore->pdevinfo, pcore->clock_index, true);
		hantro_reset_control(pcore->pdevinfo, pcore->reset_index, true);
		hantro_reset_control(pcore->pdevinfo, pcore->reset_index+1, true);
		hantro_reset_control(pcore->pdevinfo, pcore->reset_index+2, true);
		hantro_powerdomain_control(pcore->pdevinfo, pcore->pd_index, true);
		pcore->perf_data.last_resv = sched_clock();
		pcore->enabled = 1;
		trace_core_status_update(pcore->node_name,  "On", (sched_clock() - start) / 1000);
		msleep(1);
	} else
	if(!turnon && pcore->enabled)
	{
		pcore->enabled = 0;
		hantro_powerdomain_control(pcore->pdevinfo, pcore->pd_index, false);
		hantro_reset_control(pcore->pdevinfo, pcore->reset_index, false);
		hantro_reset_control(pcore->pdevinfo, pcore->reset_index+1, false);
		hantro_reset_control(pcore->pdevinfo, pcore->reset_index+2, false);
		hantro_clock_control(pcore->pdevinfo, pcore->clock_index, false);
		trace_core_status_update(pcore->node_name,  "Off", (sched_clock() - start) / 1000);
		msleep(1);
	}

}

void hantroenc_device_change_status(struct device_info *pdevinfo, bool turnon)
{
	struct hantroenc_t *enc_core = pdevinfo->enchdr;
	while (enc_core) {
		mutex_lock(&enc_core->core_mutex);
		hantroenc_core_status_change(enc_core, turnon);
		mutex_unlock(&enc_core->core_mutex);

		enc_core = enc_core->next;
	}
}

int hantroenc_probe(dtbnode *pnode)
{
	int result = 0;
	struct hantroenc_t *pcore;
	int i;
	int irqn;

	if (enable_encode == 0)
		return 0;

	pcore = vmalloc(sizeof(*pcore));
	if (!pcore)
		return -ENOMEM;

	memset(pcore, 0, sizeof(struct hantroenc_t));

	strncpy(pcore->node_name, pnode->node_name, NODE_NAME_SIZE);
	pcore->core_cfg.base_addr = pnode->ioaddr;
	pcore->core_cfg.iosize = pnode->iosize;
	pcore->reset_index = pnode->reset_index;
	pcore->clock_index = pnode->clock_index;
	pcore->pd_index = pnode->pd_index;
	pcore->enabled = 1;
	pcore->perf_data.last_resv = sched_clock();

	result = reserve_io(pcore);
	if (result < 0) {
		pr_err("hx280enc: reserve reg 0x%llx:%lldfail\n", pnode->ioaddr,
		       pnode->iosize);
		vfree(pcore);
		return -ENODEV;
	}

	reset_asic(pcore); /* reset hardware */
	irqn = 0;
	for (i = 0; i < 4; i++)
		pcore->irqlist[i] = -1;

	if (enable_irqmode == 1) {
		for (i = 0; i < 4; i++) {
			if (pnode->irq[i] > 0) {
				strcpy(pcore->irq_name[i], pnode->irq_name[i]);
				result = request_irq(pnode->irq[i],
						     hantroenc_isr, IRQF_SHARED,
						     pcore->irq_name[i],
						     (void *)pcore);
				if (result == 0) {
					pcore->irqlist[irqn] = pnode->irq[i];
					irqn++;
				} else {
					pr_info("hx280enc: request IRQ <%d> fail\n",
						pnode->irq[i]);
					release_io(pcore);
					vfree(pcore);
					return -EINVAL;
				}
			}
		}
	}

	mutex_init(&pcore->core_mutex);
	add_encnode(pnode->pdevinfo, pcore);

	pr_info("hx280enc: module inserted. Major <%d>\n", hantroenc_major);
	return 0;
}

void hantroenc_remove(struct device_info *pdevinfo)
{
	int k;
	struct hantroenc_t *pcore, *pnext;

	pcore = get_encnode(pdevinfo, 0);
	while (pcore) {
		u32 hwid = pcore->hw_id;
		u32 major_id = (hwid & 0x0000FF00) >> 8;
		u32 wclr = (major_id >= 0x61) ? (0x1FD) : (0);

		pnext = pcore->next;
		iowrite32(0, (void *)(pcore->hwregs + 0x14)); /* disable HW */
		iowrite32(wclr,
			  (void *)(pcore->hwregs + 0x04)); /* clear enc IRQ */

		/* free the encoder IRQ */
		for (k = 0; k < 4; k++)
			if (pcore->irqlist[k] > 0)
				free_irq(pcore->irqlist[k], (void *)pcore);

		release_io(pcore);
		mutex_destroy(&pcore->core_mutex);
		vfree(pcore);
		pcore = pnext;
	}
}

static int reserve_io(struct hantroenc_t *pcore)
{
	u32 hwid;

	PDEBUG("hx280enc: %s called\n", __func__);
	if (!request_mem_region(pcore->core_cfg.base_addr,
				pcore->core_cfg.iosize, pcore->reg_name)) {
		pr_info("hantroenc: failed to reserve HW regs\n");
		return -1;
	}

	pcore->hwregs = (u8 *)ioremap(pcore->core_cfg.base_addr,
				      pcore->core_cfg.iosize);
	if (!pcore->hwregs) {
		pr_info("hantroenc: failed to ioremap HW regs\n");
		release_mem_region(pcore->core_cfg.base_addr,
				   pcore->core_cfg.iosize);
		return -1;
	}

	/* read hwid and check validness and store it */
	hwid = (u32)ioread32((void *)pcore->hwregs);
	/* check for encoder HW ID */
	if (((((hwid >> 16) & 0xFFFF) != ((ENC_HW_ID1 >> 16) & 0xFFFF))) &&
	    ((((hwid >> 16) & 0xFFFF) != ((ENC_HW_ID2 >> 16) & 0xFFFF)))) {
		pr_info("hantroenc: HW not found at %llx, HWID = 0x%x\n",
			pcore->core_cfg.base_addr, (hwid >> 16) & 0xFFFF);
		release_io(pcore);
		return -1;
	}

	pcore->hw_id = hwid;
	pr_info("hantroenc: HW at base <0x%llx> with ID 0x%x\n",
		pcore->core_cfg.base_addr, (hwid >> 16) & 0xFFFF);
	return 0;
}

static void release_io(struct hantroenc_t *pcore)
{
	if (pcore->hwregs)
		iounmap((void *)pcore->hwregs);
	release_mem_region(pcore->core_cfg.base_addr, pcore->core_cfg.iosize);
}

static irqreturn_t hantroenc_isr(int irq, void *dev_id)
{
	unsigned int handled = 0;
	struct hantroenc_t *dev = (struct hantroenc_t *)dev_id;
	u32 irq_status;
	unsigned long flags;
	struct device_info *pdevinfo = dev->pdevinfo;

	/*
	 * If core is not reserved by any user, but irq is received, just
	 * ignore it
	 */
	spin_lock_irqsave(&pdevinfo->enc_owner_lock, flags);
	if (!dev->is_reserved) {
		PDEBUG("%s:received IRQ but core is not reserved!\n", __func__);
		irq_status = (u32)ioread32((void *)(dev->hwregs + 0x04));
		if (irq_status & 0x01) {
			/*
			 * clear all IRQ bits. (hwid >= 0x80006100) means IRQ
			 * is cleared by writing 1
			 */
			u32 hwid = ioread32((void *)dev->hwregs);
			u32 major_id = (hwid & 0x0000FF00) >> 8;
			u32 wclr = (major_id >= 0x61) ? irq_status :
							(irq_status & (~0x1FD));

			/*
			 * Disable HW when buffer over-flow happen
			 * HW behavior changed in over-flow
			 * in-pass, HW cleanup HWIF_ENC_E auto
			 * new version:  ask SW cleanup HWIF_ENC_E when buffer
			 * over-flow
			 */
			if (irq_status & 0x20)
				iowrite32(0, (void *)(dev->hwregs + 0x14));
			iowrite32(wclr, (void *)(dev->hwregs + 0x04));
		}

		spin_unlock_irqrestore(&pdevinfo->enc_owner_lock, flags);
		return IRQ_HANDLED;
	}

	spin_unlock_irqrestore(&pdevinfo->enc_owner_lock, flags);
	irq_status = (u32)ioread32((void *)(dev->hwregs + 0x04));
	if (irq_status & 0x01) {
		/*
		 * clear all IRQ bits. (hwid >= 0x80006100) means IRQ is
		 * cleared by writing 1
		 */
		u32 hwid = ioread32((void *)dev->hwregs);
		u32 major_id = (hwid & 0x0000FF00) >> 8;
		u32 wclr = (major_id >= 0x61) ? irq_status :
						(irq_status & (~0x1FD));
		if (irq_status & 0x20)
			iowrite32(0, (void *)(dev->hwregs + 0x14));

		iowrite32(wclr, (void *)(dev->hwregs + 0x04));
		spin_lock_irqsave(&pdevinfo->enc_owner_lock, flags);
		dev->irq_received = 1;
		dev->irq_status = irq_status & (~0x01);
		spin_unlock_irqrestore(&pdevinfo->enc_owner_lock, flags);
		wake_up_interruptible_all(&pdevinfo->enc_wait_queue);
		handled++;
	}

	if (!handled)
		pr_info("IRQ received, but not hantro enc's!\n");

	return IRQ_HANDLED;
}

static void reset_asic(struct hantroenc_t *dev)
{
	int i;

	PDEBUG("hx280enc: %s\n", __func__);
	iowrite32(0, (void *)(dev->hwregs + 0x14));
	for (i = 4; i < dev->core_cfg.iosize; i += 4)
		iowrite32(0, (void *)(dev->hwregs + i));
}

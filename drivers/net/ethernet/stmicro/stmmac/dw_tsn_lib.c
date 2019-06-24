// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/* Copyright (c) 2019, Intel Corporation.
 * dw_tsn_lib.c: DW EQoS v5.00 TSN capabilities
 */

#include "dwmac4.h"
#include "dwmac5.h"
#include "dw_tsn_lib.h"

static struct tsn_hw_cap dw_tsn_hwcap;
static bool dw_tsn_feat_en[TSN_FEAT_ID_MAX];
static unsigned int dw_tsn_hwtunable[TSN_HWTUNA_MAX];
static struct est_gc_config dw_est_gc_config;
static struct tsn_err_stat dw_err_stat;

static unsigned int est_get_gcl_depth(unsigned int hw_cap)
{
	unsigned int estdep = (hw_cap & GMAC_HW_FEAT_ESTDEP)
			>> GMAC_HW_FEAT_ESTDEP_SHIFT;
	unsigned int depth;

	switch (estdep) {
	case 1:
		depth = 64;
		break;
	case 2:
		depth = 128;
		break;
	case 3:
		depth = 256;
		break;
	case 4:
		depth = 512;
		break;
	case 5:
		depth = 1024;
		break;
	default:
		depth = 0;
	}

	return depth;
}

static unsigned int est_get_ti_width(unsigned int hw_cap)
{
	unsigned int estwid = (hw_cap & GMAC_HW_FEAT_ESTWID)
			>> GMAC_HW_FEAT_ESTWID_SHIFT;
	unsigned int width;

	switch (estwid) {
	case 1:
		width = 16;
		break;
	case 2:
		width = 20;
		break;
	case 3:
		width = 24;
		break;
	default:
		width = 0;
	}

	return width;
}

static int est_poll_srwo(void *ioaddr)
{
	/* Poll until the EST GCL Control[SRWO] bit clears.
	 * Total wait = 12 x 50ms ~= 0.6s.
	 */
	unsigned int retries = 12;
	unsigned int value;

	do {
		value = TSN_RD32(ioaddr + MTL_EST_GCL_CTRL);
		if (!(value & MTL_EST_GCL_CTRL_SRWO))
			return 0;
		msleep(50);
	} while (--retries);

	return -ETIMEDOUT;
}

static int est_set_gcl_addr(void *ioaddr, unsigned int addr,
			    unsigned int gcrr, unsigned int rwops,
			    unsigned int dbgb, unsigned int dbgm)
{
	unsigned int value;

	value = MTL_EST_GCL_CTRL_ADDR_VAL(addr) & MTL_EST_GCL_CTRL_ADDR;

	if (dbgm) {
		if (dbgb)
			value |= MTL_EST_GCL_CTRL_DBGB1;

		value |= MTL_EST_GCL_CTRL_DBGM;
	}

	if (gcrr)
		value |= MTL_EST_GCL_CTRL_GCRR;

	/* This is the only place SRWO is set and driver polls SRWO
	 * for self-cleared before exit. Therefore, caller should
	 * check return status for possible time out error.
	 */
	value |= (rwops | MTL_EST_GCL_CTRL_SRWO);

	TSN_WR32(value, ioaddr + MTL_EST_GCL_CTRL);

	return est_poll_srwo(ioaddr);
}

static int est_write_gcl_config(void *ioaddr, unsigned int data,
				unsigned int addr, unsigned int gcrr,
				unsigned int dbgb, unsigned int dbgm)
{
	TSN_WR32(data, ioaddr + MTL_EST_GCL_DATA);

	return est_set_gcl_addr(ioaddr, addr, gcrr, GCL_OPS_W, dbgb, dbgm);
}

static int est_read_gcl_config(void *ioaddr, unsigned int *data,
			       unsigned int addr, unsigned int gcrr,
			       unsigned int dbgb, unsigned int dbgm)
{
	int ret;

	ret = est_set_gcl_addr(ioaddr, addr, gcrr, GCL_OPS_R, dbgb, dbgm);
	if (ret)
		return ret;

	*data = TSN_RD32(ioaddr + MTL_EST_GCL_DATA);

	return ret;
}

static int est_read_gce(void *ioaddr, unsigned int row,
			unsigned int *gates, unsigned int *ti_nsec,
			unsigned int dbgb, unsigned int dbgm)
{
	struct tsn_hw_cap *cap = &dw_tsn_hwcap;
	unsigned int ti_wid = cap->ti_wid;
	unsigned int gates_mask;
	unsigned int ti_mask;
	unsigned int value;
	int ret;

	gates_mask = (1 << cap->txqcnt) - 1;
	ti_mask = (1 << ti_wid) - 1;

	ret = est_read_gcl_config(ioaddr, &value, row, 0, dbgb, dbgm);
	if (ret) {
		TSN_ERR("Read GCE failed! row=%u\n", row);

		return ret;
	}
	*ti_nsec = value & ti_mask;
	*gates = (value >> ti_wid) & gates_mask;

	return ret;
}

static unsigned int est_get_gcl_total_intervals_nsec(unsigned int bank,
						     unsigned int gcl_len)
{
	struct est_gc_entry *gcl = dw_est_gc_config.gcb[bank].gcl;
	unsigned int nsec = 0;
	unsigned int row;

	for (row = 0; row < gcl_len; row++) {
		nsec += gcl->ti_nsec;
		gcl++;
	}

	return nsec;
}

static int est_set_tils(void *ioaddr, const unsigned int tils)
{
	struct tsn_hw_cap *cap = &dw_tsn_hwcap;
	unsigned int value;

	if (!dw_tsn_feat_en[TSN_FEAT_ID_EST])
		return -ENOTSUPP;

	if (tils > cap->tils_max) {
		TSN_WARN("EST: invalid tils(%u), max=%u\n",
			 tils, cap->tils_max);

		return -EINVAL;
	}

	/* Ensure that HW is not in the midst of GCL transition */
	value = TSN_RD32(ioaddr + MTL_EST_CTRL);
	value &= ~MTL_EST_CTRL_SSWL;

	/* MTL_EST_CTRL value has been read earlier, if TILS value
	 * differs, we update here.
	 */
	if (tils != dw_tsn_hwtunable[TSN_HWTUNA_TX_EST_TILS]) {
		value &= ~MTL_EST_CTRL_TILS;
		value |= (tils << MTL_EST_CTRL_TILS_SHIFT);

		TSN_WR32(value, ioaddr + MTL_EST_CTRL);
		dw_tsn_hwtunable[TSN_HWTUNA_TX_EST_TILS] = tils;
	}

	return 0;
}

static int est_set_ov(void *ioaddr,
		      const unsigned int *ptov,
		      const unsigned int *ctov)
{
	unsigned int value;

	if (!dw_tsn_feat_en[TSN_FEAT_ID_EST])
		return -ENOTSUPP;

	value = TSN_RD32(ioaddr + MTL_EST_CTRL);
	value &= ~MTL_EST_CTRL_SSWL;

	if (ptov) {
		if (*ptov > EST_PTOV_MAX) {
			TSN_WARN("EST: invalid PTOV(%u), max=%u\n",
				 *ptov, EST_PTOV_MAX);

			return -EINVAL;
		} else if (*ptov !=
			   dw_tsn_hwtunable[TSN_HWTUNA_TX_EST_PTOV]) {
			value &= ~MTL_EST_CTRL_PTOV;
			value |= (*ptov << MTL_EST_CTRL_PTOV_SHIFT);
			dw_tsn_hwtunable[TSN_HWTUNA_TX_EST_PTOV] = *ptov;
		}
	}

	if (ctov) {
		if (*ctov > EST_CTOV_MAX) {
			TSN_WARN("EST: invalid CTOV(%u), max=%u\n",
				 *ctov, EST_CTOV_MAX);

			return -EINVAL;
		} else if (*ctov != dw_tsn_hwtunable[TSN_HWTUNA_TX_EST_CTOV]) {
			value &= ~MTL_EST_CTRL_CTOV;
			value |= (*ctov << MTL_EST_CTRL_CTOV_SHIFT);
			dw_tsn_hwtunable[TSN_HWTUNA_TX_EST_CTOV] = *ctov;
		}
	}

	TSN_WR32(value, ioaddr + MTL_EST_CTRL);

	return 0;
}

void dwmac_tsn_init(void *ioaddr)
{
	unsigned int hwid = TSN_RD32(ioaddr + GMAC4_VERSION) & TSN_VER_MASK;
	unsigned int hw_cap2 = TSN_RD32(ioaddr + GMAC_HW_FEATURE2);
	unsigned int hw_cap3 = TSN_RD32(ioaddr + GMAC_HW_FEATURE3);
	struct tsn_hw_cap *cap = &dw_tsn_hwcap;
	unsigned int gcl_depth;
	unsigned int tils_max;
	unsigned int ti_wid;

	memset(cap, 0, sizeof(*cap));

	if (hwid < TSN_CORE_VER) {
		TSN_WARN_NA("IP v5.00 does not support TSN\n");
		return;
	}

	if (!(hw_cap3 & GMAC_HW_FEAT_ESTSEL)) {
		TSN_WARN_NA("EST NOT supported\n");
		cap->est_support = 0;

		return;
	}

	gcl_depth = est_get_gcl_depth(hw_cap3);
	ti_wid = est_get_ti_width(hw_cap3);

	cap->ti_wid = ti_wid;
	cap->gcl_depth = gcl_depth;

	tils_max = (hw_cap3 & GMAC_HW_FEAT_ESTSEL ? 3 : 0);
	tils_max = (1 << tils_max) - 1;
	cap->tils_max = tils_max;

	cap->ext_max = EST_TIWID_TO_EXTMAX(ti_wid);
	cap->txqcnt = ((hw_cap2 & GMAC_HW_FEAT_TXQCNT) >> 6) + 1;
	cap->est_support = 1;

	TSN_INFO("EST: depth=%u, ti_wid=%u, tils_max=%u tqcnt=%u\n",
		 gcl_depth, ti_wid, tils_max, cap->txqcnt);
}

/* dwmac_tsn_setup is called within stmmac_hw_setup() after
 * stmmac_init_dma_engine() which resets MAC controller.
 * This is so-that MAC registers are not cleared.
 */
void dwmac_tsn_setup(void *ioaddr)
{
	struct tsn_hw_cap *cap = &dw_tsn_hwcap;
	unsigned int value;

	if (cap->est_support) {
		/* Enable EST interrupts */
		value = (MTL_EST_INT_EN_CGCE | MTL_EST_INT_EN_IEHS |
			 MTL_EST_INT_EN_IEHF | MTL_EST_INT_EN_IEBE |
			 MTL_EST_INT_EN_IECC);
		TSN_WR32(value, ioaddr + MTL_EST_INT_EN);
	}
}

void dwmac_get_tsn_hwcap(struct tsn_hw_cap **tsn_hwcap)
{
	*tsn_hwcap = &dw_tsn_hwcap;
}

void dwmac_set_est_gcb(struct est_gc_entry *gcl, unsigned int bank)
{
	if (bank >= 0 && bank < EST_GCL_BANK_MAX)
		dw_est_gc_config.gcb[bank].gcl = gcl;
}

void dwmac_set_tsn_feat(enum tsn_feat_id featid, bool enable)
{
	if (featid < TSN_FEAT_ID_MAX)
		dw_tsn_feat_en[featid] = enable;
}

int dwmac_set_tsn_hwtunable(void *ioaddr,
			    enum tsn_hwtunable_id id,
			    const unsigned int *data)
{
	int ret = 0;

	switch (id) {
	case TSN_HWTUNA_TX_EST_TILS:
		ret = est_set_tils(ioaddr, *data);
		break;
	case TSN_HWTUNA_TX_EST_PTOV:
		ret = est_set_ov(ioaddr, data, NULL);
		break;
	case TSN_HWTUNA_TX_EST_CTOV:
		ret = est_set_ov(ioaddr, NULL, data);
		break;
	default:
		ret = -EINVAL;
	};

	return ret;
}

int dwmac_get_tsn_hwtunable(enum tsn_hwtunable_id id, unsigned int *data)
{
	if (id >= TSN_HWTUNA_MAX)
		return -EINVAL;

	*data = dw_tsn_hwtunable[id];

	return 0;
}

int dwmac_get_est_bank(void *ioaddr, unsigned int own)
{
	int swol;

	if (!dw_tsn_feat_en[TSN_FEAT_ID_EST])
		return -ENOTSUPP;

	swol = TSN_RD32(ioaddr + MTL_EST_STATUS);

	swol = ((swol & MTL_EST_STATUS_SWOL) >>
		MTL_EST_STATUS_SWOL_SHIFT);

	if (own)
		return swol;
	else
		return (~swol & 0x1);
}

int dwmac_set_est_gce(void *ioaddr,
		      struct est_gc_entry *gce, unsigned int row,
		      unsigned int dbgb, unsigned int dbgm)
{
	struct tsn_hw_cap *cap = &dw_tsn_hwcap;
	unsigned int ti_nsec = gce->ti_nsec;
	unsigned int gates = gce->gates;
	struct est_gc_entry *gcl;
	unsigned int gates_mask;
	unsigned int ti_wid;
	unsigned int ti_max;
	unsigned int value;
	unsigned int bank;
	int ret;

	if (!dw_tsn_feat_en[TSN_FEAT_ID_EST])
		return -ENOTSUPP;

	if (dbgb >= EST_GCL_BANK_MAX)
		return -EINVAL;

	if (dbgm) {
		bank = dbgb;
	} else {
		value = TSN_RD32(ioaddr + MTL_EST_STATUS);
		bank = (value & MTL_EST_STATUS_SWOL) >>
		       MTL_EST_STATUS_SWOL_SHIFT;
	}

	if (!cap->gcl_depth || row > cap->gcl_depth) {
		TSN_WARN("EST: row(%u) > GCL depth(%u)\n",
			 row, cap->gcl_depth);

		return -EINVAL;
	}

	ti_wid = cap->ti_wid;
	ti_max = (1 << ti_wid) - 1;
	if (ti_nsec > ti_max) {
		TSN_WARN("EST: ti_nsec(%u) > upper limit(%u)\n",
			 ti_nsec, ti_max);

		return -EINVAL;
	}

	gates_mask = (1 << cap->txqcnt) - 1;
	value = ((gates & gates_mask) << ti_wid) | ti_nsec;

	ret = est_write_gcl_config(ioaddr, value, row, 0, dbgb, dbgm);
	if (ret) {
		TSN_ERR("EST: GCE write failed: bank=%u row=%u.\n",
			bank, row);

		return ret;
	}

	TSN_INFO("EST: GCE write: dbgm=%u bank=%u row=%u, gc=0x%x.\n",
		 dbgm, bank, row, value);

	/* Since GC write is successful, update GCL copy of the driver */
	gcl = dw_est_gc_config.gcb[bank].gcl + row;
	gcl->gates = gates;
	gcl->ti_nsec = ti_nsec;

	return ret;
}

int dwmac_get_est_gcrr_llr(void *ioaddr, unsigned int *gcl_len,
			   unsigned int dbgb, unsigned int dbgm)
{
	unsigned int bank, value;
	int ret;

	if (!dw_tsn_feat_en[TSN_FEAT_ID_EST])
		return -ENOTSUPP;

	if (dbgb >= EST_GCL_BANK_MAX)
		return -EINVAL;

	if (dbgm) {
		bank = dbgb;
	} else {
		value = TSN_RD32(ioaddr + MTL_EST_STATUS);
		bank = (value & MTL_EST_STATUS_SWOL) >>
		       MTL_EST_STATUS_SWOL_SHIFT;
	}

	ret = est_read_gcl_config(ioaddr, &value,
				  GCL_CTRL_ADDR_LLR, 1,
				  dbgb, dbgm);
	if (ret) {
		TSN_ERR("read LLR fail at bank=%u\n", bank);

			return ret;
	}

	*gcl_len = value;

	return 0;
}

int dwmac_set_est_gcrr_llr(void *ioaddr, unsigned int gcl_len,
			   unsigned int dbgb, unsigned int dbgm)
{
	struct tsn_hw_cap *cap = &dw_tsn_hwcap;
	unsigned int bank, value;
	struct est_gcrr *bgcrr;
	int ret = 0;

	if (!dw_tsn_feat_en[TSN_FEAT_ID_EST])
		return -ENOTSUPP;

	if (dbgb >= EST_GCL_BANK_MAX)
		return -EINVAL;

	if (dbgm) {
		bank = dbgb;
	} else {
		value = TSN_RD32(ioaddr + MTL_EST_STATUS);
		bank = (value & MTL_EST_STATUS_SWOL) >>
		       MTL_EST_STATUS_SWOL_SHIFT;
	}

	if (gcl_len > cap->gcl_depth) {
		TSN_WARN("EST: GCL length(%u) > depth(%u)\n",
			 gcl_len, cap->gcl_depth);

		return -EINVAL;
	}

	bgcrr = &dw_est_gc_config.gcb[bank].gcrr;

	if (gcl_len != bgcrr->llr) {
		ret = est_write_gcl_config(ioaddr, gcl_len,
					   GCL_CTRL_ADDR_LLR, 1,
					   dbgb, dbgm);
		if (ret) {
			TSN_ERR_NA("EST: GCRR programming failure!\n");

			return ret;
		}
		bgcrr->llr = gcl_len;
	}

	return 0;
}

int dwmac_set_est_gcrr_times(void *ioaddr,
			     struct est_gcrr *gcrr,
			     unsigned int dbgb, unsigned int dbgm)
{
	unsigned int cycle_nsec = gcrr->cycle_nsec;
	unsigned int cycle_sec = gcrr->cycle_sec;
	unsigned int base_nsec = gcrr->base_nsec;
	unsigned int base_sec = gcrr->base_sec;
	unsigned int ext_nsec = gcrr->ter_nsec;
	struct tsn_hw_cap *cap = &dw_tsn_hwcap;
	unsigned int gcl_len, tti_ns, value;
	struct est_gcrr *bgcrr;
	u64 val_ns, sys_ns;
	unsigned int bank;
	int ret = 0;

	if (!dw_tsn_feat_en[TSN_FEAT_ID_EST])
		return -ENOTSUPP;

	if (dbgb >= EST_GCL_BANK_MAX)
		return -EINVAL;

	if (dbgm) {
		bank = dbgb;
	} else {
		value = TSN_RD32(ioaddr + MTL_EST_STATUS);
		bank = (value & MTL_EST_STATUS_SWOL) >>
		       MTL_EST_STATUS_SWOL_SHIFT;
	}

	if (base_nsec > 1000000000ULL || cycle_nsec > 1000000000ULL) {
		TSN_WARN("EST: base(%u) or cycle(%u) nsec > 1s !\n",
			 base_nsec, cycle_nsec);

		return -EINVAL;
	}

	/* Ensure base time is later than MAC system time */
	val_ns = (u64)base_nsec;
	val_ns += (u64)(base_sec * 1000000000ULL);

	/* Get the MAC system time */
	sys_ns = TSN_RD32(ioaddr + TSN_PTP_STNSR);
	sys_ns += TSN_RD32(ioaddr + TSN_PTP_STSR) * 1000000000ULL;

	if (val_ns <= sys_ns) {
		TSN_WARN("EST: base time(%llu) <= system time(%llu)\n",
			 val_ns, sys_ns);

		return -EINVAL;
	}

	if (cycle_sec > EST_CTR_HI_MAX) {
		TSN_WARN("EST: cycle time(%u) > 255 seconds\n", cycle_sec);

		return -EINVAL;
	}

	if (ext_nsec > cap->ext_max) {
		TSN_WARN("EST: invalid time extension(%u), max=%u\n",
			 ext_nsec, cap->ext_max);

		return -EINVAL;
	}

	bgcrr = &dw_est_gc_config.gcb[bank].gcrr;
	gcl_len = bgcrr->llr;

	/* Sanity test on GCL total time intervals against cycle time.
	 * a) For GC length = 1, if its time interval is equal or greater
	 *    than cycle time, it is a constant gate error.
	 * b) If total time interval > cycle time, irregardless of GC
	 *    length, it is not considered an error that GC list is
	 *    truncated. In this case, giving a warning message is
	 *    sufficient.
	 * c) If total time interval < cycle time, irregardless of GC
	 *    length, all GATES are OPEN after the last GC is processed
	 *    until cycle time lapses. This is potentially due to poor
	 *    GCL configuration but is not an error, so we inform user
	 *    about it.
	 */
	tti_ns = est_get_gcl_total_intervals_nsec(bank, gcl_len);
	val_ns = (u64)cycle_nsec;
	val_ns += (u64)(cycle_sec * 1000000000ULL);
	if (gcl_len == 1 && tti_ns >= val_ns) {
		TSN_WARN_NA("EST: Constant gate error!\n");

		return -EINVAL;
	}

	if (tti_ns > val_ns)
		TSN_WARN_NA("EST: GCL is truncated!\n");

	if (tti_ns < val_ns) {
		TSN_INFO("EST: All GCs OPEN at %u of %llu-ns cycle\n",
			 tti_ns, val_ns);
	}

	/* Finally, start programming GCL related registers if the value
	 * differs from the driver copy for efficiency.
	 */

	if (base_nsec != bgcrr->base_nsec)
		ret |= est_write_gcl_config(ioaddr, base_nsec,
					    GCL_CTRL_ADDR_BTR_LO, 1,
					    dbgb, dbgm);

	if (base_sec != bgcrr->base_sec)
		ret |= est_write_gcl_config(ioaddr, base_sec,
					    GCL_CTRL_ADDR_BTR_HI, 1,
					    dbgb, dbgm);

	if (cycle_nsec != bgcrr->cycle_nsec)
		ret |= est_write_gcl_config(ioaddr, cycle_nsec,
					    GCL_CTRL_ADDR_CTR_LO, 1,
					    dbgb, dbgm);

	if (cycle_sec != bgcrr->cycle_sec)
		ret |= est_write_gcl_config(ioaddr, cycle_sec,
					    GCL_CTRL_ADDR_CTR_HI, 1,
					    dbgb, dbgm);

	if (ext_nsec != bgcrr->ter_nsec)
		ret |= est_write_gcl_config(ioaddr, ext_nsec,
					    GCL_CTRL_ADDR_TER, 1,
					    dbgb, dbgm);

	if (ret) {
		TSN_ERR_NA("EST: GCRR programming failure!\n");

		return ret;
	}

	/* Finally, we are ready to switch SWOL now. */
	value = TSN_RD32(ioaddr + MTL_EST_CTRL);
	value |= MTL_EST_CTRL_SSWL;
	TSN_WR32(value, ioaddr + MTL_EST_CTRL);

	/* Update driver copy */
	bgcrr->base_sec = base_sec;
	bgcrr->base_nsec = base_nsec;
	bgcrr->cycle_sec = cycle_sec;
	bgcrr->cycle_nsec = cycle_nsec;
	bgcrr->ter_nsec = ext_nsec;

	TSN_INFO_NA("EST: gcrr set successful\n");

	return 0;
}

int dwmac_set_est_enable(void *ioaddr, bool enable)
{
	unsigned int value;

	if (!dw_tsn_feat_en[TSN_FEAT_ID_EST])
		return -ENOTSUPP;

	value = TSN_RD32(ioaddr + MTL_EST_CTRL);
	value &= ~(MTL_EST_CTRL_SSWL | MTL_EST_CTRL_EEST);
	value |= (enable & MTL_EST_CTRL_EEST);
	TSN_WR32(value, ioaddr + MTL_EST_CTRL);
	dw_est_gc_config.enable = enable;

	return 0;
}

int dwmac_get_est_gcc(void *ioaddr,
		      struct est_gc_config **gcc, bool frmdrv)
{
	struct est_gc_config *pgcc;
	unsigned int bank;
	unsigned int value;
	int ret;

	if (!dw_tsn_feat_en[TSN_FEAT_ID_EST])
		return -ENOTSUPP;

	/* Get GC config from driver */
	if (frmdrv) {
		*gcc = &dw_est_gc_config;

		TSN_INFO_NA("EST: read GCL from driver copy done.\n");

		return 0;
	}

	/* Get GC config from HW */
	pgcc = &dw_est_gc_config;

	value = TSN_RD32(ioaddr + MTL_EST_CTRL);
	pgcc->enable = value & MTL_EST_CTRL_EEST;

	for (bank = 0; bank < EST_GCL_BANK_MAX; bank++) {
		unsigned int llr, row;
		struct est_gc_bank *gcbc = &pgcc->gcb[bank];

		ret = est_read_gcl_config(ioaddr, &value,
					  GCL_CTRL_ADDR_BTR_LO, 1,
					  bank, 1);
		if (ret) {
			TSN_ERR("read BTR(low) fail at bank=%u\n", bank);

			return ret;
		}
		gcbc->gcrr.base_nsec = value;

		ret = est_read_gcl_config(ioaddr, &value,
					  GCL_CTRL_ADDR_BTR_HI, 1,
					  bank, 1);
		if (ret) {
			TSN_ERR("read BTR(high) fail at bank=%u\n", bank);

			return ret;
		}
		gcbc->gcrr.base_sec = value;

		ret = est_read_gcl_config(ioaddr, &value,
					  GCL_CTRL_ADDR_CTR_LO, 1,
					  bank, 1);
		if (ret) {
			TSN_ERR("read CTR(low) fail at bank=%u\n", bank);

			return ret;
		}
		gcbc->gcrr.cycle_nsec = value;

		ret = est_read_gcl_config(ioaddr, &value,
					  GCL_CTRL_ADDR_CTR_HI, 1,
					  bank, 1);
		if (ret) {
			TSN_ERR("read CTR(high) fail at bank=%u\n", bank);

			return ret;
		}
		gcbc->gcrr.cycle_sec = value;

		ret = est_read_gcl_config(ioaddr, &value,
					  GCL_CTRL_ADDR_TER, 1,
					  bank, 1);
		if (ret) {
			TSN_ERR("read TER fail at bank=%u\n", bank);

			return ret;
		}
		gcbc->gcrr.ter_nsec = value;

		ret = est_read_gcl_config(ioaddr, &value,
					  GCL_CTRL_ADDR_LLR, 1,
					  bank, 1);
		if (ret) {
			TSN_ERR("read LLR fail at bank=%u\n", bank);

			return ret;
		}
		gcbc->gcrr.llr = value;
		llr = value;

		for (row = 0; row < llr; row++) {
			unsigned int gates, ti_nsec;
			struct est_gc_entry *gce = gcbc->gcl + row;

			ret = est_read_gce(ioaddr, row, &gates, &ti_nsec,
					   bank, 1);
			if (ret) {
				TSN_ERR("read GCE fail at bank=%u\n", bank);

				return ret;
			}
			gce->gates = gates;
			gce->ti_nsec = ti_nsec;
		}
	}

	*gcc = pgcc;
	TSN_INFO_NA("EST: read GCL from HW done.\n");

	return 0;
}

int dwmac_est_irq_status(void *ioaddr)
{
	struct tsn_err_stat *err_stat = &dw_err_stat;
	struct tsn_hw_cap *cap = &dw_tsn_hwcap;
	unsigned int txqcnt_mask = 0;
	unsigned int status = 0;
	unsigned int value = 0;
	unsigned int feqn = 0;
	unsigned int hbfq = 0;
	unsigned int hbfs = 0;

	txqcnt_mask = (1 << cap->txqcnt) - 1;
	status = TSN_RD32(ioaddr + MTL_EST_STATUS);

	value = (MTL_EST_STATUS_CGCE | MTL_EST_STATUS_HLBS |
		 MTL_EST_STATUS_HLBF | MTL_EST_STATUS_BTRE |
		 MTL_EST_STATUS_SWLC);

	/* Return if there is no error */
	if (!(status & value))
		return 0;

	/* spin_lock() is not needed here because of BTRE and SWLC
	 * bit will not be altered. Both of the bit will be
	 * polled in dwmac_set_est_gcrr_times()
	 */
	if (status & MTL_EST_STATUS_CGCE) {
		/* Clear Interrupt */
		TSN_WR32(MTL_EST_STATUS_CGCE, ioaddr + MTL_EST_STATUS);

		err_stat->cgce_n++;
	}

	if (status & MTL_EST_STATUS_HLBS) {
		value = TSN_RD32(ioaddr + MTL_EST_SCH_ERR);
		value &= txqcnt_mask;

		/* Clear Interrupt */
		TSN_WR32(value, ioaddr + MTL_EST_SCH_ERR);

		/* Collecting info to shows all the queues that has HLBS */
		/* issue. The only way to clear this is to clear the     */
		/* statistic  */
		err_stat->hlbs_q |= value;
	}

	if (status & MTL_EST_STATUS_HLBF) {
		value = TSN_RD32(ioaddr + MTL_EST_FRM_SZ_ERR);
		feqn = value & txqcnt_mask;

		value = TSN_RD32(ioaddr + MTL_EST_FRM_SZ_CAP);
		hbfq = (value & MTL_EST_FRM_SZ_CAP_HBFQ_MASK(cap->txqcnt))
			>> MTL_EST_FRM_SZ_CAP_HBFQ_SHIFT;
		hbfs = value & MTL_EST_FRM_SZ_CAP_HBFS_MASK;

		/* Clear Interrupt */
		TSN_WR32(feqn, ioaddr + MTL_EST_FRM_SZ_ERR);

		err_stat->hlbf_sz[hbfq] = hbfs;
	}

	if (status & MTL_EST_STATUS_BTRE) {
		if ((status & MTL_EST_STATUS_BTRL) ==
		    MTL_EST_STATUS_BTRL_MAX)
			err_stat->btre_max_n++;
		else
			err_stat->btre_n++;

		err_stat->btrl = (status & MTL_EST_STATUS_BTRL) >>
					MTL_EST_STATUS_BTRL_SHIFT;

		TSN_WR32(MTL_EST_STATUS_BTRE, ioaddr +
		       MTL_EST_STATUS);
	}

	if (status & MTL_EST_STATUS_SWLC) {
		TSN_WR32(MTL_EST_STATUS_SWLC, ioaddr +
			 MTL_EST_STATUS);
		TSN_INFO_NA("SWOL has been switched\n");
	}

	return status;
}

int dwmac_get_est_err_stat(struct tsn_err_stat **err_stat)
{
	if (!dw_tsn_feat_en[TSN_FEAT_ID_EST])
		return -ENOTSUPP;

	*err_stat = &dw_err_stat;

	return 0;
}

int dwmac_clr_est_err_stat(void *ioaddr)
{
	if (!dw_tsn_feat_en[TSN_FEAT_ID_EST])
		return -ENOTSUPP;

	memset(&dw_err_stat, 0, sizeof(dw_err_stat));

	return 0;
}

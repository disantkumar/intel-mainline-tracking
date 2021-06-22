/* SPDX-License-Identifier: GPL-2.0-only */
/*****************************************************************************
 *
 * Intel XPCIe XLink PCIe Driver
 *
 * Copyright (C) 2020 Intel Corporation
 *
 ****************************************************************************/

#ifndef XPCIE_PCI_HEADER_
#define XPCIE_PCI_HEADER_

#include <linux/bitfield.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/pci.h>
#include <linux/xlink_drv_inf.h>

#include "../common/xpcie.h"
#include "../common/util.h"
#include "../common/boot.h"

#if (IS_ENABLED(CONFIG_ARCH_THUNDERBAY))
#include <linux/mxlk_boot_inf.h>
#endif

#define XPCIE_DRIVER_NAME "mxlk"
#define XPCIE_DRIVER_DESC "Intel(R) XPCIe XLink PCIe driver"

#define XPCIE_MAX_NAME_LEN	(32)

struct xpcie_dev {
	struct list_head list;
	struct mutex lock; /* Device Lock */

	struct pci_dev *pci;
	char name[XPCIE_MAX_NAME_LEN];
	u32 devid;
#if (IS_ENABLED(CONFIG_ARCH_THUNDERBAY))
	u32 sw_devid;
#endif
	char fw_name[XPCIE_MAX_NAME_LEN];

	struct delayed_work wait_event;
	struct delayed_work shutdown_event;
	wait_queue_head_t waitqueue;
	bool irq_enabled;
	irq_handler_t core_irq_callback;

	struct xpcie xpcie;
	xlink_device_event event_fn;

#if (IS_ENABLED(CONFIG_ARCH_THUNDERBAY))
	struct work_struct irq_event;
	bool boot_dev_link;
	mxlk_pcie_boot_event boot_notif_fn;
#endif
};

static inline struct device *xpcie_to_dev(struct xpcie *xpcie)
{
	struct xpcie_dev *xdev = container_of(xpcie, struct xpcie_dev, xpcie);

	return &xdev->pci->dev;
}

int intel_xpcie_pci_init(struct xpcie_dev *xdev, struct pci_dev *pdev);
int intel_xpcie_pci_cleanup(struct xpcie_dev *xdev);
int intel_xpcie_pci_register_irq(struct xpcie_dev *xdev,
				 irq_handler_t irq_handler);
int intel_xpcie_pci_raise_irq(struct xpcie_dev *xdev,
			      enum xpcie_doorbell_type type,
			      u8 value);

struct xpcie_dev *intel_xpcie_create_device(u32 sw_device_id,
					    struct pci_dev *pdev);
void intel_xpcie_remove_device(struct xpcie_dev *xdev);
void intel_xpcie_list_add_device(struct xpcie_dev *xdev);
void intel_xpcie_list_del_device(struct xpcie_dev *xdev);
void intel_xpcie_pci_notify_event(struct xpcie_dev *xdev,
				  enum xlink_device_event_type event_type);

struct xpcie_dev *intel_xpcie_get_device_by_name(const char *name);
struct xpcie_dev *intel_xpcie_get_device_by_phys_id(u32 phys_id);
#endif /* XPCIE_PCI_HEADER_ */

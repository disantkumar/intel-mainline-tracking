// SPDX-License-Identifier: GPL-2.0-only
/*****************************************************************************
 *
 * Intel XPCIe XLink PCIe Driver
 *
 * Copyright (C) 2020 Intel Corporation
 *
 ****************************************************************************/

#ifdef XLINK_PCIE_RH_DRV_AER
#include <linux/aer.h>
#endif

#include "pci.h"
#include "../common/core.h"

#define HW_ID_LO_MASK	GENMASK(7, 0)
#define HW_ID_HI_MASK	GENMASK(15, 8)

static bool driver_unload;

static const struct pci_device_id xpcie_pci_table[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_KEEMBAY), 0 },
#if (IS_ENABLED(CONFIG_ARCH_THUNDERBAY))
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_TBH_FULL), 0 },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_TBH_PRIME), 0 },
#endif
	{ 0 }
};

#ifdef XLINK_PCIE_RH_DRV_AER
static pci_ers_result_t
intel_xpcie_pci_err_detected(struct pci_dev *pdev, enum pci_channel_state err)
{
	/*
	 * TODO:
	 * This API will be called to warn that a PCIe error has been detected
	 * on the channel. So, there should be mechanism to handle these errors
	 * which is very specific to HW used.
	 *
	 * For now, this API does only print information on what error
	 * has been detected and notify higher layers that error has been
	 * recovered (= PCI_ERS_RESULT_RECOVERED).
	 */

	dev_info(&pdev->dev, "PCIe AER Error Channel IO: ");

	switch (err) {
	case pci_channel_io_normal:
		dev_info(&pdev->dev, "NORMAL\n");
		break;
	case pci_channel_io_frozen:
		dev_info(&pdev->dev, "FROZEN\n");
		break;
	case pci_channel_io_perm_failure:
		dev_info(&pdev->dev, "PERMANENT FAILURE\n");
		break;
	}

	return PCI_ERS_RESULT_RECOVERED;
}

static pci_ers_result_t intel_xpcie_pci_err_mmio_enabled(struct pci_dev *pdev)
{
	/*
	 * TODO:
	 * This API will be called if xxx_error_detected() returns
	 * PCI_ERS_RESULT_CAN_RECOVER
	 *
	 * For time-being, treat that all the errors are recovered.
	 */
	dev_info(&pdev->dev, "PCIe AER MMIO Enabled callback\n");

	return PCI_ERS_RESULT_RECOVERED;
}

static pci_ers_result_t intel_xpcie_pci_err_slot_reset(struct pci_dev *pdev)
{
	/*
	 * TODO:
	 * This API will be called after a PCIe slot reset to see
	 * if it can recover from reset.
	 *
	 * Need to add specific implementation, if any, to check if
	 * HW can recover from reset. For now, we report to higher
	 * layers that it cannot be recovered from a slot reset.
	 */
	dev_info(&pdev->dev, "PCIe AER Error Slot Reset callback\n");

	return PCI_ERS_RESULT_DISCONNECT;
}

static void intel_xpcie_pci_err_resume(struct pci_dev *pdev)
{
	/*
	 * TODO:
	 * This API will be called in a process to re-initialize HW
	 * any, after reset has happened.
	 */
	dev_info(&pdev->dev, "PCIe AER Error Resume Callback\n");
}

static const struct pci_error_handlers intel_xpcie_pci_err_handler = {
	.error_detected = intel_xpcie_pci_err_detected,
	.mmio_enabled = intel_xpcie_pci_err_mmio_enabled,
	.slot_reset = intel_xpcie_pci_err_slot_reset,
	.resume = intel_xpcie_pci_err_resume
};
#endif

static int intel_xpcie_probe(struct pci_dev *pdev,
			     const struct pci_device_id *ent)
{
	bool new_device = false;
	struct xpcie_dev *xdev;
	u32 sw_devid;
	u16 hw_id;
	int ret;

#if (IS_ENABLED(CONFIG_ARCH_THUNDERBAY))
	if (PCI_FUNC(pdev->devfn) & 0x1)
		return 0;
#endif

	hw_id = ((u16)pdev->bus->number << 8) |
		 (PCI_DEVFN(PCI_SLOT(pdev->devfn), PCI_FUNC(pdev->devfn)));
#if (!IS_ENABLED(CONFIG_ARCH_THUNDERBAY))
	sw_devid = FIELD_PREP(XLINK_DEV_INF_TYPE_MASK, XLINK_DEV_INF_PCIE) |
		FIELD_PREP(XLINK_DEV_PHYS_ID_MASK, hw_id) |
		FIELD_PREP(XLINK_DEV_TYPE_MASK, XLINK_DEV_TYPE_KMB) |
		FIELD_PREP(XLINK_DEV_PCIE_ID_MASK, XLINK_DEV_PCIE_0) |
		FIELD_PREP(XLINK_DEV_FUNC_MASK, XLINK_DEV_FUNC_VPU);
#else
	sw_devid = hw_id;
#endif
	xdev = intel_xpcie_get_device_by_phys_id(sw_devid);
	if (!xdev) {
		xdev = intel_xpcie_create_device(sw_devid, pdev);
		if (!xdev)
			return -ENOMEM;

		new_device = true;
	}

	ret = intel_xpcie_pci_init(xdev, pdev);
	if (ret) {
		intel_xpcie_remove_device(xdev);
		return ret;
	}

	if (new_device)
		intel_xpcie_list_add_device(xdev);

	intel_xpcie_pci_notify_event(xdev, NOTIFY_DEVICE_CONNECTED);

	return ret;
}

static void intel_xpcie_remove(struct pci_dev *pdev)
{
	struct xpcie_dev *xdev = pci_get_drvdata(pdev);

	if (xdev) {
		intel_xpcie_pci_cleanup(xdev);
		intel_xpcie_pci_notify_event(xdev, NOTIFY_DEVICE_DISCONNECTED);
		if (driver_unload) {
#if (IS_ENABLED(CONFIG_ARCH_THUNDERBAY))
			intel_xpcie_list_del_device(xdev);
#endif
			intel_xpcie_remove_device(xdev);
		}
	}
}

static struct pci_driver xpcie_driver = {
	.name = XPCIE_DRIVER_NAME,
	.id_table = xpcie_pci_table,
	.probe = intel_xpcie_probe,
	.remove = intel_xpcie_remove
#ifdef XLINK_PCIE_RH_DRV_AER
	.err_handler = &intel_xpcie_pci_err_handler
#endif
};

static int __init intel_xpcie_init_module(void)
{
	return pci_register_driver(&xpcie_driver);
}

static void __exit intel_xpcie_exit_module(void)
{
	driver_unload = true;
	pci_unregister_driver(&xpcie_driver);
}

module_init(intel_xpcie_init_module);
module_exit(intel_xpcie_exit_module);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Intel Corporation");
MODULE_DESCRIPTION(XPCIE_DRIVER_DESC);

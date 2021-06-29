// SPDX-License-Identifier: GPL-2.0-only
/*
 * xlink Core Driver.
 *
 * Copyright (C) 2018-2019 Intel Corporation
 *
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/kref.h>

#ifdef CONFIG_XLINK_LOCAL_HOST
#include <linux/xlink-ipc.h>
#endif

#include "xlink-core.h"
#include "xlink-defs.h"
#include "xlink-ioctl.h"
#include "xlink-multiplexer.h"
#include "xlink-platform.h"

// xlink version number
#define XLINK_VERSION_MAJOR		0
#define XLINK_VERSION_MINOR		1
#define XLINK_VERSION_REVISION		2
#define XLINK_VERSION_SUB_REV		"a"

// timeout in milliseconds used to wait for the reay message from the VPU
#ifdef CONFIG_XLINK_PSS
#define XLINK_VPU_WAIT_FOR_READY (3000000)
#else
#define XLINK_VPU_WAIT_FOR_READY (3000)
#endif

// device, class, and driver names
#define DEVICE_NAME	"xlnk"
#define CLASS_NAME	"xlkcore"
#define DRV_NAME	"xlink-driver"

// used to determine if an API was called from user or kernel space
#define CHANNEL_SET_USER_BIT(chan)	((chan) |= (1 << 15))
#define CHANNEL_USER_BIT_IS_SET(chan)	((chan) & (1 << 15))
#define CHANNEL_CLEAR_USER_BIT(chan)	((chan) &= ~(1 << 15))

static dev_t xdev;
static struct class *dev_class;
static struct cdev xlink_cdev;

static long xlink_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

static const struct file_operations fops = {
		.owner		= THIS_MODULE,
		.unlocked_ioctl = xlink_ioctl,
};

struct xlink_link {
	u32 id;
	struct xlink_handle handle;
	struct kref refcount;
};

struct keembay_xlink_dev {
	struct platform_device *pdev;
	struct xlink_link links[XLINK_MAX_CONNECTIONS];
	u32 nmb_connected_links;
	struct mutex lock;  // protect access to xlink_dev
};

/*
 * global variable pointing to our xlink device.
 *
 * This is meant to be used only when platform_get_drvdata() cannot be used
 * because we lack a reference to our platform_device.
 */
static struct keembay_xlink_dev *xlink;

/*
 * get_next_link() - Searches the list of links to find the next available.
 *
 * Note: This function is only used in xlink_connect, where the xlink mutex is
 * already locked.
 *
 * Return: the next available link, or NULL if maximum connections reached.
 */
static struct xlink_link *get_next_link(void)
{
	struct xlink_link *link = NULL;
	int i;

	for (i = 0; i < XLINK_MAX_CONNECTIONS; i++) {
		if (xlink->links[i].handle.sw_device_id == XLINK_INVALID_SW_DEVICE_ID) {
			link = &xlink->links[i];
			break;
		}
	}
	return link;
}

/*
 * get_link_by_sw_device_id()
 *
 * Searches the list of links to find a link by sw device id.
 *
 * Return: the handle, or NULL if the handle is not found.
 */
static struct xlink_link *get_link_by_sw_device_id(u32 sw_device_id)
{
	struct xlink_link *link = NULL;
	int i;

	mutex_lock(&xlink->lock);
	for (i = 0; i < XLINK_MAX_CONNECTIONS; i++) {
		if (xlink->links[i].handle.sw_device_id == sw_device_id) {
			link = &xlink->links[i];
			break;
		}
	}
	mutex_unlock(&xlink->lock);
	return link;
}

// For now , do nothing and leave for further consideration
static void release_after_kref_put(struct kref *ref) {}

/* Driver probing. */
static int kmb_xlink_probe(struct platform_device *pdev)
{
	struct keembay_xlink_dev *xlink_dev;
	struct device *dev_ret;
	int rc, i;

	dev_info(&pdev->dev, "Keem Bay xlink v%d.%d.%d:%s\n", XLINK_VERSION_MAJOR,
		 XLINK_VERSION_MINOR, XLINK_VERSION_REVISION, XLINK_VERSION_SUB_REV);

	xlink_dev = devm_kzalloc(&pdev->dev, sizeof(*xlink), GFP_KERNEL);
	if (!xlink_dev)
		return -ENOMEM;

	xlink_dev->pdev = pdev;

	// initialize multiplexer
	rc = xlink_multiplexer_init(xlink_dev->pdev);
	if (rc != X_LINK_SUCCESS) {
		pr_err("Multiplexer initialization failed\n");
		goto r_multiplexer;
	}

	// initialize xlink data structure
	xlink_dev->nmb_connected_links = 0;
	mutex_init(&xlink_dev->lock);
	for (i = 0; i < XLINK_MAX_CONNECTIONS; i++) {
		xlink_dev->links[i].id = i;
		xlink_dev->links[i].handle.sw_device_id =
				XLINK_INVALID_SW_DEVICE_ID;
	}

	platform_set_drvdata(pdev, xlink_dev);

	/* Set the global reference to our device. */
	xlink = xlink_dev;

	/*Allocating Major number*/
	if ((alloc_chrdev_region(&xdev, 0, 1, "xlinkdev")) < 0) {
		dev_info(&pdev->dev, "Cannot allocate major number\n");
		goto r_multiplexer;
	}
	dev_info(&pdev->dev, "Major = %d Minor = %d\n", MAJOR(xdev),
		 MINOR(xdev));

	/*Creating struct class*/
	dev_class = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(dev_class)) {
		dev_info(&pdev->dev, "Cannot create the struct class - Err %ld\n",
			 PTR_ERR(dev_class));
		goto r_class;
	}

	/*Creating device*/
	dev_ret = device_create(dev_class, NULL, xdev, NULL, DEVICE_NAME);
	if (IS_ERR(dev_ret)) {
		dev_info(&pdev->dev, "Cannot create the Device 1 - Err %ld\n",
			 PTR_ERR(dev_ret));
		goto r_device;
	}
	dev_info(&pdev->dev, "Device Driver Insert...Done!!!\n");

	/*Creating cdev structure*/
	cdev_init(&xlink_cdev, &fops);

	/*Adding character device to the system*/
	if ((cdev_add(&xlink_cdev, xdev, 1)) < 0) {
		dev_info(&pdev->dev, "Cannot add the device to the system\n");
		goto r_class;
	}

	return 0;

r_device:
	class_destroy(dev_class);
r_class:
	unregister_chrdev_region(xdev, 1);
r_multiplexer:
	xlink_multiplexer_destroy();
	return -1;
}

/* Driver removal. */
static int kmb_xlink_remove(struct platform_device *pdev)
{
	int rc;

	mutex_lock(&xlink->lock);
	// destroy multiplexer
	rc = xlink_multiplexer_destroy();
	if (rc != X_LINK_SUCCESS)
		pr_err("Multiplexer destroy failed\n");

	mutex_unlock(&xlink->lock);
	mutex_destroy(&xlink->lock);
	// unregister and destroy device
	unregister_chrdev_region(xdev, 1);
	device_destroy(dev_class, xdev);
	cdev_del(&xlink_cdev);
	class_destroy(dev_class);
	pr_info("XLink Driver removed\n");
	return 0;
}

/*
 * IOCTL function for User Space access to xlink kernel functions
 *
 */

static long xlink_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int rc;

	switch (cmd) {
	case XL_CONNECT:
		rc = ioctl_connect(arg);
		break;
	case XL_OPEN_CHANNEL:
		rc = ioctl_open_channel(arg);
		break;
	case XL_READ_DATA:
		rc = ioctl_read_data(arg);
		break;
	case XL_WRITE_DATA:
		rc = ioctl_write_data(arg);
		break;
	case XL_WRITE_VOLATILE:
		rc = ioctl_write_volatile_data(arg);
		break;
	case XL_RELEASE_DATA:
		rc = ioctl_release_data(arg);
		break;
	case XL_CLOSE_CHANNEL:
		rc = ioctl_close_channel(arg);
		break;
	case XL_DISCONNECT:
		rc = ioctl_disconnect(arg);
		break;
	}
	if (rc)
		return -EIO;
	else
		return 0;
}

/*
 * xlink Kernel API.
 */

enum xlink_error xlink_initialize(void)
{
	return X_LINK_SUCCESS;
}
EXPORT_SYMBOL_GPL(xlink_initialize);

enum xlink_error xlink_connect(struct xlink_handle *handle)
{
	struct xlink_link *link;
	enum xlink_error rc;
	int interface;

	if (!xlink || !handle)
		return X_LINK_ERROR;

	link = get_link_by_sw_device_id(handle->sw_device_id);
	mutex_lock(&xlink->lock);
	if (!link) {
		link = get_next_link();
		if (!link) {
			pr_err("max connections reached %d\n",
			       XLINK_MAX_CONNECTIONS);
			mutex_unlock(&xlink->lock);
			return X_LINK_ERROR;
		}
		// platform connect
		interface = get_interface_from_sw_device_id(handle->sw_device_id);
		rc = xlink_platform_connect(interface, handle->sw_device_id);
		if (rc) {
			pr_err("platform connect failed %d\n", rc);
			mutex_unlock(&xlink->lock);
			return X_LINK_ERROR;
		}
		// set link handle reference and link id
		link->handle = *handle;
		xlink->nmb_connected_links++;
		kref_init(&link->refcount);
		// initialize multiplexer connection
		rc = xlink_multiplexer_connect(link->id);
		if (rc) {
			pr_err("multiplexer connect failed\n");
			goto r_cleanup;
		}
		pr_info("dev 0x%x connected - dev_type %d - nmb_connected_links %d\n",
			link->handle.sw_device_id,
			link->handle.dev_type,
			xlink->nmb_connected_links);
	} else {
		// already connected
		pr_info("dev 0x%x ALREADY connected - dev_type %d\n",
			link->handle.sw_device_id,
			link->handle.dev_type);
		kref_get(&link->refcount);
		*handle = link->handle;
	}
	mutex_unlock(&xlink->lock);
	// TODO: implement ping
	return X_LINK_SUCCESS;

r_cleanup:
	link->handle.sw_device_id = XLINK_INVALID_SW_DEVICE_ID;
	mutex_unlock(&xlink->lock);
	return X_LINK_ERROR;
}
EXPORT_SYMBOL_GPL(xlink_connect);

enum xlink_error xlink_open_channel(struct xlink_handle *handle,
				    u16 chan, enum xlink_opmode mode,
				    u32 data_size, u32 timeout)
{
	struct xlink_event *event;
	struct xlink_link *link;
	int event_queued = 0;
	enum xlink_error rc;

	if (!xlink || !handle)
		return X_LINK_ERROR;

	link = get_link_by_sw_device_id(handle->sw_device_id);
	if (!link)
		return X_LINK_ERROR;

	event = xlink_create_event(link->id, XLINK_OPEN_CHANNEL_REQ,
				   &link->handle, chan, data_size, timeout);
	if (!event)
		return X_LINK_ERROR;

	event->data = (void *)mode;
	rc = xlink_multiplexer_tx(event, &event_queued);
	if (!event_queued)
		xlink_destroy_event(event);
	return rc;
}
EXPORT_SYMBOL_GPL(xlink_open_channel);

enum xlink_error xlink_close_channel(struct xlink_handle *handle,
				     u16 chan)
{
	struct xlink_event *event;
	struct xlink_link *link;
	enum xlink_error rc;
	int event_queued = 0;

	if (!xlink || !handle)
		return X_LINK_ERROR;

	link = get_link_by_sw_device_id(handle->sw_device_id);
	if (!link)
		return X_LINK_ERROR;

	event = xlink_create_event(link->id, XLINK_CLOSE_CHANNEL_REQ,
				   &link->handle, chan, 0, 0);
	if (!event)
		return X_LINK_ERROR;

	rc = xlink_multiplexer_tx(event, &event_queued);
	if (!event_queued)
		xlink_destroy_event(event);
	return rc;
}
EXPORT_SYMBOL_GPL(xlink_close_channel);

enum xlink_error xlink_write_data(struct xlink_handle *handle,
				  u16 chan, u8 const *pmessage, u32 size)
{
	struct xlink_event *event;
	struct xlink_link *link;
	enum xlink_error rc;
	int event_queued = 0;

	if (!xlink || !handle)
		return X_LINK_ERROR;

	if (size > XLINK_MAX_DATA_SIZE)
		return X_LINK_ERROR;

	link = get_link_by_sw_device_id(handle->sw_device_id);
	if (!link)
		return X_LINK_ERROR;

	event = xlink_create_event(link->id, XLINK_WRITE_REQ, &link->handle,
				   chan, size, 0);
	if (!event)
		return X_LINK_ERROR;

	if (chan < XLINK_IPC_MAX_CHANNELS &&
	    event->interface == IPC_INTERFACE) {
		/* only passing message address across IPC interface */
		event->data = &pmessage;
		rc = xlink_multiplexer_tx(event, &event_queued);
		xlink_destroy_event(event);
	} else {
		event->data = (u8 *)pmessage;
		event->paddr = 0;
		rc = xlink_multiplexer_tx(event, &event_queued);
		if (!event_queued)
			xlink_destroy_event(event);
	}
	return rc;
}
EXPORT_SYMBOL_GPL(xlink_write_data);

enum xlink_error xlink_write_data_user(struct xlink_handle *handle,
				       u16 chan, u8 const *pmessage,
				       u32 size)
{
	struct xlink_event *event;
	struct xlink_link *link;
	enum xlink_error rc;
	int event_queued = 0;
	dma_addr_t paddr = 0;
	u32 addr;

	if (!xlink || !handle)
		return X_LINK_ERROR;

	if (size > XLINK_MAX_DATA_SIZE)
		return X_LINK_ERROR;

	link = get_link_by_sw_device_id(handle->sw_device_id);
	if (!link)
		return X_LINK_ERROR;

	event = xlink_create_event(link->id, XLINK_WRITE_REQ, &link->handle,
				   chan, size, 0);
	if (!event)
		return X_LINK_ERROR;
	event->user_data = 1;

	if (chan < XLINK_IPC_MAX_CHANNELS &&
	    event->interface == IPC_INTERFACE) {
		/* only passing message address across IPC interface */
		if (get_user(addr, (u32 __user *)pmessage)) {
			xlink_destroy_event(event);
			return X_LINK_ERROR;
		}
		event->data = &addr;
		rc = xlink_multiplexer_tx(event, &event_queued);
		xlink_destroy_event(event);
	} else {
		event->data = xlink_platform_allocate(&xlink->pdev->dev, &paddr,
						      size,
						      XLINK_PACKET_ALIGNMENT,
						      XLINK_NORMAL_MEMORY);
		if (!event->data) {
			xlink_destroy_event(event);
			return X_LINK_ERROR;
		}
		if (copy_from_user(event->data, (void __user *)pmessage, size)) {
			xlink_platform_deallocate(&xlink->pdev->dev,
						  event->data, paddr, size,
						  XLINK_PACKET_ALIGNMENT,
						  XLINK_NORMAL_MEMORY);
			xlink_destroy_event(event);
			return X_LINK_ERROR;
		}
		event->paddr = paddr;
		rc = xlink_multiplexer_tx(event, &event_queued);
		if (!event_queued) {
			xlink_platform_deallocate(&xlink->pdev->dev,
						  event->data, paddr, size,
						  XLINK_PACKET_ALIGNMENT,
						  XLINK_NORMAL_MEMORY);
			xlink_destroy_event(event);
		}
	}
	return rc;
}

enum xlink_error xlink_write_volatile(struct xlink_handle *handle,
				      u16 chan, u8 const *message, u32 size)
{
	enum xlink_error rc = 0;

	rc = do_xlink_write_volatile(handle, chan, message, size, 0);
	return rc;
}

enum xlink_error do_xlink_write_volatile(struct xlink_handle *handle,
					 u16 chan, u8 const *message,
					 u32 size, u32 user_flag)
{
	enum xlink_error rc = 0;
	struct xlink_link *link = NULL;
	struct xlink_event *event = NULL;
	int event_queued = 0;
	dma_addr_t paddr;
	int region = 0;

	if (!xlink || !handle)
		return X_LINK_ERROR;

	if (size > XLINK_MAX_BUF_SIZE)
		return X_LINK_ERROR; // TODO: XLink Parameter Error

	link = get_link_by_sw_device_id(handle->sw_device_id);
	if (!link)
		return X_LINK_ERROR;

	event = xlink_create_event(link->id, XLINK_WRITE_VOLATILE_REQ,
				   &link->handle, chan, size, 0);
	if (!event)
		return X_LINK_ERROR;

	region = XLINK_NORMAL_MEMORY;
	event->data = xlink_platform_allocate(&xlink->pdev->dev, &paddr, size,
					      XLINK_PACKET_ALIGNMENT, region);
	if (!event->data) {
		xlink_destroy_event(event);
		return X_LINK_ERROR;
	}
	memcpy(event->data, message, size);
	event->user_data = user_flag;
	event->paddr = paddr;
	rc = xlink_multiplexer_tx(event, &event_queued);
	if (!event_queued) {
		xlink_platform_deallocate(&xlink->pdev->dev, event->data, paddr, size,
					  XLINK_PACKET_ALIGNMENT, region);
		xlink_destroy_event(event);
	}
	return rc;
}

enum xlink_error xlink_read_data(struct xlink_handle *handle,
				 u16 chan, u8 **pmessage, u32 *size)
{
	struct xlink_event *event;
	struct xlink_link *link;
	int event_queued = 0;
	enum xlink_error rc;

	if (!xlink || !handle)
		return X_LINK_ERROR;

	link = get_link_by_sw_device_id(handle->sw_device_id);
	if (!link)
		return X_LINK_ERROR;

	event = xlink_create_event(link->id, XLINK_READ_REQ, &link->handle,
				   chan, *size, 0);
	if (!event)
		return X_LINK_ERROR;

	event->pdata = (void **)pmessage;
	event->length = size;
	rc = xlink_multiplexer_tx(event, &event_queued);
	if (!event_queued)
		xlink_destroy_event(event);
	return rc;
}
EXPORT_SYMBOL_GPL(xlink_read_data);

enum xlink_error xlink_read_data_to_buffer(struct xlink_handle *handle,
					   u16 chan, u8 *const message, u32 *size)
{
	enum xlink_error rc = 0;
	struct xlink_link *link = NULL;
	struct xlink_event *event = NULL;
	int event_queued = 0;

	if (!xlink || !handle)
		return X_LINK_ERROR;

	link = get_link_by_sw_device_id(handle->sw_device_id);
	if (!link)
		return X_LINK_ERROR;

	event = xlink_create_event(link->id, XLINK_READ_TO_BUFFER_REQ,
				   &link->handle, chan, *size, 0);
	if (!event)
		return X_LINK_ERROR;

	event->data = message;
	event->length = size;
	rc = xlink_multiplexer_tx(event, &event_queued);
	if (!event_queued)
		xlink_destroy_event(event);
	return rc;
}
EXPORT_SYMBOL_GPL(xlink_read_data_to_buffer);

enum xlink_error xlink_release_data(struct xlink_handle *handle,
				    u16 chan, u8 * const data_addr)
{
	struct xlink_event *event;
	struct xlink_link *link;
	int event_queued = 0;
	enum xlink_error rc;

	if (!xlink || !handle)
		return X_LINK_ERROR;

	link = get_link_by_sw_device_id(handle->sw_device_id);
	if (!link)
		return X_LINK_ERROR;

	event = xlink_create_event(link->id, XLINK_RELEASE_REQ, &link->handle,
				   chan, 0, 0);
	if (!event)
		return X_LINK_ERROR;

	event->data = data_addr;
	rc = xlink_multiplexer_tx(event, &event_queued);
	if (!event_queued)
		xlink_destroy_event(event);
	return rc;
}
EXPORT_SYMBOL_GPL(xlink_release_data);

enum xlink_error xlink_disconnect(struct xlink_handle *handle)
{
	struct xlink_link *link;
	enum xlink_error rc = X_LINK_ERROR;

	if (!xlink || !handle)
		return X_LINK_ERROR;

	link = get_link_by_sw_device_id(handle->sw_device_id);
	if (!link)
		return X_LINK_ERROR;

	// decrement refcount, if count is 0 lock mutex and disconnect
	if (kref_put_mutex(&link->refcount, release_after_kref_put,
			   &xlink->lock)) {
		// deinitialize multiplexer connection
		rc = xlink_multiplexer_disconnect(link->id);
		if (rc) {
			pr_err("multiplexer disconnect failed\n");
			mutex_unlock(&xlink->lock);
			return X_LINK_ERROR;
		}
		// TODO: reset device?
		// invalidate link handle reference
		link->handle.sw_device_id = XLINK_INVALID_SW_DEVICE_ID;
		xlink->nmb_connected_links--;
		mutex_unlock(&xlink->lock);
	}
	return rc;
}
EXPORT_SYMBOL_GPL(xlink_disconnect);

/* Device tree driver match. */
static const struct of_device_id kmb_xlink_of_match[] = {
	{
		.compatible = "intel,keembay-xlink",
	},
	{}
};

/* The xlink driver is a platform device. */
static struct platform_driver kmb_xlink_driver = {
	.probe = kmb_xlink_probe,
	.remove = kmb_xlink_remove,
	.driver = {
			.name = DRV_NAME,
			.of_match_table = kmb_xlink_of_match,
		},
};

/*
 * The remote host system will need to create an xlink platform
 * device for the platform driver to match with
 */
#ifndef CONFIG_XLINK_LOCAL_HOST
static struct platform_device pdev;
static void kmb_xlink_release(struct device *dev) { return; }
#endif

static int kmb_xlink_init(void)
{
	int rc;

	rc = platform_driver_register(&kmb_xlink_driver);
#ifndef CONFIG_XLINK_LOCAL_HOST
	pdev.dev.release = kmb_xlink_release;
	pdev.name = DRV_NAME;
	pdev.id = -1;
	if (!rc) {
		rc = platform_device_register(&pdev);
		if (rc)
			platform_driver_unregister(&kmb_xlink_driver);
	}
#endif
	return rc;
}
module_init(kmb_xlink_init);

static void kmb_xlink_exit(void)
{
#ifndef CONFIG_XLINK_LOCAL_HOST
	platform_device_unregister(&pdev);
#endif
	platform_driver_unregister(&kmb_xlink_driver);
}
module_exit(kmb_xlink_exit);

MODULE_DESCRIPTION("Keem Bay xlink Kernel Driver");
MODULE_AUTHOR("Seamus Kelly <seamus.kelly@intel.com>");
MODULE_LICENSE("GPL v2");

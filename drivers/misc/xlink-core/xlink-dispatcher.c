// SPDX-License-Identifier: GPL-2.0-only
/*
 * xlink Dispatcher.
 *
 * Copyright (C) 2018-2019 Intel Corporation
 *
 */
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/sched/signal.h>
#include <linux/semaphore.h>
#include <linux/slab.h>

#include "xlink-dispatcher.h"
#include "xlink-multiplexer.h"
#include "xlink-platform.h"

#define DISPATCHER_RX_TIMEOUT_MSEC 0

/* state of a dispatcher servicing a link to a device*/
enum dispatcher_state {
	XLINK_DISPATCHER_INIT,		/* initialized but not used */
	XLINK_DISPATCHER_RUNNING,	/* currently servicing a link */
	XLINK_DISPATCHER_STOPPED,	/* no longer servicing a link */
	XLINK_DISPATCHER_ERROR,		/* fatal error */
};

/* queue for dispatcher tx thread event handling */
struct event_queue {
	struct list_head head;	/* head of event linked list */
	u32 count;		/* number of events in the queue */
	u32 capacity;		/* capacity of events in the queue */
	struct mutex lock;	/* locks queue while accessing */
};

/* dispatcher servicing a single link to a device */
struct dispatcher {
	u32 link_id;				/* id of link being serviced */
	int interface;				/* underlying interface of link */
	enum dispatcher_state state;		/* state of the dispatcher */
	struct xlink_handle *handle;		/* xlink device handle */
	struct task_struct *rxthread;		/* kthread servicing rx */
	struct task_struct *txthread;		/* kthread servicing tx */
	struct event_queue queue;		/* xlink event queue */
	struct event_queue event_buffer_queue;	/* xlink buffer event queue */
	struct semaphore event_sem;		/* signals tx kthread of events */
	struct completion rx_done;		/* sync start/stop of rx kthread */
	struct completion tx_done;		/* sync start/stop of tx thread */
};

/* xlink dispatcher system component */
struct xlink_dispatcher {
	struct dispatcher dispatchers[XLINK_MAX_CONNECTIONS];	/* disp queue */
	struct device *dev;					/* deallocate data */
	struct mutex lock;					/* locks when start new disp */
};

/* global reference to the xlink dispatcher data structure */
static struct xlink_dispatcher *xlinkd;

/*
 * Dispatcher Internal Functions
 *
 */

struct xlink_event *xlink_create_event(uint32_t link_id,
					enum xlink_event_type type,
					struct xlink_handle *handle,
					uint16_t chan, uint32_t size,
					uint32_t timeout)
{
	struct xlink_event *new_event = NULL;

	new_event = alloc_event(link_id);
	if (new_event == NULL)
		return new_event;
	new_event->link_id = link_id;
	new_event->handle = handle;
	new_event->interface = get_interface_from_sw_device_id(handle->sw_device_id);
	new_event->user_data = 0;
	new_event->header.magic = XLINK_EVENT_HEADER_MAGIC;
	new_event->header.id = XLINK_INVALID_EVENT_ID;
	new_event->header.type = type;
	new_event->header.chan = chan;
	new_event->header.size = size;
	new_event->header.timeout = timeout;
	return new_event;
}

inline void xlink_destroy_event(struct xlink_event *event)
{
	free_event(event);
}

struct xlink_event *event_dequeue_buffer(struct event_queue *queue)
{
	struct xlink_event *event = NULL;

	mutex_lock(&queue->lock);
	if (!list_empty(&queue->head)) {
		event = list_first_entry(&queue->head, struct xlink_event, list);
		list_del(&event->list);
		queue->count--;
	}
	mutex_unlock(&queue->lock);
	return event;
}

int event_enqueue_buffer(struct event_queue *queue, struct xlink_event *event)
{
	int rc = -1;

	mutex_lock(&queue->lock);
	list_add_tail(&event->list, &queue->head);
	queue->count++;
	rc = 0;
	mutex_unlock(&queue->lock);
	return rc;
}

static struct dispatcher *get_dispatcher_by_id(u32 id)
{
	if (!xlinkd)
		return NULL;

	if (id >= XLINK_MAX_CONNECTIONS)
		return NULL;

	return &xlinkd->dispatchers[id];
}

struct xlink_event *alloc_event(uint32_t link_id)
{
	struct xlink_event *new_event = NULL;
	struct dispatcher *disp = NULL;

	disp = get_dispatcher_by_id(link_id);
	if (!disp)
		return NULL;
	new_event = event_dequeue_buffer(&disp->event_buffer_queue);
	if (!new_event)
		return NULL;
	return new_event;
}

void free_event(struct xlink_event *event)
{
	struct dispatcher *disp = NULL;

	disp = get_dispatcher_by_id(event->link_id);
	if (!disp)
		return;
	event_enqueue_buffer(&disp->event_buffer_queue, event);
}

void deinit_buffers(struct event_queue *queue)
{
	int j = 0;
	struct xlink_event *new_event = NULL;

	for (j = 0; j < queue->capacity; j++) {
		new_event = event_dequeue_buffer(queue);
		if (new_event)
			kfree(new_event);
	}
}

int init_buffers(struct event_queue *queue)
{
	int j = 0;
	int rc = -1;
	struct xlink_event *new_event = NULL;

	for (j = 0; j < queue->capacity; j++) {
		new_event = kzalloc(sizeof(*new_event), GFP_KERNEL);
		if (!new_event)
			break;
		rc = event_enqueue_buffer(queue, new_event);
		if (rc == -1)
			break;
	}
	return rc;
}

static int wait_tx_queue_empty(struct dispatcher *disp)
{
	do {
		mutex_lock(&disp->queue.lock);
		if (disp->queue.count == 0)
			break;
		mutex_unlock(&disp->queue.lock);
	} while (1);
	mutex_unlock(&disp->queue.lock);
	return 0;
}

static u32 event_generate_id(void)
{
	static u32 id = 0xa;

	return id++;
}

static struct xlink_event *event_dequeue(struct event_queue *queue)
{
	struct xlink_event *event = NULL;

	mutex_lock(&queue->lock);
	if (!list_empty(&queue->head)) {
		event = list_first_entry(&queue->head, struct xlink_event,
					 list);
		list_del(&event->list);
		queue->count--;
	}
	mutex_unlock(&queue->lock);
	return event;
}

static int event_enqueue(struct event_queue *queue, struct xlink_event *event)
{
	int rc = -1;

	mutex_lock(&queue->lock);
	if (queue->count < ((queue->capacity / 10) * 7)) {
		list_add_tail(&event->list, &queue->head);
		queue->count++;
		rc = 0;
	}
	mutex_unlock(&queue->lock);
	return rc;
}

static struct xlink_event *dispatcher_event_get(struct dispatcher *disp)
{
	int rc = 0;
	struct xlink_event *event = NULL;

	// wait until an event is available
	rc = down_interruptible(&disp->event_sem);
	// dequeue and return next event to process
	if (!rc)
		event = event_dequeue(&disp->queue);
	return event;
}

static int is_valid_event_header(struct xlink_event *event)
{
	if (event->header.magic != XLINK_EVENT_HEADER_MAGIC)
		return 0;
	else
		return 1;
}

static int dispatcher_event_send(struct xlink_event *event)
{
	int rc = 0;
	static int error_printed;
	size_t event_header_size = sizeof(event->header) - XLINK_MAX_CONTROL_DATA_PCIE_SIZE;
	size_t transfer_size = 0;

	trace_xlink_dispatcher_header(event->handle->sw_device_id, event->header.chan,
				      event->header.id, event_header_size);
	// write event header
	// printk(KERN_DEBUG "Sending event: type = 0x%x, id = 0x%x\n",
			// event->header.type, event->header.id);
	if (event->header.type == XLINK_WRITE_CONTROL_REQ)
		event_header_size += event->header.size;

	transfer_size = event_header_size;

	rc = xlink_platform_write(event->interface,
			event->handle->sw_device_id, &event->header,
			&event_header_size, event->header.timeout, NULL);
	if (rc || event_header_size != transfer_size) {
		if (!error_printed)
			pr_err("Write header failed %d\n", rc);
		error_printed = 1;
		return rc;
	}
	if (event->header.type == XLINK_WRITE_REQ ||
		event->header.type == XLINK_WRITE_VOLATILE_REQ ||
		event->header.type == XLINK_PASSTHRU_VOLATILE_WRITE_REQ ||
		event->header.type == XLINK_PASSTHRU_WRITE_REQ) {
		error_printed = 0;
		// write event data
		rc = xlink_platform_write(event->interface,
				event->handle->sw_device_id, event->data,
				&event->header.size, event->header.timeout,
				NULL);
		if (rc)
			pr_err("Write data failed %d\n", rc);
		if (event->user_data == 1) {
			if (event->paddr != 0) {
				xlink_platform_deallocate(xlinkd->dev,
					event->data, event->paddr,
					event->header.size,
					XLINK_PACKET_ALIGNMENT,
					XLINK_CMA_MEMORY, event->handle->sw_device_id);
			} else {
				xlink_platform_deallocate(xlinkd->dev,
					event->data, event->paddr,
					event->header.size,
					XLINK_PACKET_ALIGNMENT,
					XLINK_NORMAL_MEMORY, event->handle->sw_device_id);
			}
		}
	}
	return rc;
}

static int xlink_dispatcher_rxthread(void *context)
{
	struct dispatcher *disp = (struct dispatcher *)context;
	struct xlink_event *event;
	size_t size;
	int rc;

	event = xlink_create_event(disp->link_id, 0, disp->handle, 0, 0, 0);
	if (!event)
		return -1;

	allow_signal(SIGTERM); // allow thread termination while waiting on sem
	complete(&disp->rx_done);
	while (!kthread_should_stop()) {
		size = (sizeof(event->header) - XLINK_MAX_CONTROL_DATA_PCIE_SIZE);
		rc = xlink_platform_read(disp->interface,
					 disp->handle->sw_device_id,
					 &event->header, &size,
					 DISPATCHER_RX_TIMEOUT_MSEC, NULL);
		if (rc || size != (int)(sizeof(event->header) - XLINK_MAX_CONTROL_DATA_PCIE_SIZE))
			continue;
		if (is_valid_event_header(event)) {
			event->link_id = disp->link_id;
			trace_xlink_event_receive(event->handle->sw_device_id,
						  event->header.chan,
						  event->header.id,
						  event->header.size);
			rc = xlink_multiplexer_rx(event);
			if (!rc) {
				event = xlink_create_event(disp->link_id, 0,
							   disp->handle, 0, 0,
							   0);
				if (!event)
					return -1;
			}
		}
	}
	complete(&disp->rx_done);
	do_exit(0);
	return 0;
}

static int xlink_dispatcher_txthread(void *context)
{
	struct dispatcher *disp = (struct dispatcher *)context;
	struct xlink_event *event;

	allow_signal(SIGTERM); // allow thread termination while waiting on sem
	complete(&disp->tx_done);
	while (!kthread_should_stop()) {
		event = dispatcher_event_get(disp);
		if (!event)
			continue;

		dispatcher_event_send(event);
		xlink_destroy_event(event); // free handled event
	}
	complete(&disp->tx_done);
	do_exit(0);
	return 0;
}

/*
 * Dispatcher External Functions
 *
 */

enum xlink_error xlink_dispatcher_init(void *dev)
{
	struct platform_device *plat_dev = (struct platform_device *)dev;
	struct dispatcher *dsp;
	int i;

	xlinkd = kzalloc(sizeof(*xlinkd), GFP_KERNEL);
	if (!xlinkd)
		return X_LINK_ERROR;

	xlinkd->dev = &plat_dev->dev;
	for (i = 0; i < XLINK_MAX_CONNECTIONS; i++) {
		dsp = &xlinkd->dispatchers[i];
		dsp->link_id = i;
		sema_init(&dsp->event_sem, 0);
		init_completion(&dsp->rx_done);
		init_completion(&dsp->tx_done);
		INIT_LIST_HEAD(&dsp->queue.head);
		mutex_init(&dsp->queue.lock);
		dsp->queue.count = 0;
		dsp->queue.capacity = XLINK_EVENT_QUEUE_CAPACITY;
		INIT_LIST_HEAD(&xlinkd->dispatchers[i].event_buffer_queue.head);
		mutex_init(&xlinkd->dispatchers[i].event_buffer_queue.lock);
		xlinkd->dispatchers[i].event_buffer_queue.count = 0;
		xlinkd->dispatchers[i].event_buffer_queue.capacity = 1024;/*XLINK_EVENT_QUEUE_CAPACITY*/;
		init_buffers(&xlinkd->dispatchers[i].event_buffer_queue);
		dsp->state = XLINK_DISPATCHER_INIT;
	}
	mutex_init(&xlinkd->lock);

	return X_LINK_SUCCESS;
}

enum xlink_error xlink_dispatcher_start(int id, struct xlink_handle *handle)
{
	struct dispatcher *disp;

	mutex_lock(&xlinkd->lock);
	// get dispatcher by link id
	disp = get_dispatcher_by_id(id);
	if (!disp)
		goto r_error;

	// cannot start a running or failed dispatcher
	if (disp->state == XLINK_DISPATCHER_RUNNING ||
	    disp->state == XLINK_DISPATCHER_ERROR)
		goto r_error;

	// set the dispatcher context
	disp->handle = handle;
	disp->interface = get_interface_from_sw_device_id(handle->sw_device_id);

	// run dispatcher thread to handle and write outgoing packets
	disp->txthread = kthread_run(xlink_dispatcher_txthread,
				     (void *)disp, "txthread");
	if (!disp->txthread) {
		pr_err("xlink txthread creation failed\n");
		goto r_txthread;
	}
	wait_for_completion(&disp->tx_done);
	disp->state = XLINK_DISPATCHER_RUNNING;
	// run dispatcher thread to read and handle incoming packets
	disp->rxthread = kthread_run(xlink_dispatcher_rxthread,
				     (void *)disp, "rxthread");
	if (!disp->rxthread) {
		pr_err("xlink rxthread creation failed\n");
		goto r_rxthread;
	}
	wait_for_completion(&disp->rx_done);
	mutex_unlock(&xlinkd->lock);

	return X_LINK_SUCCESS;

r_rxthread:
	kthread_stop(disp->txthread);
r_txthread:
	disp->state = XLINK_DISPATCHER_STOPPED;
r_error:
	mutex_unlock(&xlinkd->lock);
	return X_LINK_ERROR;
}

enum xlink_error xlink_dispatcher_event_add(enum xlink_event_origin origin,
					    struct xlink_event *event)
{
	struct dispatcher *disp;
	int rc;

	// get dispatcher by link id
	disp = get_dispatcher_by_id(event->link_id);
	if (!disp)
		return X_LINK_ERROR;

	// only add events if the dispatcher is running
	if (disp->state != XLINK_DISPATCHER_RUNNING)
		return X_LINK_ERROR;

	// configure event and add to queue
	if (origin == EVENT_TX)
		event->header.id = event_generate_id();
	event->origin = origin;
	rc = event_enqueue(&disp->queue, event);
	if (rc)
		return X_LINK_CHAN_FULL;

	// notify dispatcher tx thread of new event
	up(&disp->event_sem);
	return X_LINK_SUCCESS;
}

enum xlink_error xlink_dispatcher_stop(int id)
{
	struct dispatcher *disp;
	int rc;

	mutex_lock(&xlinkd->lock);
	// get dispatcher by link id
	disp = get_dispatcher_by_id(id);
	if (!disp)
		goto r_error;

	// don't stop dispatcher if not started
	if (disp->state != XLINK_DISPATCHER_RUNNING)
		goto r_error;

	if (disp->rxthread) {
		wait_tx_queue_empty(disp);
		// stop dispatcher rx thread
		send_sig(SIGTERM, disp->rxthread, 0);
		rc = kthread_stop(disp->rxthread);
		if (rc)
			goto r_thread;
	}
	wait_for_completion(&disp->rx_done);
	if (disp->txthread) {
		// stop dispatcher tx thread
		send_sig(SIGTERM, disp->txthread, 0);
		rc = kthread_stop(disp->txthread);
		if (rc)
			goto r_thread;
	}
	wait_for_completion(&disp->tx_done);
	disp->state = XLINK_DISPATCHER_STOPPED;
	mutex_unlock(&xlinkd->lock);
	return X_LINK_SUCCESS;

r_thread:
	// dispatcher now in error state and cannot be used
	disp->state = XLINK_DISPATCHER_ERROR;
r_error:
	mutex_unlock(&xlinkd->lock);
	return X_LINK_ERROR;
}

enum xlink_error xlink_dispatcher_destroy(void)
{
	enum xlink_event_type type;
	struct xlink_event *event;
	struct dispatcher *disp;
	int i;

	for (i = 0; i < XLINK_MAX_CONNECTIONS; i++) {
		// get dispatcher by link id
		disp = get_dispatcher_by_id(i);
		if (!disp)
			continue;

		// stop all running dispatchers
		if (disp->state == XLINK_DISPATCHER_RUNNING)
			xlink_dispatcher_stop(i);

		// empty queues of all used dispatchers
		if (disp->state == XLINK_DISPATCHER_INIT)
			continue;

		// deallocate remaining events in queue
		while (!list_empty(&disp->queue.head)) {
			event = event_dequeue(&disp->queue);
			if (!event)
				continue;
			type = event->header.type;
			if (type == XLINK_WRITE_REQ ||
			    type == XLINK_WRITE_VOLATILE_REQ) {
				// deallocate event data
				xlink_platform_deallocate(xlinkd->dev,
							  event->data,
							  event->paddr,
							  event->header.size,
							  XLINK_PACKET_ALIGNMENT,
							  XLINK_NORMAL_MEMORY,
							  XLINK_INVALID_SW_DEVICE_ID);
			}
			xlink_destroy_event(event);
		}
		mutex_destroy(&disp->queue.lock);
	}
	mutex_destroy(&xlinkd->lock);
	return X_LINK_SUCCESS;
}
